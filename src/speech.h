#pragma once

#include <string>

namespace sfva {
namespace speech {

// Picks the best available output: Prism (any supported screen reader),
// NVDA's own controller client, then SAPI. Both the first two are optional and
// are loaded from beside the mod.
bool init(int rate);
void speak(const std::wstring& text, bool interrupt);
void stop();
void requestStop();
void shutdown();
std::wstring backendName();

}  // namespace speech
}  // namespace sfva
