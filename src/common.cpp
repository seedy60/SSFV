#include "common.h"

#include <cstdio>
#include <iterator>
#include <mutex>

namespace sfva {

HMODULE g_self = nullptr;
std::wstring g_dir;

namespace {
std::mutex g_logLock;
FILE* g_log = nullptr;
std::wstring g_logPath;
}  // namespace

std::wstring moduleDir() {
    if (!g_dir.empty()) return g_dir;
    wchar_t path[MAX_PATH * 2]{};
    DWORD len = GetModuleFileNameW(g_self, path, static_cast<DWORD>(std::size(path)));
    if (!len) return L".";
    std::wstring full(path, len);
    const size_t slash = full.find_last_of(L"\\/");
    g_dir = slash == std::wstring::npos ? L"." : full.substr(0, slash);
    return g_dir;
}

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    int need = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), need);
    return out;
}

std::wstring logPath() {
    return g_logPath;
}

void logInit(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_logLock);
    g_logPath = path;
    g_log = _wfopen(path.c_str(), L"a, ccs=UTF-8");
    if (g_log) {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        fwprintf(g_log, L"\n=== SFAccess Native %04d-%02d-%02d %02d:%02d:%02d ===\n",
                 now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        fflush(g_log);
    }
}

void logLine(const std::string& text) {
    logLine(wide(text));
}

void logLine(const std::wstring& text) {
    {
        std::lock_guard<std::mutex> lock(g_logLock);
        if (g_log) {
            // Every line is timestamped: a failed screen is usually reported
            // with what happened just before it.
            SYSTEMTIME now{};
            GetLocalTime(&now);
            fwprintf(g_log, L"[%02d:%02d:%02d.%03d] %ls\n", now.wHour, now.wMinute, now.wSecond,
                     now.wMilliseconds, text.c_str());
            fflush(g_log);
            if (ferror(g_log)) clearerr(g_log);
            if (ftell(g_log) > 4 * 1024 * 1024) {
                fclose(g_log);
                g_log = _wfopen(g_logPath.c_str(), L"w, ccs=UTF-8");
            }
        }
    }
    OutputDebugStringW((L"SFAccess: " + text + L"\n").c_str());
}

bool readRaw(uintptr_t address, void* out, size_t size) {
    if (!out || !size || !plausiblePointer(address)) return false;
    __try {
        memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool gameHasFocus() {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId();
}

}  // namespace sfva
