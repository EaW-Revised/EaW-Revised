#pragma once

#include "session_store_outcome.hpp"
#include "session_store_abilities.hpp"
#include "session_store_bonuses.hpp"
#include "session_store_diagnostics.hpp"
#include "session_store_economy.hpp"
#include "session_store_rules.hpp"
#include "session_store_environment.hpp"
#include "session_store_healing.hpp"
#include "session_store_publication.hpp"
#include "session_store_squadrons.hpp"
#include "session_store_visibility.hpp"
#include "session_store_reinforcements.hpp"
#include "session_store_paths.hpp"
#include "session_store_projectiles.hpp"
#include "session_store_station_spawn.hpp"
#include "session_store_registry.hpp"
#include "session_store_commands.hpp"
#include "blast_internal.hpp"
#include "combat_internal.hpp"
#include "staging.hpp"
#include "session_land_internal.hpp"
#include "../../../third_party/entt/single_include/entt/entt.hpp"

#include <algorithm>
#include <set>
#include <tuple>

namespace eawr::sim::tactical {

namespace session_detail { struct CanonicalCounts; struct CanonicalEncoding; }

using namespace session_detail;

// Engine-synthesised, unrecorded commands (the autofired CONCENTRATE_FIRE, WAB-36): bit 63 of the
// sequence marks them and the low bits name the source entity. Coordinator-reserved range in the
// numbers registry (out/coord/numbers.md); every recorded command's sequence is a small counter.
inline constexpr std::uint64_t synthesised_command_bit = std::uint64_t{1} << 63;
[[nodiscard]] constexpr CommandKey synthesised_command_key(const std::uint64_t tick, const PlayerId owner,
    const EntityId source) noexcept {
    return {tick, owner, synthesised_command_bit | source};
}
[[nodiscard]] constexpr bool synthesised_command(const CommandKey& key) noexcept {
    return (key.sequence & synthesised_command_bit) != 0;
}

// WAB-36: the unit ability served by a nested handler whose own admission accepts an enemy attack
// target, so that it can switch on through the attack-state pass. The other handlers refuse that
// target: BUZZ_DROIDS and CORRUPT_SYSTEMS admit only a MARKER object; STEALTH, SENSOR_JAMMING,
// LASER_DEFENSE, BLAST and the cluster bomb only no target or the unit itself. The ion shot's
// team path is AB-68.
[[nodiscard]] inline std::optional<AbilityKind> attack_target_ability(const SpecialAbilityKind kind) noexcept {
    switch (kind) {
    case SpecialAbilityKind::tractor_beam: return AbilityKind::tractor_beam;
    case SpecialAbilityKind::energy_weapon: return AbilityKind::energy_weapon;
    case SpecialAbilityKind::leech_shields: return AbilityKind::leech_shields;
    case SpecialAbilityKind::super_laser: return AbilityKind::super_laser;
    case SpecialAbilityKind::concentrate_fire: return AbilityKind::concentrate_fire;
    case SpecialAbilityKind::lucky_shot: return AbilityKind::lucky_shot;
    default: return std::nullopt;
    }
}

// Base order preserves the original field construction and reverse destruction order.
// Canonical state and checkpoints enumerate named fields, never object layout.
class TacticalSession::Impl final
    : public session_detail::SessionStoreRetreat,
      public session_detail::SessionStoreAbilityLocks,
      public session_detail::SessionStoreBonuses,
      public session_detail::SessionStoreInitialization,
      public session_detail::SessionStoreIncomeService,
      public session_detail::SessionStoreAbilityEffects,
      public session_detail::SessionStoreRules,
      public session_detail::SessionStorePads,
      public session_detail::SessionStoreEnvironment,
      public session_detail::SessionStoreHealing,
      public session_detail::SessionStoreEconomy,
      public session_detail::SessionStoreOutcome,
      public session_detail::SessionStoreHistory,
      public session_detail::SessionStoreSquadronRoster,
      public session_detail::SessionStoreVisibility,
      public session_detail::SessionStoreReinforcements,
      public session_detail::SessionStorePaths,
      public session_detail::SessionStoreProjectiles,
      public session_detail::SessionStoreFlightScratch,
      public session_detail::SessionStoreTickWork,
      public session_detail::SessionStoreChaseScratch,
      public session_detail::SessionStoreFlights,
      public session_detail::SessionStoreStationSpawn,
      public session_detail::SessionStoreDeathSpins,
      public session_detail::SessionStoreCollections,
      public session_detail::SessionStoreProjectileIds,
      public session_detail::SessionStoreRegistry,
      public session_detail::SessionStoreCommands,
      public session_detail::SessionStoreObjectTurn,
      public session_detail::SessionStoreSpecialWeapons,
      public session_detail::SessionStorePublication {
public:
    bool publish_damage_changes{true}; // transient observer switch, never canonical
    std::unique_ptr<session_detail::LandSession> land;
    [[nodiscard]] core::Result<void> initialize_land(const TacticalContent& content);
    [[nodiscard]] core::Result<void> spawn_land_company(const LandTerrain& terrain, const UnitState& entry,
        const LandCompanyBinding& binding, LandMovement::Checkpoint& moving, std::vector<LiveUnit>& live, LandSpawnFilter& filter);
    [[nodiscard]] core::Result<TacticalTick> step_land(TacticalSession& session, const PartitionExecutor& executor);
    // G-04..G-07 (session_land_garrison.cpp): the garrison service seam after the movement and occupancy commit.
    [[nodiscard]] core::Result<void> land_garrison_service(std::vector<LiveUnit>& live, const PartitionExecutor& executor);
    // WLAB-01..11, WLAB-77 (#3238, session_land_abilities.cpp): a land ability command, the per-tick expiry, and the Force
    // disabling window the enabled gate reads.
    void land_ability_command(std::vector<LiveUnit>& live, const PlayerCommand& command, std::vector<core::Diagnostic>& diagnostics);
    void land_ability_service(std::vector<LiveUnit>& live);
    [[nodiscard]] bool land_force_disabled(EntityId unit, std::uint64_t tick) const;
    void land_force_disable_service(const std::vector<LiveUnit>& live);
    void land_weapon_swap_service(const std::vector<LiveUnit>& live); // WLAB-71 (#3239)
    void land_jump_service(std::vector<LiveUnit>& live);              // WLAB-16 (#7084, land_jump.cpp)
    [[nodiscard]] std::vector<std::uint8_t> land_bytes(bool partitioned) const;
    void encode_land_partition(std::size_t partition) const;
    [[nodiscard]] core::Result<void> restore_land(std::span<const std::uint8_t> bytes);
    [[nodiscard]] RetreatGate retreat_gate(PlayerId player) const;
    [[nodiscard]] std::vector<RetreatView> retreat_views() const;
    // TSW-13: the switch presenter's rows from the committed records and the published instances (ascending id).
    [[nodiscard]] std::vector<TswView> tsw_views(std::span<const TacticalInstance> instances) const;
    [[nodiscard]] std::vector<RetreatActor> retreat_actors(const UnitStage* staged = nullptr) const;
    [[nodiscard]] bool retreat_holds(EntityId id) const;
    [[nodiscard]] bool retreat_escape(EntityId id) const;
    Impl(const TacticalSetup& source, const std::span<const SensorProfile> sensor_table, const DurabilityTable& table,
        const MotionTable& motion_table, const std::optional<FogRules>& fog_rules, const CombatTable& combat_table,
        const VictoryRules& victory_rules, const AbilityTable& ability_table, const EconomyRules& economy_rules,
        const MapBox& box);

    // A counted star base (#77, VT-03) joins the ascending-ID list the victory evaluation walks.
    // Tick lifecycle notifications reconcile destruction and ownership conversion (WBF-31).
    void add_starbase(const UnitState& unit);

    // A unit entering the session at `frame`: full health, at rest, no target (#72, #70, #73).
    // With damage rules its shield and energy recharge on phases of its own (DG-13, EN-02): the
    // first at `frame` plus a keyed draw in [0, interval - 1], then every interval.
    [[nodiscard]] std::optional<DurabilityState> entering_durability(
        const DurabilityProfile* profile, const EntityId unit, const std::uint64_t frame) const;

    [[nodiscard]] LiveUnit new_unit(const UnitState& unit, const std::uint64_t frame) const;

    [[nodiscard]] std::vector<std::size_t> radar_slots(TypeId type) const;
    [[nodiscard]] bool radar_mode_multiplayer() const;
    void record_radar_applied(NamedAbilityLocks&, PlayerId owner, TypeId type) const;
    [[nodiscard]] core::Result<std::vector<SnapshotPlayer>> publish_named_abilities(
        std::vector<LiveUnit>& units, const NamedAbilityLocks&, const RadarRoster&, const PartitionExecutor&) const;

    [[nodiscard]] const DurabilityProfile* health_profile(const LiveUnit& unit) const noexcept;
    // WTA-46: a type's profile as `owner`'s objects have it, before any upgrade bonus: the table's
    // profile, or the owner's difficulty-scaled copy. Resolved at the current owner, so a transfer
    // between players of different levels changes it (apply_bonuses restores the percentages).
    [[nodiscard]] const DurabilityProfile* base_profile(PlayerId owner, TypeId type) const noexcept;
    // WTA-46: a cause-damage mode (WHE-51; 1 for a shooter that is gone) times the difficulty damage
    // scale of the shooter's owner. A Normal owner returns the mode itself.
    [[nodiscard]] core::Result<math::Fixed> caused_damage(math::Fixed mode, PlayerId owner) const;

    // WBR-01: the player reveal cache (ledger multipliers and the last multipliers the fog service saw) exists
    // for campaigns and for standalone content with a nonzero player reveal contribution. Every consumer
    // (construction seeding, source-event refresh, the fog service, checkpoint save/restore) uses this one
    // predicate, so a resumed battle keeps the state an uninterrupted one has.
    [[nodiscard]] bool tracks_player_reveal() const noexcept {
        return setup.campaign || std::any_of(economy.command_bonuses.begin(), economy.command_bonuses.end(),
            [](const auto& row) { return row.battlefield && row.reveal_delta != math::Fixed{}; });
    }
    // WBR-02/03: the stealth, capture-time and special-weapon player ledgers add checkpoint fields only for content
    // whose live sources author one, so every other closure keeps the bytes it always had.
    [[nodiscard]] bool tracks_player_effects() const noexcept {
        return std::any_of(economy.command_bonuses.begin(), economy.command_bonuses.end(), [](const auto& row) {
            return row.battlefield && (row.stealth_delta != math::Fixed{} || row.capture_delta != math::Fixed{} || row.special_weapons);
        });
    }
    [[nodiscard]] std::array<math::Fixed, max_players> fog_multipliers(const CommandLedger& ledger) const {
        std::array<math::Fixed, max_players> values{};
        for (std::size_t i = 0; i < setup.players.size(); ++i) {
            const auto player = setup.players[i].player_id;
            const auto found = std::lower_bound(ledger.reveal_multipliers.begin(), ledger.reveal_multipliers.end(), player,
                [](const auto& row, const PlayerId id) { return row.first < id; });
            values[i] = found != ledger.reveal_multipliers.end() && found->first == player
                ? found->second : math::Fixed::from_raw(math::Fixed::scale);
        }
        return values;
    }
    [[nodiscard]] core::Result<CommandLedger> update_command_ledger(const std::vector<LiveUnit>& live,
        const EntityId first_new, const PartitionExecutor& executor) const;
    [[nodiscard]] CombatBonuses command_bonuses_for(const LiveUnit& unit, const CommandLedger& ledger,
        const std::vector<PlayerEconomy>& accounts, const std::map<EntityId, TypeId>& containers,
        const std::span<CombatBonuses> categories, const UnitStage* effect_world = nullptr,
        bool targeted_effects = true, std::optional<PlayerId> owner = std::nullopt, bool retained_only = false) const;
    [[nodiscard]] core::Result<void> apply_command_bonuses(std::vector<LiveUnit>& live, const CommandLedger& ledger,
        const std::vector<PlayerEconomy>& accounts, const EntityId first, const PartitionExecutor& executor) const;
    [[nodiscard]] core::Result<void> service_income_modifiers(std::vector<PlayerEconomy>& accounts,
        std::span<const LiveUnit> live, std::span<const std::pair<EntityId, PlayerId>> streams,
        std::uint64_t frame, const PartitionExecutor& executor, const NamedAbilityLocks& locks, bool initialization_only = false);
    void retire_income_modifier(PlayerEconomy& account, const CompletedBuild& held) const;
    void reduce_income(const std::vector<PlayerEconomy>& accounts, EntityId stream,
        std::span<IncomeCategory> categories) const;

    [[nodiscard]] std::size_t ownership_slot(const PlayerId player, const TypeId type) const;
    void adjust_owned(const UnitState& unit, const bool added);
    [[nodiscard]] core::Result<void> count_owned(const std::vector<LiveUnit>& units, const PartitionExecutor& executor);

    [[nodiscard]] CombatBonuses bonuses_for(const LiveUnit& unit, const bool committed = false) const;

    [[nodiscard]] CombatBonuses profile_bonuses(const OwnershipKey key, const std::vector<PlayerEconomy>& accounts,
        const std::map<EntityId, TypeId>& containers, const std::span<CombatBonuses> categories,
        bool total = true, bool retained_only = false) const;

    [[nodiscard]] core::Result<void> build_bonus_profiles(const std::vector<PlayerEconomy>& accounts,
        const std::map<EntityId, TypeId>& containers, const PartitionExecutor& executor);

    // WPR-57: stock skirmish has one station per team; holder loss destroys its held sources.
    // The debug build's alternate-holder path is outside this scope (#1016).
    [[nodiscard]] bool prune_upgrade_holders(std::vector<PlayerEconomy>& accounts,
        const std::vector<LiveUnit>& live) const;

    // WPR-51: a changed maximum adds its delta to current hull/shield/energy; hardpoints scale.
    enum class BonusAdjustment { legacy, gain, loss, ownership };
    // `previous` (ownership only): the profile the unit had before the transfer, when the new owner's
    // difficulty scales it differently (WTA-46); the percentages restore against it.
    [[nodiscard]] core::Result<void> apply_bonuses(LiveUnit& unit, const CombatBonuses& bonuses,
        BonusAdjustment adjustment = BonusAdjustment::legacy, const DurabilityProfile* previous = nullptr) const;
    [[nodiscard]] core::Result<void> transfer_owner(LiveUnit& unit, PlayerId owner,
        const CombatBonuses& bonuses, EntityId controller_root = invalid_entity_id) const;
    [[nodiscard]] SeatId output_controller(PlayerId owner, SeatId preferred, std::uint64_t ordinal) const;
    [[nodiscard]] bool shared_controller_type(TypeId type) const;
    [[nodiscard]] core::Result<void> attribute_output(UnitState& unit, SeatId preferred = {},
        std::uint64_t ordinal = 0) const;

    [[nodiscard]] ProductionCounts production_counts(const PlayerId buyer, const TypeId type,
        const std::vector<PlayerEconomy>& accounts, const bool committed = false) const;

    // #76: a unit whose type has abilities enters with every one off and ready (AB-10).
    [[nodiscard]] std::optional<AbilityState> entering_abilities(const TypeId type, const PlayerId owner) const;
    void refresh_damage_modes(session_detail::LiveUnit& unit) const;
    [[nodiscard]] const BuzzDroidsProfile* buzz_profile(TypeId source) const;
    [[nodiscard]] const SpawnedAbilityProfile* spawn_profile(TypeId) const;
    void service_combat_mods(const std::vector<LiveUnit>&, std::uint64_t);
    [[nodiscard]] core::Result<void> service_weaken(std::vector<LiveUnit>&, std::uint64_t, const PartitionExecutor&);
    [[nodiscard]] const SpecialAbilityProfile* beam_profile(const LiveUnit&, AbilityKind) const;
    [[nodiscard]] bool beam_target_valid(const LiveUnit&, const LiveUnit&, const SpecialAbilityProfile&, bool admission = true) const;
    [[nodiscard]] bool beam_in_range(const LiveUnit&, const LiveUnit&, const SpecialAbilityProfile&) const;
    // The source-side gates of a targeted beam's switch-on: handler live and user-input, slot ready.
    [[nodiscard]] bool beam_usable(const LiveUnit&, AbilityKind, std::uint64_t) const;
    // Switches a targeted beam on and locks it onto `target` (a command or autofire, WAB-36).
    [[nodiscard]] AbilitySwitch lock_beam(LiveUnit&, EntityId target, AbilityKind, std::uint64_t) const;
    void release_beam(LiveUnit&, AbilityKind, std::uint64_t) const;
    // WHE-59: an energy beam locked without a chosen hardpoint aims at the target's nearest standing
    // targetable hardpoint (authored order on ties), else the hull (no_hardpoint).
    [[nodiscard]] std::uint32_t energy_hardpoint(const LiveUnit& source, const LiveUnit& target) const;
    void register_tractor(const LiveUnit&, const LiveUnit*);
    // `laser_pass`: the one service per tick that also runs the SUPER_LASER aiming, firing and continuation stages (WSPL-08).
    [[nodiscard]] core::Result<void> service_beams(std::vector<LiveUnit>&, std::uint64_t, const PartitionExecutor&, bool laser_pass = false);
    // WSPL-03/04/08: the behaviour's turn toward the locked target, the cone wait, the attack-clip gate and the fire stage.
    void turn_laser(LiveUnit&, const SpecialAbilityProfile&, SpecialAbilitySlot&, const LiveUnit* target) const;
    void fire_laser(LiveUnit&, const SpecialAbilityProfile&, std::size_t ability_index, SpecialAbilitySlot&, const LiveUnit& target,
        std::uint64_t tick) const;
    // WSPL-08: the target left the world after the beam fired: the use continues without damage for Fire_Continuation.
    void detach_laser(LiveUnit&, const SpecialAbilityProfile&, SpecialAbilitySlot&, std::uint64_t tick) const;
    [[nodiscard]] bool buzz_request_ready(const LiveUnit&, math::Vec3, std::uint64_t) const;
    [[nodiscard]] core::Result<LiveUnit> create_buzz_droids(LiveUnit&, math::Vec3, std::uint64_t,
        EntityId& allocated_next_id, SeatId controller = {});
    // WLD-03..WLD-05: active LASER_DEFENSE looks for a hostile shot to shoot down before the shots fly.
    [[nodiscard]] core::Result<void> service_laser_defence(const std::vector<LiveUnit>&, std::uint64_t,
        const PartitionExecutor&, std::vector<CombatEvent>& zaps);
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
    [[nodiscard]] const AbilitySlot* lucky_slot(const LiveUnit& unit) const;
    [[nodiscard]] AbilitySlot* lucky_slot(LiveUnit& unit) const;
    [[nodiscard]] const SpecialAbilityProfile* lucky_profile(const LiveUnit& unit) const;
    [[nodiscard]] bool lucky_target_valid(const LiveUnit& source, const LiveUnit& target) const;
    [[nodiscard]] bool lucky_in_range(const LiveUnit& source, const LiveUnit& target) const;
    void end_lucky_shot(LiveUnit& unit, std::uint64_t tick) const;
    void lock_lucky_shot(LiveUnit& unit, EntityId target, std::uint32_t hardpoint, std::uint64_t tick) const;
    void reset_lucky_weapon(LiveUnit& unit) const;
    [[nodiscard]] const AbilitySlot* ion_slot(const LiveUnit& unit) const;
    [[nodiscard]] AbilitySlot* ion_slot(LiveUnit& unit) const;

    // AB-62 (#561): whether an ION_CANNON_SHOT of `owner`'s squadron may lock onto `target`: a
    // live unit of a hostile player that can be hit (a unit with a combat profile; a squadron's team
    // container has no model to aim at, its craft do).
    [[nodiscard]] bool ion_target_valid(const PlayerId owner, const LiveUnit* target) const;
    [[nodiscard]] bool ion_handler_admits(const LiveUnit& source, const LiveUnit* target, bool from_attack) const;

    // AB-65 (#561): the squadron's ION_CANNON_SHOT ends: the container and every craft switch it
    // off; the container recharges only when at least one craft fired (a live craft no longer due).
    void finish_ion_shot(AbilitySlot& team, const std::vector<AbilitySlot*>& craft, const TypeId container_type,
        const std::uint64_t tick) const;

    // AB-20: the unit's multiplier of one kind from its active abilities; 1 without any.
    [[nodiscard]] std::optional<math::Fixed> attack_distance(const LiveUnit& unit) const noexcept;
    [[nodiscard]] math::Fixed ability_factor(const LiveUnit& unit, const AbilityModifier modifier) const;

    // AB-43: DEFEND switched on brings the unit's next shield and energy recharges forward to the
    // next tick, from where they run at DEFEND's intervals.
    void defend_switched_on(LiveUnit& unit, const std::uint64_t tick) const;
    void service_blast(LiveUnit&, std::uint64_t tick) const; // FS-34: before hardpoint fire

    // AB-42: the hull and shield a hit took from a unit that runs the DEFEND stand-in.
    void track_damage(LiveUnit& unit, const math::Fixed hull_before, const math::Fixed shields_before) const;

    // IS-01 to IS-04, IS-07 (#561): an ion shot stuns the unit it hit; the stun ends an active
    // DEFEND (early, AB-12). Speed changes wait for the next tick's ability phase (IS-05, AB-24).
    void ion_stun_unit(LiveUnit& unit, const IonStunShot& shot, const std::uint64_t tick) const;
    // WST-03 to WST-08: the cloak's switch-on, its end and its per-frame detection.
    // `factor` scales the cloak's own count (WST-03); see stealth_duration_factor.
    void stealth_switched_on(LiveUnit& unit, std::uint64_t tick, math::Fixed factor) const;
    // WST-03: 1 plus the cloak-duration modifiers of the completed upgrades that reach `owner`:
    // the largest per stacking category, the categories summed. Read when the cloak switches on.
    [[nodiscard]] math::Fixed stealth_duration_factor(PlayerId owner, const std::vector<PlayerEconomy>& accounts) const;
    static void clear_stealth(LiveUnit& unit) noexcept;
    void detect_stealth(LiveUnit& unit, std::span<const LiveUnit> units, std::uint64_t tick) const;
    // WST-11: `target` is a stealth-invisible enemy (a squadron: every live craft is) and the
    // attacker's type (a squadron: its craft's) cannot target stealth units.
    [[nodiscard]] bool hidden_from(const UnitStage& staged, EntityId target, const LiveUnit& attacker, std::uint64_t tick) const;

    // IS-03: a unit's stun ends at its end frame. IS-05: neither its start nor its end plans a
    // move under way again; a move planned while it lasts keeps its cut speed.
    static void service_ion_stun(LiveUnit& unit, const std::uint64_t tick);

    // AB-17: a shield at zero ends DEFEND (early, AB-12); a speed change waits for the next tick's
    // ability phase (AB-24).
    void end_depleted_defend(LiveUnit& unit, const std::uint64_t tick, bool storm_branch = false) const;

    // One unit's ability service at `tick` (the ability phase, AB-11, AB-16, AB-41, AB-42).
    // Returns whether its speed multiplier changed since its move was planned.
    [[nodiscard]] bool service_abilities(LiveUnit& unit, const std::uint64_t tick) const;

    // WSD-02: switching SELF_DESTRUCT on or off clears the unit's attack target, as every behaviour
    // ability does (WAB-04); the move under way is kept.
    static void self_destruct_switched(LiveUnit& unit);

    // WSD-04, WSD-09, WSD-10: one unit's SELF_DESTRUCT at `tick`, in the ability phase. A unit whose
    // countdown has run out reports true (it detonates this tick's impact phase); a unit that runs the
    // Krayt object script services it when due and switches the ability on when it meets its trigger.
    // `units` are read-only here except `units[index]`; `craft_squadron` maps each craft to its squadron
    // container; `boxes` holds this tick's unit boxes (the proximity query, WSD-10).
    [[nodiscard]] bool service_self_destruct(std::vector<LiveUnit>& units, std::size_t index,
        const std::map<EntityId, EntityId>& craft_squadron, const detail::CollectionTrees& boxes,
        std::uint64_t tick) const;

    // FS-29/FD-47: disjoint update and nested sample; true stages a direct ring creation
    // in the same ability phase's ordered commit, never a future command.
    [[nodiscard]] bool service_mc30(LiveUnit& unit, std::span<const UnitState> inputs,
        const std::map<EntityId, EntityId>& craft_squadron, const detail::CollectionTrees& boxes,
        std::uint64_t tick) const;

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
        detail::MapStage<CraftState>& flights, detail::MapStage<SquadronState>& orders,
        TypeId free_type = 0, PlayerId free_owner = 0, std::vector<EntityId>* registered = nullptr,
        std::map<EntityId, ArrivalState>* lane_arrivals = nullptr, SeatId controller = {}) const;

