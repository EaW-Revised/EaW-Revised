#pragma once

#include "eawr/sim/tactical/pathfind.hpp"

#include "eawr/sim/math/math.hpp"
#include "../math/wide.hpp"
#include "motion_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace eawr::sim::tactical::pathfind_detail {

using math::Fixed;
using math::Vec2;
using math::Vec3;
using motion_detail::Calc;
using motion_detail::clamp180;
using motion_detail::heading;
using motion_detail::whole;

namespace wide = math::detail;

[[nodiscard]] constexpr Fixed ratio(const std::int64_t numerator, const std::int64_t denominator) noexcept {
    return Fixed::from_raw(numerator * Fixed::scale / denominator);
}

// Constants of the retail code itself (research E71-06, E71-09, E71-22), rounded once to Q24.
constexpr Fixed match_factor = ratio(99, 100);
constexpr Fixed end_factor = ratio(98, 100);
constexpr Fixed arc_distance_scale = whole(400);
constexpr Fixed half = ratio(1, 2);
constexpr Fixed signature_cells = ratio(3, 2); // 1.5 cells per forward step
constexpr Fixed min_move_length = whole(40);
constexpr Fixed brake_margin = ratio(1, 1000);  // end-of-move speed + 0.001
constexpr Fixed speed_settle = ratio(1, 1000);  // speed reduction loop tolerance
constexpr Fixed tenth = ratio(1, 10);
constexpr Fixed nine_tenths = ratio(9, 10);
constexpr Fixed probe_step = whole(10);
constexpr Fixed probe_limit = whole(180);
constexpr Fixed degrees_per_radian_turn = whole(360); // turns -> degrees
// SpaceStaticObstacleAvoidanceBonusDistance: no retail XML sets it; its default is 0.
constexpr Fixed static_bonus{};
// PC-06 (project, #520): the bounded search's cap on FoC's first try, the try from which its
// estimate is weighted (FoC's third) and that weight.
constexpr std::uint32_t bounded_first_expansions = 500;
constexpr std::uint32_t bounded_weighted_try = 2;
constexpr Fixed bounded_estimate_weight = ratio(21, 20);

// SpaceExpansionType, unaligned set (research E71-02).
enum Op : int {
    op_none = -1,
    op_forward = 0,
    op_match = 1,
    op_left = 2,
    op_right = 3,
    op_end = 4,
    op_full_speed = 5,
    op_slow_speed = 6,
    op_wait = 7,
    op_emergency_left = 8,
    op_emergency_right = 9,
};

[[nodiscard]] constexpr bool is_slow(const int op) noexcept { return op == op_slow_speed || op == op_wait; }
[[nodiscard]] constexpr bool is_wait(const int op) noexcept { return op == op_slow_speed || op == op_wait; }
[[nodiscard]] constexpr bool is_emergency(const int op) noexcept {
    return op == op_emergency_left || op == op_emergency_right;
}
[[nodiscard]] constexpr bool is_unbounded(const int op) noexcept { return op == op_end; }
// Should_Do_Cost_Culling: the forward, arc and end steps.
[[nodiscard]] constexpr bool culls(const int op) noexcept {
    return op == op_forward || op == op_left || op == op_right || op == op_end;
}

extern std::atomic<PathSearchProbe*> installed_probe;

// The counters of one call and the probe it reports to (#520).
struct Work {
    PathSearchStats stats;
    PathSearchProbe* probe{installed_probe.load(std::memory_order_acquire)};
    [[nodiscard]] std::uint64_t now() const noexcept { return probe != nullptr ? probe->now() : 0; }
};

// ---- Tracking: collision test (AV-03, AV-04; research E71-13, E71-14, E71-20, E71-21) ----

[[nodiscard]] inline Fixed lerp(Calc& calc, const Fixed a, const Fixed b, const Fixed t) {
    return calc.add(a, calc.mul(calc.sub(b, a), t));
}

[[nodiscard]] inline Vec2 lerp(Calc& calc, const Vec2 a, const Vec2 b, const Fixed t) {
    return {lerp(calc, a.x, b.x, t), lerp(calc, a.y, b.y, t)};
}

[[nodiscard]] inline Vec2 minus(Calc& calc, const Vec2 a, const Vec2 b) { return {calc.sub(a.x, b.x), calc.sub(a.y, b.y)}; }

