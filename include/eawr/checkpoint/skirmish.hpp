#pragma once

#include "eawr/checkpoint/checkpoint.hpp"
#include "eawr/checkpoint/tactical.hpp"
#include "eawr/core/result.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/script/authoritative/tactical_bridge.hpp"
#include "eawr/sim/tactical/replay.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

// A skirmish battle save (docs/checkpoint-format.md "Skirmish battle saves"): a tactical
// checkpoint of the battle (seats, child-log, child-state, land-state) plus the tactical-host
// section with the battle's script host: the Lua scheduler, the script commands routed to the
// next tick, the routing cursors and the native AI continuation. Loading rebuilds the world from
// child-state without re-simulating, then restores the host into a fresh scheduler.
namespace eawr::checkpoint {

inline constexpr std::uint16_t tactical_host_section_version = 1;

// Section 20: the encoding of a campaign child host's script part (the same field order and
// framing), without the campaign-only timeline, cursors and attribution.
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_tactical_host(
    const script::authoritative::TacticalScriptSave& saved);
// Validates the framing, the bounds and that the bytes are the canonical encoding.
[[nodiscard]] core::Result<script::authoritative::TacticalScriptSave> decode_tactical_host(
    std::span<const std::uint8_t> bytes, std::string_view logical_path = {});

// The identities of a build that runs skirmish battles of `content` with the script closure
// `scripts` (a digest of the AI's selected Lua and XML; zero for a battle without scripts).
[[nodiscard]] Identities skirmish_identities(const Digest& content, const Digest& scripts);
// The digest a hex SHA-256 string names (the AI closure); zero when malformed.
[[nodiscard]] Digest script_closure_digest(std::string_view closure_sha256_hex);

// Saves `session` between two ticks. The world's own pending queue becomes the child log's
// queued commands. Fails without output when the script host cannot save.
[[nodiscard]] core::Result<std::vector<std::uint8_t>> save_skirmish_checkpoint(
    script::authoritative::ScriptedTacticalSession& session, std::span<const Seat> seats, const Identities& build);

// The script host a load restores into: a fresh scheduler for the saved battle and the call that
// binds the host's engine and verbs after the restore (script::foc::prepare_session gives both).
struct SkirmishHost {
    script::authoritative::ScriptScheduler scripts;
    std::function<core::Result<void>(script::authoritative::ScriptedTacticalSession&)> bind;
};
// Built from the saved battle's log (setup and every command so far).
using SkirmishHostFactory = std::function<core::Result<SkirmishHost>(const sim::tactical::TacticalReplay&)>;

struct SkirmishRestore {
    script::authoritative::ScriptedTacticalSession session;
    std::vector<Seat> seats;
    Header header;
    RestoreWork work;
};

// Validates everything, restores the world from child-state and the host from tactical-host, and
// checks that the restored host saves back to the same bytes. Nothing outside the result is
// touched. A tactical checkpoint without the tactical-host section is refused.
[[nodiscard]] core::Result<SkirmishRestore> load_skirmish_checkpoint(std::span<const std::uint8_t> bytes,
    const Identities& build, const TacticalSessionFactory& world_factory, const SkirmishHostFactory& host_factory,
    const sim::PartitionExecutor& executor, std::string_view logical_path = {});

} // namespace eawr::checkpoint
