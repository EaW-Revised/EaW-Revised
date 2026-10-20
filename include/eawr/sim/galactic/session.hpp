#pragma once
#include "eawr/sim/galactic/hero_return.hpp"

#include "eawr/core/diagnostic.hpp"
#include "eawr/core/result.hpp"
#include "eawr/sim/galactic/commands.hpp"
#include "eawr/sim/galactic/events.hpp"
#include "eawr/sim/galactic/graph.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/sim/galactic/replay.hpp"
#include "eawr/sim/galactic/scoring.hpp"
#include "eawr/sim/galactic/snapshot.hpp"
#include "eawr/sim/galactic/native_invasion.hpp"
#include "eawr/sim/galactic/types.hpp"
#include "eawr/sim/state_hash.hpp"
#include "eawr/sim/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::sim::galactic {

// The state hash's sections, in their fixed order (encoding `eawr-galactic-state-v1`). A section
// re-encodes and re-hashes only in a tick whose commit changed it; a clean section reuses its
// digest, and the result always equals a from-scratch hash. Later sections (production,
// conflicts, victory, script hosts) are appended to this list with the work that owns them.
enum class StateSection : std::uint16_t {
    session = 0, // seed, identities, rules, clock (not the speeds), next entity ID, debug flag, seat table
    players = 1,
    planets = 2,
    routes = 3,
    fleets = 4,
    graph = 5, // only present for setups with graph rules
    fleet_control = 6, // coordinator-reserved, opt-in after graph
    movement = 7, // coordinator-reserved, opt-in after fleet control
    production = 8, // coordinator-reserved, opt-in queues
    control = 9, // coordinator-reserved
    census = 11, // coordinator-reserved, opt-in global population
    victory = 16, // coordinator-reserved
    outcomes = 12, // coordinator-reserved, applied trusted campaign returns
    observer = 13, // coordinator-reserved
    income = 15, // coordinator-reserved
    economy_corruption = 17, // coordinator-reserved, GC tag 15
    native_invasions = 18, // retained explicit invasion queue, WGA-233/236
    planet_access = 19, // coordinator-reserved, authoritative AI access
    ability_host = 20, // coordinator-reserved, prepared action lifecycle
    campaign_scoring = 21, // coordinator-reserved, GHUD-S61 ledger; present only when nonempty
};
inline constexpr std::size_t state_section_count = 22;
inline constexpr std::string_view state_encoding_name = "eawr-galactic-state-v1";

struct GalacticTick {
    std::uint64_t completed_tick{};
    // The completed tick's state hash; empty when the session hashes off the stepping thread
    // (set_state_hasher), and then `state_hash` has it.
    std::string state_sha256;
    sim::StateHash state_hash;
    std::shared_ptr<const GalacticSnapshot> snapshot;
    // Deterministic work of the tick (diagnostics and budgets, never hashed).
    struct Work {
        bool serviced{};               // the galaxy ran this tick (not paused)
        std::uint64_t income_forecast_rebuilds{}; // value-key cache misses; zero for unchanged sources
        std::uint64_t income_sources_gathered{}, income_modifier_rebuilds{}; // derived publication cache work
        std::uint64_t fiscal_cycles{}; // at most one (WGM-03)
        std::uint64_t periodic_checks{};
        std::uint64_t gathered_objects{};
        std::uint64_t control_planets{}, control_route_visits{}, control_member_visits{};
        std::uint64_t control_stealth_fleets{}; // derived input rebuilds; zero on clean ticks
        std::uint64_t sections_encoded{};
        std::uint64_t sections_reused{};
        std::uint64_t population_objects{}, population_producers{}; // changed census work, zero when idle
        std::uint64_t production_producers{};
        std::uint64_t production_entries{}; // revalidation visits, at most each active entry once
        std::uint64_t discovery_records{}, discovery_targets{}; // deterministic observer work budget
        std::uint64_t observer_projection_records{}; // zero on clean snapshot publications
        std::uint64_t production_offer_rebuilds{}; // zero while the offer rows' inputs are unchanged
        std::uint64_t production_hash_bytes{}; // mutable production plus immutable offer digests
    } work;
};

// The deterministic home of one galaxy (ADR-012).