// Point2::Normalize: a zero vector stays zero.
[[nodiscard]] inline Vec2 unit(Calc& calc, const Vec2 value) {
    if (value.x.raw() == 0 && value.y.raw() == 0) return {};
    const Fixed length = calc.length(value.x, value.y);
    if (length.raw() == 0) return {};
    return {calc.div(value.x, length), calc.div(value.y, length)};
}

[[nodiscard]] inline Fixed clamp01(const Fixed value) noexcept {
    return std::clamp(value, Fixed{}, whole(1));
}

// A footprint's reach along a direction: X extent along its facing, Y across, blended by
// the squared cosine.
[[nodiscard]] inline Fixed reach(Calc& calc, const Vec2 direction, const Vec2 facing, const Fixed x, const Fixed y) {
    const Fixed c = calc.abs(calc.dot(direction.x, direction.y, facing.x, facing.y));
    const Fixed c2 = calc.mul(c, c);
    return calc.add(calc.mul(c2, x), calc.mul(calc.sub(whole(1), c2), y));
}

// b^2 - 4ac > 0, = 0 or < 0, exactly: the raw products in 128 bits (#520).
[[nodiscard]] inline int discriminant_sign(Calc& calc, const Fixed a, const Fixed b, const Fixed c) {
    const Fixed four_a = calc.mul(whole(4), a);
    std::uint64_t square_high = 0;
    std::uint64_t square_low = 0;
    wide::multiply_64(wide::unsigned_magnitude(b.raw()), wide::unsigned_magnitude(b.raw()), square_high, square_low);
    std::uint64_t term_high = 0;
    std::uint64_t term_low = 0;
    wide::multiply_64(wide::unsigned_magnitude(four_a.raw()), wide::unsigned_magnitude(c.raw()), term_high, term_low);
    const bool term_zero = term_high == 0 && term_low == 0;
    if (term_zero || (four_a.raw() < 0) != (c.raw() < 0)) {
        // 4ac <= 0: b^2 - 4ac = b^2 + |4ac|.
        return square_high == 0 && square_low == 0 && term_zero ? 0 : 1;
    }
    if (square_high != term_high) return square_high > term_high ? 1 : -1;
    if (square_low != term_low) return square_low > term_low ? 1 : -1;
    return 0;
}

// A leaf's rectangle for the tree's prefilter: its move's bounds grown by its reach.
struct Box {
    Fixed low_x;
    Fixed high_x;
    Fixed low_y;
    Fixed high_y;
};

[[nodiscard]] inline Box box_of(Calc& calc, const TrackedLeaf& leaf) {
    return {calc.sub(std::min(leaf.start.x, leaf.end.x), leaf.reach), calc.add(std::max(leaf.start.x, leaf.end.x), leaf.reach),
        calc.sub(std::min(leaf.start.y, leaf.end.y), leaf.reach), calc.add(std::max(leaf.start.y, leaf.end.y), leaf.reach)};
}

// The broad phase of one window (or the static layer), #520: its leaves' prefilter rectangles
// and, for a window of many leaves, a grid of 1024-unit cells listing the leaves whose
// rectangle meets each cell. A leaf whose rectangle meets a query's meets one of the query's
// cells, so the grid's candidates hold every leaf the prefilter keeps.
struct BoxIndex {
    std::vector<Box> boxes;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> cells; // (cell, leaf), ascending; empty: no grid
    std::vector<std::uint32_t> wide;                            // leaves over too many cells, always candidates
};


class Boxes final {
public:
    [[nodiscard]] const BoxIndex& window(Calc& calc, const TrackingLayerView& layer, const std::size_t index);
    [[nodiscard]] const BoxIndex& statics(Calc& calc, const std::vector<TrackedLeaf>& leaves);
    // The leaves of `index` whose rectangle may meet the rectangle [low, high], ascending; valid
    // until the next call.
    [[nodiscard]] const std::vector<std::uint32_t>& candidates(const BoxIndex& index, const Fixed low_x, const Fixed high_x,
        const Fixed low_y, const Fixed high_y);

private:
    const TrackingLayerView* layer_{};
    std::vector<BoxIndex> windows_;
    std::vector<char> built_;
    const std::vector<TrackedLeaf>* statics_of_{};
    BoxIndex statics_;
    std::vector<std::uint32_t> candidates_;
};



