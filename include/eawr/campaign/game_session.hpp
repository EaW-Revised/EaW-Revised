#pragma once

#include "eawr/core/result.hpp"
#include "eawr/campaign/conflict.hpp"
#include "eawr/campaign/conflict_entry.hpp"
#include "eawr/campaign/auto_resolve_outcome.hpp"
#include "eawr/campaign/auto_resolve_progress.hpp"
#include "eawr/campaign/conflict_policy.hpp"
#include "eawr/campaign/tactical_return.hpp"
#include "eawr/campaign/galactic_ai_services.hpp"
#include "eawr/script/authoritative/tactical_bridge.hpp"
#include "eawr/script/authoritative/galactic_bridge.hpp"
#include "eawr/sim/galactic/commands.hpp"
#include "eawr/sim/galactic/replay.hpp"
#include "eawr/sim/galactic/session.hpp"
#include "eawr/sim/galactic/snapshot.hpp"
#include "eawr/sim/galactic/story_engine.hpp"
#include "eawr/sim/galactic/story_rewards.hpp"
#include "eawr/campaign/galactic_story_scripts.hpp"
#include "eawr/campaign/story_notices.hpp"
#include "eawr/sim/state_hash.hpp"
#include "eawr/sim/world.hpp"

#include "eawr/script/numeric/lua_number.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <map>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// The game session (ADR-012, ADR-014; docs/galactic-architecture.md section 3): the one component
// that knows both simulation layers. It owns the galaxy and its optional scripted child battle;
// one session tick runs exactly one layer (WGT-01).
namespace eawr::campaign {

enum class SessionMode : std::uint8_t {
    galaxy = 1, // the galaxy ran this tick
    battle = 2, // a child battle ran this tick (tactical nesting)
};

// WGA-375..384: stable mode identity and copied retained publication.
struct ModeId {
    SessionMode mode{SessionMode::galaxy};
    sim::EntityId conflict{};
    friend bool operator==(const ModeId&, const ModeId&) = default;
};
struct ModeTimelineSnapshot {
    ModeId mode;
    std::uint64_t completed_tick{};
    std::int32_t value{};
};
struct ModeTimelineState {
    std::int32_t galaxy{};
    std::optional<ModeTimelineSnapshot> historical_child;
};

struct BattleChoiceResponse {
    sim::EntityId conflict{};
    sim::SeatId seat{};
    std::string refusal;
};
// GCMP-04: the executed outcome of an InvasionAccept command, in execution order. `summary_closed` is
// true when the accepting seat's own battle-summary hold ended with the acceptance.
struct InvasionAcceptResponse {
    sim::EntityId conflict{};
    sim::SeatId seat{};
    std::string refusal;
    bool summary_closed{};
};
struct PendingConflictAlert {
    sim::galactic::PlayerId invader{}, defender{};
    bool land{};
};
// Publication-only correlation for a local producer. Gameplay keys are still derived from the authenticated seat.
struct TacticalInputExecution {
    sim::SeatId seat{};
    std::uint64_t producer_sequence{};
    std::uint64_t tactical_tick{};
};
struct GameTick {
    std::uint64_t completed_tick{};
    SessionMode mode{SessionMode::galaxy};
    // Empty when the session hashes off the stepping thread; `state_hash` always has the digest.
    std::string state_sha256;
    sim::StateHash state_hash;
    std::shared_ptr<const sim::galactic::GalacticSnapshot> galaxy;
    sim::galactic::GalacticTick::Work galaxy_work;
    std::shared_ptr<const sim::tactical::TacticalSnapshot> battle;
    std::vector<BattleSeat> battle_seats; // current conflict-scoped bindings, including observers
    sim::StateHash battle_state_hash;
    std::vector<sim::tactical::AsteroidImpact> asteroid_impacts;
    std::vector<sim::tactical::DamageChange> damage_changes; // transient child presentation facts, not campaign state
    std::optional<BattleResult> result;
    std::vector<core::Diagnostic> diagnostics;
    std::vector<BattleChoiceResponse> battle_choice_responses;
    std::vector<InvasionAcceptResponse> invasion_accept_responses;
    // Save callbacks are emitted at the command boundary, before invasion mutation, in both
    // layers. They survive the following galaxy/child snapshot publication.
    std::vector<sim::galactic::GalacticEvent> save_events;
    std::vector<sim::galactic::GalacticEvent> conflict_events;
    std::vector<GameStoryNotice> story_notices;
    // WST-20: this step's triggered plot events with their rewards, in trigger order, each with the outcome of
    // its application (#683: engine-run graph rewards, the bound reward provider, or a named refusal).
    std::vector<sim::galactic::story::Triggered> story_triggers;
    [[nodiscard]] std::vector<GameStoryNotice> observer_story_notices(sim::galactic::PlayerId) const;
    // WGP-26: retain emission context even when this step selects or retires the conflict.
    std::map<sim::EntityId, PendingConflictAlert> pending_alerts;
    std::vector<TacticalInputExecution> tactical_input_executions{};
    std::vector<sim::galactic::HeroReturnView> hero_returns; // WGUI-89: canonical humans; platform filters per requested seat.
};

struct RecordedConflict {
    std::uint64_t entry_tick{}; // galaxy tick whose ordered commit selects the child
    ConflictRecord conflict;
    friend bool operator==(const RecordedConflict&, const RecordedConflict&) = default;
};
struct RecordedAdmission {
    std::uint64_t entry_tick{}; // named conflict-service barrier after this galaxy commit
    ConflictRecord conflict;
    Arbitration arbitration;
    friend bool operator==(const RecordedAdmission&, const RecordedAdmission&) = default;
};
// #4058: a save keeps only the history since the previous save or load. The boundary it starts at carries the
// chained frontier: input_digest(k) = sha256(input_digest(k-1) || canonical bytes of the window), zero at a new game.
using HistoryDigest = std::array<std::uint8_t, 32>;
struct HistoryBoundary {
    std::uint64_t tick{};   // completed tick of the boundary; 0 for a new game
    HistoryDigest digest{}; // chained input digest through the boundary
    std::optional<sim::galactic::CommandOrderKey> last_key; // last sealed command (executed or pending) through it
    friend bool operator==(const HistoryBoundary&, const HistoryBoundary&) = default;
};
struct HistoryBase {
    HistoryBoundary at;
    // Counts keep command limits and the absolute native sequence identical to an unbounded session.
    std::uint64_t executed_dropped{}; // session-level executed commands before the boundary
    std::uint64_t native_dropped{};   // applied native journal entries before the boundary
    // Tactical envelopes dropped before the boundary, per conflict still unfinished (child cursor sanity bound).
    std::map<sim::EntityId, std::uint64_t> child_attempts;
    friend bool operator==(const HistoryBase&, const HistoryBase&) = default;
};

// The command envelope plus deterministic conflict-service selections. The flat EAWRGCR v1
// stream has no conflict-entry section yet; checkpoint/container owners serialize this seam.
struct GameRecording {
    sim::galactic::GalacticReplay commands;
    std::vector<RecordedConflict> conflicts;
    // Internal load prologues execute before player commands at their tick. They neither
    // advance time nor consume a seat's submission cursor.
    std::vector<sim::galactic::GameCommand> load_fixups{};
    std::vector<RecordedAdmission> admissions{};
    bool conflict_services{}; // opt-in ordered tail/timer context, even before the first arrival
    // The history in `commands` (numbered from native sequence 1) is the window after this boundary.
    HistoryBase base{};
    std::optional<std::uint64_t> multiplayer_vote_timeout{};
};
// The frontier a recording seals: chained over its base and the canonical replay bytes of its window; the key is the
// later of the base's last sealed command and the window's last command.
[[nodiscard]] core::Result<HistoryBoundary> history_frontier_of(const GameRecording& recording,
    std::span<const std::uint8_t> replay_bytes, std::uint64_t completed_tick);

struct BattleCheckpoint {
    std::vector<std::uint8_t> state;
    script::authoritative::TacticalScriptSave scripts;
    std::map<sim::tactical::PlayerId, std::uint64_t> sequences;
    std::map<sim::EntityId, sim::tactical::PlayerId> killers;
    std::int32_t timeline{};
    // Parent tick (completed) at which the child timeline was last serviced; it lags the parent
    // across save-only and paused barriers, so a restore cannot derive it from the parent clock.
    std::uint64_t timeline_tick{};
};
// WGS-P06: immutable, seat-approved battle perspective; observers have no command sink.
struct SeatBattleView {
    std::uint64_t completed_tick{};
    sim::tactical::PlayerId perspective{};
    bool commandable{};
    std::vector<sim::tactical::TacticalInstance> instances;
};
// Pure presentation boundary; works on published LiveGameSession ticks without a live owner.
[[nodiscard]] core::Result<SeatBattleView> seat_battle_view(const GameTick& tick, sim::SeatId seat);

using GalacticParentQuery = std::function<std::optional<script::numeric::LuaNumber>(std::string_view, sim::tactical::PlayerId)>;
using GalacticParentQueryFactory = std::function<GalacticParentQuery(const ConflictRecord&)>;
inline constexpr std::string_view missing_conflict_scripts = "EAWR-CONFLICT-MISSING-SCRIPTS";
inline constexpr std::string_view unavailable_conflict_tactical = "EAWR-CONFLICT-TACTICAL-UNAVAILABLE";

class GameSession final {
public:
    GameSession(GameSession&&) noexcept;
    GameSession& operator=(GameSession&&) noexcept;
    ~GameSession();

    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;

