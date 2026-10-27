#pragma once
#include "encoding.hpp"
#include "paused_admission.hpp"
#include "native_income_projection.hpp"
#include "native_population_projection.hpp"

#include "eawr/sim/galactic/session.hpp"
#include "eawr/sim/galactic/income.hpp"

#include "../../../third_party/entt/single_include/entt/entt.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace eawr::sim::galactic {

namespace session_detail {

// Private ECS components: plain values, no handles or references to other objects.
struct StableId {
    EntityId value{};
};
struct PlanetData {
    math::Vec3 position{};
    PlayerId owner{};
};
struct RouteData {
    PlanetId from{};
    PlanetId to{};
    math::Fixed hs_speed_factor{math::Fixed::from_raw(math::Fixed::scale)};
};
struct FleetData {
    PlayerId owner{};
    PlanetId orbit{};
    std::vector<FleetMember> members;
    std::uint8_t orbit_slot{};
    FleetMotion motion{};
    std::optional<PlanetId> raid_destination{};
};

enum class ObjectKind : std::uint8_t { planet, route, fleet };

// One object's copied state, gathered in ascending stable-ID order by the galactic-gather phase:
// the immutable input of every per-object phase of the tick.
struct GatheredObject {
    EntityId id{};
    ObjectKind kind{ObjectKind::planet};
    PlanetData planet{};
    RouteData route{};
    FleetData fleet{};
};

} // namespace session_detail

