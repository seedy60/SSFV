#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "object_index.h"

namespace sfva {

// The button layout the Controller Setting screen is editing. The screen draws
// each assigned button as a picture, so the text fields hold only the action
// names. The layout lives on the screen's own game object:
// WSKeyConfigGFxPlayer + 0x477, fifteen button numbers and a controller type.
// Unlike the ButtonConfig script objects, it follows an edit as it is made.
class KeyConfigReader {
public:
    explicit KeyConfigReader(uintptr_t moduleBase);

    // The assigned button in words, or empty while the layout cannot be read.
    std::wstring binding(const std::wstring& action);

private:
    bool readLayout();
    static int actionIndex(const std::wstring& action);
    static std::wstring buttonName(int number);
    static bool valid(const uint8_t* layout);

    uintptr_t base_ = 0;
    ObjectIndex index_;
    uintptr_t class_ = 0;
    uintptr_t object_ = 0;
    std::array<uint8_t, 16> layout_{};
    int64_t nextTryMs_ = 0;
};

}  // namespace sfva