    // Installed once on a fresh owner or after inactive checkpoint reconstruction.
    // Native state is restored after world/child/Lua/return identities exist and
    // before the first resumed service; the completed boundary is verified.
    [[nodiscard]] core::Result<void> set_galactic_ai_services(GalacticAiServices);
    // Fresh/restore owner barrier only. Gives the factory the actual retained
    // parent so production handlers never bind a detached campaign copy.
    using GalacticAiFactory = std::function<core::Result<GalacticAiServices>(sim::galactic::GalacticSession&)>;
    [[nodiscard]] core::Result<void> configure_galactic_ai_services(GalacticAiFactory);
    // Disk reconstruction installs typed native-effect owners without creating
    // or servicing an AI engine. Runtime bindings only, not retained state.
    using NativeReplayConfigurator = std::function<core::Result<void>(sim::galactic::GalacticSession&)>;
    [[nodiscard]] core::Result<void> configure_native_replay_owners(NativeReplayConfigurator);
    // Suppress external historical story delivery, including logs with no native effects.
    void begin_native_reconstruction() noexcept;
    [[nodiscard]] core::Result<void> finish_native_reconstruction(const sim::PartitionExecutor&);

    [[nodiscard]] core::Result<GalacticAiSave> save_galactic_ai() const;
    // Read only at an inactive owner boundary; does not capture or service AI.
    [[nodiscard]] bool native_ai_installed() const noexcept;
    [[nodiscard]] core::Result<void> restore_galactic_ai(const GalacticAiSave&);
    [[nodiscard]] core::Result<void> restore_planet_access(std::span<const std::uint8_t>);

