#pragma once

#include <cstdint>
#include <string>

namespace sfva {

struct Config;

namespace radar {

// Starts the optional positional pulse. `base` is the game executable's
// module base. The pulse only runs while `update` reports a live round.
void start(uintptr_t base, const Config& config);
void update(bool roundActive);
void setEnabled(bool on);
bool enabled();
// The player's canonical character name, read from the player's costume.
// Empty until a round has been seen.
std::wstring playerCharacter();
// Whether the player is currently facing right. Macro directions are written
// facing right; the macro mirrors them when this is false.
bool playerFacingRight();
std::wstring status();
// The opponent's current side, height and distance, for the on-demand key.
std::wstring describe();
void requestStop();

}  // namespace radar
}  // namespace sfva
