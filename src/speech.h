#pragma once

#include <string>

namespace ssfv {
namespace speech {

// Picks the configured output. The default, "auto", lets Prism choose the
// best screen reader or voice it knows about. An explicit Prism backend name
// (for example "nvda" or "jaws") selects that backend through Prism; "sapi"
// uses Windows SAPI directly. Prism is loaded from beside the mod, and SAPI is
// the fallback when Prism is unavailable.
bool init(int rate, const std::wstring& backend);
void speak(const std::wstring& text, bool interrupt);
void stop();
void requestStop();
void shutdown();
std::wstring backendName();

}  // namespace speech
}  // namespace ssfv
