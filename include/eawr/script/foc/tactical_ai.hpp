#pragma once

// The original FoC tactical space AI hosted in a tactical session (#79,
// docs/behaviour/foc-tactical-ai.md "#79 host"). Each AI player runs its
// retail space freestore script (BusyTacticalFreeStore) in the authoritative
// Lua runtime, driven as the FoC engine drives it: Base_Definitions, a `main`
// thread pumped every ServiceRate seconds, On_Unit_Service for each freestore
// unit every UnitServiceRate seconds. Its engine calls read an immutable view
// of the completed tick; its orders become the next tick's replay commands
// (ScriptedTacticalSession), so scripts never touch the world.
//
// With the map bounds and the AI XML (#449) the goal system runs the retail space plans
// beside the freestore (docs/behaviour/foc-tactical-ai.md "Goal system"); what the plans call
// that is not hosted is listed by unsupported_plan_calls().

#include "eawr/core/result.hpp"
#include "eawr/script/foc/ai_diagnostics.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/script/authoritative/tactical_bridge.hpp"
#include "eawr/script/authoritative/tactical_scoring.hpp"
#include "eawr/script/numeric/lua_number.hpp"
#include "eawr/sim/tactical/ai_level.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <cstdint>
#include <functional>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::script::foc {
// FC-76: synchronized scoring seconds use single-precision integer conversion
// and division, then widen exactly for Lua. Distinct from the mode clock.
[[nodiscard]] numeric::LuaNumber scoring_seconds(std::uint64_t frame) noexcept;

// Host handle kinds (Handle::kind) of the FoC wrappers.
inline constexpr std::uint32_t handle_game_object = 1; // id: entity ID
inline constexpr std::uint32_t handle_player = 2;      // id: player ID
inline constexpr std::uint32_t handle_type = 3;        // id: type ID (object type CRC)
inline constexpr std::uint32_t handle_type_list = 4;   // id: WeightedTypeList, setup only
inline constexpr std::uint32_t handle_position = 9;    // id: binary32 bits of planar x (high word) and y (low word)
inline constexpr std::uint64_t first_object_instance = std::uint64_t{1} << 36;

// Script commands the bindings issue (ScriptCommand::verb), translated to tactical orders.
inline constexpr std::string_view verb_move = "foc.move";     // issuer, unit, x, y, z
inline constexpr std::string_view verb_attack = "foc.attack"; // issuer, unit, target
// #452 orders (space-orders OR-12, OR-14): issuer, unit, then a target object or x, y, z.
inline constexpr std::string_view verb_attack_move = "foc.attack_move";
inline constexpr std::string_view verb_guard = "foc.guard";
inline constexpr std::string_view verb_buy = "foc.buy";
inline constexpr std::string_view verb_pad_build = "foc.pad_build";
inline constexpr std::string_view verb_credit_grant = "foc.credit_grant";
inline constexpr std::string_view verb_reservation_debit = "foc.reservation_debit";
inline constexpr std::string_view verb_reinforce = "foc.reinforce";
inline constexpr std::string_view verb_ability = "foc.ability"; // issuer, unit, AbilityKind, AbilityAction (#76)
inline constexpr std::string_view verb_reveal_all = "foc.reveal_all"; // issuer, player (V-20)
inline constexpr std::string_view verb_special_fire = "foc.special_fire"; // issuer, source, target, admission (WSW-40/41)
inline constexpr std::string_view verb_campaign_retreat = "foc.campaign_retreat"; // issuer, owning campaign translates

// Event producer of the engine side (EventKey::producer).
inline constexpr std::uint32_t producer_foc_engine = authoritative::first_simulation_producer;

// A weapon hardpoint of a type as the AI threat grid reads it (#449, PG-02).
struct AiWeapon {
    std::uint32_t hardpoint{};           // HardPoints index
    numeric::LuaNumber combat_power{};   // its share of the type's AI_Combat_Power
    numeric::LuaNumber range{};          // Fire_Range_Distance
    bool targetable{};
    bool destroyable{};
};

