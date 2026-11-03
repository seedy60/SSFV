#include "fight.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>

#include "common.h"

namespace sfva {
namespace {

// Facts about build v07.011, first published by the SFV Access project.
constexpr uintptr_t kRecordVtable = 0x2C51680;  // every record starts with this marker
constexpr uintptr_t kHealth = 0xD0;             // 16.16 fixed point, and its maximum at +4
constexpr uintptr_t kCritical = 0xDC;
constexpr uintptr_t kVGauge = 0xF4;
constexpr uintptr_t kRecordChara = 0x98;  // its character's data
constexpr uintptr_t kCharaCode = 0x1C0;   // the character code, "RYU"
constexpr uintptr_t kControllerFighter = 0x6B0;  // CPU controller's fighter record
constexpr uintptr_t kSettingPlayerSetting = 0x58;  // KWBattleSetting::m_player_setting
constexpr uintptr_t kPlayerTable = 0x28;  // in KWBattlePlayerSetting: the two entries
constexpr uintptr_t kPlayerEntry = 0x590;  // between the entries
constexpr uintptr_t kPlayerCtrl = 0x21C;   // EKWCtrlType: 0 USER, 2 COM
constexpr uintptr_t kPlayerChara = 0x90;   // the character code, "Z31"
constexpr int kCtrlUser = 0;
constexpr int kStock = 300;               // one Critical Art stock, one V-Trigger bar
constexpr int kMost = 3000;               // nothing here is larger
constexpr size_t kLargestRegion = 256u << 20;

struct CodeName {
    const wchar_t* code;
    const wchar_t* name;
};

// The game's character codes, from character_names.json in the SFV Access
// project, which generated it from the game's own data.
constexpr CodeName kNames[] = {
    {L"ALX", L"ALEX"},     {L"BLR", L"VEGA"},     {L"BRD", L"BIRDIE"},   {L"BSN", L"BALROG"},
    {L"CMY", L"CAMMY"},    {L"CNL", L"CHUN-LI"},  {L"DSM", L"DHALSIM"},  {L"ELV", L"ELEVEN"},
    {L"FAN", L"F.A.N.G"},  {L"GUL", L"GUILE"},    {L"IBK", L"IBUKI"},    {L"JRI", L"JURI"},
    {L"KEN", L"KEN"},      {L"KRN", L"KARIN"},    {L"LAR", L"LAURA"},    {L"NCL", L"NECALLI"},
    {L"NSH", L"NASH"},     {L"RMK", L"R. MIKA"},  {L"RSD", L"RASHID"},   {L"RYU", L"RYU"},
    {L"URN", L"URIEN"},    {L"VEG", L"M. BISON"}, {L"Z20", L"KOLIN"},    {L"Z21", L"AKUMA"},
    {L"Z22", L"ED"},       {L"Z23", L"MENAT"},    {L"Z24", L"ABIGAIL"},  {L"Z25", L"ZEKU"},
    {L"Z26", L"SAKURA"},   {L"Z27", L"BLANKA"},   {L"Z28", L"FALKE"},    {L"Z29", L"CODY"},
    {L"Z30", L"G"},        {L"Z31", L"SAGAT"},    {L"Z32", L"KAGE"},     {L"Z33", L"POISON"},
    {L"Z34", L"E. HONDA"}, {L"Z35", L"LUCIA"},    {L"Z36", L"GILL"},     {L"Z37", L"SETH"},
    {L"Z38", L"DAN"},      {L"Z39", L"ROSE"},     {L"Z40", L"ORO"},      {L"Z41", L"AKIRA"},
    {L"Z42", L"LUKE"},     {L"ZGF", L"ZANGIEF"},
};

bool sameName(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (towupper(a[i]) != towupper(b[i])) return false;
    }
    return true;
}

int whole(int32_t fixed) {
    return fixed >> 16;
}

// Character codes are narrow ASCII in this build, not wide strings. The
// three-letter slot is zero padded, so a padded read is still just "Z31".
std::wstring codeAt(uintptr_t address) {
    char code[8]{};
    if (!readRaw(address, code, sizeof(code))) return {};
    std::wstring out;
    for (char c : code) {
        if (!c) break;
        const auto ch = static_cast<unsigned char>(c);
        if (!std::isalnum(ch)) return {};
        out.push_back(static_cast<wchar_t>(ch));
    }
    if (out.size() < 2 || out.size() > 4) return {};
    return out;
}

}  // namespace

