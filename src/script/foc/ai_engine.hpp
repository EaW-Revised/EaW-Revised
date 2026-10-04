#pragma once

// The FoC tactical AI engine side (#449, docs/behaviour/foc-tactical-ai.md "Goal system",
// "Perception", "Plans and TaskForces"): the per-player perception, goal, planning, execution
// and learning systems that decide which plans run and with which units. Decisions commit
// serially on the tick barrier (ScriptEngine::before_service / after_service); independent
// perception contributions may prepare on workers. Bindings read between those barrier steps.
// Rule IDs are those of the behaviour note.

#include "ai_data.hpp"
#include "host.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace eawr::script::foc::ai {

using detail::Host;
using detail::ViewUnit;
using detail::WorldView;
namespace tactical = sim::tactical;
namespace math = sim::math;

inline constexpr std::uint32_t handle_taskforce = 5;   // id: TaskForce ID
inline constexpr std::uint32_t handle_block = 6;       // id: block_id(instance, command sequence)
inline constexpr std::uint32_t handle_ai_target = 7;   // id: AI target ID

// Engine requests of the plan bindings (ScriptCommand::verb); after_service takes them.
inline constexpr std::string_view verb_ai = "foc.ai";

// Plan script instances: 100000 + n, n counting from 0 over the battle.
inline constexpr std::uint64_t first_plan_instance = 100000;

// SAE-10: one requested point, then expanding ten-angle rings; one ring per service.
[[nodiscard]] std::optional<math::Vec3> reinforcement_candidate(const math::Vec3& requested,
    std::uint32_t attempt, math::Fixed yaw = {},
    const std::optional<std::array<math::Fixed, 4>>& bounds = std::nullopt);

[[nodiscard]] constexpr std::uint64_t block_id(std::uint64_t instance, std::uint64_t sequence) noexcept {
    return (instance << 32) | (sequence & 0xffffffffULL);
}

// ---- Perception grid (PG) -------------------------------------------------------------------

struct Rect {
    Real x{};
    Real y{};
    Real width{};
    Real height{};
};

// One threat entry of an object: the object as a whole or one of its weapon hardpoints (PG-02).
struct ThreatEntry {
    std::int32_t weapon{-1};     // AiWeapon index, -1 for the object
    Real power{};                // combat power spread over the zone, per cell
    Real x{};                    // zone centre relative to the grid's top left (x right, y down)
    Real y{};
    Real radius{};               // negative: no zone (a destroyed hardpoint)
    std::int32_t x0{}, x1{}, y0{}, y1{}; // covered cells, inclusive
};

class ThreatGrid {
public:
    // Only the ordered engine preparation owns this scope. It ends before Lua's parallel
    // readers; those readers use the original const query path without shared cache writes.
    class Preparation final {
    public:
        ~Preparation();
        Preparation(const Preparation&) = delete;
        Preparation& operator=(const Preparation&) = delete;
    private:
        friend class ThreatGrid;
        Preparation(ThreatGrid& grid, const sim::PartitionExecutor* executor, const Host& host, const WorldView& view);
        ThreatGrid& grid_;
    };
    [[nodiscard]] Preparation prepare(const sim::PartitionExecutor* executor, const Host& host, const WorldView& view) {
        return Preparation(*this, executor, host, view);
    }
    void partition(const AiBounds& bounds, std::int32_t x_cells, std::int32_t y_cells, const Constants& constants);
    [[nodiscard]] bool ready() const noexcept { return x_cells_ > 0; }
    // PG-03: each object's entries are refreshed on the object's own cadence.
    void service(const WorldView& view, const Host& host, std::int64_t frame);

