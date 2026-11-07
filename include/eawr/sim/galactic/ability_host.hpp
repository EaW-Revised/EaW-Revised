#pragma once

#include "eawr/sim/galactic/types.hpp"

namespace eawr::sim::galactic {
// WGCH-70/157: current authored group and enabled/cancelled service facts.
[[nodiscard]] bool ground_ability_enabled(const GalacticAbilityProfile&, const AbilityActorState&, AbilityKind, bool listed_implicitly = false);
// Prepared facts are display data until the host checks their retained identity and exact value.
[[nodiscard]] core::Result<void> validate_ability_host(const GalacticSetup&);
[[nodiscard]] PreparedAbility prepared_ability(const AbilityPreparation&, std::uint32_t choice);
// WGCH-54: the registered technologies a theft by `actor` may offer, in registration order: exact current
// player affiliation, initial lock, slicer flag, tech level and the player's current locks/unlocks.
// The same function feeds the host's preparation and the stock-data offer-count test.
[[nodiscard]] std::vector<const TheftTechnology*> theft_offer_candidates(const GalacticAbilitySetup&,
    const ProductionSetup&, const ProductionPlayer& actor_player, PlayerId actor, std::int64_t current_tech);
} // namespace eawr::sim::galactic
