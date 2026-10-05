#pragma once

#include "session_types.hpp"
#include "blast_internal.hpp"
#include "combat_internal.hpp"
#include "staging.hpp"
#include "../../../third_party/entt/single_include/entt/entt.hpp"

namespace eawr::sim::tactical {

using namespace session_detail;

class TacticalSession::Impl final {
public:
    Impl(const TacticalSetup& source, const std::span<const SensorProfile> sensor_table, const DurabilityTable& table,
        const MotionTable& motion_table, const std::optional<FogRules>& fog_rules, const CombatTable& combat_table,
        const VictoryRules& victory_rules, const AbilityTable& ability_table, const EconomyRules& economy_rules);

    // A counted star base (#77, VT-03) joins the ascending-ID list the victory evaluation walks.
    // Tick lifecycle notifications reconcile destruction and ownership conversion (WBF-31).
    void add_starbase(const UnitState& unit);

    // A unit entering the session at `frame`: full health, at rest, no target (#72, #70, #73).
    // With damage rules its shield and energy recharge on phases of its own (DG-13, EN-02): the
    // first at `frame` plus a keyed draw in [0, interval - 1], then every interval.
    [[nodiscard]] std::optional<DurabilityState> entering_durability(
        const DurabilityProfile* profile, const EntityId unit, const std::uint64_t frame) const;

    [[nodiscard]] LiveUnit new_unit(const UnitState& unit, const std::uint64_t frame) const;

    [[nodiscard]] const DurabilityProfile* health_profile(const LiveUnit& unit) const noexcept;

    // WPR-51: component-wise largest per stacking category, then sum distinct categories.
    using OwnershipKey = std::pair<PlayerId, TypeId>;
    std::vector<OwnershipKey> ownership_keys;
    std::vector<std::uint64_t> ownership_counts;
    std::vector<std::uint64_t> committed_ownership_counts;
    std::vector<std::uint64_t> ownership_parts;
    std::vector<std::uint32_t> bonus_categories;
    std::vector<CombatBonuses> bonus_profiles;
    std::vector<CombatBonuses> committed_bonus_profiles;
    std::vector<CombatBonuses> bonus_parts;
    std::vector<IncomeCategory> income_category_parts;
    struct CommandSource {
        EntityId id{}, host{};
        std::size_t profile{};
        std::vector<EntityId> targets;
        friend bool operator==(const CommandSource&, const CommandSource&) = default;
    };
    struct CommandLedger {
        std::vector<CommandSource> sources;
        std::map<EntityId, std::vector<std::size_t>> recipients;
    };
    CommandLedger command_ledger;
    std::optional<core::Diagnostic> initialization_error;
    [[nodiscard]] core::Result<CommandLedger> update_command_ledger(const std::vector<LiveUnit>& live,
        const EntityId first_new, const PartitionExecutor& executor) const;
    [[nodiscard]] CombatBonuses command_bonuses_for(const LiveUnit& unit, const CommandLedger& ledger,
        const std::vector<PlayerEconomy>& accounts, const std::map<EntityId, TypeId>& containers,
        const std::span<CombatBonuses> categories, const UnitStage* effect_world = nullptr,
        bool targeted_effects = true) const;
    [[nodiscard]] core::Result<void> apply_command_bonuses(std::vector<LiveUnit>& live, const CommandLedger& ledger,
        const std::vector<PlayerEconomy>& accounts, const EntityId first, const PartitionExecutor& executor) const;
    struct IncomeServiceRef {
        PlayerId owner{};
        EntityId object{};
        std::size_t slot{};
        const IncomeModifier* profile{};
        IncomeModifierState* state{};
        bool residual{};
    };
    std::vector<IncomeServiceRef> income_service_refs;
    [[nodiscard]] core::Result<void> service_income_modifiers(std::vector<PlayerEconomy>& accounts,
        std::span<const LiveUnit> live, std::span<const std::pair<EntityId, PlayerId>> streams,
        std::uint64_t frame, const PartitionExecutor& executor, bool initialization_only = false);
    void retire_income_modifier(PlayerEconomy& account, const CompletedBuild& held) const;
    void reduce_income(const std::vector<PlayerEconomy>& accounts, EntityId stream,
        std::span<IncomeCategory> categories) const;