    // PL-01: the shared immutable copy of a moving ship's plan (presentation only).
    [[nodiscard]] bool flies_plan(const LiveUnit& unit) const;
    [[nodiscard]] std::shared_ptr<const PredictedPath> cached_plan(const LiveUnit& unit) const;
    [[nodiscard]] std::shared_ptr<const PredictedPath> remember_plan(const LiveUnit& unit);
    void sweep_plans();

    // Publishes the snapshot of the staged units at the completed tick, with the given events.
    void publish_staged(const std::vector<LiveUnit>& live, std::vector<Event> events,
        std::vector<CombatEvent> combat_events);

    // #530: each ledger with its owner's population (PU-21) for presentation.
    [[nodiscard]] std::vector<PadView> pad_views() const;
    [[nodiscard]] std::vector<GarrisonTimerView> garrison_timer_views() const;
    [[nodiscard]] std::vector<std::pair<PlayerId, ManualPlayerClock>> manual_clock_views() const {
        return {manual_clocks.begin(), manual_clocks.end()};
    }

    [[nodiscard]] std::vector<EconomyView> economy_views(
        const std::vector<PlayerEconomy>& staged_ledgers, const std::map<EntityId, PopulationShare>& staged_shares) const;

    [[nodiscard]] bool allied(const PlayerId left, const PlayerId right) const;