    [[nodiscard]] static core::Result<GameSession> create(const sim::galactic::GalacticSetup& setup);
    // Flat command-envelope replay for a galaxy-only session. With conflict entries or load prologues, use
    // from_recording: the command envelope alone does not recreate the entry service.
    [[nodiscard]] static core::Result<GameSession> from_replay(const sim::galactic::GalacticReplay& replay);
    using ContentResolver = std::function<core::Result<sim::tactical::TacticalContent>(const ConflictRecord&)>;
    using EntryResolver = std::function<core::Result<ResolvedConflictEntry>(
        const ConflictRecord&, const sim::galactic::GalacticSnapshot&, sim::EntityId)>;
    // Space assembly seams for synthetic owners; a complete layer install seals these setters.
    void set_entry_resolver(EntryResolver resolver);
    using ScriptFactory = std::function<core::Result<script::authoritative::ScriptScheduler>(const ConflictRecord&)>;
    // Resolvers rebuild pinned content/scripts without embedding content tables in replay state.
    [[nodiscard]] static core::Result<GameSession> from_recording(const GameRecording& recording,
        ContentResolver content, ScriptFactory scripts = {}, ScriptRequirement requirement = ScriptRequirement::required);
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> save_checkpoint_state() const;
    [[nodiscard]] core::Result<void> load_checkpoint_state(std::span<const std::uint8_t>);
    using AutoResolveStateWriter = std::function<core::Result<std::vector<std::uint8_t>>()>;
    using AutoResolveStateReader = std::function<core::Result<void>(std::span<const std::uint8_t>,
        const ConflictRecord&, const sim::galactic::GalacticSnapshot&)>;
    // Stateful resolver adapters provide their own bounded owner codec; pure callbacks need none.
    void set_auto_resolve_checkpoint_services(AutoResolveStateWriter, AutoResolveStateReader);