    [[nodiscard]] std::size_t ownership_slot(const PlayerId player, const TypeId type) const;
    void adjust_owned(const UnitState& unit, const bool added);
    [[nodiscard]] core::Result<void> count_owned(const std::vector<LiveUnit>& units, const PartitionExecutor& executor);

    [[nodiscard]] CombatBonuses bonuses_for(const LiveUnit& unit, const bool committed = false) const;

    [[nodiscard]] CombatBonuses profile_bonuses(const OwnershipKey key, const std::vector<PlayerEconomy>& accounts,
        const std::map<EntityId, TypeId>& containers, const std::span<CombatBonuses> categories,
        bool total = true) const;

    [[nodiscard]] core::Result<void> build_bonus_profiles(const std::vector<PlayerEconomy>& accounts,
        const std::map<EntityId, TypeId>& containers, const PartitionExecutor& executor);

    // WPR-57: stock skirmish has one station per team; holder loss destroys its held sources.
    // The debug build's alternate-holder path is outside this scope (#1016).
    [[nodiscard]] bool prune_upgrade_holders(std::vector<PlayerEconomy>& accounts,
        const std::vector<LiveUnit>& live) const;

    // WPR-51: a changed maximum adds its delta to current hull/shield/energy; hardpoints scale.
    enum class BonusAdjustment { legacy, gain, loss };
    [[nodiscard]] core::Result<void> apply_bonuses(LiveUnit& unit, const CombatBonuses& bonuses,
        BonusAdjustment adjustment = BonusAdjustment::legacy) const;

    [[nodiscard]] ProductionCounts production_counts(const PlayerId buyer, const TypeId type,
        const std::vector<PlayerEconomy>& accounts, const bool committed = false) const;

    // #76: a unit whose type has abilities enters with every one off and ready (AB-10).
    [[nodiscard]] std::optional<AbilityState> entering_abilities(const TypeId type, const PlayerId owner) const;
    void refresh_damage_modes(session_detail::LiveUnit& unit) const;
    struct ConcentrateEffect {
        EntityId target{}, source{};
        std::uint32_t category{};
        math::Fixed defense{};
        math::Fixed speed{};
        AbilityKind kind{AbilityKind::concentrate_fire};
        math::Fixed cause{};
        friend bool operator==(const ConcentrateEffect&, const ConcentrateEffect&) = default;
    };
    // Derived source references; only activation recruits, service never scans recipients.
    std::vector<ConcentrateEffect> concentrate_effects;
    std::set<EntityId> concentrate_changed_targets;
    std::size_t concentrate_service_census{};
    EntityId concentrate_service_last_id{};
    mutable std::map<EntityId, std::vector<CombatBonuses>> impact_bonus_bases;
    mutable std::map<EntityId, TypeId> impact_bonus_containers;
    mutable bool impact_bonus_containers_ready{};
    std::vector<EntityId> beam_replans;
    std::vector<AbilitySpawnState> ability_spawns; // ascending shared projectile IDs; HABL
    [[nodiscard]] const SpawnedAbilityProfile* spawn_profile(TypeId) const;
    [[nodiscard]] core::Result<void> service_weaken(std::vector<LiveUnit>&, std::uint64_t, const PartitionExecutor&);
    [[nodiscard]] const SpecialAbilityProfile* beam_profile(const LiveUnit&, AbilityKind) const;
    [[nodiscard]] bool beam_target_valid(const LiveUnit&, const LiveUnit&, const SpecialAbilityProfile&) const;
    [[nodiscard]] bool beam_in_range(const LiveUnit&, const LiveUnit&, const SpecialAbilityProfile&) const;
    void release_beam(LiveUnit&, AbilityKind, std::uint64_t) const;
    void register_tractor(const LiveUnit&, const LiveUnit*);
    [[nodiscard]] core::Result<void> service_beams(std::vector<LiveUnit>&, std::uint64_t, const PartitionExecutor&);
    [[nodiscard]] const SpecialAbilityProfile* concentrate_profile(const LiveUnit& unit) const;
    [[nodiscard]] core::Result<void> service_concentrate(std::vector<LiveUnit>& units,
        std::uint64_t tick, const PartitionExecutor& executor);
    void register_concentrate(const LiveUnit& unit);
    [[nodiscard]] math::Fixed concentrate_defense(const LiveUnit& target, const UnitStage& units,
        const std::vector<PlayerEconomy>& accounts) const;
    [[nodiscard]] bool accumulate_concentrate_bonus(EntityId target, std::span<CombatBonuses> categories,
        const UnitStage* units = nullptr) const;
    [[nodiscard]] core::Result<void> refresh_concentrate_bonuses(std::vector<LiveUnit>& units,
        const CommandLedger& ledger, const std::vector<PlayerEconomy>& accounts, const PartitionExecutor& executor) const;