class GalacticSession::Impl {
public:
    GalacticSetup setup;
    std::shared_ptr<const session_detail::NativePopulationSchema> native_population_schema;
    std::shared_ptr<const IncomeSetup> native_income_schema;
    std::shared_ptr<const FiscalSetup> native_fiscal_schema;
    std::optional<VictoryState> victory;
    bool victory_tail_deferred{}; // runtime owning mode dependency, never serialized
    void service_victory_tail(std::uint64_t tick, std::vector<GalacticEvent>& events);
    [[nodiscard]] VictoryContext victory_context() const;
    void update_victory_actor(const CensusObject&);
    void remove_victory_actor(EntityId);
    [[nodiscard]] core::Result<void> test_conditions(const PartitionExecutor&, std::span<const VictoryKind>, std::uint64_t, std::vector<GalacticEvent>&);
    [[nodiscard]] core::Result<void> test_credit_condition(PlayerId, std::uint64_t, std::vector<GalacticEvent>&);
    std::optional<PlayerEconomySetup> economy; // live ledger; setup remains tick zero
    std::optional<ProductionSetup> production;
    TechnologyNoticeHandler technology_notice_handler;
    FeedbackNoticeHandler feedback_notice_handler;
    [[nodiscard]] core::Result<std::int64_t> assign_technology(const TechnologyWrite&);
    std::optional<IncomeState> income;
    std::optional<EconomyCorruptionState> economy_corruption;
    // GCMP-24: both saved copies come from the one campaign setup mode.
    [[nodiscard]] bool multiplayer_flag() const noexcept {
        return victory ? victory->context.multiplayer : economy_corruption && economy_corruption->multiplayer;
    }
    std::optional<GalacticAbilityState> ability_host;
    // GHUD-S61: raw scoring counters; only ordered commits write them.
    CampaignScoreLedger score_ledger;
    HistoryArchive history_archive;
    bool history_service_deferred{}; // rebound service owner, never saved
    void touch_score_ledger() { dirty[static_cast<std::size_t>(StateSection::campaign_scoring)] = true; }
    void record_build(PlayerId owner, PlanetId planet, TypeId type);
    void record_death(PlayerId owner, PlayerId killer, TypeId type);
    void record_planet_change(PlanetId planet, PlayerId old_owner, PlayerId new_owner);
    std::uint64_t ability_input_generation{1};
    mutable AbilityWork ability_work;
    void touch_abilities() { dirty[static_cast<std::size_t>(StateSection::ability_host)] = true; ++ability_input_generation; }
    AbilityConsumptionHandler ability_consumption_handler;
    GroundAbilityStoryHandler ground_ability_story_handler;
    EntityId consuming_spy{}; // synchronous consumption guard; never survives a command barrier
    CorruptionMissionHandler corruption_mission_handler;
    CorruptionCompletionHandler corruption_completion_handler;
    CorruptionResultHandler corruption_result_handler;
    std::map<std::uint64_t, std::vector<PlanetId>> corruption_due;
    std::span<const TypeId> corruption_local_unlocks(PlanetId, PlayerId) const;
    std::size_t corruption_unit_count(std::span<const TypeId>, PlayerId) const;
    std::shared_ptr<const CorruptionMenu> publish_corruption_menu(PlayerId, std::span<const ObservedPlanet>) const;
    core::Result<bool> begin_corruption(PlanetId, corruption_fixture::Type, math::Fixed);
    core::Result<void> finish_corruption_mission(std::uint64_t, CorruptionMissionState);
    core::Result<void> service_corruption(const PartitionExecutor&, std::size_t preceding_events);
    core::Result<void> apply_corruption_completion(PlanetId, corruption_fixture::Type, corruption_fixture::Type,
        std::optional<PlayerId> = {}, std::optional<std::size_t> preceding_events = {});
    core::Result<void> initialize_corruption();
    // WGCH-67/140..142: the shared live ruins owner; native effect 17 journals the public request.
    PlanetRuinsStoryHandler planet_ruins_story_handler;
    struct RuinsPlan {
        PlanetId planet{};
        bool destroy{true};
        std::vector<HeroDefeat> losses; // WGCH-141 canonical loss requests in execution order
        std::vector<EntityId> landed;    // landed transports removed from the surface list
    };
    // Pure prevalidation over copied inputs; nullopt means no mutation may start (named seam or unavailable).
    [[nodiscard]] std::optional<RuinsPlan> plan_planet_ruins(PlanetId, bool destroy, std::span<const EntityId> excluded = {});
    [[nodiscard]] core::Result<void> execute_planet_ruins(const RuinsPlan&);
    // Reverse container + named-hero traversal shared by ruins (landed transports) and hacking (orbit fleets).
    [[nodiscard]] bool plan_container_loss(EntityId container, TypeId type, PlayerId owner, std::vector<HeroDefeat>& losses,
        std::span<const EntityId> excluded = {});
    // WGCH-57/66/109..112: live hacking over the same host; excluded identities are already leaving.
    struct HackWeapon { EntityId id{}; TypeId type{}; PlayerId owner{}; };
    [[nodiscard]] std::optional<HackWeapon> find_hack_weapon(PlanetId) const;
    [[nodiscard]] bool plan_orbit_loss(PlanetId, std::span<const EntityId> excluded, std::vector<HeroDefeat>& losses);
    [[nodiscard]] core::Result<void> force_planet_owner(PlanetId, PlayerId, std::uint64_t tick, std::vector<GalacticEvent>&);
    void prepare_hack(const GameCommand&, const CensusObject&, const GalacticAbilityProfile&, std::uint64_t, std::vector<GalacticEvent>&);
    void execute_hack(const GameCommand&, const CensusObject&, const PreparedAbility&, std::uint64_t, std::vector<GalacticEvent>&);
    // WGCH-58/113..115/155..158: strategic destruction; the supplied object is the original container.
    [[nodiscard]] std::optional<CensusObject> resolve_destruction_bearer(const CensusObject&) const;
    [[nodiscard]] bool destruction_admitted(const CensusObject& bearer, PlanetId) const;
    void prepare_destruction(const GameCommand&, const CensusObject&, std::uint64_t, std::vector<GalacticEvent>&);
    void execute_destruction(const GameCommand&, const CensusObject&, const PreparedAbility&, std::uint64_t, std::vector<GalacticEvent>&);
    [[nodiscard]] core::Result<bool> planet_destruction_victory(PlayerId actor_owner, PlayerId current_controller, std::span<const TypeId> landed);
    void detach_observer_sources(std::span<const EntityId> sources);
    [[nodiscard]] math::Fixed planet_base_credits(std::size_t fiscal_index) const;
    core::Result<void> capture_corruption(PlanetId, PlayerId, PlayerId);
    void prepare_corruption(const GameCommand&, const CensusObject&, std::uint64_t, std::vector<GalacticEvent>&);
    void execute_corruption(const GameCommand&, const CensusObject&, const PreparedAbility&, std::uint64_t, std::vector<GalacticEvent>&);
    void refresh_ability_actor(const CensusObject&);
    void lose_ability_actor(EntityId);
    const CensusObject* movement_actor(EntityId id) const {
        const auto at = actor_records.find(id); return at == actor_records.end() ? nullptr : &at->second;
    }
    void dispatch_ground_ability(EntityId supplied, PlanetId, SeatId, std::uint64_t sequence, std::uint64_t tick, std::vector<GalacticEvent>&);
    void execute_ability(const GameCommand&, std::uint64_t, std::vector<GalacticEvent>&);
    [[nodiscard]] std::optional<HeroDefeat> prepare_ability_defeat(const CensusObject&, math::Fixed, bool bypass);
    // WGCH-27..30/96..99: live structure sabotage and WGCH-20..26/91..95 hero neutralization on the shared host.
    void release_structure_object(PlanetProduction&, const ProductionObject&);
    [[nodiscard]] bool sabotage_admitted(const CensusObject& actor, PlanetId) const;
    void prepare_sabotage(const GameCommand&, const CensusObject&, std::uint64_t, std::vector<GalacticEvent>&);
    void execute_sabotage(const GameCommand&, const CensusObject&, const PreparedAbility&, std::uint64_t, std::vector<GalacticEvent>&);
    void prepare_neutralization(const GameCommand&, const CensusObject&, std::uint64_t, std::vector<GalacticEvent>&);
    void execute_neutralization(const GameCommand&, const CensusObject&, const PreparedAbility&, std::uint64_t, std::vector<GalacticEvent>&);
    void prepare_theft(const GameCommand&, const CensusObject&, const GalacticAbilityProfile&, std::uint64_t, std::vector<GalacticEvent>&);
    void execute_spy(const GameCommand&, const CensusObject&, const GalacticAbilityProfile&, PlanetId, std::uint64_t, std::vector<GalacticEvent>&);
    void apply_observer(const GameCommand&, std::uint64_t, std::vector<GalacticEvent>&);
    void execute_theft(const GameCommand&, const CensusObject&, const PreparedAbility&, std::uint64_t, std::vector<GalacticEvent>&);
    // Sparse expiry calendar; reconstructed from the saved ledger at creation.
    std::set<std::tuple<std::uint64_t, PlayerId, EntityId>> technology_price_expiry;
    void rebuild_technology_price_expiry();
    void expire_technology_prices();
    [[nodiscard]] core::Result<void> refresh_planet_technology_price(PlanetId, bool ownership = false, bool remove = false);
    [[nodiscard]] core::Result<void> replay_technology_price(GalacticSession&, const NativeEffect&);
    void execute_black_market(const GameCommand&, std::uint64_t, std::vector<GalacticEvent>&);
    // WGEP-163..165: the saved ledger lives in `income->blockers`; this is its derived
    // first expiry. Idle frames compare one integer and neither scan nor allocate.
    std::uint64_t income_blocker_next_expiry{std::numeric_limits<std::uint64_t>::max()};
    [[nodiscard]] bool income_blocked(PlanetId planet) const;
    void apply_income_blocker(PlanetId planet, EntityId source, const std::optional<IncomeBlocker>& row);
    void expire_income_blockers();
    [[nodiscard]] core::Result<void> replay_income_blocker(GalacticSession&, const NativeEffect&);
    // Derived source gather, rebuilt only by relevant ordered commits. Never serialized.
    std::uint64_t income_input_generation{1};
    mutable std::uint64_t income_gather_generation{}, income_sources_gathered{};
    mutable std::optional<IncomeSetup> gathered_income_inputs;
    std::uint64_t income_control_generation{1}; // derived; separate from physical source gather
    using IncomeSourceCandidate = session_detail::NativeIncomeSourceCandidate;
    mutable std::vector<IncomeSourceCandidate> income_source_candidates;
    void invalidate_income_inputs() noexcept { ++income_input_generation; }
    [[nodiscard]] const IncomeSetup& live_income_inputs() const;
    [[nodiscard]] core::Result<void> service_income(const PartitionExecutor&, std::uint64_t,
        std::vector<GalacticEvent>&, bool sample = true);
    [[nodiscard]] core::Result<std::vector<math::Fixed>> income_bonuses(const PartitionExecutor&) const;
    [[nodiscard]] core::Result<std::vector<PlanetIncomeContribution>> income_contributions(const PartitionExecutor&) const;
    struct IncomeForecastCache {
        std::vector<Planet> planets;
        std::vector<ControlRelation> relations;
        std::uint64_t input_generation{};
        std::uint64_t control_generation{};
        std::vector<GamblingIncomeState> gambling;
        std::vector<PlanetIncomeContribution> values;
    };
    mutable std::optional<IncomeForecastCache> income_forecast_cache;
    mutable std::vector<std::vector<std::pair<std::size_t, std::size_t>>> income_edges;
    mutable std::uint64_t income_forecast_rebuilds{}; // derived diagnostic only, never encoded
    mutable std::uint64_t income_modifier_generation{}, income_modifier_rebuilds{};
    mutable std::vector<std::optional<std::vector<PlanetIncomeForecast::Modifier>>> income_modifiers;
    void publish_income_modifiers(EconomyForecast&, const std::vector<Planet>&) const;
    [[nodiscard]] core::Result<std::span<const PlanetIncomeContribution>> forecast_contributions(
        const PartitionExecutor&, const std::vector<Planet>&) const;
    std::vector<CensusObject> production_originals; // live nested originals; tick-zero setup stays immutable
    std::map<EntityId, CensusObject> actor_records; // written only at native commit boundaries
    mutable std::shared_ptr<const std::vector<CensusType>> published_sale_types;
    mutable std::shared_ptr<const std::vector<CensusObject>> published_actor_records;
    std::vector<NativeActorChange> pending_actor_changes;
    void record_actor(CensusObject, NativeActorChange::Kind);
    void relocate_actor(EntityId, PlayerId, PlanetId, EntityId);
    // Fleet-shell actor record for a newly created fleet; a no-op without an actor owner (no census, no nested creation).
    void record_fleet_shell(EntityId, PlayerId, PlanetId);
    void lose_actor(EntityId);
    [[nodiscard]] core::Result<void> deliver_actor_changes();
    // Derived canonical gather for WGM-110; excludes recursive member build-limit quotas.
    std::map<PlayerId, std::map<TypeId, std::uint64_t>> original_owned_counts;
    mutable std::shared_ptr<const std::vector<CensusObject>> published_production_originals;
    // Offers are immutable for the owner's lifetime, including after a checkpoint load.
    // The independent hash path recomputes these digests to verify the cache.
    mutable std::optional<detail::ProductionOfferDigests> production_offer_hashes;
    // Derived observer-local offer rows (never hashed), keyed by their admission inputs.
    mutable std::uint64_t production_offer_rebuilds{};
    // Published production state: copied again only after the production section changed (its dirty flag is
    // counted before the hash consumes it, like the economy input generation).
    mutable std::shared_ptr<const std::optional<ProductionSetup>> published_production;
    // Section copies shared by consecutive snapshots, rebuilt only when their section was counted dirty (see
    // refresh_economy_input_generation). keys: fleets, planets, players, routes, fleet control, economy, control,
    // observer, income, victory, economy corruption.
    mutable std::array<std::uint64_t, state_section_count> section_generation{};
    struct PublishedSections {
        SharedSnapshotSections rows;
        std::array<std::uint64_t, 11> keys{};
    };
    mutable PublishedSections published_sections;
    mutable std::uint64_t production_generation{1}, published_production_generation{};
    [[nodiscard]] std::span<const CensusObject> actor_inputs() const;
    [[nodiscard]] bool raid_qualified(FleetId, PlanetId) const;
    void refresh_fleet_combat_skip(FleetId);
    void notify_fleet_approach(FleetId, PlanetId, bool notify, std::size_t preceding_events);
    // WGM-150: departure accounting only; adds to the left planet's accrued piracy income.
    void accrue_departure_piracy(FleetId, PlayerId owner, PlanetId source, const std::vector<FleetMember>& departing);
    // WGM-84/85: (planet owner, orbital occupant) of a path planet, and the atomic debit/credit of a paid path.
    [[nodiscard]] std::pair<PlayerId, PlayerId> bribe_planet_state(PlanetId) const;
    [[nodiscard]] GalacticRejectReason settle_movement_bribe(PlayerId payer, const std::vector<std::pair<PlayerId, math::Fixed>>& recipients, math::Fixed total);
    void refresh_containing_fleet_combat_skip(EntityId);
    std::vector<PopulationTotal> population;
    bool population_stale{true};
    std::uint64_t population_objects{}, population_producers{};
    [[nodiscard]] core::Result<void> refresh_population(const PartitionExecutor&);
    std::optional<ObserverSetup> observer;
    std::shared_ptr<const PlanetAccessState> planet_access;
    // Derived presentation facts: not encoded/hashed. Every relevant ordered
    // observer, orbit/membership or live-content commit advances this generation.
    std::uint64_t observer_input_generation{1};
    mutable std::uint64_t observer_projection_records{};
    void invalidate_observer_projection(bool income_changed = true) noexcept {
        ++observer_input_generation;
        if (income_changed) invalidate_income_inputs(); // live orbit/membership changes
    }
    std::vector<std::vector<std::size_t>> observer_neighbours; // derived once, no quadratic tick walk
    std::uint64_t discovery_records{}, discovery_targets{};
    void initialize_observer();
    void transfer_planet_spies(PlanetId, PlayerId, PlayerId);
    void publish_surface_arrivals(PlanetId, std::span<const SurfaceForce>);
    // The observer's planet facts mirror the live control state (WGUI-100): ownership, controller,
    // integer political control and base levels follow every commit instead of staying at the start values.
    void sync_observer_control(std::size_t control_index);
    void unpublish_surface_departures(PlanetId, std::span<const EntityId>);
    void refresh_observers_for_load(std::uint64_t frame, std::uint32_t logical_fps);
    [[nodiscard]] std::optional<PlayerId> intelligence_source_owner(EntityId) const;
    void execute_observer(const GameCommand&, std::uint64_t, std::vector<GalacticEvent>&);
    [[nodiscard]] core::Result<void> service_discovery(const PartitionExecutor&, std::uint64_t, std::vector<GalacticEvent>&);
    std::set<std::size_t> active_producers;
    std::uint64_t production_producers{}, production_entries{};
    void execute_production(const GameCommand&, std::uint64_t, std::vector<GalacticEvent>&);
    void execute_object_sale(const GameCommand&, std::uint64_t, std::vector<GalacticEvent>&);
    // Exact local admission inputs: queue membership, local account/population/limits,
    // visible producers, fleet prerequisites, reductions and local corruption unlocks.
    // Queue countdowns and AI account payments do not affect these rows.
    struct ProductionOfferInputs {
        std::vector<PlanetId> visible;
        std::vector<std::uint64_t> fleets;
        std::vector<ProductionReductionSource> reductions;
        std::vector<std::uint8_t> admission;
        friend bool operator==(const ProductionOfferInputs&, const ProductionOfferInputs&) = default;
    };
    [[nodiscard]] ProductionOfferInputs production_offer_inputs(
        std::span<const ObservedPlanet> observations, std::span<const Fleet> fleet_inputs,
        std::span<const Planet> planets, PlayerId) const;
    [[nodiscard]] std::vector<ObservedProductionOffer> observed_production_offers(const ProductionOfferInputs& inputs,
        std::span<const Fleet> fleet_inputs, const PartitionExecutor& executor, PlayerId) const;
    [[nodiscard]] core::Result<void> service_production(const PartitionExecutor&, std::uint64_t,
        std::vector<GalacticEvent>&, bool pending_conflict);
    std::map<PlayerId, std::size_t> owned_planet_counts;
    void refresh_credit_caps();
    // `only` restricts removal to one domain; `removed` receives the removed identities in stored order.
    void clear_control_structures(std::size_t planet_index, std::optional<ProductionDomain> only = {}, std::vector<EntityId>* removed = nullptr);
    // WGS-93 / WGM-188: the ground base level follows the live ground special count (capped at 5) whenever a special joins or leaves.
    void refresh_ground_base_level(std::size_t planet_index);
    std::shared_ptr<const MovementSetup> movement_inputs; // shared by immutable snapshots
    std::optional<PlanetGraph> graph;
    std::size_t applied_outcomes{};
    std::optional<PoliticalControlSetup> control;
    void refresh_control_actor(const CensusObject&);
    void lose_control_actor(EntityId);
    std::uint64_t control_stealth_generation{}; // derived, never archived or hashed
    std::map<FleetId, core::Result<bool>> control_stealth_inputs;
    std::vector<PoliticalControlEffect> control_effects;
    ControlEffectsHandler control_handler;
    CreditNotificationHandler credit_notification_handler;
    [[nodiscard]] core::Result<void> apply_native_force(const NativeProductionRequest&, const NativeForcePlacement&, MovementSetup&);
    NativeProductionHandler native_production_handler;
    NativeProductionOutcomeHandler native_production_outcome_handler;
    NativeActorChangesHandler native_actor_changes_handler;
    bool conflict_requests_enabled{};
    bool native_invasions_enabled{}; // runtime capability of the explicit-list campaign owner
    std::uint64_t next_native_invasion{1};
    std::map<std::uint64_t, NativeInvasion> native_invasions;
    std::vector<std::uint64_t> native_invasion_queue;
    std::size_t native_invasion_transport_count{}; // derived bounded queue inventory
    [[nodiscard]] GalacticRejectReason validate_landing(PlayerId, FleetId, PlanetId, bool) const;
    [[nodiscard]] core::Result<void> complete_undefended_invasion(NativeInvasion&, std::vector<GalacticEvent>&);
    [[nodiscard]] core::Result<void> replay_native_invasion(GalacticSession&, const NativeEffect&);
    NativeMovementListener native_movement_listener;
    std::vector<NativeEffect> native_effects;
    std::vector<NativeEffect> native_replay;
    std::size_t native_replay_cursor{};
    std::uint64_t native_sequence{1}, native_replay_end{};
    bool native_replaying{}, native_applying{};
    std::map<NativeEffectKind, NativeEffectReplayer> native_replayers;
    [[nodiscard]] core::Result<void> journal_native_receipt(GalacticSession&, NativeEffectKind,
        PlayerId, EntityId, EntityId, bool, const CommandPayload*, const NativeFleetReceipt&);

