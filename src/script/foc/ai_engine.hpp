#pragma once

#include "eawr/core/gc_profile.hpp"
#include "eawr/diagnostics/gc_memory.hpp"
#include "eawr/core/record_digest.hpp"

// The FoC tactical AI engine side (#449, docs/behaviour/foc-tactical-ai.md "Goal system",
// "Perception", "Plans and TaskForces"): the per-player perception, goal, planning, execution
// and learning systems that decide which plans run and with which units. Decisions commit
// serially on the tick barrier (ScriptEngine::before_service / after_service); independent
// perception contributions may prepare on workers. Bindings read between those barrier steps.
// Rule IDs are those of the behaviour note.

#include "ai_data.hpp"
#include "host.hpp"
#include "galactic_ai_selector_runtime.hpp"
#include "eawr/script/foc/galactic_ai.hpp"

#include <cstdint>
#include <array>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace eawr::script::foc::galactic_ai { class NativeActions; }
namespace eawr::script::foc::tactical_ai_detail { class ScriptEvaluators; }
namespace eawr::script::foc::ai {
class GalacticFunding;
struct GalacticBudgetBuild;
namespace galactic_goals { struct MaterializedPlanCatalog; }

using detail::Host;
using detail::ViewUnit;
using detail::WorldView;
namespace tactical = sim::tactical;
namespace math = sim::math;

inline constexpr std::uint32_t handle_taskforce = 5;   // id: TaskForce ID
inline constexpr std::uint32_t handle_block = 6;       // id: block_id(instance, command sequence)
inline constexpr std::uint32_t handle_ai_target = 7;   // id: AI target ID
inline constexpr std::uint32_t handle_freestore = 8;   // id: galactic player ID

// Engine requests of the plan bindings (ScriptCommand::verb); after_service takes them.
inline constexpr std::string_view verb_ai = "foc.ai";

// Plan script instances: 100000 + n, n counting from 0 over the battle.
inline constexpr std::uint64_t first_plan_instance = 100000;

// S9-early, galactic only: every AI player allocates its targets, task forces, plans (potential and
// running share one counter) and plan-script instances from its own range, selected by its slot
// (its position in the setup's player list, below galactic_max_players). Identities below stay under
// 2^53 so they survive a trip through a binary64 number. Instances stay below 2^32 because a block
// identity carries its instance in the upper 32 bits. The tactical engine keeps its engine-wide counters.
inline constexpr std::uint32_t galactic_id_slots = 64;
inline constexpr unsigned galactic_id_shift = 40;       // slot + 1 above the counter
inline constexpr unsigned galactic_instance_shift = 24; // slot above the instance counter
inline constexpr std::uint64_t galactic_id_counter_limit = std::uint64_t{1} << galactic_id_shift;
inline constexpr std::uint64_t galactic_instance_counter_limit = std::uint64_t{1} << galactic_instance_shift;
[[nodiscard]] constexpr std::uint64_t galactic_id_base(std::uint32_t slot) noexcept {
    return (std::uint64_t{slot} + 1U) << galactic_id_shift;
}
[[nodiscard]] constexpr std::uint64_t galactic_instance_base(std::uint32_t slot) noexcept {
    return first_plan_instance + (std::uint64_t{slot} << galactic_instance_shift);
}
// The slot a galactic identity or instance was allocated for, or galactic_id_slots when out of range.
[[nodiscard]] constexpr std::uint32_t galactic_id_slot(std::uint64_t id) noexcept {
    const auto high = id >> galactic_id_shift;
    return high >= 1 && high <= galactic_id_slots ? static_cast<std::uint32_t>(high - 1) : galactic_id_slots;
}
[[nodiscard]] constexpr std::uint32_t galactic_instance_slot(std::uint64_t instance) noexcept {
    if (instance < first_plan_instance) return galactic_id_slots;
    const auto slot = (instance - first_plan_instance) >> galactic_instance_shift;
    return slot < galactic_id_slots ? static_cast<std::uint32_t>(slot) : galactic_id_slots;
}
// Shared GFS-A21/A24 seed mix for galactic and tactical players; the name predates tactical adoption.
[[nodiscard]] constexpr std::uint32_t galactic_stream_seed(std::uint32_t game_seed, std::uint32_t player) noexcept {
    std::uint32_t h = game_seed ^ ((player + 1U) * 0x9e3779b9U);
    h ^= h >> 16U; h *= 0x85ebca6bU; h ^= h >> 13U; h *= 0xc2b2ae35U; h ^= h >> 16U;
    return h;
}

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
    // PE-25: stamps each player's threat cell with the frame its last fog cell reached 0. Called on
    // every enabled turn, also while the AI is held; the first call only records the fog.
    void observe_fog(const Host& host, std::int64_t frame);
    // Records the current fog without stamping (a restored grid resumes from the completed tick).
    void baseline_fog(const Host& host);
    // PE-25 state, for tests and restore checks: the frame a player's threat cell (column, row)
    // was last stamped, 0 when never; how many session players are watched.
    [[nodiscard]] std::int64_t last_seen(std::size_t fog_player, std::int32_t column, std::int32_t row) const noexcept {
        if (fog_player >= fog_watch_.size() || column < 0 || row < 0 || column >= x_cells_ || row >= y_cells_) return 0;
        const auto& stamps = fog_watch_[fog_player].last_seen;
        const auto index = static_cast<std::size_t>(row) * static_cast<std::size_t>(x_cells_) + static_cast<std::size_t>(column);
        return index < stamps.size() ? stamps[index] : 0;
    }
    [[nodiscard]] std::size_t watched_players() const noexcept { return fog_watch_.size(); }
    // PG-03 state: the entity limit recorded at the first service (stations below it never join).
    [[nodiscard]] sim::EntityId setup_limit() const noexcept { return setup_limit_; }
    [[nodiscard]] bool setup_scanned() const noexcept { return setup_scanned_; }
    // PE-25: the mean seconds since the fogged threat cells a rectangle touches were last seen.
    [[nodiscard]] Real time_since_seen(const Host& host, const Rect& rect, tactical::PlayerId player,
        std::int64_t frame) const;
    // Prepare last turn's demand using this turn's completed view. Cold keys stay inline.
    void prepare_contributions();
    struct QueryCounts {
        std::size_t prepared_keys{}, queries{}, warm{}, cold{}, repeats{}, cross_player{}, scalar_hits{};
    };
    [[nodiscard]] const QueryCounts& query_counts() const noexcept { return query_counts_; }
    using QueryObserver = void (*)(std::uint64_t tick, std::size_t queried_keys, const QueryCounts&) noexcept;
    // Optional write-only profiling sink. No runtime I/O or simulation input.
    static void observe_queries(QueryObserver observer) noexcept;

    // PG-05: the threat in a rectangle of the objects of `player`'s allies (friendly) or enemies.
    [[nodiscard]] Real force(const Host& host, const WorldView& view, const Rect& rect, std::uint64_t category,
        tactical::PlayerId player, bool friendly, Real attenuator, sim::EntityId excluded) const;
    // PG-09: company power per admitted entry whose rounded base cell is queried.
    [[nodiscard]] Real concentration(const Host& host, const WorldView& view, const Rect& rect,
        std::uint64_t category, tactical::PlayerId player, bool friendly) const;
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
    friend struct GoalStateCodec;
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
    // PG-03: the first service records the entity IDs of the battle's setup; a station created
    // with it (ID below the limit) is not admitted to the grid. 0 until then.
    sim::EntityId setup_limit_{};
    bool setup_scanned_{};
    // PE-25: per session player (fog index order), the previous sweep's fog rows and, per threat
    // cell, the frame its last fog cell reached 0 (0 until then). The rows are derived and are
    // not saved; a restore re-records them from the completed tick.
    struct FogWatch {
        std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> rows;
        std::vector<std::int64_t> last_seen;
    };
    void sweep_fog(const Host& host, std::int64_t frame, bool stamp);
    std::vector<FogWatch> fog_watch_;
    using TotalKey = std::tuple<std::uint64_t, tactical::PlayerId, bool, std::uint64_t>;
    using ForceKey = std::tuple<std::uint64_t, tactical::PlayerId, bool, std::uint64_t,
        std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, sim::EntityId>;
    using ContributionKey = std::pair<std::uint64_t, std::uint64_t>;
    struct Contribution {
        const ViewUnit* unit{};
        std::optional<Real> total;
        std::size_t offset{}, count{};
    };
    struct Contributions {
        std::vector<Contribution> rows;
        std::vector<Real> entries;
        std::vector<std::vector<std::size_t>> owners;
    };
    struct ContributionSlot {
        sim::EntityId id{};
        const Tracked* tracked{};
    };
    void prepare_slots();
    [[nodiscard]] const Contributions* contributions(std::uint64_t category, Real attenuator,
        tactical::PlayerId player) const;
    [[nodiscard]] bool preparing(const Host& host, const WorldView& view) const noexcept {
        return executor_ != nullptr && preparation_host_ == &host && preparation_view_ == &view;
    }
    void clear_queries() noexcept;
    const sim::PartitionExecutor* executor_{};
    const Host* preparation_host_{};
    const WorldView* preparation_view_{};
    mutable std::map<TotalKey, Real> total_queries_;
    mutable std::map<ForceKey, Real> force_queries_;
    // Topology contains no completed-view pointers and changes only with grid membership/zones.
    bool slots_dirty_{true};
    std::vector<ContributionSlot> slots_;
    std::vector<std::vector<std::size_t>> cell_slots_;
    // The map size is the used prefix; clear_queries expires its rows and view pointers.
    // Backing vectors are staging until contributions_ready_ publishes the completed pass.
    bool contributions_ready_{};
    std::map<ContributionKey, std::size_t> contributions_;
    std::vector<Contributions> contribution_storage_;
    std::vector<tactical::PlayerId> contribution_owners_;
    std::vector<std::tuple<Contribution*, const Tracked*, Real, Real*>> contribution_work_;
    mutable std::map<ContributionKey, std::set<tactical::PlayerId>> requested_;
    std::map<ContributionKey, std::set<tactical::PlayerId>> pending_;
    mutable QueryCounts query_counts_;
};

