#pragma once

#include "combat_internal.hpp"
#include "eawr/sim/tactical/land_terrain.hpp"

namespace eawr::sim::tactical::session_detail {

// LC-30/Q2: sourced opportunity cadence. Keep its land policy replaceable independently
// of the space service; the existing ship-slot key supplies deterministic scan jitter.
inline constexpr std::uint32_t land_opportunity_scan_frames = logical_frames_per_second;
inline constexpr std::uint32_t land_opportunity_scan_jitter = logical_frames_per_second / 2;
// Q1/Q3/Q4/Q5 are unqualified slice policies, not claims of retail equivalence.
inline constexpr bool land_share_leader_target = true;
inline constexpr bool land_attack_chases = false; // Q3: no auto-move until the attack move branch is sourced
inline constexpr bool land_free_occupancy_at_death = true; // Q4: corpse persistence is presentation only
inline constexpr bool land_origin_muzzle = true; // Q5: animated bone poses are not yet supplied

// LR2-02/12: object wins an exact tie; a ground terminal uses the shared expiry/blast path once.
[[nodiscard]] core::Result<detail::ProjectileStep> land_projectile_contact(
    const LandTerrain& terrain, detail::ProjectileStep flight, std::vector<math::Fixed>* crossings = nullptr);

} // namespace eawr::sim::tactical::session_detail