    ConflictTailHandler conflict_tail;
    bool conflict_tail_outer_timer{true};
    [[nodiscard]] std::vector<std::size_t> section_ids() const {
        const std::size_t base = production ? 9 : setup.movement ? 8 : (setup.fleet_control ? 7 : (graph ? 6 : 5));
        std::vector<std::size_t> ids;
        for (std::size_t id = 0; id < base; ++id) ids.push_back(id);
        if (control) ids.push_back(9);
        if (setup.census) ids.push_back(11);
        if (applied_outcomes) ids.push_back(12);
        if (observer) ids.push_back(static_cast<std::size_t>(StateSection::observer));
        if (income) ids.push_back(15);
        if (victory) ids.push_back(16);
        if (economy_corruption) ids.push_back(17);
        if (next_native_invasion != 1) ids.push_back(18);
        if (planet_access) ids.push_back(19);
        if (ability_host) ids.push_back(20);
        if (!score_ledger.empty()) ids.push_back(static_cast<std::size_t>(StateSection::campaign_scoring));
        ids.push_back(static_cast<std::size_t>(StateSection::history));
        return ids;
    }
    [[nodiscard]] std::size_t section_count() const { return section_ids().size(); }
    entt::registry registry;
    // Ascending stable ID -> private handle, per object kind.
    std::map<EntityId, entt::entity> planets;
    std::map<EntityId, entt::entity> routes;
    std::map<EntityId, entt::entity> fleets;
    std::map<EntityId, DetachedUnit> detached_units;
    std::size_t live_object_count{};