// ---- Local threat centers (WHE-68) ------------------------------------------------------------

// The strongest local threat center of `objects` (ascending ID): planar cluster
// centers, each scored by the summed `power` of the objects whose planar squared
// distance is within `radius_squared`; equal scores keep the last center. `draw`
// supplies uniform(low, high), called Y then X for each random initial center.
struct ThreatCenter {
    Real x{};
    Real y{};
    Real threat{};
};
ThreatCenter local_threat_center(const std::vector<const ViewUnit*>& objects, Real radius_squared,
    const std::function<Real(const ViewUnit&)>& power, const std::function<Real(Real, Real)>& draw);

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
    std::vector<tactical::TypeId> types; // possible types: named list = authored order, deduped; category list = type-table order
};

struct TaskForceDef {
    std::string name;
    std::vector<TeamDef> teams;
    bool escort{};
    bool required{};
    Real minimum_force{};
    std::int32_t minimum_size{};
    bool deny_special_weapon_attach{}; // HA-01: DenySpecialWeaponAttach entry (last: positional initializers)
};

struct PlanDef {
    std::string name;               // script name as written in the path, e.g. DestroyUnit
    std::string module;             // logical path
    std::vector<std::string> goals; // upper-case goal names of Category
    std::vector<TaskForceDef> taskforces;
    bool ignore_target{};
    bool magic{};
    bool allow_free_store{true};
    // HA-01: some TaskForce with a team lacks DenySpecialWeaponAttach. Retail reads it only for the
    // extra (team-less) attach of special-weapon sources, which the host does not model; a source
    // listed in a team is admitted either way (WSW-47).
    bool allow_special_weapons{};
    bool allow_engaged{true};
    Real per_failure_contrast_adjust{};
    Real min_contrast{};
    Real max_contrast{};
    std::vector<std::uint64_t> required_categories;
    bool galactic_contrast_qualified{};
    std::vector<std::uint64_t> galactic_contrast_order;
    std::map<std::uint64_t, std::vector<detail::ContrastEntry>> galactic_contrast;
};

// One entry of a contrast list (PL-30): a category's force (entry 0: the total).
struct Contrast {
    std::uint64_t category{};
    Real force{};
};

// The unit selection of an instantiated goal (PL-20).
struct PotentialPlan {
    std::uint64_t id{};                        // galactic engine-wide allocation identity
    std::size_t plan{};                        // PlanDef index
    bool valid{};
    bool reserved{};
    bool funded{}; // WAS-25: ignored reservations retain their initial debit
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
    std::vector<std::size_t> contrast_indices; // GS-22: retained allocation categories, 0 is total force
    bool target_contrast_disabled{}; // WGA-341: GC Produce_Force without staging
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
    bool exclusions_registered{}; // WGA-57: exactly once per active/executing goal
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
    bool direct_pad{};
    bool prepaid{};
    bool acknowledged{};
    Real generic_cost{};
    bool finished{};
    bool failed{};
    sim::galactic::PlanetId staging{}, associated_producer{};
    std::uint64_t state_entered{};
    bool salvage{}, allocated{}, joined{}, synchronized_staging{};
    // WGA-485: assignment/member insertion precedes completed galactic staging.
    // These fields are absent from the tactical native archive.
    enum class GalacticAssembly : std::uint8_t { none, assigned, moving, staged, orbital, complete, failed, waiting, countdown };
    GalacticAssembly galactic_assembly{};
    std::uint64_t galactic_assembly_request{};
    Real galactic_stage_seconds{}, galactic_stage_remaining{};
    std::uint64_t galactic_stage_tick{};
};

// A blocking object a plan script waits on (EX-20).
struct Block {
    enum class Kind : std::uint8_t { produce, move, ambush, reinforce, galactic_land, galactic_launch, galactic_form, galactic_invade } kind{Kind::move};
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
    std::uint64_t galactic_request{};
    sim::EntityId galactic_actor{}; // retained result wrapper, independent of completion
    // EX-31 (#3087): -1, or the destination owner when an attack on a non-hostile object refused
    // every mover; such a block waits until the TaskForce empties, the destination dies or its new
    // owner is no enemy.
    std::int64_t held_owner{-1};
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
    sim::galactic::PlanetId galactic_stage{}; // WGA-226/254: retained authored force staging
    sim::EntityId galactic_fleet{}; // WGA-227: actual retained orbital fleet
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
    // Transient proposal diagnostics; never retained by the engine or serialized.
    std::string* proposal_trace{};
};

// ---- Per-player systems ---------------------------------------------------------------------

struct GoalFunctionEntry {
    const GoalType* goal{};
    std::string goal_name;     // upper case
    const Equation* equation{};
    std::string equation_name; // as written
};

// WGA-214..216: append-ordered outcomes with signed expiry; nonpositive rows do not stop pruning.
struct History {
    std::vector<std::pair<bool, std::int64_t>> entries;
};

// WTA-23, WTA-26 to WTA-28: a unit's attack-positioning flag and service state.
inline constexpr std::int64_t attack_positioning_frames = 150; // WTA-27
struct AttackPositioning {
    std::uint64_t instance{};    // the plan instance whose TaskForce switched it on: the orders' issuer
    std::int64_t next{};         // the frame of the unit's next positioning
    sim::EntityId target{};      // the attack target of its last move to an ideal position
};

// #957 (SCH-04): a goal whose plan script waits for an attach budget.
struct PendingAttach {
    tactical::PlayerId player{};
    std::uint64_t goal{};
};

// A free-store pad held for a goal's pad build (EX-01, WBP-47/48).
struct PadReservation {
    tactical::PlayerId player{};
    std::uint64_t goal{};
    tactical::TypeId type{};
    bool consumed{};
    std::uint64_t issued{};
};

// ---- Player effects (S-T3b, GFS-A24) --------------------------------------------------------
//
// What the AI engine writes to engine-wide state while one player's turn runs goes through the
// player's PlayerEffects instead: events and orders (their keys and sequence numbers are numbered
// at the commit), wallet deltas, pad claims and releases, attachment intents, runtime-index
// retirements, scheduler removals, native-record invalidations, journal rows, input counters and work
// counters. The turns still run one after another, and a later turn reads the earlier turns' effects
// stacked on the committed state (credits, pad claims). Engine::commit_effects applies them in
// ascending player order at the serial boundary after the player loop, which numbers and orders them
// exactly as the sequential pass did when it wrote them directly. A player task of S-T4 owns its
// PlayerEffects the same way.
//
// Writes to a row another player owns (a TaskForce, plan or block of a foreign handle) go through
// typed CrossOwnerOps. The turns are still sequential, so an op applies in place at once and the
// owner's later turn sees it exactly as before; the ops are counted, and S-T4 stages them for the
// commit under barrier-start reads (GFS-A22). Stock play never produces one.

// The engine's ID-to-player runtime indices.
enum class RuntimeTable : std::uint8_t { plans, taskforces, blocks, instances };

