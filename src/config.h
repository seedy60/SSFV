#pragma once

#include <string>

namespace sfva {

struct Config {
    bool enabled = true;
    int pollMs = 80;           // how often the screen is read
    int scanMs = 400;          // how often Scaleform's heap is swept for new fields
    int announceDelayMs = 80;  // settle time before a changed screen is spoken
    int rate = 0;              // SAPI rate, -10..10
    bool narrateInBackground = false;
    bool logFields = false;
    bool readBlurbs = false;   // include long Arcade path descriptions

    // Character radar: a panned, pitched pulse that quickens as the opponent
    // gets closer.
    bool radar = true;
    int radarVolume = 35;
    int radarRange = 1800;
    int radarHeight = 220;
    int radarNearMs = 120;
    int radarFarMs = 650;
    int radarBaseHz = 520;

    // Macros. The keys are pressed with Ctrl+Shift; the game receives the
    // sequence itself as ordinary keyboard input.
    bool macros = true;
    std::wstring macroCriticalKey = L"F1";
    std::wstring macroVSkillKey = L"F2";
    std::wstring macroVTriggerKey = L"F3";
    int macroStepMs = 34;
    int macroHoldMs = 1000;
};

Config loadConfig();

}  // namespace sfva
