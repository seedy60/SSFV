#include "speech.h"

#include <objbase.h>
#include <sapi.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "common.h"

namespace sfva {
namespace speech {
namespace {

// Prism's C ABI, as exposed by prism.dll (MPL-2.0). It is output only: it
// hands a string to whichever screen reader or voice is running.
struct PrismConfig {
    uint8_t version;
    void* registry;
    void* availability_callback;
    void* availability_userdata;
    uint32_t availability_poll_interval_ms;
    uint32_t availability_debounce_samples;
    uint32_t availability_backoff_max_ms;
    bool availability_auto_power_manage;
    void* availability_baseline_callback;
};

using ConfigInit = PrismConfig (*)();
using Init = void* (*)(PrismConfig*);
using Shutdown = void (*)(void*);
using RegistryCreateBest = void* (*)(void*);
using BackendInitialize = int (*)(void*);
using BackendName = const char* (*)(void*);
using BackendOutput = int (*)(void*, const char*, bool);
using BackendStop = int (*)(void*);
using BackendFree = void (*)(void*);

HMODULE g_prism = nullptr;
void* g_context = nullptr;
void* g_backend = nullptr;
BackendOutput g_prismOutput = nullptr;
BackendStop g_prismStop = nullptr;
std::wstring g_backendName = L"none";

ISpVoice* g_voice = nullptr;
bool g_useSapi = false;
int g_rate = 0;

// NVDA's controller client. It is the supported way for another program to
// hand NVDA a string: NVDA decides the synth, voice and rate.
typedef long(__stdcall* NvdaTestFn)();
typedef long(__stdcall* NvdaSpeakFn)(const wchar_t*);
typedef long(__stdcall* NvdaCancelFn)();
HMODULE g_nvda = nullptr;
NvdaSpeakFn g_nvdaSpeak = nullptr;
NvdaCancelFn g_nvdaCancel = nullptr;

struct Item {
    std::wstring text;
    bool interrupt = false;
    uint64_t generation = 0;
};

std::mutex g_lock;
std::condition_variable g_wake;
std::deque<Item> g_queue;
std::thread g_worker;
std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_generation{0};

std::wstring prismPath() {
    std::wstring beside = moduleDir() + L"\\prism.dll";
    if (GetFileAttributesW(beside.c_str()) != INVALID_FILE_ATTRIBUTES) return beside;
    return {};
}

bool initPrism() {
    const std::wstring path = prismPath();
    if (path.empty()) return false;
    g_prism = LoadLibraryW(path.c_str());
    if (!g_prism) {
        logLine(L"prism.dll is present but would not load");
        return false;
    }
    auto configInit = reinterpret_cast<ConfigInit>(GetProcAddress(g_prism, "prism_config_init"));
    auto init = reinterpret_cast<Init>(GetProcAddress(g_prism, "prism_init"));
    auto registryCreateBest =
        reinterpret_cast<RegistryCreateBest>(GetProcAddress(g_prism, "prism_registry_create_best"));
    auto backendInitialize =
        reinterpret_cast<BackendInitialize>(GetProcAddress(g_prism, "prism_backend_initialize"));
    auto backendName =
        reinterpret_cast<BackendName>(GetProcAddress(g_prism, "prism_backend_name"));
    g_prismOutput = reinterpret_cast<BackendOutput>(GetProcAddress(g_prism, "prism_backend_output"));
    g_prismStop = reinterpret_cast<BackendStop>(GetProcAddress(g_prism, "prism_backend_stop"));
    if (!configInit || !init || !registryCreateBest || !backendInitialize || !backendName ||
        !g_prismOutput) {
        logLine(L"prism.dll is missing the exports this mod uses");
        return false;
    }
    PrismConfig config = configInit();
    g_context = init(&config);
    if (!g_context) {
        logLine(L"prism: initialisation failed");
        return false;
    }
    g_backend = registryCreateBest(g_context);
    if (!g_backend) {
        logLine(L"prism: no usable screen reader or voice backend");
        return false;
    }
    const int result = backendInitialize(g_backend);
    // 15 is Prism's "already initialised" result, not a failure.
    if (result != 0 && result != 15) {
        logLine(L"prism: backend initialisation returned " + std::to_wstring(result));
        return false;
    }
    const char* name = backendName(g_backend);
    g_backendName = name ? wide(name) : L"prism";
    logLine(L"speech backend: prism (" + g_backendName + L")");
    return true;
}

bool initNvda() {
    const wchar_t* names[] = {L"nvdaControllerClient64.dll", L"nvdaControllerClient.dll"};
    bool present = false;
    for (const wchar_t* name : names) {
        const std::wstring path = moduleDir() + L"\\" + name;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        present = true;
        g_nvda = LoadLibraryW(path.c_str());
        if (!g_nvda) {
            logLine(L"NVDA client " + std::wstring(name) + L" would not load (error " +
                    std::to_wstring(GetLastError()) + L")");
            continue;
        }
        auto test = reinterpret_cast<NvdaTestFn>(GetProcAddress(g_nvda, "nvdaController_testIfRunning"));
        g_nvdaSpeak = reinterpret_cast<NvdaSpeakFn>(GetProcAddress(g_nvda, "nvdaController_speakText"));
        g_nvdaCancel = reinterpret_cast<NvdaCancelFn>(GetProcAddress(g_nvda, "nvdaController_cancelSpeech"));
        if (!test || !g_nvdaSpeak) {
            logLine(L"NVDA client " + std::wstring(name) + L" is missing the controller exports");
            FreeLibrary(g_nvda);
            g_nvda = nullptr;
            g_nvdaSpeak = nullptr;
            g_nvdaCancel = nullptr;
            continue;
        }
        const long status = test();
        if (status != 0) {
            // NVDA is not running, so leave it to SAPI. Clear the pointers:
            // the worker chooses the backend by looking at them.
            logLine(L"NVDA controller client loaded but NVDA is not running (status " +
                    std::to_wstring(status) + L")");
            g_nvdaSpeak = nullptr;
            g_nvdaCancel = nullptr;
            return false;
        }
        g_backendName = L"NVDA";
        logLine(L"speech backend: NVDA (controller client " + std::wstring(name) + L")");
        return true;
    }
    if (!present) logLine(L"NVDA client DLL is not beside the mod; using SAPI");
    return false;
}

// SAPI is created on the speech thread, the apartment that will call it.
bool initSapiOnThisThread() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        logLine(L"SAPI: COM initialisation failed");
        return false;
    }
    if (FAILED(CoCreateInstance(__uuidof(SpVoice), nullptr, CLSCTX_ALL, IID_ISpVoice,
                                reinterpret_cast<void**>(&g_voice))) ||
        !g_voice) {
        logLine(L"SAPI: no voice available");
        CoUninitialize();
        return false;
    }
    g_voice->SetRate(g_rate);
    logLine(L"speech backend: SAPI");
    return true;
}

void workerLoop() {
    const bool comReady = g_useSapi && initSapiOnThisThread();
    while (true) {
        Item item;
        {
            std::unique_lock<std::mutex> lock(g_lock);
            g_wake.wait(lock, [] { return !g_running || !g_queue.empty(); });
            if (!g_running && g_queue.empty()) break;
            item = std::move(g_queue.front());
            g_queue.pop_front();
            if (item.generation != g_generation.load()) continue;  // superseded
        }
        if (g_prismOutput && g_backend) {
            const std::string text = utf8(item.text);
            g_prismOutput(g_backend, text.c_str(), item.interrupt);
        } else if (g_nvdaSpeak) {
            if (item.interrupt && g_nvdaCancel) g_nvdaCancel();
            const long status = g_nvdaSpeak(item.text.c_str());
            if (status != 0) {
                static bool reported = false;
                if (!reported) {
                    reported = true;
                    logLine(L"NVDA speakText returned " + std::to_wstring(status));
                }
            }
        } else if (g_voice) {
            DWORD flags = SPF_ASYNC;
            if (item.interrupt) flags |= SPF_PURGEBEFORESPEAK;
            const HRESULT result = g_voice->Speak(item.text.c_str(), flags, nullptr);
            if (FAILED(result)) {
                static bool reported = false;
                if (!reported) {
                    reported = true;
                    logLine(L"SAPI Speak failed, 0x" + std::to_wstring(static_cast<unsigned>(result)));
                }
            }
        }
    }
    if (comReady) CoUninitialize();
}

}  // namespace