// One step runs the tick's commands, then services the galaxy unless it is paused (ADR-013): the
// galactic-gather phase, one galactic frame, at most one fiscal cycle (WGM-02/03, WGEP-01/02)
// and the 30-frame periodic checks (WGM-04). A paused galaxy only executes commands; its galactic
// frame does not advance. The service order within the frame is the walks' local order; the
// global order is unverified (WGM-U01, WGEP U-01/U-02).
// It mirrors the tactical session: commands are
// queued by submit() in canonical (tick, seat, sequence) order whatever order they arrive in and
// execute in step(); a command is authorized at execution by its seat's player (ADR-020: the seat
// must be active and seated in the player that owns what the command names); per-object work runs in partitioned phases over copied inputs with an
// ordered serial commit (ADR-009), so 1, 2, 4 or 8 workers and any storage order give the same
// hashes. One tick is one FoC logical frame, 1/30 s of galactic time (ADR-013).
class GalacticSession final {
public:
    using NativeInvasion = sim::galactic::NativeInvasion;
    struct NativeInvasionReceipt {
        bool admitted{};
        std::uint64_t request{};
        bool queued{};
        GalacticRejectReason reason{};
        bool operator==(const NativeInvasionReceipt&) const = default;
    };
    [[nodiscard]] core::Result<NativeInvasionReceipt> admit_native_invasion(PlayerId, FleetId, bool pending_local_conflict = false);
    [[nodiscard]] core::Result<NativeInvasionReceipt> admit_native_invasion(PlayerId, FleetId,
        std::span<const EntityId> retained_transports, bool pending_local_conflict = false);
    [[nodiscard]] const NativeInvasion* native_invasion(std::uint64_t request) const noexcept;
    [[nodiscard]] const NativeInvasion* pending_native_invasion() const noexcept;
    [[nodiscard]] bool native_invasion_available() const noexcept;
    void enable_native_invasions(bool enabled);
    // Only the existing campaign capable-opponents/admission owner calls this.
    // Zero conflict means its qualified undefended branch, never a solver winner.
    [[nodiscard]] core::Result<void> dispatch_native_invasion(std::uint64_t request, EntityId conflict);
    [[nodiscard]] core::Result<void> finish_native_invasion(EntityId conflict, PlayerId actual_winner);
    [[nodiscard]] core::Result<void> retire_native_invasion(std::uint64_t request);
    // GCMP-04: the typed InvasionAccept session command is itself the journal entry, so these two run
    // the same admission and release without appending a native effect. Replaying the command stream then
    // reproduces them on every peer; a replayed native effect would apply the admission twice.
    [[nodiscard]] core::Result<NativeInvasionReceipt> admit_native_invasion_by_command(PlayerId, FleetId,
        std::span<const EntityId> retained_transports);
    [[nodiscard]] core::Result<void> retire_native_invasion_by_command(std::uint64_t request);
    [[nodiscard]] core::Result<void> replay_native_invasion_service();
    [[nodiscard]] const EconomyCorruptionState* economy_corruption() const noexcept;
    [[nodiscard]] const GalacticAbilityState* ability_state() const noexcept;
    // WGCH-70: the campaign owns the existing saved hero-return queue.
    // Trusted generic service owner; saves/replays retain the enabled/cancelled facts.
    [[nodiscard]] core::Result<void> set_ability_lifecycle(EntityId, bool enabled, bool cancelled, AbilityKind = AbilityKind::black_market);
    // WGCH-58/160: the authored script bracket (Set_Check_Contested_Space) as a journaled trusted write; true arms the check.
    [[nodiscard]] core::Result<void> set_destruction_contested_check(EntityId actor, bool enabled);
    struct AbilityWork {
        std::uint64_t preparations{}, executions{}, registered_types{}, publications{};
        friend bool operator==(const AbilityWork&, const AbilityWork&) = default;
    };
    [[nodiscard]] AbilityWork ability_work() const noexcept;
    using AbilityConsumptionHandler = std::function<core::Result<void>(const HeroDefeat&)>;
    void set_ability_consumption_handler(AbilityConsumptionHandler);
    using CorruptionMissionHandler = std::function<core::Result<bool>(const LiveCorruptionMission&, const CorruptionChoice&)>;
    using CorruptionCompletionHandler = std::function<core::Result<void>(PlanetId, corruption_fixture::Type, corruption_fixture::Type, CorruptionCompletionPhase,
        std::span<const PlanetAbilityEntitlements>)>;
    void set_corruption_mission_handler(CorruptionMissionHandler);
    void set_corruption_completion_handler(CorruptionCompletionHandler);
    using CorruptionResultHandler = std::function<core::Result<void>(const LiveCorruptionMission&)>;
    void set_corruption_result_handler(CorruptionResultHandler);
    [[nodiscard]] core::Result<bool> begin_corruption(PlanetId, corruption_fixture::Type, math::Fixed);
    // WGCH-67/140..142: shared live ruins cleanup, journaled as native effect 17 (coordinator-reserved).
    // The story handler runs FIRST (destroy mode only), before any cleanup and while the flag is still unset.
    // false = nothing was applied (unavailable inputs, already ruined, or an unresolved named seam).
    using PlanetRuinsStoryHandler = std::function<core::Result<void>(PlanetId)>;
    void set_planet_ruins_story_handler(PlanetRuinsStoryHandler);
    [[nodiscard]] core::Result<bool> request_planet_ruins(PlanetId, bool destroy = true);
    [[nodiscard]] core::Result<void> finish_corruption_mission(std::uint64_t request, CorruptionMissionState);
    GalacticSession(GalacticSession&&) noexcept;
    GalacticSession& operator=(GalacticSession&&) noexcept;
    ~GalacticSession();

    GalacticSession(const GalacticSession&) = delete;
    GalacticSession& operator=(const GalacticSession&) = delete;

    [[nodiscard]] static core::Result<GalacticSession> create(const GalacticSetup& setup);
    // Creates the replay's setup session and submits every recorded command.
    [[nodiscard]] static core::Result<GalacticSession> from_replay(const GalacticReplay& replay);