    // PG-05: the threat in a rectangle of the objects of `player`'s allies (friendly) or enemies.
    [[nodiscard]] Real force(const Host& host, const WorldView& view, const Rect& rect, std::uint64_t category,
        tactical::PlayerId player, bool friendly, Real attenuator, sim::EntityId excluded) const;
    // PG-06: the combat power of every object of `player`'s allies (friendly) or enemies;
    // player 0 counts every player's.
    [[nodiscard]] Real total_force(const Host& host, const WorldView& view, std::uint64_t category,
        tactical::PlayerId player, bool friendly, Real attenuator) const;
    // PG-07: the fraction of the other players' objects of the category the player sees.
    [[nodiscard]] Real force_visibility(const Host& host, const WorldView& view, std::uint64_t category,
        tactical::PlayerId player) const;
    [[nodiscard]] Real left() const noexcept { return left_; }
    [[nodiscard]] Real top() const noexcept { return top_; }
    [[nodiscard]] Real right() const noexcept { return right_; }
    [[nodiscard]] Real bottom() const noexcept { return bottom_; }
    [[nodiscard]] Real cell_width() const noexcept { return cell_width_; }
    [[nodiscard]] Real cell_height() const noexcept { return cell_height_; }
    [[nodiscard]] std::int32_t x_cells() const noexcept { return x_cells_; }
    [[nodiscard]] std::int32_t y_cells() const noexcept { return y_cells_; }
    // The objects the grid tracks (the complete object lists), ascending ID.
    [[nodiscard]] std::vector<sim::EntityId> objects() const;

private:
    struct Tracked {
        std::vector<ThreatEntry> entries;
        std::int64_t next_service{};
        math::Vec3 last_position{};
        bool has_position{};
    };
    void add(const ViewUnit& unit, const Host& host, Tracked& tracked);
    void remove(sim::EntityId object, Tracked& tracked);

    Real left_{}, top_{}, right_{}, bottom_{};
    Real cell_width_{}, cell_height_{}, min_radius_{};
    std::int32_t x_cells_{}, y_cells_{};
    Constants constants_{};
    // Per cell, the (object, entry index) pairs whose zone covers it, in insertion order.
    std::vector<std::vector<std::pair<sim::EntityId, std::size_t>>> cells_;
    std::map<sim::EntityId, Tracked> tracked_;
    using TotalKey = std::tuple<std::uint64_t, tactical::PlayerId, bool, std::uint64_t>;
    using ForceKey = std::tuple<std::uint64_t, tactical::PlayerId, bool, std::uint64_t,
        std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, sim::EntityId>;
    [[nodiscard]] bool preparing(const Host& host, const WorldView& view) const noexcept {
        return executor_ != nullptr && preparation_host_ == &host && preparation_view_ == &view;
    }
    void clear_queries() noexcept;
    const sim::PartitionExecutor* executor_{};
    const Host* preparation_host_{};
    const WorldView* preparation_view_{};
    mutable std::map<TotalKey, Real> total_queries_;
    mutable std::map<ForceKey, Real> force_queries_;
};

// ---- Targets (GS-10) ------------------------------------------------------------------------

struct Target {
    std::uint64_t id{};              // engine-wide ID (handle_ai_target)
    sim::EntityId object{};          // a unit target; 0 for a region
    std::int32_t cell_x{}, cell_y{}; // a region target's column and row
    Rect region;                     // a region target's rectangle
    Real x{}, y{};                   // a region target's centre
    std::string name;                // OBJECT_<id> or CELL_<x>_<y> (PE-12)
};

// FT-01/FT-03: FindTarget's choice among the scores of the candidates that passed the flag and
// distance filters, in target-list order. The best is the first strictly above every earlier
// score and above 0; only positive scores weigh in the draw. `unit_draw` gives a value in
// [0, 1) and is called only when a draw happens. Answers the chosen index, nullopt for nil.
[[nodiscard]] std::optional<std::size_t> choose_target(const std::vector<Real>& scores, Real fraction,
    const std::function<Real()>& unit_draw);

// ---- Plans (PL) -----------------------------------------------------------------------------

// A TaskForce team definition entry (PL-11).
struct TeamDef {
    std::int32_t min_count{};
    std::int32_t max_count{};
    std::int32_t percentage{};
    bool percentage_based{};
    std::vector<tactical::TypeId> types; // possible types, ascending type ID
};

struct TaskForceDef {
    std::string name;
    std::vector<TeamDef> teams;
    bool escort{};
    bool required{};
    Real minimum_force{};
    std::int32_t minimum_size{};
};

