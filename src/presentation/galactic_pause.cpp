#include "eawr/presentation/galactic_pause.hpp"

#include <algorithm>

namespace eawr::presentation {

bool galactic_pause_allows(const sim::galactic::GalacticSnapshot& snapshot,
    const sim::galactic::SeatId issuer, const GalacticPausedAction action, const bool system_hold) noexcept {
    namespace gc = sim::galactic;
    if (!snapshot.clock().pause_depth) return true;
    if (system_hold || !snapshot.fleet_control()) return false;
    const auto seats = snapshot.seats();
    const auto seat = std::find_if(seats.begin(), seats.end(), [issuer](const auto& row) { return row.seat_id == issuer; });
    if (seat == seats.end() || !seat->active || seat->kind != gc::SeatKind::human) return false;
    const auto& policy = snapshot.fleet_control()->pause;
    switch (action) {
    case GalacticPausedAction::fleet_management: return policy.fleet_management == gc::PausedAdmission::admit;
    case GalacticPausedAction::surface_deploy: return policy.surface_deploy == gc::PausedAdmission::admit;
    case GalacticPausedAction::fleet_move: return policy.fleet_move == gc::PausedAdmission::admit;
    case GalacticPausedAction::ability: return false;
    }
    return false;
}

bool galactic_departure_ready(const sim::galactic::GalacticSnapshot& snapshot,
    const sim::galactic::Fleet& fleet) noexcept {
    const auto& transit = fleet.motion.transit;
    return snapshot.clock().pause_depth && !fleet.orbit && transit && transit->hop == 0
        && transit->velocity == sim::math::Vec3{};
}

} // namespace eawr::presentation