    // AB-14: what the unit's shield says about switching DEFEND on at `tick`.
    [[nodiscard]] AbilityGate ability_gate(const LiveUnit& unit, const std::uint64_t tick) const;

    // AB-60 (#561): the unit's ION_CANNON_SHOT slot (the container's team slot, or a craft's own),
    // or null.
    [[nodiscard]] const AbilitySlot* ion_slot(const LiveUnit& unit) const;
    [[nodiscard]] AbilitySlot* ion_slot(LiveUnit& unit) const;

    // AB-62 (#561): whether an ION_CANNON_SHOT of `owner`'s squadron may lock onto `target`: a
    // live unit of a hostile player that can be hit (a unit with a combat profile; a squadron's team
    // container has no model to aim at, its craft do).
    [[nodiscard]] bool ion_target_valid(const PlayerId owner, const LiveUnit* target) const;

    // AB-65 (#561): the squadron's ION_CANNON_SHOT ends: the container and every craft switch it
    // off; the container recharges only when at least one craft fired (a live craft no longer due).
    void finish_ion_shot(AbilitySlot& team, const std::vector<AbilitySlot*>& craft, const TypeId container_type,
        const std::uint64_t tick) const;

    // AB-20: the unit's multiplier of one kind from its active abilities; 1 without any.
    [[nodiscard]] math::Fixed ability_factor(const LiveUnit& unit, const AbilityModifier modifier) const;

    // AB-43: DEFEND switched on brings the unit's next shield and energy recharges forward to the
    // next tick, from where they run at DEFEND's intervals.
    void defend_switched_on(LiveUnit& unit, const std::uint64_t tick) const;

    // AB-42: the hull and shield a hit took from a unit that runs the DEFEND stand-in.
    void track_damage(LiveUnit& unit, const math::Fixed hull_before, const math::Fixed shields_before) const;

    // IS-01 to IS-04, IS-07 (#561): an ion shot stuns the unit it hit; the stun ends an active
    // DEFEND (early, AB-12). Speed changes wait for the next tick's ability phase (IS-05, AB-24).
    void ion_stun_unit(LiveUnit& unit, const IonStunShot& shot, const std::uint64_t tick) const;

    // IS-03: a unit's stun ends at its end frame. IS-05: neither its start nor its end plans a
    // move under way again; a move planned while it lasts keeps its cut speed.
    static void service_ion_stun(LiveUnit& unit, const std::uint64_t tick);

    // AB-17: a shield at zero ends DEFEND (early, AB-12); a speed change waits for the next tick's
    // ability phase (AB-24).
    void end_depleted_defend(LiveUnit& unit, const std::uint64_t tick, bool storm_branch = false) const;

