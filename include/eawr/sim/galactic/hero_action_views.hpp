#pragma once

#include "eawr/sim/galactic/hero_actions.hpp"
#include "eawr/sim/galactic/observer.hpp"

namespace eawr::sim::galactic {
// WGCH-24/32/52/63/64: copied menu data, never an execution capability.
// Pure policy fixtures retain their display payload. Live theft adds the host's
// exact typed preparation; publication itself never executes an action.
struct ObservedTheftChoice {
    std::size_t prepared_index{};
    hero_actions::Theft prepared;
    bool affordable{};
    std::optional<PreparedAbility> live_prepared;
};
struct ObservedNeutralizationChoice {
    std::size_t prepared_index{};
    hero_actions::Neutralization prepared;
    hero_actions::Id current_type{}, original_type{}, source_type{};
    bool named{}, affordable{};
};
// WGCH-66/112: the weapon identity is display-only, never a queued target ID.
struct HackMenuPreparation {
    hero_actions::Hack prepared;
    hero_actions::Id weapon{};
};
struct ObservedHackChoice {
    hero_actions::Hack prepared;
    hero_actions::Id weapon{}, weapon_type{};
    bool affordable{};
    std::optional<PreparedAbility> live_prepared{}; // the host's exact typed capability; the weapon stays display-only
};
// The ability/container and contested-space predicates belong to the host.
// WGCH-158: the preview publishes contested only for an observer-known blocker.
struct DestructionPreparation {
    hero_actions::Id actor{};
    PlanetId planet{};
    hero_actions::Refusal refusal{};
    // WGCH-155/156: original drag source/container, distinct from selected actor.
    hero_actions::Id source{};
    std::optional<PreparedAbility> live_prepared{}; // the host's typed capability for the same source/planet
};
struct HeroActionMenus {
    PlayerId player{};
    EntityId actor{};
    PlanetId planet{};
    unsigned tech{};
    std::optional<std::vector<ObservedTheftChoice>> theft;
    std::optional<std::vector<ObservedNeutralizationChoice>> neutralization;
    std::optional<ObservedHackChoice> hack;
    std::optional<DestructionPreparation> destruction;
    std::uint64_t preparation{};
};
// Missing/foreign actor, account or discovery rejects the entire publication.
// Hidden/stale target choices are omitted, preserving original option indices.
[[nodiscard]] std::optional<HeroActionMenus> observed_hero_action_menus(
    const hero_actions::Fixture&, PlayerId local_player, const ObservedPlanet&,
    hero_actions::Id actor, const hero_actions::TheftPreparation* = nullptr,
    const hero_actions::NeutralizationPreparation* = nullptr,
    const HackMenuPreparation* = nullptr, const DestructionPreparation* = nullptr);
} // namespace eawr::sim::galactic
