#pragma once

#include "session_impl.hpp"

#include <array>
#include <set>

namespace eawr::sim::tactical {

// Cross-phase scratch is grouped by its producer. Values are constructed at
// their original declaration sites; journal lifetime extends through tick exit.
namespace session_detail {

class CombatPhases;

class Tick final {
    friend class CombatPhases;
    using Impl = TacticalSession::Impl;
public:
    Tick(TacticalSession& session, Impl* impl, const PartitionExecutor& executor);
    ~Tick();
    Tick(const Tick&) = delete;
    Tick& operator=(const Tick&) = delete;
    core::Result<TacticalTick> run();
    core::Result<void> land_combat(std::vector<LiveUnit>& live,
        const std::vector<std::pair<EntityId, math::Vec3>>& starts,
        std::vector<Event>& events, std::vector<CombatEvent>& combat_events);


private:
    std::optional<BattleControllerPolicy> controller_rollback_;
    bool controller_committed_{};
    void record_damage_change(const LiveUnit& target, math::Fixed before,
        EntityId source = invalid_entity_id, const UnitStage* staged = nullptr);
    std::vector<DamageChange> damage_changes_; // ordered commits only, not partition jobs
    core::Result<void> gather();
    core::Result<void> prepare_craft();
    core::Result<void> prepare_chase_pairs(std::vector<CraftView>& craft_views, std::map<EntityId, EntityId>& craft_squadron,
        std::map<EntityId, SquadronFrame>& squadron_frames, detail::MapStage<SquadronState>& minds,
        auto& team_of, auto& hostile_units);
    core::Result<void> prepare_dogfight_cells(std::map<EntityId, SquadronFrame>& squadron_frames,
        detail::MapStage<SquadronState>& minds, std::vector<LiveUnit>& moving,
        const SquadronTable& squadron_table, auto& hostile_units, auto& squadron_of,
        auto& resolve_attack_point, auto& target_soft_radius, auto& search_cell);
    core::Result<void> prepare_attack_attachments(std::map<EntityId, SquadronFrame>& squadron_frames, std::vector<LiveUnit>& moving,
        auto& resolve_attack_point);
    core::Result<void> prepare_formation_lanes(std::vector<LiveUnit>& moving, std::map<EntityId, SquadronFrame>& squadron_frames,
        const SquadronTable& squadron_table);
    core::Result<void> prepare_squadron_frames(detail::MapStage<SquadronState>& minds,
        std::map<EntityId, SquadronFrame>& squadron_frames, std::map<EntityId, EntityId>& craft_squadron,
        const SquadronTable& squadron_table, std::vector<LiveUnit>& moving, const math::Fixed& idle_reach);
    core::Result<void> movement();
    core::Result<void> open_staging();
    core::Result<void> commit_moves();
    core::Result<void> nebulas();
    core::Result<void> prepare_targets();
    core::Result<void> hull_service();
    core::Result<void> targets();
    core::Result<void> commit_weapon_targets(std::vector<LiveUnit>& moving,
        std::vector<std::optional<detail::CombatStep>>& combat_steps,
        std::vector<std::optional<CombatState>>& weapon_inputs);
    core::Result<void> prepare_weapon_targets(std::vector<LiveUnit>& moving,
        const std::map<EntityId, ArrivalState>& arrivals, const std::map<EntityId, EntityId>& craft_squadron,
        std::vector<std::optional<CombatState>>& weapon_inputs, std::vector<bool>& weapon_idle,
        std::map<EntityId, std::pair<std::size_t, EntityId>>& acquired, detail::MapStage<SquadronState>& minds,
        const detail::CombatWorld& view, std::vector<std::optional<detail::CombatStep>>& combat_steps,
        std::vector<std::optional<core::Diagnostic>>& combat_errors);
    void scan_member_targets_partition(const std::size_t& partition, std::vector<std::size_t>& scan_indices,
        const std::map<EntityId, EntityId>& craft_squadron, std::vector<LiveUnit>& moving,
        const std::map<EntityId, ArrivalState>& arrivals, std::map<EntityId, SquadronFrame>& squadron_frames,
        detail::MapStage<SquadronState>& minds, auto& scans, const detail::CombatWorld& view,
        detail::CombatWorld& scan_view);
    core::Result<void> refresh_target_inputs(std::vector<LiveUnit>& moving,
        const std::map<EntityId, EntityId>& craft_squadron);
    core::Result<void> retry_buzz_targets(std::vector<LiveUnit>& moving, detail::MapStage<SquadronState>& minds,
        std::map<EntityId, SquadronFrame>& squadron_frames, const std::map<EntityId, ArrivalState>& arrivals);
    core::Result<void> ion_team();
    core::Result<void> orders();
    core::Result<void> projectile_inputs();
    core::Result<void> launch_shots();
    core::Result<void> abilities_tracking();
    core::Result<void> object_turn_seam();
    core::Result<RejectReason> apply_object_turn_use(const ObjectTurnUse& use, std::vector<EntityId>& replans);
    void commit_object_turn();
    core::Result<void> impacts();
    core::Result<void> commit_projectile_flight(std::size_t& flight_index,
        std::vector<std::optional<detail::ProjectileStep>>& flights, UnitStage& staged,
        std::vector<Projectile>& projectiles, std::vector<CombatEvent>& combat_events, auto& deliver_blast,
        const std::map<EntityId, ArrivalState>& arrivals, const std::map<EntityId, math::Fixed>& craft_defense,
        const DamageRules* const& damage_rules, std::vector<Projectile>& launched, auto& deliver_redirected,
        auto& queue_damage, std::vector<Event>& events, std::vector<UnitState>& killed);
    // True means this flight was deflected; the caller skips its remaining impact work.
    core::Result<bool> redirect_projectile_impact(const UnitStage::iterator& source, UnitStage& staged,
        const UnitStage::iterator& target, const Projectile& projectile,
        std::optional<detail::ProjectileStep>& flight, std::vector<Projectile>& launched,
        std::vector<CombatEvent>& combat_events, std::uint32_t& hardpoint, auto& deliver_blast,
        std::size_t& flight_index);
    core::Result<void> apply_direct_impact(const UnitStage::iterator& target,
        const DamageRules* const& damage_rules, const DurabilityProfile& profile, const Projectile& projectile,
        Hit& hit, std::uint32_t& hardpoint, std::optional<detail::ProjectileStep>& flight,
        std::vector<CombatEvent>& combat_events, std::vector<Event>& events, std::vector<UnitState>& killed,
        UnitStage& staged, auto& deliver_blast, std::size_t& flight_index);
    core::Result<void> deliver_blast_impact(const std::size_t& index,
        const std::vector<std::optional<detail::BlastStep>>& blasts,
        std::vector<std::optional<detail::BlastStep>>& blasts_without_source, UnitStage& staged,
        std::vector<std::optional<detail::ProjectileStep>>& flights, auto& deliver_area, auto& queue_damage);
    void queue_delayed_impact(const Projectile& source, const detail::BlastRecipient& recipient,
        UnitStage& staged, std::vector<Impl::PendingBlastDamage>& pending_blast_damage);
    core::Result<void> deliver_area_impact(const Projectile& projectile, const detail::BlastRecipient& recipient,
        UnitStage& staged, const std::map<EntityId, ArrivalState>& arrivals,
        const std::map<EntityId, math::Fixed>& craft_defense, const DamageRules* const& damage_rules,
        auto& deliver_redirected, std::vector<Event>& events, std::vector<UnitState>& killed);
    core::Result<void> deliver_redirected_impact(const EntityId& id, Hit& hit, const PlayerId& killer,
        const EntityId& attacker, UnitStage& staged, const std::map<EntityId, ArrivalState>& arrivals,
        const std::map<EntityId, math::Fixed>& craft_defense, const DamageRules* const& damage_rules,
        std::vector<Event>& events, std::vector<UnitState>& killed);
    core::Result<void> late_impacts();
    core::Result<void> retaliation();
    void notify_retaliation(const LiveUnit& unit, EntityId attacker, math::Fixed amount);
    std::vector<std::pair<EntityId, EntityId>> retaliation_notifications_; // tick scratch, delivery order
    core::Result<void> service_buzz_droids(UnitStage& staged, std::vector<Event>& events,
        std::vector<UnitState>& killed, const DamageRules* damage_rules);
    core::Result<void> service_self_destruct(UnitStage& staged, std::vector<Event>& events,
        std::vector<UnitState>& killed, std::vector<Projectile>& projectiles, std::uint64_t& next_projectile);
    struct CommandOrders;
    struct CommandHandlers;
    struct PlanLanes;
    core::Result<void> commands();
    core::Result<void> service_retreat_flight(UnitStage&, std::vector<Event>&, std::vector<UnitState>&);
    core::Result<void> retreat_service();
    core::Result<void> special_weapons();
    core::Result<void> systems();
    void service_asteroid_contact(LiveUnit& unit, std::size_t& index, const std::size_t& partition,
        const std::map<EntityId, ArrivalState>& arrivals, const DamageRules* const& damage_rules,
        std::vector<std::uint64_t>& asteroid_queries, AsteroidScratch& asteroid_scratch,
        SpaceIndex& field_index, math::Fixed& field_reach, std::vector<AsteroidFieldInput>& fields,
        std::vector<std::uint64_t>& asteroid_candidates, std::vector<std::optional<core::Diagnostic>>& errors);
    void service_systems_partition(const std::size_t& partition, std::vector<LiveUnit>& inputs,
        std::vector<std::uint8_t>& proxy_live, std::vector<ServiceOutcome>& serviced,
        std::vector<CaptureJob>& capture_jobs, std::vector<CombatBonuses>& transfer_categories,
        std::map<EntityId, TypeId>& transfer_containers,
        const std::map<EntityId, ConstructionState>& construction, const DamageRules* const& damage_rules,
        std::vector<std::optional<core::Diagnostic>>& errors, const std::map<EntityId, ArrivalState>& arrivals,
        std::vector<std::uint64_t>& asteroid_queries, AsteroidScratch& asteroid_scratch,
        SpaceIndex& field_index, math::Fixed& field_reach, std::vector<AsteroidFieldInput>& fields,
        std::vector<std::uint64_t>& asteroid_candidates,
        std::array<std::vector<std::size_t>, tick_partition_count>& repair_units,
        std::vector<TacticalInstance>& instances, const detail::MapStage<CraftState>& craft_states,
        const detail::MapStage<SquadronState>& squadron_states);
    core::Result<void> service_hardpoint_repairs(std::vector<LiveUnit>& inputs, std::vector<ServiceOutcome>& serviced,
        std::array<std::vector<std::size_t>, tick_partition_count>& repair_units,
        std::vector<TacticalInstance>& instances, std::vector<std::optional<core::Diagnostic>>& errors);
    core::Result<void> commit_environmental_hits(std::vector<LiveUnit>& inputs, std::vector<ServiceOutcome>& serviced,
        const DamageRules* const& damage_rules, std::vector<std::vector<AsteroidImpact>>& asteroid_impacts,
        AsteroidScratch& asteroid_scratch, std::vector<TacticalInstance>& instances);
    core::Result<void> prepare_capture_jobs(std::vector<CaptureJob>& capture_jobs, std::vector<CaptureCandidate>& capture_inputs,
        std::map<EntityId, PadState>& pads, std::vector<LiveUnit>& inputs,
        const std::map<EntityId, ArrivalState>& arrivals, const core::Result<detail::CombatWorld>& world);
    core::Result<void> prepare_asteroid_fields(std::vector<LiveUnit>& inputs, const DamageRules* const& damage_rules,
        const std::map<EntityId, ArrivalState>& arrivals,
        std::vector<std::optional<AsteroidFieldInput>>& field_slots, AsteroidScratch& asteroid_scratch,
        std::vector<AsteroidFieldInput>& fields, math::Fixed& field_reach, SpaceIndex& field_index,
        std::vector<std::vector<AsteroidImpact>>& asteroid_impacts);
    core::Result<void> commit_survivors();
    core::Result<void> pad_lifecycle();
    core::Result<void> fighters();
    core::Result<void> retire_fighter_state(detail::MapStage<SquadronState>& minds, detail::MapStage<CraftState>& crafts,
        const SquadronTable& squadron_table, const std::uint64_t& frame, std::vector<Squadron>& squadrons);
    core::Result<void> prepare_squadron_outcomes(detail::MapStage<SquadronState>& minds, std::vector<LiveUnit>& survivors,
        const std::vector<Squadron>& arrived_squadrons, detail::MapStage<CraftState>& crafts,
        const SquadronTable& squadron_table,
        const std::vector<std::pair<EntityId, math::Vec3>>& movement_starts,
        const std::vector<std::optional<CraftStep>>& movement_steps, const detail::CombatWorld& combat_world,
        const std::uint64_t& frame, std::vector<TacticalInstance>& instances);
    core::Result<void> commit_squadron_outcomes(std::vector<const Squadron*>& squadron_inputs, std::vector<SquadronOutcome>& outcomes,
        detail::MapStage<SquadronState>& minds, auto& find,
        std::vector<std::pair<std::size_t, PlayerId>>& final_killers,
        std::vector<std::uint8_t>& completed_bases, std::vector<LiveUnit>& survivors,
        std::vector<TacticalInstance>& instances, std::vector<Squadron>& squadrons);
    core::Result<void> service_death_projectiles_and_spins(const std::vector<UnitState>& killed, const SquadronTable& squadron_table,
        const std::uint64_t& frame, detail::MapStage<CraftState>& crafts, std::vector<Event>& events);
    void prepare_squadron_outcomes_partition(const std::size_t& partition,
        std::vector<const Squadron*>& squadron_inputs, std::vector<SquadronOutcome>& outcomes, auto& find,
        detail::MapStage<SquadronState>& minds, std::vector<std::pair<std::size_t, PlayerId>>& final_killers,
        const SquadronTable& squadron_table, std::vector<std::uint8_t>& completed_bases,
        const std::vector<std::pair<EntityId, math::Vec3>>& movement_starts,
        const std::vector<std::optional<CraftStep>>& movement_steps, const detail::CombatWorld& combat_world,
        std::vector<const LiveUnit*>& attack_leaders, detail::MapStage<CraftState>& crafts,
        const std::uint64_t& frame);
    bool squadron_refresh_skips(const LiveUnit*& target, const CraftProfile*& craft, const SquadronState& state,
        detail::MapStage<SquadronState>& minds, SquadronOutcome& outcome, const EntityId& target_id,
        const std::uint64_t& frame, const LiveUnit*& leader, const detail::CombatWorld& combat_world,
        std::vector<const LiveUnit*>& attack_leaders, std::size_t& index, const LiveUnit*& container,
        detail::MapStage<CraftState>& crafts, auto& finish_base);
    core::Result<void> station_upgrades();
    core::Result<void> economy();
    core::Result<void> hangars();
    core::Result<void> bonuses();
    core::Result<void> finalize();
    core::Result<void> finalize_departures();
    core::Result<void> finalize_defences();
    core::Result<void> finalize_fog(const std::vector<UnitState>& positions, const SensorField& sensing,
        const std::vector<Impl::HeldReveal>& held_reveals, const std::vector<CombatEvent>& combat_events,
        const detail::CombatWorld& view);
    core::Result<void> finalize_visibility(const std::vector<UnitState>& positions, const SensorField& sensing,
        const std::vector<EntityId>& disabled_revealers);
    core::Result<void> finalize_outcomes();
    core::Result<void> commit();
    core::Result<TacticalTick> finish();
    void mark(std::string_view section, bool begin) const;
    void register_jammer(EntityId source, bool active);
    void spawn_instant_ability(LiveUnit& source, AbilityKind kind, const SpawnedAbilityProfile& spawn,
        const math::Vec3& position);
    const LiveUnit* survivor(const EntityId id);
    static math::Vec3 roster_offset(const SquadronFrame& frame, const EntityId craft);
    static LiveUnit* live_unit(std::vector<LiveUnit>& units, const EntityId id);
    static const LiveUnit* live_unit(const std::vector<LiveUnit>& units, const EntityId id);
    const CraftView* craft_view(const EntityId id);
    void claim_idle(const EntityId container, SquadronState& state, const math::Vec3& desired);
    void hand_target(const EntityId container, const EntityId previous, const EntityId target,
        const std::uint32_t hardpoint = no_hardpoint, const bool direct = false, const bool divert = true);
    void divert_target(EntityId container, EntityId target, bool direct = false);
    void end_combat(const EntityId container, SquadronState& state, const CraftView* leader);
    [[nodiscard]] Event destruction_event(const std::uint64_t tick, const EventKind kind, const UnitState& unit,
        const std::uint32_t hardpoint = 0, const PlayerId killer = 0);
    void sync_radar_creations(const UnitStage& staged, EntityId first, EntityId next);
    void track_victory_change(const UnitState* before, const UnitState* after, bool evaluate);
    void pay_bounty(TypeId victim, PlayerId owner, PlayerId killer);
    // MAT-15: a destroyed object's type goes back to its owner's pool in an attract session. Called
    // once per destruction, in destruction order; `garrison` and `team_member` are the object's
    // marks (a garrison unit and a team member return nothing).
    void note_pool_return(const UnitState& unit, bool garrison, bool team_member);
    void set_special_hardpoint_gate(PlayerId owner, bool enabled);
    void publish_special_hardpoints(const LiveUnit& unit);
    [[nodiscard]] bool damage_blocked() const { return pending_outcome_.has_value(); }
    [[nodiscard]] static Order order_for(const CommandPayload& payload, const std::uint64_t tick);
    [[nodiscard]] static std::optional<math::Vec3> point_move(const CommandPayload& payload);
    [[nodiscard]] static EntityId approach_target_of(const CommandPayload& payload);
    [[nodiscard]] static std::int64_t midpoint(const std::int64_t low, const std::int64_t high) noexcept;
    [[nodiscard]] std::vector<EntityId> redirect_recipients(EntityId target) const;
    [[nodiscard]] const LiveUnit* redirect_unit(EntityId target) const;
    template <typename Apply>
    core::Result<bool> redirect_damage(const EntityId target, const Hit& incoming, Apply&& apply) {
        const auto members = redirect_recipients(target);
        if (members.empty()) return core::Result<bool>::success(false);
        Hit share = incoming;
        share.amount = math::Fixed::from_raw(incoming.amount.raw() / static_cast<std::int64_t>(members.size() - 1));
        share.hardpoint = hull_target;
        share.bypass_take_damage_mode = false;
        // WHE-64: ordered raw shares enter ordinary recipient damage with a recursion guard.
        // The guard is structural: another redirecting member is skipped, never re-entered.
        for (const auto id : members) {
            if (id == target) continue;
            const auto* recipient = redirect_unit(id);
            if (!recipient) continue;
            const auto* profile = impl_->combat.find(recipient->state.type_id);
            if (profile && profile->redirect_damage_to_teammates) continue;
            if (auto result = apply(id, share); !result) return core::Result<bool>::failure(result.error());
        }
        return core::Result<bool>::success(true);
    }

