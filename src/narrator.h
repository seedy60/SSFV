#pragma once

namespace sfva {
namespace narrator {

enum class Action {
    Repeat,
    NextLine,
    PreviousLine,
    ReadAll,
    Toggle,
    Status,
    Snapshot,
    Silence,
    ReadFight,
    ToggleRadar,
    DescribeRadar,
    MacroCritical,
    MacroVSkill,
    MacroVTrigger,
};

void start();
void post(Action action);
// Asks the worker to finish before the loader tears the DLL down. Safe to
// call from DllMain; it only touches an atomic.
void requestStop();
void shutdown();

}  // namespace narrator
}  // namespace sfva
