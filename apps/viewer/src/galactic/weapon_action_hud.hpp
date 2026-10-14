#pragma once
#include "hero_action_hud.hpp"
namespace eawr::presentation::godot_backend {
struct WeaponActionHudState {
    std::array<HeroActionSurface, 1> surfaces;
    godot::Control* blocker{};
    std::shared_ptr<const sim::galactic::GalacticSnapshot> fixture, publication;
    std::optional<sim::galactic::HeroActionMenus> facts;
    std::uint32_t width{}, height{};
    bool closed{}, enabled{};
    std::uint64_t rebuilds{}, choices_accepted{}, closes{};
    std::optional<sim::galactic::hero_actions::Hack> accepted;
};
}
