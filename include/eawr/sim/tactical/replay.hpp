#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::sim::tactical {

inline constexpr std::uint16_t replay_format_version = 2;
// Version 3 (#271) is version 2 plus a squadron table. A replay without squadrons is written
// as version 2, so every replay has exactly one encoding; the reader accepts both.
inline constexpr std::uint16_t replay_format_version_squadrons = 3;
// Coordinator-reserved versions: tagged header records extend v2/v3 without changing them.
inline constexpr std::uint16_t replay_format_version_extensions = 4;
inline constexpr std::uint16_t replay_format_version_squadron_extensions = 5;
inline constexpr std::size_t replay_header_size = 104;
inline constexpr std::size_t replay_policy_header_size = 116;
inline constexpr std::uint16_t replay_extension_match_policy = 1;
inline constexpr std::uint16_t replay_extension_skirmish_setup = 2; // coordinator-reserved SKSU
inline constexpr std::size_t replay_max_bytes = 256U * 1024U * 1024U;

// A fighter squadron (#271, docs/behaviour/space-visibility.md V-03): its team container,
// a unit of its own that carries the squadron's sensor, and its craft in strictly increasing
// ID. The container sits at the centre of its live craft's bounding box and leaves the
// session with its last craft.
struct Squadron {
    EntityId container{};
    std::vector<EntityId> members;
    friend bool operator==(const Squadron&, const Squadron&) = default;
};

// Recorded lobby settings are load-time metadata, never part of canonical simulation state.
struct ReplayMatchOptions final {
    bool allow_heroes{true}, allow_superweapons{true}, free_starting_units{true}, pre_built_base{true};
    bool allow_random_events{};
    math::Fixed credits{};
    std::int32_t start_tech{}, max_tech{}, game_timer{}, win_integer{}, auto_resolve{};
    math::Fixed win_float{};
    std::string win_condition, space_win_condition;
    bool operator==(const ReplayMatchOptions&) const = default;
};

struct ReplayLobbySlot final {
    PlayerId player{};
    bool human{};
    std::optional<std::uint32_t> colour_index;
    std::vector<std::string> fleet;
    bool operator==(const ReplayLobbySlot&) const = default;
};

struct ReplaySkirmishSetup final {
    std::string map, map_sha256;
    ReplayMatchOptions match;
    std::uint32_t victory_condition{};
    std::vector<ReplayLobbySlot> slots;
    bool operator==(const ReplaySkirmishSetup&) const = default;
};

// The tick-zero session: players and units in strictly increasing ID order. Setup units
// carry no order. Squadrons are in strictly increasing container ID; each container and
// craft is a setup unit of one owner, and no unit belongs to two squadrons.
struct TacticalSetup {
    std::uint64_t seed{};
    std::array<std::uint8_t, 32> content_identity{};
    std::vector<Player> players;
    std::vector<UnitState> units;
    std::vector<Squadron> squadrons;
    // Absent is the legacy all-enabled policy and retains v2/v3 bytes.
    std::optional<SkirmishMatchPolicy> match_policy;
    // Optional SKSU is attached by live recording callers; pinned builders retain old bytes.
    std::optional<ReplaySkirmishSetup> skirmish{};
    friend bool operator==(const TacticalSetup&, const TacticalSetup&) = default;
};

// Replay v2 (docs/replay-format.md; v3 with squadrons): a setup plus every recorded player
// command in strictly increasing (tick, player, sequence) order, each tick below
// final_tick_count.
struct TacticalReplay {
    TacticalSetup setup;
    std::uint64_t final_tick_count{};
    std::vector<PlayerCommand> commands;
    friend bool operator==(const TacticalReplay&, const TacticalReplay&) = default;
};

// Reads the shared EAWRPLY magic and format version without validating the rest, so a
// caller can route v1 and v2 files. Returns no value for short input or another magic.
[[nodiscard]] std::optional<std::uint16_t> peek_replay_format_version(
    std::span<const std::uint8_t> bytes) noexcept;

[[nodiscard]] core::Result<void> validate_setup(const TacticalSetup& setup);
[[nodiscard]] core::Result<void> validate_replay(const TacticalReplay& replay);
[[nodiscard]] core::Result<TacticalReplay> parse_replay(
    std::span<const std::uint8_t> bytes,
    std::string_view logical_path = {});
[[nodiscard]] core::Result<std::vector<std::uint8_t>> write_replay(const TacticalReplay& replay);

} // namespace eawr::sim::tactical
