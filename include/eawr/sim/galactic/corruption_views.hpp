#pragma once

#include "eawr/sim/galactic/corruption_actions.hpp"
#include "eawr/sim/galactic/observer.hpp"

namespace eawr::sim::galactic {
// WGCH-04/49: copied observer-local facts, not an execution capability.
struct ObservedCorruptionChoice {
    unsigned slot{}; // authored slot 1..3, including gaps
    corruption_actions::Prepared prepared;
    bool affordable{}, units_satisfied{}, missions_satisfied{}, planets_satisfied{};
    std::optional<PreparedAbility> live_prepared{};
    std::optional<CorruptionChoice> live_choice{};
};
struct CorruptionMenu {
    PlayerId player{};
    EntityId actor{};
    corruption_actions::Id ability{};
    PlanetId planet{};
    std::vector<ObservedCorruptionChoice> choices;
    std::uint64_t preparation{};
};
// The campaign host supplies its fixture, tutorial admission and routing names.
// No live campaign producer or action executor is installed by this seam.
[[nodiscard]] std::optional<CorruptionMenu> observed_corruption_menu(
    const corruption_actions::Fixture&, PlayerId local_player, const ObservedPlanet&,
    corruption_actions::Id actor, bool tutorial_allowed,
    std::span<const corruption_actions::Id> activated_abilities);
} // namespace eawr::sim::galactic
