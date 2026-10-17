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
    std::uint64_t rebuilds{},accepts{};
    sim::galactic::hero_actions::Id accepted_source{};
    sim::galactic::PlanetId accepted_planet{};
    godot::Vector2 authored_offset,text_offset,authored_size;
    bool projected{},enabled{},right_justified{};
};
}