bool init(int rate) {
    auto startWorker = [] {
        g_running = true;
        g_worker = std::thread(workerLoop);
        // Detached: the game exits without unloading the DLL, and a joinable
        // std::thread destroyed by the CRT at process exit calls terminate.
        g_worker.detach();
    };
    if (initPrism()) {
        startWorker();
        return true;
    }
    if (initNvda()) {
        startWorker();
        return true;
    }
    // SAPI ships with Windows, so this path always exists; it only fails if
    // COM or the voice itself does, and that is logged from the worker.
    g_useSapi = true;
    g_rate = rate;
    g_backendName = L"SAPI";
    startWorker();
    return true;
}

void speak(const std::wstring& text, bool interrupt) {
    if (text.find_first_not_of(L" \t\r\n") == std::wstring::npos) return;
    if (!g_running) return;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (interrupt) {
            g_queue.clear();
            g_generation.fetch_add(1);
        }
        g_queue.push_back({text, interrupt, g_generation.load()});
    }
    g_wake.notify_one();
}

void requestStop() {
    if (!g_running.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_generation.fetch_add(1);
        g_queue.clear();
    }
    g_wake.notify_all();
}

void stop() {
    if (g_prismStop && g_backend) {
        g_prismStop(g_backend);
    } else if (g_nvdaCancel) {
        g_nvdaCancel();
    } else if (g_voice) {
        g_voice->Speak(nullptr, SPF_PURGEBEFORESPEAK | SPF_ASYNC, nullptr);
    }
}

std::wstring backendName() {
    return g_backendName;
}

void shutdown() {
    if (!g_running.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_generation.fetch_add(1);
        g_queue.clear();
    }
    g_wake.notify_all();
    if (g_worker.joinable()) g_worker.join();
    if (g_voice) {
        g_voice->Release();
        g_voice = nullptr;
    }
    logLine(L"speech stopped");
}

}  // namespace speech
}  // namespace sfva