    // One unit's ability service at `tick` (the ability phase, AB-11, AB-16, AB-41, AB-42).
    // Returns whether its speed multiplier changed since its move was planned.
    [[nodiscard]] bool service_abilities(LiveUnit& unit, const std::uint64_t tick) const;

    // AB-50: the unit's abilities as the snapshot shows them, ready as of `tick`.
    [[nodiscard]] std::vector<AbilityStatus> ability_statuses(const LiveUnit& unit, const std::uint64_t tick) const;

    // #75: the tick-zero squadrons whose type the squadron table knows hold their start position
    // (FM-20) and their craft fly by their craft profiles; every spawner starts its hangar.
    void bind_squadrons(const std::vector<LiveUnit>& live);

    // FL-06, FL-07: one squadron of `decision` leaves `spawner`'s bay at `frame`: its craft at the
    // bay facing along the bay's spawn vector at full speed, then its team container; the squadron
    // escorts a mobile spawner and holds the end of the spawn vector otherwise.
    [[nodiscard]] core::Result<void> launch(const LiveUnit& spawner, const SpawnerProfile& profile,
        const SpawnDecision& decision, const std::uint64_t frame, EntityId& next, std::vector<LiveUnit>& survivors,
        std::vector<TacticalInstance>& instances, std::vector<Squadron>& squadron_list,
        detail::MapStage<CraftState>& flights, detail::MapStage<SquadronState>& orders) const;

    // Publishes the snapshot of the staged units at the completed tick, with the given events.
    void publish_staged(const std::vector<LiveUnit>& live, std::vector<Event> events,
        std::vector<CombatEvent> combat_events);

    // #530: each ledger with its owner's population (PU-21) for presentation.
    [[nodiscard]] std::vector<PadView> pad_views() const;
    [[nodiscard]] std::vector<std::pair<PlayerId, ManualPlayerClock>> manual_clock_views() const {
        return {manual_clocks.begin(), manual_clocks.end()};
    }

    [[nodiscard]] std::vector<EconomyView> economy_views(
        const std::vector<PlayerEconomy>& staged_ledgers, const std::map<EntityId, PopulationShare>& staged_shares) const;

    [[nodiscard]] bool allied(const PlayerId left, const PlayerId right) const;

    [[nodiscard]] const Player* player_of(const PlayerId id) const noexcept;

    // The staged economy a step's commands and service change (#530).
    struct EconomyStage {
        std::vector<PlayerEconomy>& ledgers;
        std::map<EntityId, ArrivalState>& arrivals;
        std::map<EntityId, PopulationShare>& shares;
        detail::MapStage<CraftState>& crafts;
        detail::MapStage<SquadronState>& minds;
        std::vector<Squadron>& squadrons; // squadrons brought in this frame
        std::vector<EntityId>& born_spawners; // only carrier births, never a world scan
        std::vector<std::pair<EntityId, PlayerId>>& earners;
        EntityId& next_id;
        std::map<EntityId, PadState>& pads;
        std::map<EntityId, ConstructionState>& construction;
        std::vector<PlayerCommand>& pad_requests;
        ObjectStage<PadState>& pad_stage;
        ObjectStage<ConstructionState>& construction_stage;
        std::vector<BattleEconomyCue>& cues;
        TacticalSession::PlacementWork* placement_work{};
    };

    [[nodiscard]] PlayerEconomy* ledger_of(std::vector<PlayerEconomy>& staged_ledgers, const PlayerId id) const;
    // WHE-63: append authored escorts immediately; a full team returns false without recharge.
    [[nodiscard]] core::Result<bool> replenish_wingmen(EntityId leader, TypeId team, UnitStage& units,
        EconomyStage& stage, std::uint64_t tick);