    // WRB-05, WRB-06: the enabled Redirect_Blaster handlers of `defender` that react to a shot from `attacker`,
    // in authored order. A handler is enabled when its authored Initially_Enabled holds (true when absent) or an
    // Enable_Ability of a completed upgrade names it for the defender's type: held by the defender's owner, or
    // by an ally when the upgrade affects all allies.
    [[nodiscard]] std::vector<const SpecialAbilityProfile*> redirect_blaster_handlers(const LiveUnit& defender,
        const LiveUnit& attacker, const std::vector<PlayerEconomy>& staged_ledgers) const;

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

    enum class CompanyPlacement { exact, reinforcement };
    // A birth request carries placement/lifecycle choices, never admission. Pool, cap,
    // hero and command policy belong to the caller. Failures abort the enclosing tick;
    // units, craft/mind journals and lifecycle lists are published only by its commit.
    struct CompanySpawn {
        UnitState prototype;
        math::Fixed yaw{};
        CompanyPlacement placement{CompanyPlacement::exact};
        bool arrival{};
        bool vulnerable{};
        bool create_teams{true};
        std::optional<std::span<const TypeId>> members;
        std::uint32_t max_squad_size{10};
        std::uint32_t population{};
        std::uint32_t scan_delay{};
        EntityId persistent_root{};
    };
    struct CompanyBirth {
        EntityId root{};
        std::vector<EntityId> created;
    };
    [[nodiscard]] core::Result<CompanyBirth> spawn_company(const CompanySpawn& request,
        UnitStage& units, EconomyStage& stage, std::uint64_t tick);