// A game object type as the AI reads it (from the #65 unit tables).
struct AiType {
    sim::tactical::TypeId type_id{};
    std::string name;                 // XML name, upper case (Get_Name)
    bool decoration{};
    std::string lua_script; // SOS-01: resolved object attachment; independent of type metadata
    // L-32: scaled collidable model bounds, for the wrapper's box query.
    std::optional<std::array<sim::math::Vec3, 2>> proximity_bounds;
    std::uint64_t category_bits{};    // CategoryMask in GameObjectCategoryType bits
    std::uint64_t property_bits{};    // Property_Flags in GameObjectPropertiesType bits
    std::vector<std::string> abilities; // upper-case ability types
    std::uint32_t projectile_types{}; // Projectile_Types entries the tables keep
    // WTA-34: the projectile types the unit fires, for Get_All_Projectile_Types: each hardpoint's
    // Fire_Projectile_Type in HardPoints order (0: none), then the object weapon's.
    std::vector<sim::tactical::TypeId> hardpoint_projectiles;
    sim::tactical::TypeId object_projectile{};
    // AI-32/WLD-10: the type's Lua_Script is ObjectScript_PointDefense, which the engine stands in for.
    bool point_defence_script{};
    bool corrupt_systems_script{};    // WHE-67: ObjectScript_IG2000 proximity/area activation
    bool locomotor{};                 // BEHAVIOR_LOCO: it has a motion profile or is a squadron
    bool star_base{};                 // BEHAVIOR_DUMMY_STAR_BASE
    bool space_special_weapon{};     // WSW-47: existing planetary source, available for attachment
    // Goal system and perception inputs (#449).
    bool space_evaluator{};           // Has_Space_Evaluator: a space goal target (GS-10)
    std::uint32_t tech_level{};       // Tech_Level
    std::uint32_t base_level{};       // SAE-02: Base_Level
    numeric::LuaNumber combat_power{};        // AI_Combat_Power (0 when absent)
    numeric::LuaNumber max_attack_distance{}; // Targeting_Max_Attack_Distance
    numeric::LuaNumber min_attack_distance{}; // WTA-36: Targeting_Min_Attack_Distance (0 when absent)
    // WTA-36: Is_Hero. The type's first company unit, else its first squadron unit, else the type
    // itself, is a named hero or a special weapon in space.
    bool hero{};
    numeric::LuaNumber max_speed{};           // Max_Speed
    numeric::LuaNumber soft_radius{};         // Custom_Soft_Footprint_Radius (0 when absent)
    numeric::LuaNumber reveal_range{};        // Space_FOW_Reveal_Range (0 when absent)
    // WTA-28: the half extents of the type's bounds in the plane (project choice: the collision
    // bounds times Scale_Factor, the placement box of PL-02; 0 without one).
    numeric::LuaNumber bounds_half_x{};
    numeric::LuaNumber bounds_half_y{};
    std::vector<AiWeapon> weapons;            // weapon hardpoints, HardPoints order
    std::map<std::uint32_t, numeric::LuaNumber> weapon_damage; // weapon slot -> projectile Damage (DT-02)
    bool squadron{};                          // a squadron company type
    bool targeting{};                         // EX-31: effective TARGETING behavior
    bool craft{};                             // a squadron member type
    std::uint32_t squadron_units{};           // its Squadron_Units count
    sim::tactical::TypeId squadron_unit{};    // its first craft type
    bool capture_point{};                    // CAPTURE_POINT ownership behavior
    bool build_pad{};                        // GS-11: type identity, independent of enabled economy
    bool initial_state_visible_under_fow{};  // WNO-13: authored object-goal fog exception
    bool last_state_visible_under_fow{};
    std::optional<sim::math::Fixed> tactical_cost; // WAS-26: generic cost, distinct from selected effective price
    bool unit_ai{}; // PG-03: authored UNIT_AI behavior admits this type to combat perception
    std::vector<std::uint32_t> previous_base_levels{}; // SAE-02: authored predecessor levels, nearest first
};

// What the defensive abilities read of a projectile type (WTA-34): its category.
inline constexpr std::uint32_t ai_projectile_missile = 1U;
inline constexpr std::uint32_t ai_projectile_rocket = 2U;
inline constexpr std::uint32_t ai_projectile_mptl_rocket = 4U;
struct AiContent {
    std::vector<AiType> types; // the type table in load order: a category team enumerates it in this order (PL-20)
    std::map<sim::tactical::TypeId, std::uint32_t> projectile_categories; // projectile object type -> ai_projectile_* bits
    std::map<std::string, std::uint64_t, std::less<>> categories; // upper-case name -> bit
    std::map<std::string, std::uint64_t, std::less<>> properties; // upper-case name -> bit
};

