#include "speech.h"

#include <objbase.h>
#include <sapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cwctype>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "common.h"

namespace ssfv {
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
using RegistryCreateBest = void* (*)(void*);
using RegistryCreate = void* (*)(void*, uint64_t);
using RegistryCount = size_t (*)(void*);
using RegistryIdAt = uint64_t (*)(void*, size_t);
using RegistryIdByName = uint64_t (*)(void*, const char*);
using RegistryName = const char* (*)(void*, uint64_t);
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
BackendFree g_prismFree = nullptr;
RegistryCreate g_registryCreate = nullptr;
RegistryCount g_registryCount = nullptr;
RegistryIdAt g_registryIdAt = nullptr;
RegistryIdByName g_registryIdByName = nullptr;
RegistryName g_registryName = nullptr;
std::wstring g_backendName = L"none";

ISpVoice* g_voice = nullptr;
bool g_useSapi = false;
int g_rate = 0;

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

std::wstring lower(std::wstring text) {
    for (wchar_t& c : text) c = static_cast<wchar_t>(towlower(c));
    return text;
}

// Resolve a backend name through Prism's own registry. Names are published in
// upper case (NVDA, JAWS, SAPI), so try that first and then compare without
// case.
uint64_t backendId(const std::string& name) {
    std::string upper = name;
    for (char& c : upper) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    if (g_registryIdByName) {
        const uint64_t id = g_registryIdByName(g_context, upper.c_str());
        if (id) return id;
    }
    if (!g_registryCount || !g_registryIdAt || !g_registryName) return 0;
    const size_t count = g_registryCount(g_context);
    for (size_t i = 0; i < count; ++i) {
        const uint64_t id = g_registryIdAt(g_context, i);
        const char* registered = g_registryName(g_context, id);
        if (registered && _stricmp(registered, upper.c_str()) == 0) return id;
    }
    return 0;
}

bool initPrism(const std::wstring& requested) {
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
    g_registryCreate =
        reinterpret_cast<RegistryCreate>(GetProcAddress(g_prism, "prism_registry_create"));
    g_registryCount =
        reinterpret_cast<RegistryCount>(GetProcAddress(g_prism, "prism_registry_count"));
    g_registryIdAt =
        reinterpret_cast<RegistryIdAt>(GetProcAddress(g_prism, "prism_registry_id_at"));
    g_registryIdByName =
        reinterpret_cast<RegistryIdByName>(GetProcAddress(g_prism, "prism_registry_id"));
    g_registryName =
        reinterpret_cast<RegistryName>(GetProcAddress(g_prism, "prism_registry_name"));
    auto backendInitialize =
        reinterpret_cast<BackendInitialize>(GetProcAddress(g_prism, "prism_backend_initialize"));
    auto backendName =
        reinterpret_cast<BackendName>(GetProcAddress(g_prism, "prism_backend_name"));
    g_prismOutput = reinterpret_cast<BackendOutput>(GetProcAddress(g_prism, "prism_backend_output"));
    g_prismStop = reinterpret_cast<BackendStop>(GetProcAddress(g_prism, "prism_backend_stop"));
    g_prismFree = reinterpret_cast<BackendFree>(GetProcAddress(g_prism, "prism_backend_free"));
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
    const std::wstring choice = lower(requested);
    const bool best = choice.empty() || choice == L"auto" || choice == L"prism" ||
                      choice == L"best";
    uint64_t requestedId = 0;
    if (!best) {
        requestedId = backendId(utf8(choice));
        if (!requestedId) {
            logLine(L"prism: backend \"" + requested + L"\" is not registered; using best");
        }
    }

    auto activate = [&](void* backend) {
        g_backend = backend;
        if (!g_backend) return false;
        int result = backendInitialize(g_backend);
        // 15 is Prism's "already initialised" result, not a failure.
        if (result == 15) result = 0;
        if (result != 0) {
            logLine(L"prism: backend initialisation returned " + std::to_wstring(result));
            if (g_prismFree) g_prismFree(g_backend);
            g_backend = nullptr;
            return false;
        }
        return true;
    };

    void* chosen = nullptr;
    if (requestedId && g_registryCreate) {
        chosen = g_registryCreate(g_context, requestedId);
    }
    if (!activate(chosen)) {
        if (requestedId) logLine(L"prism: requested backend failed; using best");
        if (!activate(registryCreateBest(g_context))) {
            logLine(L"prism: no usable screen reader or voice backend");
            return false;
        }
    }
    const char* name = backendName(g_backend);
    g_backendName = name ? wide(name) : L"prism";
    logLine(L"speech backend: prism (" + g_backendName + L")");
    return true;
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

bool init(int rate, const std::wstring& backend) {
    auto startWorker = [] {
        g_running = true;
        g_worker = std::thread(workerLoop);
        // Detached: the game exits without unloading the DLL, and a joinable
        // std::thread destroyed by the CRT at process exit calls terminate.
        g_worker.detach();
    };
    const std::wstring choice = lower(backend);
    if (choice == L"sapi") {
        logLine(L"speech backend: SAPI (configured)");
    } else if (initPrism(choice)) {
        startWorker();
        return true;
    } else {
        logLine(L"speech: Prism is unavailable; using SAPI");
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
}  // namespace ssfv
