#pragma once

#include "combat_internal.hpp"
#include "eawr/sim/tactical/fighters.hpp"

#include <cstdint>

namespace eawr::sim::tactical::detail {

struct SquadronScan {
    EntityId target{};
    std::uint64_t next_scan_frame{};
};

// The squadron's attack target at world.frame (FT-01 to FT-04): the current one while it lives and
// its owner sees it; otherwise, on a scan frame, the best hostile unit by the leader's priority set
// within the chase range plus the leader's attack distance of `anchor`. Reads only `world`.
[[nodiscard]] SquadronScan squadron_target(const CombatWorld& world, const SquadronState& state,
    const SquadronProfile& squadron, const CombatUnit& leader, math::Vec3 anchor);

} // namespace eawr::sim::tactical::detail
