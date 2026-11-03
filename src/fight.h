#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "object_index.h"

namespace sfva {

struct Gauges {
    int health = 0;
    int healthMax = 0;
    int critical = 0;
    int criticalMax = 0;
    int v = 0;
    int vMax = 0;

    bool valid() const;
    std::wstring words(const std::wstring& label) const;
};

// The two fighters' gauges, read from the game's own records. The records are
// found by their type marker, which is a pointer to a function table at a
// fixed offset in the executable.
class FightReader {
public:
    explicit FightReader(uintptr_t moduleBase);

    // Finds the records. Takes about a second; call it when a fight is on
    // screen, not on every poll.
    bool scan();
    bool found() const { return records_.size() == 2; }
    void forget() {
        records_.clear();
        resetOrder();
    }

    // The left and right fighters' gauges. When the names on the fight
    // display resolve to the two records, left is the player's side and right
    // the opponent's; otherwise the order is the records' in memory and
    // `matched` is false.
    bool read(const std::wstring& leftName, const std::wstring& rightName, Gauges& left,
              Gauges& right, bool& matched);

    // The canonical name for a fighter, or empty if the text is not one.
    static std::wstring knownName(const std::wstring& text);

    // The canonical name for a character code such as GIL or NSH.
    static std::wstring nameForCode(const std::wstring& code);

    // The player's character from the first record. Used when the fight
    // display's names have not all been found; macros only need a section.
    std::wstring playerCharacter() const;

private:
    bool readGauges(uintptr_t record, Gauges& out) const;
    std::wstring recordCode(uintptr_t record) const;
    static std::wstring codeForName(const std::wstring& name);

    // The CPU's controller holds its fighter's record at +0x6B0. Its record is
    // the opponent's, so the other record is the player's; this also settles a
    // mirror match, where the character names cannot tell the sides apart.
    uintptr_t cpuRecord();

    // The player's character, from the live battle settings' USER entry. This
    // is the game's own answer to which side the player is on, so it works
    // before the HUD has painted and in a mirror match.
    std::wstring battlePlayerCode();
    uintptr_t battleSetting();

    void resetOrder();

    uintptr_t base_ = 0;
    std::vector<uintptr_t> records_;
    ObjectIndex index_;
    uintptr_t cpuRecord_ = 0;
    bool cpuRecordKnown_ = false;
    int64_t nextCpuTryMs_ = 0;
    uintptr_t setting_ = 0;  // the live KWBattleSetting, not its saved copy
    int64_t nextSettingTryMs_ = 0;
    uintptr_t playerRecord_ = 0;  // set once the player's side is known
    std::wstring playerCode_;     // the USER entry's character code, "Z31"
};

}  // namespace sfva
