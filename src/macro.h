#pragma once

#include <string>

#include "config.h"

namespace sfva {
namespace macro {

enum class Kind { Critical, VSkill, VTrigger };

// Speaks a short confirmation, then plays the character's sequence in the
// background. The return value is the line the narrator should say.
std::wstring trigger(Kind kind, const std::wstring& character, bool facingRight,
                     const Config& config);

// True while the macro's synthetic keys are being sent, and briefly after.
// Those keys arrive with the user's Ctrl+Shift still held, so they would
// otherwise trigger the narrator's own hotkeys.
bool active();

void requestStop();

}  // namespace macro
}  // namespace sfva