// A typed operation on a TaskForce, plan or block that another player owns (a handle that
// resolves to a foreign row). Sequential turns apply it in place; S-T4 stages it for the commit, in
// ascending acting-player order, onto the row as it is then.
struct CrossOwnerOp {
    enum class Kind : std::uint8_t { taskforce_join, taskforce_pool, taskforce_consume, plan_release_pad, block_finish };
    Kind kind{};
    std::uint64_t id{};      // the foreign row's identity: TaskForce, plan or block
    std::uint64_t first{};   // join: unit; pool: type; consume: type; release: pad
    std::uint64_t second{};  // pool: token; consume: producer
    bool flag{};             // join: the unit counts as the TaskForce's first (had_units); finish: the result
};

// Reads a player's turn made of other players' uncommitted effects or rows (the overlay depth S-T4
// has to unstack). A read counts only when its answer depends on the other player's data.
struct EffectReads {
    std::uint64_t credits{};   // credit balances that included another player's staged delta
    std::uint64_t pads{};      // pad claims answered by another player's staged claim
    std::uint64_t pad_rows{};  // other players' running plans scanned for pads their goals hold
    std::uint64_t handles{};   // plan/TaskForce/block handles that resolved to another player's row
    std::uint64_t row_scans{}; // other players' runtime rows visited by a global scan
    bool operator==(const EffectReads&) const = default;
};

struct EffectCounts {
    std::uint64_t turns{};        // player turns that staged
    // credit_deltas and pad_effects count the players and pads whose net change the turn staged, not writes.
    std::uint64_t events{}, orders{}, removals{}, credit_deltas{}, pad_effects{}, attach_intents{};
    std::uint64_t index_effects{}, operations{}, diagnostics{}, invalidations{};
    EffectReads reads;
    bool operator==(const EffectCounts&) const = default;
};

struct PlayerAi;
struct PlayerEffects {
    tactical::PlayerId owner{};
    // Scheduler-facing effects, chronological. An event takes its sequence at the commit; `submit` ones
    // go straight to the scheduler (the freestore's service tests), the others wait in the engine's events.
    struct SchedulerEvent { authoritative::ScriptEvent event; bool submit{}; };
    struct SchedulerRemoval { std::uint64_t instance{}; };
    std::vector<std::variant<SchedulerEvent, SchedulerRemoval>> scheduler;
    std::vector<authoritative::ScriptCommand> orders; // sequence taken at the commit
    std::map<tactical::PlayerId, math::Fixed> credits; // wallet deltas, summed
    std::map<sim::EntityId, std::optional<PadReservation>> pads; // net claim per pad; none = released
    std::vector<PendingAttach> attaches;
    // Runtime-index retirements, chronological (the owner already erased the row). Inserts happen at the
    // serial attach boundary, never inside a turn.
    struct IndexEffect { RuntimeTable table{}; std::uint64_t id{}; };
    std::vector<IndexEffect> index;
    std::uint64_t operations{}; // typed operations on foreign rows
    std::vector<CrossOwnerOp> foreign_operations;
    // Each acting player owns any foreign rows it has touched over the immutable barrier-start copy.
    std::map<tactical::PlayerId, std::shared_ptr<PlayerAi>> foreign_players;
    // Pad-perception inputs the turn asked for: the late-prepass counter and the open-pad requests.
    std::uint64_t pad_queries{};
    std::set<tactical::PlayerId> pad_open;
    // Native-record invalidations; a revision bump per mark_native_state_changed of the turn.
    struct Invalidation { std::uint32_t owner{}, kind{}; std::uint64_t key{}; bool exact{}; };
    std::vector<Invalidation> invalidations;
    std::uint64_t revisions{};
    std::size_t scope_depth{}; // nested NativeRecordWrite scopes of the turn; never publish inside it
    // Journal and records, chronological.
    struct JournalPlan { PlanEvent row; bool raw{}; };
    std::vector<std::variant<PlanRecord, JournalPlan, AiServiceEvent>> diagnostics;
    TickCost work{};
    std::uint64_t producer_candidates{};
    EffectReads reads;
    // Galactic only: task changes of the turn, and the turn's own build publication memo (never committed).
    std::uint64_t tasks_revision{};
    std::shared_ptr<const std::vector<GalacticBudgetBuild>> build_cache;
    std::array<std::uint64_t, 3> build_cache_key{};
    tactical::PlayerId build_cache_reader{};

    [[nodiscard]] bool empty() const noexcept {
        return scheduler.empty() && orders.empty() && credits.empty() && pads.empty() && attaches.empty()
            && index.empty() && operations == 0 && invalidations.empty() && revisions == 0
            && diagnostics.empty() && producer_candidates == 0 && pad_queries == 0 && pad_open.empty()
            && tasks_revision == 0;
    }
    // Back to a fresh turn, keeping the vectors' capacity.
    void clear(tactical::PlayerId player) noexcept {
        owner = player;
        scheduler.clear(); orders.clear(); credits.clear(); pads.clear(); attaches.clear(); index.clear();
        operations = 0; foreign_operations.clear(); foreign_players.clear(); pad_queries = 0; pad_open.clear();
        invalidations.clear(); revisions = 0; scope_depth = 0; diagnostics.clear();
        work = {}; producer_candidates = 0; reads = {};
        tasks_revision = 0; build_cache.reset(); build_cache_key = {}; build_cache_reader = 0;
    }
};

// #3944: why a galactic goal or budget evaluation produced nothing (observer counters only).
enum class GalacticRefusal : std::uint8_t { unavailable, blocked, desire, no_plan, units, time, funds, budget };
struct PlayerAi {
    // Mutable execution state belongs to this player; serial adapters preserve global ID order.
    std::map<std::uint64_t, TaskForce> taskforces;
    std::map<std::uint64_t, Plan> running;
    std::map<std::uint64_t, Block> blocks;
    std::map<std::uint64_t, std::uint64_t> plan_of_instance;
    std::map<sim::EntityId, AttackPositioning> positioning;
    tactical::PlayerId player{};
    const PlayerType* type{};
    const Template* space_template{};
    Difficulty difficulty{};
    bool galactic{};
    galactic_ai::TemplatePolicy galactic_policy;
    std::shared_ptr<galactic_ai::Funding> galactic_funding;
    std::map<std::string, std::uint32_t, std::less<>> proposal_counts, exclusion_counts;
    std::vector<GoalFunctionEntry> functions;
    std::vector<std::uint64_t> targets;            // goal target list (GS-10), in list order
    std::int64_t next_goal{}, next_planning{}, next_execution{}, next_learning{};
    // #957 (SCH-02): the player's position among the AI players, in ascending ID; its phase, in
    // frames, under the staggered schedule.
    std::int64_t phase{};
    bool admission_pending{}; // fixture controller: refresh budget on its first admitted service
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
    std::map<std::uint64_t, std::uint32_t> proposal_blockers; // EX-55: goal -> holder count
    // Learning (LS): by (goal, target ID) and (plan index, target ID); plan success by plan.
    std::map<std::pair<std::string, std::uint64_t>, History> activations;
    std::map<std::pair<std::string, std::uint64_t>, History> goal_outcomes;
    std::map<std::pair<std::size_t, std::uint64_t>, History> plan_outcomes;
    std::map<std::size_t, std::pair<std::int64_t, std::int64_t>> plan_success; // successes, attempts
    // WGA-213/221/223: cumulative tables do not decay during recent sweeps.
    std::map<std::string, std::pair<std::int64_t, std::int64_t>, std::less<>> goal_success, unit_survival;
    std::map<std::pair<std::string, std::uint64_t>, std::pair<std::int64_t, std::int64_t>> target_goal_success;
    std::map<std::pair<std::size_t, std::uint64_t>, std::pair<std::int64_t, std::int64_t>> target_plan_success;
    // Execution: free store reservations (EX-01) and build tasks (EX-10).
    std::map<sim::EntityId, std::uint64_t> reserved; // object -> goal ID
    std::map<sim::EntityId, std::uint64_t> assigned; // object -> TaskForce ID
    std::map<tactical::TypeId, std::uint32_t> reserved_pool;
    std::map<std::uint64_t, std::uint64_t> reserved_pool_tokens; // purchase token -> goal
    std::map<std::uint64_t, Real> reserved_credits;
    std::vector<BuildTask> tasks;
    std::uint64_t next_goal_id{1};
    // GFS-A21/A24: per-player synchronized draws in either mode.
    AiRandom sync;
    // Galactic only: the slot selects the identity/instance bases; counters hold the next local number.
    std::uint32_t slot{};
    std::uint64_t next_target_local{1}, next_taskforce_local{1}, next_plan_local{1}, next_instance_local{};
    // S-T3b: this player's uncommitted writes to engine-wide state. Empty outside a player loop; never saved.
    PlayerEffects effects;
    // Observer counters (#3944), player-local: (refusal, goal function index or budget category) -> count. Written
    // only while gc_profile::enabled; never saved, hashed or read by a decision.
    std::map<std::pair<GalacticRefusal, std::uint64_t>, std::uint64_t> refusals;
    void count_refusal(GalacticRefusal why, std::uint64_t index) {
        if (gc_profile::enabled.load(std::memory_order_relaxed)) ++refusals[{why, index}];
    }
};

