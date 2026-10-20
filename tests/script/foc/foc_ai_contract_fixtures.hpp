#pragma once

// Contracts of the hosted FoC goal-system pieces (#449, docs/behaviour/foc-tactical-ai.md): the
// perceptual equations (PE-01 to PE-05), the AI random (PE-11), the target name hash (PE-12),
// the threat grid (PG-01 to PG-07), the TaskForce definitions (PL-10 to PL-12) and the
// scheduler reads the engine uses (thread slots, instance removal, command sequences). The
// M2 battle with game data (tests/skirmish/foc_plan_tests.cpp) covers the plan bindings.

#include "ai_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::script::foc::ai {
std::shared_ptr<Host> make_tactical_host();

struct EngineContractAccess {
    static PlayerAi& add_player(Engine& engine, tactical::PlayerId id);
    static PlayerAi& fixture_owner(Engine& engine, tactical::PlayerId id = 2);
    static const PlayerAi& fixture_owner(const Engine& engine, tactical::PlayerId id = 2);

    static ThreatGrid& grid(Engine& engine);
    static void prepare_inputs(Engine& engine, const sim::PartitionExecutor& executor);
    static void prepare_pads(Engine& engine, const sim::PartitionExecutor& executor);
    static void request_open(Engine& engine, tactical::PlayerId player);
    static bool has_prepared_open(const Engine& engine, tactical::PlayerId player);
    static void reset_pads(Engine& engine);
    static bool has_prepared_pads(const Engine& engine);
    static const std::vector<sim::EntityId>& producers(const Engine& engine, tactical::PlayerId player, tactical::TypeId type);
    static void reserve_pad(Engine& engine, sim::EntityId pad, tactical::PlayerId player, tactical::TypeId type);
    static void release_pad(Engine& engine, sim::EntityId pad);
    static bool has_prepared_station_levels(const Engine& engine);
    static std::optional<Real> prepared_station_level(Engine& engine, tactical::PlayerId player,
        const sim::PartitionExecutor& executor);
    static PlayerAi& collect_fixture(Engine& engine);
    static const PlayerAi* player(const Engine& engine, tactical::PlayerId id);
    static void initialize_goals(Engine& engine, PlayerAi& player);
    static void build_targets(Engine& engine, PlayerAi& player);
    static void update_targets(Engine& engine);
    static bool matches(const Engine& engine, tactical::PlayerId player, const std::set<std::string>& flags,
        const Target& target);
    static void target_fixture(Engine& engine, std::uint64_t id, sim::EntityId object);
    static void order_fixture(Engine& engine, std::vector<sim::EntityId> members);
    static void proposal_fixture(Engine& engine);
    static void add_find_target(Engine& engine, std::uint64_t id, sim::EntityId object);
    static void find_target_fixture(Engine& engine);
    static void propose(Engine& engine, PlayerAi& player);
    static std::uint32_t evaluated(const Engine& engine);
    static void goals(Engine& engine, PlayerAi& player);
    static std::uint32_t maintenances(const Engine& engine);
    static std::int64_t admission_budget(const Engine& engine);
    static void maintain(Engine& engine, PlayerAi& player);
    static PlayerAi& active_desire_fixture(Engine& engine, const GoalType& goal,
        const GoalType& competitor, bool removable, bool proposals);
    static void contrast_fixture(Engine& engine, PlayerAi& player);
    static bool contrast(Engine& engine, PlayerAi& player);
    static void retain_pad(Engine& engine, std::uint64_t pad);
    static void release_retained_pad(Engine& engine);
    static bool completed_pad_contrast(Engine& engine, PlayerAi& player);
    static bool selection(Engine& engine, PlayerAi& player, PotentialPlan& potential, std::uint32_t seed,
        std::uint64_t goal_id = 1);
    static const PlayerAi& checked_player(const Engine& engine, tactical::PlayerId id);
    static PlayerAi& checked_player(Engine& engine, tactical::PlayerId id);
    static std::uint32_t selection_seed(const Engine& engine, tactical::PlayerId player);
    static void set_selection_seed(Engine& engine, tactical::PlayerId player, std::uint32_t seed);
    static std::uint32_t engine_seed(const Engine& engine);
    static std::uint32_t draw(Engine& engine, tactical::PlayerId player);
    static void retain_plan_without_instance(Engine& engine, tactical::PlayerId id, bool exited);
    static void ownership_fixture(Engine& engine, authoritative::ScriptScheduler& scripts);
    static void foreign_task(Engine& engine);
    static void restore_foreign_units(Engine& engine);
    // ---- S-T3b: player effects. The reference engine writes directly; the other runs the same turns staged. ----
    template<class Body> static void turn(Engine& engine, tactical::PlayerId id, Body&& body) {
        auto& player = checked_player(engine, id);
        const Engine::TurnScope scope(engine, player);
        body(player);
    }
    static core::Result<void> commit(Engine& engine, authoritative::ScriptScheduler& scripts);
    static bool staging(const Engine& engine);
    static const EffectCounts& totals(const Engine& engine);
    static const PlayerEffects& effects_of(const Engine& engine, tactical::PlayerId id);
    static void emit(Engine& engine, tactical::PlayerId owner, std::uint64_t plan_id, std::uint64_t force_id, std::string_view event);
    static void signal(Engine& engine, tactical::PlayerId owner, std::uint64_t plan_id, std::uint64_t force_id, std::string_view event);
    static void move(Engine& engine, tactical::PlayerId owner, std::uint64_t plan_id, const ViewUnit& unit);
    static void note(Engine& engine, tactical::PlayerId owner, std::uint64_t plan_id, std::string text);
    static void wallet(Engine& engine, tactical::PlayerId player, Real amount);
    static std::int64_t stacked(const Engine& engine, tactical::PlayerId player);
    static std::int64_t host_stack(const Engine& engine, tactical::PlayerId player);
    static void claim_pad(Engine& engine, sim::EntityId pad, tactical::PlayerId player, std::uint64_t goal);
    static std::optional<PadReservation> claim(const Engine& engine, sim::EntityId pad);
    static void free_pads(Engine& engine, tactical::PlayerId player, std::uint64_t goal);
    static void free_pad(Engine& engine, sim::EntityId pad);
    static void wait_for_attach(Engine& engine, PlayerAi& player, std::uint64_t goal_id);
    static void reindex(Engine& engine);
    static void backlog(Engine& engine, tactical::PlayerId player, std::uint64_t goal);
    static void finish(Engine& engine, tactical::PlayerId owner, std::uint64_t plan_id, authoritative::ScriptScheduler& scripts);
    static void execute(Engine& engine, tactical::PlayerId owner);
    static void pool_task(Engine& engine, tactical::PlayerId owner, std::uint64_t force, std::uint64_t token);
    // A goal the player's active list holds; an attachable one carries a potential plan of the first PlanDef.
    static void active_goal(Engine& engine, tactical::PlayerId id, std::uint64_t goal_id, bool attachable);
    // Inside a turn: a scheduler event (submitted at the commit) of the given tick, and an engine order.
    static void stage_event(Engine& engine, std::uint64_t tick);
    static void stage_order(Engine& engine);
    static void bare_players(Engine& engine, authoritative::ScriptScheduler& scripts);
    static core::Result<void> drain(Engine& engine, authoritative::ScriptScheduler& scripts);
    static std::string goal_plans(const Engine& engine);
    static bool attach_throws(Engine& engine, PlayerAi& player, authoritative::ScriptScheduler& scripts);
    static std::size_t waiting_events(const Engine& engine);
    static std::size_t waiting_orders(const Engine& engine);
    static std::size_t waiting_attaches(const Engine& engine);
    static std::size_t forced_units(const Engine& engine, std::uint64_t force);
    static std::vector<std::uint64_t> pooled_tokens(const Engine& engine, std::uint64_t force);
    static void arm_native_records(Engine& engine, bool on);
    static void hold_native_scope(std::optional<Engine::NativeRecordWrite>& slot, Engine& engine);
    static std::size_t committed_claims(const Engine& engine);
    static std::size_t committed_orders(const Engine& engine);
    static std::size_t staged_effects(const Engine& engine);
    static std::size_t committed_wallets(const Engine& engine);
    static void native_changes(Engine& engine, std::uint32_t owner);
    static void native_shared_change(Engine& engine);
    static std::size_t native_depth(const Engine& engine);
    static std::string native_snapshot(const Engine& engine);
    static std::string snapshot(Engine& engine, authoritative::ScriptScheduler& scripts);
    static void orphan_blocks_fixture(Engine& engine);
    static void interleaved_native_fixture(Engine& engine);
    static void stage_movement_queries(Engine& engine);
    static void clear_movement_queries(Engine& engine);
    static void depart_positioning_plan(Engine& engine, tactical::PlayerId id);
    static core::Result<void> attach(Engine& engine, PlayerAi& player, Goal& goal,
        authoritative::ScriptScheduler& scripts, std::uint64_t& sequence);
    static const std::vector<authoritative::ScriptEvent>& attachment_events(const Engine& engine);
    static std::vector<authoritative::ScriptEvent> take_attachment_events(Engine& engine);
    static void defer_event(Engine& engine, authoritative::ScriptEvent event);
    static std::vector<sim::EntityId> eligible(Engine& engine, const PlayerAi& player,
        const Goal& goal, const PlanDef& plan);
    static std::vector<authoritative::ScriptEvent> hazard_events(Engine& engine,
        authoritative::ScriptScheduler& scripts, std::uint64_t& sequence);
    static void event_force(Engine& engine, authoritative::ScriptScheduler& scripts, std::vector<sim::EntityId> members);
    static std::vector<authoritative::ScriptEvent> run_events(Engine& engine);
    static void seed(Engine& engine, PlayerAi& player, authoritative::ScriptScheduler& scripts);
    static void service(Engine& engine, PlayerAi& player, std::int64_t frame);
    static PlayerAi& rejected_station_fixture(Engine& engine);
    static void direct_pad_service(Engine& engine, PlayerAi& player);
    static void reserve(Engine& engine, PlayerAi& player, Goal& goal);
    static Goal& pool_goal(Engine& engine, PlayerAi& player, std::vector<std::uint64_t> tokens, bool active, bool non_pool = false);
    static void retire_pool_goal(Engine& engine, PlayerAi& player, authoritative::ScriptScheduler& scripts, bool abandoned);
    static void await_pool_arrival(Engine& engine, PlayerAi& player, std::uint64_t plan_id);
    static void bind_scripts(Engine& engine, authoritative::ScriptScheduler& scripts, std::uint64_t& sequence);
    static void pause_planning(Engine& engine);
    static void release(Engine& engine, PlayerAi& player, Goal& goal, bool refund = true);
    static void perception_executor(Engine& engine, const sim::PartitionExecutor* executor);
    static std::vector<authoritative::ScriptCommand> take_orders(Engine& engine);
    static void return_orders(Engine& engine, authoritative::ServiceReport& report);
    static void purchase(Engine& engine, PlayerAi& player, const Goal& goal,
        tactical::TypeId type, sim::EntityId producer);
    static void abandon_goal(Engine& engine, PlayerAi& player, std::uint64_t id, authoritative::ScriptScheduler& scripts);
    static PlayerAi& direct_pad_fixture(Engine& engine, tactical::TypeId type, sim::EntityId pad);
    static void seed_purchases(Engine& engine, PlayerAi& player, authoritative::ScriptScheduler& scripts);
    static std::size_t orders(const Engine& engine);
    static void seed_reinforcements(Engine& engine, PlayerAi& player, const std::uint64_t count, const bool large_block = false);
    static const std::vector<authoritative::ScriptCommand>& reinforce_service(
        Engine& engine, PlayerAi& player, const std::int64_t frame);
    static std::uint32_t reinforcement_candidates(const Engine& engine);
    static PlayerAi& duplicate_fixture(Engine& engine);
    static PlayerAi& concurrent_fixture(Engine& engine);
    static bool in_freestore(const Engine& engine, const ViewUnit& unit);
    static void abandon(Engine& engine, PlayerAi& player, authoritative::ScriptScheduler& scripts);
    static bool test_valid(Engine& engine, PlayerAi& player, Goal& goal);
    static void reinforce_at(Engine& engine, const math::Vec3& position);
    static void movement_fixture(Engine& engine, authoritative::ScriptScheduler& scripts,
        std::uint64_t& sequence);
    static void movement_service(Engine& engine, std::int64_t frame);
    static std::uint64_t event_sequence(const Engine& engine);
    static bool movement_finished(const Engine& engine, std::uint64_t id);
    static void guard_fixture(Engine& engine, authoritative::ScriptScheduler& scripts, std::uint64_t& sequence);
    static void empty_guard_force(Engine& engine);
    // EX-31 (#3087): one attack block of player 1's force {10} on `target`, ordered at frame 0.
    static const Block& attack_fixture(Engine& engine, authoritative::ScriptScheduler& scripts,
        std::uint64_t& sequence, sim::EntityId target);
    static std::size_t order_count(const Engine& engine);
    static void point_defence(Engine& engine, std::int64_t frame);
    static void corrupt_systems(Engine& engine, std::int64_t frame);
    static std::size_t point_defence_orders(const Engine& engine);
    static const authoritative::ScriptCommand* last_order(const Engine& engine);
    // WTA-23 to WTA-28: plan instance first_plan_instance of player 2 runs TaskForce 1 with unit 30;
    // a perception grid of 20 x 20 cells of 400 over -4000..4000.
    static void positioning_fixture(Engine& engine);
    // The plan script's Enable_Attack_Positioning request, as after_service takes it.
    static void enable_positioning(Engine& engine, std::int64_t frame, bool on);
    static void positioning_service(Engine& engine, std::int64_t frame);
    static void positioning_point_order(Engine& engine, const math::Vec3& point);
    static void positioning_script_point_order(Engine& engine, const math::Vec3& point);
    static bool positioning_on(const Engine& engine, sim::EntityId unit);
    static void threat(Engine& engine, sim::EntityId victim, sim::EntityId attacker, std::int64_t amount);
};
}

