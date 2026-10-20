#pragma once
#include "map_input.hpp"
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/label.hpp>
namespace eawr::presentation::godot_backend {
class EawrHudButton;
struct DestructionHudState {
    EawrHudButton* slot{};
    godot::Label* text{};
    std::shared_ptr<const sim::galactic::GalacticSnapshot> fixture,publication;
    std::optional<sim::galactic::DestructionPreparation> facts;
    viewer::galactic::PendingAbilityDrop pending{};
    // WGCH-151..155: the dragged fleet's hero action at its parked planet, resolved once per dragged fleet and
    // publication (the slot is positioned per planet every frame). `drop_label` is the kind the text shows.
    std::optional<viewer::galactic::AbilityDrop> drop;
    std::uint64_t drop_fleet{};
    const void* drop_publication{};
    std::optional<sim::galactic::AbilityKind> drop_label;
    std::uint64_t prepares{};
    std::uint64_t rebuilds{},accepts{};
    sim::galactic::hero_actions::Id accepted_source{};
    sim::galactic::PlanetId accepted_planet{};
    godot::Vector2 authored_offset,text_offset,authored_size;
    bool projected{},enabled{},right_justified{};
};
}
