#pragma once

#include "eawr/data/ui/command_bar.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::viewer::galactic {

// Selected state of the command-bar controls a mod authors (#5857; WST-37/75, HCB-67). It is presentation only: it
// never reaches the simulation, which hears only the named click. Every rule that decides how a press changes the
// selection lives in ControlStateRules, so the retail capture of button order, exclusion and reset (out/gc/5857-capture)
// changes values here and nowhere else.
struct ControlStateRules final {
    // A `Toggle` control flips its own selection on a press. (Authored flag; retail meaning of the flag is unverified.)
    bool toggle_flips = true;
    // Selecting a `Tab` control clears every other `Tab` control that shares one of its groups. Retail capture
    // (2026-11-05, one pair of space categories): a second category replaced the first. Other groups are unverified.
    bool tab_excludes_group_mates = true;
    // Pressing the selected `Tab` control again: a control that is also `Toggle` clears; a plain Tab stays selected.
    // Retail capture: a second click on the active category restored the full list.
    bool selected_tab_toggles_off = true;
    // A press of the production domain tab (space/ground) clears the authored selections. Off by default: the capture's
    // "full list restored" is an offer list the script owns, and the script clears its own filter button through a forced
    // click, so a clear here would invert that button's selected state. The rule stays for a capture that shows otherwise.
    bool domain_change_clears = false;
    // A press made by a FORCE_CLICK_GUI reward raises the story click like a human press. Off: the debug build's
    // Reward_Force_Click_GUI calls only the control's click slot, while Handle_Mouse_Event calls the slot and then, as a
    // separate call, Tutorial_Click_UI, which raises STORY_CLICK_GUI (WST-75). Raising it would also close a loop for
    // plots that force a tab after each filter click and a filter after each tab click.
    bool forced_press_raises_story_click = false;

    // Whether a press makes the story hear its click. A human press always does (when a plot listens).
    [[nodiscard]] constexpr bool raises_story_click(const bool forced) const noexcept {
        return !forced || forced_press_raises_story_click;
    }
};

class ControlStates final {
public:
    explicit ControlStates(ControlStateRules rules = {}) : rules_(rules) {}

    [[nodiscard]] const ControlStateRules& rules() const noexcept { return rules_; }

    void add(std::string name, std::vector<std::string> groups, bool toggle, bool tab) {
        for (auto& group : groups) group = lower(group);
        controls_.push_back({std::move(name), std::move(groups), toggle, tab, false});
    }
    void add(const data::ui::CommandBarComponent& component) {
        add(component.name, component.groups, component.flag(data::ui::Field::toggle), component.flag(data::ui::Field::tab));
    }

    [[nodiscard]] bool has(std::string_view name) const noexcept { return find(name) != nullptr; }
    // A control that keeps a selection (Toggle or Tab) is drawn pressed while selected; any other control is momentary.
    [[nodiscard]] bool holds_selection(std::string_view name) const noexcept {
        const auto* control = find(name);
        return control && (control->toggle || control->tab);
    }
    [[nodiscard]] bool selected(std::string_view name) const noexcept {
        const auto* control = find(name);
        return control && control->selected;
    }

    // One press of the named control. An unknown name changes nothing.
    void press(std::string_view name) {
        auto* control = find(name);
        if (!control) return;
        if (control->tab) {
            if (control->selected) {
                if (control->toggle && rules_.selected_tab_toggles_off) control->selected = false;
                return;
            }
            control->selected = true;
            if (rules_.tab_excludes_group_mates)
                for (auto& other : controls_)
                    if (&other != control && other.tab && other.selected && share_group(*control, other)) other.selected = false;
        } else if (control->toggle && rules_.toggle_flips) {
            control->selected = !control->selected;
        }
    }

    void domain_changed() {
        if (rules_.domain_change_clears) clear();
    }
    void clear() {
        for (auto& control : controls_) control.selected = false;
    }

private:
    struct Control final {
        std::string name;
        std::vector<std::string> groups; // lower-case
        bool toggle{}, tab{}, selected{};
    };

    [[nodiscard]] static std::string lower(std::string text) {
        for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }
    [[nodiscard]] static bool same(std::string_view a, std::string_view b) noexcept {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
        });
    }
    [[nodiscard]] static bool share_group(const Control& a, const Control& b) {
        return std::any_of(a.groups.begin(), a.groups.end(), [&](const std::string& group) {
            return std::find(b.groups.begin(), b.groups.end(), group) != b.groups.end();
        });
    }
    [[nodiscard]] const Control* find(std::string_view name) const noexcept {
        const auto at = std::find_if(controls_.begin(), controls_.end(), [&](const Control& c) { return same(c.name, name); });
        return at == controls_.end() ? nullptr : &*at;
    }
    [[nodiscard]] Control* find(std::string_view name) noexcept {
        return const_cast<Control*>(static_cast<const ControlStates*>(this)->find(name));
    }

    ControlStateRules rules_;
    std::vector<Control> controls_;
};

// How many forced presses the viewer hands to the HUD in one drain; the rest are dropped with a diagnostic.
// A forced press does not raise a story click (ControlStateRules), so forced presses cannot feed each other; this and the
// simulation's per-seat per-tick count (sim::galactic::max_gui_clicks_per_tick) are safety caps, not the loop stop.
struct ForcedClickBudget final {
    unsigned remaining;
    [[nodiscard]] bool take() noexcept {
        if (remaining == 0) return false;
        --remaining;
        return true;
    }
};

} // namespace eawr::viewer::galactic