// ---- The engine -----------------------------------------------------------------------------

class TacticalWorld;

// WGA-56: goal IDs are local to a player; potential/running plan IDs are
// engine-wide. A definition index is never a runtime reservation identity.
struct GalacticGoalIdentity {
    tactical::PlayerId player{};
    std::uint64_t goal{}, potential_plan{}, running_plan{}, instance{}, target{};
    std::string definition;
    bool operator==(const GalacticGoalIdentity&) const = default;
};
class GoalStateContributor;

// FD-73: only service deadlines use this clock. World, scoring and Lua clocks
// keep advancing during a hold; enabled AI time preserves remaining phases.
struct StaggeredServiceClock {
    std::uint64_t paused_frames{};
    std::optional<std::uint64_t> held_since;

    void visit(std::uint64_t frame, bool suspended) {
        if (suspended) {
            if (!held_since) held_since = frame;
        } else if (held_since) {
            paused_frames += frame - *held_since;
            held_since.reset();
        }
    }
    [[nodiscard]] std::uint64_t enabled_frame(std::uint64_t frame) const noexcept {
        return frame - paused_frames - (held_since && frame >= *held_since ? frame - *held_since : 0);
    }
};

// Orders native record rows like std::less on the (owner, kind, identity) tuple; it also compares a row key
// with an (owner, kind) prefix so range lookups never build a key with an empty identity.
struct NativeRecordPrefix { std::uint32_t owner; std::uint32_t kind; };
struct NativeRecordLess {
    using is_transparent = void;
    using Key = std::tuple<std::uint32_t, std::uint32_t, core::RecordDigest::Bytes>;
    static std::pair<std::uint32_t, std::uint32_t> head(const Key& key) { return {std::get<0>(key), std::get<1>(key)}; }
    static std::pair<std::uint32_t, std::uint32_t> head(const NativeRecordPrefix& prefix) { return {prefix.owner, prefix.kind}; }
    bool operator()(const Key& a, const Key& b) const { return a < b; }
    bool operator()(const Key& a, const NativeRecordPrefix& b) const { return head(a) < head(b); }
    bool operator()(const NativeRecordPrefix& a, const Key& b) const { return head(a) < head(b); }
};

class Engine {
public:
    Engine(std::shared_ptr<Host> host, AiData data, std::vector<PlanDef> plans);
    // WGS-66: exact native child continuation; immutable content is rebuilt,
    // references are validated, and restore never services gameplay.
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> save_tactical_native(
        std::string_view content_identity, authoritative::ScriptScheduler&) const;
    [[nodiscard]] core::Result<void> restore_tactical_native(std::span<const std::uint8_t>,
        std::string_view content_identity, authoritative::ScriptScheduler&);
    // Continuation mutations, including synchronized draws, invalidate goals.
    // A player turn stages the bump in its effects (the commit applies the count); anything else bumps now.
    void mark_native_state_changed() noexcept {
        if (auto* effects = staged()) { ++effects->revisions; return; }
        if (++native_revision_ == 0) std::terminate();
        native_record_header_dirty_ = true;
    }
    // The revision readers see: the committed counter plus the bumps staged by player turns that have not
    // committed (the passes are still sequential, so a later read must see an earlier turn's change).
    // Serial only: it reads every player's staged effects without a lock, so no partitioned phase may call it.
    [[nodiscard]] std::uint64_t native_state_revision() const noexcept {
        std::uint64_t staged_total{};
        for (const auto& player : players_) staged_total += player.effects.revisions;
        return native_revision_ + staged_total;
    }
    // Nested mutations publish their record deltas at the outer owner boundary. A player turn runs inside
    // the outer scope of the barrier and never publishes, so its scopes count in its own effects.
    class NativeRecordWrite {
    public:
        explicit NativeRecordWrite(Engine& owner) noexcept : owner_(owner), staged_(owner.staged()) {
            if (staged_) ++staged_->scope_depth; else ++owner_.native_record_depth_;
        }
        NativeRecordWrite(const NativeRecordWrite&) = delete;
        ~NativeRecordWrite() {
            if (staged_) { --staged_->scope_depth; return; }
            if (--owner_.native_record_depth_ == 0 && owner_.native_records_enabled_) owner_.refresh_native_records();
        }
    private:
        Engine& owner_;
        PlayerEffects* staged_;
    };
    [[nodiscard]] NativeRecordWrite native_record_scope() { return NativeRecordWrite(*this); }
    // Semantic writers identify the records they can change. A group is used
    // when the operation owns a whole ordered list or visits an entire map.
    static constexpr std::uint32_t native_all_players = UINT32_MAX;
    void native_records_changed(std::uint32_t owner, std::initializer_list<std::uint32_t> kinds);
    void native_record_changed(std::uint32_t owner, std::uint32_t kind, std::uint64_t key);
    void native_records_replaced() noexcept { native_records_all_dirty_ = true; }
    [[nodiscard]] const std::array<std::uint64_t, 34>& native_record_work() const noexcept { return native_record_work_; }
    [[nodiscard]] std::uint64_t native_record_inspections() const noexcept { return native_record_inspections_; }