    TacticalSession& session_;
    Impl* impl_;
    const PartitionExecutor& executor;
    const std::uint64_t tick;
    // Transactional victory state, updated by sparse ordered lifecycle hooks (VT-02).
    std::vector<StarbaseEntry> pending_starbases_;
    std::optional<BattleOutcome> pending_outcome_;
    RetreatState pending_retreat_;
    std::optional<PlayerId> pending_midbattle_solve_;
    std::uint64_t pending_rng_{};
    std::optional<Event> pending_victory_event_;
    std::optional<core::Diagnostic> victory_error_;
    std::optional<core::Diagnostic> bounty_error_;
    std::vector<BattleLoss> pending_losses_;
    struct PoolReturn final {
        PlayerId owner{};
        TypeId type{};
    };
    std::vector<PoolReturn> pool_returns_; // MAT-15: ordered, applied by economy() before production
    std::vector<BattleProduction> pending_productions_;
    std::vector<BattleEconomyCue> pending_economy_cues_;
    std::map<EntityId, std::pair<std::size_t, PlayerId>> loss_killers_;
    std::set<EntityId> loss_ids_;
    std::set<EntityId> victory_removed_;
    std::array<std::uint64_t, max_players> victory_counts_{};
    std::vector<PlayerQuit> pending_quits_; // notification order; processed after this frame
    std::vector<BeaconState> beacons_; // transactional, bounded by max_players.
    std::vector<PlayerId> pending_reveals_; // V-20: applied to the transactional fog copy
    std::map<PlayerId, ManualPlayerClock> manual_clocks_; // transactional tick copy
    std::map<EntityId, SpecialWeaponState> special_weapons_;
    std::set<PlayerId> special_disabled_by_hardpoint_;
    NamedAbilityLocks named_ability_locks_;
    std::optional<Impl::RadarRoster> radar_roster_; // copied only on a radar lifecycle change
    bool radar_dirty_{};
    void set_named_hardpoint_gate(const UnitState&, std::string_view name, bool enabled);
    std::set<EntityId> special_initial_; // initial launches accepted this tick, in source order
    struct SpecialShot {
        detail::CombatUnit source, target;
        std::optional<DurabilityState> target_health;
        std::optional<DurabilityProfile> target_health_profile;
        std::uint32_t pulse{};
    };
    std::vector<SpecialShot> special_shots_;
    std::vector<EntityId> projectile_defence_order_; // ordered activation notifications, committed only on success
    std::vector<LiveUnit> object_turn_births_; // sparse turn creations join after indexed tracking rows
    std::vector<ObjectTurnRecord> object_turn_log_; // #3644: uses applied this tick, moved to the record on success
    std::vector<ObjectTurnOutcome> object_turn_outcomes_; // #3644: reported on TacticalTick
    std::vector<EntityId> created_static_defences_; // creation notifications within this frame
    std::vector<Event> manual_feedback_;
    struct Metrics {
        std::optional<std::uint64_t> initial_emplacements;
    };
    std::optional<Metrics> metrics_;
    struct Gather {
        std::array<std::vector<EntityId>, tick_partition_count> prevention_ids;
        std::optional<TacticalSession::TickWork> tick_work;
        std::optional<TacticalSession::PlacementWork> reinforcement_work;
        std::optional<std::array<std::uint64_t, tick_partition_count>> movement_builds;
        std::optional<std::vector<LiveUnit>> moving;
        std::optional<std::vector<std::pair<EntityId, math::Vec3>>> starts;
        std::optional<std::vector<std::optional<CraftView>>> gathered_crafts;
        std::optional<std::array<std::array<std::uint64_t, max_players>, tick_partition_count>> victory_parts;
    };
    std::optional<Gather> gather_;
    struct CraftPrep {
        std::optional<detail::MapStage<SquadronState>> minds;
        std::optional<std::vector<CraftView>> craft_views;
        std::optional<std::map<EntityId, SquadronFrame>> squadron_frames;
        std::optional<std::map<EntityId, EntityId>> craft_squadron;
        std::optional<std::uint64_t> dogfight_cone_tests;
    };
    std::optional<CraftPrep> craft_prep_;
    struct Movement {
        std::optional<std::vector<std::optional<CraftStep>>> craft_steps;
    };
    std::optional<Movement> movement_;
    struct Staging {
        std::optional<detail::MapStage<CraftState>> crafts;
        std::optional<std::map<EntityId, ArrivalState>> arrivals;
        std::optional<std::vector<EntityId>> unloaded;
        std::optional<std::vector<PlayerEconomy>> ledgers;
        std::optional<detail::MapStage<RespawnBatch>> respawns;
        std::optional<std::vector<PlayerCommand>> pad_requests;
        std::optional<std::map<EntityId, PopulationShare>> shares;
        std::optional<std::vector<Squadron>> arrived_squadrons;
        std::vector<EntityId> born_spawners;
        std::optional<std::vector<std::pair<EntityId, PlayerId>>> earners;
        std::optional<EntityId> next_id;
    };
    std::optional<Staging> staging_;
    struct Moved {
        std::optional<std::map<EntityId, math::Fixed>> craft_defense;
        std::optional<std::set<EntityId>> closing_squadrons;
    };
    std::optional<Moved> moved_;
    struct Targets {
        std::optional<core::Result<detail::CombatWorld>> world;
        std::optional<detail::CollectionTrees> collection;
        std::optional<detail::CollectionTrees> projectile_collection;
        std::optional<std::vector<CombatEvent>> combat_events;
        std::optional<std::vector<math::Vec3>> aim_offsets;
        std::optional<std::vector<std::pair<EntityId, math::Vec3>>> combat_turns;
    };
    std::optional<Targets> targets_;
    struct Orders {
        std::optional<std::vector<std::pair<EntityId, Impl::ApproachPlan>>> approaches;
        std::vector<EntityId> waypoint_ready;
    };
    std::optional<Orders> orders_;
    struct Flights {
        std::vector<std::vector<EntityId>> weaken_recipients;
        std::optional<const DamageRules*> damage_rules;
        std::optional<std::vector<std::optional<detail::ProjectileStep>>> flights;
        std::optional<std::uint64_t> projectile_candidates;
        std::optional<std::uint64_t> projectile_exact_tests;
        std::optional<std::vector<std::optional<detail::BlastStep>>> blasts;
        std::optional<std::vector<std::optional<detail::BlastStep>>> blasts_without_source;
        std::optional<std::uint64_t> blast_detonations;
        std::optional<std::uint64_t> blast_recipients_examined;
        std::optional<std::vector<Projectile>> launched;
        std::optional<std::uint64_t> next_projectile;
    };
    std::optional<Flights> flights_;
    struct Tracked {
        std::optional<std::vector<EntityId>> replans;
        std::optional<std::vector<EntityId>> self_destructs; // WSD-04: ascending, countdown run out
        std::optional<std::vector<EntityId>> lure_ends; // WLU-07: ascending, LURE expired this tick
        std::optional<std::uint64_t> frame;
        std::optional<std::array<bool, 4>> rebuilt;
        std::optional<std::optional<Tracking>> tracking;
        std::optional<std::vector<std::pair<std::size_t, EntityId>>> members_before;
        std::optional<std::uint64_t> production_census_visits;
        std::optional<UnitStage> staged;
    };
    std::optional<Tracked> tracked_;
    struct Impacts {
        std::optional<std::vector<Event>> events;
        std::optional<std::vector<UnitState>> killed;
        std::optional<std::vector<Impl::PendingBlastDamage>> pending_blast_damage;
        std::optional<std::vector<Projectile>> projectiles;
    };
    std::optional<Impacts> impacts_;
    struct Commands {
        struct Search {
            CommandKey key;
            TacticalSession::ReinforcementSearch request;
            TacticalSession::ReinforcementSearchResult result;
            TacticalSession::PlacementWork work;
            std::optional<core::Diagnostic> error;
        };
        std::vector<Search> searches;
        std::optional<std::vector<core::Diagnostic>> diagnostics;
        std::optional<std::map<CommandKey, PlayerCommand>::iterator> due_end;
    };
    std::optional<Commands> commands_;
    struct Systems {
        std::optional<std::vector<LiveUnit>> inputs;
        std::optional<std::uint64_t> capture_index_bodies;
        std::optional<std::uint64_t> capture_prepare_bodies;
        std::optional<std::vector<TacticalInstance>> instances;
        std::optional<std::vector<ServiceOutcome>> serviced;
    };
    std::optional<Systems> systems_;
    struct Surviving {
        std::optional<std::vector<LiveUnit>> survivors;
        std::optional<std::uint64_t> capture_candidates;
    };
    std::optional<Surviving> surviving_;
    struct Fighters {
        std::optional<std::vector<DeathSpin>> spins;
        std::optional<std::vector<Squadron>> squadrons;
        std::optional<detail::MapStage<SpawnerState>> spawners;
        std::optional<std::vector<Impl::FreeGarrisonState>> free_garrisons;
    };
    std::optional<Fighters> fighters_;
    struct Bonuses {
        std::optional<bool> sources_changed;
        std::optional<std::uint64_t> bonus_profile_evaluations;
        std::optional<Impl::CommandLedger> command_ledger;
    };
    std::optional<Bonuses> bonuses_;
    struct Finalized {
        std::optional<std::array<math::Fixed, max_players>> fog_multipliers;
        std::optional<std::vector<SnapshotPlayer>> players;
        std::optional<std::optional<FogCells>> fog;
        std::optional<std::vector<SpinningCraft>> spinning;
        std::optional<std::array<std::uint64_t, 4>> anchors;
        std::optional<std::vector<StarbaseEntry>> starbases;
        std::optional<std::optional<BattleOutcome>> outcome;
        std::optional<std::vector<PlayerQuit>> quits;
        std::optional<std::vector<PlayerId>> basic_ai;
        std::optional<std::vector<DepartureControl>> departure_controls;
        std::optional<std::vector<BattleLoss>> losses;
        std::optional<std::vector<BattleProduction>> productions;
        std::optional<std::vector<BattleEconomyCue>> economy_cues;
    };
    std::optional<Finalized> finalized_;
    struct Committed {
        std::optional<std::uint64_t> registry_component_writes;
        std::optional<std::size_t> staged_map_copies;
        std::optional<std::uint64_t> registry_emplacements;
    };
    std::optional<Committed> committed_;

};

} // namespace session_detail
} // namespace eawr::sim::tactical
