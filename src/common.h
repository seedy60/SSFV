// Shared basics: windows, logging, UTF-8 and guarded reads.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>

namespace sfva {

extern HMODULE g_self;
extern std::wstring g_dir;

std::wstring moduleDir();

std::string utf8(const std::wstring& wide);
std::wstring wide(const std::string& text);

void logInit(const std::wstring& path);
void logLine(const std::string& text);
void logLine(const std::wstring& text);
std::wstring logPath();

// A read that cannot take the game down. Every address in this mod came from
// reading the game's own pointers, so a stale one is expected on every screen
// change; a bad read must fail quietly.
bool readRaw(uintptr_t address, void* out, size_t size);

template <typename T>
bool readValue(uintptr_t address, T& out) {
    return readRaw(address, &out, sizeof(T));
}

inline bool plausiblePointer(uintptr_t value) {
    return value >= 0x10000 && value < 0x7FFFFFFFFFFFULL;
}

// The foreground window belongs to this process. Used to keep hotkeys from
// stealing Ctrl+Shift combinations from other programs.
bool gameHasFocus();

}  // namespace sfva
