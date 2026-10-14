#pragma once

#include "eawr/data/ui/shell_anchors.hpp"
#include "eawr/sim/galactic/snapshot.hpp"
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <array>
#include <map>

namespace eawr::presentation::godot_backend {
class EawrTacticalHud;
class EawrHudButton;
struct CorruptionHudState {
    data::ui::ShellAnchors anchors;
    EawrTacticalHud* panel{};
    godot::Control* blocker{};
    std::map<std::string, godot::Label*> labels;
    std::map<std::string, EawrHudButton*> icons;
    std::map<std::string, godot::TextureRect*> satisfaction;
    std::map<sim::galactic::corruption_actions::Id, std::string> briefing_keys;
    std::array<std::string, 2> satisfaction_textures;
    std::array<EawrHudButton*, 3> choices{};
    EawrHudButton* close{};
    std::array<std::vector<std::string>, 5> display;
    std::shared_ptr<const sim::galactic::GalacticSnapshot> fixture, publication;
    std::optional<sim::galactic::CorruptionMenu> facts;
    std::uint32_t width{}, height{};
    bool closed{}, enabled{};
    std::uint64_t rebuilds{}, choices_accepted{}, closes{};
    std::optional<unsigned> accepted_slot;
};
} // namespace eawr::presentation::godot_backend