    // Rejects, without changing the session: an unknown seat, a tactical-layer command (the battle
    // takes those), a malformed or unimplemented payload, a debug command without the setup flag, a
    // tick that has already executed or is past the tick limit, and a (tick, sequence) that does
    // not follow the seat's previous submission.
    [[nodiscard]] core::Result<void> submit(const GameCommand& command);
    // Executes the commands of tick completed_tick() and advances it by one. Fails without
    // changing the session at galactic_max_ticks. A failed phase fails the step after the tick's
    // commands ran; the session then refuses every further step (EAWR-SIM-0508).
    [[nodiscard]] core::Result<GalacticTick> step(const PartitionExecutor& executor,
        bool pending_local_conflict = false);
    // WGS-99/142: the conflict owner opens this window for the ticks in which a pending, not yet
    // entered tactical conflict waits for its choice. Only then is a prebattle autosave admitted
    // (the galaxy is held by the conflict's own pause at that point). Derived by the owner each
    // step; never part of the saved state.
    void set_prebattle_autosave_window(bool open) noexcept;
    // With a hasher, step() leaves the SHA-256 work to it, off the stepping thread; the digest is
    // the one the synchronous path returns. The hasher is not state.
    void set_state_hasher(std::shared_ptr<StateHasher> hasher) noexcept;
    // Campaign boundary commits, without galaxy service or advancing its local tick (WGT-01).
    // Session controls execute before the active child; galactic actions stay in the game queue.
    // WGUI-102: campaign restore supplies the active child frame/logical FPS for
    // observer refresh; absent a child, retain the galaxy's clock and rate.
    [[nodiscard]] core::Result<void> apply_session_controls(std::span<const GameCommand> commands,
        bool pending_local_conflict = false, bool append_events = false,
        std::optional<std::pair<std::uint64_t, std::uint32_t>> observer_load_clock = {});
    [[nodiscard]] core::Result<void> reconcile_battle(EntityId first_free, std::uint64_t session_tick,
        std::uint64_t tactical_frame);
    // WAR-45/54: trusted return boundary. Removes persistent identities from every
    // orbital container and detached roster, including movement's live contents.
    // The caller expands container deaths through the WAR-U04 lethal-effects seam.
    [[nodiscard]] core::Result<void> apply_battle_losses(std::span<const EntityId> destroyed);
    // GHUD-S61 / WGP-98: the same removal, crediting each recorded final blow (ascending id).
    // A destroyed identity with no entry is credited to the missing-id sentinel.
    [[nodiscard]] core::Result<void> apply_battle_losses(std::span<const EntityId> destroyed,
        std::span<const BattleKiller> killers);
    // WGP-97..103: the saved scoring counters, copied by the campaign end provider before shutdown.
    [[nodiscard]] const CampaignScoreLedger& score_ledger() const noexcept;
    // WGP-102/WGS-177: the forced-player-quit producer marks that player's score as zero.
    [[nodiscard]] core::Result<void> record_player_quit(PlayerId player);
    [[nodiscard]] core::Result<void> apply_battle_credits(std::span<const std::pair<PlayerId, math::Fixed>> balances);
    [[nodiscard]] std::span<const OutcomeJournalEntry> pending_outcomes() const noexcept;
    using OutcomeCommit = std::function<core::Result<void>(const OutcomeJournalEntry&)>;
    [[nodiscard]] core::Result<void> commit_outcome(const OutcomeJournalEntry& entry, const OutcomeCommit& apply,
        const PartitionExecutor& executor);
    // Stages engine-selected input for a later explicit campaign-return boundary.
    [[nodiscard]] core::Result<void> stage_outcome(OutcomeJournalEntry entry);
    void append_outcome_events(PlanetId planet, EntityId conflict, PlayerId winner, PlayerId loser);
    [[nodiscard]] core::Result<void> apply_control_battle(const ControlBattleReturn& returned);
    // WGT-84: the tactical child/parent adapter supplies its conflict planet and
    // holder owner; standalone and linked-resolution writers are excluded there.
    [[nodiscard]] core::Result<void> add_tactical_built_structure(PlanetId,
        const TacticalBuiltStructure&, PlayerId holder_owner);
    [[nodiscard]] core::Result<void> remove_tactical_built_structure(PlanetId, TypeId, math::Vec3 position);
    [[nodiscard]] core::Result<void> fix_tactical_built_structures(PlanetId, PlayerId winner);