bool Gauges::valid() const {
    const std::array<std::pair<int, int>, 3> pairs = {{{health, healthMax},
                                                       {critical, criticalMax},
                                                       {v, vMax}}};
    for (const auto& [value, most] : pairs) {
        if (most <= 0 || most > kMost || value < 0 || value > most) return false;
    }
    return true;
}

std::wstring Gauges::words(const std::wstring& label) const {
    const int healthPercent = healthMax ? (health * 100 + healthMax / 2) / healthMax : 0;
    return label + L", health " + std::to_wstring(healthPercent) + L" percent, V-Trigger " +
           std::to_wstring(v / kStock) + L" of " + std::to_wstring(vMax / kStock) +
           L", Critical " + std::to_wstring(critical / kStock) + L" of " +
           std::to_wstring(criticalMax / kStock) + L".";
}

FightReader::FightReader(uintptr_t moduleBase) : base_(moduleBase) {}

bool FightReader::scan() {
    records_.clear();
    resetOrder();
    const uint64_t marker = static_cast<uint64_t>(base_ + kRecordVtable);
    std::vector<uint8_t> buffer;
    buffer.reserve(8u << 20);

    uintptr_t address = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    while (address < 0x7FFFFFFFFFFFULL &&
           VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi))) {
        const uintptr_t size = static_cast<uintptr_t>(mbi.RegionSize);
        const DWORD protect = mbi.Protect;
        const bool readable = protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
                              protect == PAGE_EXECUTE_READWRITE ||
                              protect == PAGE_EXECUTE_WRITECOPY;
        if (mbi.State == MEM_COMMIT && readable && size && size <= kLargestRegion) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            // Read in chunks so one huge region does not need a huge buffer.
            for (uintptr_t offset = 0; offset < size; offset += (8u << 20)) {
                const size_t chunk = static_cast<size_t>(std::min<uintptr_t>(8u << 20, size - offset));
                buffer.resize(chunk);
                if (!readRaw(base + offset, buffer.data(), chunk)) continue;
                const size_t words = chunk / 8;
                for (size_t i = 0; i < words; ++i) {
                    uint64_t value = 0;
                    std::memcpy(&value, buffer.data() + i * 8, sizeof(value));
                    if (value != marker) continue;
                    const uintptr_t record = base + offset + i * 8;
                    Gauges gauges;
                    if (!readGauges(record, gauges)) continue;
                    if (std::find(records_.begin(), records_.end(), record) == records_.end()) {
                        records_.push_back(record);
                    }
                }
            }
        }
        address = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + (size ? size : 0x1000);
    }

    if (records_.size() > 2) records_.resize(2);
    logLine(L"fight: " + std::to_wstring(records_.size()) + L" fighter records");
    return records_.size() == 2;
}

bool FightReader::readGauges(uintptr_t record, Gauges& out) const {
    // The pairs are not contiguous: the record holds other fields between them.
    int32_t health = 0, healthMax = 0, critical = 0, criticalMax = 0, v = 0, vMax = 0;
    if (!readValue(record + kHealth, health)) return false;
    if (!readValue(record + kHealth + 4, healthMax)) return false;
    if (!readValue(record + kCritical, critical)) return false;
    if (!readValue(record + kCritical + 4, criticalMax)) return false;
    if (!readValue(record + kVGauge, v)) return false;
    if (!readValue(record + kVGauge + 4, vMax)) return false;
    out.health = whole(health);
    out.healthMax = whole(healthMax);
    out.critical = whole(critical);
    out.criticalMax = whole(criticalMax);
    out.v = whole(v);
    out.vMax = whole(vMax);
    return out.valid();
}

