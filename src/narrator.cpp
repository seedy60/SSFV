#include "narrator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <deque>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "common.h"
#include "config.h"
#include "fight.h"
#include "keyconfig.h"
#include "macro.h"
#include "radar.h"
#include "scaleform.h"
#include "speech.h"

#include <memory>

namespace sfva {
namespace narrator {
namespace {

using Clock = std::chrono::steady_clock;

Config g_config;
Reader g_reader{0};
std::unique_ptr<FightReader> g_fight;
std::unique_ptr<KeyConfigReader> g_keyConfig;
uintptr_t g_base = 0;
int g_lastTimer = -1;
int g_fightClock = -1;
std::wstring g_playerName;
bool g_timer99 = false;
Clock::time_point g_timer99Since;
Clock::time_point g_nextFightScan = Clock::time_point::min();
bool g_enabled = true;
bool g_running = false;
std::atomic<bool> g_stopRequested{false};
std::thread g_thread;

std::mutex g_actionsLock;
std::deque<Action> g_actions;

std::vector<std::wstring> g_lastLines;
int g_reviewIndex = -1;
std::wstring g_lastKey;
std::wstring g_pendingKey;
std::wstring g_pendingGridKey;
Clock::time_point g_pendingGridAt;
std::wstring g_lastGridKey;
std::wstring g_pendingStageKey;
Clock::time_point g_pendingStageAt;
std::wstring g_lastStageKey;
Clock::time_point g_stageSelectSeen = Clock::time_point::min();
bool g_stageMoved = false;
float g_lastControllerX = -1.0f;
float g_lastControllerY = -1.0f;
bool g_storyGrid = false;
Clock::time_point g_storyGridAt = Clock::time_point::min();
std::wstring g_storyCardName;
std::vector<std::wstring> g_storyCardLines;
std::set<std::wstring> g_storyPageSlots;
std::set<std::wstring> g_storyAwaySlots;
float g_storyCardX = 0.0f;
float g_storyCardY = 0.0f;
bool g_storyCardAway = false;
bool g_storyCardFallbackDone = false;
Clock::time_point g_storyCardLostAt = Clock::time_point::min();
Clock::time_point g_pendingAt;
std::wstring g_lastFooter;
std::set<std::wstring> g_previousShown;
std::vector<std::wstring> g_pendingTexts;
Clock::time_point g_pendingTextsAt;
Clock::time_point g_lastSpokenAt;
std::wstring g_lastQuestion;
std::wstring g_lastPanelContext;
Clock::time_point g_lastChosenAt = Clock::now();
size_t g_shownCount = 0;
bool g_wasFocused = false;

std::wstring trim(const std::wstring& text) {
    const size_t first = text.find_first_not_of(L" \t\r\n\u00A0");
    if (first == std::wstring::npos) return {};
    const size_t last = text.find_last_not_of(L" \t\r\n\u00A0");
    return text.substr(first, last - first + 1);
}

// What a screen reader should hear for one on-screen line: paragraphs joined,
// runs of whitespace collapsed, no newlines mid-sentence.
std::wstring spoken(const std::wstring& text) {
    std::wstring out;
    bool space = false;
    for (wchar_t c : text) {
        if (c == L'\r' || c == L'\n' || c == L'\t' || c == L' ' || c == L'\u00A0') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out.push_back(L' ');
        space = false;
        out.push_back(c);
    }
    return trim(out);
}

// Arcade result screens put the loading counter before the tip quote, either
// as its own field or attached to it. Remove the counter prefix before speech
// can join it to the quote; keep the quote that follows it.
std::wstring stripStageOverlay(std::wstring text) {
    const std::wstring t = trim(text);
    if (t.rfind(L"GAME OVER", 0) != 0) return text;
    size_t i = t.find(L"TIP");
    if (i == std::wstring::npos) return text;
    i += 3;
    if (i < t.size() && t[i] == L'S') ++i;
    bool digits = false;
    while (i < t.size() && (iswdigit(t[i]) || t[i] == L' ')) {
        if (iswdigit(t[i])) digits = true;
        ++i;
    }
    if (!digits) return text;
    while (i < t.size() && (t[i] == L'.' || t[i] == L' ')) ++i;
    return t.substr(i);
}

std::wstring stripTrailingPeriods(std::wstring text) {
    while (!text.empty() && (text.back() == L'.' || text.back() == L' ')) text.pop_back();
    return text;
}

std::wstring join(const std::vector<std::wstring>& lines) {
    std::wstring out;
    for (const auto& line : lines) {
        if (line.empty()) continue;
        if (!out.empty()) {
            out += (out.back() == L'.' || out.back() == L'!' || out.back() == L'?') ? L" " : L". ";
        }
        out += line;
    }
    return out;
}

std::wstring keyFor(const Field& field) {
    return field.text + L"@" + std::to_wstring(static_cast<int>(field.x)) + L"," +
           std::to_wstring(static_cast<int>(field.y));
}

bool stageConditionLabel(const std::wstring& text, std::wstring& label) {
    const size_t bar = text.find(L'|');
    if (bar == std::wstring::npos) return false;
    label = trim(text.substr(0, bar));
    // Backing out of character select can leave the panel hidden with its
    // values not filled in yet. The label is still the screen's marker.
    return label == L"Weather" || label == L"Time" || label == L"Temperature";
}

// Steps from one object up to the nearest ancestor it shares with the other.
// Stage select draws the stage name twice, the favourite on the left and the
// one under the cursor beside the conditions. Tree distance picks the latter
// without relying on a fixed position.
int treeDistance(const std::vector<uintptr_t>& a, const std::vector<uintptr_t>& b) {
    int best = std::numeric_limits<int>::max();
    for (size_t i = 0; i < a.size(); ++i) {
        for (size_t j = 0; j < b.size(); ++j) {
            if (a[i] != b[j]) continue;
            best = std::min(best, static_cast<int>(i + j));
        }
    }
    return best;
}

bool stageNameSlot(const Field& field) {
    // The stage name is drawn once as the large right-hand title and once at
    // the left edge of the preview. Other ??? text is not the random stage.
    return (field.x >= 1000.0f && field.x < 1400.0f && field.y >= 540.0f &&
            field.y < 620.0f) ||
           (field.x < 100.0f && field.y >= 480.0f && field.y < 540.0f);
}

bool randomStageName(const Field& field) {
    return stageNameSlot(field) && trim(field.text) == L"???";
}

std::wstring stageNameLabel(const std::wstring& text) {
    return trim(text) == L"???" ? L"Random stage" : text;
}

bool readingOrder(const Field& a, const Field& b) {
    const long ay = std::lround(a.y / 12.0f);
    const long by = std::lround(b.y / 12.0f);
    if (ay != by) return ay < by;
    return a.x < b.x;
}

bool isCharacterSelect(const std::vector<Field>& shown);

std::vector<Field> onScreen(const std::vector<Field>& fields) {
    std::vector<Field> shown;
    std::set<std::wstring> seen;
    auto add = [&](const Field& field) {
        if (!field.shown()) return;
        // A title drawn in three layers reads once.
        if (!seen.insert(keyFor(field)).second) return;
        shown.push_back(field);
    };
    for (const auto& field : fields) add(field);

    // Stage select and character select are panels of one movie. Backing out
    // of character select can leave stage select on screen while its
    // container still has the visible bit clear, so every field above is
    // filtered out and the narrator goes silent. Restore only the stage panel:
    // the fields hidden by the same containers as its weather/time conditions,
    // and no others. Conditions may still have empty values while hidden, so
    // the label alone identifies them.
    std::set<std::wstring> hiddenConditionLabels;
    std::set<uintptr_t> stagePanels;
    for (const auto& field : fields) {
        if (!field.hidden || !field.rooted || field.chain.size() < 2) continue;
        std::wstring label;
        if (!stageConditionLabel(field.text, label)) continue;
        hiddenConditionLabels.insert(label);
        for (uintptr_t parent : field.hiddenBy) stagePanels.insert(parent);
    }
    // A hidden STAGE SELECT heading or random-stage title is the fallback when
    // the conditions are not among the readable fields at all.
    if (hiddenConditionLabels.size() < 2) {
        for (const auto& field : fields) {
            if (!field.hidden || !field.rooted || field.chain.size() < 2) continue;
            if (trim(field.text) != L"STAGE SELECT" && !randomStageName(field)) continue;
            for (uintptr_t parent : field.hiddenBy) stagePanels.insert(parent);
        }
    }
    if (!stagePanels.empty()) {
        const bool characterVisible = isCharacterSelect(shown);
        bool stageVisible = false;
        bool opaqueShown = false;
        bool characterRemnant = false;
        for (const auto& field : shown) {
            const std::wstring text = trim(field.text);
            if (field.tint[3] > 0.01f) opaqueShown = true;
            if (text == L"STAGE SELECT") stageVisible = true;
            if (randomStageName(field)) stageVisible = true;
            std::wstring label;
            if (stageConditionLabel(field.text, label)) stageVisible = true;
            // These roster labels are left behind, faded, when character
            // select closes. They identify the return to stage select even
            // when a fighter name is the only other text on screen.
            if (text == L"2nd" || text == L"LEADER" || text == L"YOU") {
                characterRemnant = true;
            }
        }
        // Character select leaves fighter names and labels visible with zero
        // alpha after the stage panel comes back. They are not stage content,
        // so they do not block restoring the hidden panel; an actual character
        // select (heading or side badge) still does.
        if (!characterVisible &&
            (shown.empty() || stageVisible || !opaqueShown || characterRemnant)) {
            for (const auto& field : fields) {
                if (!field.hidden || !field.rooted || field.hiddenBy.empty()) continue;
                bool inStagePanel = false;
                for (uintptr_t object : field.chain) {
                    if (stagePanels.count(object)) {
                        inStagePanel = true;
                        break;
                    }
                }
                if (!inStagePanel) continue;
                bool onlyStageHidden = true;
                for (uintptr_t parent : field.hiddenBy) {
                    if (!stagePanels.count(parent)) {
                        onlyStageHidden = false;
                        break;
                    }
                }
                if (!onlyStageHidden) continue;
                Field restored = field;
                restored.hidden = false;
                restored.hiddenBy.clear();
                add(restored);
            }
        }
    }
    std::sort(shown.begin(), shown.end(), readingOrder);
    return shown;
}

// Some screens draw their visible text with an effective alpha of zero. Once a
// screen also has a normal, opaque selection, any zero-alpha text left behind
// belongs to the screen it came from: the Arcade path cards were still being
// read as a prefix on the battle settings screen. On a screen with no opaque
// selection of its own (path select, character select), the zero-alpha text is
// the screen.
void dropFaded(std::vector<Field>& shown) {
    bool opaqueSelection = false;
    for (const auto& field : shown) {
        if (field.tint[3] <= 0.01f) continue;
        if (field.gold() || field.chosen) {
            opaqueSelection = true;
            break;
        }
    }
    if (!opaqueSelection) return;
    shown.erase(std::remove_if(shown.begin(), shown.end(),
                              [](const Field& field) { return field.tint[3] <= 0.01f; }),
                shown.end());
}

void say(const std::vector<std::wstring>& lines, bool interrupt) {
    std::vector<std::wstring> cleaned;
    for (const auto& line : lines) {
    std::wstring text = stripStageOverlay(spoken(line));
        if (!text.empty()) cleaned.push_back(text);
    }
    if (cleaned.empty()) return;
    g_lastLines = cleaned;
    g_reviewIndex = -1;
    g_lastSpokenAt = Clock::now();
    const std::wstring utterance = join(cleaned);
    speech::speak(utterance, interrupt);
    logLine(L"say: " + utterance);
}

// A dialog that redraws its buttons, or a screen that rebuilds a field, must
// not repeat something that was just said.
bool recentlySaid(const std::vector<std::wstring>& lines) {
    if (g_lastLines.empty()) return false;
    if (Clock::now() - g_lastSpokenAt > std::chrono::seconds(4)) return false;
    for (const auto& line : lines) {
        const std::wstring text = spoken(line);
        if (std::find(g_lastLines.begin(), g_lastLines.end(), text) == g_lastLines.end()) {
            return false;
        }
    }
    return true;
}

// A prompt whose buttons arrive one at a time can say "Yes" and then "No. Yes"
// as the second button appears. Adding a short label to something just said is
// a redraw, not a new screen.
bool recentlyCovered(const std::vector<std::wstring>& lines) {
    if (g_lastLines.empty()) return false;
    if (Clock::now() - g_lastSpokenAt > std::chrono::seconds(12)) return false;
    std::vector<std::wstring> cleaned;
    for (const auto& line : lines) cleaned.push_back(spoken(line));
    for (const auto& previous : g_lastLines) {
        if (std::find(cleaned.begin(), cleaned.end(), previous) == cleaned.end()) return false;
    }
    for (const auto& line : cleaned) {
        if (line.size() > 24 && std::find(g_lastLines.begin(), g_lastLines.end(), line) ==
                                   g_lastLines.end()) {
            return false;
        }
    }
    return true;
}

bool worthAnnouncing(const std::wstring& text);

// Character select's roster is pictures, so its fighter name is new text
// rather than a selection. That path normally waits half a second for a
// prompt's buttons to finish painting; on this screen the wait is what made
// the name arrive a beat after the cursor. It is not a prompt, so it can use
// the normal announcement delay.
bool isCharacterSelectHeading(const std::wstring& text) {
    return trim(text) == L"CHARACTER SELECT";
}

bool isPlayerBadge(const std::wstring& text) {
    const std::wstring t = trim(text);
    return t == L"1P" || t == L"2P";
}

bool isCpuBadge(const std::wstring& text) {
    return trim(text) == L"CPU";
}

bool isSideBadge(const std::wstring& text) {
    return isPlayerBadge(text) || isCpuBadge(text);
}

bool isCharacterSelect(const std::vector<Field>& shown) {
    for (const auto& field : shown) {
        if (isCharacterSelectHeading(field.text)) return true;
        // The heading is not always in the readable set while the roster is
        // repainting. The side badges belong to this screen alone, and unlike
        // the heading they stay on screen throughout it.
        if (field.y >= 700.0f && field.y < 1000.0f && isSideBadge(field.text)) {
            return true;
        }
    }
    return false;
}

// After player one is chosen, Training opens the same character-select movie
// for the opponent. The CPU badge is dimmed on the P1 screen and bright once
// the opponent is choosing. Its position moves with the roster row, so neither
// its x nor its y can identify the screen; the active tint does.
bool isOpponentSelect(const std::vector<Field>& shown) {
    for (const auto& field : shown) {
        if (!isCpuBadge(field.text)) continue;
        if (field.tint[0] >= 0.9f && field.tint[1] >= 0.9f && field.tint[2] >= 0.9f) {
            return true;
        }
    }
    return false;
}

// Player one's fighter name is drawn on the left of the character-select
// stage and the CPU's on the right. The CPU changes its pick on its own, and
// backing out can leave that side's name on screen; it is not the character
// the player is choosing. The CPU name sits at about x=932, so the old x=960
// boundary let it through. This filter must not run on opponent select, where
// the CPU-side name is exactly the choice being made.
bool characterSelectCpuName(const Field& field) {
    return field.x >= 700.0f && !FightReader::knownName(field.text).empty();
}

// The random tile is labelled "???" in a side panel. It has no letters, so the
// ordinary worthAnnouncing filter drops it; recognise it here before that
// filter runs. On P1 select only the left panel is the player's choice; on
// opponent select the right panel is.
bool isCharacterSelectRandom(const Field& field, bool opponentSelect) {
    return trim(field.text) == L"???" && (field.x < 700.0f || opponentSelect);
}

// The random-select tile can also reach the reader as a bare player badge
// while its "???" name is still painting. When that badge is the whole
// selection, name the slot; when a fighter name is present, the badge is only
// grid decoration. The CPU tag is always decoration: the fighter name is the
// selection. Returns true when the result is only the random tile, so the
// caller can wait for a fighter name that is still painting before saying it.
bool characterSelectLabels(std::vector<std::wstring>& lines) {
    if (lines.empty()) return false;
    bool onlyBadges = true;
    bool anyPlayerBadge = false;
    bool anyRandom = false;
    for (const auto& line : lines) {
        const std::wstring t = trim(line);
        if (isCharacterSelectHeading(t) || isCpuBadge(t)) {
            // The heading and CPU tag accompany ordinary roster navigation;
            // they must not turn a player badge into a random-tile call.
            onlyBadges = false;
            continue;
        }
        if (t == L"???") {
            anyRandom = true;
            onlyBadges = false;
            continue;
        }
        if (isPlayerBadge(t)) {
            anyPlayerBadge = true;
            continue;
        }
        onlyBadges = false;
    }
    lines.erase(std::remove_if(lines.begin(), lines.end(), [](const std::wstring& line) {
                     const std::wstring t = trim(line);
                     return isCharacterSelectHeading(t) || isSideBadge(t) || t == L"???";
                 }),
                 lines.end());
    if (anyRandom) {
        if (std::find(lines.begin(), lines.end(), L"Random select") == lines.end()) {
            lines.push_back(L"Random select");
        }
        return false;
    }
    if (onlyBadges && anyPlayerBadge) {
        lines.push_back(L"Random select");
        return true;
    }
    return false;
}

// The main menu's bottom row of icons is text too, and it is not gold either.
// Its labels are short and stable, so it does not need a prompt's settle time.
bool isMainMenu(const std::vector<Field>& shown) {
    // Only one of the rail labels is drawn at a time, so look for one of them
    // plus the static cards behind the rail. Requiring all the labels at once
    // never matched, which is why the icon row kept the prompt settle.
    bool rail = false;
    bool arcade = false;
    bool story = false;
    bool shop = false;
    bool battleSettings = false;
    for (const auto& field : shown) {
        const std::wstring text = spoken(field.text);
        auto starts = [&](const wchar_t* label) { return text.rfind(label, 0) == 0; };
        if (starts(L"MESSAGE LOG") || starts(L"OPTIONS") || starts(L"GALLERY") ||
            starts(L"LOGIN") || starts(L"EXIT")) {
            rail = true;
        }
        if (starts(L"ARCADE")) arcade = true;
        if (starts(L"STORY")) story = true;
        if (starts(L"SHOP")) shop = true;
        if (starts(L"BATTLE SETTINGS")) battleSettings = true;
    }
    return (arcade && story) || (rail && (shop || battleSettings));
}

// The Character Story screen keeps its left-hand tab gold while the cursor is
// in the character grid. That makes the tab look like a selection when the
// grid is between cards, which the narrator read as a lone "Character Story".
bool isStoryTab(const Field& field) {
    return field.x < 400.0f && field.y >= 200.0f && field.y < 300.0f &&
           trim(field.text) == L"Character Story";
}

// The character cards fill the right-hand grid; the sidebar tab and the
// bottom description are outside it.
bool isStoryCardSlot(const Field& field) {
    return field.x >= 500.0f && field.x < 1800.0f && field.y >= 150.0f && field.y < 950.0f;
}

bool isCharacterStoryTab(const std::vector<Field>& shown) {
    for (const auto& field : shown) {
        if (isStoryTab(field) && field.gold()) return true;
    }
    return false;
}

bool hasCharacterStoryCard(const std::vector<Field>& shown) {
    for (const auto& field : shown) {
        if (!isStoryCardSlot(field)) continue;
        if (!FightReader::knownName(field.text).empty()) return true;
    }
    return false;
}

std::wstring storyCardName(const Field& field) {
    if (!isStoryCardSlot(field)) return {};
    return FightReader::knownName(field.text);
}

bool storyCardVisible(const std::vector<Field>& shown, const std::wstring& name) {
    if (name.empty()) return false;
    for (const auto& field : shown) {
        if (storyCardName(field) == name) return true;
    }
    return false;
}

std::set<std::wstring> storyPageSlots(const std::vector<Field>& shown) {
    std::set<std::wstring> slots;
    for (const auto& field : shown) {
        const std::wstring name = storyCardName(field);
        if (name.empty()) continue;
        slots.insert(name + L"@" + std::to_wstring(static_cast<int>(field.x)) + L"," +
                    std::to_wstring(static_cast<int>(field.y)));
    }
    return slots;
}

const Field* storyCardAtSlot(const std::vector<Field>& shown, float x, float y) {
    for (const auto& field : shown) {
        if (!storyCardName(field).empty() && field.x >= x - 4.0f && field.x <= x + 4.0f &&
            field.y >= y - 4.0f && field.y <= y + 4.0f) {
            return &field;
        }
    }
    return nullptr;
}

std::vector<std::wstring> storyCardLinesAt(const std::vector<Field>& shown,
                                           const Field& slot) {
    std::vector<const Field*> parts;
    for (const auto& field : shown) {
        if (field.x < slot.x - 8.0f || field.x > slot.x + 420.0f) continue;
        if (field.y < slot.y - 8.0f || field.y > slot.y + 80.0f) continue;
        if (trim(field.text).empty()) continue;
        parts.push_back(&field);
    }
    std::sort(parts.begin(), parts.end(), [](const Field* a, const Field* b) {
        if (a->y != b->y) return a->y < b->y;
        return a->x < b->x;
    });
    std::vector<std::wstring> lines;
    std::set<std::wstring> seen;
    for (const Field* field : parts) {
        if (seen.insert(field->text).second) lines.push_back(field->text);
    }
    return lines;
}

// The command list is a full panel with tabs across the top: a character
// name, the tab, the moves, and a footer. It is not a normal menu: the tab is
// drawn with a gold border image, so no text on the panel is gold. Detect it
// by that tab and footer rather than by the pause row that opened it.
bool isCommandTab(const std::wstring& text) {
    static const wchar_t* kTabs[] = {L"Special Move",  L"Special Moves", L"Unique Attacks",
                                     L"Unique Attack", L"V-Skill",       L"V-Trigger",
                                     L"Critical Art",  L"Command List"};
    for (const wchar_t* tab : kTabs) {
        if (text == tab) return true;
    }
    return false;
}

struct CommandPanel {
    std::wstring name;
    std::wstring tab;
    std::wstring footer;
    std::wstring key;
    std::vector<std::wstring> row;  // only the move row carrying the cursor
};

// The command list always has its tab strip above the moves. The tab is drawn
// with a border image rather than gold text, so it is the one reliable mark of
// this screen. The pause menu's own "Command List" row sits in the left
// column and is not mistaken for one.
bool isCommandScreen(const std::vector<Field>& shown) {
    for (const auto& field : shown) {
        if (field.tint[3] < 0.5f) continue;
        if (field.y >= 180.0f && field.y < 300.0f && field.x >= 600.0f && field.x < 1350.0f &&
            isCommandTab(field.text)) {
            return true;
        }
    }
    return false;
}

// Training's Controller Setting lists its actions down one column, each with
// the assigned button drawn as a picture. Both Training and Versus leave the
// menu that opened the overlay visible behind it, and that menu keeps its
// Controller Settings row gold. Reading every gold field interleaved that row
// with the action rows: "Light Punch. Controller Settings" and then
// "Controller Settings. Light Kick" where the row's y fell between them.
// Identify the overlay by its Controller Type heading and its action rows.
bool isControllerRow(const std::wstring& text) {
    static const wchar_t* kRows[] = {
        L"Light Punch", L"Medium Punch", L"Hard Punch", L"Light Kick", L"Medium Kick",
        L"Hard Kick",   L"Throw",        L"V-Skill",     L"V-Trigger", L"V-Shift",
        L"Button Combo 1", L"Button Combo 2", L"Button Combo 3", L"Button Combo 4",
    };
    for (const wchar_t* row : kRows) {
        if (text == row) return true;
    }
    return false;
}

bool isControllerSetting(const std::vector<Field>& shown) {
    bool heading = false;
    int rows = 0;
    for (const auto& field : shown) {
        if (trim(field.text) == L"Controller Type") heading = true;
        if (isControllerRow(trim(field.text))) ++rows;
    }
    return heading && rows >= 8;
}

// A greyed-out menu entry is drawn at 60% brightness instead of the bright
// white of a normal row. It cannot be selected, so the game can jump straight
// past it; remember the rows between two selections and report them as
// unavailable rather than silently dropping them.
bool isUnavailableRow(const Field& field) {
    const float level = field.tint[0];
    return !field.gold() && field.tint[3] > 0.5f && level >= 0.55f &&
           level <= 0.70f && std::fabs(field.tint[1] - level) < 0.02f &&
           std::fabs(field.tint[2] - level) < 0.02f;
}

// Collects the command panel if this screen is one. The active tab and the
// move row under the cursor are read; the pause menu behind the overlay is
// outside these columns.
bool commandPanelLines(const std::vector<Field>& shown, bool commandScreen, CommandPanel& panel) {
    panel = CommandPanel{};
    const Field* name = nullptr;
    const Field* tab = nullptr;
    const Field* footer = nullptr;
    std::vector<const Field*> body;
    for (const auto& field : shown) {
        if (field.tint[3] < 0.5f) continue;
        if (!footer && field.y >= 740.0f && field.x >= 250.0f && field.x < 1100.0f &&
            field.text.find(L"commands assume") != std::wstring::npos) {
            footer = &field;
            continue;
        }
        if (!tab && field.y >= 180.0f && field.y < 300.0f && field.x >= 600.0f &&
            field.x < 1350.0f && isCommandTab(field.text)) {
            tab = &field;
            continue;
        }
        if (!name && field.x >= 240.0f && field.x < 430.0f && field.y >= 200.0f &&
            field.y < 300.0f && !FightReader::knownName(field.text).empty()) {
            name = &field;
            continue;
        }
        if (field.x >= 430.0f && field.x < 1000.0f && field.y >= 260.0f &&
            field.y < 740.0f && worthAnnouncing(field.text)) {
            body.push_back(&field);
        }
    }
    // The footer is fixed at the bottom of every tab. Without it, require a
    // known tab and several moves; a tab label alone is not a panel.
    if (body.empty()) return false;
    if (!footer && !commandScreen) return false;
    if (!footer && !tab && body.size() < 3) return false;

    if (name) {
        const std::wstring canonical = FightReader::knownName(name->text);
        panel.name = canonical.empty() ? name->text : canonical;
    }
    if (tab) panel.tab = tab->text;
    if (footer) panel.footer = footer->text;

    // markCommandRow found the one row with the cursor's part switched on.
    // If that rule has not landed yet, wait for the row rather than pouring
    // the whole list out at once.
    std::vector<const Field*> rowFields;
    uintptr_t row = 0;
    for (const Field* field : body) {
        if (!field->rowSelected) continue;
        if (!row) row = field->row;
        if (field->row == row) rowFields.push_back(field);
    }
    if (rowFields.empty()) return false;

    // The game sometimes draws the strength letter as its own field before
    // the coloured button. The button already says "medium", so drop it.
    for (size_t i = 0; i + 1 < rowFields.size();) {
        const std::wstring& line = rowFields[i]->text;
        const std::wstring& next = rowFields[i + 1]->text;
        const bool drop = (line == L"M" && next.rfind(L"medium ", 0) == 0) ||
                          (line == L"H" && next.rfind(L"heavy ", 0) == 0) ||
                          (line == L"L" && next.rfind(L"light ", 0) == 0);
        if (drop) {
            rowFields.erase(rowFields.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }

    panel.key = L"panel\x1f" + panel.name + L"\x1f" + panel.tab + L"\x1f";
    for (const Field* field : rowFields) {
        panel.row.push_back(field->text);
        panel.key += field->text + L"@" + std::to_wstring(static_cast<int>(field->y)) + L"\x1f";
    }
    for (const auto& badge : g_reader.commandBadges()) {
        if (std::find(panel.row.begin(), panel.row.end(), badge) != panel.row.end()) continue;
        panel.row.push_back(badge);
        panel.key += badge + L"\x1f";
    }
    return true;
}

// The two fighter names the fight display draws above the health bars. The
// left one is the player's side; SFV mirrors the display so it stays left.
std::pair<std::wstring, std::wstring> hudNames(const std::vector<Field>& shown) {
    std::vector<std::pair<float, std::wstring>> found;
    for (const auto& field : shown) {
        if (field.y > 300.0f) continue;
        const std::wstring name = FightReader::knownName(field.text);
        if (!name.empty()) found.push_back({field.x, name});
    }
    if (found.size() < 2) return {};
    std::sort(found.begin(), found.end());
    return {found.front().second, found.back().second};
}

// The round clock is the only one or two digit text in the top middle.
int hudTimer(const std::vector<Field>& shown) {
    for (const auto& field : shown) {
        if (field.y > 170.0f || field.x < 700.0f || field.x > 1220.0f) continue;
        if (field.text.size() < 1 || field.text.size() > 2) continue;
        bool digits = true;
        for (wchar_t c : field.text) {
            if (c < L'0' || c > L'9') {
                digits = false;
                break;
            }
        }
        if (!digits) continue;
        return (field.text[0] - L'0') * 10 + (field.text.size() > 1 ? field.text[1] - L'0' : 0);
    }
    return -1;
}

// The labels over the two health bars. They are present in the final stage's
// fight even when no round clock text is (the clock is art there), so they are
// what keeps the radar and the fight reader awake when hudTimer finds nothing.
bool fightLabels(const std::vector<Field>& shown) {
    bool player = false;
    bool opponent = false;
    for (const auto& field : shown) {
        if (field.y > 170.0f) continue;
        const std::wstring text = trim(field.text);
        if (text == L"PLAYER 1" || text == L"PLAYER 2" || text == L"YOU") player = true;
        if (text == L"CPU" || text == L"PLAYER 2" || text == L"OPPONENT" || text == L"RIVAL") {
            opponent = true;
        }
    }
    return player && opponent;
}

// Arcade's and Survival's tips screens put the heading in one holder and the
// tip itself under the same holder, but the tip is not a selected row. The
// generic "new text" rule therefore spoke the heading and the "TIP" label and
// left the tip unread. Matching them by their container reads the whole thing.
bool tipsHeading(const std::wstring& text) {
    const std::wstring t = trim(text);
    return t == L"ARCADE MODE TIPS" || t == L"SURVIVAL MODE TIPS" ||
           t == L"EXTRA BATTLE TIPS" || t == L"TIPS FOR TRIALS" ||
           t == L"DEMONSTRATION TIPS";
}

bool tipsLabel(const std::wstring& text) {
    std::wstring t = trim(text);
    for (wchar_t& c : t) {
        if (c == L'\u00A0') c = L' ';
    }
    if (t == L"TIP" || t == L"TIPS") return true;
    if (t.rfind(L"TIP ", 0) != 0 && t.rfind(L"TIPS ", 0) != 0) return false;
    for (size_t i = t.find(L' ') + 1; i < t.size(); ++i) {
        if (!iswdigit(t[i]) && t[i] != L' ') return false;
    }
    return true;
}

// A numbered TIP/TIPS label is a progress counter left behind by the loading
// or result overlay, not a tip. Leave a bare TIP/TIPS alone because that can
// be a real menu label.
bool tipCounter(const std::wstring& text) {
    const std::wstring t = trim(text);
    if (!tipsLabel(t)) return false;
    for (wchar_t c : t) {
        if (iswdigit(c)) return true;
    }
    return false;
}

bool handleTipsScreen(const std::vector<Field>& shown, Clock::time_point now) {
    const Field* heading = nullptr;
    for (const auto& field : shown) {
        if (tipsHeading(field.text)) {
            heading = &field;
            break;
        }
    }
    if (!heading) return false;

    const uintptr_t holder = heading->chain.size() >= 2 ? heading->chain[1] : 0;
    const Field* tip = nullptr;
    for (const auto& field : shown) {
        if (&field == heading || tipsLabel(field.text) || trim(field.text).empty()) continue;
        if (holder && std::find(field.chain.begin(), field.chain.end(), holder) ==
                          field.chain.end()) {
            continue;
        }
        if (!tip || field.text.size() > tip->text.size()) tip = &field;
    }

    if (!tip) {
        // The tip arrives a fraction of a second after its heading. Wait for
        // it rather than announcing the label alone; a real heading with no
        // body still gets said after two seconds.
        const std::wstring key = L"tips\x1f" + heading->text;
        if (g_pendingKey != key) {
            g_pendingKey = key;
            g_pendingAt = now + std::chrono::seconds(2);
            return true;
        }
        if (now < g_pendingAt || key == g_lastKey) return true;
        say({heading->text}, true);
        g_lastKey = key;
        return true;
    }

    const std::wstring key = L"tips\x1f" + heading->text + L"\x1f" + tip->text;
    if (key != g_pendingKey) {
        g_pendingKey = key;
        g_pendingAt = now + std::chrono::milliseconds(
                                 g_config.announceDelayMs < 500 ? 500 : g_config.announceDelayMs);
        return true;
    }
    if (now < g_pendingAt || key == g_lastKey) return true;
    say({heading->text, tip->text}, true);
    g_pendingTexts.clear();
    g_lastKey = key;
    return true;
}

// The final-stage loading overlay leaves a hidden progress label with the
// game-over tips count in the field list. It is not screen content, but the
// generic new-text path otherwise reads "GAME OVER. TIPS 1" during the boss
// entrance. Keep a real standalone GAME OVER message read; only the combined
// overlay is noise.
bool hiddenStageOverlay(const std::wstring& text) {
    const std::wstring t = trim(text);
    return t.rfind(L"GAME OVER", 0) == 0 && t.find(L"TIP") != std::wstring::npos;
}

// The login and title screens draw a legal copyright line alongside the
// server status. It has no navigation value and was being collected with the
// status text, so skip it without suppressing the status or its buttons.
bool copyrightNotice(const std::wstring& text) {
    const std::wstring t = trim(text);
    return t.find(L"CAPCOM CO., LTD.") != std::wstring::npos &&
           t.find(L"ALL RIGHTS RESERVED") != std::wstring::npos;
}

// HUD counters, damage totals and lone symbols are not menu text. Reading
// every new two- or three-digit field made a fight announce "500", "600" and
// "70" over the action. A line with no letters and only a few characters is
// an instrument, not an announcement.
bool worthAnnouncing(const std::wstring& text) {
    if (hiddenStageOverlay(text) || copyrightNotice(text) || tipCounter(text)) return false;
    bool allDigits = true;
    for (wchar_t c : text) {
        if (iswalpha(c)) return true;
        if (!iswdigit(c)) allDigits = false;
    }
    // A long score is still worth saying. A clock such as 9:99:99.99 or a
    // symbol such as the infinity marker is not.
    return allDigits && text.size() > 6;
}

// Arcade path select keeps the selected path's lore in one wide field on the
// right, outside the gold card. The field is not selected, so the normal
// selection path never reads it.
bool isArcadePathScreen(const std::vector<Field>& shown) {
    bool path = false;
    bool battles = false;
    bool prompt = false;
    for (const auto& field : shown) {
        const std::wstring text = trim(field.text);
        if (text.rfind(L"STREET FIGHTER", 0) == 0) path = true;
        if (text.rfind(L"NO. OF BATTLES", 0) == 0) battles = true;
        if (text.rfind(L"Please select a path", 0) == 0) prompt = true;
    }
    return path && battles && prompt;
}

bool isArcadePathBlurb(const Field& field) {
    if (field.x < 1000.0f || field.y < 300.0f || field.y >= 950.0f) return false;
    const std::wstring text = trim(field.text);
    return text.size() >= 40 && worthAnnouncing(text);
}

std::vector<std::wstring> arcadePathBlurbs(const std::vector<Field>& shown) {
    std::vector<const Field*> parts;
    for (const auto& field : shown) {
        if (isArcadePathBlurb(field)) parts.push_back(&field);
    }
    std::sort(parts.begin(), parts.end(), [](const Field* a, const Field* b) {
        if (a->y != b->y) return a->y < b->y;
        return a->x < b->x;
    });
    std::vector<std::wstring> lines;
    std::set<std::wstring> seen;
    for (const Field* field : parts) {
        if (seen.insert(field->text).second) lines.push_back(field->text);
    }
    return lines;
}

bool isBattleSettingItem(const std::wstring& text) {
    static const wchar_t* kItems[] = {
        L"Favorite Character", L"Favorite Stage", L"Random Stage Settings",
        L"Battle BGM Settings", L"Title",
    };
    for (const wchar_t* item : kItems) {
        if (text == item) return true;
    }
    return false;
}

bool isBattleGridHeader(const std::wstring& text) {
    return text == L"ALL" || text.find(L"Additional Stages") != std::wstring::npos;
}

// The preview panel beside these grids repeats the same labels on every
// screen. They are not grid entries and can sit close enough to a row to be
// mistaken for the bright one.
bool isStatusLabel(const std::wstring& text) {
    static const wchar_t* kLabels[] = {
        L"V-Skill", L"V-TRIGGER", L"V-Comment", L"Costume", L"Color", L"Character",
        L"Stage",   L"Health",    L"Range",     L"Power",   L"Techniques", L"Mobility",
    };
    for (const wchar_t* label : kLabels) {
        if (text == label) return true;
    }
    return false;
}

// Battle Settings' submenus are grids of pictures. Their labels are not gold,
// so the selection path only saw the left-hand setting name and said nothing
// when the cursor moved. The grid draws every label at 75% brightness except
// the cursor's, which is full brightness. Reading the one bright label in a
// row of dim labels gives the cursor's item without dumping the whole grid.
bool battleSettingsGrid(const std::vector<Field>& shown, std::wstring& line) {
    line.clear();
    // The Command List has rows of labels too; its own reader handles it.
    for (const auto& field : shown) {
        if (field.text.find(L"commands assume") != std::wstring::npos) return false;
    }
    bool goldSetting = false;
    bool header = false;
    for (const auto& field : shown) {
        const std::wstring text = trim(field.text);
        if (field.gold() && field.x < 700.0f && isBattleSettingItem(text)) goldSetting = true;
        if (field.y >= 210.0f && field.y < 270.0f && field.x >= 950.0f &&
            isBattleGridHeader(text)) {
            header = true;
        }
    }

    // A grid row has at least three short labels at the same height, spread
    // across the screen. Exactly one is bright; the rest are dimmed.
    for (const auto& field : shown) {
        if (field.text.size() > 40) continue;
        if (isStatusLabel(trim(field.text))) continue;
        if (!worthAnnouncing(field.text)) continue;
        std::vector<const Field*> row;
        for (const auto& other : shown) {
            if (other.text.size() > 40) continue;
            if (isStatusLabel(trim(other.text))) continue;
            if (!worthAnnouncing(other.text)) continue;
            if (std::fabs(other.y - field.y) > 1.5f) continue;
            row.push_back(&other);
        }
        if (row.size() < 3) continue;
        std::sort(row.begin(), row.end(),
                  [](const Field* a, const Field* b) { return a->x < b->x; });
        int spread = 1;
        for (size_t i = 1; i < row.size(); ++i) {
            if (row[i]->x - row[i - 1]->x >= 60.0f) ++spread;
        }
        if (spread < 3) continue;

        const Field* bright = nullptr;
        int brightCount = 0;
        int dimCount = 0;
        for (const Field* item : row) {
            const bool isBright =
                item->tint[0] >= 0.95f && item->tint[1] >= 0.95f && item->tint[2] >= 0.95f;
            const bool isDim =
                item->tint[0] <= 0.85f && item->tint[1] <= 0.85f && item->tint[2] <= 0.85f;
            if (isBright) {
                bright = item;
                ++brightCount;
            } else if (isDim) {
                ++dimCount;
            }
        }
        if (brightCount == 1 && dimCount >= 2) {
            line = bright->text;
            return true;
        }
        // Some grids leave every label bright and mark the cursor with a
        // checkmark inside its cell instead.
        if (brightCount == static_cast<int>(row.size())) {
            const Field* chosen = nullptr;
            int count = 0;
            for (const Field* item : row) {
                if (g_reader.gridChosen(*item)) {
                    chosen = item;
                    ++count;
                }
            }
            if (count == 1) {
                line = chosen->text;
                return true;
            }
        }
    }

    // The stage grids have no labels. The stage under the cursor is the
    // rightmost value under the preview's Stage row; the value on the left is
    // the current favourite, which stays put while the cursor moves.
    if (goldSetting && header) {
        const Field* stage = nullptr;
        for (const auto& field : shown) {
            if (trim(field.text) == L"Stage" && field.x >= 950.0f && field.y >= 700.0f &&
                field.y < 800.0f) {
                stage = &field;
                break;
            }
        }
        if (stage) {
            const Field* value = nullptr;
            for (const auto& field : shown) {
                if (field.x < stage->x + 10.0f) continue;
                if (field.y < stage->y + 30.0f || field.y >= stage->y + 80.0f) continue;
                if (field.tint[0] < 0.9f || field.tint[1] < 0.9f || field.tint[2] < 0.9f) continue;
                if (field.text.size() > 60 || !worthAnnouncing(field.text)) continue;
                if (!value || field.x > value->x) value = &field;
            }
            if (value) {
                line = value->text;
                return true;
            }
        }
    }
    return false;
}

bool isStageSelect(const std::vector<Field>& shown) {
    // Two conditions identify the screen even while its heading blinks out
    // during the stage-change transition.
    std::set<std::wstring> labels;
    for (const auto& field : shown) {
        if (field.y < 200.0f && trim(field.text) == L"STAGE SELECT") return true;
        if (randomStageName(field)) return true;
        std::wstring label;
        if (stageConditionLabel(field.text, label)) labels.insert(label);
    }
    return labels.size() >= 2;
}

// Stage select has no gold text and no cursor field. It draws the stage name
// twice, the favourite on the left and the one under the cursor beside the
// conditions; the name nearest the conditions in the display tree is the one
// being offered. Moving says just the name; Read All still gives the details.
bool stageSelectLine(const std::vector<Field>& shown, std::wstring& line) {
    line.clear();
    std::set<std::wstring> labels;
    std::vector<const Field*> conditions;
    for (const auto& field : shown) {
        std::wstring label;
        if (!stageConditionLabel(field.text, label)) continue;
        labels.insert(label);
        conditions.push_back(&field);
    }
    bool random = false;
    for (const auto& field : shown) {
        if (randomStageName(field)) {
            random = true;
            break;
        }
    }
    if (labels.size() < 2) {
        // The random stage has no weather, time or temperature fields.
        if (random) {
            line = L"Random stage";
            return true;
        }
        return false;
    }

    const Field* name = nullptr;
    int nearest = std::numeric_limits<int>::max();
    for (const auto& field : shown) {
        const std::wstring text = trim(field.text);
        if (text.empty() || text == L"STAGE SELECT" || !worthAnnouncing(text)) continue;
        std::wstring ignored;
        if (stageConditionLabel(text, ignored)) continue;
        for (const Field* condition : conditions) {
            const int distance = treeDistance(field.chain, condition->chain);
            if (distance < nearest) {
                nearest = distance;
                name = &field;
            }
        }
    }
    if (!name) return false;
    line = stageNameLabel(trim(name->text));
    return !line.empty();
}

void clearStageSelectState() {
    g_pendingStageKey.clear();
    g_lastStageKey.clear();
    g_stageMoved = false;
    g_stageSelectSeen = Clock::time_point::min();
}

bool handleStageSelect(const std::vector<Field>& shown, Clock::time_point now) {
    if (!isStageSelect(shown)) {
        // Character select is a different screen at once; clear so coming back
        // announces the stage again. Otherwise hold through a one-poll hidden
        // or repainted panel rather than clearing the line and repeating it.
        if (isCharacterSelect(shown)) {
            clearStageSelectState();
            return false;
        }
        if (g_stageSelectSeen != Clock::time_point::min() &&
            now - g_stageSelectSeen < std::chrono::milliseconds(750)) {
            return true;
        }
        clearStageSelectState();
        return false;
    }
    g_stageSelectSeen = now;

    std::wstring line;
    if (!stageSelectLine(shown, line)) {
        // The conditions can be up for a poll before the name paints. Hold the
        // screen rather than falling through to generic text.
        g_pendingStageKey.clear();
        return true;
    }
    const std::wstring key = L"stage\x1f" + line;
    if (key != g_pendingStageKey) {
        if (key != g_lastStageKey) g_stageMoved = true;
        g_pendingStageKey = key;
        g_pendingStageAt = now + std::chrono::milliseconds(g_config.announceDelayMs);
        return true;
    }
    if (now < g_pendingStageAt) return true;
    if (key == g_lastStageKey && !g_stageMoved) return true;

    // Moving away and straight back is a real move even when it lands on the
    // stage that was last spoken, so do not let the repeat guard swallow it.
    const bool returned = key == g_lastStageKey;
    if (!returned && recentlySaid({line})) {
        g_lastStageKey = key;
        g_stageMoved = false;
        return true;
    }
    say({line}, true);
    g_lastStageKey = key;
    g_stageMoved = false;
    g_pendingTexts.clear();
    return true;
}

bool handleBattleGrid(const std::vector<Field>& shown, Clock::time_point now) {
    std::wstring line;
    if (!battleSettingsGrid(shown, line)) return false;
    line = spoken(line);
    if (line.empty()) return false;
    const std::wstring key = L"battleGrid\x1f" + line;
    if (key != g_pendingGridKey) {
        g_pendingGridKey = key;
        g_pendingGridAt = now + std::chrono::milliseconds(g_config.announceDelayMs);
        return true;
    }
    if (now < g_pendingGridAt || key == g_lastGridKey) return true;
    if (!recentlySaid({line})) say({line}, true);
    g_lastGridKey = key;
    g_pendingTexts.clear();
    return true;
}

bool isVersusModeScreen(const std::vector<Field>& shown) {
    bool modeItem = false;
    bool preMatch = false;
    for (const auto& field : shown) {
        const std::wstring text = trim(field.text);
        if (text == L"PLAYER 1 VS PLAYER 2" || text == L"CPU VS PLAYER 1" ||
            text == L"PLAYER 1 VS CPU" ||
            text.find(L"Fight in a 1 vs 1 battle") != std::wstring::npos) {
            modeItem = true;
        }
        if (text == L"Proceed to Match" || text == L"Round Settings" || text == L"Ready" ||
            text.find(L"When Player 1 is ready") != std::wstring::npos) {
            preMatch = true;
        }
    }
    return modeItem && !preMatch;
}

void announceFight() {
    const auto shown = onScreen(g_reader.read());
    const auto [leftName, rightName] = hudNames(shown);
    if (!leftName.empty()) {
        const std::wstring canonical = FightReader::knownName(leftName);
        g_playerName = canonical.empty() ? leftName : canonical;
    }
    if (!g_fight->found() && !g_fight->scan()) {
        g_nextFightScan = Clock::now() + std::chrono::seconds(5);
        say({L"No fight to read."}, true);
        return;
    }
    Gauges left, right;
    bool matched = false;
    if (!g_fight->read(leftName, rightName, left, right, matched)) {
        say({L"No fight to read."}, true);
        return;
    }
    if (g_playerName.empty()) {
        const std::wstring recordCharacter = g_fight->playerCharacter();
        if (!recordCharacter.empty()) g_playerName = recordCharacter;
    }
    // The round clock is drawn as art, not as a text field, so hudTimer picks
    // up unrelated HUD text (a 00, a stage number, the 9:59:59.99 sample).
    // Until a real clock source is found, read the two fighters only.
    say({left.words(matched ? L"You" : L"Player 1"),
         right.words(matched ? L"Opponent" : L"Player 2")},
        true);
}

// The pause overlay leaves the fight HUD, including both health labels, on
// screen, so the live-round check below still looks like a fight. Its rows are
// the reliable marker: Resume, Controller Setting and Command List are pause
// menu entries, and their descriptions arrive as separate fields. Stop the
// radar as soon as one of those rows is visible.
bool isPauseMenu(const std::vector<Field>& shown) {
    static const wchar_t* kRows[] = {
        L"Resume", L"RESUME", L"PAUSE", L"Controller Setting", L"Command List",
        L"Training Settings", L"Sound Settings", L"Button Configuration",
        L"Return to Main Menu",
    };
    for (const auto& field : shown) {
        const std::wstring text = trim(field.text);
        for (const wchar_t* row : kRows) {
            if (text == row) return true;
        }
        if (text.find(L"Change controller settings") != std::wstring::npos ||
            text.find(L"View Character's Commands") != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

// Round starts and round ends, from the clock and the gauges. Silent until a
// fight's clock is on screen, so the menus are unaffected.
void updateFight(const std::vector<Field>& shown) {
    if (isPauseMenu(shown)) {
        radar::update(false);
        return;
    }
    if (isStageSelect(shown)) {
        // Stage select can carry character portraits and a stray number, but
        // it is not a round. Clear any fight state so the menu reader is not
        // suppressed by a clock it mistook for the HUD.
        g_fightClock = -1;
        g_lastTimer = -1;
        g_playerName.clear();
        g_timer99 = false;
        radar::update(false);
        return;
    }
    const int timer = hudTimer(shown);
    g_fightClock = timer;
    // The final stage draws its clock as art, so hudTimer finds no field
    // there. Its health-bar labels do show, and they are enough to tell that
    // a fight is on screen; don't switch the radar off just for a missing
    // clock.
    const bool display = fightLabels(shown);
    if (timer < 0 && !display) {
        g_lastTimer = -1;
        g_playerName.clear();
        g_timer99 = false;
        radar::update(false);
        return;
    }
    const auto now = Clock::now();
    if (timer == 99) {
        if (!g_timer99) {
            g_timer99 = true;
            g_timer99Since = now;
        }
    } else {
        g_timer99 = false;
    }
    if (!g_fight->found()) {
        if (now < g_nextFightScan) {
            radar::update(false);
            return;
        }
        if (!g_fight->scan()) {
            g_nextFightScan = now + std::chrono::seconds(5);
            radar::update(false);
            return;
        }
    }
    const auto [leftName, rightName] = hudNames(shown);
    if (!leftName.empty()) {
        const std::wstring canonical = FightReader::knownName(leftName);
        g_playerName = canonical.empty() ? leftName : canonical;
    }
    Gauges left, right;
    bool matched = false;
    if (!g_fight->read(leftName, rightName, left, right, matched)) {
        radar::update(false);
        return;
    }

    // A live round is anything but the opening countdown. Training leaves the
    // clock at 99, so after four seconds there it counts as live too; a normal
    // countdown reaches 98 well before that. With no clock text, as in the
    // final stage, both health bars still being up means the fight is live.
    const bool fighting = left.health > 0 && right.health > 0;
    const bool roundLive =
        timer > 0
            ? (timer < 99 || (g_timer99 && now - g_timer99Since > std::chrono::seconds(4)))
            : fighting;
    g_fightClock = roundLive ? 1 : timer;
    radar::update(roundLive);
    const std::wstring radarCharacter = radar::playerCharacter();
    if (!radarCharacter.empty()) {
        g_playerName = radarCharacter;
    } else if (g_playerName.empty()) {
        const std::wstring recordCharacter = g_fight->playerCharacter();
        if (!recordCharacter.empty()) g_playerName = recordCharacter;
    }

    // The game's own announcer calls the round and the knockout, and the
    // health bars are visible, so the automatic round summary is redundant.
    // The fight reader hotkey still gives the full health/gauge readout.
    if (g_lastTimer > 0 && timer == 0) {
        say({L"Time."}, true);
    }
    g_lastTimer = timer;
}

void status() {
    std::wstring text = L"SFAccess. " + std::to_wstring(g_shownCount) + L" text fields on screen, " +
                        std::to_wstring(g_reader.fieldCount()) + L" known";
    if (g_reader.scanMillis() > 0) {
        text += L". Sweep " + std::to_wstring(g_reader.scanMillis()) + L" milliseconds";
    }
    text += L". Speech " + speech::backendName() + L". Narration " + (g_enabled ? L"on" : L"off") +
            L". " + radar::status() + L".";
    say({text}, true);
}

void snapshot() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t name[64]{};
    swprintf_s(name, L"snapshot-%04d%02d%02d-%02d%02d%02d.txt", now.wYear, now.wMonth, now.wDay,
               now.wHour, now.wMinute, now.wSecond);
    const std::wstring path = moduleDir() + L"\\" + name;
    FILE* file = _wfopen(path.c_str(), L"w, ccs=UTF-8");
    if (!file) {
        say({L"Could not save a snapshot."}, true);
        return;
    }
    const auto fields = g_reader.read();
    const auto shown = onScreen(fields);
    fwprintf(file, L"SFAccess snapshot, %04d-%02d-%02d %02d:%02d:%02d\n", now.wYear, now.wMonth,
             now.wDay, now.wHour, now.wMinute, now.wSecond);
    fwprintf(file, L"%zu fields read, %zu on screen\n", fields.size(), shown.size());
    for (const auto& field : fields) {
        fwprintf(file, L"%s %7.0f,%7.0f tint %.2f %.2f %.2f %.2f%s%s%s%s %ls\n",
                 field.shown() ? L"*" : L" ", field.x, field.y, field.tint[0], field.tint[1],
                 field.tint[2], field.tint[3], field.hidden ? L" hidden" : L"",
                 field.rooted ? L"" : L" loose", field.chosen ? L" chosen" : L"",
                 field.button ? L" button" : L"", field.text.c_str());
    }
    fclose(file);
    logLine(L"snapshot written to " + path);
    say({L"Snapshot saved."}, true);
}

void poll(std::vector<Field> shown) {
    g_reader.markChoices(shown);
    g_reader.markCommandRow(shown);
    dropFaded(shown);
    g_shownCount = shown.size();
    if (shown.empty()) {
        g_previousShown.clear();
        g_pendingTexts.clear();
        // Keep stage select's keys until the screen is definitely gone. A
        // hidden panel can produce an empty poll just before it is restored.
        return;
    }

    // A tips screen has a heading and a body under one holder, with no
    // selected row. Read the pair as one announcement before the generic
    // "new text" path can say the heading and its TIP label alone.
    if (handleTipsScreen(shown, Clock::now())) return;

    // Battle Settings' picture grids mark the cursor with brightness rather
    // than the gold used by ordinary menu rows.
    if (handleBattleGrid(shown, Clock::now())) return;
    if (handleStageSelect(shown, Clock::now())) return;

    // The Character Story tab is gold for the whole screen. Once the card
    // grid has painted, treat the tab as context rather than as the selected
    // item. The grid's fields are rebuilt as it scrolls and disappear from
    // the reader for a poll or two; keep the story state across that gap so
    // the tab is not announced alone between cards.
    const auto storyNow = Clock::now();
    const bool storyTab = isCharacterStoryTab(shown);
    if (hasCharacterStoryCard(shown)) {
        g_storyGrid = true;
        g_storyGridAt = storyNow;
    }
    if (!storyTab ||
        (g_storyGrid && storyNow - g_storyGridAt > std::chrono::seconds(5))) {
        g_storyGrid = false;
    }
    const bool characterStory = storyTab && g_storyGrid;
    if (!g_storyGrid) {
        g_storyCardName.clear();
        g_storyCardLines.clear();
        g_storyPageSlots.clear();
        g_storyAwaySlots.clear();
        g_storyCardX = 0.0f;
        g_storyCardY = 0.0f;
        g_storyCardAway = false;
        g_storyCardFallbackDone = false;
        g_storyCardLostAt = Clock::time_point::min();
    }

    // A prompt marks its chosen button by structure, not by gold. When one is
    // present it is the selection, even if a menu behind it is still gold.
    uintptr_t chosenGroup = 0;
    uintptr_t chosenPanel = 0;
    for (const auto& field : shown) {
        if (field.chosen) {
            chosenGroup = field.group;
            chosenPanel = field.panel;
            break;
        }
    }
    const bool anyChosen = chosenGroup != 0;
    if (anyChosen) {
        g_lastChosenAt = Clock::now();
    } else if (!g_lastQuestion.empty() &&
               Clock::now() - g_lastChosenAt > std::chrono::milliseconds(1500)) {
        // The prompt is gone. The same question reopened later is news again.
        g_lastQuestion.clear();
    }

    // A disabled menu item has no gold text; its cursor is a full-row
    // highlight. When nothing is gold or chosen, ask the reader to find that
    // highlight so greyed entries such as General Story still speak. The
    // Controller Settings overlay leaves the previous row gold while a
    // disabled row carries the highlight, so identify the overlay first and
    // look for the highlight even when that gold row is present.
    const bool controllerSetting = isControllerSetting(shown);
    if (!anyChosen) {
        bool anyGold = false;
        for (const auto& field : shown) {
            if (field.gold()) {
                anyGold = true;
                break;
            }
        }
        if (!anyGold || controllerSetting) g_reader.markListFocus(shown);
    }

    // The pause menu item that opened Controller Setting stays gold behind the
    // overlay, and its y falls between two rows. Leave that one item out; the
    // rows, the bottom actions and any confirmation dialog still read.
    const bool versusMode = isVersusModeScreen(shown);
    const bool characterSelect = isCharacterSelect(shown);
    const bool opponentSelect = characterSelect && isOpponentSelect(shown);

    std::vector<std::wstring> selected;
    std::set<std::wstring> selectedTexts;
    const Field* selectedField = nullptr;
    bool unavailableFocus = false;
    if (controllerSetting) {
        for (const auto& field : shown) {
            if (field.listSelected && isUnavailableRow(field)) {
                unavailableFocus = true;
                break;
            }
        }
    }
    const Field* footer = nullptr;
    for (const auto& field : shown) {
        if (hiddenStageOverlay(field.text) || copyrightNotice(field.text) ||
            tipCounter(field.text)) continue;
        if (controllerSetting && field.x < 700.0f &&
            (trim(field.text) == L"Controller Setting" ||
             trim(field.text) == L"Controller Settings")) {
            continue;
        }
        if (versusMode && trim(field.text) == L"Controller Settings") continue;
        const bool lit =
            anyChosen ? field.chosen
                      : (controllerSetting && unavailableFocus
                             ? field.listSelected
                             : (field.gold() || field.listSelected));
        // "BEST SCORE ---" is a label with no score behind it. The path name
        // and battle count are the useful part of the card.
        if (lit && field.text.rfind(L"BEST SCORE", 0) == 0 &&
            field.text.find(L"---") != std::wstring::npos) {
            continue;
        }
        if (lit) {
            if (characterStory && g_storyGrid && isStoryTab(field)) continue;
            // Character select's heading and CPU tag are labels on the screen,
            // not the fighter under the cursor. Only the fighter name is read.
            if (characterSelect &&
                (isCharacterSelectHeading(field.text) || isCpuBadge(field.text))) {
                continue;
            }
            if (characterSelect && !opponentSelect && characterSelectCpuName(field)) continue;
            if (!selectedField) selectedField = &field;
            // The screen draws each row's assigned button as a picture, so the
            // layout has to be read from the screen's game object. Its binding
            // is part of the announcement and of the change key, which makes
            // an edit read back as soon as it is made.
            std::wstring text = field.text;
            if (controllerSetting) {
                const std::wstring binding = g_keyConfig ? g_keyConfig->binding(text) : L"";
                if (!binding.empty()) text += L", " + binding;
                if (isUnavailableRow(field)) text += L", unavailable";
            }
            if (selectedTexts.insert(text).second) selected.push_back(text);
        }
        if (!anyChosen && !footer && field.y >= 960.0f && field.x < 400.0f &&
            !copyrightNotice(field.text) && !hiddenStageOverlay(field.text) &&
            !tipCounter(field.text)) {
            footer = &field;
        }
    }
    // On the Controller Settings overlay, moving onto a disabled entry can
    // remove every gold marker. The disabled row is then the only grey row in
    // the bottom action column, so use it as the cursor when no row is
    // selected. This is the state in which Restore Previous Settings silently
    // disappeared from the reader.
    if (controllerSetting && selected.empty()) {
        for (const auto& field : shown) {
            if (!isUnavailableRow(field)) continue;
            if (field.x >= 400.0f || field.y < 700.0f) continue;
            if (!worthAnnouncing(field.text)) continue;
            selected.push_back(field.text + L", unavailable");
            selectedField = &field;
            break;
        }
    }
    bool characterSelectRandomOnly = false;
    if (characterSelect) characterSelectRandomOnly = characterSelectLabels(selected);
    std::vector<std::wstring> unavailable;
    if (!controllerSetting) {
        g_lastControllerX = -1.0f;
        g_lastControllerY = -1.0f;
    } else if (selectedField && g_lastControllerX >= 0.0f &&
               g_lastControllerY >= 0.0f &&
               std::fabs(selectedField->x - g_lastControllerX) < 60.0f &&
               std::fabs(selectedField->y - g_lastControllerY) > 1.0f) {
        const float low = std::min(selectedField->y, g_lastControllerY);
        const float high = std::max(selectedField->y, g_lastControllerY);
        for (const auto& field : shown) {
            if (&field == selectedField || !isUnavailableRow(field)) continue;
            if (std::fabs(field.x - selectedField->x) >= 60.0f) continue;
            if (field.y <= low || field.y >= high) continue;
            if (!worthAnnouncing(field.text)) continue;
            const std::wstring note = field.text + L", unavailable";
            if (std::find(unavailable.begin(), unavailable.end(), note) == unavailable.end()) {
                unavailable.push_back(note);
            }
        }
    }
    std::wstring footerText = footer ? footer->text : L"";

    // What is new since the previous read. A dialog's text arrives this way;
    // a selection move changes these keys too, but is spoken from the
    // selection below, which does not care about text that fades in and out.
    std::vector<std::wstring> appeared;
    std::set<std::wstring> current;
    for (const auto& field : shown) {
        const std::wstring key = keyFor(field);
        current.insert(key);
        if (!g_previousShown.count(key)) appeared.push_back(field.text);
    }
    g_previousShown = std::move(current);

    const auto now = Clock::now();
    if (g_config.logFields && isMainMenu(shown)) {
        logLine(L"menu: " + std::to_wstring(selected.size()) + L" selected, " +
                std::to_wstring(appeared.size()) + L" appeared");
        if (!appeared.empty()) logLine(L"menu text: " + join(appeared));
    }

    // While the card grid scrolls, its cursor tint can drop permanently. The
    // game leaves the cards visible but dim, so there is no gold selection to
    // read even after the page comes back. Keep the card the cursor last had
    // and restore it when that card is visible again.
    bool storyRestored = false;
    if (characterStory && g_storyGrid && !selected.empty()) {
        std::wstring card;
        const Field* cardField = nullptr;
        bool manyCards = false;
        for (const auto& field : shown) {
            if (!(field.gold() || field.listSelected)) continue;
            if (isStoryTab(field)) continue;
            const std::wstring name = storyCardName(field);
            if (name.empty()) continue;
            if (!card.empty() && card != name) {
                manyCards = true;
                break;
            }
            if (card.empty()) cardField = &field;
            card = name;
        }
        if (!manyCards && !card.empty() && cardField) {
            g_storyCardName = card;
            g_storyCardLines = selected;
            g_storyPageSlots = storyPageSlots(shown);
            g_storyAwaySlots.clear();
            g_storyCardX = cardField->x;
            g_storyCardY = cardField->y;
            g_storyCardAway = false;
            g_storyCardFallbackDone = false;
            g_storyCardLostAt = Clock::time_point::min();
        }
    } else if (characterStory && g_storyGrid) {
        const auto currentSlots = storyPageSlots(shown);
        size_t pageMatches = 0;
        for (const auto& slot : currentSlots) {
            if (g_storyPageSlots.count(slot)) ++pageMatches;
        }
        const bool pageBack =
            storyCardVisible(shown, g_storyCardName) || pageMatches >= 3;
        if (!pageBack) {
            if (currentSlots == g_storyAwaySlots) {
                g_lastKey.clear();
                g_pendingTexts.clear();
                return;
            }
            const Field* next = storyCardAtSlot(shown, g_storyCardX, g_storyCardY);
            const std::wstring nextName = next ? storyCardName(*next) : L"";
            if (!nextName.empty() && nextName != g_storyCardName) {
                std::vector<std::wstring> lines = storyCardLinesAt(shown, *next);
                if (lines.empty()) lines.push_back(nextName);
                selected = lines;
                storyRestored = true;
                g_storyAwaySlots = currentSlots;
                g_storyCardAway = false;
                g_storyCardFallbackDone = false;
                g_storyCardLostAt = Clock::time_point::min();
                g_lastKey.clear();
                logLine(L"story card page: " + join(selected));
            } else {
                g_storyCardAway = true;
                g_storyCardFallbackDone = false;
                g_storyCardLostAt = Clock::time_point::min();
                g_lastKey.clear();
                g_pendingTexts.clear();
                return;
            }
        }
        if (pageBack) {
            g_storyAwaySlots.clear();
            if (!g_storyCardFallbackDone &&
                g_storyCardLostAt == Clock::time_point::min()) {
                g_storyCardLostAt = storyNow;
            }
            const bool held =
                storyNow - g_storyCardLostAt >= std::chrono::milliseconds(300);
            const bool quiet = g_lastSpokenAt == Clock::time_point::min() ||
                               storyNow - g_lastSpokenAt >= std::chrono::milliseconds(1200);
            if (!g_storyCardFallbackDone && !g_storyCardAway && (!held || !quiet)) {
                g_pendingTexts.clear();
                return;
            }
            if (!g_storyCardFallbackDone && !g_storyCardLines.empty()) {
                selected = g_storyCardLines;
                storyRestored = true;
                g_storyCardAway = false;
                g_storyCardFallbackDone = true;
                g_storyCardLostAt = Clock::time_point::min();
                g_lastKey.clear();
                logLine(L"story card fallback: " + join(selected));
            }
        }
        if (selected.empty()) {
            g_lastKey.clear();
            g_pendingTexts.clear();
            return;
        }
    }

    // The command list draws its tab with a gold border image, not gold text,
    // so the normal selected-row path has nothing to hold onto. Read the
    // panel itself; a tab change is a new screen and reads its own moves.
    {
        const bool commandScreen = isCommandScreen(shown);
        CommandPanel panel;
        if (commandPanelLines(shown, commandScreen, panel)) {
            const std::wstring context = panel.name + L"\x1f" + panel.tab;
            const bool contextChanged = context != g_lastPanelContext;
            if (panel.key != g_pendingKey) {
                g_pendingKey = panel.key;
                // The moves paint a frame before the name and the tab. Hold
                // the first panel or a tab change long enough to collect all
                // three. A row move within the same tab is just cursor
                // navigation; the full-panel settle there is heard as lag.
                const int settle =
                    contextChanged
                        ? (g_config.announceDelayMs < 500 ? 500 : g_config.announceDelayMs)
                        : g_config.announceDelayMs;
                g_pendingAt = now + std::chrono::milliseconds(settle);
                return;
            }
            if (now < g_pendingAt || panel.key == g_lastKey) return;

            // Say the character, tab and footer when the panel or its tab
            // changes. Moving the cursor down a row says only that move.
            std::vector<std::wstring> lines;
            if (contextChanged && !panel.name.empty()) lines.push_back(panel.name);
            if (contextChanged && !panel.tab.empty()) lines.push_back(panel.tab);
            for (const auto& text : panel.row) {
                if (std::find(lines.begin(), lines.end(), text) == lines.end()) {
                    lines.push_back(text);
                }
            }
            if (contextChanged && !panel.footer.empty()) lines.push_back(panel.footer);
            if (!lines.empty() && !recentlySaid(lines)) {
                say(lines, true);
                logLine(L"  command row: " + std::to_wstring(panel.row.size()) + L" lines");
            }
            g_lastPanelContext = context;
            g_lastKey = panel.key;
            g_pendingTexts.clear();
            return;
        }
        // The tab strip is on screen but the cursor row has not painted yet.
        // Wait for it: reading the body here is what dumped the whole list.
        if (commandScreen) return;
    }

    if (!selected.empty()) {
        // The question belongs to the prompt, not to the focused button.
        // Gather it on every pass and remember the text: a move between Yes
        // and No then says only the button that changed. Other buttons are
        // left out by kind and by length, so both are never read as one line.
        std::vector<std::wstring> question;
        if (anyChosen) {
            for (const auto& field : shown) {
                if (hiddenStageOverlay(field.text) || copyrightNotice(field.text) ||
            tipCounter(field.text)) continue;
                if (field.chosen || field.button) continue;
                if (field.text.size() <= 24) continue;
                if (std::find(field.chain.begin(), field.chain.end(), chosenPanel) ==
                    field.chain.end()) {
                    continue;
                }
                if (std::find(field.chain.begin(), field.chain.end(), chosenGroup) !=
                    field.chain.end()) {
                    continue;
                }
                if (std::find(question.begin(), question.end(), field.text) == question.end()) {
                    question.push_back(field.text);
                }
            }
        }
        const std::wstring questionKey = join(question);
        const bool newQuestion =
            anyChosen && !questionKey.empty() && questionKey != g_lastQuestion;

        std::vector<std::wstring> blurbs;
        if (g_config.readBlurbs && isArcadePathScreen(shown)) {
            blurbs = arcadePathBlurbs(shown);
        }

        std::wstring key = join(selected);
        key += L"\x1f";
        key += footerText;
        for (const auto& text : blurbs) {
            key += L"\x1f";
            key += text;
        }
        if (anyChosen) {
            key += L"\x1f" + std::to_wstring(chosenGroup) + L"\x1f" + questionKey;
        }

        if (storyRestored) {
            // The grid only exposes the synthesized card for one poll. Do not
            // wait for the normal second poll before speaking it.
            g_pendingKey = key;
        } else {
            if (key != g_pendingKey) {
                if (g_config.logFields && isMainMenu(shown)) {
                    logLine(L"menu selected: " + key);
                }
                g_pendingKey = key;
                // A badge-only poll can be the random tile, or it can be the
                // player tag that paints a frame before a fighter's name. Give
                // the name time to arrive before naming the random slot.
                const int settle =
                    (characterSelect && characterSelectRandomOnly)
                        ? std::max(900, g_config.announceDelayMs)
                        : g_config.announceDelayMs;
                g_pendingAt = now + std::chrono::milliseconds(settle);
                return;
            }
            // A prompt draws its question a moment after its first button.
            // Hold that first announcement until the panel stops changing;
            // later moves between its buttons are not held.
            if (newQuestion && !appeared.empty()) {
                g_pendingAt = now + std::chrono::milliseconds(500);
                return;
            }
            if (now < g_pendingAt || key == g_lastKey) return;
        }

        std::vector<std::wstring> lines;
        if (newQuestion) {
            lines = question;
            g_lastQuestion = questionKey;
        }
        for (const auto& text : unavailable) {
            if (std::find(lines.begin(), lines.end(), text) == lines.end()) {
                lines.push_back(text);
            }
        }
        for (const auto& text : selected) {
            if (std::find(lines.begin(), lines.end(), text) == lines.end()) lines.push_back(text);
        }
        for (const auto& text : blurbs) {
            if (std::find(lines.begin(), lines.end(), text) == lines.end()) lines.push_back(text);
        }
        if (!footerText.empty() && footerText != g_lastFooter) lines.push_back(footerText);
        const bool repeated = !storyRestored && recentlySaid(lines);
        if (!repeated) say(lines, true);
        if (controllerSetting && selectedField) {
            g_lastControllerX = selectedField->x;
            g_lastControllerY = selectedField->y;
        }
        if (repeated) return;
        g_pendingTexts.clear();
        g_lastKey = key;
        g_lastFooter = footerText;
        if (g_config.logFields) {
            for (const auto& field : shown) {
                logLine(L"  field " + std::to_wstring(static_cast<int>(field.x)) + L"," +
                        std::to_wstring(static_cast<int>(field.y)) + L" " + field.text);
            }
        }
        return;
    }

    // A live fight clock means the HUD is drawing counters and damage numbers.
    // They are not menu text, and the round and knockout calls come from
    // updateFight. Stay quiet here until the clock is gone.
    if (g_fightClock > 0) {
        g_pendingTexts.clear();
        return;
    }

    // Nothing selected: this is a prompt, a toast or a status line. Collect
    // what appears and speak once the screen has stopped changing, so a menu
    // building itself is not read out one word at a time. A menu's footer is
    // skipped: it arrives a moment before the gold entry on the same screen,
    // and would otherwise be read alone and again with the entry.
    const bool quickMenu = characterSelect || isMainMenu(shown);
    const bool arcadePath = isArcadePathScreen(shown);
    for (const auto& field : shown) {
        if (field.y >= 960.0f && field.x < 400.0f) continue;
        if (field.text.size() > 400) continue;
        if (!g_config.readBlurbs && arcadePath && isArcadePathBlurb(field)) continue;
        if (controllerSetting &&
            (trim(field.text) == L"Controller Setting" ||
             trim(field.text) == L"Controller Settings")) {
            continue;
        }
        if (versusMode && trim(field.text) == L"Controller Settings") continue;
        // The random tile's "???" is real content, but has no letters for the
        // usual worthAnnouncing filter, so translate it before that filter.
        // Only new text counts; otherwise an unchanged tile would be repeated
        // after every poll.
        if (characterSelect && isCharacterSelectRandom(field, opponentSelect)) {
            if (std::find(appeared.begin(), appeared.end(), field.text) != appeared.end() &&
                std::find(g_pendingTexts.begin(), g_pendingTexts.end(), L"Random select") ==
                    g_pendingTexts.end()) {
                g_pendingTexts.push_back(L"Random select");
            }
            continue;
        }
        if (!worthAnnouncing(field.text)) continue;
        // CPU, 1P and 2P are roster labels, not new screen text. The CPU's own
        // fighter name is not the pick the player is making on the P1 screen,
        // but it is the pick on opponent select.
        if (characterSelect && isSideBadge(field.text)) continue;
        if (characterSelect && !opponentSelect && characterSelectCpuName(field)) continue;
        if (std::find(appeared.begin(), appeared.end(), field.text) == appeared.end()) continue;
        if (std::find(g_pendingTexts.begin(), g_pendingTexts.end(), field.text) ==
            g_pendingTexts.end()) {
            g_pendingTexts.push_back(field.text);
        }
    }
    if (!appeared.empty()) {
        // Prompts build themselves a button at a time; give them longer to
        // finish than a menu entry needs. A character name, a menu icon or a
        // short menu label is not a prompt: waiting half a second there is
        // heard as the cursor lagging. A long line means a question or a
        // description is still arriving, so keep the longer settle for it.
        bool longLine = false;
        for (const auto& line : g_pendingTexts) {
            if (line.size() >= 24) {
                longLine = true;
                break;
            }
        }
        const int settle = (quickMenu && !longLine)
                               ? g_config.announceDelayMs
                               : (g_config.announceDelayMs < 500 ? 500
                                                               : g_config.announceDelayMs);
        if (g_config.logFields && quickMenu) {
            logLine(L"menu settle: " + std::to_wstring(settle) + L" ms for " +
                    join(g_pendingTexts));
        }
        g_pendingTextsAt = now + std::chrono::milliseconds(settle);
    }
    if (g_pendingTexts.empty() || now < g_pendingTextsAt) return;
    std::vector<std::wstring> lines = g_pendingTexts;
    g_pendingTexts.clear();
    if (characterSelect) characterSelectLabels(lines);
    if (lines.size() > 4) return;
    if (!recentlySaid(lines) && !recentlyCovered(lines)) say(lines, true);
    g_lastKey.clear();
}

void handle(Action action) {
    switch (action) {
        case Action::Repeat:
            if (g_lastLines.empty()) {
                say({L"Nothing to repeat."}, true);
            } else {
                say(g_lastLines, true);
            }
            break;
        case Action::NextLine:
        case Action::PreviousLine: {
            if (g_lastLines.empty()) {
                say({L"Nothing to review."}, true);
                break;
            }
            const int count = static_cast<int>(g_lastLines.size());
            if (g_reviewIndex < 0) {
                g_reviewIndex = action == Action::NextLine ? 0 : count - 1;
            } else {
                g_reviewIndex += action == Action::NextLine ? 1 : -1;
                g_reviewIndex = std::clamp(g_reviewIndex, 0, count - 1);
            }
            const std::wstring prefix = L"Line " + std::to_wstring(g_reviewIndex + 1) + L" of " +
                                        std::to_wstring(count) + L". ";
            speech::speak(prefix + g_lastLines[static_cast<size_t>(g_reviewIndex)], true);
            logLine(L"review: " + prefix + g_lastLines[static_cast<size_t>(g_reviewIndex)]);
            break;
        }
        case Action::ReadAll: {
            auto shown = onScreen(g_reader.read());
            dropFaded(shown);
            const bool characterSelect = isCharacterSelect(shown);
            const bool stageSelect = isStageSelect(shown);
            std::vector<std::wstring> lines;
            std::set<std::wstring> seen;
            for (const auto& field : shown) {
                if (hiddenStageOverlay(field.text) || copyrightNotice(field.text) ||
            tipCounter(field.text)) continue;
                std::wstring text = field.text;
                if (stageSelect && randomStageName(field)) text = L"Random stage";
                if (!seen.insert(text).second) continue;
                lines.push_back(text);
                if (lines.size() >= 60) break;
            }
            if (characterSelect) characterSelectLabels(lines);
            if (lines.empty()) {
                say({L"Nothing to read yet."}, true);
            } else {
                say(lines, true);
            }
            break;
        }
        case Action::Toggle:
            g_enabled = !g_enabled;
            if (!g_enabled) radar::update(false);
            speech::speak(g_enabled ? L"Narration on." : L"Narration off.", true);
            logLine(g_enabled ? L"narration on" : L"narration off");
            break;
        case Action::Status:
            status();
            break;
        case Action::Snapshot:
            snapshot();
            break;
        case Action::Silence:
            speech::stop();
            break;
        case Action::ReadFight:
            announceFight();
            break;
        case Action::ToggleRadar:
            radar::setEnabled(!radar::enabled());
            speech::speak(radar::enabled() ? L"Radar on." : L"Radar off.", true);
            break;
        case Action::DescribeRadar:
            say({radar::describe()}, true);
            break;
        case Action::MacroCritical:
        case Action::MacroVSkill:
        case Action::MacroVTrigger: {
            const macro::Kind kind = action == Action::MacroCritical   ? macro::Kind::Critical
                                     : action == Action::MacroVSkill ? macro::Kind::VSkill
                                                                     : macro::Kind::VTrigger;
            const std::wstring line =
                macro::trigger(kind, g_playerName, radar::playerFacingRight(), g_config);
            say({line}, true);
            break;
        }
    }
}

void loop() {
    auto nextScan = Clock::now();
    int emptyScans = 0;
    while (g_running && !g_stopRequested) {
        {
            std::deque<Action> actions;
            {
                std::lock_guard<std::mutex> lock(g_actionsLock);
                actions.swap(g_actions);
            }
            for (Action action : actions) handle(action);
        }

        const bool focused = gameHasFocus();
        if (!focused && !g_config.narrateInBackground) {
            if (g_wasFocused) {
                // Dropping the last screen means coming back announces where
                // the cursor is rather than staying silent.
                g_lastKey.clear();
                g_lastFooter.clear();
                g_previousShown.clear();
                g_pendingTexts.clear();
                speech::stop();
                radar::update(false);
            }
            g_wasFocused = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            continue;
        }
        if (!g_wasFocused) g_lastKey.clear();
        g_wasFocused = true;

        const auto now = Clock::now();
        if (now >= nextScan) {
            g_reader.scan(false);
            if (g_reader.fieldCount() == 0 && ++emptyScans >= 2) {
                g_reader.scan(true);
                emptyScans = 0;
            }
            nextScan = Clock::now() + std::chrono::milliseconds(g_config.scanMs);
        }
        if (g_enabled) {
            const auto frameStart = Clock::now();
            auto shown = onScreen(g_reader.read());
            updateFight(shown);
            poll(shown);
            const auto frameMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     Clock::now() - frameStart)
                                     .count();
            if (frameMs >= 150) {
                logLine(L"slow: read, fight and poll took " + std::to_wstring(frameMs) +
                        L" ms for " + std::to_wstring(shown.size()) + L" fields");
            }
        } else {
            radar::update(false);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(g_config.pollMs));
    }
}

}  // namespace

void start() {
    if (g_running) return;
    g_config = loadConfig();
    g_enabled = g_config.enabled;

    speech::init(g_config.rate);

    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    g_reader = Reader(g_base);
    g_fight = std::make_unique<FightReader>(g_base);
    g_keyConfig = std::make_unique<KeyConfigReader>(g_base);
    if (!g_reader.verifyBuild()) {
        logLine(L"this executable does not match the mod's offsets; narration is off");
        speech::speak(L"SFAccess: this version of Street Fighter V is not supported yet.", true);
        return;
    }
    radar::start(g_base, g_config);

    g_running = true;
    g_thread = std::thread(loop);
    // Detached: the game exits without unloading the DLL, and a joinable
    // std::thread destroyed by the CRT at process exit calls terminate.
    g_thread.detach();
    logLine(L"narrator started");
}

void post(Action action) {
    {
        std::lock_guard<std::mutex> lock(g_actionsLock);
        g_actions.push_back(action);
    }
}

void requestStop() {
    g_stopRequested = true;
    g_running = false;
    macro::requestStop();
    radar::requestStop();
}

void shutdown() {
    if (!g_running) return;
    requestStop();
    if (g_thread.joinable()) g_thread.join();
    speech::shutdown();
    logLine(L"narrator stopped");
}

}  // namespace narrator
}  // namespace sfva
