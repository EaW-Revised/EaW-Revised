#pragma once
#include "hero_action_hud.hpp"
#include "planet_hud.hpp"
#include "eawr/presentation/ui/encyclopedia_timing.hpp"
#include <godot_cpp/classes/texture_rect.hpp>

namespace eawr::presentation::godot_backend {
struct BlackMarketArt {
    std::string icon, title, description;
    std::array<std::string, 3> units;
};
struct BlackMarketSubmission {
    sim::EntityId buyer{};
    sim::galactic::PlanetId planet{};
    std::uint32_t index{}, item{};
    sim::math::Fixed price, absence;
};
struct BlackMarketHudState {
    std::array<HeroActionSurface, 1> surfaces;
    godot::Control* blocker{};
    std::shared_ptr<const sim::galactic::GalacticSnapshot> fixture, publication;
    std::optional<sim::galactic::ObservedBlackMarketMenu> facts;
    // Identity survives clearing copied contents; same activation cannot reopen.
    std::optional<std::array<std::uint64_t, 4>> activation;
    std::map<std::uint32_t, BlackMarketArt> art;
    std::vector<std::optional<sim::galactic::ObservedBlackMarketOffer>> bindings;
    std::uint32_t width{}, height{};
    bool closed{}, enabled{};
    std::uint64_t rebuilds{}, choices_accepted{}, closes{}, omitted{};
    std::optional<BlackMarketSubmission> accepted;
    presentation::ui::EncyclopediaTiming timing;
    std::uint64_t wall_time{};
    std::uint32_t shown_item{};
    godot::TextureRect* encyclopedia{};
    godot::Label* title{}, *body{};
    godot::TextureRect* icon{};
    std::array<godot::TextureRect*, 3> units{};
};
} // namespace eawr::presentation::godot_backend