struct PlanDef {
    std::string name;               // script name as written in the path, e.g. DestroyUnit
    std::string module;             // logical path
    std::vector<std::string> goals; // upper-case goal names of Category
    std::vector<TaskForceDef> taskforces;
    bool ignore_target{};
    bool magic{};
    bool allow_free_store{true};
    bool allow_engaged{true};
    Real per_failure_contrast_adjust{};
    Real min_contrast{};
    Real max_contrast{};
    std::vector<std::uint64_t> required_categories;
};

// One entry of a contrast list (PL-30): a category's force (entry 0: the total).
struct Contrast {
    std::uint64_t category{};
    Real force{};
};

// The unit selection of an instantiated goal (PL-20).
struct PotentialPlan {
    std::size_t plan{};                        // PlanDef index
    bool valid{};
    bool reserved{};
    std::vector<tactical::TypeId> units;       // instantiated units' types
    std::vector<sim::EntityId> freestore;      // the free store object of each, 0 when none
    // SAE-03: 0 existing object, 1 pooled reinforcement, 2 new production.
    std::vector<std::uint8_t> sources;
    std::vector<sim::EntityId> producers;
    std::vector<std::uint64_t> pool_tokens;
    Real cost{};
    std::vector<std::size_t> taskforce_of_unit; // the TaskForce each unit joins
    std::vector<std::size_t> team_of_unit; // its authored team within that TaskForce
    std::vector<Contrast> target_contrast;     // PL-31: the target's contrast list
    std::vector<Contrast> current_contrast;    // what the selected units leave of it
    Real threshold{};
};
// SAE-05: one activation gate; SAE-09's tactical activation estimate is zero.
[[nodiscard]] inline Real production_time_limit(const GoalType& goal, const std::optional<Real> linear,
    const Real adjustment = Real{}) {
    return linear ? to_single((*linear + adjustment) * goal.build_time_delay_tolerance)
                  : to_single(goal.time_limit + adjustment);
}
[[nodiscard]] inline bool production_time_allowed(const GoalType& goal, const Real linear, const Real estimate) {
    const auto limit = production_time_limit(goal, linear);
    return !(Real{} < limit && limit < estimate);
}

// ---- Goals (GS) -----------------------------------------------------------------------------

struct Goal {
    std::uint64_t id{};
    std::size_t function{};        // index into the player's goal functions
    std::uint64_t target{};        // Target ID, 0 for a global goal
    Real desire{};                 // current raw desire
    bool active{};
    bool finished{};
    std::optional<PotentialPlan> potential;
    std::uint64_t plan{};          // running plan ID, 0 when none
};

// ---- Execution (EX) -------------------------------------------------------------------------

struct BuildTask {
    std::uint64_t taskforce{};
    std::uint64_t block{};
    tactical::TypeId type{};
    sim::EntityId object{};                 // the free store object assigned
    std::uint8_t source{};
    sim::EntityId producer{};
    std::uint64_t completion{};
    std::uint64_t issued{};
    sim::EntityId child_floor{};
    std::uint64_t pool_token{};
    bool pad_build{};
    bool finished{};
    bool failed{};
};

// A blocking object a plan script waits on (EX-20).
struct Block {
    enum class Kind : std::uint8_t { produce, move, ambush, reinforce } kind{Kind::move};
    std::uint64_t id{};
    std::uint64_t instance{};
    std::uint64_t taskforce{};
    // Movement (EX-30): what the movers are ordered to do; an attack or attack-move on a
    // visible enemy object ends like an attack (EX-31).
    enum class Order : std::uint8_t { move, attack, attack_move, guard } order{Order::move};
    bool attack{};
    sim::EntityId destination_object{};
    math::Vec3 destination{};
    std::vector<sim::EntityId> movers;       // the move list still moving
    std::map<sim::EntityId, std::uint64_t> ordered_tick;
    // Ambush (EX-35): the side of the target, the distance and the threat tolerance.
    std::int64_t side{};
    Real distance{};
    Real tolerance{};
    std::int64_t last_update{};
    bool ready{};
    bool finished{};
    bool result{true};
    tactical::TypeId reinforcing{};
    sim::EntityId entity_floor{};
    std::uint64_t issued{};
    std::uint32_t reinforcement_attempt{};
    std::uint64_t pool_token{};
    std::uint64_t waiting_on{}; // SAE-11: another Lua handle for the same live operation
};

