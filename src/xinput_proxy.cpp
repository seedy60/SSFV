// The mod ships as xinput1_3.dll, forwarding the whole XInput 1.3 surface to
// Windows' own copy. Street Fighter V imports this DLL by ordinal (2 and 3),
// so the proxy is loaded by the loader before any game code runs.
//
// Forwarding all seven exports matters: any later module that asks for
// xinput1_3.dll by name receives this one, and must still work.
#include <windows.h>

#include <mutex>
#include <string>

namespace {

std::once_flag g_once;
HMODULE g_real = nullptr;

HMODULE realXInput() {
    std::call_once(g_once, [] {
        wchar_t windows[MAX_PATH]{};
        if (!GetWindowsDirectoryW(windows, MAX_PATH)) return;
        const std::wstring path = std::wstring(windows) + L"\\System32\\xinput1_3.dll";
        g_real = LoadLibraryW(path.c_str());
    });
    return g_real;
}

FARPROC resolve(const char* name) {
    HMODULE module = realXInput();
    return module ? GetProcAddress(module, name) : nullptr;
}

constexpr DWORD kNotConnected = ERROR_DEVICE_NOT_CONNECTED;

}  // namespace

extern "C" {

// The pointer types are opaque here: the mod forwards the call and never looks
// inside the structures, so the ABI is all that matters.
DWORD WINAPI XInputGetState(DWORD index, void* state) {
    using Fn = DWORD(WINAPI*)(DWORD, void*);
    const Fn fn = reinterpret_cast<Fn>(resolve("XInputGetState"));
    return fn ? fn(index, state) : kNotConnected;
}

DWORD WINAPI XInputSetState(DWORD index, void* vibration) {
    using Fn = DWORD(WINAPI*)(DWORD, void*);
    const Fn fn = reinterpret_cast<Fn>(resolve("XInputSetState"));
    return fn ? fn(index, vibration) : kNotConnected;
}

DWORD WINAPI XInputGetCapabilities(DWORD index, DWORD flags, void* capabilities) {
    using Fn = DWORD(WINAPI*)(DWORD, DWORD, void*);
    const Fn fn = reinterpret_cast<Fn>(resolve("XInputGetCapabilities"));
    return fn ? fn(index, flags, capabilities) : kNotConnected;
}

void WINAPI XInputEnable(BOOL enable) {
    using Fn = void(WINAPI*)(BOOL);
    const Fn fn = reinterpret_cast<Fn>(resolve("XInputEnable"));
    if (fn) fn(enable);
}

DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD index, GUID* render, GUID* capture) {
    using Fn = DWORD(WINAPI*)(DWORD, GUID*, GUID*);
    const Fn fn = reinterpret_cast<Fn>(resolve("XInputGetDSoundAudioDeviceGuids"));
    return fn ? fn(index, render, capture) : kNotConnected;
}

DWORD WINAPI XInputGetBatteryInformation(DWORD index, BYTE deviceType, void* information) {
    using Fn = DWORD(WINAPI*)(DWORD, BYTE, void*);
    const Fn fn = reinterpret_cast<Fn>(resolve("XInputGetBatteryInformation"));
    return fn ? fn(index, deviceType, information) : kNotConnected;
}

DWORD WINAPI XInputGetKeystroke(DWORD index, DWORD reserved, void* keystroke) {
    using Fn = DWORD(WINAPI*)(DWORD, DWORD, void*);
    const Fn fn = reinterpret_cast<Fn>(resolve("XInputGetKeystroke"));
    return fn ? fn(index, reserved, keystroke) : kNotConnected;
}

}  // extern "C"
