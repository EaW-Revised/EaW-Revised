#pragma once

#include "eawr/data/ui/shell_anchors.hpp"
#include "eawr/sim/galactic/snapshot.hpp"
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/label.hpp>
#include <array>
#include <map>

namespace eawr::presentation::godot_backend {
class EawrTacticalHud;
class EawrHudButton;
struct HeroActionSurface {
    data::ui::ShellAnchors anchors;
    EawrTacticalHud* panel{};
    std::map<std::string, godot::Label*> labels;
    std::vector<std::pair<std::string, EawrHudButton*>> choices;
    EawrHudButton* close{};
    std::string close_name;
};
struct HeroActionHudState {
    std::array<HeroActionSurface, 2> surfaces;
    godot::Control* blocker{};
    std::shared_ptr<const sim::galactic::GalacticSnapshot> fixture, publication;
    std::optional<sim::galactic::HeroActionMenus> facts;
    std::uint32_t width{}, height{};
    bool closed{}, theft{}, sabotage{}, enabled{};
    std::uint64_t rebuilds{}, choices_accepted{}, closes{};
    std::optional<std::size_t> accepted_index;
};
} // namespace eawr::presentation::godot_backend
