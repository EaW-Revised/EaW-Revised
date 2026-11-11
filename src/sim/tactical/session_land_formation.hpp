#pragma once

#include "session_land_internal.hpp"

#include <optional>
#include <span>
#include <utility>
#include <vector>

// The land session's side of a group move order (docs/behaviour/walks/land-pathing.md, LMF-01..LMF-08). The pure plan is
// land_plan_formations; these functions read the session's squads for it and carry its results out.
namespace eawr::sim::tactical::session_detail {

// The formation plan for the eligible squads of one move order, or nullopt (the caller keeps the per-squad rule).
// `ranges` is each squad's longest attack range (zero when it has none); the rest is read from the land session.
[[nodiscard]] std::optional<std::vector<LandFormationPlan>> plan_land_group_move(const LandSession& land,
    std::span<const EntityId> squads, math::Vec2 click, const std::map<EntityId, math::Fixed>& ranges);

// A squad leaves any pending formation order that names it (it was given another order). A lead's order is released as
// a whole: its other members search alone for their slots.
// `keep` names formation leads whose pending orders stay (the orders this very command is creating). `renewed` names the
// squads that the same command puts into new formations: a released lead leaves them alone (their field and search belong to
// the new order).
void release_land_formation_member(LandSession& land, EntityId squad, std::span<const EntityId> keep = {},
    std::span<const EntityId> renewed = {});

// A member that cannot use the shared route searches alone, for its slot beside the formation destination.
void begin_land_member_search(LandSession& land, EntityId squad, const LandFormationOrder& order, math::Vec2 slot);

// The centre path of a formation ended: each member gets the shared route at its slot. The result lists the members
// (squad, slot) whose route could not be built or installed; they search alone.
[[nodiscard]] std::vector<std::pair<EntityId, math::Vec2>> install_land_formation(LandSession& land,
    const LandFormationOrder& order, const LandPath& centre, std::uint64_t tick);
[[nodiscard]] core::Result<void> prepare_land_formation_speeds(LandSession& land, const PartitionExecutor& executor);

} // namespace eawr::sim::tactical::session_detail
