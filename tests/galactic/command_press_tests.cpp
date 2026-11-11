#include "galactic/command_press.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <map>
#include <string>
#include <vector>

// #5857: the press path of the command-bar controls (CommandPresses), the code behind the HUD's press_command_control and
// service_forced_clicks, with recorders for the widgets. Synthetic control names only.
using namespace eawr::viewer::galactic;

namespace {
struct Recorder final : CommandPresses::Surface, CommandPresses::Effects {
    CommandPresses& presses;
    ControlStates& states;
    std::map<std::string, CommandPresses::Target> controls;
    std::vector<std::string> pressed, clicks, actions;
    std::vector<std::string> listened;
    int refreshes{};
    std::vector<std::string> nested; // a control whose action asks for another drain
    Recorder(CommandPresses& p, ControlStates& s) : presses(p), states(s) {}
    [[nodiscard]] bool story_listens(const std::string& name) const override {
        for (const auto& item : listened) if (item == name) return true;
        return false;
    }
    void raise_click(const std::string& name) override { clicks.push_back(name); }
    void action(const std::string& name) override {
        actions.push_back(name);
        if (name == "nests") presses.drain(std::vector<std::string>{"plain"}, *this, 8, [](const CommandPresses::Drop&) { assert(false); });
    }
    void refresh() override { ++refreshes; }
    [[nodiscard]] CommandPresses::Target find(std::string_view component) const override {
        const auto at = controls.find(std::string(component));
        return at == controls.end() ? CommandPresses::Target{} : at->second;
    }
    void activate(std::string_view component) override {
        pressed.emplace_back(component);
        presses.press(states, std::string(component), *this);
    }
};
} // namespace

int case_galactic_command_press_tests() {
    {
        // A human press raises the story click when a plot listens, changes the selection, runs the action and refreshes.
        CommandPresses presses;
        ControlStates states;
        states.add("heard", {"Pane"}, true, true);
        states.add("quiet", {"Pane"}, true, true);
        Recorder hud(presses, states);
        hud.listened = {"heard"};
        presses.press(states, "heard", hud);
        assert(hud.clicks == std::vector<std::string>{"heard"} && states.selected("heard") && hud.refreshes == 1);
        assert(hud.actions == std::vector<std::string>{"heard"});
        presses.press(states, "quiet", hud);
        assert(hud.clicks.size() == 1 && states.selected("quiet") && !states.selected("heard"));
        presses.press(states, "unknown", hud);
        assert(hud.clicks.size() == 1 && hud.actions.size() == 3 && hud.refreshes == 3);
    }
    {
        // A forced press runs the same press path but raises no story click and does not refresh (ControlStateRules).
        CommandPresses presses;
        ControlStates states;
        states.add("tab", {"Tabs"}, false, true);
        states.add("filter", {"Filters"}, true, true);
        Recorder hud(presses, states);
        hud.listened = {"tab", "filter"};
        hud.controls = {{"tab", {true, false, true}}, {"filter", {true, false, true}}};
        const std::vector<std::string> requests{"tab", "filter"};
        assert(presses.drain(requests, hud, 8, [](const CommandPresses::Drop&) { assert(false); }) == 2);
        assert(hud.pressed == requests && hud.clicks.empty() && hud.refreshes == 0);
        assert(states.selected("tab") && states.selected("filter"));
        assert(!presses.forced_press());
        // The same press by a human raises the click and refreshes.
        presses.press(states, "tab", hud);
        assert(hud.clicks == std::vector<std::string>{"tab"} && hud.refreshes == 1);
        // A mod-selectable rule: when forced presses are allowed to raise clicks, they do.
        ControlStateRules rules; rules.forced_press_raises_story_click = true;
        ControlStates raising(rules);
        raising.add("tab", {"Tabs"}, false, true);
        Recorder loud(presses, raising);
        loud.listened = {"tab"};
        loud.controls = {{"tab", {true, false, true}}};
        presses.drain(std::vector<std::string>{"tab"}, loud, 8, [](const CommandPresses::Drop&) { assert(false); });
        assert(loud.clicks == std::vector<std::string>{"tab"});
    }
    {
        // Absent, hidden and disabled controls and over-budget presses are dropped, each pair reported once, never an abort.
        CommandPresses presses;
        ControlStates states;
        Recorder hud(presses, states);
        hud.controls = {{"plain", {true, false, true}}, {"hidden", {true, false, false}}, {"off", {true, true, true}}};
        std::vector<CommandPresses::Drop> drops;
        const auto report = [&](const CommandPresses::Drop& drop) { drops.push_back(drop); };
        const std::vector<std::string> requests{"missing", "hidden", "off", "plain", "plain", "plain"};
        assert(presses.drain(requests, hud, 2, report) == 2);
        assert(hud.pressed == (std::vector<std::string>{"plain", "plain"}));
        assert(drops.size() == 4 && drops[0].component == "missing" && drops[1].component == "hidden" && drops[2].component == "off"
            && drops[3].component == "plain" && drops[3].reason.find("more forced clicks") != std::string::npos);
        presses.drain(requests, hud, 2, report); // the same pairs again: pressed again, reported no more
        assert(drops.size() == 4 && hud.pressed.size() == 4);
    }
    {
        // A press that asks for another drain gets none: the guard is part of the press path.
        CommandPresses presses;
        ControlStates states;
        Recorder hud(presses, states);
        hud.controls = {{"nests", {true, false, true}}, {"plain", {true, false, true}}};
        presses.drain(std::vector<std::string>{"nests"}, hud, 8, [](const CommandPresses::Drop&) { assert(false); });
        assert(hud.pressed == std::vector<std::string>{"nests"});
        assert(!presses.forced_press());
    }
    return 0;
}
