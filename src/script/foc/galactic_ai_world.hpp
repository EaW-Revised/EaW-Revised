#pragma once

// WGA-06/07/62/65/80: campaign perception owns copied completed inputs.
// Execution routes and goal membership are separate owning-service inputs.
#include "ai_engine.hpp"
#include "ai_find_target.hpp"
#include "galactic_ai_markup.hpp"
#include "eawr/script/foc/galactic_ai.hpp"
#include "eawr/sim/galactic/snapshot.hpp"

#include <atomic>
#include <mutex>
#include <utility>

namespace eawr::campaign { struct ModeTimelineSnapshot; }

namespace eawr::script::foc::ai {
namespace galactic = sim::galactic;

// B-U05: only the derived execution restore owner can admit this query.
class DerivedExecutionRestore;
struct GalacticPlayerFacts {
    tactical::PlayerId player{}, team{};
    std::string faction, difficulty;
    bool neutral{}, ai{}, human{};
    galactic::PlanetId home{};
};
struct GalacticIndigenousPotential {
    tactical::TypeId type{};
    std::uint64_t categories{};
    Real power{};
    std::vector<galactic::PlayerId> affiliations;
};
struct GalacticPlanetFacts {
    galactic::PlanetId planet{};
    std::string name;
    tactical::TypeId type{};
    // Already normalized by the qualified authoring provider. Missing is unknown.
    std::optional<Real> base_income, maximum_structure_slots, maximum_ground_level;
    // WGA-184: potential indigenous contributions need qualified type/faction facts.
    std::optional<std::vector<std::pair<std::uint64_t, Real>>> enemy_indigenous;
    std::optional<std::vector<GalacticIndigenousPotential>> indigenous_potential;
};
struct GalacticForceSpawn { tactical::TypeId type{}; std::int32_t count{}; };
struct GalacticObjectTraits { bool hero{}, named_hero{}; };
struct GalacticForceType {
    tactical::TypeId type{};
    std::uint64_t categories{};
    Real power{}; // WGA-182: original type metric, rounded once to single precision
    std::array<std::vector<GalacticForceSpawn>, 6> starting_spawns;
    bool space_weapon{};
    bool transport{}; // WGA-187: space bitfields inspect payload instead of carrier type
    std::optional<std::uint64_t> space_structure_categories;
    std::optional<GalacticObjectTraits> object_traits;
};
// WGA-289/291/293/296: inherited generic type values, before location/player
// discounts. Resolved constructor omissions are zero (WGA-291).
struct GalacticEconomyType {
    tactical::TypeId type{};
    Real generic_price{}, generic_seconds{};
    std::optional<Real> maintenance;
    std::vector<std::string> affiliations;
    std::uint32_t tech_level{}, advances_tech{};
    bool initially_locked{}, ground_unit{}, space_unit{}, ground_structure{}, space_structure{}, built_on_ground{};
};
struct GalacticBudgetCategory { Real income{}, resources{}; };
struct GalacticBudgetPlayer {
    tactical::PlayerId player{};
    Real income{}; // WGA-293: retained last-service rate, never a forecast
    std::map<std::uint32_t, GalacticBudgetCategory> categories;
};
struct GalacticBudgetBuild {
    galactic::PlanetId planet{};
    tactical::TypeId type{};
    enum class Status { unassigned, salvage, goal, finished } status{Status::unassigned};
    Real cost_to_completion{}, spendable{}, budget_seconds{};
    tactical::PlayerId builder{};
    galactic::ProductionDomain channel{};
    sim::EntityId build{}; // WGA-354: actual head correlation, never type alone
};
struct GalacticEconomyRuntime {
    std::uint64_t completed_tick{};
    std::optional<std::int32_t> timeline;
    std::vector<GalacticBudgetPlayer> budgets;
    // WGA-293: absence means no task only for a complete producer publication.
    bool build_tasks_complete{};
    std::vector<GalacticBudgetBuild> builds;
};
// WGA-294/295: only the genuinely fresh, pre-service boundary has these zeros.
[[nodiscard]] std::shared_ptr<const GalacticEconomyRuntime> fresh_economy_runtime(
    std::span<const GalacticPlayerFacts>);
struct GalacticPerceptionSetup {
    std::uint32_t seed{}, logical_fps{30};
    bool uses_fog{};
    std::optional<Real> maximum_ground_level, maximum_space_level;
    std::vector<GalacticPlayerFacts> players;
    std::vector<GalacticPlanetFacts> planets;
    std::vector<AiType> types; // original campaign type IDs, not tactical deployment IDs
    // WGA-65: absent interval means uncached; do not invent a common timeout.
    std::map<std::string, Real, std::less<>> cache_intervals;
    std::vector<GalacticForceType> force_types;
    std::optional<Real> maximum_fleet_distance; // authored MaximumFleetMovementDistance
    std::vector<GalacticEconomyType> economy_types; // WGEP-149: supplied in registration order
    bool economy_types_complete{};
    std::map<tactical::PlayerId, Real> faction_maintenance;
    std::optional<bool> is_story_campaign; // WGA-423: immutable authored campaign state.
    std::map<tactical::PlayerId, GalacticMarkupHints> markup_hints;
    std::vector<std::string> markup_diagnostics;
};
struct GalacticExecutionRoute {
    tactical::PlayerId player{};
    galactic::PlanetId source{}, candidate{};
    bool empty{};
    Real distance{}, threat{}, prefix_threat{};
};
struct GalacticGoalReach {
    tactical::PlayerId player{};
    galactic::PlanetId source{};
    std::string policy;
    std::vector<galactic::PlanetId> production_sites;
};
struct GalacticConnectivity {
    std::uint64_t revision{};
    bool execution_available{}; // E has published a complete route generation
    std::vector<GalacticExecutionRoute> execution_routes;
    std::vector<GalacticGoalReach> goal_reach;
    // WGA-194: producer-owned potential island paths, separate from execution costs.
    struct Island { tactical::PlayerId player{}; galactic::PlanetId planet{}; bool least_force{}, largest{}; };
    std::vector<Island> island_paths;
    std::set<sim::EntityId> stealth_fleets; // live fleet stealth predicate, WGA-185
};

// Full context/binding equality, not a hash alone (WGA-65).
struct GalacticCacheKey {
    tactical::PlayerId player{};
    std::optional<tactical::PlayerId> enemy, human;
    galactic::PlanetId planet{};
    std::vector<std::string> tokens;
    std::vector<Binding> bindings;
    friend bool operator==(const GalacticCacheKey&, const GalacticCacheKey&);
    friend bool operator<(const GalacticCacheKey&, const GalacticCacheKey&);
};
struct GalacticCachedValue {
    GalacticCacheKey key;
    Real value{}, interval{};
    std::uint64_t frame{};
};
struct GalacticVisibility {
    tactical::PlayerId player{};
    galactic::PlanetId planet{};
    bool visible{true};
    std::uint64_t last_observed{};
    std::uint32_t mask{}; // zero: explicit fixture visibility, otherwise qualified WGUI bit
};
[[nodiscard]] std::uint32_t galactic_visibility_mask(std::span<const std::string>);
struct GalacticStoryFlag {
    tactical::PlayerId player{};
    galactic::PlanetId planet{}; // zero is an untargeted flag, never a wildcard
    std::string name;
};
struct GalacticHint {
    tactical::PlayerId player{};
    std::string planet, name;
    Real value{};
};
struct GalacticPerceptionState {
    // F archives these value records with its versioned manager state.
    std::vector<GalacticCachedValue> cache;
    std::vector<GalacticVisibility> visibility;
    std::vector<GalacticStoryFlag> flags;
    std::vector<GalacticHint> hints;
};
struct GalacticPerceptionWork {
    std::uint64_t prepared_planets{}, prepared_objects{}, prepared_routes{}, producer_offers{};
    std::uint64_t force_terms{}, neighbourhood_pairs{};
    std::uint64_t economy_planets{}, economy_objects{}, economy_rebuilds{};
    std::uint64_t producer_offer_preparations{};
};

class GalacticWorld final : public World, public galactic_ai::TrackedStateContributor {
public:
    explicit GalacticWorld(GalacticPerceptionSetup setup);
    // Production keeps stable perception/producer rows and publishes live motion
    // separately. The atomic binder remains the independent fixture oracle.
    void enable_live_deltas(std::uint64_t perception_period = 300,
        std::shared_ptr<const sim::PartitionExecutor> read_workers = {}) noexcept {
        live_deltas_ = true; perception_period_ = std::max<std::uint64_t>(1, perception_period);
        read_workers_ = std::move(read_workers);
    }
    void set_perception_period(std::uint64_t frames) noexcept {
        const auto period = std::max<std::uint64_t>(1, frames);
        if (period != perception_period_) { perception_period_ = period; last_catalog_slice_.reset(); }
    }
    void set_producer_executor_supplier(std::function<const sim::PartitionExecutor*()> supplier) {
        producer_executor_ = std::move(supplier);
    }
    [[nodiscard]] std::optional<std::uint64_t> state_revision() const noexcept override {
        std::scoped_lock lock(cache_mutex_); return *TrackedStateContributor::state_revision() + markup_->revision();
    }
    // Ordered barrier operation. Queries never retain a mutable session pointer.
    [[nodiscard]] core::Result<void> bind(std::shared_ptr<const galactic::GalacticSnapshot>,
        GalacticConnectivity, const sim::PartitionExecutor&, std::uint64_t synchronized_frame,
        sim::EntityId next_entity_id, bool perception_due = false);
    [[nodiscard]] std::shared_ptr<const detail::WorldView> view() const override { return view_; }
    [[nodiscard]] bool available() const noexcept override { return snapshot_ != nullptr; }
    [[nodiscard]] const std::set<sim::EntityId>& stealth_fleets() const noexcept { return connectivity_.stealth_fleets; }
    [[nodiscard]] std::uint64_t completed_tick() const noexcept override;
    [[nodiscard]] std::uint64_t synchronized_frame() const noexcept { return synchronized_frame_; }
    [[nodiscard]] std::uint64_t observer_input_generation() const noexcept {
        return snapshot_ ? snapshot_->observer_input_generation() : 0;
    }
    // Derived B invalidation, independent of unrelated budget/hint cache writes.
    [[nodiscard]] std::uint64_t movement_input_revision() const noexcept {
        std::scoped_lock lock(cache_mutex_); return movement_input_revision_;
    }
    // Parent world remains frozen in a child; the synchronizer clock still advances.
    void set_synchronized_frame(std::uint64_t frame) noexcept { synchronized_frame_ = frame; }
    [[nodiscard]] std::optional<Real> perception(const Engine&, const Equation&, const Context&, AiRandom&) const override;
    [[nodiscard]] std::optional<Real> perception_read_only(const Engine&, const Equation&, const Context&, AiRandom&) const;
    [[nodiscard]] std::optional<Real> evaluate_read_only(const Engine&, std::string_view, tactical::PlayerId, const Target*) const;
    [[nodiscard]] std::optional<Real> evaluate_read_only(const Engine&, std::string_view, const Context&) const;
    [[nodiscard]] std::optional<Real> reconstruction_enemy_space(const DerivedExecutionRestore&, const Context&) const;
    [[nodiscard]] std::optional<Real> reconstruction_perception(const DerivedExecutionRestore&,
        const Engine&, const Equation&, const Context&, AiRandom&) const;
    // WGA-80/81: uses the selected parent's store immediately, without engine service.
    [[nodiscard]] std::optional<Real> evaluate(const Engine&, std::string_view, tactical::PlayerId,
        const Target*) const;
    [[nodiscard]] std::optional<Real> evaluate(const Engine&, std::string_view, const Context&) const;
    using TimelineReader = std::function<core::Result<int>()>;
    void bind_timeline_reader(TimelineReader reader) { timeline_reader_ = std::move(reader); }
    [[nodiscard]] std::optional<Real> evaluate_with_timeline(const Engine&, std::string_view,
        tactical::PlayerId, const Target*, int) const;
    [[nodiscard]] Context goal_context(const PlayerAi&, const Target*) const override;
    // WGA-441: cached enemy raw force plus the non-allied planet's direct
    // defensive structures. Category zero requests the unfiltered total.
    [[nodiscard]] std::optional<Real> target_enemy_force(const Context&, galactic::ProductionDomain,
        std::uint64_t category = 0) const;
    [[nodiscard]] bool target_matches(tactical::PlayerId, const std::set<std::string>&, const Target*) const override;
    [[nodiscard]] std::vector<Target> targets(const Engine&, const PlayerAi&) const override;
    [[nodiscard]] std::vector<Target> new_targets(const Engine&) const override;
    [[nodiscard]] bool target_alive(const Target&) const override;
    [[nodiscard]] std::string_view goal_mode() const noexcept override { return "GALACTIC"; }
    [[nodiscard]] std::span<const tactical::Player> players() const noexcept override { return players_; }
    [[nodiscard]] const tactical::EconomyRules& economy() const noexcept override { return economy_; }
    [[nodiscard]] const tactical::EconomyView* account(tactical::PlayerId) const override;
    [[nodiscard]] const std::map<sim::EntityId, tactical::PadState>& pads() const noexcept override { return pads_; }
    [[nodiscard]] bool arriving(sim::EntityId) const override;
    [[nodiscard]] bool build_allowed(tactical::PlayerId, sim::EntityId, tactical::TypeId) const override;
    [[nodiscard]] const tactical::BuildOption* build_option(tactical::PlayerId, sim::EntityId, tactical::TypeId) const override;
    [[nodiscard]] tactical::ProductionCounts production_counts(tactical::PlayerId, tactical::TypeId) const override;
    [[nodiscard]] bool construction_allowed(tactical::PlayerId, sim::EntityId) const override { return false; }
    [[nodiscard]] bool constructing_for(sim::EntityId, tactical::PlayerId) const override { return false; }
    [[nodiscard]] sim::EntityId next_entity_id() const noexcept override { return next_id_; }
    [[nodiscard]] std::optional<ReinforcementResult> reinforcement_search_result(tactical::PlayerId, std::uint64_t) const override { return {}; }
    [[nodiscard]] std::span<const tactical::Event> events() const noexcept override { return {}; }
    [[nodiscard]] std::span<const tactical::CombatEvent> combat_events() const noexcept override { return {}; }

