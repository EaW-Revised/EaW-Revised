#pragma once

#include "eawr/sim/galactic/types.hpp"
#include <span>

namespace eawr::sim::galactic {
// WGM-83: any qualifying nontransport; an empty flattened fleet qualifies. Refuses
// containers until their live children are available; authored children are not survivors.
// With `movement`, a transport member's carried units are listed too (the debug build's
// fleet breakdown), so a fleet holding only a loaded transport qualifies through its cargo.
[[nodiscard]] core::Result<bool> fleet_move_without_ability(const Fleet& fleet,
    std::span<const FleetTypeTraits> types, const MovementSetup* movement = nullptr);
} // namespace eawr::sim::galactic