    // Install fresh pinned schedulers, or restore their barrier save after galaxy reconstruction.
    [[nodiscard]] core::Result<void> install_galactic_scripts(script::authoritative::ScriptScheduler ai,
        script::authoritative::ScriptScheduler story,
        const std::optional<script::authoritative::GalacticScriptSave>& saved = {});
    [[nodiscard]] script::authoritative::ScriptedGalacticSession* galactic_scripts() noexcept;
    // The Lua side of the plot engine (WST-53/55): the plot engine's per-event callbacks are forwarded to the
    // sub-plot scripts of the installed story scheduler. Absent: the callbacks are dropped, as before.
    void bind_story_scripts(std::shared_ptr<const GalacticStoryScripts>) noexcept;
    [[nodiscard]] core::Result<std::optional<script::authoritative::GalacticScriptSave>> save_galactic_scripts() const;

    // Every player, AI, script and test input enters here (ADR-015). Session and galactic layers
    // go to the galaxy; a tactical command needs a running battle, so without one it is refused.
    [[nodiscard]] core::Result<void> submit(const sim::galactic::GameCommand& command);
    // Multiplayer setup keeps session ticks alive while agreed pause controls freeze the child and its scripts.
    // The saved setup, rather than a caller flag or local clock, determines this on replay and restore as well.
    [[nodiscard]] core::Result<GameTick> step(const sim::PartitionExecutor& executor);
    // The child owns one nested galactic pause level; only levels above that pause the child itself.
    [[nodiscard]] bool child_paused() const noexcept;
    // Ordered conflict-service seam: stage an already selected entry for the next galaxy commit.
    // This is not a player-input path. The caller supplies immutable content matching its identity.
    [[nodiscard]] core::Result<void> stage_battle(ConflictRecord record, const sim::tactical::TacticalContent& content,
        script::authoritative::ScriptScheduler scripts);
    // An explicit scriptless synthetic stage cannot bypass an installed layer's requirement.
    [[nodiscard]] core::Result<void> stage_battle(ConflictRecord record, const sim::tactical::TacticalContent& content,
        ScriptRequirement requirement = ScriptRequirement::required);
    using ResultApplier = std::function<core::Result<void>(sim::galactic::GalacticSession&, const BattleResult&)>;
    // Layer returns receive the admitted conflict. Directly staged automatic outcomes
    // supply its identity, planet, layer and participants without a tactical roster.
    using LayerResultApplier = std::function<core::Result<void>(sim::galactic::GalacticSession&,
        const ConflictRecord&, const BattleResult&)>;
    using ResultDecorator = std::function<core::Result<void>(const ConflictRecord&,
        const sim::tactical::TacticalSession&, BattleResult&)>;
    using HeroDefeatResolver = std::function<core::Result<std::optional<sim::galactic::HeroDefeat>>(
        const ConflictRecord&, const ConflictForce&, const ForceResult&, const sim::galactic::GalacticSnapshot&)>;
    void set_hero_defeat_resolver(HeroDefeatResolver resolver);
    // WGCH-76/WGM-78..80: ordinary death/action owners supply a qualified defeat;
    // this owning boundary schedules returns and tests leaders even on refusal.
    [[nodiscard]] core::Result<sim::galactic::HeroSchedule> schedule_hero_defeat(
        const sim::galactic::HeroDefeat&, const sim::PartitionExecutor&);
    void set_hero_placement(sim::galactic::HeroPlacement placement);
    struct LiveHeroReturns {
        std::function<core::Result<bool>(sim::galactic::GalacticSession&,
            const sim::galactic::HeroReturnRecord&, sim::galactic::PlanetId, const sim::PartitionExecutor&)> place;
        std::function<core::Result<sim::galactic::HeroStructureState>(sim::galactic::GalacticSession&,
            const sim::galactic::HeroReturnRecord&, const sim::PartitionExecutor&)> structures;
    };
    // Fresh and restored owners bind providers; callbacks receive the current world and tick executor.
    void set_live_hero_returns(LiveHeroReturns services);
    // #683: the plot rewards the engine queued (WST-40/41/64) are applied once per galaxy step after the hosted work
    // (GFS-15), inside the tick executor. The provider owns the stock bindings; a reward it declines, or that no
    // provider is bound for, is reported by name and never dropped silently. A step that replays the native journal
    // applies nothing: the journal carries the effects.
    struct StoryRewardOutcome { bool applied{}; std::string note; };
    using StoryRewardApplier = std::function<core::Result<StoryRewardOutcome>(
        sim::galactic::GalacticSession&, const sim::galactic::story::Triggered&, const sim::PartitionExecutor&)>;
    void set_story_rewards(StoryRewardApplier);
    [[nodiscard]] const BattleResult* latest_tactical_result() const noexcept;
    // Recorded solo presentation policy; configure before the live owner starts.
    [[nodiscard]] core::Result<void> set_interactive_seat(sim::galactic::SeatId);
    [[nodiscard]] sim::EntityId battle_summary_hold() const noexcept;
    [[nodiscard]] const ConflictRecord* latest_tactical_context() const noexcept;
    [[nodiscard]] core::Result<void> restore_latest_tactical_result(ConflictRecord context, BattleResult result);
    void clear_latest_tactical_result() noexcept;
    [[nodiscard]] const TacticalReturnState& return_state() const noexcept;
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> save_return_state() const;
    [[nodiscard]] core::Result<void> restore_return_state(std::span<const std::uint8_t> bytes);
    using ChildConfigurator = std::function<core::Result<void>(script::authoritative::ScriptedTacticalSession&)>;
    // The return service installs this at creation; it is called once at the child-ending barrier.
    void set_result_applier(ResultApplier applier);
    [[nodiscard]] core::Result<void> stage_auto_resolve_outcome(const AutoResolveOutcomeApplication& application);
    void set_auto_resolve_outcome_services(AutoResolveOutcomeServices services);
    // WAR-54/63: chronological runtime history, never checkpointed or hashed.
    [[nodiscard]] std::vector<AutoResolveOutcomeApplication> auto_resolve_history() const;
    void clear_auto_resolve_history() noexcept;
    [[nodiscard]] std::shared_ptr<const AutoResolveOutcomeApplication> latest_auto_resolve_outcome() const noexcept;
    using AutoResolveProgressReader = std::function<std::optional<AutoResolveProgress>()>;
    void set_auto_resolve_progress_reader(AutoResolveProgressReader reader);
    using AutoResolveWithdrawal = std::function<bool(sim::EntityId, sim::galactic::PlayerId)>;
    void set_auto_resolve_withdrawal(AutoResolveWithdrawal withdraw);
    [[nodiscard]] std::optional<AutoResolveProgress> auto_resolve_progress() const;
    [[nodiscard]] bool auto_resolve_needs_service() const noexcept;
    void set_control_effects_handler(sim::galactic::GalacticSession::ControlEffectsHandler handler);
    [[nodiscard]] core::Result<void> change_actor_current_type(sim::EntityId, sim::galactic::TypeId);
    [[nodiscard]] core::Result<void> set_ability_lifecycle(sim::EntityId, bool enabled, bool cancelled, sim::galactic::AbilityKind = sim::galactic::AbilityKind::black_market);
    using StoryNoticeHandler = std::function<core::Result<void>(const GameStoryNotice&)>;
    // WGM-192 / WAR-59: synchronous native producers; the plot engine owns reduction.
    void set_story_notice_handler(StoryNoticeHandler);
    // Authored content, rebuilt on load; no fixture names in runtime paths.
    void set_story_identities(std::map<sim::galactic::PlanetId, std::string> planets,
        std::map<sim::galactic::PlayerId, std::string> factions);
    void set_technology_notice_handler(sim::galactic::GalacticSession::TechnologyNoticeHandler);
    // WGS-07/WGS-140: install the loaded plots before the first step (or before a checkpoint restore
    // fills the saved state). Fails when plots are already installed or the session has stepped.
    // `notes` (the loader's skipped files and dropped events) and the engine's own refusals are published once,
    // one line each, as diagnostics of the first step.
    [[nodiscard]] core::Result<void> install_story(sim::galactic::story::Config, std::vector<sim::galactic::story::PlotRecord>,
        std::vector<sim::galactic::story::SubPlotDef>, std::vector<std::string> notes = {});
    [[nodiscard]] const sim::galactic::story::Engine* story_engine() const noexcept;
    void set_feedback_notice_handler(sim::galactic::GalacticSession::FeedbackNoticeHandler);
    [[nodiscard]] core::Result<sim::math::Fixed> set_technology_price_modifier(const sim::galactic::GalacticSession::TechnologyPriceWrite&);
    // WGEP-05/163..165 native credit-halt sabotage writer and explicit source removal.
    [[nodiscard]] core::Result<sim::galactic::GalacticSession::IncomeBlockerReceipt> halt_planet_credits(
        sim::EntityId actor, sim::galactic::PlanetId planet);
    [[nodiscard]] core::Result<bool> remove_income_blocker(sim::galactic::PlanetId planet, sim::EntityId source);
    [[nodiscard]] core::Result<std::int64_t> set_technology(const sim::galactic::GalacticSession::TechnologyWrite&);
    [[nodiscard]] core::Result<std::int64_t> increment_technology(sim::galactic::PlayerId);
    // WST-64 (#683): the journaled production unlock/lock writer and its faction post-check.
    [[nodiscard]] core::Result<sim::galactic::GalacticSession::ProductionLockReceipt> apply_production_lock(
        const sim::galactic::GalacticSession::ProductionLockWrite&);
    void set_movement_access_provider(sim::galactic::GalacticSession::MovementAccessProvider provider);
    // Installs pinned translators/AI engine on each newly built child, before its first step.
    void set_child_configurator(ChildConfigurator configure);
    // A paired parent-aware host must be installed before child reconstruction.
    [[nodiscard]] core::Result<void> set_child_script_services(ScriptFactory scripts, ChildConfigurator configure);
    [[nodiscard]] core::Result<void> set_child_script_services(BattleLayer, ScriptFactory, ChildConfigurator);
    // WGM-41..49: source-qualified admission/map/roster owner supplies the FT-09-built
    // record. No distance heuristic, map default, frame schedule or solver is inferred here.
    using ConflictService = std::function<core::Result<std::optional<RecordedAdmission>>(
        const sim::galactic::GalacticSnapshot&, const sim::galactic::GalacticEvent&, sim::EntityId)>;
    void set_conflict_service(ConflictService service, ContentResolver content, ScriptFactory scripts = {}, ScriptRequirement requirement = ScriptRequirement::required);
    using NativeConflictService = std::function<core::Result<std::optional<RecordedAdmission>>(
        const sim::galactic::GalacticSnapshot&, const sim::galactic::GalacticSession::NativeInvasion&, sim::EntityId)>;
    void set_native_conflict_service(NativeConflictService service);
    struct ConflictLayerServices {
        EntryResolver entry;
        ContentResolver content;
        ScriptFactory scripts;
        ChildConfigurator configure;
        ResultDecorator decorate;
        LayerResultApplier apply; // absent preserves the default land persistence writer
        ConflictService admission;
        NativeConflictService native_admission;
        ScriptRequirement script_requirement{ScriptRequirement::required};
        // Empty for a tactical provider. Otherwise ordinary choices use auto-resolve,
        // and forced/direct child construction returns this provider's named refusal.
        std::string tactical_refusal;
    };
    // One complete install per layer, before child construction. Parent queries are additive.
    [[nodiscard]] core::Result<void> install_conflict_layer(BattleLayer, ConflictLayerServices);
    [[nodiscard]] core::Result<void> add_conflict_parent_queries(BattleLayer, GalacticParentQueryFactory);
    [[nodiscard]] bool has_conflict_parent_queries(BattleLayer) const noexcept;
    [[nodiscard]] GalacticParentQueryFactory conflict_parent_queries(BattleLayer) const;