struct TaskForce {
    std::uint64_t id{};
    std::uint64_t plan{};
    std::size_t definition{};
    std::string name;
    std::vector<sim::EntityId> units;         // members, in joining order
    std::vector<tactical::TypeId> types;      // what Produce_Force builds
    std::vector<std::uint8_t> sources;
    std::vector<sim::EntityId> producers;
    struct Purchase {
        tactical::TypeId type{};
        std::uint64_t token{};
    };
    std::vector<Purchase> pooled;
    std::vector<std::uint64_t> pool_tokens;
    std::int64_t thread{-1};                  // thread slot of <name>_Thread
    bool had_units{};
    bool damaged_pending{};                   // one Unit_Damaged queued since the last pump
    std::map<sim::EntityId, sim::EntityId> last_target; // unit -> attack target (Target_In_Range)
};

struct Plan {
    std::uint64_t id{};
    std::uint64_t instance{};
    std::size_t definition{};
    std::uint64_t goal{};
    tactical::PlayerId player{};
    std::uint64_t target{};                   // Target ID, 0 for none
    sim::EntityId target_object{};
    bool target_destroyed_signalled{};
    std::vector<std::uint64_t> taskforces;
    bool result{};
    bool removable{true};
    bool exited{};
    bool purge{}; // PL-45: Purge_Goals was called; applied at the next planning service
    std::uint64_t start_tick{};
    bool requires_production{};
    std::vector<sim::EntityId> reserved_pads;
};

// A line of the plan timeline (#449 eye check, headless test).
struct PlanRecord {
    std::uint64_t tick{};
    tactical::PlayerId player{};
    std::string plan;
    std::string goal;
    std::string target;
    std::string event; // started, produced, order, event, finished
    std::string detail;
};

// The Lua cost of one tick (#449 budget): instructions of the AI's instances.
struct TickCost {
    std::uint64_t tick{};
    std::int64_t freestore{};
    std::int64_t plans{};
    std::uint32_t plan_instances{};
    std::uint32_t freestore_runs{};
    // #957: the tick's scheduled work.
    std::uint32_t goals_evaluated{};
    std::uint32_t maintenances{};
    std::uint32_t plans_attached{};
    std::uint32_t plans_deferred{};
    std::uint32_t plans_pumped{};
    std::uint32_t reinforcement_candidates{};
    std::uint64_t reinforcement_search_ns{};
};

// ---- Perception evaluation (PE) -------------------------------------------------------------

// The context of one evaluation (PE-06): the AI player and the context variables.
struct Context {
    tactical::PlayerId player{};
    std::optional<tactical::PlayerId> enemy;
    std::optional<tactical::PlayerId> human;
    const Target* target{};
};

// ---- Per-player systems ---------------------------------------------------------------------

struct GoalFunctionEntry {
    const GoalType* goal{};
    std::string goal_name;     // upper case
    const Equation* equation{};
    std::string equation_name; // as written
};

// Learning records (LS-01): outcomes with their expiry frame (-1: never).
struct History {
    std::vector<std::pair<bool, std::int64_t>> entries;
};

