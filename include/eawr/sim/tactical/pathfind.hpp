#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// FoC's space path finder and object tracking system (#71, docs/behaviour/space-movement.md
// AV-01 to AV-15): the weighted A* search a move runs once at its start, and the per-layer,
// time-windowed predictions of the other ships and static objects that its collision test
// queries. Pure functions of Q24 values: no clock, thread or host input.
namespace eawr::sim::tactical {

// SpaceCollisionType bits a query reports (research E71-19).
inline constexpr std::uint8_t collision_moving = 0x01;
inline constexpr std::uint8_t collision_static = 0x02;
inline constexpr std::uint8_t collision_all = 0x3f;

// One tracked object in one time window (AV-03): a straight move from `start` to `end` with a
// rectangular footprint of half extents x/y along `facing`. `reach` bounds the footprint
// (the larger half extent) for the rectangle prefilter.
struct TrackedLeaf {
    EntityId entity{};
    math::Vec2 start{};
    math::Vec2 end{};
    math::Vec2 facing{};
    math::Fixed x_extent{};
    math::Fixed y_extent{};
    math::Fixed reach{};
    std::uint8_t collision{};
    friend constexpr bool operator==(const TrackedLeaf&, const TrackedLeaf&) noexcept = default;
};

// A dynamic layer as a query sees it: window k covers frames [start_frame + k * interval,
// start_frame + (k + 1) * interval].
struct TrackingLayerView {
    std::uint64_t start_frame{};
    std::vector<std::vector<TrackedLeaf>> windows;
};

// The dynamic layers in the order of dynamic_layer_index, and the static layer's objects; a
// null view is an empty layer. The views outlive the queries that read them.
struct CollisionWorld {
    std::uint32_t interval{};
    std::array<const TrackingLayerView*, 4> layers{};
    const std::vector<TrackedLeaf>* statics{};
};

// capital 0, frigate 1, corvette 2, super capital 3; nullopt for none and the static layer.
[[nodiscard]] std::optional<std::size_t> dynamic_layer_index(SpaceLayer layer) noexcept;

// A linear collision query (AV-04): the searching ship's footprint moving from `start` at
// `start_frame` to `end` at `end_frame`, facing `facing`; `ignore` is the ship itself and
// `ignore_group` (ascending IDs) the members of a group move whose slots are being mapped (FM-05).
struct LinearQuery {
    math::Vec2 start{};
    math::Vec2 end{};
    math::Fixed start_frame{};
    math::Fixed end_frame{};
    math::Vec2 facing{};
    math::Fixed x_extent{};
    math::Fixed y_extent{};
    EntityId ignore{invalid_entity_id};
    std::span<const EntityId> ignore_group{};
};

// AV-03, AV-04: the collision bits of a query against one dynamic layer; the first window
// with a hit answers. Fails only on arithmetic overflow.
[[nodiscard]] core::Result<std::uint8_t> find_linear_collision(
    const TrackingLayerView& layer, std::uint32_t interval, const LinearQuery& query);

// WR-28: the same arrival sweep against the static layer, without path-search padding.
[[nodiscard]] core::Result<std::uint8_t> find_static_collision(
    const std::vector<TrackedLeaf>& statics, const LinearQuery& query);

// Find_Dual_Collision (FM-05, research E344-08): a query against the dynamic layer of `layer`
// and the static layer. Fails only on arithmetic overflow.
[[nodiscard]] core::Result<std::uint8_t> find_dual_collision(
    const CollisionWorld& world, SpaceLayer layer, const LinearQuery& query);

// A tracked object's predicted place at a frame (AV-02): a unit following a path is where the
// path puts it (and where it stops once the path is over), any other unit where it is now.
struct Prediction {
    math::Vec3 position{};
    math::Fixed yaw{};
};
[[nodiscard]] core::Result<Prediction> predict(
    const MotionState& state, std::uint64_t frame, math::Vec3 position, math::Fixed yaw);

// Get_Nearest_Open_Position as the planner calls it (AV-19, AV-20): the XY `destination` when
// the square of the unit's occupation radius fits there without hitting its own layer or the
// static layer at any frame from `tick` on, else the first open point of up to 40 rings around
// it, biased toward the unit at `position`; the destination itself when no ring has one.
// `ignore_group` (ascending IDs) is the formation slot mapping's ignore list (FM-05); the plain
// move passes none. Fails only on arithmetic overflow.
[[nodiscard]] core::Result<math::Vec2> nearest_open_position(const AvoidanceRules& rules, const Footprint& footprint,
    const CollisionWorld& world, EntityId entity, std::uint64_t tick, math::Vec2 position, math::Vec2 destination,
    std::span<const EntityId> ignore_group = {});

// What a PathSearchStats reports (PC-08): a whole search; a budgeted search that stopped at
// its budget (plan_space_move_within), whose result is dropped; one slice of a sliced search;
// or the slice in which a sliced search ended.
enum class PathSearchPart : std::uint8_t { whole, abandoned, slice, last_slice };

// The work of one plan_space_move or nearest_open_position call (#520), or of one slice of a
// SlicedPathSearch: what the path cost regression test pins and path_bench prints. Counting
// never changes a result.
struct PathSearchStats {
    EntityId entity{};
    bool slot{};                 // a nearest_open_position call (FM-05), not a path search
    PathSearchPart part{};
    std::uint32_t tries{};       // path finder tries (AV-14)
    std::uint64_t expansions{};  // children put on the open list (AV-10)
    std::uint64_t children{};    // children built, culled or not
    std::uint64_t queries{};     // collision queries, each against the unit's layer and the static layer
    std::uint64_t windows{};     // tracking windows scanned, the static layer's one included
    std::uint64_t leaves{};      // tracked leaves the rectangle prefilter looked at
    std::uint64_t narrow{};      // leaves past the prefilter: exact tests (AV-04)
    // Host clock ticks, only with a probe (set_path_search_probe): the whole call, its collision
    // queries, and its open list and cell table.
    std::uint64_t total_ticks{};
    std::uint64_t query_ticks{};
    std::uint64_t set_ticks{};
};

// Benchmark seam (#520, path_bench): a host clock and a sink for every call's stats. The probe
// is read on the searching threads, so it must be thread safe; nothing it returns reaches a
// result. Null (the default) reads no clock.
class PathSearchProbe {
public:
    virtual ~PathSearchProbe() = default;
    [[nodiscard]] virtual std::uint64_t now() noexcept = 0;
    virtual void searched(const PathSearchStats& stats) noexcept = 0;
};
void set_path_search_probe(PathSearchProbe* probe) noexcept;

// Plans a move with the path finder (AV-10 to AV-14): the unit `entity` of footprint
// `footprint`, with the movement limits `limits` (after the durability factor, MV-33) and the
// type's rate of turn in `limits`, starts at `tick` from the given position, yaw and speed.
// The target is first moved to its nearest open position (AV-19); the plan's target is that
// point. Kind none when the move is shorter than 40 units, a coordinate is out of range or
// every try fails.
// The search's form (#520, docs/behaviour/space-movement.md PC-06): bounded, the project rule
// the session plans with, or exact, FoC's tries as they are (the reference the path cost test
// measures the bounded search against).
enum class PathSearchMode : std::uint8_t { bounded, exact };

// `stats`, when given, receives the call's work counters (#520).
[[nodiscard]] core::Result<MotionState> plan_space_move(const MotionTable& table, const MotionProfile& limits,
    const Footprint& footprint, const CollisionWorld& world, EntityId entity, std::uint64_t tick,
    math::Vec3 position, math::Fixed yaw, math::Fixed speed, math::Vec3 target, PathSearchStats* stats = nullptr,
    PathSearchMode mode = PathSearchMode::bounded);

// PC-08 (#520): the bounded plan_space_move, given up once its expansions reach `budget`
// (checked between two parents): nullopt then, else the plan_space_move result.
[[nodiscard]] core::Result<std::optional<MotionState>> plan_space_move_within(const MotionTable& table,
    const MotionProfile& limits, const Footprint& footprint, const CollisionWorld& world, EntityId entity,
    std::uint64_t tick, math::Vec3 position, math::Fixed yaw, math::Fixed speed, math::Vec3 target, std::uint64_t budget,
    PathSearchStats* stats = nullptr);

// PC-08 (#520): the bounded plan_space_move run in slices over several ticks. It copies the
// unit's layer view and the static layer of `world` when it is made, and reads only those
// copies, so its result is plan_space_move's against the world as it was then, whenever and on
// whichever thread its slices run. `table` must outlive it. Nothing runs until run().
class SlicedPathSearch final {
public:
    SlicedPathSearch(const MotionTable& table, const MotionProfile& limits, const Footprint& footprint,
        const CollisionWorld& world, EntityId entity, std::uint64_t tick, math::Vec3 position, math::Fixed yaw,
        math::Fixed speed, math::Vec3 target);
    SlicedPathSearch(SlicedPathSearch&&) noexcept;
    SlicedPathSearch& operator=(SlicedPathSearch&&) noexcept;
    SlicedPathSearch(const SlicedPathSearch&) = delete;
    SlicedPathSearch& operator=(const SlicedPathSearch&) = delete;
    ~SlicedPathSearch();

    // Runs the search until it ends (true) or it has counted `expansions` more; `stats`, when
    // given, receives this slice's work.
    bool run(std::uint64_t expansions, PathSearchStats* stats = nullptr);
    [[nodiscard]] bool ended() const noexcept;
    // The plan, once ended.
    [[nodiscard]] core::Result<MotionState> result() const;
    // The expansions of every slice so far.
    [[nodiscard]] std::uint64_t expansions() const noexcept;
    // The memory it holds: the copied views and its search scratch.
    [[nodiscard]] std::size_t held_bytes() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace eawr::sim::tactical
