#pragma once

#include "eawr/sim/galactic/types.hpp"

namespace eawr::sim::galactic {
// WGCH-70/157: current authored group and enabled/cancelled service facts.
[[nodiscard]] bool ground_ability_enabled(const GalacticAbilityProfile&, const AbilityActorState&, AbilityKind);
// Prepared facts are display data until the host checks their retained identity and exact value.
[[nodiscard]] core::Result<void> validate_ability_host(const GalacticSetup&);
[[nodiscard]] PreparedAbility prepared_ability(const AbilityPreparation&, std::uint32_t choice);
} // namespace eawr::sim::galactic
