#pragma once

#include "eawr/sim/galactic/corruption_model.hpp"

#include "eawr/core/result.hpp"
#include "eawr/sim/commands.hpp"
#include "eawr/sim/seat.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/galactic/observer_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <limits>
#include <span>
#include <string_view>
#include <string>
#include <vector>

// The galactic simulation's setup and object types (ADR-012, docs/galactic-architecture.md
// section 1). Planets, trade routes and fleets share one stable-ID space: nonzero, never reused,
// handed out by one allocator per game session (ADR-014), so a ship keeps its ID from the galaxy
// into a battle and back.
namespace eawr::sim::galactic {

using PlayerId = std::uint32_t;
using sim::SeatId;
using sim::SeatKind;
using HumanSideId = std::uint32_t;
using FactionId = std::uint32_t;
using TypeId = std::uint64_t;
using PlanetId = EntityId;
using RouteId = EntityId;
using FleetId = EntityId;

// A player of the galaxy: the owner of planets, fleets and (later) credits, tech and victory
// (ADR-020: players own, seats command). Faction and flags are the campaign data's; the galaxy
// gives them no meaning yet.
struct GalacticPlayer {
    PlayerId player_id{};
    FactionId faction_id{};
    std::uint32_t flags{};
    friend constexpr bool operator==(const GalacticPlayer&, const GalacticPlayer&) noexcept = default;
};

// A seat: one controller seated in exactly one player (ADR-020: seats command, players own).
// Every command is issued by a seat. Vanilla GC has one human seat per human player, coop GC two
// human seats in one player; every AI player and each script host get a seat of their own, so all
// input has the same shape. `active` is the tick-zero state; a seat leaving or rejoining is a
// session command (coop seats work). The human roster is at most eight seats on one coop side
// or exactly two versus sides, explicitly grouped independently of owning player/faction.
// AI/script players do not consume human seats or sides.
struct Seat {
    SeatId seat_id{};
    PlayerId player{};
    SeatKind kind{SeatKind::human};
    std::uint32_t lobby_slot{};
    bool active{true};
    HumanSideId human_side_id{}; // nonzero for humans; zero for system issuers (AI/script)
    friend constexpr bool operator==(const Seat&, const Seat&) noexcept = default;
};

// WGS-P01..P06: explicit session policy, independent of persistent ownership.
enum class FactionControl : std::uint8_t { human = 1, ai = 2, passive = 3 };
struct TeamLead {
    HumanSideId side{};
    SeatId lead{};
    bool observer_ai{}; // agreed filtered battle perspective
    friend bool operator==(const TeamLead&, const TeamLead&) = default;
};
struct FactionController {
    PlayerId player{};
    FactionControl kind{FactionControl::ai};
    FactionControl dropout{FactionControl::ai};
    std::vector<std::uint8_t> configuration, state;
    friend bool operator==(const FactionController&, const FactionController&) = default;
};
struct SeatControlSetup {
    std::vector<TeamLead> teams; // ascending side ID
    std::vector<FactionController> controllers; // every player, ascending player ID
    friend bool operator==(const SeatControlSetup&, const SeatControlSetup&) = default;
};
enum class BattleTeamRole : std::uint8_t { pending = 0, ai_side = 1, spectate = 2, ally = 3 };
struct BattleRoleChoice {
    EntityId conflict{};
    PlayerId human_player{}, ai_player{};
    HumanSideId choosing_side{};
    PlayerId observer_player{};
    BattleTeamRole role{BattleTeamRole::pending};
    friend bool operator==(const BattleRoleChoice&, const BattleRoleChoice&) = default;
};
struct SeatControlState {
    SeatControlSetup control;
    std::optional<BattleRoleChoice> battle;
    friend bool operator==(const SeatControlState&, const SeatControlState&) = default;
};
// WGS-P02/P04: proposed policy pending owner; bounded roster work, one replaceable function.
void update_seat_control_policy(SeatControlState& state, std::span<const Seat> seats);

// A planet at its galactic position, with its current owner. Target control, bases, slots,
// structures and the rest of WGS-49..52 join as their tickets land.
struct Planet {
    PlanetId id{};
    math::Vec3 position{};
    PlayerId owner{};
    friend constexpr bool operator==(const Planet&, const Planet&) noexcept = default;
};

// WGA-127/256: single authoritative access owner. A missing reveal override
// uses the live local observer's discovery record; story reveal writes are explicit.
struct PlanetAccess {
    PlanetId planet{};
    bool restricted{};
    std::optional<bool> local_revealed;
    friend bool operator==(const PlanetAccess&, const PlanetAccess&) = default;
};
struct PlanetAccessState {
    std::uint64_t generation{1};
    std::vector<PlanetAccess> planets; // ascending planet identity, complete table
    friend bool operator==(const PlanetAccessState&, const PlanetAccessState&) = default;
};

// A trade route between two distinct planets (WGM-16 owns the graph rule).
struct Route {
    RouteId id{};
    PlanetId from{};
    PlanetId to{};
    math::Fixed hs_speed_factor{math::Fixed::from_raw(math::Fixed::scale)};
    friend constexpr bool operator==(const Route&, const Route&) noexcept = default;
};

// One fleet member: its persistent ID and object type.
struct FleetMember {
    EntityId id{};
    TypeId type{};
    friend constexpr bool operator==(const FleetMember&, const FleetMember&) noexcept = default;
};

// A fleet in orbit of a planet, with its members in their fleet order (WGS-53: ordered
// membership, never re-sorted). Paths, hops and speeds join with fleet locomotion.
struct FleetTransit {
    std::vector<PlanetId> path;
    std::uint32_t hop{}; // source index; desired planet is hop + 1
    math::Vec3 desired{}, velocity{};
    math::Fixed base_speed{}, speed{};
    std::vector<EntityId> packed;
    std::optional<std::uint8_t> destination_slot;
    SeatId producer{};
    std::uint64_t sequence{};
    math::Vec3 desired_facing{};
    EntityId destination_actor{}; // WGM-14/34: original supplied identity, resolved on final arrival
    friend bool operator==(const FleetTransit&, const FleetTransit&) = default;
};
struct FleetMotion {
    math::Vec3 position{}, facing{};
    std::optional<FleetTransit> transit;
    std::uint64_t orbit_entry{}; // runtime orbit-list order, retained while enroute
    friend bool operator==(const FleetMotion&, const FleetMotion&) = default;
};
struct Fleet {
    FleetId id{};
    PlayerId owner{};
    PlanetId orbit{};
    std::vector<FleetMember> members;
    std::uint8_t orbit_slot{}; // WGM-15: logical 0/1/2, independent of travel
    FleetMotion motion{};
    std::optional<PlanetId> raid_destination{};
    friend bool operator==(const Fleet&, const Fleet&) = default;
};

// Existing live identities outside a fleet, awaiting attachment (production/setup seam).
// This is orbital presence; transport surface contents remain a separate owning lane.
struct DetachedUnit {
    FleetMember member;
    PlayerId owner{};
    PlanetId planet{};
    friend bool operator==(const DetachedUnit&, const DetachedUnit&) = default;
};
struct FleetTypeTraits {
    TypeId type{};
    bool transport{}, can_hyperspace_without_ability{}; // WGM-83, validated original types
    bool requires_live_children{}; // flat members cannot stand in for live container contents
    friend bool operator==(const FleetTypeTraits&, const FleetTypeTraits&) = default;
};
// Typed replayable inputs for WGM-07's mode admission. Later speech/conflict services own
// changes to these inputs; this lane does not infer them from presentation state.
struct FleetManagementGates {
    bool queued_campaign_conflict{}, active_speech{};
    friend bool operator==(const FleetManagementGates&, const FleetManagementGates&) = default;
};
struct FleetControlSetup {
    std::vector<DetachedUnit> detached_units; // ascending member ID
    std::vector<FleetTypeTraits> types; // ascending supplied TypeId
    FleetManagementGates gates;
    friend bool operator==(const FleetControlSetup&, const FleetControlSetup&) = default;
};

// Authored, replayable locomotion inputs. Eligibility and live children are supplied by
// the owning object/ability service, never inferred from an authored squadron template.
struct MovementType {
    TypeId type{};
    math::Fixed hyperspace_speed{}, build_cost{};
    std::uint32_t squadron_capacity{};
    bool hyperspace_eligible{}, squadron{}, fleet_container{}, skips_combat{};
    bool transport{}; // WGM-26/38: own travel speed, contained troops for combat skipping
    friend bool operator==(const MovementType&, const MovementType&) = default;
};
struct MovementContents {
    EntityId container{};
    std::vector<FleetMember> children;
    friend bool operator==(const MovementContents&, const MovementContents&) = default;
};
struct MovementPlayer {
    PlayerId player{};
    math::Fixed hyperspace_factor{}, corruption_bonus{};
    bool benefits_from_corruption{};
    friend bool operator==(const MovementPlayer&, const MovementPlayer&) = default;
};
struct MovementPlanet {
    PlanetId planet{};
    math::Fixed bounds_half_z{}; // WGM-28: model world bounds, not galaxy-art coordinates
    bool corrupted{};
    friend bool operator==(const MovementPlanet&, const MovementPlanet&) = default;
};
struct MovementAccess {
    PlayerId player{};
    PlanetId planet{};
    bool accessible{}, potential_conflict{}; // control/conflict-owner seams, WGM-31/32
    friend bool operator==(const MovementAccess&, const MovementAccess&) = default;
};
// WGM-37: immutable raid policy and resolved type categories from effective data.
struct RaidRules {
    FactionId required_faction{};
    std::uint32_t free_categories{}, limited_categories{}, max_heroes{}, max_limited{};
    friend bool operator==(const RaidRules&, const RaidRules&) = default;
};
struct RaidType {
    TypeId type{};
    std::uint32_t categories{};
    bool hero{}, stays_in_transport{};
    friend bool operator==(const RaidType&, const RaidType&) = default;
};
struct RaidSetup {
    RaidRules rules;
    std::vector<RaidType> types;
    friend bool operator==(const RaidSetup&, const RaidSetup&) = default;
};
struct MovementSetup {
    std::vector<MovementType> types;
    std::vector<MovementContents> contents;
    std::vector<MovementPlayer> players;
    std::vector<MovementPlanet> planets;
    std::vector<MovementAccess> access; // player, planet order; complete observer table
    math::Fixed mode_modifier{}; // explicit effective GC modifier, WGM-26; never a slider value
    std::optional<RaidSetup> raid{};
    friend bool operator==(const MovementSetup&, const MovementSetup&) = default;
};

// WGM-66: political owner, military controller and orbital occupant are separate.
struct ControlBase {
    PlayerId owner{};
    std::uint32_t level{};
    friend bool operator==(const ControlBase&, const ControlBase&) = default;
};
struct SurfaceForce {
    EntityId id{};
    TypeId type{};
    PlayerId owner{};
    friend bool operator==(const SurfaceForce&, const SurfaceForce&) = default;
};
struct ControlType {
    TypeId type{};
    std::uint8_t political_control{}; // WGM-67: eligible flattened forces author 0 or 1
    bool stealth{}; // supplied by the live ability service
    bool ground_capable{}; // supplied capability predicate, WGM-U09
    friend bool operator==(const ControlType&, const ControlType&) = default;
};
// GPC-01: an ability instance is keyed by actor and its authored profile index.
struct ControlAbility {
    bool protection{}, enabled{true};
    std::uint32_t category{};
    math::Fixed multiplier{};
    friend bool operator==(const ControlAbility&, const ControlAbility&) = default;
};
struct ControlAbilityType {
    TypeId type{};
    std::vector<ControlAbility> abilities;
    friend bool operator==(const ControlAbilityType&, const ControlAbilityType&) = default;
};
struct ControlAbilitySource {
    PlanetId planet{};
    EntityId actor{};
    TypeId type{};
    std::uint32_t ability{}; // zero-based index in the current type's abilities
    PlayerId owner{};
    friend bool operator==(const ControlAbilitySource&, const ControlAbilitySource&) = default;
};
// WGM-106: current ordinary-object facts; capability depends on this object's
// direct live payload, so it cannot be cached once per shared carrier type.
struct GroundCapabilityType {
    TypeId type{};
    math::Fixed combat_power;
    bool transport{}, stays_in_transport{};
    friend bool operator==(const GroundCapabilityType&, const GroundCapabilityType&) = default;
};
struct ControlPlayer {
    PlayerId player{};
    bool neutral{}, playable{};
    FactionId faction{}; // leaning compares factions, not owning player identities
    friend bool operator==(const ControlPlayer&, const ControlPlayer&) = default;
};
struct ControlRelation {
    PlayerId player{}, other{};
    bool allied{};
    friend bool operator==(const ControlRelation&, const ControlRelation&) = default;
};
struct ControlCap {
    PlayerId player{};
    std::uint8_t maximum{}; // resolved faction-name cap, WGM-95; absent means 5
    friend bool operator==(const ControlCap&, const ControlCap&) = default;
};
struct ControlTransitionModifier {
    std::uint32_t category{};
    EntityId source{};
    math::Fixed multiplier{};
    std::uint64_t expires_frame{}; // zero means no expiry
    friend bool operator==(const ControlTransitionModifier&, const ControlTransitionModifier&) = default;
};
// WGT-84 / WGM-187: separate tactical records, never galactic production actors.
struct TacticalBuiltStructure {
    TypeId type{};
    math::Vec3 position{};
    std::uint32_t enable_level{}; // WGT-84 validates 0..5
    PlayerId owner{};
    friend bool operator==(const TacticalBuiltStructure&, const TacticalBuiltStructure&) = default;
};
struct ControlPlanet {
    PlanetId planet{};
    TypeId type{}; // capture history is per original planet type, WGM-76
    bool surface_accessible{}, retains_residual{}, destroyed{};
    ControlBase ground_base{}, starbase{};
    std::vector<SurfaceForce> landed; // live surface-list order, WGM-66
    std::array<PlayerId, 2> production_owner{}; // ground then space, WGM-66
    PlayerId leaning_player{};
    math::Fixed leaning{};
    std::vector<math::Fixed> special_control; // ground then space; at most three, WGM-70
    std::vector<ControlCap> caps;
    std::vector<ControlTransitionModifier> transition_modifiers;
    math::Fixed capture_reward{};
    std::vector<TacticalBuiltStructure> built_structures; // retained build order, WGT-84
    friend bool operator==(const ControlPlanet&, const ControlPlanet&) = default;
};
struct ControlRoute {
    RouteId route{};
    math::Fixed political_gain{};
    friend bool operator==(const ControlRoute&, const ControlRoute&) = default;
};
struct PlanetControl {
    PlanetId planet{};
    bool initialized{};
    PlayerId current_player{}, target_player{}, controller{}, orbital_player{};
    math::Fixed current{}, target{};
    bool contested{}, blockaded{};
    math::Fixed political_income{}; // WGEP-08/75 retained source; neutral/nonpositive does not remove it
    friend bool operator==(const PlanetControl&, const PlanetControl&) = default;
};
struct CapturedPlanetType {
    PlayerId player{};
    TypeId type{};
    friend bool operator==(const CapturedPlanetType&, const CapturedPlanetType&) = default;
};
struct PoliticalControlSetup {
    math::Fixed change_seconds{}; // authored Political_Control_Change_Time_Seconds, WGM-73
    std::uint32_t max_ground_forces{}; // authored Max_Ground_Forces_On_Planet, WGM-39/60
    std::vector<ControlPlayer> players;
    std::vector<ControlRelation> relations; // complete ordered player/other table
    std::vector<ControlType> types;
    std::vector<ControlPlanet> planets;
    std::vector<ControlRoute> routes;
    std::vector<PlanetControl> state; // empty initializes at construction, WGM-C04
    std::vector<CapturedPlanetType> captured_types;
    std::vector<EntityId> absent_forces; // control roster deaths; full return reconciliation owns containers
    std::vector<GroundCapabilityType> ground_capability_types; // empty retains legacy supplied predicates
    std::vector<ControlAbilityType> ability_types; // current-type profiles, ascending type
    std::vector<ControlAbilitySource> ability_sources; // physical presence, ascending planet/actor/ability
    friend bool operator==(const PoliticalControlSetup&, const PoliticalControlSetup&) = default;
};
// Completed control commits; consumers own victory/story, economy, production,
// corruption and observer visibility. Effects carry no guessed subsystem defaults.
struct PoliticalControlEffect {
    PlanetId planet{};
    PlayerId old_owner{}, new_owner{};
    bool owner_changed{}, stop_production{}, refresh_visibility{};
    bool first_capture{};
    math::Fixed capture_reward{};
    friend bool operator==(const PoliticalControlEffect&, const PoliticalControlEffect&) = default;
};
// Ordinary FT-08 result input; the winner does not replace political ownership.
struct ControlBattleReturn {
    PlanetId planet{};
    PlayerId invader{}, defender{}, winner{};
    PlayerId station_owner{}; // space return: the owner whose starbase fought; zero means the defender
    bool land{};
    std::uint32_t station_level{};
    std::vector<EntityId> destroyed;
    std::vector<SurfaceForce> surviving_invaders;
};
// Galactic time (ADR-013): one galactic tick is one FoC logical frame, 1/30 s.
inline constexpr std::uint32_t galactic_frames_per_second = 30;
// WGM-04: the periodic checks run on every galactic frame divisible by 30; a code constant.
inline constexpr std::uint64_t periodic_check_frames = 30;

// The galaxy's rule constants, converted from the campaign's content at load (data-driven, never
// a literal in the simulation). `fiscal_cycle_frames` is `Fiscal_Cycle_Time_In_Secs` in logical
// frames (WGM-02, WGEP-02; fiscal_cycle_frames() below); zero means the galaxy has no fiscal window.
struct GalacticRules {
    std::uint32_t fiscal_cycle_frames{};
    // Opt-in graph rules; absent in the legacy synthetic corpus. WGM-16, authored constant.
    std::optional<math::Fixed> maximum_fleet_movement_distance{};
    friend constexpr bool operator==(const GalacticRules&, const GalacticRules&) noexcept = default;
};

// WGS-11..13/39: represented player startup economy. Future fiscal/production services own
// changes to these balances; campaign entry alone assigns the initial values.
struct GalacticPlayerEconomy {
    PlayerId player{};
    math::Fixed credits;
    math::Fixed credit_cap;
    std::int64_t current_tech{};
    std::int64_t maximum_tech{};
    friend bool operator==(const GalacticPlayerEconomy&, const GalacticPlayerEconomy&) = default;
};
struct PlayerEconomySetup {
    std::vector<GalacticPlayerEconomy> players; // exactly one row per listed player, in player order
    friend bool operator==(const PlayerEconomySetup&, const PlayerEconomySetup&) = default;
};

// Fiscal eligibility and positive-addition factor are explicit, sourced
// inputs (WGEP-03/16); human callers supply one, AI callers supply difficulty data.
// Corruption and the black-market multiplier writer remain separate seams.
struct FiscalPlayer {
    PlayerId player{};
    bool earns_planet_income{};
    math::Fixed positive_credit_multiplier;
    friend bool operator==(const FiscalPlayer&, const FiscalPlayer&) = default;
};
struct PlanetBaseIncome {
    PlanetId planet{};
    math::Fixed credits;
    friend bool operator==(const PlanetBaseIncome&, const PlanetBaseIncome&) = default;
};
// WGEP-06..09/63/75: immutable authored inputs, separate from live ownership.
enum class IncomeAlignment : std::uint8_t { neutral, light, dark };
struct IncomeCurvePoint {
    math::Fixed x, y;
    friend bool operator==(const IncomeCurvePoint&, const IncomeCurvePoint&) = default;
};
struct IncomeFaction {
    PlayerId player{};
    IncomeAlignment alignment{};
    friend bool operator==(const IncomeFaction&, const IncomeFaction&) = default;
};
struct IncomePlanet {
    PlanetId planet{};
    IncomeAlignment alignment{};
    math::Fixed strength{};
    friend bool operator==(const IncomePlanet&, const IncomePlanet&) = default;
};
struct IncomeRoute {
    RouteId route{};
    math::Fixed factor{};
    friend bool operator==(const IncomeRoute&, const IncomeRoute&) = default;
};
struct PlanetIncomeRules {
    math::Fixed maximum_alignment_bonus, maximum_alignment_penalty;
    std::vector<IncomeCurvePoint> political_curve;
    std::vector<IncomeFaction> factions;
    std::vector<IncomePlanet> planets;
    std::vector<IncomeRoute> routes;
    friend bool operator==(const PlanetIncomeRules&, const PlanetIncomeRules&) = default;
};
struct FiscalSetup {
    math::Fixed credit_cap_per_planet;
    std::vector<FiscalPlayer> players; // exactly in setup player order
    std::vector<PlanetBaseIncome> planets; // exactly in setup planet order
    std::optional<PlanetIncomeRules> contributions{}; // optional tail of GC fiscal tag 6
    friend bool operator==(const FiscalSetup&, const FiscalSetup&) = default;
};

enum class ProductionDomain : std::uint8_t { ground = 0, space = 1 };
enum class ProductionKind : std::uint8_t { unit = 0, structure = 1, starbase = 2, technology = 3 };
// Location-adjusted, validated content inputs (WGEP-18..34). Modifier/corruption
// producers own price/time preparation; queues retain these values at admission.
struct ProductionOffer {
    TypeId type{};
    ProductionDomain domain{};
    ProductionKind kind{};
    math::Fixed price, seconds, population;
    std::int64_t tech{}, required_ground_level{}, required_space_level{}, base_level{}, target_tech{};
    std::int64_t planet_limit{-1}, current_limit{-1}, lifetime_limit{-1};
    bool affiliated{}, cinematic{}, initially_locked{}, initially_placed{}, requires_initial_placement{};
    // Named producer/prerequisite seams; unknown content is never admitted by default.
    bool direct_build_allowed{}, surface_exception{}, slicer_unlockable{};
    std::vector<std::vector<TypeId>> required_structures; // AND of OR groups, WGEP-21
    std::vector<TypeId> required_orbiting_units;
    // GHUD-S57: the type's authored Build_Cost_Credits before every multiplier; presentation compares `price` with it.
    math::Fixed authored_price;
    bool sellable{true}; // GHUD-S84: effective authored permission, including hero/superweapon defaults.
    friend bool operator==(const ProductionOffer&, const ProductionOffer&) = default;
};
struct ProductionEntry {
    TypeId type{};
    PlayerId builder{};
    EntityId build_id{};
    math::Fixed price, full_seconds, last_remaining;
    std::uint64_t start_frame{}, duration_frames{};
    bool pay_as_you_go{};
    friend bool operator==(const ProductionEntry&, const ProductionEntry&) = default;
};
struct ProductionObject {
    EntityId id{};
    TypeId type{};
    PlayerId owner{};
    math::Fixed paid_cost;
    ProductionDomain domain{};
    ProductionKind kind{};
    friend bool operator==(const ProductionObject&, const ProductionObject&) = default;
};
struct PlanetProduction {
    PlanetId planet{};
    PlayerId target_owner{}, starbase_owner{}, ground_occupier{}, orbit_occupier{};
    TypeId starbase_type{};
    std::int64_t ground_level{}, space_level{}, maximum_space_level{}, ground_slots{}, space_slots{};
    std::int64_t landed_transports{}, maximum_ground_forces{};
    // Legacy setup-only gate; live WGEP-24/38 checks use fleet_blockers when supplied.
    bool enabled{}, surface_accessible{}, starbase_blocked{};
    std::vector<ProductionOffer> offers; // ascending type ID; planet restrictions already filtered
    std::array<std::vector<ProductionEntry>, 2> queues;
    std::vector<ProductionObject> objects; // ground units/structures; orbital structures
    friend bool operator==(const PlanetProduction&, const PlanetProduction&) = default;
};
struct ProductionCount {
    TypeId type{};
    std::uint64_t owned{}, historical{};
    friend bool operator==(const ProductionCount&, const ProductionCount&) = default;
};
struct ProductionPlayer {
    PlayerId player{};
    bool human{}, pay_as_you_go{}, uses_population_cap{};
    math::Fixed population_capacity, population_used;
    // Census seam: supplied owned/history counts exclude this setup's queues.
    std::vector<ProductionCount> counts;
    std::vector<TypeId> locked, unlocked;
    friend bool operator==(const ProductionPlayer&, const ProductionPlayer&) = default;
};
// WGEP-24/38: explicit owning-service declarations, not inferred from diplomacy
// or member templates. Orbit and owner are read from the live fleet identity.
struct FleetProductionBlocker {
    FleetId fleet{};
    bool pending_deletion{}, skips_space_combat{}, cancels_starbase_production{};
    friend bool operator==(const FleetProductionBlocker&, const FleetProductionBlocker&) = default;
};
// WGS-80 / CGS-03: resolved authored inputs for the shared nested allocator.
// Member order and repetition are significant; these inputs survive reconstruction.
enum class NestedCreationRole : std::uint8_t { other, transport, company, squadron };
// WGM-161..163/169: qualified runtime dispatch and scalar ability owner.
enum class ActorCombatRole : std::uint8_t { unavailable, ordinary, fleet, transport, squadron };
struct ActorAbilityState {
    bool present{}, combat_skip{}, self_target{};
    math::Fixed evade_chance;
    friend bool operator==(const ActorAbilityState&, const ActorAbilityState&) = default;
};
struct NestedCreationType {
    TypeId type{}, transport{};
    NestedCreationRole role{};
    std::vector<TypeId> members;
    bool skips_combat{}, always_spawn_in_orbit{};
    ActorCombatRole combat_role{ActorCombatRole::unavailable};
    ActorAbilityState abilities;
    friend bool operator==(const NestedCreationType&, const NestedCreationType&) = default;
};
struct NestedCreationRelation {
    PlayerId player{}, other{};
    bool enemy{};
    friend bool operator==(const NestedCreationRelation&, const NestedCreationRelation&) = default;
};
struct NestedCreationSetup {
    TypeId fleet_type{};
    std::vector<NestedCreationType> types;
    std::vector<NestedCreationRelation> relations;
    TypeId squadron_type{}; // CRP-01: validated current container, distinct from represented template
    friend bool operator==(const NestedCreationSetup&, const NestedCreationSetup&) = default;
};
// WGEP-147: complete registered types, independent of producer offers or price.
struct TechnologyType {
    TypeId type{};
    std::int64_t tech{};
    bool initially_locked{}, slicer_unlockable{};
    std::vector<PlayerId> affiliated_players; // ascending player ID
    friend bool operator==(const TechnologyType&, const TechnologyType&) = default;
};
struct ProductionSetup {
    std::uint32_t maximum_human_queue{}; // Max_Build_Queue, WGEP-27
    std::vector<ProductionPlayer> players;
    std::vector<PlanetProduction> planets;
    // Absent retains legacy setup flags; present (including empty) selects live checks.
    std::optional<std::vector<FleetProductionBlocker>> fleet_blockers{};
    std::optional<NestedCreationSetup> nested_creation{};
    std::vector<TechnologyType> technology_types{}; // ascending type ID, complete registry
    friend bool operator==(const ProductionSetup&, const ProductionSetup&) = default;
};

// Original strategic types and owned-list metadata (WGEP-48/49/62).
// Existing planet/member/result IDs may carry an override; other rows represent
// owned containers or transports absent from the fleet/result tables.
struct HistoryType {
    math::Fixed power; // resolved galactic AI_Combat_Power, WGP-191
    bool fleet{}, transport{}, squadron{};
    friend bool operator==(const HistoryType&, const HistoryType&) = default;
};
struct CensusType {
    TypeId type{};
    math::Fixed population;
    std::int64_t additional_capacity{};
    // WGEP-78: sorted unique company members/transport and squadron members.
    std::vector<TypeId> members{};
    bool company{}; // WGS-80: original dummy company on its live transport
    TypeId transport{}; // exact authored runtime transport binding, not any member
    // GHUD-S93: current representation, contents and original-type permission differ.
    math::Fixed authored_price{};
    std::uint8_t sale_policy{}; // 0 default, 1 always, 2 never
    bool named_hero{}, superweapon{};
    std::optional<HistoryType> history{};
    friend bool operator==(const CensusType&, const CensusType&) = default;
};
struct CensusObject {
    EntityId id{};
    TypeId original_type{};
    PlayerId owner{};
    PlanetId planet{};
    EntityId container{}; // immediate container only, never the whole ancestor chain
    bool dead{}, delete_pending{};
    TypeId current_type{}; // zero is an unavailable representation, never original-type fallback
    EntityId generated_source{}; // CRP-02: actual source actor; zero for ordinary construction
    ActorCombatRole combat_role{ActorCombatRole::unavailable};
    ActorAbilityState abilities{};
    std::optional<bool> garrison{}; // GAG-01..03: retained owner fact, absence refuses
    math::Fixed paid_cost{}; // GHUD-S94: actual cost survives nested placement and save/restore.
    friend bool operator==(const CensusObject&, const CensusObject&) = default;
};
struct CensusPlayer {
    PlayerId player{};
    std::int64_t base_capacity{};
    bool ai_uses_cap{}; // WGEP-62: omitted XML defaults false
    std::vector<PlanetId> unusable_planets;
    friend bool operator==(const CensusPlayer&, const CensusPlayer&) = default;
};
struct PopulationCensus {
    std::vector<CensusType> types; // ascending original type ID
    std::vector<CensusPlayer> players; // setup player order
    std::vector<CensusObject> objects; // ascending ID
    friend bool operator==(const PopulationCensus&, const PopulationCensus&) = default;
};
struct PopulationTotal {
    PlayerId player{};
    math::Fixed capacity, used, reserved;
    bool uses_cap{};
    friend bool operator==(const PopulationTotal&, const PopulationTotal&) = default;
};

// WGEP-16: positive additions use the supplied factor; debits do not. Clamp after
// adjustment. Arithmetic overflow is a diagnostic, never a wrapped account.
[[nodiscard]] core::Result<math::Fixed> adjusted_credits(math::Fixed balance,
    math::Fixed delta, math::Fixed positive_multiplier, math::Fixed cap);

// WGEP-02: seconds to logical frames, nearest integer with half ties away from zero. Refuses a
// duration that is not positive or does not fit.
[[nodiscard]] core::Result<std::uint32_t> fiscal_cycle_frames(math::Fixed seconds);

// The tick-zero galaxy. Players in strictly increasing nonzero ID; seats in strictly increasing
// nonzero ID, each seated in a listed player with a known kind; planets, routes and fleets
// each in strictly increasing ID; every planet, route, fleet and member ID nonzero and distinct
// from every other. Owners are listed players, route ends and orbits listed planets, and a
// fleet has at least one member. `debug_commands` admits the debug commands (tests and tools
// only; a campaign never sets it).
// Trusted campaign-return inputs, separate from player commands. Payloads belong
// to the campaign outcome owner; the galaxy journals successful boundary commits.
struct OutcomeJournalEntry {
    std::uint64_t completed_tick{};
    EntityId conflict{};
    std::vector<std::uint8_t> payload;
    friend bool operator==(const OutcomeJournalEntry&, const OutcomeJournalEntry&) = default;
};
enum class VictoryKind : std::uint8_t {
    all_planets = 1, no_enemy_planets, credits, control_fraction,
    homeworld_and_leader, kill_leader, named_planets, cycles,
    destroy_superweapon, superweapon_last_planet, superweapon_kill_leader,
    story_forced // WGS-173: distinct from authored ordinary galactic conditions
};
struct VictoryParameters {
    std::int64_t credits{}, cycles{};
    math::Fixed control_fraction{math::Fixed::from_raw(math::Fixed::scale)}; // WGS-78
    std::vector<std::string> factions, leaders, planets;
    friend bool operator==(const VictoryParameters&, const VictoryParameters&) = default;
};
struct GalacticVictoryCondition {
    VictoryKind kind{VictoryKind::all_planets};
    PlayerId player{};
    std::uint32_t delay{30}; // WGS-15..17
    VictoryParameters parameters;
    bool occurred{}, suppress_text{};
    friend bool operator==(const GalacticVictoryCondition&, const GalacticVictoryCondition&) = default;
};
struct VictoryTest {
    VictoryKind kind{VictoryKind::all_planets};
    PlayerId player{};
    friend bool operator==(const VictoryTest&, const VictoryTest&) = default;
};
// Copied provider values keep political, hero and return ownership in their subsystems.
struct VictoryPlayer {
    PlayerId player{};
    std::string faction;
    bool neutral{}, playable{}, human{}, ai{};
    math::Fixed credits{};
    PlayerId primary_enemy{};
    TypeId home{}, leader_company{};
    std::string leader_unit{}; // first authored ground unit of the leader company, WGS-24
    std::string planet_leader_unit{}; // direct faction leader type for landed destruction, WGS-79
    TypeId leader_unit_type{}, planet_leader_type{}; // exact authored bindings, never company/member guesses
    friend bool operator==(const VictoryPlayer&, const VictoryPlayer&) = default;

};
struct VictoryPlanet {
    PlanetId planet{};
    TypeId type{};
    PlayerId current{}, target{}, owner{};
    bool destroyed{};
    friend bool operator==(const VictoryPlanet&, const VictoryPlanet&) = default;

};
struct VictoryLeader {
    PlayerId owner{};
    TypeId company{};
    std::string unit;
    bool scheduled_return{}, dead{}, marked_for_death{}, deletion_pending{};
    PlayerId killer{};
    EntityId object{}; // qualified active unit or defeated return identity
    friend bool operator==(const VictoryLeader&, const VictoryLeader&) = default;

};
struct VictoryType {
    std::string name;
    TypeId type{};
    friend bool operator==(const VictoryType&, const VictoryType&) = default;
};
struct VictoryContext {
    bool multiplayer{}, tutorial_delayed{};
    std::uint64_t fiscal_cycle{};
    std::vector<VictoryPlayer> players;
    std::vector<VictoryPlanet> planets;
    std::vector<ControlRelation> relations;
    std::vector<VictoryType> types;
    std::vector<TypeId> superweapon_killers; // resolved original-type traits
    std::vector<VictoryLeader> leaders; // scheduled returns then active-mode object order
    friend bool operator==(const VictoryContext&, const VictoryContext&) = default;

};

struct VictoryState {
    VictoryContext context; // pinned metadata; live politics/credits are gathered by the session
    std::vector<GalacticVictoryCondition> installed;
    std::optional<GalacticVictoryCondition> pending;
    std::int64_t remaining{};
    std::uint64_t last_service{};
    std::uint32_t waits{};
    bool use_retry{}, retry_waiting{}, result_dispatched{};
    std::vector<VictoryTest> queued;
    friend bool operator==(const VictoryState&, const VictoryState&) = default;
};
// WGEP-10/11/31/54: authored source profiles, independent of tactical streams.
struct PlanetIncomeAbility {
    math::Fixed percentage, absolute, colony_multiplier;
    friend bool operator==(const PlanetIncomeAbility&, const PlanetIncomeAbility&) = default;
};
struct GamblingIncomeAbility {
    math::Fixed chance, minimum, maximum;
    std::uint32_t interval_frames{};
    friend bool operator==(const GamblingIncomeAbility&, const GamblingIncomeAbility&) = default;
};
struct EconomicReduction {
    TypeId type{};
    std::uint32_t category{};
    math::Fixed reduction;
    bool price{}, galaxy_wide{};
    friend bool operator==(const EconomicReduction&, const EconomicReduction&) = default;
};
// WGEP-163: authored credit-halt sabotage profile. The duration is the authored
// seconds value; the live writer converts it once and retains the frames.
struct CreditHaltAbility {
    math::Fixed seconds;
    friend bool operator==(const CreditHaltAbility&, const CreditHaltAbility&) = default;
};
struct EconomicType {
    TypeId type{};
    std::optional<PlanetIncomeAbility> mining;
    std::optional<GamblingIncomeAbility> gambling;
    std::vector<EconomicReduction> reductions;
    std::optional<CreditHaltAbility> credit_halt;
    std::optional<math::Fixed> technology_price_reduction; // WGEP-166..168; planet-scoped source
    friend bool operator==(const EconomicType&, const EconomicType&) = default;
};
struct EconomicPlanet {
    PlanetId planet{};
    bool mining_colony{}, destroyed{}, intimidation{};
    friend bool operator==(const EconomicPlanet&, const EconomicPlanet&) = default;
};
struct EconomicSource {
    EntityId id{};
    TypeId type{};
    PlanetId planet{};
    PlayerId owner{};
    bool planet_source{}, enabled{true};
    friend bool operator==(const EconomicSource&, const EconomicSource&) = default;
};
struct IncomeSetup {
    std::vector<EconomicType> types; // ascending original type
    std::vector<EconomicPlanet> planets; // complete, ascending planet
    // Declared nonplanet identities must be supplied by an owning lifecycle producer.
    // Missing movement/destruction callbacks remain GFS-U05, never guessed polling.
    std::vector<EconomicSource> sources; // ascending stable identity
    struct OfferBasis {
        PlanetId planet{};
        TypeId type{};
        math::Fixed price, seconds; // WGEP-29/32: before reduction and price rounding
        friend bool operator==(const OfferBasis&, const OfferBasis&) = default;
    };
    std::vector<OfferBasis> offers; // ascending (planet, type), required for modifier admission
    friend bool operator==(const IncomeSetup&, const IncomeSetup&) = default;
};
struct GamblingIncomeState {
    EntityId source{};
    PlanetId planet{};
    std::uint64_t deadline{std::numeric_limits<std::uint32_t>::max()};
    math::Fixed income;
    bool scheduled{};
    PlayerId planet_owner{}; // previous target owner for WGEP-77 cleanup
    friend bool operator==(const GamblingIncomeState&, const GamblingIncomeState&) = default;
};
// WGEP-05/163/164: one unexpired Boolean income-blocking modifier. Actual planetary
// income is the logical OR of the rows of its planet; orbital blockade, AI
// hypothetical income and political ownership are separate inputs.
struct IncomeBlocker {
    PlanetId planet{};
    EntityId source{}; // unique ability source, never the planet owner
    std::uint64_t first_frame{}; // GC frame at which the source registered true
    std::uint64_t first_expired_frame{}; // first GC frame at which it no longer holds
    friend bool operator==(const IncomeBlocker&, const IncomeBlocker&) = default;
};
struct IncomeState {
    IncomeSetup inputs;
    std::vector<GamblingIncomeState> gambling; // stable source order
    std::uint64_t random_draws{};
    std::vector<IncomeBlocker> blockers; // ascending (planet, source); only unexpired rows
    friend bool operator==(const IncomeState&, const IncomeState&) = default;
};

// WGEP-04/12..14/29/32/55/58/80/81. Authored inputs and retained live facts.
// Corruption transition, actor lifecycle and ability callbacks are owning-host seams.
struct TechnologyPriceModifier {
    EntityId source{};
    math::Fixed additive;
    // Owning provider supplies the first expired GC frame, not a guessed duration.
    std::optional<std::uint64_t> first_expired_frame;
    friend bool operator==(const TechnologyPriceModifier&, const TechnologyPriceModifier&) = default;
};
struct CorruptionEconomyPlayer {
    PlayerId player{};
    std::uint32_t faction{};
    bool benefits{}, playable{}, human{};
    std::array<math::Fixed, 2> price{corruption_fixture::one, corruption_fixture::one};
    std::array<math::Fixed, 2> time{corruption_fixture::one, corruption_fixture::one};
    math::Fixed tech_purchase_multiplier{corruption_fixture::one};
    std::vector<TechnologyPriceModifier> technology_price_modifiers; // ascending source; WGEP-30/110
    std::int64_t displayed_tech_adjustment{}; // WGEP-99: add to current internal GC tech, no clamp
    std::set<std::uint32_t> unlocks;
    friend bool operator==(const CorruptionEconomyPlayer&, const CorruptionEconomyPlayer&) = default;
};
struct CorruptionEconomyPlanet {
    PlanetId planet{};
    corruption_fixture::Type corruption{};
    bool influenced{}, destroyed{};
    math::Fixed factor{corruption_fixture::one}, piracy{}, market_modifier{};
    corruption_fixture::Siphon siphon{};
    friend bool operator==(const CorruptionEconomyPlanet&, const CorruptionEconomyPlanet&) = default;
};
struct BlackMarketBuyer {
    EntityId buyer{};
    PlayerId owner{};
    bool present{true}, opposing_planet{}, causes_despawn{};
    math::Fixed price_modifier{corruption_fixture::one};
    std::vector<math::Fixed> absence_min, absence_max;
    // WGCH-70: committed handoff to the owning hero absence/destruction route.
    bool despawn_requested{};
    math::Fixed prepared_absence;
    friend bool operator==(const BlackMarketBuyer&, const BlackMarketBuyer&) = default;
};
struct BlackMarketMenu {
    EntityId buyer{};
    PlanetId planet{};
    std::vector<corruption_fixture::Offer> offers;
    math::Fixed absence;
    bool active{};
    friend bool operator==(const BlackMarketMenu&, const BlackMarketMenu&) = default;
};
struct EconomyCorruptionState {
    std::array<math::Fixed, 9> income_factors{}; // none + authored order
    math::Fixed factor_min, factor_max;
    bool tutorial{}, multiplayer{};
    std::vector<CorruptionEconomyPlayer> players;
    std::vector<CorruptionEconomyPlanet> planets;
    std::vector<corruption_fixture::MarketItem> items; // authored order, not sorted
    std::vector<BlackMarketBuyer> buyers; // stable identity order
    std::vector<BlackMarketMenu> menus; // stable buyer order
    std::uint64_t random_draws{}; // dedicated synchronized stream position
    friend bool operator==(const EconomyCorruptionState&, const EconomyCorruptionState&) = default;
};

// GFS-C-01/WGCH-70: live content is immutable; actors remain in the canonical census.
enum class AbilityKind : std::uint8_t { black_market = 1, technology_theft = 2, corruption = 3,
    superweapon_hack = 4, planet_destruction = 5, structure_sabotage = 6, hero_neutralization = 7 }; // 4..7: coordinator-reserved host-local values
struct CorruptionChoice {
    corruption_fixture::Type type{}, required_mission_type{};
    math::Fixed price, credit_reward, unlock_build_cost;
    std::uint32_t unit_count{}, mission_count{};
    std::vector<TypeId> unit_types, planet_types;
    std::string briefing, map, script;
    std::uint8_t mode{}; // 0 none, 1 land, 2 space
    TypeId tactical_upgrade{}, build_unlock{};
    friend bool operator==(const CorruptionChoice&, const CorruptionChoice&) = default;
};
struct CorruptionAbilityProfile {
    std::string name;
    bool initially_enabled{true};
    friend bool operator==(const CorruptionAbilityProfile&, const CorruptionAbilityProfile&) = default;
};
// WGCH-57/139: a named activated hack ability; names route through the authored activated list.
struct HackAbilityProfile {
    std::string name;
    bool initially_enabled{true};
    friend bool operator==(const HackAbilityProfile&, const HackAbilityProfile&) = default;
};
// WGCH-137/157: a named activated destruction ability; the authored activation style still selects the group.
struct DestructionAbilityProfile {
    std::string name;
    bool initially_enabled{true};
    friend bool operator==(const DestructionAbilityProfile&, const DestructionAbilityProfile&) = default;
};
struct CorruptionPlanet {
    PlanetId planet{};
    TypeId type{};
    bool retains_influence{};
    std::array<CorruptionChoice, 3> slots;
    friend bool operator==(const CorruptionPlanet&, const CorruptionPlanet&) = default;
};
struct CorruptionPlayer {
    PlayerId player{};
    std::array<TypeId, 9> automatic_upgrades{};
    friend bool operator==(const CorruptionPlayer&, const CorruptionPlayer&) = default;
};
struct CorruptionPrerequisiteType {
    TypeId type{};
    bool consumable{}, named{};
    friend bool operator==(const CorruptionPrerequisiteType&, const CorruptionPrerequisiteType&) = default;
};
struct InitialCorruption {
    PlanetId planet{};
    corruption_fixture::Type type{};
    PlayerId recipient{};
    friend bool operator==(const InitialCorruption&, const InitialCorruption&) = default;
};
struct PreparedCorruption {
    corruption_fixture::Type type{};
    std::uint8_t slot{};
    friend bool operator==(const PreparedCorruption&, const PreparedCorruption&) = default;
};
struct LiveCorruptionTransition {
    PlanetId planet{};
    corruption_fixture::Transition transition;
    friend bool operator==(const LiveCorruptionTransition&, const LiveCorruptionTransition&) = default;
};
enum class CorruptionMissionState : std::uint8_t { queued, succeeded, failed, cancelled };
enum class CorruptionCompletionPhase : std::uint8_t { state_committed, automatic_upgrades, rewards };
struct LiveCorruptionMission {
    std::uint64_t request{};
    EntityId actor{};
    PlanetId planet{};
    PlayerId player{};
    PreparedCorruption choice;
    CorruptionMissionState state{};
    friend bool operator==(const LiveCorruptionMission&, const LiveCorruptionMission&) = default;
};
struct CorruptionUsage {
    PlayerId player{};
    std::array<std::uint32_t, 9> uses{};
    friend bool operator==(const CorruptionUsage&, const CorruptionUsage&) = default;
};
struct PlanetAbilityEntitlements {
    PlanetId planet{};
    PlayerId player{};
    std::vector<TypeId> automatic_upgrades, tactical_upgrades, build_unlocks;
    friend bool operator==(const PlanetAbilityEntitlements&, const PlanetAbilityEntitlements&) = default;
};
using AbilityCurvePoint = IncomeCurvePoint;
// WGCH-27/28/97: the authored sabotage ability; the escalation factor defaults to two when absent (WGCH-28).
struct SabotageAbilityProfile {
    std::string name;
    bool initially_enabled{true};
    math::Fixed cost_factor{math::Fixed::from_integer(2).value()};
    friend bool operator==(const SabotageAbilityProfile&, const SabotageAbilityProfile&) = default;
};
// WGCH-20..24: the authored neutralization ability (curves, cost modifiers, absence ranges, permissions).
struct NeutralizeAbilityProfile {
    std::string name;
    bool initially_enabled{true};
    std::vector<AbilityCurvePoint> base_curve, history_curve, corruption_curve;
    math::Fixed minor_modifier{math::Fixed::from_integer(1).value()}, major_modifier{math::Fixed::from_integer(1).value()};
    math::Fixed actor_absence_min, actor_absence_max, target_absence_min, target_absence_max;
    bool can_minor{}, can_major{};
    friend bool operator==(const NeutralizeAbilityProfile&, const NeutralizeAbilityProfile&) = default;
};
// WGCH-27/97: authored per-structure-type sabotage prices (type order); below one is not a target.
struct SabotageStructureCost {
    TypeId type{};
    math::Fixed cost;
    friend bool operator==(const SabotageStructureCost&, const SabotageStructureCost&) = default;
};
// WGCH-20/22/52: authored per-hero-type neutralization cost and permissions (type order).
struct NeutralizationTarget {
    TypeId type{};
    math::Fixed cost;
    bool major_allowed{true}, minor_allowed{true};
    friend bool operator==(const NeutralizationTarget&, const NeutralizationTarget&) = default;
};
struct TheftAbilityProfile {
    std::vector<AbilityCurvePoint> availability, ground, space, difficulty;
    math::Fixed affinity_price, affinity_chance;
    std::vector<math::Fixed> absence_min, absence_max;
    bool initially_enabled{};
    friend bool operator==(const TheftAbilityProfile&, const TheftAbilityProfile&) = default;
};
struct TheftTechnology {
    TypeId type{};
    math::Fixed price;
    bool space{};
    friend bool operator==(const TheftTechnology&, const TheftTechnology&) = default;
};
struct TheftPlanet {
    PlanetId planet{};
    math::Fixed availability, difficulty;
    std::vector<TypeId> affinity_types;
    std::optional<bool> auto_capture_minor;
    friend bool operator==(const TheftPlanet&, const TheftPlanet&) = default;
};
// WGCH-143: authored destroyed base income, read when the control owner marks the planet ruined.
struct RuinedPlanetIncome {
    PlanetId planet{};
    math::Fixed credits;
    friend bool operator==(const RuinedPlanetIncome&, const RuinedPlanetIncome&) = default;
};
struct PreparedTheft {
    TypeId technology{};
    math::Fixed price, chance, absence;
    bool advance{};
    friend bool operator==(const PreparedTheft&, const PreparedTheft&) = default;
};
struct GalacticAbilityProfile {
    TypeId type{};
    BlackMarketBuyer market;
    bool named{}, respawn_whole_team{};
    bool initially_enabled{true};
    TypeId respawn_structure{};
    bool has_market{true};
    std::optional<TheftAbilityProfile> theft;
    std::optional<CorruptionAbilityProfile> corruption{};
    bool lifecycle_only{}; // named prerequisite scheduling metadata, no activation
    std::optional<HackAbilityProfile> hack{};
    std::optional<DestructionAbilityProfile> destruction{};
    std::optional<SabotageAbilityProfile> sabotage{};
    std::optional<NeutralizeAbilityProfile> neutralize{};
    std::vector<AbilityKind> ground_activated{}; // WGCH-157: authored Activation_Style membership
    friend bool operator==(const GalacticAbilityProfile&, const GalacticAbilityProfile&) = default;
};
struct GalacticAbilitySetup {
    math::Fixed default_hero_absence;
    std::vector<GalacticAbilityProfile> profiles; // current type identity order
    std::vector<TheftTechnology> technologies; // original registration order
    std::vector<TheftPlanet> theft_planets; // planet identity order
    std::vector<std::string> activated_corruption_abilities{}; // authored routing order
    std::vector<CorruptionPlanet> corruption_planets{};
    std::vector<CorruptionPlayer> corruption_players{};
    std::vector<CorruptionPrerequisiteType> corruption_prerequisites{};
    std::vector<InitialCorruption> initial_corruption{}; // ordered initialization then authored overrides
    std::vector<RuinedPlanetIncome> ruined_income{}; // planet identity order, WGCH-143/176
    // WGCH-109/110/139: authored hack constants; an absent required type never matches, an absent cost is zero (unverified).
    TypeId hack_required_type{};
    math::Fixed hack_cost;
    std::vector<std::string> activated_hack_abilities{}; // authored routing order
    std::vector<std::string> activated_destruction_abilities{}; // WGCH-137/157 authored routing order
    std::vector<std::string> activated_sabotage_abilities{}, activated_neutralize_abilities{}; // WGCH-97 authored routing order
    std::vector<SabotageStructureCost> sabotage_costs{}; // ascending type
    std::vector<NeutralizationTarget> neutralization_targets{}; // ascending type
    bool multiplayer{}, fast_corruption{}, tutorial_corruption_allowed{true};
    friend bool operator==(const GalacticAbilitySetup&, const GalacticAbilitySetup&) = default;
};
struct PreparedAbility {
    std::uint64_t preparation{};
    EntityId actor{};
    PlanetId planet{};
    AbilityKind kind{AbilityKind::black_market};
    std::uint32_t choice{}, item{};
    math::Fixed price, absence;
    bool owned{};
    PreparedTheft theft;
    PreparedCorruption corruption{};
    // WGCH-96/92: sabotage structure identity, or neutralization victim, company type and prepared target absence.
    EntityId target{};
    TypeId company{};
    math::Fixed target_absence;
    bool valid{true};
    friend bool operator==(const PreparedAbility&, const PreparedAbility&) = default;
};
enum class AbilityOutcome : std::uint8_t { purchased = 1, refused, cancelled };
struct AbilityResult {
    std::uint64_t request{}, preparation{};
    EntityId actor{};
    PlanetId planet{};
    PlayerId player{};
    std::uint32_t item{};
    AbilityOutcome outcome{};
    AbilityKind kind{AbilityKind::black_market};
    TypeId technology{};
    friend bool operator==(const AbilityResult&, const AbilityResult&) = default;
};
struct PreparedTarget {
    EntityId target{};
    TypeId company{};
    math::Fixed price, absence, target_absence;
    bool valid{true};
    friend bool operator==(const PreparedTarget&, const PreparedTarget&) = default;
};
struct AbilityPreparation {
    std::uint64_t id{};
    EntityId actor{};
    PlanetId planet{};
    std::vector<corruption_fixture::Offer> offers;
    math::Fixed absence;
    bool open{};
    AbilityKind kind{AbilityKind::black_market};
    std::vector<PreparedTheft> theft_choices;
    std::vector<PreparedCorruption> corruption_choices{};
    std::vector<PreparedTarget> target_choices{}; // sabotage structures or neutralization victims, prepared order
    friend bool operator==(const AbilityPreparation&, const AbilityPreparation&) = default;
};
struct AbilityActorState {
    EntityId actor{};
    TypeId type{};
    bool enabled{}, cancelled{};
    bool theft_enabled{}, theft_cancelled{};
    bool corruption_enabled{}, corruption_cancelled{};
    bool hack_enabled{}, hack_cancelled{};
    bool destruction_enabled{}, destruction_cancelled{};
    bool sabotage_enabled{}, sabotage_cancelled{}, neutralize_enabled{}, neutralize_cancelled{};
    bool contested_check_disabled{}; // WGCH-58/160: false (the default) keeps the contested-orbit refusal armed
    friend bool operator==(const AbilityActorState&, const AbilityActorState&) = default;
};
struct SabotageHistory {
    PlanetId planet{};
    std::uint32_t count{};
    friend bool operator==(const SabotageHistory&, const SabotageHistory&) = default;
};
// WGCH-23/94: keyed by victim company type and the attacking player.
struct NeutralizationHistory {
    TypeId company{};
    PlayerId attacker{};
    std::uint32_t count{};
    friend bool operator==(const NeutralizationHistory&, const NeutralizationHistory&) = default;
};
struct GalacticAbilityState {
    std::uint64_t next_request{1};
    std::vector<AbilityActorState> actors; // live identity order; no idle census scan
    // One retained preparation/result per actor. A replaced preparation explicitly expires.
    std::vector<AbilityPreparation> preparations;
    std::vector<AbilityResult> results;
    std::vector<LiveCorruptionTransition> transitions{};
    std::vector<LiveCorruptionMission> missions{}; // request order; bounded retained receipts
    std::vector<CorruptionUsage> corruption_usage{};
    std::vector<PlanetAbilityEntitlements> planet_entitlements{}; // planet, then player
    std::vector<SabotageHistory> sabotage_history{}; // planet order, WGCH-28/30
    std::vector<NeutralizationHistory> neutralization_history{}; // (company, attacker) order, WGCH-94
    friend bool operator==(const GalacticAbilityState&, const GalacticAbilityState&) = default;
};

struct GalacticSetup {
    std::uint64_t seed{};
    // WGP-81/89: recorded opt-in for a solo presentation observer (zero: headless).
    SeatId interactive_seat{};
    std::array<std::uint8_t, 32> content_identity{};
    std::array<std::uint8_t, 32> campaign_identity{};
    std::vector<GalacticPlayer> players;
    std::vector<Seat> seats;
    std::vector<Planet> planets;
    std::vector<Route> routes;
    std::vector<Fleet> fleets;
    GalacticRules rules;
    bool debug_commands{};
    std::optional<FleetControlSetup> fleet_control{}; // opt-in GC extension tag 2
    std::optional<FiscalSetup> fiscal{}; // coordinator-reserved opt-in GC tag 6
    std::optional<PlayerEconomySetup> player_economy{}; // coordinator-reserved opt-in GC tag 3
    std::optional<ProductionSetup> production{}; // coordinator-reserved opt-in GC tag 7
    std::optional<SeatControlSetup> seat_control{}; // coordinator-reserved GC extension tag 5
    std::optional<MovementSetup> movement{}; // coordinator-reserved GC extension tag 4
    std::optional<PopulationCensus> census{}; // coordinator-reserved opt-in GC tag 10
    std::vector<OutcomeJournalEntry> outcomes{}; // coordinator-reserved replay tag 11
    std::optional<PoliticalControlSetup> control{}; // coordinator-reserved GC extension tag 8
    std::optional<ObserverSetup> observer{}; // coordinator-reserved GC extension tag 12
    std::optional<IncomeSetup> income;
    std::optional<EconomyCorruptionState> economy_corruption; // GC tag 15 / section 17
    std::optional<GalacticAbilitySetup> ability_host; // coordinator-reserved GC tag 17 / section 20
    std::optional<VictoryState> victory{}; // coordinator-reserved tag 14
    friend bool operator==(const GalacticSetup&, const GalacticSetup&) = default;
};

// Galactic session ticks are bounded so a replay's tick count always fits its limits: about 38
// days of galactic time at 30 frames a second.
inline constexpr std::uint64_t galactic_max_ticks = 100'000'000;
inline constexpr std::size_t galactic_max_objects = 1'000'000;
inline constexpr std::size_t galactic_max_players = 64;
inline constexpr std::size_t galactic_max_seats = 256;
// Galactic admission has its own human-seat limit within the shared match capacity.
inline constexpr std::size_t galactic_max_human_seats = 8;
static_assert(galactic_max_human_seats <= sim::max_match_seats);
inline constexpr std::uint64_t galactic_max_commands = 10'000'000;

// Rejects, with a named EAWR-SIM-05xx diagnostic, any setup that breaks the rules above.
[[nodiscard]] core::Result<void> validate_setup(const GalacticSetup& setup);

// Galactic diagnostics (docs/galactic-architecture.md section 1): block EAWR-SIM-05xx.
namespace diagnostic_codes {
inline constexpr std::string_view invalid_setup = "EAWR-SIM-0501";
inline constexpr std::string_view invalid_command = "EAWR-SIM-0502";
inline constexpr std::string_view late_command = "EAWR-SIM-0503";
inline constexpr std::string_view command_order = "EAWR-SIM-0504";
inline constexpr std::string_view unknown_issuer = "EAWR-SIM-0505";
inline constexpr std::string_view resource_limit = "EAWR-SIM-0506";
inline constexpr std::string_view simulation_complete = "EAWR-SIM-0507";
inline constexpr std::string_view worker_failure = "EAWR-SIM-0508";
// The galactic replay (EAWRGCR, docs/galactic-replay-format.md).
inline constexpr std::string_view replay_malformed = "EAWR-SIM-0510";
inline constexpr std::string_view replay_version = "EAWR-SIM-0511";
inline constexpr std::string_view replay_resource_limit = "EAWR-SIM-0512";
inline constexpr std::string_view replay_order = "EAWR-SIM-0513";
inline constexpr std::string_view replay_command = "EAWR-SIM-0514";
} // namespace diagnostic_codes

} // namespace eawr::sim::galactic