    // PU-11: whether `station` (live, staged) builds `type` for `buyer`: an ally's station whose
    // list for its owner's faction offers the type in M2.
    [[nodiscard]] const BuildOption* build_option(const LiveUnit& station, const PlayerId buyer, const TypeId type) const;

    // PU-21: the population value of a type any station builds, 0 when none does.
    [[nodiscard]] std::uint32_t population_of(const TypeId type) const noexcept;

    // PU-31, PU-32: whether `player` may bring `type` in at `point` among the `staged` units.
    // It visits the staged units once per reinforce command, never per tick.
    [[nodiscard]] core::Result<bool> placement_valid(const PlayerId player, const TypeId type, const math::Vec3& point,
        const UnitStage& staged, const std::uint64_t tick,
        const CollisionWorld* collisions = nullptr, TacticalSession::PlacementWork* work = nullptr,
        const std::vector<UnitState>* prevention_units = nullptr) const;

    // PL-05: the world box a unit blocks in an arrival's free-space search, from its type's placement
    // box and the yaw of its rotation; a type with no box never blocks (PL-02).
    [[nodiscard]] core::Result<void> block_placement(const UnitState& unit, std::vector<PlacementBox>& blockers) const;

    // #530: one buy, cancel or reinforce of the frame's commands phase (PU-10 to PU-17, PU-30 to
    // PU-34), serial in command order. Appends its event.
    [[nodiscard]] core::Result<void> execute_economy(const PlayerCommand& command, const std::uint64_t tick,
        UnitStage& staged, EconomyStage& stage, std::vector<Event>& events,
        const CollisionWorld* collisions = nullptr, const bool executing_pad = false);

    // Live units with a sensor profile, in the order of `units` (ascending ID).
    [[nodiscard]] static std::vector<FogRevealer> revealers(const SensorField& field, const std::span<const UnitState> units,
        const std::span<const EntityId> disabled = {});

    // Own-team players always see a unit (V-04). Other players see it by the retail fog cells
    // when the session is bound to fog rules (V-11 to V-17), else by the exact range test.
    [[nodiscard]] std::uint64_t visible_to(const SensorField& field, const FogCells* cells, const PlayerId owner,
        const math::Vec3& position) const;

    // `now`: the tick the snapshot's readers act at (the completed tick it is published with).
    [[nodiscard]] TacticalInstance instance_for(const LiveUnit& unit, const math::Mat3x4& transform, const std::uint64_t now) const;

    [[nodiscard]] LiveUnit live_at(const EntityId id, const entt::entity handle) const;

    [[nodiscard]] std::vector<LiveUnit> sorted_live() const;

    // Movement of an accepted order (MV-10, MV-20, MV-22): a move or face plans from the unit's
    // state at `start` (completed tick + 1), a stop ends any plan; other orders keep it.
    // With avoidance rules (#71) a move runs the path finder against `world` (AV-10 to AV-15).
    [[nodiscard]] core::Result<void> plan_order(LiveUnit& unit, const CommandPayload& payload, const std::uint64_t start,
        const CollisionWorld* world = nullptr) const;

    // HD-11: the unit's movement limits after lost engines scale its maximum speed,
    // acceleration and deceleration. The unit has a motion profile.
    [[nodiscard]] core::Result<MotionProfile> limits_of(const LiveUnit& unit) const;

    // A move's path from the unit's state at `start` towards `target`. `max_speed`: a group
    // move's planning speed (FM-09), which caps the unit's own; zero plans nothing. `stats`, when
    // given, receives the path search's work (PC-07).
    [[nodiscard]] core::Result<void> plan_path(LiveUnit& unit, const math::Vec3 target, const std::uint64_t start,
        const CollisionWorld* world, const std::optional<math::Fixed> max_speed, PathSearchStats* const stats) const;