    // WGM-61..63: trusted campaign return reuses ordinary departure and movement.
    // The caller owns destination selection; surface packing excludes the supplied
    // sorted confirmed casualties, which remain for subsequent ordinary cleanup.
    [[nodiscard]] core::Result<void> apply_retreat_return(PlayerId player, PlanetId source,
        PlanetId destination, bool surface, std::span<const FleetId> orbital_fleets,
        std::span<const EntityId> confirmed_destroyed = {});
    using MovementAccessProvider = std::function<core::Result<std::vector<MovementAccess>>(
        const GalacticSnapshot&, const PartitionExecutor&)>;
    void set_movement_access_provider(MovementAccessProvider provider);
    [[nodiscard]] core::Result<void> refresh_movement_access(const PartitionExecutor& executor);
    // WGA-227/228: ordered native admission uses the ordinary fleet command
    // validators and publishes containment before the next native reader.
    struct NativeFleetReceipt {
        bool admitted{};
        EntityId object{};
        GalacticRejectReason reason{GalacticRejectReason::none};
    };
    [[nodiscard]] core::Result<NativeFleetReceipt> admit_native_fleet(PlayerId,
        const CommandPayload&, bool pending_local_conflict = false);
    // WGA-227/234: ordinary transport containment only. Ground-ability
    // deployment is a separate capability; hostile landing is never invasion.
    [[nodiscard]] core::Result<NativeFleetReceipt> launch_native_transport(PlayerId, EntityId,
        FleetId target = 0, bool pending_local_conflict = false);
    [[nodiscard]] core::Result<NativeFleetReceipt> land_native_transports(PlayerId, FleetId,
        bool pending_local_conflict = false);
    // WGA-411..413: direct fleetless event; admission ignores surface-link success.
    struct NativeTransportLandingReceipt { NativeFleetReceipt event; bool linked{}; };
    [[nodiscard]] core::Result<NativeTransportLandingReceipt> land_native_transport(PlayerId, EntityId, PlanetId,
        bool pending_local_conflict = false, std::optional<std::uint32_t> live_landed = {});
    // The admitted phase can supply its cached count, updated after each ordered link.
    enum class NativeTransportLandingGate { admitted, capacity, surface, mixed_allied_owner_unavailable };
    [[nodiscard]] core::Result<NativeTransportLandingGate> native_transport_landing_available(PlayerId, PlanetId,
        std::optional<std::uint32_t> live_landed = {}) const;
    [[nodiscard]] core::Result<void> recheck_native_orbit(PlayerId, FleetId);
    // Ordered movement notifications retain native listener flags independently
    // of how often an authored block polls. Runtime dependency, rebound on load.
    using NativeMovementListener = std::function<void(const GalacticEvent&)>;
    void set_native_movement_listener(NativeMovementListener);
    // Reconstruction applies recorded effects without invoking AI decisions.
    // Typed owners retain validation and consume world-service notices in place.
    [[nodiscard]] core::Result<void> append_native_effect(NativeEffectKind, std::vector<std::uint8_t>,
        NativeEffectPhase = NativeEffectPhase::barrier);
    using NativeEffectReplayer = std::function<core::Result<void>(GalacticSession&, const NativeEffect&, const PartitionExecutor&)>;
    void set_native_effect_replayer(NativeEffectKind, NativeEffectReplayer);
    [[nodiscard]] core::Result<void> stage_native_replay(std::span<const NativeEffect>, std::uint64_t final_tick);
    // #4058: the effects are a recording window numbered from 1; the journal before it (sequence_base entries) was dropped.
    [[nodiscard]] core::Result<void> stage_native_replay(std::span<const NativeEffect>, std::uint64_t final_tick, std::uint64_t sequence_base);
    // #4058 save/load boundary: forget the executed commands and the applied native journal (their counts stay in
    // executed_dropped/native_dropped so command limits and native sequences are unchanged). Pending input is kept.
    void rebase_history() noexcept;
    [[nodiscard]] std::uint64_t executed_dropped() const noexcept;
    [[nodiscard]] std::uint64_t native_dropped() const noexcept;
    [[nodiscard]] core::Result<void> replay_native_barrier(const PartitionExecutor&);
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> consume_native_effect(NativeEffectKind);
    [[nodiscard]] core::Result<void> finish_native_replay();
    [[nodiscard]] bool replaying_native_effects() const noexcept;
    [[nodiscard]] std::span<const NativeEffect> native_effects() const noexcept;
    // WGM-74..77 / WGEP-60: production-stop, victory/story, refresh_credit_caps,
    // capture credit and corruption/visibility owners consume ordered effects before fiscal.
    // A handler is a runtime dependency; it does not enter a replay or a checkpoint.
    using ControlEffectsHandler = std::function<core::Result<void>(std::span<const PoliticalControlEffect>, std::span<const Planet>)>;
    void set_control_effects_handler(ControlEffectsHandler handler);
    // GFS-A18/GFD-A13: completed object tech before fiscal/AI; support phase is a named seam.
    // Every admitted shared-mode invocation, including mode pause.
    using TimelineService = std::function<core::Result<void>(const std::optional<PlayerEconomySetup>&)>;
    void set_timeline_service(TimelineService);
    struct TechnologyNotice {
        enum class Kind : std::uint8_t { assigned, new_options };
        Kind kind{};
        PlayerId player{};
        std::int64_t tech{};
    };
    // WGEP-130/147: assigned reaches story before unlocks; new_options is a
    // local request only, whose handler still resolves the faction SFX event.
    using TechnologyNoticeHandler = std::function<core::Result<void>(const TechnologyNotice&)>;
    void set_technology_notice_handler(TechnologyNoticeHandler);
    using FeedbackNoticeHandler = std::function<void(const GalacticFeedbackNotice&)>;
    void set_feedback_notice_handler(FeedbackNoticeHandler);
    // WST-41: how a story write moves the account's maximum. `raise_to_target` is SET_TECH_LEVEL (the maximum
    // rises first when the target is above it); `set_only` is SET_MAX_TECH_LEVEL (the maximum alone, target = the
    // new maximum, never below the current level). Journaled only when not `keep`.
    enum class TechnologyMaximum : std::uint8_t { keep, raise_to_target, set_only };
    struct TechnologyWrite {
        PlayerId player{};
        std::int64_t target{};
        bool process_unlocks{true}, request_sound{true};
        TechnologyMaximum maximum{TechnologyMaximum::keep};
    };
    // Ordered trusted writers, journaled independently of ordinary production.
    // Assignment preserves maximum; increment at/above maximum skips the setter.
    [[nodiscard]] core::Result<std::int64_t> set_technology(const TechnologyWrite&);
    [[nodiscard]] core::Result<std::int64_t> increment_technology(PlayerId);
    // WST-64: one journaled production-lock write (native effect kind 19). Unlock first (the build lock is removed
    // and the type joins the unlocked list), then lock (the type joins the locked list); a zero type skips its
    // half. `slicer_check` runs the faction's post-check: when no affiliated, initially locked, slicer-unlockable
    // type at or below the current level is left off the unlocked list, the level goes up by one (unlocks run).
    struct ProductionLockWrite {
        PlayerId player{};
        TypeId unlock{}, lock{};
        bool slicer_check{};
    };
    struct ProductionLockReceipt { bool tech_incremented{}; };
    [[nodiscard]] core::Result<ProductionLockReceipt> apply_production_lock(const ProductionLockWrite&);
    // WGA-13..15/85: actual post-multiplier/cap deltas, delivered at the credit
    // commit boundary before the outer player AI turn. Only initialized AI
    // ledgers consume them. Runtime dependency, never checkpoint state.
    struct CreditNotification {
        enum class Kind : std::uint8_t { fiscal, windfall };
        Kind kind{};
        PlayerId player{};
        // WGEP-112: fiscal delivers the unadjusted aggregate after account
        // crediting. WGA-170: windfall delivers the actual clamped delta.
        math::Fixed credits{}, amount{}, seconds{};
    };
    using CreditNotificationHandler = std::function<core::Result<void>(const CreditNotification&)>;
    void set_credit_notification_handler(CreditNotificationHandler handler);
    struct TechnologyPriceWrite {
        PlayerId player{};
        EntityId source{};
        std::optional<math::Fixed> additive; // absent removes this source
        std::optional<std::uint64_t> first_expired_frame;
    };
    // Trusted ability/story provider write; checkpoint reconstruction journals it.
    [[nodiscard]] core::Result<math::Fixed> set_technology_price_modifier(const TechnologyPriceWrite&);
    // WGEP-05/163..165: native credit-halt sabotage writer. The actor's authored
    // halt profile supplies the duration; the row registers under the actor's own
    // source identity until its first expired GC frame, which only active GC time
    // advances. A live source never extends or duplicates: repeating the write is a
    // receipt with changed=false. Journaled; reconstruction replays absolute frames.
    struct IncomeBlockerReceipt {
        bool changed{};
        std::uint64_t first_expired_frame{}; // zero when nothing is registered (zero-length authored halt)
    };
    [[nodiscard]] core::Result<IncomeBlockerReceipt> halt_planet_credits(EntityId actor, PlanetId planet);
    // Explicit source removal (source terminated). Returns whether a live row existed.
    [[nodiscard]] core::Result<bool> remove_income_blocker(PlanetId planet, EntityId source);
    // WGA-64/100: the caller supplies the selected positive AI multiplier (one
    // without AI). Negative additions bypass it. Refunds/returns use their own
    // owners and must not invoke this explicit windfall path.
    [[nodiscard]] core::Result<math::Fixed> add_credits(PlayerId player,
        math::Fixed delta, math::Fixed positive_multiplier);
    // WGA-23/24/99: trusted AI admission at an ordered native barrier. The
    // build identity lives in the ordinary queue; handlers are rebound on load.
    struct NativeProductionRequest { PlayerId player{}; PlanetId planet{}; TypeId type{}; std::optional<math::Fixed> mark_cost; };
    struct NativeProductionReceipt {
        ProductionAdmission admission{ProductionAdmission::unavailable};
        EntityId build{};
        math::Fixed price{};
    };
    struct NativeProductionNotice {
        enum class Kind : std::uint8_t { marked, billed };
        Kind kind{};
        PlayerId player{};
        EntityId build{};
        math::Fixed amount{};
        math::Fixed price{}, remaining{};
        PlayerId planet_owner{};
    };
    struct NativeProductionPayment { bool accepted{}; PlayerId payer{}; math::Fixed amount{}; };
    using NativeProductionHandler = std::function<core::Result<NativeProductionPayment>(const NativeProductionNotice&)>;
    void set_native_production_handler(NativeProductionHandler);
    // GFD-A05/07: publish the exact retained build and created actor after the
    // world commit. This notification never bills or advances a coroutine.
    struct NativeProductionOutcome { PlayerId player{}; EntityId build{}, object{}; bool completed{}; };
    using NativeProductionOutcomeHandler = std::function<void(const NativeProductionOutcome&)>;
    void set_native_production_outcome_handler(NativeProductionOutcomeHandler);
    // WGA-314..316: sparse actual world writes, never a membership scan.
    // Changed rows preserve omitted actor flags. A generated birth names its
    // actual source; zero source denotes an ordinary constructor boundary.
    struct NativeActorChange {
        enum class Kind : std::uint8_t { created, changed, lost };
        Kind kind{};
        EntityId actor{}, source{};
        std::optional<bool> dead, deleting, death_clone;
    };
    using NativeActorChangesHandler = std::function<core::Result<void>(
        const GalacticSnapshot&, std::span<const NativeActorChange>)>;
    void set_native_actor_changes_handler(NativeActorChangesHandler);
    // CRP-03: trusted type-owner write; requires a bound literal current type.
    [[nodiscard]] core::Result<void> change_actor_current_type(EntityId, TypeId);
    // WGM-161/162: effect owner supplies a null/self target, never a fleet aura.
    [[nodiscard]] core::Result<void> apply_actor_stealth(EntityId owner, EntityId target, math::Fixed chance, bool apply);
    // GAG-01..03: qualified lifecycle write, never type/location inference.
    [[nodiscard]] core::Result<void> set_actor_garrison(EntityId, bool);
    // Restore current/source facts after reconstructing world references, before
    // native AI restoration. Pure publication: no births or pool operations.
    [[nodiscard]] core::Result<void> restore_actor_representations(std::span<const std::uint8_t> census_section);
    [[nodiscard]] core::Result<NativeProductionReceipt> admit_native_production(const NativeProductionRequest&, const PartitionExecutor&);
    [[nodiscard]] core::Result<void> finish_native_production_task(EntityId build, bool salvage, math::Fixed positive_multiplier);
    struct NativeForcePlacement {
        EntityId next_id{};
        std::vector<Fleet> fleets;
        std::vector<MovementContents> contents;
        std::vector<SurfaceForce> landed;
        std::vector<ProductionObject> structures;
        std::vector<CensusObject> originals;
        std::optional<ControlBase> starbase, ground_base;
        TypeId starbase_type{};
        std::vector<math::Fixed> special_control;
        EntityId primary{}; // direct-production result, separate from recursive additional objects
    };
    using NativeForceBuilder = std::function<core::Result<NativeForcePlacement>(const GalacticSnapshot&, EntityId)>;
    struct NativeForceView {
        const Planet& planet;
        const ControlPlanet& presence;
        const PlanetProduction& production;
        const NestedCreationSetup& creation;
        std::uint32_t maximum_ground_forces{};
        std::span<const Fleet> orbit;
        std::span<const MovementContents> contents;
        std::function<core::Result<bool>(const Fleet&)> combat_skip;
    };
    using NativeShadowForceBuilder = std::function<core::Result<NativeForcePlacement>(const NativeForceView&, EntityId)>;
    // Service-local sparse shadow. Copies share the completed snapshot and
    // immutable prepared placements; only written accounts/locations detach.
    // A layer and its lazy publication belong to one decision thread. Before
    // sharing reads, that owner materializes snapshot() and hands off the const
    // snapshot, never an unfrozen shadow. S9 must retain this freeze boundary.
    class NativeActionShadow {
    public:
        struct Work {
            std::uint64_t requests{}, copied_accounts{}, copied_planets{}, copied_orbital_objects{}, prepared_objects{};
            std::uint64_t copied_scene_objects{}; // always zero: no ECS/scene clone
            std::uint64_t copied_fleet_rows{}, copied_actor_rows{}; // mutable frontier, not read-publication columns
        };
        [[nodiscard]] core::Result<math::Fixed> add_credits(PlayerId, math::Fixed, math::Fixed);
        [[nodiscard]] core::Result<std::vector<EntityId>> create_force(
            const NativeProductionRequest&, const NativeShadowForceBuilder&, const PartitionExecutor&);
        [[nodiscard]] std::optional<math::Fixed> credits(PlayerId) const;
        [[nodiscard]] const CensusObject* created_object(EntityId) const;
        [[nodiscard]] std::span<const NativeActorChange> actor_changes() const noexcept;
        [[nodiscard]] const Planet* planet(PlanetId) const;
        // Sequential player passes inherit an immutable lower-ID frontier.
        // Forking does not copy any scene rows or the preceding player's writes.
        [[nodiscard]] core::Result<NativeActionShadow> for_player(PlayerId, std::size_t max_requests) const;
        [[nodiscard]] PlayerId player() const noexcept;
        // Read adapter for player layers. Legacy immediate batches keep their
        // narrow credit/creation queries and return no player publication here.
        [[nodiscard]] std::shared_ptr<const GalacticSnapshot> snapshot() const;
        [[nodiscard]] const Fleet* fleet(FleetId) const;
        [[nodiscard]] const CensusObject* actor(EntityId) const;
        [[nodiscard]] core::Result<NativeFleetReceipt> admit_fleet(PlayerId, const CommandPayload&, bool pending_local_conflict);
        [[nodiscard]] core::Result<NativeProductionReceipt> admit_production(const NativeProductionRequest&,
            const PartitionExecutor&, const NativeProductionHandler&, std::span<const TypeId> local_unlocks = {});
        [[nodiscard]] core::Result<NativeFleetReceipt> launch_transport(PlayerId, EntityId, FleetId, bool pending_local_conflict);
        [[nodiscard]] core::Result<NativeFleetReceipt> land_transports(PlayerId, FleetId, bool pending_local_conflict);
        [[nodiscard]] core::Result<void> recheck_orbit(PlayerId, FleetId);
        [[nodiscard]] core::Result<NativeTransportLandingReceipt> land_transport(PlayerId, EntityId, PlanetId,
            bool pending_local_conflict, std::optional<std::uint32_t> live_landed = {});
        [[nodiscard]] EntityId next_entity_id() const noexcept;
        [[nodiscard]] const Work& work() const noexcept;
    private:
        struct Data;
        std::shared_ptr<Data> data_;
        explicit NativeActionShadow(std::shared_ptr<Data> data) : data_(std::move(data)) {}
        void detach();
        friend class GalacticSession;
    };
    [[nodiscard]] core::Result<NativeActionShadow> begin_native_actions(std::size_t max_requests) const;
    [[nodiscard]] core::Result<NativeActionShadow> begin_native_actions(PlayerId, std::size_t max_requests) const;
    // Legacy immediate batches apply prepared placements and publish once;
    // player layers follow the ordered replay contract below.
    [[nodiscard]] core::Result<void> commit_native_actions(const NativeActionShadow&, bool actor_changes_prepared = false);
    // Validate predecessor ownership, player order and identity frontiers before
    // applying any layer. Effects then replay in order; a replay failure can
    // leave earlier effects applied. NativeActionBarrier invalidates its session.
    [[nodiscard]] core::Result<void> commit_native_actions(std::span<const NativeActionShadow>, bool actor_changes_prepared = false, bool native_effects_prepared = false);

