#include "radar.h"

#include <cmath>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>

#include "common.h"
#include "config.h"
#include "fight.h"
#include "object_index.h"

namespace sfva {
namespace radar {
namespace {

using Clock = std::chrono::steady_clock;

// Facts about build v07.011: KBP_BattlePawn_C is the fighter pawn, and its
// root component's relative location is the character's place on the stage.
constexpr wchar_t kPawnClass[] = L"KBP_BattlePawn_C";
constexpr uintptr_t kPawnRoot = 0x168;    // USceneComponent* RootComponent
constexpr uintptr_t kRootLocation = 0x128;
constexpr uintptr_t kPawnCostume = 0x380;  // KWBattleCharaCostumeDataAsset*
constexpr int kSampleRate = 44100;
constexpr int kChannels = 2;
constexpr int kFrames = 2048;       // one queued block, about 46 ms
constexpr int kPulseFrames = 1764;  // the tone inside it, about 40 ms
constexpr int kBlocks = 4;

struct Options {
    bool enabled = true;
    int volume = 35;
    int range = 1800;    // world units from one side of the stage to the other
    int height = 220;    // height at which the pulse reaches its top pitch
    int nearMs = 120;    // fastest pulse, at point-blank range
    int farMs = 650;     // slowest pulse, at full range
    int baseHz = 520;    // pitch at the same height as the opponent
};

Options g_options;
ObjectIndex g_index;
uintptr_t g_base = 0;
uintptr_t g_class = 0;
uintptr_t g_pawn[2] = {0, 0};
std::wstring g_playerCharacter;
Clock::time_point g_nextClassScan = Clock::time_point::min();
Clock::time_point g_nextGraphTry = Clock::time_point::min();

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_active{false};
bool g_roundActive = false;
std::atomic<bool> g_stop{false};
std::atomic<int> g_rateMs{200};
std::atomic<int> g_pan{0};
std::atomic<int> g_freqHz{520};
std::atomic<int> g_volume{35};
std::atomic<int> g_facing{1};

struct Audio {
    HWAVEOUT out = nullptr;
    WAVEFORMATEX format{};
    WAVEHDR headers[kBlocks]{};
    std::vector<short> blocks[kBlocks];
    bool used[kBlocks] = {false, false, false, false};
    bool ready = false;
};

Audio g_audio;
std::thread g_thread;

void fillBlock(int index, int pan, int freq, int volume) {
    auto& block = g_audio.blocks[index];
    const double twoPi = 6.283185307179586;
    const double angle = (std::clamp(pan, -1000, 1000) / 1000.0 + 1.0) * 3.141592653589793 / 4.0;
    const double leftGain = std::min(1.0, std::cos(angle) * 1.414213562373095);
    const double rightGain = std::min(1.0, std::sin(angle) * 1.414213562373095);
    const double amplitude = std::clamp(volume, 0, 100) / 100.0 * 0.45;
    const double step = twoPi * std::clamp(freq, 80, 4000) / kSampleRate;

    for (int frame = 0; frame < kFrames; ++frame) {
        double envelope = 0.0;
        if (frame < kPulseFrames) {
            envelope = std::sin(3.141592653589793 * frame / kPulseFrames);
        }
        const double tone = std::sin(step * frame) * amplitude * envelope;
        block[static_cast<size_t>(frame) * 2] = static_cast<short>(tone * leftGain * 32767.0);
        block[static_cast<size_t>(frame) * 2 + 1] = static_cast<short>(tone * rightGain * 32767.0);
    }
    g_audio.headers[index].dwBufferLength =
        static_cast<DWORD>(block.size() * sizeof(short));
}

void audioLoop() {
    auto nextPulse = Clock::now();
    bool wasActive = false;
    while (!g_stop) {
        const auto now = Clock::now();
        const bool active = g_active && g_enabled;
        if (!active) {
            wasActive = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (!wasActive || now >= nextPulse) {
            for (int i = 0; i < kBlocks; ++i) {
                WAVEHDR& header = g_audio.headers[i];
                if (g_audio.used[i] && !(header.dwFlags & WHDR_DONE)) continue;
                fillBlock(i, g_pan.load(), g_freqHz.load(), g_volume.load());
                if (waveOutWrite(g_audio.out, &header, sizeof(header)) != MMSYSERR_NOERROR) {
                    logLine(L"radar: waveOutWrite failed");
                    g_enabled = false;
                    break;
                }
                g_audio.used[i] = true;
                nextPulse = now + std::chrono::milliseconds(std::clamp(g_rateMs.load(), 60, 2000));
                break;
            }
            wasActive = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool openAudio() {
    g_audio.format.wFormatTag = WAVE_FORMAT_PCM;
    g_audio.format.nChannels = kChannels;
    g_audio.format.nSamplesPerSec = kSampleRate;
    g_audio.format.wBitsPerSample = 16;
    g_audio.format.nBlockAlign =
        static_cast<WORD>(g_audio.format.nChannels * g_audio.format.wBitsPerSample / 8);
    g_audio.format.nAvgBytesPerSec = g_audio.format.nSamplesPerSec * g_audio.format.nBlockAlign;

    if (waveOutOpen(&g_audio.out, WAVE_MAPPER, &g_audio.format, 0, 0, CALLBACK_NULL) !=
        MMSYSERR_NOERROR) {
        logLine(L"radar: no audio output; radar is off");
        return false;
    }
    for (int i = 0; i < kBlocks; ++i) {
        g_audio.blocks[i].assign(static_cast<size_t>(kFrames) * kChannels, 0);
        WAVEHDR& header = g_audio.headers[i];
        header.lpData = reinterpret_cast<LPSTR>(g_audio.blocks[i].data());
        header.dwBufferLength = static_cast<DWORD>(g_audio.blocks[i].size() * sizeof(short));
        header.dwFlags = 0;
        if (waveOutPrepareHeader(g_audio.out, &header, sizeof(header)) != MMSYSERR_NOERROR) {
            logLine(L"radar: could not prepare an audio block; radar is off");
            waveOutClose(g_audio.out);
            g_audio.out = nullptr;
            return false;
        }
    }
    g_audio.ready = true;
    return true;
}

bool finiteLocation(const float location[3]) {
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(location[i]) || std::fabs(location[i]) > 100000.0f) return false;
    }
    return true;
}

bool readLocation(uintptr_t pawn, float location[3]) {
    uintptr_t root = 0;
    if (!readValue(pawn + kPawnRoot, root) || !plausiblePointer(root)) return false;
    if (!readRaw(root + kRootLocation, location, sizeof(float) * 3)) return false;
    return finiteLocation(location);
}

struct Candidate {
    uintptr_t pawn;
    float x;
    float z;
};

// The stage spawns hidden duplicate pawns for effects; they share a location
// with the real fighter, so one place on the stage is one candidate.
std::vector<Candidate> candidates(uintptr_t classAddress) {
    std::vector<Candidate> out;
    for (uintptr_t pawn : g_index.instances(classAddress)) {
        // The stage also spawns hidden pawns for effects. They share the
        // dummy location and have no costume; only real fighters carry one.
        uintptr_t costume = 0;
        if (!readValue(pawn + kPawnCostume, costume) || !plausiblePointer(costume)) continue;
        float location[3]{};
        if (!readLocation(pawn, location)) continue;
        const bool duplicate = std::any_of(out.begin(), out.end(), [&](const Candidate& other) {
            return std::fabs(other.x - location[0]) < 2.0f &&
                   std::fabs(other.z - location[2]) < 2.0f;
        });
        if (duplicate) continue;
        out.push_back({pawn, location[0], location[2]});
    }
    return out;
}

void rememberCharacter() {
    if (!g_pawn[0]) return;
    uintptr_t costume = 0;
    if (!readValue(g_pawn[0] + kPawnCostume, costume) || !plausiblePointer(costume)) return;
    std::wstring object = g_index.objectName(costume);
    size_t start = object.find(L"DA_");
    if (start == std::wstring::npos) return;
    start += 3;
    const size_t end = object.find(L"_Costume", start);
    if (end == std::wstring::npos || end <= start) return;
    const std::wstring code = object.substr(start, end - start);
    const std::wstring name = FightReader::nameForCode(code);
    const std::wstring character = name.empty() ? code : name;
    if (character == g_playerCharacter) return;
    g_playerCharacter = character;
    logLine(L"radar: player character " + g_playerCharacter);
}

bool readPositions(float& dx, float& dz) {
    if (!g_class) {
        const auto now = Clock::now();
        if (now < g_nextClassScan) return false;
        g_class = g_index.classAddress(kPawnClass);
        if (!g_class) {
            // A class that is not loaded yet is worth another look, but not on
            // every poll: the scan walks the whole object list.
            g_nextClassScan = now + std::chrono::seconds(5);
            logLine(L"radar: fighter class not found; retrying in 5 seconds");
            return false;
        }
    }

    if (g_pawn[0] && g_pawn[1] && g_index.instanceAlive(g_pawn[0], g_class) &&
        g_index.instanceAlive(g_pawn[1], g_class)) {
        float first[3]{};
        float second[3]{};
        if (readLocation(g_pawn[0], first) && readLocation(g_pawn[1], second) &&
            finiteLocation(first) && finiteLocation(second)) {
            rememberCharacter();
            dx = second[0] - first[0];
            dz = second[2] - first[2];
            if (std::fabs(dx) > 1.0f) g_facing = dx > 0.0f ? 1 : -1;
            return true;
        }
    }

    auto found = candidates(g_class);
    if (found.size() < 2) return false;
    // The player is the fighter on the left. In every mode the mod supports,
    // player 1 starts there; the opponent is the other pawn.
    std::sort(found.begin(), found.end(),
              [](const Candidate& a, const Candidate& b) { return a.x < b.x; });
    g_pawn[0] = found.front().pawn;
    g_pawn[1] = found.back().pawn;
    rememberCharacter();
    dx = found.back().x - found.front().x;
    dz = found.back().z - found.front().z;
    if (std::fabs(dx) > 1.0f) g_facing = dx > 0.0f ? 1 : -1;
    logLine(L"radar: fighters at " + std::to_wstring(found.front().x) + L" and " +
            std::to_wstring(found.back().x));
    return true;
}

}  // namespace

void start(uintptr_t base, const Config& config) {
    g_options.enabled = config.radar;
    g_options.volume = std::clamp(config.radarVolume, 0, 100);
    g_options.range = std::clamp(config.radarRange, 100, 20000);
    g_options.height = std::clamp(config.radarHeight, 10, 2000);
    g_options.nearMs = std::clamp(config.radarNearMs, 60, 2000);
    g_options.farMs = std::clamp(config.radarFarMs, 80, 3000);
    g_options.baseHz = std::clamp(config.radarBaseHz, 100, 2000);

    g_base = base;
    g_enabled = g_options.enabled;
    g_volume = g_options.volume;
    if (!g_index.init(base)) {
        // The engine builds its name and object tables after the DLL loads;
        // update() keeps trying once a round is on screen.
        logLine(L"radar: object graph not ready; will retry");
        g_nextGraphTry = Clock::now() + std::chrono::seconds(5);
    }
    if (!openAudio()) {
        g_enabled = false;
        g_options.enabled = false;
        return;
    }
    g_stop = false;
    g_thread = std::thread(audioLoop);
    g_thread.detach();
    logLine(L"radar: " + std::wstring(g_options.enabled ? L"on" : L"off") + L", volume " +
            std::to_wstring(g_options.volume) + L", range " + std::to_wstring(g_options.range));
}

void update(bool roundActive) {
    g_roundActive = roundActive;
    if (!roundActive) {
        g_active = false;
        return;
    }
    if (!g_index.ready()) {
        const auto now = Clock::now();
        if (now < g_nextGraphTry) {
            g_active = false;
            return;
        }
        if (!g_index.init(g_base)) {
            g_nextGraphTry = now + std::chrono::seconds(5);
            g_active = false;
            return;
        }
    }
    float dx = 0.0f;
    float dz = 0.0f;
    if (!readPositions(dx, dz)) {
        g_active = false;
        return;
    }

    const float distance = std::fabs(dx);
    const float distanceRatio = std::clamp(distance / g_options.range, 0.0f, 1.0f);
    const int rate =
        g_options.nearMs +
        static_cast<int>((g_options.farMs - g_options.nearMs) * distanceRatio);
    const int pan = static_cast<int>(
        std::clamp(dx / (g_options.range * 0.5f), -1.0f, 1.0f) * 1000.0f);
    const float heightRatio = std::clamp(dz / g_options.height, -1.0f, 1.0f);
    const int frequency = static_cast<int>(
        std::clamp(g_options.baseHz * (1.0 + 0.9 * heightRatio), 100.0, 3000.0));

    g_rateMs = rate;
    g_pan = pan;
    g_freqHz = frequency;
    // Positions are still refreshed with the pulse off, so the on-demand
    // readout works without turning the audio back on.
    g_active = g_enabled && g_audio.ready;

    static auto lastLog = Clock::time_point::min();
    const auto now = Clock::now();
    if (lastLog == Clock::time_point::min() || now - lastLog > std::chrono::seconds(1)) {
        lastLog = now;
        logLine(L"radar: dx " + std::to_wstring(static_cast<int>(dx)) + L", dz " +
                std::to_wstring(static_cast<int>(dz)) + L", pulse " + std::to_wstring(rate) +
                L" ms, pan " + std::to_wstring(pan) + L", pitch " +
                std::to_wstring(frequency) + L" Hz");
    }
}

void setEnabled(bool on) {
    g_enabled = on;
    g_options.enabled = on;
    if (!on) g_active = false;
    logLine(on ? L"radar on" : L"radar off");
}

bool enabled() {
    return g_enabled;
}

std::wstring playerCharacter() {
    return g_playerCharacter;
}

bool playerFacingRight() {
    float dx = 0.0f;
    float dz = 0.0f;
    // Ask the live positions first; the cached value is only a fallback for a
    // frame when the fighters are not readable.
    if (readPositions(dx, dz)) return dx >= 0.0f;
    return g_facing.load() > 0;
}

std::wstring status() {
    if (!g_audio.ready) return L"radar unavailable";
    if (!g_enabled) return L"radar off";
    return g_active ? L"radar on, opponent in range" : L"radar on, waiting for a round";
}

std::wstring describe() {
    if (!g_roundActive) return L"No live round to read.";
    if (!g_index.ready()) return L"The radar is not ready.";
    float dx = 0.0f;
    float dz = 0.0f;
    if (!readPositions(dx, dz)) return L"I can't read the opponent.";

    std::wstring where;
    if (dx > 30.0f) {
        where = L"to the right";
    } else if (dx < -30.0f) {
        where = L"to the left";
    }
    const float height = std::max(40.0f, g_options.height * 0.4f);
    if (dz > height) {
        where += where.empty() ? L"above you" : L" and above you";
    } else if (dz < -height) {
        where += where.empty() ? L"below you" : L" and below you";
    }

    const int distance = static_cast<int>(std::lround(std::hypot(dx, dz)));
    const wchar_t* band = distance < 150   ? L"point-blank"
                          : distance < 450 ? L"close"
                          : distance < 900 ? L"mid-range"
                          : distance < 1400 ? L"far"
                                            : L"full screen";
    const std::wstring units = distance < 10
                                   ? L"less than 10"
                                   : L"about " +
                                         std::to_wstring(((distance + 5) / 10) * 10);
    const std::wstring lead =
        where.empty() ? L"Opponent is right on top of you" : L"Opponent " + where;
    return lead + L", " + band + L", " + units + L" units away.";
}

void requestStop() {
    g_stop = true;
    g_active = false;
}

}  // namespace radar
}  // namespace sfva