    // What plan_path plans from (PC-08): the unit's position, yaw and speed, and its limits
    // under a group move's `max_speed`.
    struct PathInputs {
        math::Vec3 position{};
        math::Fixed yaw{};
        math::Fixed speed{};
        MotionProfile limits{};
        friend bool operator==(const PathInputs&, const PathInputs&) noexcept = default;
    };
    [[nodiscard]] core::Result<PathInputs> path_inputs(const LiveUnit& unit, const std::optional<math::Fixed> max_speed) const;

    // plan_path's last step: the unit takes `plan`.
    static void take_plan(LiveUnit& unit, MotionState plan);

    // plan_path, whose path search (a tracked unit with a world) is given up once it has counted
    // `budget` expansions (PC-08): false then, and the unit is unchanged.
    [[nodiscard]] core::Result<bool> plan_path_within(LiveUnit& unit, const math::Vec3 target, const std::uint64_t start,
        const CollisionWorld* world, const std::optional<math::Fixed> max_speed, PathSearchStats* const stats,
        const std::optional<std::uint64_t> budget) const;

    // A path search that runs in slices and lands at `frame` (PC-08, #520): the plan the unit
    // would make at `frame` towards `target`, from where its current plan puts it then, against
    // its layer and the static layer as `world` holds them now. `inputs` is what it plans from;
    // the landing takes the plan only while the unit still plans from exactly that.
    struct SlicedSearch {
        SlicedPathSearch search;
        std::uint64_t frame{};
        math::Vec3 target{};
        std::optional<math::Fixed> max_speed;
        PathInputs inputs;
        // PC-09: the plan, once the search has ended; its unit's layer predicts it from then on.
        std::optional<MotionState> plan;
    };
    [[nodiscard]] core::Result<SlicedSearch> start_sliced(const LiveUnit& unit, const math::Vec3 target,
        const std::optional<math::Fixed> max_speed, const std::uint64_t now, const std::uint64_t frame,
        const CollisionWorld& world) const;

    // --- Approaching a unit (#452, docs/behaviour/space-orders.md OR-02 to OR-17) --------------

    struct ApproachPlan {
        math::Vec3 slot{};
        std::uint64_t prediction_frame{};
    };

    // The unit the order in force sends a unit towards (OR-08, OR-17): an attack's target while
    // it is still the unit's player-ordered target (T-01 drops it), an attack-move's or guard's
    // unit. Zero otherwise, and for a unit without a motion profile.
    [[nodiscard]] static EntityId approach_target(const LiveUnit& unit) noexcept;

    // OR-06: an approach is checked every reevaluation interval after the order's tick.
    [[nodiscard]] bool approach_due(const LiveUnit& unit, const std::uint64_t tick) const noexcept;

    // OR-03, OR-04, OR-14: the approach distance of the unit's order, also its in-range test.
    [[nodiscard]] math::Fixed approach_range(const LiveUnit& unit, const bool guard,
        const detail::CombatUnit* target = nullptr) const noexcept;

    // OR-21: an attack may name a hardpoint of the target's type that is targetable and standing.
    [[nodiscard]] bool target_hardpoint_standing(const LiveUnit& target, const std::uint32_t index) const;

    // OR-04, OR-14: the point the in-range test measures to: a guarded unit's position, else the
    // aim point of A-04 seen from `from`; OR-25: an attack on a hardpoint measures to that
    // hardpoint while it stands.
    [[nodiscard]] static math::Vec3 approach_point(const bool guard, const LiveUnit& target,
        const detail::CombatUnit* target_view, const math::Vec3& from, const std::uint32_t hardpoint = attack_hull);

    // Where the unit's current movement leaves it (OR-06): the tracked prediction of its path at
    // the path's end (where the last frame before the last node puts it, MV-32), else where it is.
    // #662: sampling the path after its end gives back the position passed in, the unit's current
    // one, so every check found the end out of range and planned the approach again.
    [[nodiscard]] static core::Result<math::Vec3> movement_end(const LiveUnit& unit);