    // WGA-01..05: the same native engine state, with manager-owned due tests.
    [[nodiscard]] core::Result<void> begin_galactic_barrier(const World&, galactic_ai::Clocks,
        authoritative::ScriptScheduler&, std::uint64_t& sequence, std::span<const galactic_ai::PlayerSelection>,
        const sim::PartitionExecutor* executor = nullptr);
    [[nodiscard]] core::Result<void> apply_galactic_policy(tactical::PlayerId, const galactic_ai::TemplatePolicy&);
    void set_galactic_funding(std::shared_ptr<galactic_ai::Funding>);
    void set_galactic_selection(GalacticSelectionProvider provider) { galactic_selection_ = std::move(provider); }
    void set_galactic_stage_catalog(std::shared_ptr<const galactic_goals::MaterializedPlanCatalog> catalog) { galactic_stage_catalog_ = std::move(catalog); }
    using GalacticStageUsability = std::function<core::Result<bool>(tactical::PlayerId, const sim::galactic::GalacticSnapshot&, sim::galactic::PlanetId)>;
    void set_galactic_stage_usability(GalacticStageUsability reader) { galactic_stage_usability_ = std::move(reader); }
    using GalacticStageObjectReader = std::function<core::Result<bool>(tactical::PlayerId, const sim::galactic::GalacticSnapshot&, sim::EntityId)>;
    void set_galactic_stage_garrison(GalacticStageObjectReader reader) { galactic_stage_garrison_ = std::move(reader); }
    void set_galactic_stage_combat_skip(GalacticStageObjectReader reader) { galactic_stage_combat_skip_ = std::move(reader); }
    using GalacticStageSpeed = std::function<core::Result<Real>(tactical::PlayerId, const sim::galactic::GalacticSnapshot&, tactical::TypeId)>;
    void set_galactic_stage_speed(GalacticStageSpeed reader) { galactic_stage_speed_ = std::move(reader); }
    [[nodiscard]] core::Result<std::pair<Real, bool>> galactic_freestore_selection_facts(
        tactical::PlayerId, sim::galactic::PlanetId target, sim::EntityId object, const PlanDef&) const;
    // One (player, target, plan) read of many actors: the goal type, stage list and stage positions do not depend
    // on the actor, so the batch resolves them once, at the first actor that reaches them. Actor predicates,
    // usability and route arithmetic stay live per call. The plan must outlive the returned reader.
    using GalacticFreestoreActorFacts = std::function<core::Result<std::pair<Real, bool>>(sim::EntityId)>;
    [[nodiscard]] GalacticFreestoreActorFacts galactic_freestore_selection_batch(
        tactical::PlayerId, sim::galactic::PlanetId target, const PlanDef&) const;
    void set_galactic_build_observer(GalacticBuildObserver observer) { galactic_build_observer_ = std::move(observer); }
    // Allocation is separate from force selection, funding and plan execution.
    // Positive authored desire chooses a template-enabled plan with the shared
    // synchronized weighted draw, then retains an unselected potential plan.
    [[nodiscard]] core::Result<std::optional<GalacticGoalIdentity>> allocate_galactic_goal(
        tactical::PlayerId, std::size_t function, std::uint64_t target);
    [[nodiscard]] std::optional<GalacticGoalIdentity> galactic_goal(tactical::PlayerId, std::uint64_t goal) const;
    [[nodiscard]] std::optional<std::uint64_t> galactic_goal_for_plan(tactical::PlayerId, std::uint64_t plan) const;
    [[nodiscard]] bool galactic_reservation_valid(tactical::PlayerId, sim::EntityId, std::uint64_t potential_plan) const;
    [[nodiscard]] core::Result<std::map<sim::EntityId, std::uint64_t>> galactic_execution_reservations(tactical::PlayerId) const;
    [[nodiscard]] core::Result<void> reserve_galactic_object(tactical::PlayerId, std::uint64_t goal, sim::EntityId);
    [[nodiscard]] core::Result<void> release_galactic_goal(tactical::PlayerId, std::uint64_t goal);
    // Owner-only budget rows feed only that player's BUDGETFRACTIONTOBUILD queries. The producer still
    // validates every player's associations at the original read point until its barrier-start freeze.
    class BuildBudgetReads {
    public:
        // A boundary refresh serves mixed-player readers and must publish every owner.
        BuildBudgetReads() noexcept;
        BuildBudgetReads(const Engine&, tactical::PlayerId) noexcept;
        BuildBudgetReads(const BuildBudgetReads&) = delete;
        BuildBudgetReads& operator=(const BuildBudgetReads&) = delete;
        ~BuildBudgetReads();
    private:
        const Engine* previous_engine_{};
        tactical::PlayerId previous_player_{};
    };
    [[nodiscard]] tactical::PlayerId build_budget_reader() const noexcept {
        return build_reader_engine_ == this ? build_reader_player_ : tactical::PlayerId{};
    }
    using GalacticBuildPublication = std::shared_ptr<const std::vector<GalacticBudgetBuild>>;
    [[nodiscard]] core::Result<GalacticBuildPublication> galactic_build_budgets(
        const galactic_ai::NativeActions&, const GalacticFunding&) const;
    [[nodiscard]] core::Result<math::Fixed> galactic_build_remaining_cost(tactical::PlayerId, const BuildTask&) const;
    // S9a: a player turn counts its task changes in its effects (the commit applies them) and keeps its own
    // publication memo; the committed memo and counter are only written outside a turn.
    void mark_galactic_tasks_changed() noexcept {
        if (auto* turn = staged()) { ++turn->tasks_revision; turn->build_cache.reset(); return; }
        if (++galactic_tasks_revision_ == 0) std::terminate();
        galactic_build_cache_.reset();
    }
    // Serial only, like native_state_revision().
    [[nodiscard]] std::uint64_t galactic_build_tasks_revision() const noexcept {
        std::uint64_t staged_total{};
        for (const auto& player : players_) staged_total += player.effects.tasks_revision;
        return galactic_tasks_revision_ + staged_total;
    }
    [[nodiscard]] std::optional<std::uint64_t> galactic_potential_for_goal(tactical::PlayerId, std::uint64_t goal) const;
    [[nodiscard]] std::vector<GalacticGoalIdentity> galactic_goals(tactical::PlayerId) const;
    // Observer output only: the player's live goals with the goal function name, current desire (shortest decimal text),
    // whether the goal is active, and the potential or running plan's definition. Read between ticks; never saved or hashed.
    struct GalacticGoalObservation {
        std::string function, desire, plan;
        std::uint64_t goal{}, target{}, running_plan{};
        bool active{}, planned{};
    };
    [[nodiscard]] std::vector<GalacticGoalObservation> galactic_goal_observations(tactical::PlayerId) const;
    [[nodiscard]] core::Result<void> service_galactic_system(tactical::PlayerId, galactic_ai::Service,
        authoritative::ScriptScheduler&, std::uint64_t& sequence);
    [[nodiscard]] core::Result<void> commit_galactic_events(authoritative::ScriptScheduler&, std::uint64_t& sequence);
    // S9a: every galactic service call runs as a player turn that stages its writes to engine-wide state in the
    // player's effects. While deferral is on (the barrier owner turns it on after begin_galactic_barrier), the
    // staged effects of all players wait for commit_galactic_events and apply there in ascending player order,
    // so no player's pass sees another's appends (GFS-A20); otherwise each call commits its own turn on return.
    void defer_galactic_effects(bool deferred) noexcept { galactic_deferred_ = deferred; }
    // GFS-A22 semantic switch, independent of whether the caller dispatches the chains concurrently.
    // Both calls belong to the serial boundary; an active turn sees only its own live row and frozen foreign rows.
    void freeze_galactic_decision_reads();
    void finish_galactic_decision_reads() noexcept { decision_reads_frozen_ = false; decision_players_.clear(); }
    // Keeps the owner's frozen foreign view across native turns, attachment boundaries and Lua work.
    [[nodiscard]] core::Result<std::shared_ptr<void>> galactic_decision_scope(tactical::PlayerId);
    // S9a: the player whose Lua chain a galactic script instance belongs to (a freestore instance carries its
    // player, a plan instance its slot's player), or 0 for an instance no player owns.
    [[nodiscard]] std::uint64_t galactic_chain_owner(std::uint64_t instance) const;

    // Serial, before the script service: perception, goals, planning and execution of every AI
    // player in ascending player ID (GS-01). Engine events for the scripts are submitted to
    // `scripts` with keys from `sequence`. WTA-01/02: on a player's execution frame,
    // `execution_hook` (the freestore's service tests) runs after its goals and planning and
    // before its build tasks.
    // What an execution hook writes (S-T3b): freestore scheduler events keyed from `sequence`, and journal
    // rows. A deferred turn hands the hook a local counter and stages both; the commit numbers the events
    // from the engine's live counter and submits them in the order the hook did.
    struct ExecutionEffects {
        std::uint64_t& sequence;
        std::function<core::Result<void>(authoritative::ScriptEvent)> submit;
        std::function<void(PlanEvent)> journal;
    };
    using ExecutionHook = std::function<core::Result<void>(tactical::PlayerId, ExecutionEffects&)>;
    [[nodiscard]] core::Result<void> before_service(const World& world,
        authoritative::ScriptScheduler& scripts, std::uint64_t& sequence,
        const sim::PartitionExecutor* executor = nullptr, const ExecutionHook& execution_hook = {});
    // Serial, after the script service: takes the plan scripts' engine requests out of the
    // report and adds the engine's unit orders.
    [[nodiscard]] core::Result<void> after_service(authoritative::ServiceReport& report);

    // ---- Reads for the bindings (const; no engine step runs during the script service) ----
    // #1702: the Script_* evaluator modules of the mounted corpus; null until attached.
    void attach_script_evaluators(std::shared_ptr<tactical_ai_detail::ScriptEvaluators> evaluators) noexcept {
        script_evaluators_ = std::move(evaluators);
    }
    [[nodiscard]] tactical_ai_detail::ScriptEvaluators* script_evaluators() const noexcept { return script_evaluators_.get(); }
    [[nodiscard]] std::optional<Real> evaluate(std::string_view equation, tactical::PlayerId player,
        const Target* target) const;
    // FT-05/06: selected manager owns object liveness and positions.
    [[nodiscard]] const WorldView* completed_view() const;
    [[nodiscard]] const Target* target(std::uint64_t id) const;
    [[nodiscard]] const Target* target_of_object(sim::EntityId object) const;
    [[nodiscard]] const TaskForce* taskforce(std::uint64_t id) const;
    [[nodiscard]] const Plan* plan_of_instance(std::uint64_t instance) const;
    [[nodiscard]] const Plan* plan(std::uint64_t id) const;
    [[nodiscard]] const Block* block(std::uint64_t id) const;
    [[nodiscard]] std::optional<galactic_ai::MovementReach> galactic_movement_reach(std::uint64_t plan) const;
    [[nodiscard]] std::vector<std::uint64_t> galactic_movement_requests() const;
    [[nodiscard]] core::Result<void> validate_galactic_movement(std::uint64_t request,
        std::uint64_t plan, std::uint64_t taskforce, std::uint64_t block, bool registered) const;
    // WGA-202/204: qualified retained listener delivery, without parent service.
    [[nodiscard]] core::Result<void> deliver_galactic_space_conflict(std::uint64_t request,
        std::uint64_t plan, std::uint64_t taskforce, std::uint64_t block, bool begin, authoritative::ScriptScheduler&);