struct PlayerAi {
    tactical::PlayerId player{};
    const PlayerType* type{};
    const Template* space_template{};
    Difficulty difficulty{};
    std::vector<GoalFunctionEntry> functions;
    std::vector<std::uint64_t> targets;            // goal target list (GS-10), in list order
    std::int64_t next_goal{}, next_planning{}, next_execution{}, next_learning{};
    // #957 (SCH-02): the player's position among the AI players, in ascending ID; its phase, in
    // frames, under the staggered schedule.
    std::int64_t phase{};
    // Goal system (GS-02 to GS-05).
    std::int64_t sleep_frames{};
    std::int64_t per_frame{1};
    std::int64_t nontrivial{};
    std::size_t next_function{};
    std::size_t next_target{};
    bool maintenance_due{};
    std::vector<Goal> proposed;
    std::vector<Goal> active;
    std::uint32_t proposal_blocks{};               // Block_Goal_Proposal holders
    // Learning (LS): by (goal, target ID) and (plan index, target ID); plan success by plan.
    std::map<std::pair<std::string, std::uint64_t>, History> activations;
    std::map<std::pair<std::string, std::uint64_t>, History> goal_outcomes;
    std::map<std::pair<std::size_t, std::uint64_t>, History> plan_outcomes;
    std::map<std::size_t, std::pair<std::int64_t, std::int64_t>> plan_success; // successes, attempts
    // Execution: free store reservations (EX-01) and build tasks (EX-10).
    std::map<sim::EntityId, std::uint64_t> reserved; // object -> goal ID
    std::map<sim::EntityId, std::uint64_t> assigned; // object -> TaskForce ID
    std::map<tactical::TypeId, std::uint32_t> reserved_pool;
    std::map<std::uint64_t, std::uint64_t> reserved_pool_tokens; // purchase token -> goal
    std::map<std::uint64_t, Real> reserved_credits;
    std::vector<BuildTask> tasks;
    std::uint64_t next_goal_id{1};
};

// #957 (SCH-04): a goal whose plan script waits for an attach budget.
struct PendingAttach {
    tactical::PlayerId player{};
    std::uint64_t goal{};
};

// ---- The engine -----------------------------------------------------------------------------

class Engine {
public:
    Engine(std::shared_ptr<Host> host, AiData data, std::vector<PlanDef> plans);

    // Serial, before the script service: perception, goals, planning and execution of every AI
    // player in ascending player ID (GS-01). Engine events for the scripts are submitted to
    // `scripts` with keys from `sequence`.
    [[nodiscard]] core::Result<void> before_service(const tactical::TacticalSession& world,
        const tactical::TacticalSnapshot& snapshot, authoritative::ScriptScheduler& scripts, std::uint64_t& sequence,
        const sim::PartitionExecutor* executor = nullptr);
    // Serial, after the script service: takes the plan scripts' engine requests out of the
    // report and adds the engine's unit orders.
    [[nodiscard]] core::Result<void> after_service(authoritative::ServiceReport& report);