    // Owns one sequential AI barrier. Private player layers are selected in
    // ascending order; deferred script commands use the final inherited tail.
    // Use and commit on the creator's thread. Destruction on another thread
    // requires that the creator is quiescent; it aborts the session safely.
    class NativeActionBarrier {
    public:
        struct Work { std::size_t layers{}, read_checkpoints{}; };
        ~NativeActionBarrier();
        NativeActionBarrier(const NativeActionBarrier&) = delete;
        NativeActionBarrier& operator=(const NativeActionBarrier&) = delete;
        [[nodiscard]] core::Result<void> begin_players();
        [[nodiscard]] core::Result<void> select_player(PlayerId);
        [[nodiscard]] core::Result<void> begin_tail();
        [[nodiscard]] core::Result<void> commit();
        [[nodiscard]] Work work() const noexcept;
    private:
        struct State;
        std::shared_ptr<State> state_;
        explicit NativeActionBarrier(std::shared_ptr<State>);
        friend class GalacticSession;
    };
    [[nodiscard]] core::Result<std::unique_ptr<NativeActionBarrier>> begin_native_barrier(
        std::span<const PlayerId>, std::size_t max_requests);

    // WGA-99: direct placement, no paid/population admission. The pure builder
    // resolves original types and recursively expands additional creations.
    // Paid queues remain intact; separate scheduled-spawn-list cleanup requires its owner.
    [[nodiscard]] core::Result<std::vector<EntityId>> create_native_force(const NativeProductionRequest&, const NativeForceBuilder&);
    struct DueHeroPlacement {
        EntityId primary{};
        std::vector<CensusObject> objects;
    };
    // WGM-132..135: direct ordinary placement, no purchase; commits the complete tree
    // even for secondary-only results and reports only the primary as success.
    [[nodiscard]] core::Result<DueHeroPlacement> place_due_hero(const NativeProductionRequest&,
        const NativeForceBuilder&, const PartitionExecutor&);
    [[nodiscard]] core::Result<HeroStructureState> due_hero_structure_state(
        const HeroReturnRecord&, const PartitionExecutor&);
    // Runtime conflict-service dependency, like the control-effects consumer. Without it,
    // defended landing remains unavailable; requests never perform an uncontested landing.
    void enable_conflict_requests(bool enabled);
    [[nodiscard]] core::Result<void> adjust_conflict_pause(bool begin);
    [[nodiscard]] core::Result<void> reserve_conflict_entities(EntityId first_free);
    void publish_conflict_events(std::span<const GalacticEvent> events);
    using ConflictTailHandler = std::function<core::Result<void>(GalacticSession&)>;
    // `outer_timer`: the installing mode manager advances the frame with finish_conflict_frame_timer
    // after its hosted services (conflict services or a victory monitor). A tail installed only to service
    // pending hero returns leaves the frame timer where the galaxy step keeps it without a tail.
    void set_conflict_tail_handler(ConflictTailHandler handler, bool outer_timer = true);
    // GFD-26/GFSC-18: the owning mode manager finishes its ACTIVE parent timer after
    // players/global/story service. No increment when its final pause predicate is true.
    void finish_conflict_frame_timer();
    // GFS-15: GameSession owns this tail after hosted AI/story; standalone worlds use their step tail.
    void defer_victory_tail(bool deferred);
    void finish_victory_frame();

