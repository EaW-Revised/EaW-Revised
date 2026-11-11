#pragma once

#include "eawr/checkpoint/tactical.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/mods/save_description.hpp"

namespace eawr::checkpoint {

// Galaxy v19 retains live ability and representation lifecycle alongside tactical built records.
// v25 (coordinator-reserved, WGEP-163..165): the income section retains source-scoped income blockers
// and the authored credit-halt profile; native effect 18 journals their writes.
// v26 (coordinator-reserved, #2912): conflicts retain their corruption request; native effect 16 journals
// corruption lifecycle writes.
// v27 (coordinator-reserved, ruins adapter #2928): native effect 17 and the authored ruined-income inputs.
// v28 (coordinator-reserved, hacking #2946): the hack service state and authored hack constants.
// v29 (coordinator-reserved, destruction #2948): destruction service state, contested-space flag and routing names.
// v30 (coordinator-reserved, GHUD-S61, WGP-97..103): the campaign-scoring section retains the
// raw build/death/planet-change counters and the quit table the authored scoring module reads.
// v32: complete direct owner/barrier payload; recordings retain history only.
// v33 (#4058): the owner keeps only the native journal's length (the recording carries its entries), and a
// production bill journals just its outcome (build, accepted, payer, charge).
// v34 (#4058): a save keeps the history since the previous save or load. The session section carries the history base
// (boundary tick, chained frontier digest, last sealed key, dropped counts); the frontier input digest is
// sha256(previous digest || replay window bytes), never a digest of the window alone.
// v48 (#2044): the owner tail ends with the holocron story goals and objectives per plot owner, after the plot state.
// v49: agreed resume metadata, producer continuation and complete policy site archives.
// v53: authored corruption-removal profiles, planet costs and live enabled/cancelled service state.
// v54: ordered market-item ability identities.
// v56: the production offer tables are stored once, in the required offer-pool section; the session recording, the
// production state and the owner barrier carry one table index per planet (docs/checkpoint-format.md, Offer pool).
inline constexpr std::uint16_t galaxy_section_version = 56;
inline constexpr std::uint16_t mod_list_section_version = 1;

// Transport starts a fresh epoch at completed_tick. Both cursors denote the first
// unsealed/unsent tick; producer sequences continue by logical seat, never peer index.
struct GameResumeMetadata {
    std::uint64_t completed_tick{}, next_send{}, next_seal{}, hash_tick{};
    Digest capture_commitment{}, state_digest{}, frontier_digest{}, monitor_digest{};
    std::vector<std::uint8_t> network_state;
    std::vector<sim::galactic::GalacticPlayer> players;
    std::vector<sim::galactic::Seat> seats;
    std::optional<sim::galactic::SeatControlState> control;
    std::map<sim::SeatId, std::uint64_t> producer_sequences;
};
[[nodiscard]] core::Result<GameResumeMetadata> inspect_game_resume(const Checkpoint&);
// Original recording setup for private mounted-service preparation, without a restore.
[[nodiscard]] core::Result<sim::galactic::GalacticSetup> inspect_game_setup(const Checkpoint&);
[[nodiscard]] core::Result<mods::ModDescription> inspect_game_mods(const Checkpoint&);
// The battle-vote timeout a multiplayer save recorded, in galactic ticks (#6156); empty for a game without voting. Read
// from the session section's prologue, without restoring anything.
[[nodiscard]] core::Result<std::optional<std::uint64_t>> inspect_game_vote_timeout(const Checkpoint&);

[[nodiscard]] Identities game_identities(const sim::galactic::GalacticSetup& setup);
[[nodiscard]] core::Result<std::vector<std::uint8_t>> save_game_checkpoint(
    const campaign::GameSession& session, const Identities& build,
    std::span<const std::uint8_t> presentation_hints = {},
    const mods::ModDescription& mod_description = {});

struct GameLoadOptions {
    mods::ModDescription mounted_mods;
    // Reinstall pure runtime adapters before direct owner import. Never run new-game placement.
    std::function<core::Result<void>(campaign::GameSession&)> configure_session;
    // Space assembly callbacks. Complete providers for other layers register
    // through configure_session before owner/child reconstruction.
    campaign::GameSession::ContentResolver content;
    campaign::GameSession::ScriptFactory scripts;
    campaign::ScriptRequirement script_requirement{campaign::ScriptRequirement::required};
    campaign::GameSession::ResultApplier apply_result;
    campaign::AutoResolveOutcomeServices auto_resolve_outcomes;
    campaign::GameSession::ChildConfigurator configure_child;
    campaign::GameSession::AutoResolver auto_resolve;
    campaign::GameSession::VoteProgress vote_progress;
    campaign::GameSession::ConflictGate conflict_gate;
    campaign::GameSession::ConflictService conflict_service;
    // Fresh schedulers include pinned modules/bindings; configure reinstalls pure translators/engines.
    using GalacticSchedulers = std::pair<script::authoritative::ScriptScheduler, script::authoritative::ScriptScheduler>;
    std::function<core::Result<GalacticSchedulers>()> galactic_scripts;
    // Bindings must precede Lua restoration; this per-load closure binds their
    // exact native owners only after world/child/Lua reconstruction (WGA-56).
    struct PreparedGalacticScripts {
        GalacticSchedulers schedulers;
        std::function<core::Result<void>(campaign::GameSession&)> configure_native_ai;
        campaign::GameSession::ScriptFactory child_scripts;
        campaign::GameSession::ChildConfigurator configure_child;
        campaign::GalacticParentQueryFactory child_parent_queries{};
    };
    std::function<core::Result<PreparedGalacticScripts>()> prepare_galactic;
    std::function<core::Result<void>(script::authoritative::ScriptedGalacticSession&)> configure_galactic;
    // Fresh native adapters, after world/child/Lua/return reconstruction. This
    // callback must not initialize goals or step the AI; restore follows it.
    std::function<core::Result<void>(campaign::GameSession&)> configure_native_ai;
    // Saved session length bound, independent of the byte budget; direct restore never steps.
    std::uint64_t max_restore_ticks{sim::galactic::galactic_max_ticks};
    bool player_load{}; // false for exact runtime/resync restoration
    bool reserved_autosave{}; // WGS-64 presentation recovery, only for a player load
};
struct GameRestore {
    campaign::GameSession session;
    Header header;
    RestoreWork work;
    std::vector<std::uint8_t> presentation_hints;
    std::vector<sim::galactic::GameCommand> first_commands;
    std::vector<mods::DescriptionFinding> mod_findings;
};

[[nodiscard]] core::Result<GameRestore> load_game_checkpoint(std::span<const std::uint8_t> bytes,
    const Identities& build, const GameLoadOptions& options, const sim::PartitionExecutor& executor,
    std::string_view logical_path = {});
// Development equality oracle for the current format, never selected by production loads.
// A save whose history starts at an earlier save needs that save as `base` (its window is replayed on top of the base
// state, never from tick 0); a save from a new game needs none.
[[nodiscard]] core::Result<GameRestore> replay_game_checkpoint_for_testing(std::span<const std::uint8_t> bytes,
    const Identities& build, const GameLoadOptions& options, const sim::PartitionExecutor& executor,
    std::span<const std::uint8_t> base = {});
// Any decode, identity, section, reconstruction or fixup failure leaves running untouched.
[[nodiscard]] core::Result<void> restore_game_checkpoint(GameRestore& running,
    std::span<const std::uint8_t> bytes, const Identities& build, const GameLoadOptions& options,
    const sim::PartitionExecutor& executor, std::string_view logical_path = {});

// WGS-34: only the initiating local seat retains its requested slot. This callback projection
// never enters authoritative state; every peer sees the same save event.
[[nodiscard]] std::int64_t local_save_slot(const sim::galactic::GalacticEvent& event,
    sim::galactic::SeatId local_seat) noexcept;

} // namespace eawr::checkpoint