    [[nodiscard]] std::optional<galactic::PlanetId> containing_planet(sim::EntityId) const;
    [[nodiscard]] const GalacticExecutionRoute* execution_route(tactical::PlayerId, galactic::PlanetId, galactic::PlanetId) const;
    [[nodiscard]] bool execution_available() const noexcept { return connectivity_.execution_available; }
    // WGA-138/139: derived E publication does not re-copy completed forces,
    // production offers or perception caches, and does not advance gameplay.
    [[nodiscard]] core::Result<void> publish_execution_routes(bool available, std::uint64_t revision,
        std::vector<GalacticExecutionRoute>);
    // nullopt: production-site relation not supplied; false: known nonmember.
    [[nodiscard]] std::optional<bool> goal_reachable(tactical::PlayerId, galactic::PlanetId,
        galactic::PlanetId, std::string_view) const;
    [[nodiscard]] GalacticPerceptionWork work() const noexcept { return work_; }
    [[nodiscard]] std::uint64_t producer_admission_preparations() const noexcept { return admission_preparations_; }
    [[nodiscard]] std::uint64_t producer_demand_offer_checks() const noexcept;
    [[nodiscard]] std::shared_ptr<const galactic_ai::WorldInput> copy_input() const;
    // Copied owning-service input, refreshed at the same completed boundary.
    [[nodiscard]] core::Result<void> bind_economy_runtime(std::shared_ptr<const GalacticEconomyRuntime>) const;
    using EconomySupplier = std::function<core::Result<std::shared_ptr<const GalacticEconomyRuntime>>() >;
    // Copies retained upstream values before an ordinary/read-only query takes
    // the cache lock. Archived reconstruction consumes the explicit publication.
    // The supplier may read owner caches that are not thread-safe: the world
    // calls it on one thread at a time. A partitioned phase that queries
    // perception opens partitioned_reads() instead of calling it per query (#2971).
    void bind_economy_supplier(EconomySupplier supplier) { economy_supplier_ = std::move(supplier); }
    // ADR-009 copied input for one partitioned phase: opened serially by the
    // owner before the fan-out, it refreshes the economy copy once; until it
    // closes, queries read that copy and never call the supplier.
    class PartitionedReads {
    public:
        PartitionedReads(PartitionedReads&& other) noexcept : world_(std::exchange(other.world_, nullptr)) {}
        PartitionedReads& operator=(PartitionedReads&&) = delete;
        ~PartitionedReads() { if (world_) world_->partitioned_reads_.fetch_sub(1, std::memory_order_release); }
    private:
        friend class GalacticWorld;
        explicit PartitionedReads(const GalacticWorld& world) : world_(&world) {}
        const GalacticWorld* world_;
    };
    [[nodiscard]] PartitionedReads partitioned_reads() const;
    [[nodiscard]] GalacticPerceptionState state() const;
    // WGA-65: reduce remembered force expiries under the cache lock, preserving
    // canonical visibility precedence without copying the native state.
    [[nodiscard]] Real force_cache_deadline(tactical::PlayerId, std::uint32_t logical_fps, bool fog) const;
    [[nodiscard]] std::shared_ptr<GalacticMarkup> markup() const noexcept { return markup_; }
    [[nodiscard]] std::optional<galactic::PlanetId> markup_stage(tactical::PlayerId,
        galactic::PlanetId, std::string_view policy) const;
    [[nodiscard]] core::Result<void> restore(GalacticPerceptionState);
    // WGA-195: versioned component owned by this retained perception store.
    [[nodiscard]] core::Result<galactic_ai::NativeState> capture() const override;
    [[nodiscard]] core::Result<void> restore(const galactic_ai::NativeState&) override;
    [[nodiscard]] core::Result<std::string> state_hash() const override;
    // WGA-62: first hint insertion wins; callers validate the authored hint vocabulary.
    [[nodiscard]] bool insert_hint(GalacticHint);
    void set_story_flag(tactical::PlayerId, galactic::PlanetId, std::string);
    [[nodiscard]] bool check_story_flag(tactical::PlayerId, galactic::PlanetId, std::string_view, bool clear);
    // WGA-65: refresh existing bindings on gain AND loss, before recording hidden.
    void visibility_changed(tactical::PlayerId, galactic::PlanetId, bool visible, std::uint32_t mask = 0);

private:
    std::shared_ptr<GalacticMarkup> markup_;
    struct Evaluator;
    [[nodiscard]] bool refresh_economy_runtime() const;
    mutable std::uint64_t movement_input_revision_{};
    struct PlanetRow {
        galactic::Planet planet;
        GalacticPlanetFacts facts;
        const galactic::PlanetProduction* production{}; // immutable retained snapshot owns it
        std::uint64_t routes{}, active_routes{};
        std::optional<Real> income;
        struct Force { tactical::PlayerId owner{}; std::uint64_t categories{}; Real power{}; };
        std::vector<Force> space, ground, starbase;
        struct Unit { tactical::PlayerId owner{}; tactical::TypeId type{}; };
        std::vector<Unit> space_units, ground_units;
        std::vector<tactical::PlayerId> space_presence, ground_presence;
        struct Nearby { galactic::PlanetId planet{}; Real distance{}; bool trade_route{}; };
        std::vector<Nearby> nearby;
        std::uint64_t neighbourhood_pairs{};
        bool forces_available{true};
        bool ground_units_available{true};
    };
    [[nodiscard]] const PlanetRow* planet(galactic::PlanetId) const;
    [[nodiscard]] const GalacticPlayerFacts* player(tactical::PlayerId) const;
    [[nodiscard]] bool allied(tactical::PlayerId, tactical::PlayerId) const;
    enum class ReadMode { ordinary, read_only, archived };
    [[nodiscard]] std::optional<Real> lookup(const Context&, const Lookup&, const std::vector<Binding>&, ReadMode mode = ReadMode::ordinary) const;
    [[nodiscard]] std::optional<Real> uncached(const GalacticCacheKey&, ReadMode mode = ReadMode::ordinary) const;
    [[nodiscard]] std::optional<Real> read(const GalacticCacheKey&, ReadMode mode = ReadMode::ordinary) const; // caller holds cache_mutex_
    [[nodiscard]] std::optional<Real> force_token(const GalacticCacheKey&, ReadMode mode = ReadMode::ordinary) const;
    [[nodiscard]] std::optional<Real> player_economy_token(const GalacticCacheKey&, tactical::PlayerId,
        std::string_view, ReadMode) const;
    [[nodiscard]] std::optional<Real> target_economy_token(const GalacticCacheKey&, std::string_view) const;
    [[nodiscard]] core::Result<void> prepare_economy_tokens(const sim::PartitionExecutor&);
    // Called only after bind proves every nonqueue/noncredit input unchanged.
    [[nodiscard]] core::Result<void> refresh_economy_queues(const sim::PartitionExecutor&, bool queues_changed);
    [[nodiscard]] std::optional<Real> budget_fraction(const GalacticCacheKey&, tactical::PlayerId) const;
    [[nodiscard]] core::Result<void> prepare_forces(std::vector<PlanetRow>&,
        const galactic::GalacticSnapshot&, const GalacticConnectivity&, const sim::PartitionExecutor&) const;
    void bind_connectivity(GalacticConnectivity);
    [[nodiscard]] GalacticCachedValue* cached_value(const GalacticCacheKey&) const;
    [[nodiscard]] GalacticVisibility* visibility(tactical::PlayerId, galactic::PlanetId, std::uint32_t mask = 0, bool publish = true) const;
    GalacticPerceptionSetup setup_;
    // Immutable first-match lookup, derived before sorting identity metadata.
    std::map<std::string, std::map<std::uint32_t, tactical::TypeId>, std::less<>> next_technology_types_;
    std::shared_ptr<const galactic::GalacticSnapshot> snapshot_;
    // PlanetRow's production pointers belong to this retained physical publication.
    std::shared_ptr<const galactic::GalacticSnapshot> bound_source_;
    bool live_deltas_{};
    std::shared_ptr<const WorldView> view_;
    std::vector<PlanetRow> planets_;
    std::vector<tactical::Player> players_;
    std::vector<tactical::EconomyView> accounts_;
    struct BuildRow { tactical::PlayerId player{}; sim::EntityId producer{}; tactical::BuildOption option; galactic_ai::ProducerInput input; };
    std::vector<BuildRow> builds_;
    struct PreparedProducerOffers;
    [[nodiscard]] core::Result<void> prepare_catalog_slice(const sim::PartitionExecutor&, std::uint64_t);
    [[nodiscard]] core::Result<void> refresh_live_producers(
        std::shared_ptr<const galactic::GalacticSnapshot>, sim::EntityId, std::size_t);
    [[nodiscard]] galactic_ai::ProducerInputs::Query producer_query() const;
    [[nodiscard]] static core::Result<std::vector<galactic_ai::ProducerInput>> read_producers(
        const PreparedProducerOffers&, std::uint32_t logical_fps, tactical::PlayerId,
        galactic::PlanetId, std::span<const galactic::TypeId>, bool first_allowed, const sim::PartitionExecutor&);
    std::shared_ptr<const PreparedProducerOffers> prepared_producer_offers_;
    std::uint64_t perception_period_{300};
    std::optional<std::uint64_t> last_catalog_slice_;
    std::shared_ptr<const sim::PartitionExecutor> read_workers_;
    std::function<const sim::PartitionExecutor*()> producer_executor_;
    mutable std::map<std::tuple<tactical::PlayerId, sim::EntityId, tactical::TypeId>, tactical::BuildOption> build_options_;
    std::uint64_t admission_preparations_{};
    std::shared_ptr<const galactic_ai::WorldInput> input_;
    tactical::EconomyRules economy_;
    std::map<sim::EntityId, tactical::PadState> pads_;
    std::map<sim::EntityId, galactic::PlanetId> containment_;
    std::set<sim::EntityId> transit_;
    GalacticConnectivity connectivity_;
    GalacticPerceptionWork work_;
    struct EconomyPlanet {
        galactic::PlanetId planet{};
        tactical::PlayerId owner{};
        std::int64_t ground{}, space{};
        std::optional<Real> maximum_ground, maximum_space;
        bool production_available{};
        std::map<tactical::TypeId, std::int64_t> structures, queued;
        tactical::TypeId ground_head{};
    };
    struct EconomyPlayer {
        std::map<tactical::TypeId, std::int64_t> owned, queued;
        std::vector<PlanetRow::Force> space, ground;
        bool force_available{true};
        std::optional<Real> maintenance;
    };
    std::vector<EconomyPlanet> economy_planets_;
    std::map<tactical::PlayerId, EconomyPlayer> economy_players_;
    std::shared_ptr<const galactic::EconomyInputGeneration> economy_generation_;
    mutable std::shared_ptr<const GalacticEconomyRuntime> economy_runtime_;
    EconomySupplier economy_supplier_;
    TimelineReader timeline_reader_;
    std::uint64_t economy_rebuilds_{};
    mutable std::map<GalacticCacheKey, std::optional<Real>> economy_queries_;
    mutable std::optional<Real> economy_queries_expiry_;
    mutable std::optional<std::uint64_t> economy_queries_frame_;
    sim::EntityId next_id_{1};
    std::uint64_t synchronized_frame_{};
    // Open PartitionedReads scopes, and whether their one refresh succeeded.
    mutable std::atomic<std::uint32_t> partitioned_reads_{};
    mutable bool partitioned_refresh_ok_{};
    mutable std::mutex supplier_mutex_; // one economy_supplier_ call at a time; taken before cache_mutex_
    mutable std::mutex cache_mutex_;
    mutable GalacticPerceptionState state_;
    // Exact-key positions are derived and guarded by cache_mutex_. Values remain
    // in the native vectors; restore/erase invalidates these indexes.
    mutable std::map<GalacticCacheKey, std::size_t> cache_index_;
    mutable std::map<std::tuple<tactical::PlayerId, galactic::PlanetId, std::uint32_t>, std::size_t> visibility_index_;
    mutable bool refreshing_visibility_{}; // guarded by cache_mutex_
};

// Child context is supplied by the real campaign owner; no inference from a tactical target.
struct GalacticParentContext {
    const Engine* manager{};
    const GalacticWorld* world{};
    galactic::PlanetId conflict_planet{};
    bool campaign_child{};
    std::shared_ptr<const campaign::ModeTimelineSnapshot> timeline{};
    std::optional<int> timeline_value{}; // copied fixed-mode reader value, no allocation
};
[[nodiscard]] std::optional<Real> evaluate_in_galactic_context(const GalacticParentContext&,
    std::string_view equation, tactical::PlayerId player);

class GalacticFindTargetQueries final : public FindTargetQueries {
public:
    GalacticFindTargetQueries(const Engine& galactic_manager, const GalacticWorld& world,
        const Engine* current = nullptr, std::shared_ptr<const FindTargetQueries> current_queries = {},
        galactic_ai::Mode current_mode = galactic_ai::Mode::space)
        : galactic_(galactic_manager), world_(world), current_(current ? current : &galactic_manager),
          current_queries_(std::move(current_queries)), current_mode_(current_mode) {}
    [[nodiscard]] const Engine* manager(FindTargetManager) const override;
    [[nodiscard]] const Target* source_target(const Engine&, sim::EntityId) const override;
    [[nodiscard]] std::optional<FindTargetPath> execution_path(const Engine&, tactical::PlayerId,
        const Target&, const Target&) const override;
private:
    const Engine& galactic_;
    const GalacticWorld& world_;
    const Engine* current_{};
    std::shared_ptr<const FindTargetQueries> current_queries_;
    galactic_ai::Mode current_mode_{};
};

class GalacticQueries final : public galactic_ai::Queries {
public:
    GalacticQueries(const Engine& manager, const GalacticWorld& world) : manager_(manager), world_(world) {}
    [[nodiscard]] std::shared_ptr<const galactic_ai::WorldInput> copy_world() const override { return world_.copy_input(); }
    [[nodiscard]] core::Result<Real> equation(std::string_view, const galactic_ai::QueryContext&) const override;
    [[nodiscard]] std::optional<galactic::PlanetId> containing_planet(sim::EntityId id) const override { return world_.containing_planet(id); }
private:
    const Engine& manager_;
    const GalacticWorld& world_;
};
} // namespace eawr::script::foc::ai