std::wstring FightReader::recordCode(uintptr_t record) const {
    uintptr_t data = 0;
    if (!readValue(record + kRecordChara, data) || !plausiblePointer(data)) return {};
    return codeAt(data + kCharaCode);
}

std::wstring FightReader::codeForName(const std::wstring& name) {
    for (const auto& entry : kNames) {
        if (sameName(name, entry.name)) return entry.code;
    }
    return {};
}

std::wstring FightReader::knownName(const std::wstring& text) {
    for (const auto& entry : kNames) {
        if (sameName(text, entry.name)) return entry.name;
    }
    return {};
}

std::wstring FightReader::nameForCode(const std::wstring& code) {
    for (const auto& entry : kNames) {
        if (sameName(code, entry.code)) return entry.name;
    }
    return {};
}

std::wstring FightReader::playerCharacter() const {
    if (playerRecord_) return nameForCode(recordCode(playerRecord_));
    if (records_.empty()) return {};
    return nameForCode(recordCode(records_[0]));
}

void FightReader::resetOrder() {
    cpuRecord_ = 0;
    cpuRecordKnown_ = false;
    nextCpuTryMs_ = 0;
    setting_ = 0;
    nextSettingTryMs_ = 0;
    playerRecord_ = 0;
    playerCode_.clear();
}

uintptr_t FightReader::battleSetting() {
    if (setting_) {
        const uintptr_t cls = index_.classAddress(L"KWBattleSetting");
        if (cls && index_.instanceAlive(setting_, cls)) return setting_;
        setting_ = 0;
    }

    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    if (now < nextSettingTryMs_) return 0;
    nextSettingTryMs_ = now + 2000;

    if (!index_.ready()) index_.init(base_);
    if (!index_.ready()) return 0;
    const uintptr_t cls = index_.classAddress(L"KWBattleSetting");
    if (!cls) return 0;

    for (const uintptr_t object : index_.instances(cls)) {
        const std::wstring name = index_.objectName(object);
        if (name.rfind(L"Default__", 0) == 0) continue;
        uintptr_t players = 0;
        if (!readValue(object + kSettingPlayerSetting, players) ||
            !plausiblePointer(players)) {
            continue;
        }
        uintptr_t table = 0;
        if (!readValue(players + kPlayerTable, table) || !plausiblePointer(table)) continue;

        // The saved copy (BattleSettingSave) names a USER and a DUMMY with
        // empty character slots. The live battle names a USER and a COM and
        // has both characters filled in, so only that one is taken.
        int user = -1;
        std::wstring userCode;
        bool otherFilled = false;
        for (int side = 0; side < 2; ++side) {
            const uintptr_t entry = table + static_cast<uintptr_t>(side) * kPlayerEntry;
            int ctrl = -1;
            if (!readValue(entry + kPlayerCtrl, ctrl)) continue;
            const std::wstring code = codeAt(entry + kPlayerChara);
            if (ctrl == kCtrlUser) {
                user = side;
                userCode = code;
            } else if (!code.empty()) {
                otherFilled = true;
            }
        }
        if (user < 0 || userCode.empty() || !otherFilled) continue;

        setting_ = object;
        logLine(L"fight: battle setting at 0x" + std::to_wstring(object) + L", player " +
                userCode + L" on side " + std::to_wstring(user + 1));
        return setting_;
    }
    return 0;
}

std::wstring FightReader::battlePlayerCode() {
    const uintptr_t setting = battleSetting();
    if (!setting) return playerCode_;

    uintptr_t players = 0;
    if (!readValue(setting + kSettingPlayerSetting, players) || !plausiblePointer(players)) {
        return playerCode_;
    }
    uintptr_t table = 0;
    if (!readValue(players + kPlayerTable, table) || !plausiblePointer(table)) {
        return playerCode_;
    }
    for (int side = 0; side < 2; ++side) {
        const uintptr_t entry = table + static_cast<uintptr_t>(side) * kPlayerEntry;
        int ctrl = -1;
        if (!readValue(entry + kPlayerCtrl, ctrl) || ctrl != kCtrlUser) continue;
        const std::wstring code = codeAt(entry + kPlayerChara);
        if (!code.empty()) {
            playerCode_ = code;
            return code;
        }
    }
    return playerCode_;
}