    using CorruptionMissionService = std::function<core::Result<std::optional<RecordedAdmission>>(
        const sim::galactic::GalacticSnapshot&, const sim::galactic::LiveCorruptionMission&,
        const sim::galactic::CorruptionChoice&, sim::EntityId)>;
    void set_corruption_mission_service(CorruptionMissionService);
    [[nodiscard]] core::Result<void> finish_corruption_mission(std::uint64_t, sim::galactic::CorruptionMissionState);
    // WGCH-67/140: trusted shared ruins request (native effect 17); false means nothing was applied.
    [[nodiscard]] core::Result<bool> request_planet_ruins(sim::galactic::PlanetId, bool destroy = true);
    [[nodiscard]] core::Result<void> queue_conflict(ConflictRecord record, Arbitration arbitration);
    [[nodiscard]] const ConflictRecord* pending_conflict() const noexcept;
    // Same pending/staged fact passed to the ordinary world's management admission.
    [[nodiscard]] bool has_pending_conflict_for_admission() const noexcept;
    struct NativeConflictReaders { std::function<bool()> admission, global; };
    // Borrowed from this session's stable Impl; bind anew on each load and keep
    // only while this session's owners live. Moving GameSession preserves them.
    [[nodiscard]] NativeConflictReaders native_conflict_readers() const;
    [[nodiscard]] std::optional<PendingConflictView> pending_conflict_view(sim::galactic::SeatId observer = {}) const;
    using PendingBattleAlertReader = std::function<std::optional<PendingBattleAlertInputs>(const ConflictRecord&)>;
    // Empty result means its sourced delay/solver has not completed. No frame-to-wall-clock
    // conversion or substitute winner is invented by this session.
    using AutoResolver = std::function<core::Result<std::optional<AutoResolveOutcomeApplication>>(
        const ConflictRecord&, const sim::galactic::GalacticSnapshot&, const sim::PartitionExecutor&)>;
    void set_auto_resolver(AutoResolver resolve);
    using VoteProgress = std::function<core::Result<std::optional<bool>>(
        const ConflictRecord&, const std::map<sim::galactic::PlayerId, bool>&)>;
    void set_vote_progress(VoteProgress progress);
    // GCMP-06 / WGM-47: session-tick timeout injected from mounted XML. Explicit
    // choices still enter through BattleChoice in the agreed seat inbox.
    [[nodiscard]] core::Result<void> configure_multiplayer_votes(std::uint64_t timeout_ticks);
    // Named zoom/vote/load/speech eligibility input of the manager tail (WGM-43/47,
    // GFD-18). A closed gate delays arbitration, without inventing a timeout.
    using ConflictGate = std::function<bool(const sim::galactic::GalacticSnapshot&)>;
    void set_conflict_gate(ConflictGate gate);
    void set_state_hasher(std::shared_ptr<sim::StateHasher> hasher) noexcept;