// the query's clipped positions; the leaves move over their window [window_start,
// window_end] and are placed at the query's full frames. `facing()` is the query's facing and
// the window fractions are computed at the first exact test only (#520): the prefilter
// rejects most leaves, and neither changes what the prefilter keeps.
template <typename Facing>
[[nodiscard]] std::uint8_t detect(Calc& calc, const std::vector<TrackedLeaf>& leaves, const BoxIndex& index,
    Boxes& boxes, const LinearQuery& query, const Vec2 from, const Vec2 to, const Fixed window_start,
    const Fixed window_end, PathSearchStats& stats, Facing&& facing) {
    ++stats.windows;
    const Fixed query_reach = std::max(query.x_extent, query.y_extent);
    const Fixed low_x = calc.sub(std::min(from.x, to.x), query_reach);
    const Fixed low_y = calc.sub(std::min(from.y, to.y), query_reach);
    const Fixed high_x = calc.add(std::max(from.x, to.x), query_reach);
    const Fixed high_y = calc.add(std::max(from.y, to.y), query_reach);
    bool timed = false;
    Fixed t0{};
    Fixed t1 = whole(1);
    std::uint8_t hits = 0;
    // The grid's candidates in leaf order: the leaves the rectangle prefilter can keep.
    for (const std::uint32_t candidate : boxes.candidates(index, low_x, high_x, low_y, high_y)) {
        const auto& leaf = leaves[candidate];
        if (leaf.entity == query.ignore
            || std::binary_search(query.ignore_group.begin(), query.ignore_group.end(), leaf.entity)) {
            continue;
        }
        ++stats.leaves;
        // The tree's rectangle prefilter; with reach >= every blended reach it never drops a hit.
        const Box& box = index.boxes[candidate];
        if (box.high_x < low_x || box.low_x > high_x || box.high_y < low_y || box.low_y > high_y) continue;
        ++stats.narrow;
        if (!timed && window_end != window_start) {
            const Fixed span = calc.sub(window_end, window_start);
            t0 = clamp01(calc.div(calc.sub(query.start_frame, window_start), span));
            t1 = clamp01(calc.div(calc.sub(query.end_frame, window_start), span));
        }
        timed = true;
        const Vec2 p0 = lerp(calc, leaf.start, leaf.end, t0);
        const Vec2 p1 = lerp(calc, leaf.start, leaf.end, t1);
        const Vec2 relative = minus(calc, from, p0);
        const Vec2 motion = minus(calc, minus(calc, to, from), minus(calc, p1, p0));
        const Fixed motion_square = calc.dot(motion.x, motion.y, motion.x, motion.y);
        const Fixed along = calc.dot(relative.x, relative.y, motion.x, motion.y);
        // An early miss, the same bits (#520): the test below hits only where the relative position
        // comes within the blended reach sum, and a reach is at most the footprint's larger extent.
        // A closest approach more than one unit beyond the extents' sum keeps its quadratic above
        // 2 R + 1 units^2 on [0, 1], far above its Q24 roundings (under 2^-22), so it misses too.
        Fixed nearest{};
        if (along.raw() < 0 && motion_square.raw() > 0) nearest = std::min(whole(1), calc.div(calc.neg(along), motion_square));
        const Fixed closest_x = calc.add(relative.x, calc.mul(motion.x, nearest));
        const Fixed closest_y = calc.add(relative.y, calc.mul(motion.y, nearest));
        const Fixed limit = calc.add(calc.add(query_reach, std::max(leaf.x_extent, leaf.y_extent)), whole(1));
        if (calc.dot(closest_x, closest_y, closest_x, closest_y) > calc.mul(limit, limit)) continue;
        const Vec2& query_facing = facing();
        const Vec2 dir0 = unit(calc, minus(calc, leaf.start, from));
        const Vec2 dir1 = unit(calc, minus(calc, leaf.end, to));
        const Fixed reach0 = calc.add(reach(calc, dir0, query_facing, query.x_extent, query.y_extent),
            reach(calc, dir0, leaf.facing, leaf.x_extent, leaf.y_extent));
        const Fixed reach1 = calc.add(reach(calc, dir1, query_facing, query.x_extent, query.y_extent),
            reach(calc, dir1, leaf.facing, leaf.x_extent, leaf.y_extent));
        const Fixed growth = calc.sub(reach1, reach0);
        // |relative + t motion| = reach0 + t growth: a t^2 + b t + c = 0.
        const Fixed a = calc.sub(motion_square, calc.mul(growth, growth));
        const Fixed b = calc.mul(whole(2), calc.sub(along, calc.mul(reach0, growth)));
        const Fixed c = calc.sub(calc.dot(relative.x, relative.y, relative.x, relative.y), calc.mul(reach0, reach0));
        const Fixed at_end = calc.add(calc.add(a, b), c);
        bool hit = false;
        if (a.raw() == 0) {
            hit = c.raw() <= 0 || at_end.raw() <= 0;
        } else if (a.raw() > 0) {
            // Convex: the roots must overlap [0, 1].
            if (discriminant_sign(calc, a, b, c) > 0) {
                const Fixed twice = calc.mul(whole(2), a);
                const Fixed vertex = calc.neg(b); // t* = -b / 2a
                hit = c.raw() <= 0 || at_end.raw() <= 0 || (vertex.raw() > 0 && vertex < twice);
            }
        } else {
            // Concave: a miss needs [0, 1] strictly inside the roots.
            hit = discriminant_sign(calc, a, b, c) <= 0 || c.raw() < 0 || at_end.raw() < 0;
        }
        if (hit) hits = static_cast<std::uint8_t>(hits | leaf.collision);
    }
    return hits;
}

