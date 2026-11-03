// SFAccess Native: an in-process menu narrator for Street Fighter V.
//
// The mod is an xinput1_3.dll proxy. The game imports xinput1_3.dll at startup,
// so copying this DLL beside StreetFighterV.exe loads it before the game's own
// code runs; no injector, no launcher, and Steam's launch options stay clean.
//
// Everything here is read-only. The only process memory written is the
// loader's own image mapping.
#include <iterator>
#include <process.h>

#include "common.h"
#include "hotkeys.h"
#include "narrator.h"
#include "speech.h"

namespace {

unsigned WINAPI initThread(void*) {
    // The proxy is loaded before the game has a window or any Scaleform
    // movie. A short wait keeps the log's first lines about a real screen
    // instead of about an empty heap.
    Sleep(1500);

    sfva::logInit(sfva::moduleDir() + L"\\sfvaccess-native.log");
    sfva::logLine(L"SFAccess Native 0.1.0 starting");

    wchar_t exePath[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    sfva::logLine(std::wstring(L"game: ") + exePath);

    sfva::narrator::start();
    sfva::hotkeys::start();
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        sfva::g_self = instance;
        DisableThreadLibraryCalls(instance);
        HANDLE thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, initThread, nullptr, 0, nullptr));
        if (thread) CloseHandle(thread);
    }
    // DLL_PROCESS_DETACH runs while the loader lock is held. Joining threads
    // or releasing COM there can deadlock the process that is already exiting,
    // but the workers must be told to stop before the DLL's globals are
    // destroyed under them: a thread still walking a std::mutex or std::map at
    // static destruction is the crash the game used to show on exit. The
    // threads are detached, so this only sets flags and waits briefly.
    if (reason == DLL_PROCESS_DETACH) {
        sfva::narrator::requestStop();
        sfva::hotkeys::requestStop();
        sfva::speech::requestStop();
        Sleep(250);
    }
    return TRUE;
}