    std::uint64_t completed_tick{};
    EntityId next_id{1};
    // The clock; clock.session_tick mirrors completed_tick in every published copy.
    GalacticClock clock;
    GalacticSession::TimelineService timeline_service;
    // Terminal liveness only: partition joins publish the staged data. Keep
    // every existing boolean use relaxed without making caller error paths
    // perform shared non-atomic writes during independent player decisions.
    struct FailureFlag {
        std::atomic<bool> value{};
        operator bool() const noexcept { return value.load(std::memory_order_relaxed); }
        FailureFlag& operator=(bool value_to_store) noexcept { value.store(value_to_store, std::memory_order_relaxed); return *this; }
    } failed;
    bool prebattle_autosave_window{}; // WGS-99/142: owner-derived per step, never saved
    bool launch_leg{}; // a native surface launch is running its fleet-admission leg: policy kind surface_deploy
    bool system_hold{}; // WGM-07: derived from campaign conflict/summary ownership; native journals retain the effective gate

    // The seat table as state: whether each seat is active (ascending seat ID).
    std::vector<Seat> seats;
    std::optional<SeatControlState> seat_control;

    std::map<CommandOrderKey, GameCommand> pending;
    std::map<SeatId, std::pair<std::uint64_t, std::uint64_t>> last_submitted;
    std::vector<GameCommand> executed;
    // #4058: history before the last save/load boundary is dropped from memory; the counts keep the
    // command-limit arithmetic and the absolute native sequence (native_sequence == native_dropped + journal + 1) unchanged.
    std::uint64_t executed_dropped{}, native_dropped{};

