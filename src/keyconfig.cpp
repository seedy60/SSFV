#include "keyconfig.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "common.h"

namespace sfva {
namespace {

// Facts about build v07.011, from the SFV Access project's engine work. The
// layout is fifteen button numbers in the game's action order and a type byte.
constexpr uintptr_t kMovie = 0x318;  // GFxMovie, set only on the showing screen
constexpr uintptr_t kLayout = 0x477;   // the layout being edited
constexpr int kFunctions = 15;
constexpr int kUnassigned = 17;

// The rows' actions in the layout's order. The rows themselves are not in that
// order, which is why each label is mapped rather than counted.
struct ActionSlot {
    const wchar_t* name;
    int slot;
};

constexpr ActionSlot kSlots[] = {
    {L"Light Punch", 0},   {L"Medium Punch", 1}, {L"Light Kick", 2},
    {L"Medium Kick", 3},   {L"Button Combo 3", 4}, {L"Hard Punch", 5},
    {L"Button Combo 4", 6}, {L"Hard Kick", 7},   {L"Throw", 8},
    {L"V-Skill", 9},       {L"V-Trigger", 10},   {L"Button Combo 1", 11},
    {L"Button Combo 2", 12}, {L"V-Shift", 13},
};

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

KeyConfigReader::KeyConfigReader(uintptr_t moduleBase) : base_(moduleBase) {}

bool KeyConfigReader::valid(const uint8_t* layout) {
    for (int i = 0; i < kFunctions; ++i) {
        if (layout[i] > kUnassigned) return false;
    }
    return layout[kFunctions] <= 3;
}

int KeyConfigReader::actionIndex(const std::wstring& action) {
    for (const auto& entry : kSlots) {
        if (action == entry.name) return entry.slot;
    }
    return -1;
}

std::wstring KeyConfigReader::buttonName(int number) {
    switch (number) {
        case 0: return L"up";
        case 1: return L"down";
        case 2: return L"left";
        case 3: return L"right";
        case 4: return L"X";
        case 5: return L"Y";
        case 6: return L"A";
        case 7: return L"B";
        case 8: return L"left bumper";
        case 9: return L"right bumper";
        case 10: return L"left trigger";
        case 11: return L"right trigger";
        case 12: return L"left stick press";
        case 13: return L"right stick press";
        case 14: return L"Start";
        case 15: return L"Back";
        case kUnassigned: return L"none";
        default: return L"button " + std::to_wstring(number);
    }
}

bool KeyConfigReader::readLayout() {
    if (object_) {
        uint8_t current[16]{};
        if (index_.instanceAlive(object_, class_) && readRaw(object_ + kLayout, current, 16) &&
            valid(current)) {
            std::memcpy(layout_.data(), current, sizeof(current));
            return true;
        }
        object_ = 0;
    }

    // The object list is large and the class can still be loading. A failed
    // search is worth repeating, but never on every poll.
    const int64_t now = nowMs();
    if (now < nextTryMs_) return false;
    nextTryMs_ = now + 2000;

    if (!index_.ready()) index_.init(base_);
    if (!index_.ready()) return false;
    if (!class_) class_ = index_.classAddress(L"WSKeyConfigGFxPlayer");
    if (!class_) return false;

    for (uintptr_t candidate : index_.instances(class_)) {
        if (index_.objectName(candidate).rfind(L"Default__", 0) == 0) continue;
        uintptr_t movie = 0;
        if (!readValue(candidate + kMovie, movie) || !plausiblePointer(movie)) continue;
        uint8_t current[16]{};
        if (!readRaw(candidate + kLayout, current, 16) || !valid(current)) continue;
        object_ = candidate;
        std::memcpy(layout_.data(), current, sizeof(current));
        wchar_t address[32]{};
        swprintf_s(address, L"0x%llX", static_cast<unsigned long long>(candidate));
        logLine(L"controller: button layout at " + std::wstring(address) + L", type " +
                std::to_wstring(static_cast<int>(layout_[kFunctions])));
        return true;
    }
    return false;
}

std::wstring KeyConfigReader::binding(const std::wstring& action) {
    const int slot = actionIndex(action);
    if (slot < 0) return {};
    if (!readLayout()) return {};
    return buttonName(layout_[static_cast<size_t>(slot)]);
}

}  // namespace sfva