    [[nodiscard]] std::uint64_t completed_tick() const noexcept;
    [[nodiscard]] GalacticClock clock() const noexcept;
    // During a native barrier only snapshot() and the routed native queries
    // expose staged reads. victory() and the other owner getters remain live.
    [[nodiscard]] const std::optional<VictoryState>& victory() const noexcept;
    // GCMP-24: the one multiplayer flag of this session. Setup copies it from the campaign setup mode into the
    // saved victory context and corruption economy; this reads the saved copy, never a seat-count proxy.
    [[nodiscard]] bool multiplayer() const noexcept;
    // WGS-172..174: trusted reward calls after the plot's Lua trigger callback.
    [[nodiscard]] core::Result<bool> apply_story_victory(PlayerId, bool suppress_text, const PartitionExecutor&);
    [[nodiscard]] core::Result<bool> apply_story_victory_wait(bool enable, const PartitionExecutor&);
    // WGS-57: hydrate the saved owning monitor without reward/notice replay.
    [[nodiscard]] core::Result<void> restore_victory_state(const VictoryState&);
    // WGCH-76/WGS-23/24: attributed lifecycle producer before removal; return
    // records come from their owner, not from absence of a live object.
    [[nodiscard]] core::Result<void> apply_hero_defeat_victory(const HeroDefeat&,
        std::span<const HeroReturnRecord>, const PartitionExecutor&);
    void sync_victory_hero_returns(std::span<const HeroReturnRecord>);
    // Trusted return effects, alongside control/loss reconciliation; no player command.
    [[nodiscard]] core::Result<void> apply_superweapon_victory_return(PlayerId weapon_owner, PlayerId winner, PlayerId loser, bool killed);
    // WGS-79: after strategic destruction admission, before ruins cleanup.
    // Landed contents are current numeric types, resolved by the action owner.
    [[nodiscard]] core::Result<bool> apply_planet_destruction_victory(PlayerId actor_owner,
        PlayerId current_controller, std::span<const TypeId> landed_current_types);
    [[nodiscard]] EntityId next_entity_id() const noexcept;
    // The tick-zero setup the session was created from (not the current state).
    [[nodiscard]] const GalacticSetup& setup() const noexcept;
    [[nodiscard]] std::span<const Seat> seats() const noexcept;
    [[nodiscard]] const std::optional<SeatControlState>& seat_control() const noexcept;
    // Trusted conflict-service boundary, not a player action. Roles are chosen by commands.
    [[nodiscard]] core::Result<void> begin_battle_roles(EntityId conflict, PlayerId invader, PlayerId defender);
    void end_battle_roles();
    [[nodiscard]] const PlanetGraph* graph() const noexcept;
    [[nodiscard]] std::size_t pending_command_count() const noexcept;
    // The executed commands so far, canonical order (accepted or rejected at execution).
    [[nodiscard]] std::span<const GameCommand> executed_commands() const noexcept;
    // The replay of this session so far: its setup, final tick = completed tick, and every executed
    // command in canonical order.
    [[nodiscard]] GalacticReplay record() const;