namespace foc_contract {


namespace ai = eawr::script::foc::ai;
namespace auth = eawr::script::authoritative;
namespace foc = eawr::script::foc;
namespace sim = eawr::sim;
namespace tactical = eawr::sim::tactical;
using ai::Real;


extern int& failures;
void expect(bool condition, const std::string& message);


// Two players on different teams, one AI unit type of power 1000 and reach 500 on a 2000 x 2000 map.
struct World {
    std::shared_ptr<ai::Host> host = ai::make_tactical_host();
    std::shared_ptr<ai::WorldView> view = std::make_shared<ai::WorldView>();
    World() {
        foc::AiType type;
        type.type_id = 1;
        type.name = "SHIP";
        type.unit_ai = true;
        type.category_bits = 2;
        type.combat_power = ai::real(1000);
        type.max_attack_distance = ai::real(500);
        type.locomotor = true;
        host->setup.content.types.push_back(type);
        host->setup.content.categories.emplace("CORVETTE", 2);
        host->setup.content.categories.emplace("FIGHTER", 1);
        host->setup.players = {foc::AiPlayer{1, "REBEL", false, true, ""}, foc::AiPlayer{2, "EMPIRE", false, true, ""}};
        for (const foc::AiType& entry : host->setup.content.types) {
            host->types.emplace(entry.type_id, &entry);
            host->types_by_name.emplace(entry.name, &entry);
        }
        for (const foc::AiPlayer& player : host->setup.players) host->players.emplace(player.player, &player);
        view->players = {foc::detail::ViewPlayer{1, 1, 0}, foc::detail::ViewPlayer{2, 2, 1}};
        ai::ViewUnit unit;
        unit.id = 10;
        unit.type = 1;
        unit.owner = 2;
        unit.visible_to = 3;
        view->units.push_back(unit);
        host->view = view;
    }
};



// This host has no tactical session or snapshot. It supplies a different goal
// domain, target set and equation lookup while the shared engine owns the goals.
class ModeWorld final : public ai::World {
public:
    bool friendly_planet_fixture{};
    bool provide_account{};
    tactical::EconomyView reservation_account;
    std::vector<tactical::Player> reservation_players;
    std::map<eawr::sim::EntityId, tactical::PadState> reservation_pads;
    std::shared_ptr<const ai::WorldView> world_view = std::make_shared<ai::WorldView>();
    std::shared_ptr<const ai::WorldView> view() const override { return world_view; }
    mutable std::uint32_t contexts{};
    mutable std::uint32_t evaluations{};
    bool available() const noexcept override { return true; }
    std::uint64_t frame{45};
    std::uint64_t completed_tick() const noexcept override { return frame; }
    std::vector<ai::Target> new_targets(const ai::Engine&) const override { return {}; }
    bool target_alive(const ai::Target& target) const override { return target.object == 700; }
    std::string mode = "GALACTIC";
    std::string_view goal_mode() const noexcept override { return mode; }
    std::optional<Real> perception(const ai::Engine&, const ai::Equation& equation, const ai::Context& context, ai::AiRandom& random) const override {
        ++evaluations;
        class Resolver final : public ai::LookupResolver {
        public:
            const ai::Context& context;
            explicit Resolver(const ai::Context& value) : context(value) {}
            std::optional<Real> resolve(const ai::Lookup& lookup, const std::vector<ai::Binding>&) override {
                if (lookup.tokens == std::vector<std::string>{"VARIABLE_SELF", "PLANETCOUNT"}
                    && context.enemy == 7U && context.human == 9U) return ai::real(3);
                return std::nullopt;
            }
        } resolver(context);
        return ai::run(equation, resolver, random);
    }
    ai::Context goal_context(const ai::PlayerAi& player, const ai::Target* target) const override {
        ++contexts;
        return ai::Context{player.player, 7, 9, target};
    }
    bool target_matches(tactical::PlayerId, const std::set<std::string>& flags, const ai::Target* target) const override {
        return target && target->object == 700 && (flags.contains("PLANET")
            || (friendly_planet_fixture && flags.contains("FRIENDLY")));
    }
    std::vector<ai::Target> targets(const ai::Engine&, const ai::PlayerAi&) const override {
        ai::Target target; target.object = 700; target.name = "PLANET";
        return {target};
    }
    std::span<const tactical::Player> players() const noexcept override { return reservation_players; }
    const tactical::EconomyRules& economy() const noexcept override { static const tactical::EconomyRules empty; return empty; }
    const tactical::EconomyView* account(tactical::PlayerId) const override { return provide_account ? &reservation_account : nullptr; }
    const std::map<eawr::sim::EntityId, tactical::PadState>& pads() const noexcept override {
        return reservation_pads;
    }
    bool arriving(eawr::sim::EntityId) const override { return false; }
    bool build_allowed(tactical::PlayerId, eawr::sim::EntityId, tactical::TypeId) const override { return false; }
    const tactical::BuildOption* build_option(tactical::PlayerId, eawr::sim::EntityId, tactical::TypeId) const override { return nullptr; }
    tactical::ProductionCounts production_counts(tactical::PlayerId, tactical::TypeId) const override { return {}; }
    bool construction_allowed(tactical::PlayerId, eawr::sim::EntityId) const override { return false; }
    bool constructing_for(eawr::sim::EntityId, tactical::PlayerId) const override { return false; }
    eawr::sim::EntityId next_entity_id() const noexcept override { return 701; }
    std::optional<ai::ReinforcementResult> reinforcement_search_result(tactical::PlayerId, std::uint64_t) const override { return std::nullopt; }
    std::span<const tactical::Event> events() const noexcept override { return {}; }
    std::span<const tactical::CombatEvent> combat_events() const noexcept override { return {}; }
};
void equations();
void random_and_hash();
void game_random_forms();
void capture_target_application();
void forced_object_fog();
void freestore_unit_service();
void time_till_dead_bindings();
void type_range_and_hero_bindings();
void collect_free_categories();
void order_destinations();
void special_weapon_bindings();
void manual_special_weapon_binding();
void special_weapon_attachment();
void running_target_contrast();
void running_reservation_contrast();
void retiring_pool_reservations();
void active_tactical_desire();
void active_tactical_preference();
void initial_goal_budget();
void selection_team_starts();
void station_barrier_expiry();
void shared_station_perception();
void campaign_defender_fallback();
void campaign_retreat_inputs();
void economy_inputs();
void tactical_activation_estimate();
void production_lifecycle();
void spending_pad_states();
void spending_pad_refunds();
void spending_wallet_reservations();
void allied_resource_upgrade_selection();
void completed_purchases_are_available();
void reinforcement_work_budget();
void duplicate_reinforcement_calls();
void taskforce_attachment_waits_for_arrival();
void concurrent_reserved_arrivals();
void reinforcement_service_budget();
void reinforcement_gravity_well();
void reinforcement_fog_persistence();
void stealth_blind_queries();
void ai_ability_bound();
void reinforcement_population_wait();
void threat_grid();
void unit_ai_grid_admission();
void shared_view_prepass();
void shared_view_formation_join();
void shared_input_prepasses();
void threat_preparation();
void threat_flat_storage();
void plan_definition_paths();
void mounted_ai_discovery();
void authored_goal_extension();
void unit_concentration();
void coroutine_module_loading();
void movement_query_projection();
void mod_ai_calls();
void taskforce_definitions();
void named_team_authored_order();
void category_team_table_order();
void required_category_unions();
void scheduler_reads();
void taskforce_team_order();
void optional_taskforce_startup();
void hazard_plan_events();
void squadron_target_in_range();
void point_defence_stand_in();
void corrupt_systems_stand_in();
void corrupt_systems_cluster_coordinates();
void squadron_ability_context();
void buzz_droids_lua_point(const bool corrupt = false);
void squadron_movement_blocks();
void guard_block_lifecycle();
void refused_attack_block();
void find_target_reach_contract();
void reachable_target_contract();
void installed_galactic_freestore();
void attack_positioning();
void target_choice();
void threat_decay_constant();
void proximity_contracts();
void reveal_all_contracts();
void faction_controller_inventory();
void slave_i_contracts();
void sundered_contracts();
void missile_shield_contracts();
void native_object_script_ownership();
void object_turn_cadence_pins();
void production_object_turns();
void object_script_contracts();
void ai_difficulty_levels();

} // namespace foc_contract
