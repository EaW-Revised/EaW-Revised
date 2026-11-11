#pragma once

#include "galactic_ai_execution_state.hpp"
#include "galactic_ai_funding.hpp"
#include "galactic_ai_selector_runtime.hpp"
#include "galactic_ai_freestore.hpp"

#include <algorithm>
#include <functional> // include-guard: keep
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace eawr::script::foc::galactic_ai {
// W1 S3: a pay-as-you-go bill as a pure function of the AI budget, the purchase row and the notice. The direct path decides
// against the live AI state; the async lane decides against the billing view and the AI applies the recorded effect later.
struct BillRow {
    gc::PlayerId player{};
    std::uint64_t goal{};
    sim::math::Fixed price{}, billed{}, marked{}, remaining{};
    bool orphan{};
    bool operator==(const BillRow&) const = default;
};
struct BillEffect {
    enum class Kind : std::uint8_t { none, mark, spend };
    Kind kind{};
    gc::PlayerId payer{};
    std::uint64_t goal{}; // spend: 0 charges the dead budget
    sim::math::Fixed amount{}, remaining{};
    bool operator==(const BillEffect&) const = default;
};
struct BillDecision {
    gc::GalacticSession::NativeProductionPayment payment;
    BillEffect effect;
};
using BillLedgers = std::function<const gc::funding::Ledger*(gc::PlayerId)>;
[[nodiscard]] core::Result<BillDecision> decide_bill(const BillRow&, const gc::GalacticSession::NativeProductionNotice&,
    const BillLedgers&, std::span<const gc::ProductionPlayer>);
// Where a recorded effect lands in a ledger that may have moved on since the decision: a mark for a goal that is gone is dropped
// (its obligations moved to the dead goals when it was removed); a spend the goal or its dead obligation cannot take goes to the
// dead budget, as orphan billing does. The live goal, as decided, is the common case.
[[nodiscard]] bool bill_mark_applies(const gc::funding::Ledger&, const BillEffect&);
[[nodiscard]] std::uint64_t bill_spend_goal(const gc::funding::Ledger&, const BillEffect&);
[[nodiscard]] core::Result<gc::funding::Ledger> apply_bill_ledger(gc::funding::Ledger, const BillEffect&);
void apply_bill_row(BillRow&, const BillEffect&);

// Ordered native adapter. Default Lua transactions stage commands; the bounded
// immediate host exposes private cash/placement writes and publishes at the barrier.
class NativeActions final : public Actions, public TrackedStateContributor {
    struct BoundState;
public:
    // Serial root phase: per-key purchase/movement undo, moved result scratch,
    // scalar cursors and the qualified freestore's sparse savepoint. World and
    // funding have separate participants; restore them before native goals.
    class PhaseSavepoint {
    public:
        struct Work { std::size_t purchases{}, movements{}, created_players{}; };
        ~PhaseSavepoint();
        PhaseSavepoint(const PhaseSavepoint&) = delete;
        PhaseSavepoint& operator=(const PhaseSavepoint&) = delete;
        [[nodiscard]] Work work() const noexcept;
        [[nodiscard]] core::Result<void> commit();
        [[nodiscard]] core::Result<void> rollback();
    private:
        struct Saved;
        explicit PhaseSavepoint(std::unique_ptr<Saved>);
        std::unique_ptr<Saved> saved_;
        friend class NativeActions;
    };
    [[nodiscard]] core::Result<std::unique_ptr<PhaseSavepoint>> begin_phase();
    [[nodiscard]] core::Result<void> prepare_player_frontiers(const sim::PartitionExecutor&,
        std::span<const gc::PlayerId>);
    class PlayerFrontier {
    public:
        PlayerFrontier(const PlayerFrontier&) = delete;
        PlayerFrontier& operator=(const PlayerFrontier&) = delete;
        ~PlayerFrontier();
    private:
        friend class NativeActions;
        PlayerFrontier(NativeActions&, gc::PlayerId);
        const NativeActions* previous_owner_{};
        BoundState* previous_frontier_{};
        std::unique_lock<std::recursive_mutex> lock_;
    };
    [[nodiscard]] PlayerFrontier player_frontier(gc::PlayerId player) { return PlayerFrontier(*this, player); }
    [[nodiscard]] std::optional<std::uint64_t> state_revision() const noexcept override { return bound().revision; }
    void mark_state_changed() const noexcept { if (++bound().revision == 0) std::terminate(); }
    struct Purchase {
        gc::PlayerId player{};
        std::uint64_t goal{};
        sim::math::Fixed price{}, billed{}, marked{}, remaining{};
        bool orphan{};
        bool completed{}, failed{};
        sim::EntityId object{};
        gc::PlanetId producer{};
        gc::TypeId requested_type{};
        gc::ProductionDomain channel{};
        bool task_finished{}, salvage{};
        bool operator==(const Purchase&) const = default;
    };
    using GoalResolver = std::function<std::optional<std::uint64_t>(gc::PlayerId, std::uint64_t plan)>;
    using BaseCost = std::function<core::Result<sim::math::Fixed>(gc::TypeId)>;
    using Placement = std::function<core::Result<gc::GalacticSession::NativeForcePlacement>(const ActionRequest&, const gc::GalacticSnapshot&, sim::EntityId)>;
    using ShadowPlacement = std::function<core::Result<gc::GalacticSession::NativeForcePlacement>(const ActionRequest&, const gc::GalacticSession::NativeForceView&, sim::EntityId)>;
    using ReconciliationPlacement = std::function<core::Result<std::optional<gc::GalacticSession::NativeForcePlacement>>(
        const ActionRequest&, const gc::GalacticSession::NativeForceView&, sim::EntityId)>;
    void bind_reconciliation_placement(ReconciliationPlacement placement) { reconciliation_placement_ = std::move(placement); }
    [[nodiscard]] std::shared_ptr<authoritative::ImmediateEffectHost> immediate_host(ShadowPlacement, std::size_t max_requests);
    [[nodiscard]] const gc::GalacticSession::NativeActionShadow::Work& immediate_work() const noexcept { return bound().immediate_work_; }
    NativeActions(gc::GalacticSession&, std::shared_ptr<ai::GalacticFunding>,
        Placement, std::map<gc::PlayerId, sim::math::Fixed> multipliers, GoalResolver = {}, const sim::PartitionExecutor* = nullptr, BaseCost = {});
    ~NativeActions() override;
    NativeActions(const NativeActions&) = delete;
    NativeActions& operator=(const NativeActions&) = delete;
    // Runtime handlers are recreated before the first producer service/load resume.
    void bind_handlers();
    // Actor lifecycle batches in two parts: the facts are read from the delivery snapshot when the world delivers the batch (sim side),
    // the owner applies them to its retained freestore (AI side), at once or at the next async window start (A2.1).
    using ActorFacts = std::vector<std::pair<sim::EntityId, FreestoreFacts>>; // ascending actor, one row per non-lost actor
    using ActorFactsReader = std::function<core::Result<ActorFacts>(const gc::GalacticSnapshot&,
        std::span<const gc::GalacticSession::NativeActorChange>)>;
    using ActorChanges = std::function<core::Result<void>(NativeActions&, std::span<const gc::GalacticSession::NativeActorChange>,
        const ActorFacts&)>;
    void bind_actor_changes(ActorFactsReader read, ActorChanges apply) { actor_facts_ = std::move(read); actor_changes_ = std::move(apply); }
    // A2.1 sim->AI deliveries. While the bound sink takes them (async schedule, outside the AI window), the world's handlers hand an
    // owned copy to the sink instead of writing AI state; the owner applies it in order at the next window start (apply_delivery).
    struct ActorChangesDelivery {
        std::vector<gc::GalacticSession::NativeActorChange> changes;
        ActorFacts facts;
    };
    using Delivery = std::variant<gc::GalacticSession::NativeProductionOutcome, gc::GalacticEvent, ActorChangesDelivery>;
    using DeliverySink = std::function<bool(Delivery&)>; // true: the sink took the delivery
    void bind_delivery_sink(DeliverySink sink) { delivery_sink_ = std::move(sink); }
    [[nodiscard]] core::Result<void> apply_delivery(const Delivery&);
    // W1 S3 funding. A sink that answers a world bill or credit notice outside the AI window (async lane: the billing view decides
    // and the AI applies the recorded effect at the next window start). No value: not taken, the AI answers at once as before.
    using BillingSink = std::function<std::optional<core::Result<gc::GalacticSession::NativeProductionPayment>>(
        const gc::GalacticSession::NativeProductionNotice&)>;
    using CreditSink = std::function<std::optional<core::Result<void>>(const gc::GalacticSession::CreditNotification&)>;
    void bind_funding_sinks(BillingSink billing, CreditSink credits) { billing_sink_ = std::move(billing); credit_sink_ = std::move(credits); }
    [[nodiscard]] static BillRow bill_row(const Purchase&);
    [[nodiscard]] std::map<sim::EntityId, BillRow> bill_rows() const;
    // The bill decision against the AI's current ledgers and purchase row, without writing either.
    [[nodiscard]] core::Result<BillDecision> decide_live_bill(const gc::GalacticSession::NativeProductionNotice&) const;
    // The recorded outcome of a bill decided outside the window, applied to the AI's ledger and purchase row (never decided again).
    [[nodiscard]] core::Result<void> apply_bill_effect(sim::EntityId build, const BillEffect&);
    [[nodiscard]] core::Result<void> apply_credit(const gc::GalacticSession::CreditNotification&);
    using FreestoreBirths = std::function<core::Result<void>(QualifiedFreestore&, const gc::GalacticSnapshot&,
        const gc::GalacticSession::NativeActionShadow&, std::span<const gc::GalacticSession::NativeActorChange>,
        const sim::PartitionExecutor&)>;
    void bind_freestore_births(FreestoreBirths callback) { freestore_births_ = std::move(callback); }
    void set_executor(const sim::PartitionExecutor* executor) noexcept { executor_ = executor; }
    // A decision chain selects its inline adapter without changing the barrier's shared adapter.
    class ExecutorScope {
    public:
        ExecutorScope(const NativeActions*, const sim::PartitionExecutor*) noexcept;
        ExecutorScope(const ExecutorScope&) = delete;
        ExecutorScope& operator=(const ExecutorScope&) = delete;
        ~ExecutorScope();
    private:
        const NativeActions* previous_owner_{};
        const sim::PartitionExecutor* previous_executor_{};
    };
    // Reconstruction only: installed before world replay, then replaced by the
    // restored native owner's bind_handlers. Does not initialize or service AI.
    static void bind_economic_replay_owners(gc::GalacticSession&, Placement = {});
    [[nodiscard]] bool supports(ActionKind) const noexcept override;
    [[nodiscard]] core::Result<ActionReceipt> admit(const ActionRequest&) override;
    [[nodiscard]] core::Result<ActionReceipt> admit_paid(const ActionRequest&, std::uint64_t goal);
    [[nodiscard]] core::Result<void> finish_task(sim::EntityId build, bool salvage);
    // WGA-228: copied ordinary-world facts, never execution route-cost rows.
    struct MovementInput {
        std::vector<gc::PathPlanet> access;
        std::map<gc::PlanetId, gc::PlayerId> orbital_occupants;
        bool ai_player{}, multiplayer{}, benefits_from_corruption{};
    };
    struct Movement {
        gc::PlayerId player{};
        sim::EntityId actor{};
        gc::PlanetId destination{}, conflict_location{};
        MovementReach reach{};
        bool taskforce{}, finished{}, advance_requested{};
        std::uint64_t observed_tick{}, event_cursor{};
        std::uint64_t plan_id{}, taskforce_id{}, block_id{};
        bool registered{true};
        sim::EntityId freestore_actor{}; // WGA-307/313: retained selected identity
        bool operator==(const Movement&) const = default;
    };
    using MovementInventoryValidator = std::function<core::Result<void>(const std::map<std::uint64_t, Movement>&)>;
    void bind_movement_inventory(MovementInventoryValidator validator) { movement_inventory_ = std::move(validator); }
    using MovementReferenceValidator = std::function<core::Result<void>(std::uint64_t request, const Movement&)>;
    void bind_movement_references(MovementReferenceValidator validator) { movement_references_ = std::move(validator); }
    using MovementProvider = std::function<core::Result<MovementInput>(gc::PlayerId, const gc::GalacticSnapshot&)>;
    using ConflictPending = std::function<bool()>;
    // Player-command management gate is distinct from WGA-229's global
    // conflict-list gate. Both runtime predicates must come from the owner.
    void bind_admission_gate(ConflictPending pending_local_conflict) { admission_pending_ = std::move(pending_local_conflict); }
    // WGA-230's relink/gather/conflict insertion and AI bribe flag need their
    // real owners. An absent hook refuses that boundary rather than inventing it.
    using NoHop = std::function<core::Result<void>(gc::PlayerId, sim::EntityId)>;
    using BribeHop = std::function<core::Result<void>(gc::PlayerId, sim::EntityId, gc::PlanetId)>;
    void bind_movement(MovementProvider, ConflictPending, NoHop = {}, BribeHop = {});
    using MovementFrontier = std::function<core::Result<void>(gc::PlayerId,
        std::shared_ptr<const gc::GalacticSnapshot>)>;
    void bind_movement_frontier(MovementFrontier frontier) { movement_frontier_ = std::move(frontier); }
    [[nodiscard]] core::Result<Movement> poll_movement(std::uint64_t request, bool taskforce_has_members = true);
    // Test seam: reads the orbit from the live world. Production goes through the battle listener with the notice's orbits
    // (the overload below); no source caller may use this one.
    [[nodiscard]] core::Result<void> movement_conflict(std::uint64_t request, gc::PlanetId location, bool begin);
    // The same, with the fleets in orbit of `location` when the battle notice was taken (ascending): a delivery applied later
    // than its notice never reads the world of its apply tick.
    [[nodiscard]] core::Result<void> movement_conflict(std::uint64_t request, gc::PlanetId location, bool begin,
        std::span<const sim::EntityId> orbiting);
    // Async lane: a movement polled inside the batch counted the lane view's events. Once the lane is dropped the cursor
    // may only point into the live world's events of the same tick.
    [[nodiscard]] core::Result<void> settle_movement_cursors();
    [[nodiscard]] core::Result<void> invalidate_freestore_actor(std::uint64_t request, gc::FleetId subject);
    [[nodiscard]] core::Result<void> retire_movement(std::uint64_t request);
    // GFS-A22 ordered commit: the merged world refused a move the player projected as admitted.
    // Ends that player's unfinished block for the actor, as a refused hop does in advance_movement.
    // Returns whether a retained row changed; idempotent.
    [[nodiscard]] core::Result<bool> refuse_movement(gc::PlayerId, gc::FleetId actor);
    // GFS-A22 ordered commit: the merged world refused a speculative paid build. Marks the retained
    // purchase failed, like a failed-build outcome; the goal then refunds its mark as usual.
    // Returns whether a retained row changed; idempotent.
    [[nodiscard]] core::Result<bool> refuse_purchase(gc::PlayerId, sim::EntityId build);
    [[nodiscard]] core::Result<void> retire_freestore_movement(sim::EntityId actor, bool actor_lost = false);
    struct FreestoreMoveInput {
        bool actor_valid{}, source_ai_usable{};
        sim::EntityId grandparent{};
    };
    using FreestoreMoveProvider = std::function<core::Result<FreestoreMoveInput>(
        gc::PlayerId, const FreestoreFacts&, const gc::GalacticSnapshot&)>;
    void bind_freestore_movement(FreestoreMoveProvider reader) { freestore_move_ = std::move(reader); }
    // WGA-438: the player-independent planetary predicate behind Lua Get_Is_Planet_AI_Usable.
    using PlanetUsability = std::function<core::Result<bool>(const gc::GalacticSnapshot&, gc::PlanetId)>;
    void bind_planet_usability(PlanetUsability reader) { planet_usability_ = std::move(reader); }
    struct FreestorePathInput {
        std::vector<gc::PlanetId> planets;
        Number threat{};
        std::optional<gc::PlayerId> next_occupant; // unknown differs from known empty (0)
    };
    using FreestorePathProvider = std::function<core::Result<FreestorePathInput>(gc::PlayerId,
        const gc::GalacticSnapshot&, gc::PlanetId source, gc::PlanetId destination)>;
    void bind_freestore_path(FreestorePathProvider, ConflictPending);
    enum class FreestoreMove { refused, already_there, move };
    [[nodiscard]] core::Result<FreestoreMove> preflight_freestore_move(
        const QualifiedFreestore&, gc::PlayerId, sim::EntityId, gc::PlanetId) const;
    // Commit only after partitioned Lua service, preserving native event order.
    [[nodiscard]] core::Result<bool> admit_freestore_move(gc::PlayerId, sim::EntityId, gc::PlanetId);
    [[nodiscard]] core::Result<bool> commit_freestore_move(const authoritative::ScriptCommand&);
    // WGA-307: native unit service retires completed blocks; not the Lua cadence.
    [[nodiscard]] core::Result<void> service_freestore_movement(gc::PlayerId, sim::EntityId);
    // WGA-405..416: immutable cached frontier, partitioned decisions, ordered links.
    [[nodiscard]] core::Result<void> service_freestore_landing(gc::PlayerId,
        const FreestorePool&, const sim::PartitionExecutor&);
    [[nodiscard]] std::uint64_t freestore_move_refusals() const noexcept { return bound().freestore_move_refusals_; }
    [[nodiscard]] const std::map<std::uint64_t, Movement>& movements() const noexcept { return bound().movements_; }
    // Completed owner identities for TaskForce formation/launch, independent
    // of tactical scene parents and the copied perception generation.
    [[nodiscard]] std::optional<gc::FleetId> containing_fleet(gc::PlayerId, sim::EntityId) const;
    struct AssemblyLocation { gc::PlanetId planet{}; gc::FleetId fleet{}; bool landed{}, transit{}; };
    [[nodiscard]] std::optional<AssemblyLocation> assembly_location(gc::PlayerId, sim::EntityId) const;
    // WGA-485: ordinary per-actor staging, independent of Launch_Units' batch.
    [[nodiscard]] core::Result<ActionReceipt> prepare_assembly_actor(gc::PlayerId, sim::EntityId, bool separate = true);
    [[nodiscard]] core::Result<void> land_assembly_arrival(gc::PlayerId, sim::EntityId);
    [[nodiscard]] bool movement_actor_exists(sim::EntityId) const;
    [[nodiscard]] const gc::GalacticSession::NativeInvasion* invasion_status(std::uint64_t request) const noexcept;
    [[nodiscard]] core::Result<void> retire_invasion(std::uint64_t request);

    [[nodiscard]] core::Result<ai::GalacticBuildStatus> build_status(gc::PlayerId, sim::EntityId build) const;
    [[nodiscard]] core::Result<sim::EntityId> build_actor(gc::PlayerId, sim::EntityId build) const;
    [[nodiscard]] core::Result<sim::math::Fixed> generic_build_cost(gc::TypeId type) const;
    [[nodiscard]] std::shared_ptr<const gc::GalacticSnapshot> snapshot() const { return world_.snapshot(); }
    // W1 S6 (async lane): a sink that takes a runtime load note raised inside the AI window (published with it and appended by
    // the world at its apply tick). False: not taken, the note goes to the world at once.
    using LoadNoteSink = std::function<bool(core::LoadNote&)>;
    void bind_load_note_sink(LoadNoteSink sink) { load_note_sink_ = std::move(sink); }
    // Runtime diagnostics are a sidecar, outside native state and replay bytes.
    void note_battle_delivery(std::uint64_t request, const core::Diagnostic& diagnostic) {
        core::LoadNote note;
        note.code = "EAWR-LOAD-1236"; note.object = std::to_string(request);
        note.tag = "battle_delivery"; note.phase = core::LoadPhase::runtime;
        note.action = core::LoadAction::runtime_degraded;
        note.effective_value = "unavailable AI battle notification skipped; battle continues";
        note.diagnostic_code = diagnostic.code; note.message = diagnostic.message;
        if (load_note_sink_ && load_note_sink_(note)) return;
        world_.add_runtime_load_notes(std::span<const core::LoadNote>(&note, 1));
    }
    // Direct native actions can allocate after the AI perception view was bound.
    [[nodiscard]] sim::EntityId next_entity_id() const noexcept { return world_.next_entity_id(); }
    struct BuildHead {
        gc::PlanetId producer{};
        gc::ProductionDomain channel{};
        std::optional<gc::ProductionEntry> entry;
        ai::Real progress{}, remaining_seconds{};
    };
    // WGA-343/344: front only, sampled progress and current countdown separately.
    [[nodiscard]] core::Result<BuildHead> build_head(gc::PlayerId, sim::EntityId build) const;
    [[nodiscard]] core::Result<sim::math::Fixed> build_remaining_cost(gc::PlayerId, sim::EntityId build) const;
    [[nodiscard]] core::Result<NativeState> capture() const override;
    [[nodiscard]] core::Result<void> restore(const NativeState&) override;
    [[nodiscard]] core::Result<std::string> state_hash() const override;
    [[nodiscard]] core::Result<std::string> record_sync_hash(bool cold = false) const override;
    [[nodiscard]] std::size_t record_byte_upper_bound() const noexcept override {
        const auto store = bound().freestore_->record_byte_upper_bound();
        return records_dirty_ || store == 0 ? 0 : bound().records_.byte_size() + store + 1024;
    }
    [[nodiscard]] std::uint64_t build_state_revision() const noexcept { return bound().build_revision_; }
    [[nodiscard]] const std::map<sim::EntityId, Purchase>& purchases() const noexcept { return bound().purchases_; }
    [[nodiscard]] QualifiedFreestore& qualified_freestore() noexcept { return *bound().freestore_; }
    [[nodiscard]] const QualifiedFreestore& qualified_freestore() const noexcept { return *bound().freestore_; }
    [[nodiscard]] std::weak_ptr<QualifiedFreestore> freestore_owner() const noexcept { return bound().freestore_; }
    using FreestoreRestoreValidator = std::function<core::Result<void>(const QualifiedFreestore&)>;
    void bind_freestore_validation(FreestoreRestoreValidator validator) { freestore_validation_ = std::move(validator); }
    // Async lane: world events the AI has not received yet (they wait for the next window start). A restore checks the saved
    // freestore against the world only after these pending actor batches are replayed onto a staged copy of it.
    using PendingFreestoreReplay = std::function<core::Result<void>(QualifiedFreestore&)>;
    void bind_pending_freestore_replay(PendingFreestoreReplay replay) { pending_freestore_replay_ = std::move(replay); }
    // Async lane (W1 S3): bills the world decided against the billing view that the AI has not applied yet. A restore compares
    // the production queue with each purchase row as it will be after they apply (the newest pending remaining time per build).
    using PendingBillRemaining = std::function<std::optional<sim::math::Fixed>(sim::EntityId)>;
    void bind_pending_bill_remaining(PendingBillRemaining pending) { pending_bill_remaining_ = std::move(pending); }
    // Async lane: identities the AI holds beyond the world's frontier (intents decided but not applied yet).
    void bind_identity_ceiling(std::function<sim::EntityId()> ceiling) { identity_ceiling_ = std::move(ceiling); }
    // Identities from this one up are the lane's: objects the AI holds that the world creates when their slice applies.
    void bind_lane_floor(std::function<sim::EntityId()> floor) { lane_floor_ = std::move(floor); }
    // Invasion requests the AI holds that the world creates when their slice applies (decided, not applied yet).
    // The lane's pending intents answer it, not the world's counter: that already stands past the window's reserved block.
    void bind_invasion_pending(std::function<bool(std::uint64_t)> pending) { invasion_pending_ = std::move(pending); }
    [[nodiscard]] bool invasion_pending(std::uint64_t request) const { return invasion_pending_ && invasion_pending_(request); }
    [[nodiscard]] sim::EntityId lane_floor() const { return lane_floor_ ? lane_floor_() : sim::EntityId{}; }
    [[nodiscard]] sim::EntityId identity_ceiling() const { return std::max(world_.next_entity_id(), identity_ceiling_ ? identity_ceiling_() : sim::EntityId{}); }
    [[nodiscard]] bool qualified_freestore_available() const noexcept { return static_cast<bool>(freestore_validation_); }
    [[nodiscard]] std::span<const sim::EntityId> created(gc::PlayerId) const noexcept;
private:
    void update_record(std::uint32_t kind, std::uint64_t id);
    void update_metadata_record();
    [[nodiscard]] core::RecordDigest rebuild_records() const;
    struct RecordWrite {
        NativeActions& owner; std::uint32_t kind{}; std::uint64_t id{};
        RecordWrite(NativeActions& target, std::uint32_t type, std::uint64_t key) : owner(target), kind(type), id(key) { owner.retain_record(type, key); }
        ~RecordWrite() { owner.update_record(kind, id); }
    };
    void retain_record(std::uint32_t, std::uint64_t);
    void clear_created(gc::PlayerId);
    PhaseSavepoint::Saved* phase_{};
    bool phase_abandoned_{}, records_dirty_{};
    void mark_build_state_changed() const noexcept {
        if (++bound().build_revision_ == 0) std::terminate();
        mark_state_changed();
    }
    struct ImmediateState;
    struct ImmediateHost;
    [[nodiscard]] core::Result<gc::GalacticSession::NativeProductionPayment> notice(const gc::GalacticSession::NativeProductionNotice&);
    [[nodiscard]] core::Result<gc::GalacticSession::NativeProductionPayment> notice_unrecorded(const gc::GalacticSession::NativeProductionNotice&);
    [[nodiscard]] core::Result<void> advance_movement(Movement&, bool taskforce_has_members = true);
    [[nodiscard]] core::Result<void> advance_freestore_movement(Movement&);
    void bind_movement_listener();
    // A2.1: a movement event outside the async window goes to the delivery sink; otherwise it applies at once.
    void on_movement_event(const gc::GalacticEvent&);
    void apply_movement_event(const gc::GalacticEvent&);
    [[nodiscard]] core::Result<ActionReceipt> admit_movement(const ActionRequest&);
    [[nodiscard]] core::Result<void> refresh_freestore_movement(sim::EntityId, gc::FleetId, bool tracked);
    gc::GalacticSession& world_;
    std::shared_ptr<ai::GalacticFunding> funding_;
    Placement placement_;
    ReconciliationPlacement reconciliation_placement_;
    std::map<gc::PlayerId, sim::math::Fixed> multipliers_;
    GoalResolver goal_resolver_;
    BaseCost base_cost_;
    const sim::PartitionExecutor* executor_{};
    static thread_local const NativeActions* executor_owner_;
    static thread_local const sim::PartitionExecutor* scoped_executor_;
    [[nodiscard]] const sim::PartitionExecutor* current_executor() const noexcept {
        return executor_owner_ == this ? scoped_executor_ : executor_;
    }
    std::shared_ptr<NativeActions*> lifetime_;
    FreestoreRestoreValidator freestore_validation_;
    PendingFreestoreReplay pending_freestore_replay_;
    PendingBillRemaining pending_bill_remaining_;
    std::function<sim::EntityId()> identity_ceiling_;
    std::function<sim::EntityId()> lane_floor_;
    std::function<bool(std::uint64_t)> invasion_pending_;
    ActorFactsReader actor_facts_;
    ActorChanges actor_changes_;
    DeliverySink delivery_sink_;
    BillingSink billing_sink_;
    CreditSink credit_sink_;
    LoadNoteSink load_note_sink_;
    void apply_production_outcome(const gc::GalacticSession::NativeProductionOutcome&);
    FreestoreBirths freestore_births_;
    FreestoreMoveProvider freestore_move_;
    PlanetUsability planet_usability_;
    FreestorePathProvider freestore_path_;
    MovementProvider movement_provider_;
    MovementFrontier movement_frontier_;
    MovementReferenceValidator movement_references_;
    MovementInventoryValidator movement_inventory_;
    ConflictPending conflict_pending_;
    ConflictPending admission_pending_;
    NoHop no_hop_;
    BribeHop bribe_hop_;
    struct PlayerScratch {
        std::optional<ActionRequest> pending;
        std::uint64_t pending_goal{};
        std::vector<sim::EntityId> created;
        std::vector<gc::PlanetId> landing_stage;
        std::vector<FreestoreSurface> landing_surfaces;
    };
    // Physical retained state selected by the decision owner. Callbacks remain
    // on this facade and resolve the same scope as ordinary adapter calls.
    struct BoundState {
        mutable std::uint64_t revision{1};
        core::RecordDigest records_;
        mutable std::uint64_t build_revision_{}; // derived; quiet movement cannot dirty task publications
        gc::GalacticSession::NativeActionShadow::Work immediate_work_;
        std::map<sim::EntityId, Purchase> purchases_;
        std::shared_ptr<QualifiedFreestore> freestore_;
        std::map<std::uint64_t, Movement> movements_;
        std::map<sim::EntityId, std::uint64_t> freestore_requests_; // derived registered-request index
        std::uint64_t freestore_move_refusals_{}; // saved project seam counter, silent Lua false
        std::uint64_t next_movement_{1};
        std::map<gc::PlayerId, PlayerScratch> player_scratch_;
    };
    mutable BoundState root_;
    struct Frontier { BoundState state; std::recursive_mutex mutex; };
    std::map<gc::PlayerId, Frontier> frontiers_;
    std::vector<gc::PlayerId> prepared_players_;
    static thread_local const NativeActions* frontier_owner_;
    static thread_local BoundState* active_frontier_;
    [[nodiscard]] BoundState& bound() const noexcept {
        return frontier_owner_ == this ? *active_frontier_ : root_;
    }
    [[nodiscard]] bool pending_admission() const noexcept;
};
} // namespace eawr::script::foc::galactic_ai