    // Every section's canonical encoding: magic "EAWRGST\0", encoding version, section count,
    // then per section its ID, version, length and bytes. The checkpoint's galaxy sections use
    // the same encodings.
    [[nodiscard]] std::vector<std::uint8_t> canonical_state_bytes() const;
    // Complete owner barrier, including non-hashed command/native cursors. No historical service.
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> save_checkpoint_state() const;
    [[nodiscard]] core::Result<void> load_checkpoint_state(std::span<const std::uint8_t>);
    // The sectioned state hash computed from scratch: SHA-256 over the encoding name and every
    // section's ID, version and SHA-256. Equals the hash step() returned for this tick.
    [[nodiscard]] std::string state_sha256() const;
    // Completed owner barrier: refresh only sections dirtied since the last
    // hash, including native effects admitted after ordinary world service.
    [[nodiscard]] sim::StateHash cached_state_hash() const;
    // Test/CI flag; also enabled by EAWR_VERIFY_GALACTIC_HASH=1 at creation.
    // Independently recompute every section on each cached-hash request.
    void set_hash_verification(bool enabled) noexcept;
    [[nodiscard]] std::shared_ptr<const GalacticSnapshot> snapshot() const noexcept;
    // WGA-256: trusted deterministic effects only; external story dispatch is WST-42's seam.
    [[nodiscard]] core::Result<bool> set_planet_access(PlanetId, bool restricted,
        std::optional<bool> local_revealed);
    [[nodiscard]] core::Result<void> restore_planet_access(std::span<const std::uint8_t>);