    [[nodiscard]] const ThreatGrid& grid() const noexcept { return grid_; }
    [[nodiscard]] bool reserved(sim::EntityId object) const;
    // DT-03: the attacker that threatens the objects most.
    [[nodiscard]] sim::EntityId deadly_enemy(const std::vector<sim::EntityId>& objects) const;
    [[nodiscard]] const std::vector<PlanRecord>& records() const noexcept { return records_; }
    [[nodiscard]] const std::vector<TickCost>& costs() const noexcept { return costs_; }
    [[nodiscard]] std::uint64_t records_dropped() const noexcept { return records_dropped_; }
    // Observer output only (#3944): galactic refusals (goal allocation and selection outcomes, unavailable budget
    // equations) summed over the players, by player and "goal:"/"alloc:<goal>:<reason>"/"budget:<category>" name.
    // Empty unless gc_profile::enabled; never read by a decision, saved or hashed.
    [[nodiscard]] std::map<std::pair<tactical::PlayerId, std::string>, std::uint64_t> galactic_refusals() const;
    [[nodiscard]] std::uint64_t costs_dropped() const noexcept { return costs_dropped_; }
    [[nodiscard]] std::int64_t frame() const noexcept { return frame_; }
    [[nodiscard]] const AiData& data() const noexcept { return data_; }
    [[nodiscard]] const std::vector<PlanDef>& plans() const noexcept { return plans_; }
    [[nodiscard]] const std::vector<std::uint64_t>* target_list(tactical::PlayerId player) const;
    // WGA-452: exact category membership of the current manager's active set.
    [[nodiscard]] std::optional<std::uint64_t> active_goal_count(tactical::PlayerId player, std::uint64_t category) const;
    [[nodiscard]] std::optional<Real> global_value(const std::string& name) const;
    // A unit's type as the goal system counts it (a squadron container's squadron type).
    [[nodiscard]] bool in_freestore(const ViewUnit& unit, tactical::PlayerId player) const;
    // FH-11, AI-08: a freestore unit the freestore's unit service calls On_Unit_Service for: one
    // that no goal has reserved for its plan (#782).
    [[nodiscard]] bool unit_service(const ViewUnit& unit, tactical::PlayerId player) const;
    // WTA-02 (#783): whether the player's execution service, which runs its freestore's service
    // tests, falls on `frame`: every 0.1 s from its first service. A player the goal system does
    // not run (or before its first service) is tested every frame.
    [[nodiscard]] bool execution_due(tactical::PlayerId player, std::int64_t frame) const;
    // WTA-02: whether the goal system runs the player, so its execution service runs the
    // freestore's tests (before_service's execution hook).
    [[nodiscard]] bool runs_player(tactical::PlayerId player) const;
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
    struct InputCounts {
        std::uint64_t view_rows{}, producer_entities{}, producer_prepasses{}, pad_rows{}, pad_prepasses{}, pad_queries{}, late_prepasses{};
    };
    using InputObserver = void (*)(std::uint64_t, const InputCounts&) noexcept;
    static void observe_inputs(InputObserver) noexcept;
    [[nodiscard]] InputCounts input_counts() const noexcept { return input_counts_; }
    // S-T3b: what the player turns staged since the engine started, and the reads they made of other
    // players' uncommitted effects. Diagnostics only: never saved, hashed or read by a decision.
    [[nodiscard]] const EffectCounts& effect_counts() const noexcept { return effect_totals_; }
    // Optional write-only profiling sink, called once per barrier with that barrier's counts.
    using EffectObserver = void (*)(std::uint64_t tick, const EffectCounts&) noexcept;
    static void observe_effects(EffectObserver) noexcept;
    // The wallet change of turns that have not committed yet (raw fixed units); stacked onto Host::credits.
    [[nodiscard]] std::int64_t uncommitted_credits(tactical::PlayerId) const noexcept;

    void memory_inventory(diagnostics::GcMemory& m) const {
        m.row("gc.ai.engine", players_, plans_, records_, costs_, targets_, threats_, producers_, native_configuration_, native_record_rows_);
        for (const auto& p : players_) {
            const auto prefix = "gc.ai.player" + std::to_string(p.player);
            m.row(prefix + ".goals", p.proposed, p.active, p.targets, p.functions);
            m.row(prefix + ".execution", p.running, p.taskforces, p.blocks, p.plan_of_instance, p.positioning, p.tasks,
                p.reserved, p.assigned, p.reserved_pool, p.reserved_pool_tokens, p.reserved_credits);
            m.row(prefix + ".learning", p.activations, p.goal_outcomes, p.plan_outcomes, p.plan_success,
                p.goal_success, p.unit_survival, p.target_goal_success, p.target_plan_success);
        }
        if (scripts_) m.rows["lua.ai_vm_lower_bound"] += scripts_->diagnostic_lua_bytes();
    }

private:
    friend struct EngineContractAccess;
    friend class TacticalWorld;
    friend class GoalStateContributor;
    friend struct GoalStateCodec;
    void report_inputs() const noexcept;
    void report_effects() const noexcept;
    using PadReservation = ai::PadReservation;