uintptr_t FightReader::cpuRecord() {
    if (cpuRecordKnown_) return cpuRecord_;
    if (records_.size() != 2) return 0;

    // A failed scan is retried at most every two seconds; scanning the object
    // list on every poll would cost more than the whole fight reader.
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    if (now < nextCpuTryMs_) return 0;
    nextCpuTryMs_ = now + 2000;

    // The object graph can still be loading on the first read; keep trying on
    // the same slow cadence rather than disabling the link for the whole fight.
    if (!index_.ready()) index_.init(base_);
    if (!index_.ready()) return 0;

    const uintptr_t controller = index_.classAddress(L"KBP_BattlePlayerController_C");
    if (!controller) return 0;
    for (const uintptr_t object : index_.instances(controller)) {
        uintptr_t record = 0;
        if (!readValue(object + kControllerFighter, record)) continue;
        if (record != records_[0] && record != records_[1]) continue;
        cpuRecord_ = record;
        cpuRecordKnown_ = true;
        logLine(L"fight: CPU record found; player is the other side");
        return record;
    }
    return 0;
}

bool FightReader::read(const std::wstring& leftName, const std::wstring& rightName, Gauges& left,
                       Gauges& right, bool& matched) {
    matched = false;
    if (!found()) return false;
    // A record whose marker is gone is a reused allocation, not a fighter.
    const uint64_t marker = static_cast<uint64_t>(base_ + kRecordVtable);
    uint64_t first = 0;
    uint64_t second = 0;
    if (!readValue(records_[0], first) || !readValue(records_[1], second) || first != marker ||
        second != marker) {
        forget();
        return false;
    }
    if (!readGauges(records_[0], left) || !readGauges(records_[1], right)) {
        forget();
        return false;
    }
    const std::wstring firstCode = recordCode(records_[0]);
    const std::wstring secondCode = recordCode(records_[1]);

    // The battle settings say which side the player controls and what that
    // player's character is. Match that against the two records: this is the
    // game's own answer, and unlike the HUD names it is there at round start.
    const std::wstring mine = battlePlayerCode();
    if (!mine.empty()) {
        if (firstCode == mine && secondCode != mine) {
            if (playerRecord_ != records_[0]) {
                logLine(L"fight: player record is the " + mine + L" one, by battle settings");
            }
            playerRecord_ = records_[0];
            matched = true;
            return true;
        }
        if (secondCode == mine && firstCode != mine) {
            if (playerRecord_ != records_[1]) {
                logLine(L"fight: player record is the " + mine + L" one, by battle settings");
            }
            playerRecord_ = records_[1];
            std::swap(left, right);
            matched = true;
            return true;
        }
    }

    // Against the CPU, its controller names the opponent's record outright,
    // which leaves the player's record as the other one. This is the fallback
    // for a mirror match, where both records have the same character.
    const uintptr_t cpu = cpuRecord();
    if (cpu) {
        playerRecord_ = cpu == records_[0] ? records_[1] : records_[0];
        if (cpu == records_[0]) std::swap(left, right);
        matched = true;
        return true;
    }

    const std::wstring leftCode = codeForName(leftName);
    const std::wstring rightCode = codeForName(rightName);
    const bool namesUsable = !leftName.empty() && !rightName.empty() && !leftCode.empty() &&
                             !rightCode.empty() && leftCode != rightCode;
    if (namesUsable) {
        if (firstCode == leftCode && secondCode == rightCode) {
            playerRecord_ = records_[0];
            matched = true;
            return true;
        }
        if (firstCode == rightCode && secondCode == leftCode) {
            playerRecord_ = records_[1];
            std::swap(left, right);
            matched = true;
            return true;
        }
    }

    if (namesUsable) {
        logLine(L"fight: records are " + recordCode(records_[0]) + L" and " +
                recordCode(records_[1]) + L", display " + leftCode + L" and " + rightCode +
                L"; left as memory order");
    }
    return true;
}

}  // namespace sfva