    std::shared_ptr<StateHasher> hasher;
    // Each section's digest as of its last change, and whether a commit changed it since.
    std::array<StateHash, state_section_count> digests;
    std::array<bool, state_section_count> dirty{};
    StateHash cached_hash;
    bool cached_hash_present{};
    bool verify_cached_hash{}; // test runtime option, never authoritative state
    [[nodiscard]] StateHash refresh_hash(GalacticTick::Work* work = nullptr);
    void verify_hash() const;
    std::shared_ptr<const GalacticSnapshot> snapshot;
    // Diagnostic ownership flag, never a simulation input or archive field.
    std::atomic<bool> native_barrier_active{};
    // Only the ordered replay of a fully prepared AI barrier may substitute
    // logical publications. Callbacks already consumed their staged views.
    std::shared_ptr<const GalacticSnapshot> native_commit_publication;
    mutable std::uint64_t native_commit_publications{};
    mutable std::shared_ptr<const EconomyInputGeneration> economy_input_generation =
        std::make_shared<const EconomyInputGeneration>();
    mutable std::shared_ptr<const EconomyInputGeneration> published_economy_input_generation;
    struct ViewPublicationCache {
        std::shared_ptr<const HistorySnapshot> history;
        std::array<std::uint64_t, 2> history_key{std::numeric_limits<std::uint64_t>::max(), 0};
        std::uint64_t observer_projection_generation{};
        std::shared_ptr<const std::vector<ObservedProductionOffer>> published_production_offers;
        std::shared_ptr<const std::vector<ObservedPlanet>> observer_projection;
        ProductionOfferInputs published_offer_inputs;
        std::shared_ptr<const std::vector<ObservedBlackMarketMenu>> published_black_market_menus;
        std::shared_ptr<const std::vector<AbilityResult>> published_ability_results;
        std::shared_ptr<const std::vector<AbilityActorState>> published_ability_services;
        std::shared_ptr<const std::vector<LiveCorruptionTransition>> published_corruption_transitions;
        std::array<std::uint64_t, 3> corruption_publication_key{};
        std::shared_ptr<const CorruptionMenu> published_corruption_menu;
        std::shared_ptr<const HeroActionMenus> published_hero_action_menus;
        std::array<std::uint64_t, 5> black_market_publication_key{};
    };
    // Session-owner-only, like published_sections above. Live publication builds
    // seat frames serially after workers return; UI frame(seat) only copies them.
    mutable std::map<PlayerId, ViewPublicationCache> view_publications;
    void refresh_economy_input_generation() const;

