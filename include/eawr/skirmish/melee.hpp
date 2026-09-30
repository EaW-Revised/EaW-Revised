#pragma once

#include "eawr/core/result.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/units/unit_tables.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace eawr::skirmish {

// The close-range battle benchmark "melee" (#601, docs/performance/battle-bench.md): both M2 sides'
// capital ships and fighter/bomber squadrons in two lines across the M2 map's centre, within
// weapon range, with no station and no map object, as a replay built from a seed. Ship i of a side
// attacks the enemy ship at the same place in the placement order (scaled to the enemy's count)
// and every squadron attack-moves onto the enemy ships' centre, all at tick 0. path_bench --melee
// times it and the viewer plays it (--eawr-live-session melee); both bind it with the replay
// path's content (the M2 fog grid, the ability table and the fixture's victory rules).
//
// Size S is the largest real FoC space skirmish: each side at its highest population cap (about
// 42 Rebel and 37 Empire population: Rebel 11 ships + 10 squadrons, Empire 11 + 10). M doubles
// every count and L quadruples it, past the cap.
enum class MeleeSize : std::uint8_t { s, m, l };
[[nodiscard]] std::optional<MeleeSize> melee_size(std::string_view name) noexcept;
[[nodiscard]] std::string_view to_string(MeleeSize size) noexcept;

struct Melee final {
    sim::tactical::TacticalReplay replay;
    std::size_t ships{};
    std::size_t squadrons{};
    std::size_t craft{};
};

// The melee of `size` from `seed` over `ticks` ticks, with the players and content identity of
// `start` (the M2 start). Fails when the start has no Rebel and Empire lobby players or the tables
// lack a roster type.
[[nodiscard]] core::Result<Melee> build_melee(const units::UnitTables& tables, const SkirmishStart& start,
    MeleeSize size, std::uint64_t seed, std::uint64_t ticks);

} // namespace eawr::skirmish