    // ---- Reads for the bindings (const; no engine step runs during the script service) ----
    [[nodiscard]] std::optional<Real> evaluate(std::string_view equation, tactical::PlayerId player,
        const Target* target) const;
    [[nodiscard]] const Target* target(std::uint64_t id) const;
    [[nodiscard]] const Target* target_of_object(sim::EntityId object) const;
    [[nodiscard]] const TaskForce* taskforce(std::uint64_t id) const;
    [[nodiscard]] const Plan* plan_of_instance(std::uint64_t instance) const;
    [[nodiscard]] const Plan* plan(std::uint64_t id) const;
    [[nodiscard]] const Block* block(std::uint64_t id) const;
    [[nodiscard]] const ThreatGrid& grid() const noexcept { return grid_; }
    [[nodiscard]] bool reserved(sim::EntityId object) const;
    // DT-03: the attacker that threatens the objects most.
    [[nodiscard]] sim::EntityId deadly_enemy(const std::vector<sim::EntityId>& objects) const;
    [[nodiscard]] const std::vector<PlanRecord>& records() const noexcept { return records_; }
    [[nodiscard]] const std::vector<TickCost>& costs() const noexcept { return costs_; }
    [[nodiscard]] std::int64_t frame() const noexcept { return frame_; }
    [[nodiscard]] const AiData& data() const noexcept { return data_; }
    [[nodiscard]] const std::vector<PlanDef>& plans() const noexcept { return plans_; }
    // The region target that holds a point (FT-02).
    [[nodiscard]] const Target* region_at(Real x, Real y) const;
    [[nodiscard]] const std::vector<std::uint64_t>* target_list(tactical::PlayerId player) const;
    [[nodiscard]] std::optional<Real> global_value(const std::string& name) const;
    // A unit's type as the goal system counts it (a squadron container's squadron type).
    [[nodiscard]] bool in_freestore(const ViewUnit& unit, tactical::PlayerId player) const;
    // GS-11: whether a target matches application flags (upper-case names) for the player.
    [[nodiscard]] bool target_matches(tactical::PlayerId player, const std::set<std::string>& flags, const Target* target) const;
    [[nodiscard]] sim::EntityId producer(tactical::PlayerId player, tactical::TypeId type,
        sim::EntityId preferred = 0) const;
    [[nodiscard]] const tactical::BuildOption* build_option(tactical::PlayerId player, tactical::TypeId type,
        sim::EntityId producer) const;
    struct ProducerWork {
        std::uint64_t entities{};
        std::uint64_t candidates{};
    };
    [[nodiscard]] ProducerWork producer_work() const noexcept { return producer_work_; }

private:
    friend struct EngineContractAccess;
    void prepare_producers() const;
    struct Evaluator;
    // Perception.
    void build_targets(PlayerAi& player);
    void update_targets();
    [[nodiscard]] std::optional<Real> run_equation(const Equation& equation, const Context& context) const;
    // Goal system.
    void initialize_goals(PlayerAi& player);
    [[nodiscard]] std::int64_t proposal_budget(std::int64_t count) const;
    void service_goals(PlayerAi& player);
    // #957: the schedule's counters, and the plans waiting for their attach budget.
    [[nodiscard]] bool staggered() const noexcept { return host_->setup.schedule.mode == AiSchedule::Mode::staggered; }
    void queue_attach(PlayerAi& player, const Goal& goal);
    [[nodiscard]] core::Result<void> drain_attaches(authoritative::ScriptScheduler& scripts, std::uint64_t& sequence);
    void propose(PlayerAi& player);
    void maintain(PlayerAi& player);
    void maintain_category(PlayerAi& player, const std::string& category, std::vector<Goal>& kept);
    [[nodiscard]] bool proposable(const PlayerAi& player, const GoalFunctionEntry& function) const;
    [[nodiscard]] bool applies(const PlayerAi& player, const GoalType& goal, const Target* target) const;
    [[nodiscard]] bool like(const PlayerAi& player, const Goal& a, const Goal& b) const;
    [[nodiscard]] bool plan_goal(PlayerAi& player, Goal& goal);
    [[nodiscard]] bool select_units(PlayerAi& player, const Goal& goal, PotentialPlan& potential);
    [[nodiscard]] bool production_time_allowed(const PlayerAi& player, const Goal& goal) const;
    [[nodiscard]] bool test_valid(PlayerAi& player, Goal& goal);
    [[nodiscard]] bool test_target_contrast(PlayerAi& player, Goal& goal);
    void reserve(PlayerAi& player, Goal& goal);
    void release(PlayerAi& player, const Goal& goal);
    void register_activation(PlayerAi& player, const Goal& goal, bool success);
    [[nodiscard]] std::int64_t recent_failures(const History* history) const;
    [[nodiscard]] Real category_budget(const PlayerAi& player, const std::string& category) const;
    [[nodiscard]] Context context_of(const PlayerAi& player, const Target* target) const;
    [[nodiscard]] std::vector<Contrast> contrast_list(const PlayerAi& player, const Goal& goal, const PlanDef& plan,
        Real max_factor) const;
    [[nodiscard]] std::vector<const ViewUnit*> freestore_list(const PlayerAi& player, const Goal& goal,
        const PlanDef& plan) const;
    // Planning and execution.
    [[nodiscard]] core::Result<void> attach_plan(PlayerAi& player, Goal& goal, authoritative::ScriptScheduler& scripts,
        std::uint64_t& sequence);
    [[nodiscard]] core::Result<void> service_plans(PlayerAi& player, authoritative::ScriptScheduler& scripts,
        std::uint64_t& sequence);
    void finish_plan(PlayerAi& player, std::uint64_t plan_id, authoritative::ScriptScheduler& scripts, bool abandoned = false);
    void service_execution(PlayerAi& player);
    void service_reinforcements(PlayerAi& player);
    void service_blocks();
    void service_taskforce_events();
    void track_damage(const tactical::TacticalSnapshot& snapshot);
    void emit_call(const Plan& plan, const TaskForce& taskforce, std::string_view event, authoritative::ValueList arguments);
    void emit_thread_event(const Plan& plan, const TaskForce& taskforce, std::string_view event,
        authoritative::ValueList arguments);
    void order_block(Block& block, const TaskForce& taskforce, const Plan& plan);
    [[nodiscard]] std::optional<math::Vec3> ambush_point(const Block& block, const Plan& plan) const;
    void order_ambush(Block& block, const TaskForce& taskforce, const Plan& plan, bool fresh);
    void remove_member(TaskForce& taskforce, sim::EntityId unit);
    [[nodiscard]] bool defines_function(std::uint64_t instance, const std::string& name) const;
    void record(const Plan* plan, tactical::PlayerId player, std::string event, std::string detail = {});
    [[nodiscard]] PlayerAi* player_ai(tactical::PlayerId player);
    [[nodiscard]] const PlayerAi* player_ai(tactical::PlayerId player) const;
    [[nodiscard]] Goal* find_goal(PlayerAi& player, std::uint64_t id);
    [[nodiscard]] std::string target_name(const Target* target) const;
    [[nodiscard]] Real power(const ViewUnit& unit) const;
    void issue_move(const Plan& plan, const ViewUnit& unit, const math::Vec3& destination);
    void issue_attack(const Plan& plan, const ViewUnit& unit, sim::EntityId target);
    // FH-40: an attack-move or guard of `target`, or of `destination` when it is zero.
    void issue_order(const Plan& plan, const ViewUnit& unit, std::string_view verb, sim::EntityId target,
        const math::Vec3& destination);