    void insert(const Planet& planet);
    void insert(const Route& route);
    void insert(const Fleet& fleet);
    // Rebuilds the registry from the current state, in reverse ID order when `reverse`.
    void rebuild(bool reverse);

    [[nodiscard]] std::vector<Planet> sorted_planets() const;
    [[nodiscard]] std::vector<Route> sorted_routes() const;
    [[nodiscard]] std::vector<Fleet> sorted_fleets() const;
    [[nodiscard]] FleetControlSetup fleet_control_state() const;
    [[nodiscard]] core::Result<void> service_tick_control(GalacticSession& session, const PartitionExecutor& executor,
        const std::uint64_t executing, std::vector<GalacticEvent>& events, GalacticTick& tick);
    [[nodiscard]] core::Result<void> service_tick_objects(GalacticSession& session, const PartitionExecutor& executor,
        const std::uint64_t executing, std::vector<GalacticEvent>& events, GalacticTick& tick,
        const bool pending_local_conflict);
    [[nodiscard]] core::Result<void> service_tick_fiscal(const PartitionExecutor& executor, const std::uint64_t executing,
        std::vector<GalacticEvent>& events, GalacticTick& tick, const std::uint32_t duration);
    [[nodiscard]] core::Result<void> service_tick_clock(const PartitionExecutor& executor, const std::uint64_t executing,
        std::vector<GalacticEvent>& events, GalacticTick& tick, const bool frame_admitted,
        const bool step_timer);
    [[nodiscard]] core::Result<void> commit_tick_commands(const PartitionExecutor& executor, const std::uint64_t executing,
        std::vector<GalacticEvent>& events, const bool pending_local_conflict);

