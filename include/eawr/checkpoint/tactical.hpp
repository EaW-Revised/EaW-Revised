#pragma once

#include "eawr/checkpoint/checkpoint.hpp"
#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/world.hpp"
#include "eawr/sim/seat.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

// The seats section and the child-log section, and a whole checkpoint of a tactical battle
// (docs/checkpoint-format.md).
namespace eawr::checkpoint {

// ADR-020: a seat is a controller seated in exactly one player; players own, seats command.
using sim::SeatId;
using sim::SeatKind;

inline constexpr std::uint16_t seats_section_version = 1;
// Controller capacity is a byte-budget bound, independent of faction/lobby policy.
inline constexpr std::size_t max_seats = (max_bytes - 4U) / 16U;
inline constexpr std::uint32_t no_lobby_slot = 0xffffffffU;

// Section 2 v1 DTO: 16 encoded bytes, with tactical side derived from the team.
// Galactic replay v1 uses its own 18-byte row with an explicit human side;
// shared identity/kind types do not change either format.
struct Seat {
    SeatId seat_id{};
    std::uint32_t player_id{};
    SeatKind kind{SeatKind::human};
    bool active{true};
    std::uint32_t lobby_slot{no_lobby_slot};
    friend bool operator==(const Seat&, const Seat&) = default;
};

// Fails on a seat ID that does not strictly increase, an unknown kind or more than max_seats.
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_seats(std::span<const Seat> seats);
[[nodiscard]] core::Result<std::vector<Seat>> decode_seats(std::span<const std::uint8_t> bytes,
    std::string_view logical_path = {});

// One seat per player of the setup, seat ID = player ID: human when the recorded lobby says so,
// AI otherwise. A tactical battle has no seats of its own; this is the one-seat-per-player case.
[[nodiscard]] std::vector<Seat> default_seats(const sim::tactical::TacticalSetup& setup);

inline constexpr std::uint16_t child_log_section_version = 1;
inline constexpr std::uint16_t child_state_section_version = 8; // Combined campaign and Skirmish continuation with retained squadron hardpoints and the station ship ledger (#3956), section 11
// The conflict record is the galactic session's (FT-08); the container bounds it only.
inline constexpr std::size_t max_conflict_record_bytes = 1U * 1024U * 1024U;

// A battle saved as its entry plus its commands, re-simulated on load. `log` holds the setup and
// every command the battle accepted, executed or still queued, in canonical order; its final tick
// is the completed tick, or one past the last queued command's tick when that is later.
struct ChildLog {
    std::vector<std::uint8_t> conflict_record; // empty for a battle outside a galaxy
    std::uint64_t completed_tick{};
    Digest state_sha256{}; // the battle's state hash at completed_tick
    sim::tactical::TacticalReplay log;
    friend bool operator==(const ChildLog&, const ChildLog&) = default;
};

[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_child_log(const ChildLog& child);
// Validates the framing, the embedded replay and that its final tick is the canonical one.
[[nodiscard]] core::Result<ChildLog> decode_child_log(std::span<const std::uint8_t> bytes,
    std::string_view logical_path = {});

// The child log of `session`. `queued` is every command the caller submitted and the session
// accepted but has not executed yet, in canonical order. Fails unless the supplied commands
// equal the session's pending queue, including every key and payload.
[[nodiscard]] core::Result<ChildLog> capture_child_log(const sim::tactical::TacticalSession& session,
    std::span<const sim::tactical::PlayerCommand> queued, std::vector<std::uint8_t> conflict_record = {});

// Builds a session from a replay with the content its setup names (sensors, durability, motion,
// fog, combat, victory, abilities, economy). Content is rebuilt by the loader, never saved.
using TacticalSessionFactory =
    std::function<core::Result<sim::tactical::TacticalSession>(const sim::tactical::TacticalReplay&)>;
// TacticalSession::from_replay without content tables: the way the replay corpus runs.
[[nodiscard]] TacticalSessionFactory content_free_factory();

// Deterministic restore work (never wall-clock): the ticks re-simulated and the bytes decoded.
struct RestoreWork {
    std::uint64_t resimulated_ticks{};
    std::uint64_t checkpoint_bytes{};
    friend bool operator==(const RestoreWork&, const RestoreWork&) = default;
};

// Re-simulates the battle to its completed tick into a new session and checks its state hash
// and queue against the saved ones.
[[nodiscard]] core::Result<sim::tactical::TacticalSession> resimulate_child_log(const ChildLog& child,
    const TacticalSessionFactory& factory, const sim::PartitionExecutor& executor,
    RestoreWork* work = nullptr, std::string_view logical_path = {});

// The identities of a build that runs tactical battles of `content` without Lua state.
[[nodiscard]] Identities tactical_identities(const Digest& content);

// A checkpoint of a battle outside a galaxy: the seats section and the child-log section.
[[nodiscard]] core::Result<std::vector<std::uint8_t>> save_tactical_checkpoint(
    const sim::tactical::TacticalSession& session, std::span<const sim::tactical::PlayerCommand> queued,
    std::span<const Seat> seats, const Identities& build);

struct TacticalRestore {
    sim::tactical::TacticalSession session;
    std::vector<Seat> seats;
    Header header;
    RestoreWork work;
};

// Validates everything and re-simulates into a new inactive session; nothing outside the result
// is touched.
[[nodiscard]] core::Result<TacticalRestore> load_tactical_checkpoint(std::span<const std::uint8_t> bytes,
    const Identities& build, const TacticalSessionFactory& factory, const sim::PartitionExecutor& executor,
    std::string_view logical_path = {});
// Validate-then-swap: replaces `running` only when the whole load succeeded; on any defect
// `running` is untouched and the diagnostic names the section.
[[nodiscard]] core::Result<void> restore_tactical_checkpoint(TacticalRestore& running,
    std::span<const std::uint8_t> bytes, const Identities& build, const TacticalSessionFactory& factory,
    const sim::PartitionExecutor& executor, std::string_view logical_path = {});

} // namespace eawr::checkpoint
