#include "macro.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <iterator>
#include <thread>
#include <vector>

#include "common.h"

namespace sfva {
namespace macro {
namespace {

// SFV's default keyboard layout. The macros send the game the same keys a
// player would: WASD for directions, G/H/J for punches and B/N/M for kicks.
// Virtual-key codes, not scan codes: SFV reads the keyboard by virtual key,
// and scan-only events did not reach the fight.
constexpr WORD kLeft = 'A';
constexpr WORD kRight = 'D';
constexpr WORD kUp = 'W';
constexpr WORD kDown = 'S';
constexpr WORD kLp = 'G';
constexpr WORD kMp = 'H';
constexpr WORD kHp = 'J';
constexpr WORD kLk = 'B';
constexpr WORD kMk = 'N';
constexpr WORD kHk = 'M';

enum Button {
    BtnLp = 1 << 0,
    BtnMp = 1 << 1,
    BtnHp = 1 << 2,
    BtnLk = 1 << 3,
    BtnMk = 1 << 4,
    BtnHk = 1 << 5,
};

struct Step {
    int direction = 5;  // numpad, relative to facing right
    int buttons = 0;
    int ms = 34;
};

std::atomic<bool> g_busy{false};
std::atomic<bool> g_stop{false};
std::atomic<int64_t> g_finishedAt{0};
std::vector<Step> g_steps;
std::vector<WORD> g_pressed;
std::thread g_thread;

bool sendKey(WORD key, bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    // Send the scan code too. Some keyboard paths read it instead of the VK,
    // and it makes the event look like a real key press.
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    return SendInput(1, &input, sizeof(input)) == 1;
}

// The macro key is Ctrl+Shift+F1, so the user is still holding Ctrl and Shift
// when the first direction is sent. Windows would take Ctrl+Shift+D, H or M
// as its own hotkey and never pass it to the game, so release the modifiers
// before the sequence. They are not pressed again: releasing a key the user is
// holding is harmless, while pressing it again after they let go would leave
// it stuck down.
void releaseHeldModifiers() {
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
        sendKey(VK_LCONTROL, false);
        sendKey(VK_RCONTROL, false);
    }
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
        sendKey(VK_LSHIFT, false);
        sendKey(VK_RSHIFT, false);
    }
}

// Only keys this macro pressed are released. A direction the player is
// already holding belongs to the player, not to the macro.
void press(WORD key) {
    if (std::find(g_pressed.begin(), g_pressed.end(), key) != g_pressed.end()) return;
    if (sendKey(key, true)) g_pressed.push_back(key);
}

void release(WORD key) {
    const auto found = std::find(g_pressed.begin(), g_pressed.end(), key);
    if (found == g_pressed.end()) return;
    sendKey(key, false);
    g_pressed.erase(found);
}

void releaseAll() {
    while (!g_pressed.empty()) {
        sendKey(g_pressed.back(), false);
        g_pressed.pop_back();
    }
}

void directionKeys(int direction, std::vector<WORD>& keys) {
    keys.clear();
    switch (direction) {
        case 1:
            keys = {kLeft, kDown};
            break;
        case 2:
            keys = {kDown};
            break;
        case 3:
            keys = {kRight, kDown};
            break;
        case 4:
            keys = {kLeft};
            break;
        case 6:
            keys = {kRight};
            break;
        case 7:
            keys = {kLeft, kUp};
            break;
        case 8:
            keys = {kUp};
            break;
        case 9:
            keys = {kRight, kUp};
            break;
        default:
            break;
    }
}

// The presets are written facing right. A fighter on the other side needs
// every horizontal component reversed, or the motion runs away from the
// opponent and the move never comes out.
int mirrorDirection(int direction) {
    switch (direction) {
        case 1:
            return 3;
        case 3:
            return 1;
        case 4:
            return 6;
        case 6:
            return 4;
        case 7:
            return 9;
        case 9:
            return 7;
        default:
            return direction;
    }
}

std::vector<WORD> buttonKeys(int buttons) {
    std::vector<WORD> keys;
    if (buttons & BtnLp) keys.push_back(kLp);
    if (buttons & BtnMp) keys.push_back(kMp);
    if (buttons & BtnHp) keys.push_back(kHp);
    if (buttons & BtnLk) keys.push_back(kLk);
    if (buttons & BtnMk) keys.push_back(kMk);
    if (buttons & BtnHk) keys.push_back(kHk);
    return keys;
}

bool pause(int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (g_stop || !gameHasFocus()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return !g_stop && gameHasFocus();
}

void run() {
    releaseHeldModifiers();

    std::vector<WORD> heldDirections;
    for (const Step& step : g_steps) {
        if (g_stop || !gameHasFocus()) break;
        for (WORD key : heldDirections) release(key);
        heldDirections.clear();
        directionKeys(step.direction, heldDirections);
        for (WORD key : heldDirections) press(key);

        const auto buttons = buttonKeys(step.buttons);
        for (WORD key : buttons) press(key);
        const bool finished = pause(step.ms);
        for (WORD key : buttons) release(key);
        if (!finished) break;
    }
    releaseAll();
    g_finishedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
    g_busy = false;
    logLine(L"macro: finished");
}

int buttonBit(const std::wstring& text) {
    if (text == L"P") return BtnHp;
    if (text == L"K") return BtnHk;
    if (text == L"LP") return BtnLp;
    if (text == L"MP") return BtnMp;
    if (text == L"HP") return BtnHp;
    if (text == L"LK") return BtnLk;
    if (text == L"MK") return BtnMk;
    if (text == L"HK") return BtnHk;
    return 0;
}

// Parses the compact notation the INI uses:
//   236P, 236236P, [4]6P, 2HP, MP+MK, 214K, 720P.
// Digits are numpad directions relative to facing right. Square brackets hold
// that direction for hold_ms, for charge moves. 720 is shorthand for two full
// rotations, for Zangief's Critical Art.
bool parseSequence(const std::wstring& text, int stepMs, int holdMs, std::vector<Step>& out) {
    out.clear();
    std::wstring token;
    auto flush = [&]() -> bool {
        if (token.empty()) return true;
        std::wstring word = token;
        token.clear();
        if (word.rfind(L"720", 0) == 0) {
            // A full rotation starts and ends on 6. The final 6 is needed to
            // complete the second rotation; without it the motion ends on 9
            // and is 45 degrees short, which is enough for SFV to reject it.
            word = L"63214789632147896" + word.substr(3);
        }

        size_t pos = 0;
        bool hasDirection = false;
        int buttons = 0;
        const size_t before = out.size();
        while (pos < word.size()) {
            if (word[pos] == L'[') {
                const size_t close = word.find(L']', pos + 1);
                if (close == std::wstring::npos) return false;
                int direction = 5;
                for (size_t i = pos + 1; i < close; ++i) {
                    if (word[i] >= L'1' && word[i] <= L'9') {
                        direction = word[i] - L'0';
                    }
                }
                out.push_back({direction, 0, holdMs});
                hasDirection = true;
                pos = close + 1;
                continue;
            }
            if (word[pos] >= L'1' && word[pos] <= L'9') {
                out.push_back({word[pos] - L'0', 0, stepMs});
                hasDirection = true;
                ++pos;
                continue;
            }
            if (word[pos] == L'+' || word[pos] == L',') {
                ++pos;
                continue;
            }
            break;
        }

        while (pos < word.size()) {
            while (pos < word.size() && word[pos] == L'+') ++pos;
            const size_t start = pos;
            while (pos < word.size() && word[pos] != L'+') ++pos;
            if (start == pos) continue;
            const int bit = buttonBit(word.substr(start, pos - start));
            if (!bit) return false;
            buttons |= bit;
        }

        if (buttons) {
            if (out.size() == before) {
                out.push_back({5, buttons, stepMs * 2});
            } else {
                out.back().buttons |= buttons;
                out.back().ms = stepMs * 2;
            }
        }
        if (!hasDirection && !buttons) return false;
        return true;
    };

    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (iswspace(c) || c == L',') {
            if (!flush()) return false;
        } else {
            token.push_back(static_cast<wchar_t>(towupper(c)));
        }
    }
    if (!flush()) return false;
    return !out.empty();
}

std::wstring keyFor(Kind kind) {
    switch (kind) {
        case Kind::Critical:
            return L"critical";
        case Kind::VSkill:
            return L"vskill";
        case Kind::VTrigger:
            return L"vtrigger";
    }
    return L"critical";
}

std::wstring labelFor(Kind kind) {
    switch (kind) {
        case Kind::Critical:
            return L"Critical Art";
        case Kind::VSkill:
            return L"V-Skill";
        case Kind::VTrigger:
            return L"V-Trigger";
    }
    return L"Critical Art";
}

std::wstring readMacro(const std::wstring& section, const std::wstring& key,
                       const std::wstring& fallback) {
    wchar_t buffer[256]{};
    const std::wstring path = moduleDir() + L"\\sfvaccess.ini";
    GetPrivateProfileStringW(section.c_str(), key.c_str(), fallback.c_str(), buffer,
                             static_cast<DWORD>(std::size(buffer)), path.c_str());
    return buffer;
}

}  // namespace

