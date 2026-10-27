#pragma once

#include "eawr/sim/galactic/snapshot.hpp"

namespace eawr::presentation {

enum class GalacticPausedAction { fleet_management, surface_deploy, fleet_move, ability };

// UI-07 / WGM-07: preview the published pause policy; command admission remains authoritative.
[[nodiscard]] bool galactic_pause_allows(const sim::galactic::GalacticSnapshot&,
    sim::galactic::SeatId, GalacticPausedAction, bool system_hold) noexcept;

// A paused departure has not begun translating along its first hop (WGM-28/29).
[[nodiscard]] bool galactic_departure_ready(const sim::galactic::GalacticSnapshot&,
    const sim::galactic::Fleet&) noexcept;

} // namespace eawr::presentation