    [[nodiscard]] std::uint64_t completed_tick() const noexcept;
    [[nodiscard]] SessionMode mode() const noexcept;
    // Queries/binds never run the producer. Reader survives moves and refuses
    // after owner expiry; historical copies own their mode/tick/value.
    [[nodiscard]] core::Result<int> timeline(ModeId) const;
    [[nodiscard]] core::Result<int> active_timeline() const;
    [[nodiscard]] std::function<core::Result<int>()> active_timeline_reader() const;
    [[nodiscard]] std::shared_ptr<const ModeTimelineSnapshot> historical_child_timeline() const;
    // Fixed child identity: current copies while active, finalized copies after
    // retirement; never redirects to a later child or retains the game owner.
    using ChildTimelineReader = std::function<core::Result<std::shared_ptr<const ModeTimelineSnapshot>>() >;
    [[nodiscard]] ChildTimelineReader child_timeline_reader(ModeId) const;
    // Production path: no allocation per successful read. The factory remains
    // valid across wrapper moves and can bind identities created after load.
    using ChildTimelineValueReader = std::function<core::Result<ModeTimelineSnapshot>()>;
    using ChildTimelineReaderFactory = std::function<ChildTimelineValueReader(ModeId)>;
    [[nodiscard]] ChildTimelineReaderFactory child_timeline_reader_factory() const;
    [[nodiscard]] ModeTimelineState save_timeline() const;
    [[nodiscard]] core::Result<void> restore_timeline(const ModeTimelineState&);
    // Named tech-owner seam: complete current player list, including noncombatants;
    // never restricted to the tactical roster and never a parent-tech fallback.
    // Return one tech value per supplied ascending-ID player index. The supplied
    // IDs select the global owner list, not the deployed tactical participants.
    using ChildTimelineTech = std::function<core::Result<std::span<const std::int64_t>>(
        ModeId, std::span<const sim::galactic::GalacticPlayer>)>;
    void set_child_timeline_tech(ChildTimelineTech);
    [[nodiscard]] const sim::galactic::GalacticSession& galaxy() const noexcept;
    // WGS-170/173: callback-completed active-mode boundary, never parent fallback.
    [[nodiscard]] sim::galactic::story::RewardResult apply_story_reward(
        const sim::galactic::story::Reward&, const sim::PartitionExecutor&);
    [[nodiscard]] core::Result<void> restore_victory_state(const sim::galactic::VictoryState&);
    [[nodiscard]] core::Result<void> restore_actor_representations(std::span<const std::uint8_t> census_section);
    [[nodiscard]] std::string state_sha256() const;
    [[nodiscard]] sim::galactic::GalacticReplay record() const;
    [[nodiscard]] GameRecording recording() const;
    // Complete accepted input, including future commands/conflicts and load prologues.
    // A failed session cannot be saved. Staged child scripts must be replayable by the resolver.
    [[nodiscard]] core::Result<GameRecording> checkpoint_recording() const;
    // #4058 bounded history. A save/load moves the base at the START of the next step, so a capture or recapture
    // right after either still sees the window it was taken with. The frontier is what a save taken now seals.
    [[nodiscard]] const HistoryBase& history_base() const noexcept;
    [[nodiscard]] core::Result<HistoryBoundary> history_frontier() const;
    void schedule_history_rebase(HistoryBoundary boundary);
    // Replay-oracle seam: continue a session restored at a save with the next save's window (the commands, conflicts,
    // admissions, prologues and native effects after that boundary that it does not already hold).
    [[nodiscard]] core::Result<void> append_recording_window(const GameRecording& window);
    [[nodiscard]] core::Result<void> apply_load_fixup(const sim::galactic::GameCommand& command);
    [[nodiscard]] core::Result<void> apply_restore_prologue();
    [[nodiscard]] const ConflictRecord* conflict() const noexcept;
    [[nodiscard]] const sim::tactical::TacticalSession* battle() const noexcept;
    [[nodiscard]] core::Result<SeatBattleView> battle_view(sim::SeatId seat) const;
    [[nodiscard]] sim::galactic::FactionControl battle_controller(sim::galactic::PlayerId player) const noexcept;
    // Non-owning teardown observation; never serialized or hashed.
    [[nodiscard]] std::weak_ptr<const void> battle_lifetime() const noexcept;
    // Ordinary standalone tactical replay, retained after return for the existing M2 tools.
    [[nodiscard]] std::optional<sim::tactical::TacticalReplay> battle_record() const;
    [[nodiscard]] core::Result<std::optional<BattleCheckpoint>> save_battle_checkpoint() const;
    // Restore a live child after its entry barrier has been reconstructed. Consumes only
    // parent controls/envelopes to the saved barrier; never services the saved child again.
    [[nodiscard]] core::Result<void> restore_battle_checkpoint(const BattleCheckpoint& saved,
        const sim::tactical::TacticalReplay& log, std::uint64_t completed_tick,
        const sim::PartitionExecutor& executor);