    std::shared_ptr<Host> host_;
    mutable bool producers_prepared_{};
    mutable std::map<std::pair<tactical::PlayerId, tactical::TypeId>, std::vector<sim::EntityId>> producers_;
    mutable ProducerWork producer_work_{};
    AiData data_;
    std::vector<PlanDef> plans_;
    ThreatGrid grid_;
    std::vector<PlayerAi> players_;
    std::map<std::uint64_t, Target> targets_;
    std::map<std::uint64_t, TaskForce> taskforces_;
    std::map<std::uint64_t, Plan> running_;
    std::map<std::uint64_t, Block> blocks_;
    std::map<std::uint64_t, std::uint64_t> plan_of_instance_;
    // DT-01: per damaged object, the attackers' threat.
    std::map<sim::EntityId, std::map<sim::EntityId, Real>> threats_;
    // This tick's first hit on each object: (attacker, deliberate) (EX-44).
    std::map<sim::EntityId, std::pair<sim::EntityId, bool>> last_hits_;
    std::map<std::string, Real, std::less<>> globals_;
    std::vector<PlanRecord> records_;
    std::vector<TickCost> costs_;
    // #957: counters of the barrier step in progress and the plans waiting to be attached.
    TickCost work_{};
    std::vector<PendingAttach> pending_attach_;
    // Pending script events of this barrier and the engine's own orders for the next tick.
    std::vector<authoritative::ScriptEvent> events_;
    std::vector<authoritative::ScriptCommand> orders_;
    std::uint64_t event_tick_{};
    std::uint64_t* sequence_{};
    authoritative::ScriptScheduler* scripts_{};
    AiRandom sync_;                  // the goal system's synchronized draws (GS-07)
    std::uint32_t game_seed_{};
    std::int64_t frame_{};
    std::uint64_t next_target_id_{1};
    std::uint64_t next_taskforce_id_{1};
    std::uint64_t next_plan_id_{1};
    std::uint64_t next_instance_{first_plan_instance};
    std::uint64_t order_sequence_{};
    bool initialized_{};
};

// PL-10 to PL-12: a plan from the globals its definition load leaves: Category, TaskForce,
// IgnoreTarget, MagicPlan, AllowFreeStoreUnits, AllowEngagedUnits, PerFailureContrastAdjust,
// MinContrastScale, MaxContrastScale and RequiredCategories, in that order. `notes` collects
// what the host leaves out.
[[nodiscard]] core::Result<PlanDef> build_plan(const Host& host, std::string name, std::string module,
    const authoritative::ValueList& globals, std::vector<std::string>& notes);

} // namespace eawr::script::foc::ai
