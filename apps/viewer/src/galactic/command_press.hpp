#pragma once

#include "command_control_state.hpp"

#include <cctype>
#include <cstddef>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace eawr::viewer::galactic {

// The press path of the command-bar controls, without the engine's widgets (#5857; WST-37, WST-75). A human press, a
// FORCE_CLICK_GUI press and the drain of the forced requests all run through this one class; the HUD gives it the buttons
// and the effects (`Surface`, `Effects`), and the tests give it recorders and a real game session. Nothing here knows a
// control's name: which presses raise a story click, which change a selection and which are dropped is decided by the
// rules of ControlStates and by what the loaded plots listen to.
class CommandPresses final {
public:
    // What the HUD shows of one control right now.
    struct Target {
        bool present{}, disabled{}, visible{};
    };
    // The HUD's controls. `activate` is the human press path minus the pointer: it ends in CommandPresses::press.
    class Surface {
    public:
        virtual ~Surface() = default;
        [[nodiscard]] virtual Target find(std::string_view component) const = 0;
        virtual void activate(std::string_view component) = 0;
    };
    // What a press does besides the selection state.
    class Effects {
    public:
        virtual ~Effects() = default;
        // Whether a loaded plot has a STORY_CLICK_GUI event naming the control (a click nobody hears is not sent).
        [[nodiscard]] virtual bool story_listens(const std::string& control) const = 0;
        // Sends the click to the story, on the seat's command stream.
        virtual void raise_click(const std::string& control) = 0;
        // The control's own effect: a tab view, the pause, a map layer. Not called for the selection state.
        virtual void action(const std::string& control) = 0;
        // Refreshes the whole HUD; skipped for a forced press, which runs inside the frame's own refresh.
        virtual void refresh() = 0;
    };

    // One press. A press made while the forced requests are drained is a forced one (ControlStateRules).
    void press(ControlStates& states, const std::string& name, Effects& effects) const {
        const bool forced = busy_;
        // WST-37: the plots hear a click on any control a STORY_CLICK_GUI event names, whatever else the click does.
        if (states.rules().raises_story_click(forced) && effects.story_listens(name)) effects.raise_click(name);
        if (states.has(name)) states.press(name);
        effects.action(name);
        if (!forced) effects.refresh();
    }

    // Why a request was not pressed. The caller reports each (component, reason) pair once.
    struct Drop {
        std::string component, reason;
    };
    // Presses the named controls in order. An absent, hidden or disabled control has no effect (a project choice: the
    // reward has no such gate in the debug build's reward function, and the slot's own check was not read); presses beyond
    // `budget` are dropped. A press that asks for another drain gets none. Never an abort.
    template <class Report>
    std::size_t drain(std::span<const std::string> components, Surface& surface, unsigned budget, Report&& report) {
        if (busy_) return 0;
        busy_ = true; // a press never starts a second drain, whatever its action calls
        struct Done { bool& busy; ~Done() { busy = false; } } done{busy_};
        ForcedClickBudget remaining{budget};
        std::size_t pressed{};
        for (const auto& component : components) {
            const auto target = surface.find(component);
            if (!target.present || target.disabled || !target.visible) {
                note(component, "the control is absent, hidden or disabled; no click", report);
                continue;
            }
            if (!remaining.take()) {
                note(component, "more forced clicks than one frame admits; the click is dropped", report);
                continue;
            }
            surface.activate(component);
            ++pressed;
        }
        return pressed;
    }

    [[nodiscard]] bool forced_press() const noexcept { return busy_; }

private:
    template <class Report>
    void note(const std::string& component, const char* why, Report& report) {
        if (reported_.insert(component + ' ' + why).second) report(Drop{component, why}); // once per pair
    }

    bool busy_{};
    std::set<std::string> reported_;
};

} // namespace eawr::viewer::galactic