    // Test seam: changes only private ECS storage order. Authoritative ordering is unchanged.
    void scramble_storage_for_testing();

private:
    class Impl;
    [[nodiscard]] NativeActionBarrier::State* native_barrier() const noexcept;
    [[nodiscard]] NativeActionShadow* native_shadow() const noexcept;
    [[nodiscard]] core::Result<NativeActionShadow> continue_native_actions(const NativeActionShadow&,
        std::size_t, bool immediate, bool tail) const;
    [[nodiscard]] core::Result<void> advance_native_segment(bool tail = false, std::size_t actions = 1);
    [[nodiscard]] core::Result<void> publish_native_batch(const NativeActionShadow&, bool actors_prepared);
    [[nodiscard]] core::Result<void> deliver_native_shadow_changes(const NativeActionShadow&, std::size_t first);
    [[nodiscard]] core::Result<NativeFleetReceipt> stage_native_fleet(NativeActionShadow&, PlayerId, const CommandPayload&, bool);
    [[nodiscard]] core::Result<NativeFleetReceipt> stage_native_launch(NativeActionShadow&, PlayerId, EntityId, FleetId, bool);
    [[nodiscard]] core::Result<NativeFleetReceipt> stage_native_land(NativeActionShadow&, PlayerId, FleetId, bool);
    [[nodiscard]] core::Result<void> stage_native_orbit_recheck(NativeActionShadow&, PlayerId, FleetId);
    [[nodiscard]] core::Result<NativeTransportLandingReceipt> stage_native_actor_land(NativeActionShadow&, PlayerId, EntityId,
        PlanetId, bool, std::optional<std::uint32_t>);
    [[nodiscard]] core::Result<NativeProductionReceipt> stage_native_production(NativeActionShadow&, const NativeProductionRequest&, const PartitionExecutor&);
    [[nodiscard]] core::Result<math::Fixed> stage_native_credits(NativeActionShadow&, PlayerId, math::Fixed, math::Fixed, bool advance = true, bool record = true);
    [[nodiscard]] core::Result<std::vector<EntityId>> stage_native_force(NativeActionShadow&, const NativeProductionRequest&, const NativeForceBuilder&);
    [[nodiscard]] const NativeInvasion* native_shadow_invasion(const NativeActionShadow&, std::uint64_t) const noexcept;
    [[nodiscard]] const NativeInvasion* native_shadow_pending_invasion(const NativeActionShadow&) const noexcept;
    [[nodiscard]] core::Result<NativeInvasionReceipt> stage_native_invasion(NativeActionShadow&, PlayerId, FleetId,
        std::span<const EntityId>, bool);
    [[nodiscard]] core::Result<void> stage_native_production_finish(NativeActionShadow&, EntityId, bool, math::Fixed);
    [[nodiscard]] core::Result<void> stage_native_retire_invasion(NativeActionShadow&, std::uint64_t);
    [[nodiscard]] core::Result<NativeTransportLandingGate> native_shadow_landing_available(const NativeActionShadow&,
        PlayerId, PlanetId, std::optional<std::uint32_t>) const;
    [[nodiscard]] core::Result<void> stage_native_effect(NativeActionShadow&, NativeEffectKind, std::vector<std::uint8_t>, NativeEffectPhase);
    explicit GalacticSession(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace eawr::sim::galactic