    // ---- Player effects (S-T3b) ----
    // One player's turn: its writes to engine-wide state stage in player.effects while the scope is
    // bound to the running thread (a player task of S-T4 binds its own). Only the staggered schedule's
    // tactical players defer: the faithful schedule attaches plans inside the pass and galactic services
    // are driven one at a time, so they keep writing directly.
    class TurnScope {
    public:
        // The tactical default: only the staggered schedule's tactical players stage. `stage` decides it outright.
        TurnScope(Engine& engine, PlayerAi& player) noexcept;
        TurnScope(Engine& engine, PlayerAi& player, bool stage) noexcept;
        TurnScope(const TurnScope&) = delete;
        TurnScope& operator=(const TurnScope&) = delete;
        ~TurnScope();
    private:
        const Engine* previous_engine_;
        PlayerEffects* previous_effects_;
    };
    // The serial boundary inside a galactic turn (plan attachment creates scheduler instances): the running
    // thread's turn is unbound for the scope, so the code inside writes directly, then the turn is restored.
    class BoundaryScope {
    public:
        BoundaryScope() noexcept;
        BoundaryScope(const BoundaryScope&) = delete;
        BoundaryScope& operator=(const BoundaryScope&) = delete;
        ~BoundaryScope();
    private:
        PlayerEffects* previous_effects_;
    };
    class DecisionScope {
    public:
        DecisionScope(Engine&, PlayerAi&) noexcept;
        ~DecisionScope();
        DecisionScope(const DecisionScope&) = delete;
        DecisionScope& operator=(const DecisionScope&) = delete;
    private:
        const Engine* previous_engine_;
        PlayerEffects* previous_effects_;
        TurnScope turn_;
    };
    [[nodiscard]] PlayerEffects* decision_effects() const noexcept;
    // Attaches the plans the player's goal service queued as intents, at the boundary after the service.
    [[nodiscard]] core::Result<void> settle_galactic_attaches(PlayerAi& player, authoritative::ScriptScheduler& scripts);
    // The running turn's effects, or null when the caller writes directly (serial phases, other threads).
    [[nodiscard]] PlayerEffects* staged() const noexcept;
    // Applies every player's staged effects in ascending player order (the serial boundary after the loop).
    [[nodiscard]] core::Result<void> commit_effects(authoritative::ScriptScheduler& scripts);
    // Drops a player's staged turn without applying it. The revision counts it staged still move the
    // committed counters, so a revision a reader has seen never goes backwards and never names two states.
    void discard_effects(PlayerAi& player) noexcept;
    void queue_event(authoritative::ScriptEvent event);   // for the barrier's script events
    void queue_order(authoritative::ScriptCommand order); // for the next tick's orders
    void stage_removal(authoritative::ScriptScheduler& scripts, std::uint64_t instance);
    void journal_plan(PlanEvent row, bool raw = false) const;
    void journal_service(const AiServiceEvent& row);
    // A pad-count perception read: counts the late input and asks for the open-pad prepass, staged in a turn.
    void note_pad_query(tactical::PlayerId player, bool open) const;
    [[nodiscard]] TickCost& work() noexcept { auto* effects = staged(); return effects ? effects->work : work_; }
    // Stacked pad claims: the committed claim under the staged ones of earlier turns and the own turn.
    [[nodiscard]] const PadReservation* pad_claim(sim::EntityId pad) const;
    void set_pad_claim(sim::EntityId pad, const PadReservation& claim);
    void release_pad_claim(sim::EntityId pad);
    void release_pad_claims(tactical::PlayerId player, std::uint64_t goal);
    [[nodiscard]] math::Fixed stacked_credits(tactical::PlayerId player) const;
    // Typed operations on a row another player owns: applied in place and counted (S-T4 stages them).
    void operate(const CrossOwnerOp& op);
    void apply_operation(const CrossOwnerOp& op);
    // Whether the identity belongs to a row another player owns, as seen from the running turn.
    [[nodiscard]] bool foreign_row(RuntimeTable table, std::uint64_t id) const;
    void retire_runtime(RuntimeTable table, std::uint64_t id);
    void note_handle(tactical::PlayerId owner) const noexcept;
    static thread_local const Engine* build_reader_engine_;
    static thread_local tactical::PlayerId build_reader_player_;
    static constexpr std::uint32_t touch_only_kind = 0xFFFFFFFFU; // marks the owner's group dirty, sets no bit
    void apply_native_records_changed(std::uint32_t owner, std::uint32_t kind);
    void apply_native_record_changed(std::uint32_t owner, std::uint32_t kind, std::uint64_t key);
    EffectCounts effect_totals_;
    EffectCounts effect_tick_;
    void prepare_producers(const sim::PartitionExecutor* executor = nullptr) const;
    // Perception.
    void prepare_station_levels(const sim::PartitionExecutor*);
    struct PadInput {
        sim::EntityId object{};
        const ViewUnit* unit{};
        std::array<const ViewUnit*, 2> children{};
        struct Buyer {
            tactical::PlayerId player{};
            const tactical::StationMenu* menu{};
            bool allied{}, can_build{};
            std::optional<bool> open;
        };
        std::vector<Buyer> buyers;
    };
    [[nodiscard]] PadInput pad_input(sim::EntityId, const tactical::PadState&, bool prepare_open = false) const;
    void prepare_pad_inputs(const sim::PartitionExecutor*);
    std::optional<std::vector<PadInput>> pad_inputs_;
    std::set<tactical::PlayerId> pad_open_pending_;
    mutable std::set<tactical::PlayerId> pad_open_requested_;
    [[nodiscard]] Real station_base_level(tactical::PlayerId) const;
    struct StationLevel { tactical::PlayerId owner{}; Real level{}; };
    // Derived only for the current ordered service barrier; never persisted.
    std::optional<std::vector<StationLevel>> station_levels_;
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
    [[nodiscard]] bool select_galactic_units(PlayerAi&, const Goal&, PotentialPlan&);
    [[nodiscard]] std::shared_ptr<const GalacticSelectionInput> galactic_selection(const PlayerAi&, const Goal&, const PlanDef&) const;
    [[nodiscard]] galactic_goals::ProductionEstimate galactic_estimate(const PlayerAi&, const Goal&, const PotentialPlan&, bool linear) const;
    [[nodiscard]] sim::galactic::funding::GoalRequest galactic_funding_request(const PlayerAi&, const Goal&) const;
    [[nodiscard]] bool galactic_fundable(const PlayerAi&, const Goal&, bool activation) const;
    void service_galactic_execution(PlayerAi&);
    void service_galactic_assembly(PlayerAi&);
    void begin_galactic_maintenance(PlayerAi&);
    void salvage_galactic_goal(PlayerAi&, std::uint64_t goal);
    [[nodiscard]] bool production_time_allowed(const PlayerAi& player, const Goal& goal) const;
    [[nodiscard]] bool test_valid(PlayerAi& player, Goal& goal);
    [[nodiscard]] bool test_target_contrast(PlayerAi& player, Goal& goal);
    void reserve(PlayerAi& player, Goal& goal);
    void release(PlayerAi& player, Goal& goal, bool refund = true);
    void refund_funds(PlayerAi& player, std::uint64_t goal);
    void change_wallet(tactical::PlayerId player, Real amount);
    void observe_purchases(PlayerAi& player, const World& world);
    [[nodiscard]] std::optional<Real> pad_count(tactical::PlayerId player,
        const std::vector<std::string>& types, bool open) const;
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
    void service_execution(PlayerAi& player, bool direct_pad_only = false);
    void service_reinforcements(PlayerAi& player);
    void service_blocks();
    void service_taskforce_events();
    void track_damage(const World& world);
    // AI-32/WLD-10: ObjectScript_PointDefense, once a second per AI-owned Crusader.
    void service_point_defence();
    void service_corrupt_systems();
    // WTA-26 to WTA-28: attack positioning of the flagged units.
    void service_attack_positioning();
    [[nodiscard]] math::Vec3 ideal_attack_position(const ViewUnit& target, Real half, tactical::PlayerId player) const;
    void emit_call(const Plan& plan, const TaskForce& taskforce, std::string_view event, authoritative::ValueList arguments);
    void emit_thread_event(const Plan& plan, const TaskForce& taskforce, std::string_view event,
        authoritative::ValueList arguments);
    void order_block(Block& block, const TaskForce& taskforce, const Plan& plan);
    [[nodiscard]] std::optional<math::Vec3> ambush_point(const Block& block, const Plan& plan) const;
    void order_ambush(Block& block, const TaskForce& taskforce, const Plan& plan, bool fresh);
    void remove_member(TaskForce& taskforce, sim::EntityId unit);
    void return_galactic_actor(const TaskForce&, sim::EntityId);
    [[nodiscard]] bool defines_function(std::uint64_t instance, const std::string& name) const;
    void record(const Plan* plan, tactical::PlayerId player, std::string event, std::string detail = {});
    void publish_record(PlanRecord line);
    [[nodiscard]] PlayerAi* player_ai(tactical::PlayerId player);
    [[nodiscard]] const PlayerAi* player_ai(tactical::PlayerId player) const;
    // GFS-A21/A24: every synchronized AI draw names its owner explicitly, in either mode.
    // No "active player" member exists to race in a parallel barrier.
    [[nodiscard]] AiRandom& stream(PlayerAi& player) {
        native_records_changed(player.player, {1});
        return player.sync;
    }
    // Identity allocation; the tactical engine keeps its engine-wide counters. A galactic range that is
    // exhausted refuses deterministically (nullopt) instead of wrapping into another player's range.
    [[nodiscard]] std::optional<std::uint64_t> allocate_target_id(PlayerAi& player);
    [[nodiscard]] std::optional<std::uint64_t> allocate_taskforce_id(PlayerAi& player);
    [[nodiscard]] std::optional<std::uint64_t> allocate_plan_id(PlayerAi& player);
    [[nodiscard]] std::optional<std::uint64_t> allocate_instance_id(PlayerAi& player);
    // Whether a galactic identity lies inside its owner's allocated range (the archive and request checks).
    [[nodiscard]] bool galactic_target_issued(std::uint64_t id) const;
    [[nodiscard]] bool galactic_taskforce_issued(std::uint64_t id) const;
    [[nodiscard]] bool galactic_plan_issued(std::uint64_t id) const;
    [[nodiscard]] bool galactic_instance_issued(std::uint64_t instance) const;
    [[nodiscard]] const PlayerAi* player_of_slot(std::uint32_t slot) const;
    [[nodiscard]] Goal* find_goal(PlayerAi& player, std::uint64_t id);
    [[nodiscard]] std::string target_name(const Target* target) const;
    [[nodiscard]] Real power(const ViewUnit& unit) const;
    void issue_move(const Plan& plan, const ViewUnit& unit, const math::Vec3& destination);
    void issue_attack(const Plan& plan, const ViewUnit& unit, sim::EntityId target);
    // FH-40: an attack-move or guard of `target`, or of `destination` when it is zero.
    void issue_order(const Plan& plan, const ViewUnit& unit, std::string_view verb, sim::EntityId target,
        const math::Vec3& destination);

