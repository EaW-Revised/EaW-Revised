#pragma once

#include "galactic_ai_execution_state.hpp"
#include "galactic_ai_funding.hpp"
#include "galactic_ai_selector_runtime.hpp"
#include "galactic_ai_freestore.hpp"

namespace eawr::script::foc::galactic_ai {
// Ordered native adapter. Default Lua transactions stage commands; the bounded
// immediate host exposes private cash/placement writes and publishes at the barrier.
class NativeActions final : public Actions, public TrackedStateContributor {
public:
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
    [[nodiscard]] std::shared_ptr<authoritative::ImmediateEffectHost> immediate_host(ShadowPlacement, std::size_t max_requests);
    [[nodiscard]] const gc::GalacticSession::NativeActionShadow::Work& immediate_work() const noexcept { return immediate_work_; }
    NativeActions(gc::GalacticSession&, std::shared_ptr<ai::GalacticFunding>,
        Placement, std::map<gc::PlayerId, sim::math::Fixed> multipliers, GoalResolver = {}, const sim::PartitionExecutor* = nullptr, BaseCost = {});
    ~NativeActions() override;
    NativeActions(const NativeActions&) = delete;
    NativeActions& operator=(const NativeActions&) = delete;
    // Runtime handlers are recreated before the first producer service/load resume.
    void bind_handlers();
    using ActorChanges = std::function<core::Result<void>(NativeActions&, const gc::GalacticSnapshot&,
        std::span<const gc::GalacticSession::NativeActorChange>)>;
    void bind_actor_changes(ActorChanges callback) { actor_changes_ = std::move(callback); }
    using FreestoreBirths = std::function<core::Result<void>(QualifiedFreestore&, const gc::GalacticSnapshot&,
        const gc::GalacticSession::NativeActionShadow&, std::span<const gc::GalacticSession::NativeActorChange>,
        const sim::PartitionExecutor&)>;
    void bind_freestore_births(FreestoreBirths callback) { freestore_births_ = std::move(callback); }
    void set_executor(const sim::PartitionExecutor* executor) noexcept { executor_ = executor; }
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
    [[nodiscard]] core::Result<Movement> poll_movement(std::uint64_t request, bool taskforce_has_members = true);
    [[nodiscard]] core::Result<void> movement_conflict(std::uint64_t request, gc::PlanetId location, bool begin);
    [[nodiscard]] core::Result<void> invalidate_freestore_actor(std::uint64_t request, gc::FleetId subject);
    [[nodiscard]] core::Result<void> retire_movement(std::uint64_t request);
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
    [[nodiscard]] std::uint64_t freestore_move_refusals() const noexcept { return freestore_move_refusals_; }
    [[nodiscard]] const std::map<std::uint64_t, Movement>& movements() const noexcept { return movements_; }
    // Completed owner identities for TaskForce formation/launch, independent
    // of tactical scene parents and the copied perception generation.
    [[nodiscard]] std::optional<gc::FleetId> containing_fleet(gc::PlayerId, sim::EntityId) const;
    [[nodiscard]] bool movement_actor_exists(sim::EntityId) const;
    [[nodiscard]] const gc::GalacticSession::NativeInvasion* invasion_status(std::uint64_t request) const noexcept;
    [[nodiscard]] core::Result<void> retire_invasion(std::uint64_t request);

    [[nodiscard]] core::Result<ai::GalacticBuildStatus> build_status(gc::PlayerId, sim::EntityId build) const;
    [[nodiscard]] core::Result<sim::EntityId> build_actor(gc::PlayerId, sim::EntityId build) const;
    [[nodiscard]] core::Result<sim::math::Fixed> generic_build_cost(gc::TypeId type) const;
    [[nodiscard]] std::shared_ptr<const gc::GalacticSnapshot> snapshot() const { return world_.snapshot(); }
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
    [[nodiscard]] std::uint64_t build_state_revision() const noexcept { return build_revision_; }
    [[nodiscard]] const std::map<sim::EntityId, Purchase>& purchases() const noexcept { return purchases_; }
    [[nodiscard]] QualifiedFreestore& qualified_freestore() noexcept { return *freestore_; }
    [[nodiscard]] const QualifiedFreestore& qualified_freestore() const noexcept { return *freestore_; }
    [[nodiscard]] std::weak_ptr<QualifiedFreestore> freestore_owner() const noexcept { return freestore_; }
    using FreestoreRestoreValidator = std::function<core::Result<void>(const QualifiedFreestore&)>;
    void bind_freestore_validation(FreestoreRestoreValidator validator) { freestore_validation_ = std::move(validator); }
    [[nodiscard]] bool qualified_freestore_available() const noexcept { return static_cast<bool>(freestore_validation_); }
    [[nodiscard]] std::span<const sim::EntityId> created() const noexcept { return created_; }
private:
    void mark_build_state_changed() const noexcept {
        if (++build_revision_ == 0) std::terminate();
        mark_state_changed();
    }
    mutable std::uint64_t build_revision_{}; // derived; quiet movement cannot dirty task publications
    struct ImmediateState;
    struct ImmediateHost;
    gc::GalacticSession::NativeActionShadow::Work immediate_work_;
    [[nodiscard]] core::Result<gc::GalacticSession::NativeProductionPayment> notice(const gc::GalacticSession::NativeProductionNotice&);
    [[nodiscard]] core::Result<gc::GalacticSession::NativeProductionPayment> notice_unrecorded(const gc::GalacticSession::NativeProductionNotice&);
    [[nodiscard]] core::Result<void> advance_movement(Movement&, bool taskforce_has_members = true);
    [[nodiscard]] core::Result<void> advance_freestore_movement(Movement&);
    void bind_movement_listener();
    [[nodiscard]] core::Result<ActionReceipt> admit_movement(const ActionRequest&);
    [[nodiscard]] core::Result<void> refresh_freestore_movement(sim::EntityId, gc::FleetId, bool tracked);
    gc::GalacticSession& world_;
    std::shared_ptr<ai::GalacticFunding> funding_;
    Placement placement_;
    std::map<gc::PlayerId, sim::math::Fixed> multipliers_;
    std::map<sim::EntityId, Purchase> purchases_;
    std::optional<ActionRequest> pending_;
    std::uint64_t pending_goal_{};
    GoalResolver goal_resolver_;
    BaseCost base_cost_;
    std::vector<sim::EntityId> created_; // returned by the current admission, derived
    const sim::PartitionExecutor* executor_{};
    std::shared_ptr<NativeActions*> lifetime_;
    std::shared_ptr<QualifiedFreestore> freestore_;
    FreestoreRestoreValidator freestore_validation_;
    ActorChanges actor_changes_;
    FreestoreBirths freestore_births_;
    FreestoreMoveProvider freestore_move_;
    PlanetUsability planet_usability_;
    FreestorePathProvider freestore_path_;
    MovementProvider movement_provider_;
    MovementReferenceValidator movement_references_;
    MovementInventoryValidator movement_inventory_;
    ConflictPending conflict_pending_;
    ConflictPending admission_pending_;
    NoHop no_hop_;
    BribeHop bribe_hop_;
    std::map<std::uint64_t, Movement> movements_;
    std::map<sim::EntityId, std::uint64_t> freestore_requests_; // derived registered-request index
    std::uint64_t freestore_move_refusals_{}; // saved project seam counter, silent Lua false
    std::uint64_t next_movement_{1};
    std::vector<gc::PlanetId> landing_stage_; // derived reusable disjoint staging
    std::vector<FreestoreSurface> landing_surfaces_; // ordered live capacity frontier
};
} // namespace eawr::script::foc::galactic_ai