template <typename Facing>
[[nodiscard]] std::uint8_t find_in_layer(Calc& calc, const TrackingLayerView& layer, const std::uint32_t interval,
    const LinearQuery& query, Boxes& boxes, PathSearchStats& stats, Facing&& facing) {
    if (layer.windows.empty() || query.end_frame < query.start_frame) return 0;
    const Fixed first = whole(static_cast<std::int64_t>(layer.start_frame));
    const Fixed offset = calc.sub(query.start_frame, first);
    if (offset.raw() < 0) return 0;
    const Fixed step = whole(interval);
    const std::size_t count = layer.windows.size();
    auto index = static_cast<std::size_t>(calc.div(offset, step).raw() / Fixed::scale);
    index = std::min(index, count - 1);
    Fixed span = calc.sub(query.end_frame, query.start_frame);
    if (span.raw() == 0) span = whole(1);
    const bool moves = query.start != query.end;
    Fixed from_frame = query.start_frame;
    while (from_frame <= query.end_frame && calc.ok()) {
        const Fixed window_start = calc.add(first, calc.mul(step, whole(static_cast<std::int64_t>(index))));
        const Fixed window_end = calc.add(window_start, step);
        const Fixed to_frame = std::min(window_end, query.end_frame);
        // A part that starts or ends with the query has the fraction 0 or 1, whose lerp is the
        // query's own end point exactly (#520).
        Vec2 from = query.start;
        Vec2 to = query.end;
        if (moves) {
            if (from_frame != query.start_frame) {
                from = lerp(calc, query.start, query.end, calc.div(calc.sub(from_frame, query.start_frame), span));
            }
            if (to_frame != query.end_frame && query.end_frame != query.start_frame) {
                to = lerp(calc, query.start, query.end, calc.div(calc.sub(to_frame, query.start_frame), span));
            }
        }
        const auto hits = detect(calc, layer.windows[index], boxes.window(calc, layer, index), boxes, query, from, to,
            window_start, window_end, stats, facing);
        if (hits != 0) return hits;
        // The next window starts one frame after this one's end (research E71-21).
        from_frame = calc.add(to_frame, whole(1));
        if (index == count - 1) break;
        ++index;
    }
    return 0;
}

// The query's own facing, for the queries that carry one.
[[nodiscard]] inline auto facing_of(const LinearQuery& query) {
    return [&query]() -> const Vec2& { return query.facing; };
}

[[nodiscard]] Vec2 open_position(Calc& calc, Work& work, Boxes& boxes, const AvoidanceRules& rules, const Footprint& footprint,
    const CollisionWorld& world, const EntityId entity, const Fixed now, const Vec2 position, const Vec2 destination,
    const std::span<const EntityId> ignore_group = {}, std::uint8_t filter = collision_all);

[[nodiscard]] std::uint8_t context_filter(Calc& calc, Work& work, Boxes& boxes, const CollisionWorld& world,
    const Footprint& footprint, EntityId entity, Fixed now, Vec2 position, Vec2 destination, bool through_hazards);

} // namespace eawr::sim::tactical::pathfind_detail