    using NativeRecordKey = std::tuple<std::uint32_t, std::uint32_t, core::RecordDigest::Bytes>;
    using NativeRecordRows = std::map<NativeRecordKey, core::RecordDigest::Bytes, NativeRecordLess>;
    void enable_native_records();
    void refresh_native_records() noexcept;
    [[nodiscard]] bool native_record_selected(const NativeRecordKey&) const;
    [[nodiscard]] bool native_record_kind_selected(std::uint32_t) const;
    [[nodiscard]] bool native_record_owner_kind_selected(std::uint32_t, std::uint32_t) const;
    core::RecordDigest native_records_;
    NativeRecordRows native_record_rows_;
    std::optional<core::Diagnostic> native_record_failure_;
    std::size_t native_record_depth_{};
    bool native_records_enabled_{};
    bool native_records_all_dirty_{};
    bool native_record_header_dirty_{};
    std::map<std::uint32_t, std::uint64_t> native_record_dirty_groups_;
    std::set<NativeRecordKey, std::less<>> native_record_dirty_keys_;
    std::array<std::uint64_t, 34> native_record_work_{};
    std::uint64_t native_record_inspections_{}; // candidate encodes + retained rows reconciled, never saved/hashed
    std::uint64_t native_revision_{1};
    std::uint64_t galactic_tasks_revision_{1}; // derived publication invalidation, not saved state
    mutable std::shared_ptr<const std::vector<GalacticBudgetBuild>> galactic_build_cache_;
    mutable std::array<std::uint64_t, 3> galactic_build_cache_key_{};
    mutable tactical::PlayerId galactic_build_cache_reader_{};
    std::shared_ptr<Host> host_;
    std::shared_ptr<tactical_ai_detail::ScriptEvaluators> script_evaluators_;
    mutable bool producers_prepared_{};
    mutable std::map<std::pair<tactical::PlayerId, tactical::TypeId>, std::vector<sim::EntityId>> producers_;
    mutable ProducerWork producer_work_{};
    mutable InputCounts input_counts_{};
    bool player_inputs_active_{};
    bool galactic_deferred_{}; // S9a: galactic turns' effects wait for commit_galactic_events
    const sim::PartitionExecutor* perception_executor_{};
    GalacticSelectionProvider galactic_selection_;
    std::shared_ptr<const galactic_goals::MaterializedPlanCatalog> galactic_stage_catalog_;
    GalacticStageUsability galactic_stage_usability_;
    GalacticStageObjectReader galactic_stage_garrison_, galactic_stage_combat_skip_;
    GalacticStageSpeed galactic_stage_speed_;
    GalacticBuildObserver galactic_build_observer_;
    std::map<sim::EntityId, PadReservation> pad_reservations_;
    AiData data_;
    std::vector<PlanDef> plans_;
    mutable std::string native_configuration_; // immutable closure, computed once
    ThreatGrid grid_;
    std::vector<PlayerAi> players_;
    bool decision_reads_frozen_{};
    std::map<tactical::PlayerId, std::shared_ptr<const PlayerAi>> decision_players_;
    // Incrementally published by serial mutations; adapters and bindings only read it. Never encoded.
    struct RuntimeIndex {
        std::map<std::uint64_t, tactical::PlayerId> plans, taskforces, blocks, instances;
    };
    RuntimeIndex runtime_index_;
    void rebuild_runtime_index();
    template<class T> static T* runtime_row(std::map<std::uint64_t, T>& rows, std::uint64_t id) {
        const auto found = rows.find(id); return found == rows.end() ? nullptr : &found->second;
    }
    // Serial adapters retain global foreign-handle reads and writes while rows stay player-owned.
    template<class T> class RuntimeLookup {
        Engine& engine_;
        std::map<std::uint64_t, T> PlayerAi::* member_;
        const std::map<std::uint64_t, tactical::PlayerId>& index_;
    public:
        struct Iterator {
            std::uint64_t id{};
            T* row{};
            struct Entry { std::uint64_t first; T& second; };
            struct Arrow { Entry entry; Entry* operator->() { return &entry; } };
            Arrow operator->() const { return {{id, *row}}; }
            bool operator==(const Iterator& other) const { return row == other.row; }
        };
        RuntimeLookup(Engine& engine, std::map<std::uint64_t, T> PlayerAi::* member,
            const std::map<std::uint64_t, tactical::PlayerId>& index) : engine_(engine), member_(member), index_(index) {}
        Iterator find(std::uint64_t id) const {
            const auto found = index_.find(id);
            if (found == index_.end()) return {};
            engine_.note_handle(found->second);
            const auto* owner = std::as_const(engine_).player_ai(found->second);
            if (!owner) return {};
            return {id, runtime_row(const_cast<PlayerAi*>(owner)->*member_, id)};
        }
        Iterator end() const { return {}; }
        bool contains(std::uint64_t id) const { return find(id) != end(); }
        T& at(std::uint64_t id) const {
            const auto found = find(id);
            if (found == end()) throw std::out_of_range("missing AI runtime identity");
            return found->second;
        }
    };
    [[nodiscard]] RuntimeLookup<TaskForce> all_taskforces() { return {*this, &PlayerAi::taskforces, runtime_index_.taskforces}; }
    [[nodiscard]] RuntimeLookup<Plan> all_plans() { return {*this, &PlayerAi::running, runtime_index_.plans}; }
    [[nodiscard]] RuntimeLookup<Block> all_blocks() { return {*this, &PlayerAi::blocks, runtime_index_.blocks}; }
    static void index_runtime(std::map<std::uint64_t, tactical::PlayerId>& index, std::uint64_t id, tactical::PlayerId owner) {
        if (!index.emplace(id, owner).second) throw std::logic_error("duplicate AI runtime identity");
    }
    [[nodiscard]] const Block* raw_block(std::uint64_t) const;
    [[nodiscard]] PlayerAi* positioning_owner(sim::EntityId);
    [[nodiscard]] Plan* mutable_plan_of_instance(std::uint64_t);
    [[nodiscard]] AttackPositioning* positioning(sim::EntityId);
    template<class T> auto ordered_runtime(std::map<std::uint64_t, T> PlayerAi::* member) {
        std::map<std::uint64_t, T*> result;
        auto* turn = staged();
        for (const auto& selected : players_) {
            auto* owner = player_ai(selected.player); if (!owner) continue;
            for (auto& [id, row] : owner->*member) {
                if (turn && selected.player != turn->owner) ++turn->reads.row_scans;
                if (!result.emplace(id, &row).second) throw std::logic_error("duplicate AI runtime identity");
            }
        }
        return result;
    }
    template<class T> auto ordered_runtime(std::map<std::uint64_t, T> PlayerAi::* member) const {
        std::map<std::uint64_t, const T*> result;
        auto* turn = staged();
        for (const auto& selected : players_) {
            const auto* owner = player_ai(selected.player); if (!owner) continue;
            for (const auto& [id, row] : owner->*member) {
                if (turn && selected.player != turn->owner) ++turn->reads.row_scans;
                if (!result.emplace(id, &row).second) throw std::logic_error("duplicate AI runtime identity");
            }
        }
        return result;
    }

    std::map<std::uint64_t, Target> targets_;
    // DT-01: per damaged object, the attackers' threat.
    std::map<sim::EntityId, std::map<sim::EntityId, Real>> threats_;
    // This tick's first hit on each object: (attacker, deliberate) (EX-44).
    std::map<sim::EntityId, std::pair<sim::EntityId, bool>> last_hits_;
    std::map<std::string, Real, std::less<>> globals_;
    std::vector<PlanRecord> records_;
    std::vector<TickCost> costs_;
    std::uint64_t records_dropped_{};
    std::uint64_t costs_dropped_{};
    // #957: counters of the barrier step in progress and the plans waiting to be attached.
    TickCost work_{};
    std::vector<PendingAttach> pending_attach_;
    // Pending script events of this barrier and the engine's own orders for the next tick.
    std::vector<authoritative::ScriptEvent> events_;
    std::vector<authoritative::ScriptCommand> orders_;
    std::uint64_t event_tick_{};
    std::uint64_t sequence_{}; // owned across before/after service
    authoritative::ScriptScheduler* scripts_{};
    AiRandom sync_;                  // retained archive field; do not draw here, use stream(player)
    std::uint32_t game_seed_{};
    std::int64_t frame_{};
    std::uint64_t next_target_id_{1};
    std::uint64_t next_taskforce_id_{1};
    std::uint64_t next_plan_id_{1};
    std::uint64_t next_instance_{first_plan_instance};
    std::uint64_t order_sequence_{};
    bool initialized_{};
    StaggeredServiceClock staggered_clock_;
    bool staggered_seeded_{};
};

// PL-10 to PL-12: a plan from the globals its definition load leaves: Category, TaskForce,
// IgnoreTarget, MagicPlan, AllowFreeStoreUnits, AllowEngagedUnits, PerFailureContrastAdjust,
// MinContrastScale, MaxContrastScale and RequiredCategories, in that order. `notes` collects
// what the host leaves out.
// `drop_unresolved_rows`: a roster row whose names all resolve to no type is removed at parse time, as the debug build does
// (RGC-62); the tactical host loads only part of the type set, so it keeps such rows.
[[nodiscard]] core::Result<PlanDef> build_plan(const Host& host, std::string name, std::string module,
    const authoritative::ValueList& globals, std::vector<std::string>& notes, bool drop_unresolved_rows = false);

} // namespace eawr::script::foc::ai
