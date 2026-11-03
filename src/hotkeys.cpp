#include "hotkeys.h"

#include <atomic>
#include <chrono>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

#include "common.h"
#include "config.h"
#include "macro.h"
#include "narrator.h"

namespace sfva {
namespace hotkeys {
namespace {

constexpr int kRepeat = 0x5301;
constexpr int kNext = 0x5302;
constexpr int kPrevious = 0x5303;
constexpr int kReadAll = 0x5304;
constexpr int kToggle = 0x5305;
constexpr int kStatus = 0x5306;
constexpr int kSnapshot = 0x5307;
constexpr int kSilence = 0x5308;
constexpr int kReadFight = 0x5309;
constexpr int kToggleRadar = 0x530A;
constexpr int kDescribeRadar = 0x530E;
constexpr int kMacroCritical = 0x530B;
constexpr int kMacroVSkill = 0x530C;
constexpr int kMacroVTrigger = 0x530D;

std::atomic<bool> g_running{false};
std::atomic<bool> g_stopRequested{false};
std::thread g_thread;
Config g_config;

constexpr UINT kModifiers = MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT;

struct Binding {
    int id;
    UINT key;
    narrator::Action action;
    std::wstring label;
};

std::vector<Binding> g_bindings;

bool parseKey(const std::wstring& text, UINT& key) {
    std::wstring word;
    for (wchar_t c : text) {
        if (!iswspace(c)) word.push_back(static_cast<wchar_t>(towupper(c)));
    }
    if (word.empty()) return false;
    if (word.size() >= 2 && word[0] == L'F') {
        bool digits = true;
        int number = 0;
        for (size_t i = 1; i < word.size(); ++i) {
            if (word[i] < L'0' || word[i] > L'9') {
                digits = false;
                break;
            }
            number = number * 10 + (word[i] - L'0');
        }
        if (digits && number >= 1 && number <= 24) {
            key = VK_F1 + static_cast<UINT>(number - 1);
            return true;
        }
    }
    if (word.size() == 1) {
        if ((word[0] >= L'A' && word[0] <= L'Z') || (word[0] >= L'0' && word[0] <= L'9')) {
            key = static_cast<UINT>(word[0]);
            return true;
        }
    }
    struct Named {
        const wchar_t* name;
        UINT key;
    };
    static constexpr Named kNames[] = {
        {L"SPACE", VK_SPACE},     {L"TAB", VK_TAB},       {L"ENTER", VK_RETURN},
        {L"INSERT", VK_INSERT},   {L"DELETE", VK_DELETE}, {L"HOME", VK_HOME},
        {L"END", VK_END},         {L"PAGEUP", VK_PRIOR},  {L"PAGEDOWN", VK_NEXT},
        {L"UP", VK_UP},           {L"DOWN", VK_DOWN},     {L"LEFT", VK_LEFT},
        {L"RIGHT", VK_RIGHT},
    };
    for (const auto& entry : kNames) {
        if (word == entry.name) {
            key = entry.key;
            return true;
        }
    }
    return false;
}

void addBinding(std::vector<Binding>& bindings, int id, UINT key, narrator::Action action,
                const std::wstring& label) {
    for (const auto& binding : bindings) {
        if (binding.key == key) {
            logLine(L"hotkey duplicate ignored: " + label);
            return;
        }
    }
    bindings.push_back({id, key, action, label});
}

void buildBindings() {
    g_bindings = {
        {kRepeat, 'R', narrator::Action::Repeat, L"Ctrl+Shift+R"},
        {kNext, VK_DOWN, narrator::Action::NextLine, L"Ctrl+Shift+Down"},
        {kPrevious, VK_UP, narrator::Action::PreviousLine, L"Ctrl+Shift+Up"},
        {kReadAll, 'A', narrator::Action::ReadAll, L"Ctrl+Shift+A"},
        {kToggle, 'M', narrator::Action::Toggle, L"Ctrl+Shift+M"},
        {kStatus, 'I', narrator::Action::Status, L"Ctrl+Shift+I"},
        {kSnapshot, 'S', narrator::Action::Snapshot, L"Ctrl+Shift+S"},
        {kSilence, 'X', narrator::Action::Silence, L"Ctrl+Shift+X"},
        {kReadFight, 'H', narrator::Action::ReadFight, L"Ctrl+Shift+H"},
        {kToggleRadar, 'P', narrator::Action::ToggleRadar, L"Ctrl+Shift+P"},
        {kDescribeRadar, 'D', narrator::Action::DescribeRadar, L"Ctrl+Shift+D"},
    };

    if (!g_config.macros) return;
    const struct {
        int id;
        const std::wstring* text;
        narrator::Action action;
        const wchar_t* name;
    } macros[] = {
        {kMacroCritical, &g_config.macroCriticalKey, narrator::Action::MacroCritical, L"Critical Art"},
        {kMacroVSkill, &g_config.macroVSkillKey, narrator::Action::MacroVSkill, L"V-Skill"},
        {kMacroVTrigger, &g_config.macroVTriggerKey, narrator::Action::MacroVTrigger,
         L"V-Trigger"},
    };
    for (const auto& macro : macros) {
        UINT key = 0;
        if (!parseKey(*macro.text, key)) {
            logLine(L"macro key not understood: " + *macro.text);
            continue;
        }
        std::wstring label = L"Ctrl+Shift+" + *macro.text;
        addBinding(g_bindings, macro.id, key, macro.action, label);
    }
}

void registerAll() {
    for (const auto& binding : g_bindings) {
        if (!RegisterHotKey(nullptr, binding.id, kModifiers, binding.key)) {
            // Another program can hold a combination; say so rather than
            // leaving a key that looks broken.
            logLine(L"hotkey unavailable: " + binding.label + L" (error " +
                    std::to_wstring(GetLastError()) + L")");
        }
    }
}

void unregisterAll() {
    for (const auto& binding : g_bindings) {
        UnregisterHotKey(nullptr, binding.id);
    }
}

void loop() {
    bool registered = false;
    while (g_running && !g_stopRequested) {
        // Only while the game is in front: Ctrl+Shift+R and friends belong to
        // every other program when it is not.
        const bool focus = gameHasFocus();
        if (focus && !registered) {
            registerAll();
            registered = true;
        } else if (!focus && registered) {
            unregisterAll();
            registered = false;
        }
        if (!registered) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        MsgWaitForMultipleObjects(0, nullptr, FALSE, 200, QS_HOTKEY);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message != WM_HOTKEY) continue;
            // A macro sends D, H and M as real key events while the user's
            // Ctrl+Shift are still down. Those events match Ctrl+Shift+D,
            // Ctrl+Shift+H and Ctrl+Shift+M, so a macro would otherwise
            // narrate the opponent and toggle narration while it plays.
            if (macro::active()) continue;
            for (const auto& binding : g_bindings) {
                if (message.wParam == static_cast<WPARAM>(binding.id)) {
                    narrator::post(binding.action);
                    break;
                }
            }
        }
    }
    if (registered) unregisterAll();
}

}  // namespace

void start() {
    if (g_running) return;
    g_config = loadConfig();
    buildBindings();
    g_running = true;
    g_thread = std::thread(loop);
    // See narrator::start: a joinable thread still alive at process exit is
    // destroyed by the CRT and terminates the process.
    g_thread.detach();
    logLine(L"hotkeys started");
}

void requestStop() {
    g_stopRequested = true;
    g_running = false;
}

void shutdown() {
    requestStop();
    if (g_thread.joinable()) g_thread.join();
}

}  // namespace hotkeys
}  // namespace sfva