// WTA-45..47: the multipliers one AI player's difficulty adjustment sets. 1 (and sleep 0) is Normal.
// Plain numbers, no engine or presentation types: the AI service, the production rules and the
// unit and damage setup all read the same resolved value.
struct AiDifficultyAdjustment {
    numeric::LuaNumber health{1};            // Health_Multiplier
    numeric::LuaNumber shield{1};            // Shield_Multiplier
    numeric::LuaNumber damage{1};            // Damage_Multiplier
    numeric::LuaNumber space_build_time{1};  // Space_Build_Time_Multiplier
    numeric::LuaNumber credit{1};            // Credit_Multiplier
    numeric::LuaNumber space_contrast{1};    // Space_AI_Contrast_Multiplier
    numeric::LuaNumber space_goal_cycle_sleep{0}; // Space_AI_Goal_Cycle_Sleep_Duration, seconds
    numeric::LuaNumber land_build_time{1};   // Land_Build_Time_Multiplier (#3780)
    numeric::LuaNumber land_contrast{1};     // Land_AI_Contrast_Multiplier
    numeric::LuaNumber land_goal_cycle_sleep{0}; // Land_AI_Goal_Cycle_Sleep_Duration, seconds
};

// The AI player types' Difficulty_Adjustments (level -> entry name) and the difficulty entries of
// difficultyadjustments.xml, read from the same frozen mounted XML as AiSetup::xml. resolve() is the
// one place a tactical player's level becomes its adjustment: player type -> level -> entry. A level
// the type authors no entry for, or an unknown type, gets the engine's dummy adjustment: every
// multiplier 1 (not the Normal entry).
class AiDifficultyTable final {
public:
    [[nodiscard]] static core::Result<AiDifficultyTable> load(
        const std::map<std::string, std::string, std::less<>>& xml);
    // `player_type` is an AIPlayerType name, any case.
    [[nodiscard]] AiDifficultyAdjustment resolve(std::string_view player_type, sim::tactical::AiLevel level) const;

private:
    struct Data;
    std::shared_ptr<const Data> data_;
};

struct AiPlayer {
    sim::tactical::PlayerId player{};
    std::string faction; // Get_Faction_Name, upper case
    bool neutral{};      // the faction's neutral flag (FH-20)
    bool ai{};           // runs the freestore
    std::string player_type; // AIPlayerType name (AI-11), e.g. BasicEmpire; empty: no goal system
    bool human{}; // active human lobby player; non-playable faction players are not humans
    sim::tactical::AiLevel difficulty{}; // WTA-47: a lobby AI's level (#735)
};

// The tactical map's extent as the AI's perception grid covers it (#449, PG-01).
struct AiBounds {
    numeric::LuaNumber left{};
    numeric::LuaNumber top{};
    numeric::LuaNumber right{};
    numeric::LuaNumber bottom{};
};

// The GC tactical perception inputs of the M2 fixture (m2-skirmish.md SK-41 to SK-45).
struct AiPerception {
    bool campaign_game{true}; // Game.IsCampaignGame (SK-41)
    bool defender{false};     // Variable_Self.IsDefender (SK-43)
    int base_level{1};        // Variable_Self.BaseLevel (SK-44)
    std::optional<sim::tactical::PlayerId> campaign_defender;
};

// What the goal system did (#449): a line per plan event (the eye check's per-plan timeline and
// the headless plan test) and the Lua cost of every tick. Written serially at the tick barrier.
struct PlanEvent {
    std::uint64_t tick{};
    sim::tactical::PlayerId player{};
    std::string plan;   // script name, e.g. flankplan
    std::string goal;   // goal type, upper case
    std::string target; // OBJECT_<id> or CELL_<x>_<y>
    std::string event;  // started, produce, produced, order, event, finished, failed
    std::string detail;
};

