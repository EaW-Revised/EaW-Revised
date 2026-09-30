#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Fixed-force victory and defeat (P2-14, #77; docs/behaviour/space-victory.md). The outcome is
// decided from battle state alone: which star bases stand when one is destroyed. Everything here
// is a pure function of plain values: no clock, thread or host input.
namespace eawr::sim::tactical {

// The lobby win condition (VT-01). `none` turns the evaluation off: a session without victory
// rules never decides an outcome and hashes as before #77.
enum class VictoryCondition : std::uint8_t {
    none = 0,
    // SKIRMISH_SPACE_ENEMY_STARBASE_DESTROYED, FoC's MP_Default_Space_Tactical_Win_Condition.
    enemy_starbase_destroyed = 1,
};

// The retail pending-victory countdown (VT-11): the battle ends 210 frames (7 s) after the
// destruction that decided it.
inline constexpr std::uint32_t victory_countdown_frames = 210;

// Content of the session's skirmish, like the sensor and durability tables: neither replay data
// nor state. All lists are strictly ascending.
struct VictoryRules {
    VictoryCondition condition{VictoryCondition::none};
    // The star base types that count (VT-03): BEHAVIOR_DUMMY_STAR_BASE types that are
    // Victory_Relevant in space.
    std::vector<TypeId> starbase_types;
    // Players of playable factions (the lobby players). Only their star bases count (VT-03) and
    // only they can win (VT-07).
    std::vector<PlayerId> contenders;
    // The human contenders (VT-05).
    std::vector<PlayerId> humans;
    std::uint32_t countdown_frames{victory_countdown_frames};
    friend bool operator==(const VictoryRules&, const VictoryRules&) = default;
};

// A standing star base at the moment one is destroyed.
struct StarbaseEntry {
    EntityId unit{};
    PlayerId owner{};
    friend constexpr bool operator==(const StarbaseEntry&, const StarbaseEntry&) noexcept = default;
};

// The decided battle: once set it never changes (VT-09). Hashed state and snapshot data.
struct BattleOutcome {
    VictoryCondition condition{VictoryCondition::none};
    PlayerId winner{};
    TeamId winner_team{};
    std::uint64_t decided_tick{};  // the frame of the deciding destruction
    EntityId deciding_unit{};      // the star base whose destruction decided it
    std::uint64_t end_tick{};      // decided_tick + countdown: when retail ends the battle
    friend constexpr bool operator==(const BattleOutcome&, const BattleOutcome&) noexcept = default;
};

// Fails on unsorted or duplicate lists, a human who is not a contender, a contender or human
// that is not a setup player, a non-commandable contender, or a countdown past the replay tick limit.
[[nodiscard]] core::Result<void> validate_victory(const VictoryRules& rules, std::span<const Player> players);

// VT-04 to VT-08: the winner decided by the destruction of `owner`'s star base, or nothing.
// `remaining` holds the star bases standing at that moment, the destroyed one excluded. Each
// commandable contender that is on another team than `owner`, in ascending ID, is tested in turn;
// the first that satisfies the condition wins. A non-contender owner's loss decides nothing.
[[nodiscard]] std::optional<PlayerId> starbase_destroyed_winner(
    const VictoryRules& rules, std::span<const Player> players, PlayerId owner,
    std::span<const StarbaseEntry> remaining);

[[nodiscard]] std::string_view to_string(VictoryCondition condition) noexcept;

} // namespace eawr::sim::tactical