    [[nodiscard]] PlayerEconomy* ledger_of(std::vector<PlayerEconomy>& staged_ledgers, const PlayerId id) const;
    // LC-42..51 (#5894), session_land.cpp and session_land_reinforce.cpp.
    // LC-43..45: every member position of the company laid out at `point` is placeable, enabled by an allied point and
    // outside every enemy prevention radius; false is a refusal, an error is an arithmetic failure.
    [[nodiscard]] core::Result<bool> land_company_admission(const LandCompanyBinding& binding, PlayerId player,
        const math::Vec3& point, math::Fixed yaw_degrees, const UnitStage& staged) const;
    // LC-48: the capacity the player's live objects (and, in multiplayer, its allies') add to the faction base.
    [[nodiscard]] std::int64_t land_capacity(PlayerId owner, const UnitStage& staged) const;
    // LC-46/47: the company cap of one player.
    [[nodiscard]] std::int64_t land_company_cap(PlayerId owner, std::int64_t capacity) const;
    // LC-48: companies counted against the cap: the shares of the owner's objects plus its companies in flight.
    [[nodiscard]] std::int64_t land_company_count(PlayerId owner, const std::map<EntityId, PopulationShare>& owner_shares,
        const UnitStage& staged) const;
    // #7190: the read-only verdict a land player's drop preview and drop check use: the company's cap room (LC-46..48) and the
    // whole-company admission (LC-43..45) at `point`, under the owner's reinforcement facing (LR-FACING). Reads the completed
    // world only: no RNG, no cache, no state written.
    [[nodiscard]] core::Result<bool> land_reinforcement_point(PlayerId player, TypeId type, const math::Vec3& point) const;
    // WLAI-21..23 (#3209), session_land_landing_search.cpp: the position a land AI player's company is offered, ranked from the
    // player's allied reinforcement points; none when no ring position is admitted. `candidates` counts the admissions tried.
    [[nodiscard]] core::Result<std::optional<math::Vec3>> land_landing_search(PlayerId player, TypeId type, const math::Vec3& target,
        math::Fixed yaw_degrees, const UnitStage& staged, const std::map<EntityId, PopulationShare>& shares,
        std::uint32_t& candidates) const;
    // The land economy's resolution of an AI search request (WLAI-21..23), against the state the landing is admitted in: true
    // when the command goes on with the chosen position.
    [[nodiscard]] core::Result<bool> resolve_land_reinforcement_search(const CommandKey& key, ReinforcePayload& payload, UnitStage& staged,
        const std::vector<PlayerEconomy>& staged_ledgers, const std::map<EntityId, PopulationShare>& staged_shares);
    [[nodiscard]] core::Result<void> execute_land_reinforcement(const PlayerCommand& command, std::uint64_t tick,
        UnitStage& staged, EconomyStage& stage, PlayerEconomy& ledger, const EconomyPlayer& player, Event& event,
        std::vector<Event>& events);
    // WHE-63: append authored escorts immediately; a full team returns false without recharge.
    [[nodiscard]] core::Result<bool> replenish_wingmen(EntityId leader, TypeId team, UnitStage& units,
        EconomyStage& stage, std::uint64_t tick);
    [[nodiscard]] core::Result<RejectReason> request_replenishment(EntityId leader, UnitStage& units,
        EconomyStage& stage, std::uint64_t tick);
    [[nodiscard]] core::Result<void> request_creation_defaults(std::span<const EntityId> created,
        UnitStage& units, EconomyStage& stage, std::uint64_t tick);

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
        const std::vector<UnitState>* prevention_units = nullptr, std::optional<math::Fixed> facing_yaw = {}) const;

    // PL-05: the world box a unit blocks in an arrival's free-space search, from its type's placement
    // box and the yaw of its rotation; a type with no box never blocks (PL-02).
    [[nodiscard]] core::Result<void> block_placement(const UnitState& unit, std::vector<PlacementBox>& blockers) const;

    // #530: one buy, cancel or reinforce of the frame's commands phase (PU-10 to PU-17, PU-30 to
    // PU-34), serial in command order. Appends its event.
    [[nodiscard]] core::Result<void> execute_economy(const PlayerCommand& command, const std::uint64_t tick,
        UnitStage& staged, EconomyStage& stage, std::vector<Event>& events,
        const CollisionWorld* collisions = nullptr, const bool executing_pad = false);

    // WR-43: an arrival whose reveal service is disabled keeps the circle it marked at creation, at
    // the drop point, until the service enables again (WR-39).
    struct HeldReveal {
        EntityId id{};
        math::Vec3 position{};
    };
    // Live units with a sensor profile, in the order of `units` (ascending ID). A unit in `held`
    // (ascending ID) reveals from its held point instead of its position.
    [[nodiscard]] static std::vector<FogRevealer> revealers(const SensorField& field, const std::span<const UnitState> units,
        const std::span<const HeldReveal> held = {});

    // Own-team players always see a unit (V-04). Other players see it by the retail fog cells
    // when the session is bound to fog rules (V-11 to V-17), else by the exact range test.
    [[nodiscard]] std::uint64_t visible_to(const SensorField& field, const FogCells* cells, const PlayerId owner,
        const math::Vec3& position, TypeId type = {}, math::Quat rotation = math::identity_quat()) const;

    // `now`: the tick the snapshot's readers act at (the completed tick it is published with).
    [[nodiscard]] TacticalInstance instance_for(const LiveUnit& unit, const math::Mat3x4& transform,
        const std::uint64_t now, const std::map<EntityId, SpecialWeaponState>* staged_special = nullptr) const;

    [[nodiscard]] LiveUnit live_at(const EntityId id, const entt::entity handle) const;
    // WU-37: whether the committed unit carries the garrison birth mark (false for an unknown ID).
    [[nodiscard]] bool garrison_marked(EntityId id) const;

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

    [[nodiscard]] core::Result<PathInputs> path_inputs(const LiveUnit& unit, const std::optional<math::Fixed> max_speed) const;

    // plan_path's last step: the unit takes `plan`.
    [[nodiscard]] core::Result<void> take_plan(LiveUnit& unit, MotionState plan,
        math::Vec3 target, std::uint64_t start) const;
    static void retain_path_failure(LiveUnit& unit, math::Vec3 target, std::uint64_t frame, math::Fixed max_speed);

    // plan_path, whose path search (a tracked unit with a world) is given up once it has counted
    // `budget` expansions (PC-08): false then, and the unit is unchanged.
    [[nodiscard]] core::Result<bool> plan_path_within(LiveUnit& unit, const math::Vec3 target, const std::uint64_t start,
        const CollisionWorld* world, const std::optional<math::Fixed> max_speed, PathSearchStats* const stats,
        const std::optional<std::uint64_t> budget) const;

    [[nodiscard]] core::Result<SlicedSearch> start_sliced(const LiveUnit& unit, const math::Vec3 target,
        const std::optional<math::Fixed> max_speed, const std::uint64_t now, const std::uint64_t frame,
        const CollisionWorld& world) const;

    // --- Approaching a unit (#452, docs/behaviour/space-orders.md OR-02 to OR-17) --------------

    struct ApproachPlan {
        math::Vec3 slot{};
        std::uint64_t prediction_frame{};
        std::optional<math::Vec3> centre{}; // OR-28: absent for a last-seen position rather than a target ring.
    };

    // The unit the order in force sends a unit towards (OR-08, OR-17): an attack's target while
    // it is still the unit's player-ordered target (T-01 drops it), an attack-move's or guard's
    // unit. Zero otherwise, and for a unit without a motion profile.
    [[nodiscard]] static EntityId approach_target(const LiveUnit& unit) noexcept;

    // WMV-13 (#719): the unit the order in force names (attack, attack-move, guard), whether or
    // not T-01 still holds it as the unit's target. Zero otherwise, and for a unit without a
    // motion profile.
    [[nodiscard]] static EntityId order_unit(const LiveUnit& unit) noexcept;

    // OR-06: an approach is checked every reevaluation interval after the order's tick.
    [[nodiscard]] bool approach_due(const LiveUnit& unit, const std::uint64_t tick) const noexcept;

    // OR-03, OR-04, OR-14: the approach distance of the unit's order, also its in-range test.
    [[nodiscard]] math::Fixed approach_range(const LiveUnit& unit, const bool guard,
        const detail::CombatUnit* target = nullptr) const noexcept;

    // OR-21: an attack may name a hardpoint of the target's type that is targetable and standing.
    [[nodiscard]] bool target_hardpoint_standing(const LiveUnit& target, const std::uint32_t index) const;

    // OR-04, OR-14: the point the in-range test measures to: a guarded unit's position, else the
    // aim point of A-04 seen from `unit` (its best hardpoint, WCC-18); OR-25: an attack on a
    // hardpoint measures to that hardpoint while it stands.
    [[nodiscard]] math::Vec3 approach_point(const LiveUnit& unit, const bool guard, const LiveUnit& target,
        const detail::CombatUnit* target_view, const std::uint32_t hardpoint = attack_hull) const;

    // Where the unit's current movement leaves it (OR-06): the tracked prediction of its path at
    // the path's end (where the last frame before the last node puts it, MV-32), else where it is.
    // #662: sampling the path after its end gives back the position passed in, the unit's current
    // one, so every check found the end out of range and planned the approach again.
    [[nodiscard]] static core::Result<math::Vec3> movement_end(const LiveUnit& unit);

    // OR-07: where `target` will be at frame `at` on its own planned path, seen at `frame`.
    [[nodiscard]] static core::Result<math::Vec3> predicted_position(
        const LiveUnit& target, const std::uint64_t frame, const std::uint64_t at);

    // OR-07: footprinted none/static layers predict from their resolved aim; moving
    // layers and footprint-less movers retain parent tracking. Guards pass the parent.
    [[nodiscard]] core::Result<math::Vec3> predicted_approach_position(const LiveUnit& target,
        const math::Vec3& aim, const std::uint64_t frame, const std::uint64_t at) const;

    // OR-05, OR-07: a new approach towards `target` from where the unit is at `frame`.
    [[nodiscard]] core::Result<ApproachPlan> approach_mapping(
        const LiveUnit& unit, const LiveUnit& target, const math::Fixed range, const std::uint64_t frame,
        const std::optional<math::Vec3>& aim = std::nullopt) const;

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
    // shared pool. Retain handles and unchanged components. The destroy sweep, every emplace/remove and every new
    // entity run serially in ascending ID; the changed values of existing components are assigned in place by the
    // `registry-commit` phase over the fixed index partitions (below 256 units, inline as one partition). A failed
    // phase leaves the commit unfinished: the tick fails and the session must not be stepped again.
    [[nodiscard]] core::Result<void> commit_units(std::vector<LiveUnit>& units, std::uint64_t& writes,
        const PartitionExecutor& executor);
    // The original serial commit, kept as the test-only oracle for commit_units.
    void commit_units_reference(std::vector<LiveUnit>& units, std::uint64_t& writes);

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
        LiveUnit& unit, bool projectile = false) const;

    [[nodiscard]] std::vector<std::uint8_t> canonical_bytes() const;
    [[nodiscard]] std::vector<std::uint8_t> canonical_bytes(const PartitionExecutor& executor) const;
    [[nodiscard]] std::vector<std::uint8_t> canonical_bytes(std::span<const LiveUnit> units,
        std::span<const std::vector<std::uint8_t>> encoded_units,
        const CanonicalEncoding* encoding = nullptr) const;
    void append_canonical_unit(std::vector<std::uint8_t>& bytes, const LiveUnit& unit) const;
    void append_canonical_units_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const std::span<const std::vector<std::uint8_t>> encoded_units) const;
    void append_canonical_garrison_enabled_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_named_locks_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_special_weapons_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_tsw_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_garrisons_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_manual_weapons_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_turrets_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_squadrons_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_fog_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_tracking_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_roll_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_projectiles_state(std::vector<std::uint8_t>& bytes,
        std::span<const std::vector<std::uint8_t>> encoded = {}) const;
    void append_canonical_blast_damage_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_formation_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_approach_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_sightings_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_garrison_marks_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_script_invulnerable_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_script_view_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_hold_fire_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_craft_state(std::vector<std::uint8_t>& bytes,
        std::span<const std::vector<std::uint8_t>> encoded = {}) const;
    void append_canonical_squadron_orders_state(std::vector<std::uint8_t>& bytes,
        std::span<const std::vector<std::uint8_t>> encoded = {}) const;
    void append_canonical_attack_paths_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_hangars_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_collection_state(std::vector<std::uint8_t>& bytes,
        std::span<const std::vector<std::uint8_t>> encoded = {}) const;
    void append_canonical_projectile_collection_state(std::vector<std::uint8_t>& bytes,
        std::span<const std::vector<std::uint8_t>> encoded = {}) const;
    void append_canonical_economy_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_arrivals_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_pads_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_population_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_arrival_defence_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_upgrades_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_level_up_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_income_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_outcome_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_abilities_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr,
        std::span<const std::vector<std::uint8_t>> encoded = {}) const;
    void append_canonical_stealth_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_stealth_end_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_self_destruct_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_mc30_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_ion_stuns_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_hazard_moves_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_final_facing_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_deploy_pose_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_asteroids_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_engine_disable_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_nebula_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_storms_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_damage_tracking_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_command_bonuses_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_heroes_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_projectile_defence_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_special_abilities_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_concentrate_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_ability_spawns_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_buzz_requests_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_buzz_droids_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_spins_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_departure_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_quits_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_pool_tokens_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_queue_identities_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_cinematic_seats_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_campaign_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_corruption_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_charged_blast_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_waypoints_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    void append_canonical_beacons_state(std::vector<std::uint8_t>& bytes) const;
    void append_canonical_order_history_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts = nullptr) const;
    [[nodiscard]] bool special_weapons_enabled(PlayerId player, const std::vector<PlayerEconomy>& accounts,
        const UnitStage* live = nullptr, const std::set<PlayerId>* staged_disabled = nullptr) const;

    core::Result<void> service_healing(std::vector<LiveUnit>& units, const std::map<EntityId, ArrivalState>& arrivals,
        std::uint64_t tick, const PartitionExecutor& executor, std::vector<CombatEvent>& events);
    core::Result<void> service_jamming(std::vector<LiveUnit>& units, const std::map<EntityId, ArrivalState>& arrivals,
        const NamedAbilityLocks& locks, std::uint64_t tick, const PartitionExecutor& executor,
        std::vector<std::int8_t>& transitions);
    [[nodiscard]] bool jammer_ready(const LiveUnit& unit, const AbilityProfile& ability,
        const NamedAbilityLocks& locks) const;
    [[nodiscard]] bool ordinary_handler_ready(const LiveUnit& unit, const AbilityProfile& ability,
        const NamedAbilityLocks& locks, SpecialAbilityKind kind) const;
    // WJM-05: whether a nested jammer handler carries the row's authored name; without one the row is inert.
    [[nodiscard]] static bool has_jammer_handler(const UnitAbilityProfile& profile, const AbilityProfile& ability);
    [[nodiscard]] bool jammer_reactivated(LiveUnit& unit, const AbilityProfile& ability) const;
    // WSH-05: the inclusive planar centre test, then the rolled model box; shared by healing and WSH-09 damage.
    [[nodiscard]] bool heal_in_range(const LiveUnit& source, const LiveUnit& target, math::Fixed reach) const;
    // WSH-13: a healing source is alive by its hull; a squadron company (the team container) has no hull and serves while it exists.
    // "While it exists" relies on an emptied company being erased (the team's destroy) before the systems stage of the next tick,
    // so a company with no live member never reaches the service; a hangar-held squadron has no session entity until it launches.
    [[nodiscard]] bool heal_source_serves(const LiveUnit& source) const;
    // WSH-09: immutable selection of due negative-amount automatic handlers and their victims, in source order.
    core::Result<void> select_passive_damage(const UnitStage& staged, const std::map<EntityId, ArrivalState>& arrivals,
        std::uint64_t tick, const PartitionExecutor& executor, std::vector<PassiveDamagePulse>& pulses);
};

} // namespace eawr::sim::tactical