std::wstring trigger(Kind kind, const std::wstring& character, bool facingRight,
                     const Config& config) {
    if (!config.macros) return L"Macros are off.";
    if (character.empty()) return L"Macros are only available in a fight.";
    if (g_busy) return L"A macro is already playing.";

    const std::wstring key = keyFor(kind);
    const std::wstring label = labelFor(kind);
    const std::wstring defaults = kind == Kind::VSkill   ? L"MP+MK"
                                  : kind == Kind::VTrigger ? L"HP+HK"
                                                          : L"";
    std::wstring section = L"macros." + character;
    std::wstring sequence = readMacro(section, key, L"");
    if (sequence.empty()) sequence = readMacro(L"macros", key, defaults);
    if (sequence.empty()) {
        return L"No " + label + L" macro for " + character + L".";
    }

    std::vector<Step> steps;
    if (!parseSequence(sequence, config.macroStepMs, config.macroHoldMs, steps)) {
        logLine(L"macro: cannot parse " + sequence + L" for " + character);
        return L"The " + label + L" macro for " + character + L" is not valid.";
    }

    if (!facingRight) {
        for (Step& step : steps) step.direction = mirrorDirection(step.direction);
    }

    g_finishedAt = 0;
    g_steps = steps;
    g_busy = true;
    g_thread = std::thread(run);
    g_thread.detach();
    logLine(L"macro: " + label + L" for " + character + L", " +
            std::to_wstring(steps.size()) + L" steps, facing " +
            (facingRight ? L"right" : L"left"));
    return label + L".";
}

bool active() {
    if (g_busy) return true;
    const int64_t finished = g_finishedAt.load();
    if (finished == 0) return false;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    // The hotkey thread can still be draining a key sent at the end of the
    // sequence, so keep the narrator's hotkeys out of the way briefly.
    return now - finished < 300;
}

void requestStop() {
    g_stop = true;
}

}  // namespace macro
}  // namespace sfva
