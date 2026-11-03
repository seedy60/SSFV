#include "config.h"

#include <iterator>

#include "common.h"

namespace sfva {

Config loadConfig() {
    const std::wstring path = moduleDir() + L"\\sfvaccess.ini";
    Config cfg;
    auto getInt = [&](const wchar_t* section, const wchar_t* key, int fallback) {
        return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, path.c_str()));
    };
    auto getText = [&](const wchar_t* section, const wchar_t* key, const wchar_t* fallback) {
        wchar_t buffer[128]{};
        GetPrivateProfileStringW(section, key, fallback, buffer,
                                 static_cast<DWORD>(std::size(buffer)), path.c_str());
        return std::wstring(buffer);
    };
    cfg.enabled = getInt(L"general", L"narration", 1) != 0;
    cfg.pollMs = getInt(L"general", L"poll_ms", 80);
    cfg.scanMs = getInt(L"general", L"scan_ms", 400);
    cfg.announceDelayMs = getInt(L"general", L"announce_delay_ms", 80);
    cfg.rate = getInt(L"general", L"speech_rate", 0);
    cfg.narrateInBackground = getInt(L"general", L"narrate_in_background", 0) != 0;
    cfg.logFields = getInt(L"general", L"log_fields", 0) != 0;
    cfg.readBlurbs = getInt(L"general", L"read_blurbs", 0) != 0;

    cfg.radar = getInt(L"radar", L"enabled", 1) != 0;
    cfg.radarVolume = getInt(L"radar", L"volume", 35);
    cfg.radarRange = getInt(L"radar", L"range", 1800);
    cfg.radarHeight = getInt(L"radar", L"height", 220);
    cfg.radarNearMs = getInt(L"radar", L"near_ms", 120);
    cfg.radarFarMs = getInt(L"radar", L"far_ms", 650);
    cfg.radarBaseHz = getInt(L"radar", L"base_hz", 520);

    cfg.macros = getInt(L"macros", L"enabled", 1) != 0;
    cfg.macroCriticalKey = getText(L"macros", L"critical_key", L"F1");
    cfg.macroVSkillKey = getText(L"macros", L"vskill_key", L"F2");
    cfg.macroVTriggerKey = getText(L"macros", L"vtrigger_key", L"F3");
    cfg.macroStepMs = getInt(L"macros", L"step_ms", 34);
    cfg.macroHoldMs = getInt(L"macros", L"hold_ms", 1000);

    if (cfg.pollMs < 40) cfg.pollMs = 40;
    if (cfg.pollMs > 2000) cfg.pollMs = 2000;
    if (cfg.scanMs < 100) cfg.scanMs = 100;
    if (cfg.scanMs > 5000) cfg.scanMs = 5000;
    if (cfg.announceDelayMs < 0) cfg.announceDelayMs = 0;
    if (cfg.announceDelayMs > 2000) cfg.announceDelayMs = 2000;
    if (cfg.rate < -10) cfg.rate = -10;
    if (cfg.rate > 10) cfg.rate = 10;
    if (cfg.radarVolume < 0) cfg.radarVolume = 0;
    if (cfg.radarVolume > 100) cfg.radarVolume = 100;
    if (cfg.radarRange < 100) cfg.radarRange = 100;
    if (cfg.radarRange > 20000) cfg.radarRange = 20000;
    if (cfg.radarHeight < 10) cfg.radarHeight = 10;
    if (cfg.radarHeight > 2000) cfg.radarHeight = 2000;
    if (cfg.radarNearMs < 60) cfg.radarNearMs = 60;
    if (cfg.radarNearMs > 2000) cfg.radarNearMs = 2000;
    if (cfg.radarFarMs < 80) cfg.radarFarMs = 80;
    if (cfg.radarFarMs > 3000) cfg.radarFarMs = 3000;
    if (cfg.radarBaseHz < 100) cfg.radarBaseHz = 100;
    if (cfg.radarBaseHz > 2000) cfg.radarBaseHz = 2000;
    if (cfg.macroStepMs < 10) cfg.macroStepMs = 10;
    if (cfg.macroStepMs > 100) cfg.macroStepMs = 100;
    if (cfg.macroHoldMs < 50) cfg.macroHoldMs = 50;
    if (cfg.macroHoldMs > 2000) cfg.macroHoldMs = 2000;

    logLine(L"config: " + path + L" (narration " + (cfg.enabled ? L"on" : L"off") + L")");
    return cfg;
}

}  // namespace sfva
