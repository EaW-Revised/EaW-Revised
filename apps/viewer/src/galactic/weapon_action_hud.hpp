#pragma once
#include "hero_action_hud.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include <optional>
namespace eawr::presentation::godot_backend {
// #4331: everything the weapon-action controls are written from. A publication that leaves it as it was writes nothing.
struct WeaponActionHudKey final {
    bool available{}, enabled{}, closed{}, choice{}, affordable{}, prepared{};
    std::int64_t price_raw{};
    std::uint32_t width{}, height{};
    std::optional<presentation::ui::ShellPlacement> placement;
    friend bool operator==(const WeaponActionHudKey&, const WeaponActionHudKey&) = default;
};
struct WeaponActionHudState {
    std::array<HeroActionSurface, 1> surfaces;
    godot::Control* blocker{};
    std::shared_ptr<const sim::galactic::GalacticSnapshot> fixture, publication;
    std::optional<sim::galactic::HeroActionMenus> facts;
    std::uint32_t width{}, height{};
    bool closed{}, enabled{};
    std::uint64_t rebuilds{}, choices_accepted{}, closes{}, writes{};
    std::optional<WeaponActionHudKey> applied;
    std::optional<sim::galactic::hero_actions::Hack> accepted;
};
}