    // OR-07: where `target` will be at frame `at` on its own planned path, seen at `frame`.
    [[nodiscard]] static core::Result<math::Vec3> predicted_position(
        const LiveUnit& target, const std::uint64_t frame, const std::uint64_t at);

    // OR-05, OR-07: a new approach towards `target` from where the unit is at `frame`.
    [[nodiscard]] core::Result<ApproachPlan> approach_mapping(
        const LiveUnit& unit, const LiveUnit& target, const math::Fixed range, const std::uint64_t frame) const;

    // OR-06: nullopt while the unit keeps its movement (in range of the target now, or at the end
    // of its movement in range of the target's predicted position), else its new approach.
    [[nodiscard]] core::Result<std::optional<ApproachPlan>> approach_check(const LiveUnit& unit, const LiveUnit& target,
        const detail::CombatUnit* target_view, const std::uint64_t frame) const;

    [[nodiscard]] std::vector<UnitState> sorted_units() const;

    void rebuild(const std::vector<LiveUnit>& units, const bool reverse);

    template <typename Component, typename... Args>
    Component& emplace_component(const entt::entity handle, Args&&... args);

    template <typename Component, typename Value>
    void commit_optional(const entt::entity handle, std::optional<Value>& value,
        std::uint64_t& writes);

    // ECS storage mutation is serial: creating/removing a component can grow or compact a
    // shared pool. Retain handles and unchanged components; only changed values are assigned.
    void commit_units(std::vector<LiveUnit>& units, std::uint64_t& writes);

    // The targeting phase's world view (#73): the moved units in ascending ID with their health,
    // combat state and the visibility of the last published snapshot, and one space index over
    // them. Workers fill disjoint ascending-ID slots; only the index build and squadron
    // overrides are serial (one index and ordered team state).
    [[nodiscard]] core::Result<detail::CombatWorld> combat_world(std::vector<LiveUnit>& units,
        const std::vector<std::pair<EntityId, math::Vec3>>& starts, const std::uint64_t frame,
        const PartitionExecutor& executor) const;

    // CO-01, CO-11: a live unit with a combat profile joins its owner's tree with the world box of
    // its collision box (FoC: its model's), or FoC's box for an object without a model: its
    // position to 0.1 beyond on each axis. None for a unit without a combat profile.
    [[nodiscard]] core::Result<std::optional<detail::CollectionTrees::Member>> collection_member(
        LiveUnit& unit) const;

    [[nodiscard]] std::vector<std::uint8_t> canonical_bytes() const;