    // Test seam: changes only private storage order in every layer.
    void scramble_storage_for_testing();

private:
    explicit GameSession(sim::galactic::GalacticSession galaxy);
    struct StepContext;
    // WGP-89 / GCMP-04: executes a journaled InvasionAccept from the agreed session state, revalidating the
    // retained result selection. Called only from process_session_commands.
    [[nodiscard]] core::Result<void> execute_invasion_accept(sim::galactic::SeatId, sim::EntityId conflict, bool& summary_closed);
    [[nodiscard]] core::Result<void> process_session_commands(StepContext&, const sim::PartitionExecutor&);
    [[nodiscard]] core::Result<void> step_child(StepContext&, const sim::PartitionExecutor&);
    [[nodiscard]] core::Result<void> reconcile_child_return(StepContext&, const sim::PartitionExecutor&);
    [[nodiscard]] core::Result<void> step_galaxy(StepContext&, const sim::PartitionExecutor&);
    [[nodiscard]] core::Result<void> apply_pending_returns(StepContext&, const sim::PartitionExecutor&);
    [[nodiscard]] core::Result<GameTick> finish_step(StepContext&);
    class Impl;
    std::unique_ptr<Impl> impl_;
    [[nodiscard]] core::Result<void> select_pending_battle(bool tactical, const sim::PartitionExecutor& executor);
};

} // namespace eawr::campaign