    void execute_fleet(const GameCommand& command, std::uint64_t tick, std::vector<GalacticEvent>& events,
        bool pending_local_conflict, bool production_placement = false,
        session_detail::PausedKind kind = session_detail::PausedKind::fleet_management);
    [[nodiscard]] core::Result<void> service_control(const PartitionExecutor& executor,
        std::uint64_t tick, std::vector<GalacticEvent>& events, GalacticTick::Work& work);
    [[nodiscard]] core::Result<void> initialize_control();
    void update_production_control(std::size_t index, std::uint64_t tick,
        std::vector<GalacticEvent>& events, bool stop);
    void execute_landing(const GameCommand& command, std::uint64_t tick, std::vector<GalacticEvent>& events,
        bool pending_local_conflict);
    void execute_movement(const GameCommand& command, std::uint64_t tick, std::vector<GalacticEvent>& events,
        bool pending_local_conflict, std::optional<PlayerId> returning_player = {});
    // WGM-13/WGA-227 (#2880): the one surface-to-orbit launch owner. The native launch and the seat
    // command both gate their units here, hold the temporary detached link while `admit` runs the
    // ordinary fleet admission, and only then remove the surface link. `admission_ran` tells the
    // caller whether `admit` was reached (a gate refusal publishes and journals nothing).
    using LaunchAdmission = std::function<core::Result<NativeFleetReceipt>(const CommandPayload&)>;
    [[nodiscard]] core::Result<NativeFleetReceipt> launch_surface_units(PlayerId player,
        std::span<const EntityId> units, FleetId target, std::uint8_t slot, const LaunchAdmission& admit,
        bool& admission_ran);
    void execute_launch(const GameCommand& command, std::uint64_t tick, std::vector<GalacticEvent>& events,
        bool pending_local_conflict);
    void execute_launch_fleet(const GameCommand& command, std::uint64_t tick, std::vector<GalacticEvent>& events,
        bool pending_local_conflict, bool land_on_arrival = false);
    MovementAccessProvider movement_access_provider;
    std::uint64_t movement_access_generation{std::numeric_limits<std::uint64_t>::max()};
    [[nodiscard]] core::Result<void> service_movement(std::span<const session_detail::GatheredObject> gathered,
        const PartitionExecutor& executor, std::uint64_t tick, std::vector<GalacticEvent>& events);
    void link_movement_orbit(FleetId, PlanetId, std::uint64_t, std::vector<GalacticEvent>&);
    void gather_movement_orbit(FleetId, PlanetId, std::uint64_t, std::vector<GalacticEvent>&);
    [[nodiscard]] core::Result<void> recheck_movement_orbit(EntityId, std::uint64_t, std::vector<GalacticEvent>&);
    [[nodiscard]] std::vector<std::pair<EntityId, session_detail::ObjectKind>> sorted_objects() const;
    [[nodiscard]] session_detail::GatheredObject gather(EntityId id, session_detail::ObjectKind kind) const;

    [[nodiscard]] const Seat* seat(SeatId id) const noexcept;
    // Executes one command in the serial commit, appending its events.
    void execute(const GameCommand& command, std::uint64_t tick, std::vector<GalacticEvent>& events,
        bool pending_local_conflict = false);

    [[nodiscard]] std::vector<std::uint8_t> encode_section(StateSection section) const;
    [[nodiscard]] std::vector<std::uint8_t> encode_hash_section(StateSection section, bool reuse = true) const;
    // Owner-thread publication only: const authoritative reads may rebuild the
    // mutable derived projection cache. Never call from partition workers.
    [[nodiscard]] std::shared_ptr<const GalacticSnapshot> publish_native_snapshot(std::vector<GalacticEvent> events) const;
    [[nodiscard]] std::shared_ptr<const GalacticSnapshot> make_snapshot(std::vector<GalacticEvent> events,
        const PartitionExecutor* executor = nullptr, ObserverViewContext view = {}) const;
};

} // namespace eawr::sim::galactic