    TacticalSetup setup;
    std::vector<SensorProfile> sensors;
    DurabilityTable durability;
    MotionTable motion;
    CombatTable combat;
    VictoryRules victory;
    AbilityTable abilities;
    EconomyRules economy;                           // #530
    std::map<EntityId, PadState> pads;
    std::map<EntityId, ConstructionState> construction;
    ObjectStage<PadState> pad_stage;
    ObjectStage<ConstructionState> construction_stage;
    std::vector<CaptureJob> capture_jobs; // pad-sized, retained; no world-sized outputs
    std::vector<CaptureCandidate> capture_inputs;
    std::array<CaptureScratch, tick_partition_count> capture_scratch;
    AsteroidScratch asteroid_scratch; // retained scratch, never canonical state
    NebulaScratch nebula_scratch;
    std::vector<PlayerEconomy> ledgers;             // #530: per economy player, ascending ID
    std::map<EntityId, ArrivalState> arrivals;      // #530: arriving units (PU-35)
    std::map<EntityId, PopulationShare> shares;     // #530: reinforced units' population (PU-21)
    std::vector<std::pair<EntityId, PlayerId>> earners; // #530: live income stations, ascending ID
    std::vector<IncomePayment> income_payments; // retained partition outputs, never canonical state
    std::map<EntityId, RespawnBatch> respawns; // due frame -> creation requests, in death order
    std::vector<StarbaseEntry> starbases;   // counted star bases standing, ascending ID (#77)
    std::optional<BattleOutcome> outcome;   // hashed once decided (#77)
    std::vector<PlayerQuit> quits; // WBF-48: committed departure status, ascending player ID
    // WBF-45: immutable history is shared by snapshots between sparse notification commits.
    std::shared_ptr<const std::vector<BattleLoss>> losses = std::make_shared<const std::vector<BattleLoss>>();
    std::shared_ptr<const std::vector<BattleProduction>> productions = std::make_shared<const std::vector<BattleProduction>>();
    std::shared_ptr<const std::vector<BattleEconomyCue>> economy_cues = std::make_shared<const std::vector<BattleEconomyCue>>();
    std::vector<SnapshotPlayer> snapshot_players;
    std::map<PlayerId, TeamId> teams;
    std::vector<Squadron> squadrons; // live, ascending container ID (#271)
    std::optional<FogCells> fog;     // bound fog rules only (#274)
    // Per dynamic tracking layer (#71, AV-03): the frame of its last rebuild; its windows roll
    // every tracking interval from there.
    std::array<std::uint64_t, 4> tracking_anchor{};
    std::map<CommandKey, TacticalSession::ReinforcementSearch> reinforcement_searches;
    std::map<std::pair<PlayerId, std::uint64_t>, TacticalSession::ReinforcementSearchResult> reinforcement_search_results;
    // Path searches running in slices (PC-08, #520), by unit; each unit waits for its search
    // with a sliced FormationWait of the same frame. A search's copied views and progress are a
    // function of the ticks before it, and its wait is hashed.
    std::map<EntityId, SlicedSearch> searches;
    std::vector<Projectile> projectiles; // in flight, ascending ID (#74)
    struct PendingBlastDamage {
        std::uint64_t due{};
        Projectile source;
        detail::BlastRecipient recipient;
    };
    std::vector<PendingBlastDamage> pending_blast_damage;
    // #636: the projectile phase's candidate buffers, one per partition, kept from tick to tick
    // (not state: they only hold capacity and the tick's work counts).
    std::array<detail::ProjectileScratch, tick_partition_count> projectile_scratch;
    std::map<EntityId, math::TrigCache> craft_trig; // scratch; one writer per craft
    TacticalSession::TickWork tick_work{};
    // #893: query buffers belong exclusively to their phase partition; staged
    // lists retain their high-water capacity and are committed in scanner order.
    std::array<ChaseQueryScratch, tick_partition_count> chase_scratch;
    std::vector<std::vector<EntityId>> chase_candidates;
    std::map<EntityId, CraftState> crafts;     // #75: squadron craft that fly by a craft profile
    std::map<EntityId, SquadronState> minds;   // #75: by container, squadrons of a known type
    std::map<EntityId, SpawnerState> spawners; // #75: hangars of SPAWN_SQUADRON units
    std::vector<DeathSpin> spins;              // #447: killed craft spinning away, ascending ID
    detail::CollectionTrees collection;        // #469: the candidate order of target scans
    detail::CollectionTrees projectile_collection; // DG-30: owned projectile-collidable history
    std::uint64_t next_projectile{1};
    entt::registry registry;
    std::map<EntityId, entt::entity> handles;
    std::uint64_t completed_tick{};
    EntityId next_id{};
    std::uint64_t rng_state{};
    std::map<CommandKey, PlayerCommand> pending;
    std::map<PlayerId, std::pair<std::uint64_t, std::uint64_t>> last_submitted;
    std::vector<PlayerCommand> executed;
    std::map<PlayerId, ManualPlayerClock> manual_clocks; // WAD-40: shared across stations
    std::shared_ptr<const TacticalSnapshot> current_snapshot;
    std::uint64_t registry_emplacement_count{};
    std::function<void(std::string_view, bool)> commit_observer;
    std::shared_ptr<StateHasher> hasher; // #637: null hashes on the stepping thread
};

} // namespace eawr::sim::tactical