struct AiTickCost {
    std::uint64_t tick{};
    std::int64_t freestore_instructions{};
    std::int64_t plan_instructions{};
    std::uint32_t plan_instances{};
    // #957: the tick's scheduled AI work, counted (deterministic, unlike wall-clock time).
    std::uint32_t goals_evaluated{}; // (goal function, target) pairs the proposal pass scored
    std::uint32_t maintenances{};    // goal set maintenance passes run (all players)
    std::uint32_t plans_attached{};  // plan script instances created
    std::uint32_t plans_deferred{};  // plans waiting for a later tick's attach budget at the tick's end
    std::uint32_t plans_pumped{};    // plan instances pumped by the planning service
    std::uint32_t freestore_runs{};  // freestore script instances that ran (all players)
    std::uint32_t reinforcement_candidates{};
    std::uint64_t reinforcement_search_ns{}; // wall-clock diagnostics only; never hashed
    std::uint32_t object_instances{}; // SOS-42: active attachments, separate from AI plans
    std::uint32_t object_turn_due_ticks{}, object_turn_rows{}; // diagnostic work, never hashed
    std::uint32_t object_pumps{};     // SOS-42: strictly due object services
};

// #957 (owner decision on #896): how the AI's scheduled plan, goal and freestore work is spread
// over ticks. `faithful` is FoC's own schedule: every system of every AI player starts on frame 0,
// so the players' services coincide, and a goal pass attaches all its plans on the tick it ends.
// `staggered` (the project's rule, docs/behaviour/foc-tactical-ai.md SCH-01 to SCH-05) gives each
// AI player a phase of its position among the AI players and attaches at most `attach_per_tick`
// plan scripts a tick, in order, carrying the rest to the next ticks.
struct AiSchedule {
    enum class Mode : std::uint8_t { faithful, staggered };
    Mode mode{Mode::staggered};
    std::uint32_t attach_per_tick{1};
};

// #957 (SCH-02): which of a player's goal, planning and execution services ran at a frame, one row
// per player and frame with any of them, so a test sees the players' service phases directly.
struct AiServiceEvent {
    std::uint64_t frame{};
    sim::tactical::PlayerId player{};
    bool goals{};
    bool planning{};
    bool execution{};
};

struct AiJournal {
    std::vector<PlanEvent> plans;
    std::vector<AiTickCost> costs;
    std::vector<AiServiceEvent> services;
    // Lifetime overflow counts; diagnostic clearing/restoring does not save them.
    std::uint64_t plans_dropped{};
    std::uint64_t costs_dropped{};
    std::uint64_t services_dropped{};
    void append_plan(PlanEvent row) { append_ai_diagnostic(plans, plans_dropped, std::move(row)); }
    void append_cost(AiTickCost row) { append_ai_diagnostic(costs, costs_dropped, std::move(row)); }
    void append_service(AiServiceEvent row) { append_ai_diagnostic(services, services_dropped, std::move(row)); }
};

struct AiSetup {
    // #3780: selects the goal mode, plan directory, templates and freestore tag; space is the default.
    sim::tactical::BattleDomain domain{sim::tactical::BattleDomain::space};
    AiContent content;
    std::vector<AiPlayer> players; // every session player
    AiPerception perception;
    std::uint64_t seed{};
    AiSchedule schedule;
    bool allow_special_weapons{true}; // WSW-47: frozen match admission for source proposals
    // The freestore script (AIPlayerType SpaceFreeStore, SK-40).
    std::string freestore_module = "Data/Scripts/FreeStore/BusyTacticalFreeStore.lua";
    // The goal system (#449): the map extent, and the AI XML by logical path (required_xml()).
    // Without bounds or XML only the freestore runs, as in #79.
    std::optional<AiBounds> bounds;
    std::map<std::string, std::string, std::less<>> xml;
    // Where the goal system writes its journal; none when null.
    std::shared_ptr<AiJournal> journal;
    // WGT-33: the return/retreat owner supplies eligibility from this completed
    // snapshot and constructs the ordinary admitted retreat order. Both required.
    std::function<bool(const sim::tactical::TacticalSnapshot&, sim::tactical::PlayerId)> campaign_can_retreat;
    std::function<core::Result<authoritative::TacticalOrder>(sim::tactical::PlayerId)> campaign_retreat_order;
    // WGA-80/81: the campaign owner rebuilds this retained-parent query before
    // child initialization/load resume; it reads without servicing the parent.
    std::function<std::optional<numeric::LuaNumber>(std::string_view, sim::tactical::PlayerId)> evaluate_in_galactic_context;
    // FD-71: owned authoritative scoring, distinct from the derived results view.
    std::shared_ptr<authoritative::TacticalScoring> scoring;
    // Optional fixture/controller admission boundary in completed simulation ticks.
    // Omitted players run immediately; admitted control never changes the world clock.
    std::map<sim::tactical::PlayerId, std::uint64_t> player_ai_start_ticks;
};

