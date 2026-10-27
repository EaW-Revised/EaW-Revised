#pragma once

#include "eawr/sim/galactic/snapshot.hpp"
#include "eawr/sim/galactic/types.hpp"

#include <algorithm>

namespace eawr::sim::galactic::session_detail {

// The command kinds WGM-07 refuses while the galaxy is paused. `invasion` starts a battle, so it has no
// policy entry and is never admitted during a pause.
enum class PausedKind : std::uint8_t { fleet_management, surface_deploy, fleet_move, invasion };

[[nodiscard]] constexpr PausedAdmission paused_policy_for(const PausePolicy& policy, const PausedKind kind) noexcept {
    switch (kind) {
    case PausedKind::fleet_management: return policy.fleet_management;
    case PausedKind::surface_deploy: return policy.surface_deploy;
    case PausedKind::fleet_move: return policy.fleet_move;
    case PausedKind::invasion: break;
    }
    return PausedAdmission::refuse;
}

// The one pause predicate for the fleet/movement/landing kernels. Not paused admits as always. A paused
// galaxy refuses when a system hold is active (pending conflict, battle summary, child battle: whatever
// the policy says), when the issuer is not a human seat (an AI plan or script never acts during a pause),
// or when the policy for this kind is `refuse`. Only a pure player pause with an `admit` policy proceeds.
[[nodiscard]] constexpr bool paused_refusal(const GalacticClock& clock, const FleetControlSetup& control,
    const PausedKind kind, const bool system_hold, const bool human_issuer) noexcept {
    if (clock.pause_depth == 0) return false;
    if (system_hold || !human_issuer) return true;
    return paused_policy_for(control.pause, kind) == PausedAdmission::refuse;
}

// Owner-side wrapper: `pending_local_conflict` is the step's conflict input, `owner.system_hold` the
// hold derived from campaign ownership. Native journals preserve the effective gate for replay.
template<class Owner>
[[nodiscard]] bool owner_paused_refusal(const Owner& owner, const PausedKind kind, const bool pending_local_conflict,
    const bool human_issuer) {
    return paused_refusal(owner.clock, *owner.setup.fleet_control, kind, pending_local_conflict || owner.system_hold, human_issuer);
}

// The landing kernels are keyed by player: the policy applies when that player has an active human seat.
template<class Owner>
[[nodiscard]] bool native_human_issuer(const Owner& owner, const PlayerId player) {
    return std::any_of(owner.seats.begin(), owner.seats.end(), [player](const auto& row) {
        return row.player == player && row.active && row.kind == SeatKind::human;
    });
}

} // namespace eawr::sim::galactic::session_detail
