#pragma once

#include "session_types.hpp"

namespace eawr::sim::tactical::session_detail {

inline constexpr std::array<std::uint8_t, 4> manual_tag{'M', 'A', 'N', 'N'};

inline constexpr std::array<std::uint8_t, 8> state_magic{'E', 'A', 'W', 'R', 'T', 'S', 'T', 0};
// Tags of the optional state blocks that follow the units (#271, #274).
inline constexpr std::array<std::uint8_t, 4> squadron_tag{'S', 'Q', 'D', 'N'};
inline constexpr std::array<std::uint8_t, 4> fog_tag{'F', 'O', 'G', 'C'};
// The tracking layers' window anchors (#71); only with avoidance rules.
inline constexpr std::array<std::uint8_t, 4> tracking_tag{'T', 'R', 'A', 'K'};
inline constexpr std::array<std::uint8_t, 4> roll_tag{'R', 'O', 'L', 'L'};
// Projectiles in flight (#74); only with damage rules.
inline constexpr std::array<std::uint8_t, 4> projectile_tag{'P', 'R', 'O', 'J'};
// Group members waiting to plan (#344); only while any waits.
inline constexpr std::array<std::uint8_t, 4> formation_tag{'F', 'R', 'M', 'N'};
// Ships waiting for a sliced search to land (PC-08, #520); only while any waits.
inline constexpr std::array<std::uint8_t, 4> sliced_tag{'S', 'L', 'C', 'E'};
// Approach mappings (#452); only while any unit has one.
inline constexpr std::array<std::uint8_t, 4> approach_tag{'A', 'P', 'P', 'R'};
// Squadron flight, orders and hangars (#75); only with a squadron table that names them.
inline constexpr std::array<std::uint8_t, 4> craft_tag{'C', 'R', 'F', 'T'};
inline constexpr std::array<std::uint8_t, 4> squadron_state_tag{'S', 'Q', 'S', 'T'};
inline constexpr std::array<std::uint8_t, 4> hangar_tag{'H', 'N', 'G', 'R'};
// The target scans' collection trees (#469, space-targeting CO rules), once a unit with a combat
// profile has stepped.
inline constexpr std::array<std::uint8_t, 4> collection_tag{'C', 'U', 'L', 'L'};
inline constexpr std::array<std::uint8_t, 4> projectile_collection_tag{'C', 'O', 'L', 'L'}; // DG-30, reserved
// The decided battle (#77), in the state and the snapshot; only once decided. The block carries
// its own format version so an outcome can change shape without touching undecided hashes.
inline constexpr std::array<std::uint8_t, 4> victory_tag{'V', 'I', 'C', 'T'};
// Unit abilities (#76); only when a live unit's type has abilities.
inline constexpr std::array<std::uint8_t, 4> ability_tag{'A', 'B', 'I', 'L'};
// Ion stuns (#561); only while a live unit has one.
inline constexpr std::array<std::uint8_t, 4> ion_stun_tag{'I', 'O', 'N', 'S'};
inline constexpr std::array<std::uint8_t, 4> asteroid_tag{'A', 'S', 'T', 'D'}; // coordinator-reserved
inline constexpr std::array<std::uint8_t, 4> hazard_move_tag{'M', 'V', 'H', 'Z'}; // coordinator-reserved
// EN-08: coordinator-reserved, only while a unit has a temporary engine-disable deadline.
inline constexpr std::array<std::uint8_t, 4> engine_disable_tag{'E', 'D', 'I', 'S'};
inline constexpr std::array<std::uint8_t, 4> nebula_tag{'N', 'E', 'B', 'C'}; // coordinator-reserved
inline constexpr std::array<std::uint8_t, 4> storm_tag{'S', 'T', 'M', 'C'}; // coordinator-reserved
inline constexpr std::uint32_t victory_block_version = 1;
// Skirmish purchasing (#530): credits, queues and pools, only with economy rules; arrivals and
// population shares, only while any.
inline constexpr std::array<std::uint8_t, 4> economy_tag{'E', 'C', 'O', 'N'};
inline constexpr std::array<std::uint8_t, 4> arrival_tag{'A', 'R', 'R', 'V'};
inline constexpr std::array<std::uint8_t, 4> population_tag{'P', 'O', 'P', 'S'};
// Killed craft spinning away (#447), in the state and the snapshot; only while any spins.
inline constexpr std::array<std::uint8_t, 4> spin_tag{'S', 'P', 'I', 'N'};
inline constexpr std::array<std::uint8_t, 8> snapshot_magic{'E', 'A', 'W', 'R', 'T', 'S', 'N', 0};

void append_outcome(std::vector<std::uint8_t>& bytes, const BattleOutcome& outcome);

void append_fixed(std::vector<std::uint8_t>& bytes, const math::Fixed value);

void append_ledger(std::vector<std::uint8_t>& bytes, const PlayerEconomy& ledger);

void append_vec3(std::vector<std::uint8_t>& bytes, const math::Vec3& value);

void append_craft(std::vector<std::uint8_t>& bytes, const EntityId id, const CraftState& craft);

void append_spin(std::vector<std::uint8_t>& bytes, const DeathSpin& spin);

void append_squadron_state(std::vector<std::uint8_t>& bytes, const SquadronState& state);

void append_spawner(std::vector<std::uint8_t>& bytes, const EntityId id, const SpawnerState& state);

void append_motion(std::vector<std::uint8_t>& bytes, const MotionState& motion);

void append_nodes(std::vector<std::uint8_t>& bytes, const MotionState& motion);

} // namespace eawr::sim::tactical::session_detail