// The AI XML files the goal system reads (#449), as logical paths.
[[nodiscard]] std::vector<std::string> required_xml();
[[nodiscard]] std::vector<std::string> required_xml(sim::tactical::BattleDomain domain);

// The Lua files the host loads, as logical paths: the freestore and its
// library chain, and the plan library that holds the contrast weights.
[[nodiscard]] std::vector<std::string> required_modules(const AiSetup& setup);

// AI-16: the selected controller's authored space freestore; the setup default
// remains available to hosts that deliberately run without goal XML.
[[nodiscard]] std::string freestore_module(const AiSetup& setup, const AiPlayer& player);

// The instance ID of an AI player's freestore script.
[[nodiscard]] constexpr std::uint64_t freestore_instance(sim::tactical::PlayerId player) noexcept {
    return 1000U + player;
}

// Wraps a fresh world (tick zero) in a scripted session that runs the AI.
// `modules` holds at least required_modules(), keyed by logical path.
[[nodiscard]] core::Result<authoritative::ScriptedTacticalSession> create_session(
    sim::tactical::TacticalSession world, const AiSetup& setup, const std::map<std::string, std::string>& modules);

// Prepare the same AI for a campaign-owned child. The scheduler and callback share
// one host; use the callback after creating/restoring that child's script session.
struct PreparedAiSession {
    authoritative::ScriptScheduler scripts;
    std::function<core::Result<void>(authoritative::ScriptedTacticalSession&)> bind;
};
[[nodiscard]] core::Result<PreparedAiSession> prepare_session(
    const AiSetup& setup, const std::map<std::string, std::string>& modules);

// The stock qualification cohort (AI-30 / SAE-03), and the plan library chain above the
// freestore's (pgevents, PGTaskForce), as logical paths.
[[nodiscard]] std::vector<std::string> selected_plans();
// PL-02: candidates from the frozen mounted script inventory, in logical order.
[[nodiscard]] std::vector<std::string> selected_plans(const std::map<std::string, std::string>& modules);
// The same discovery for a mode's plan directory (space: SpaceMode, land: LandMode).
[[nodiscard]] std::vector<std::string> selected_plans(const std::map<std::string, std::string>& modules,
    sim::tactical::BattleDomain domain);
[[nodiscard]] std::vector<std::string> plan_library_modules();

// One selected plan after its definition load: the goal category its Definitions() sets and
// what the load reported (#79 "plans" step).
struct PlanInspection {
    std::string path;
    std::string category;                  // empty when Definitions() set none
    std::vector<std::string> diagnostics;  // "code message"
    bool loaded{};                         // the chunk ran and the definition load finished
};

// Loads each plan in its own instance through the host's bindings and runs the engine's plan
// definition load (PlanDefinitionLoad, Base_Definitions). `modules` holds at least
// required_modules(), plan_library_modules() and the admitted plan inventory.
[[nodiscard]] core::Result<std::vector<PlanInspection>> inspect_plans(
    const AiSetup& setup, const std::map<std::string, std::string>& modules);
// Qualify an explicit fixture cohort while retaining its full dependency manifest.
[[nodiscard]] core::Result<std::vector<PlanInspection>> inspect_plans(
    const AiSetup& setup, const std::map<std::string, std::string>& modules, const std::vector<std::string>& paths);

// The engine APIs the selected plans call that the host does not simulate (stand-ins), one
// line each (#449).
[[nodiscard]] std::vector<std::string> unsupported_plan_calls();

} // namespace eawr::script::foc
