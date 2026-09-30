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

// docs/behaviour/space-movement.md, "Avoidance": the rules are cited there (AV-nn) with their
// research evidence; this file follows FoC's space path finder and object tracking
// step by step, in Q24 where FoC uses binary32.
namespace eawr::sim::tactical {
namespace {

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

// The probe path_bench installs (#520); null reads no clock.
std::atomic<PathSearchProbe*> installed_probe{nullptr};

// The counters of one call and the probe it reports to (#520).
struct Work {
    PathSearchStats stats;
    PathSearchProbe* probe{installed_probe.load(std::memory_order_acquire)};
    [[nodiscard]] std::uint64_t now() const noexcept { return probe != nullptr ? probe->now() : 0; }
};

// ---- Tracking: collision test (AV-03, AV-04; research E71-13, E71-14, E71-20, E71-21) ----

[[nodiscard]] Fixed lerp(Calc& calc, const Fixed a, const Fixed b, const Fixed t) {
    return calc.add(a, calc.mul(calc.sub(b, a), t));
}

[[nodiscard]] Vec2 lerp(Calc& calc, const Vec2 a, const Vec2 b, const Fixed t) {
    return {lerp(calc, a.x, b.x, t), lerp(calc, a.y, b.y, t)};
}

[[nodiscard]] Vec2 minus(Calc& calc, const Vec2 a, const Vec2 b) { return {calc.sub(a.x, b.x), calc.sub(a.y, b.y)}; }

// Point2::Normalize: a zero vector stays zero.
[[nodiscard]] Vec2 unit(Calc& calc, const Vec2 value) {
    if (value.x.raw() == 0 && value.y.raw() == 0) return {};
    const Fixed length = calc.length(value.x, value.y);
    if (length.raw() == 0) return {};
    return {calc.div(value.x, length), calc.div(value.y, length)};
}

[[nodiscard]] Fixed clamp01(const Fixed value) noexcept {
    return std::clamp(value, Fixed{}, whole(1));
}

// A footprint's reach along a direction: X extent along its facing, Y across, blended by
// the squared cosine.
[[nodiscard]] Fixed reach(Calc& calc, const Vec2 direction, const Vec2 facing, const Fixed x, const Fixed y) {
    const Fixed c = calc.abs(calc.dot(direction.x, direction.y, facing.x, facing.y));
    const Fixed c2 = calc.mul(c, c);
    return calc.add(calc.mul(c2, x), calc.mul(calc.sub(whole(1), c2), y));
}

// b^2 - 4ac > 0, = 0 or < 0, exactly: the raw products in 128 bits (#520).
[[nodiscard]] int discriminant_sign(Calc& calc, const Fixed a, const Fixed b, const Fixed c) {
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

[[nodiscard]] Box box_of(Calc& calc, const TrackedLeaf& leaf) {
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

constexpr unsigned cell_shift = Fixed::fractional_bits + 10; // 1024 units
constexpr std::size_t grid_leaves = 8;                       // fewer leaves: a plain scan
constexpr std::int64_t grid_span = 16;                       // more cells: a plain scan

[[nodiscard]] std::int64_t cell_of(const Fixed value) noexcept { return value.raw() >> cell_shift; }

[[nodiscard]] std::uint64_t cell_key(const std::int64_t x, const std::int64_t y) noexcept {
    return (static_cast<std::uint64_t>(x) << 32U) ^ (static_cast<std::uint64_t>(y) & 0xffffffffU);
}

void build_index(Calc& calc, const std::vector<TrackedLeaf>& leaves, BoxIndex& index) {
    index.boxes.clear();
    index.cells.clear();
    index.wide.clear();
    index.boxes.reserve(leaves.size());
    for (const auto& leaf : leaves) index.boxes.push_back(box_of(calc, leaf));
    if (leaves.size() < grid_leaves) return;
    for (std::uint32_t leaf = 0; leaf < index.boxes.size(); ++leaf) {
        const Box& box = index.boxes[leaf];
        const std::int64_t low_x = cell_of(box.low_x);
        const std::int64_t high_x = cell_of(box.high_x);
        const std::int64_t low_y = cell_of(box.low_y);
        const std::int64_t high_y = cell_of(box.high_y);
        if ((high_x - low_x + 1) * (high_y - low_y + 1) > grid_span) {
            index.wide.push_back(leaf);
            continue;
        }
        for (std::int64_t x = low_x; x <= high_x; ++x) {
            for (std::int64_t y = low_y; y <= high_y; ++y) index.cells.emplace_back(cell_key(x, y), leaf);
        }
    }
    std::sort(index.cells.begin(), index.cells.end());
}

// The prefilter rectangles and grids of the windows and static objects one call queries, each
// built on first use (#520): a search queries the same few windows thousands of times. The
// views outlive the call and do not change during it.
class Boxes final {
public:
    [[nodiscard]] const BoxIndex& window(Calc& calc, const TrackingLayerView& layer, const std::size_t index) {
        if (layer_ != &layer) {
            layer_ = &layer;
            windows_.assign(layer.windows.size(), {});
            built_.assign(layer.windows.size(), 0);
        }
        if (built_[index] == 0) {
            build_index(calc, layer.windows[index], windows_[index]);
            built_[index] = 1;
        }
        return windows_[index];
    }
    [[nodiscard]] const BoxIndex& statics(Calc& calc, const std::vector<TrackedLeaf>& leaves) {
        if (statics_of_ != &leaves) {
            statics_of_ = &leaves;
            build_index(calc, leaves, statics_);
        }
        return statics_;
    }
    // The leaves of `index` whose rectangle may meet the rectangle [low, high], ascending; valid
    // until the next call.
    [[nodiscard]] const std::vector<std::uint32_t>& candidates(const BoxIndex& index, const Fixed low_x, const Fixed high_x,
        const Fixed low_y, const Fixed high_y) {
        candidates_.clear();
        const std::int64_t first_x = cell_of(low_x);
        const std::int64_t last_x = cell_of(high_x);
        const std::int64_t first_y = cell_of(low_y);
        const std::int64_t last_y = cell_of(high_y);
        if (index.cells.empty() || (last_x - first_x + 1) * (last_y - first_y + 1) > grid_span) {
            for (std::uint32_t leaf = 0; leaf < index.boxes.size(); ++leaf) candidates_.push_back(leaf);
            return candidates_;
        }
        candidates_ = index.wide;
        for (std::int64_t x = first_x; x <= last_x; ++x) {
            for (std::int64_t y = first_y; y <= last_y; ++y) {
                const std::uint64_t key = cell_key(x, y);
                auto entry = std::lower_bound(index.cells.begin(), index.cells.end(),
                    std::pair<std::uint64_t, std::uint32_t>{key, 0});
                for (; entry != index.cells.end() && entry->first == key; ++entry) candidates_.push_back(entry->second);
            }
        }
        std::sort(candidates_.begin(), candidates_.end());
        candidates_.erase(std::unique(candidates_.begin(), candidates_.end()), candidates_.end());
        return candidates_;
    }

private:
    const TrackingLayerView* layer_{};
    std::vector<BoxIndex> windows_;
    std::vector<char> built_;
    const std::vector<TrackedLeaf>* statics_of_{};
    BoxIndex statics_;
    std::vector<std::uint32_t> candidates_;
};

// The object tracking tree's linear collision detection over one window's leaves: `from`/`to` are
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
[[nodiscard]] auto facing_of(const LinearQuery& query) {
    return [&query]() -> const Vec2& { return query.facing; };
}

// ---- Path finder (AV-10 to AV-14; research E71-01 to E71-11, E71-22, E71-23) ----

struct Config {
    Fixed max_speed;
    Fixed rate_of_turn;
    Fixed radius;
    Fixed x_extent;
    Fixed y_extent;
    std::uint32_t max_expansions{};
    Fixed path_cost_coefficient = whole(1);
    Fixed distance_cutoff{};
    Fixed forward_coefficient = whole(1);
    Fixed rotation_coefficient = whole(1);
    bool allow_wait = true;
    bool only_finish_on_end = true;
    bool allow_emergency = true;
    bool cull_bounded = true;
    std::uint8_t mask = collision_all;
    bool penalize_filtered = false;
    // PC-06: the estimate's weight in the bounded search's weighted tries; 1 is FoC's.
    Fixed estimate_weight = whole(1);
};

struct Signature {
    std::int64_t x{};
    std::int64_t y{};
    std::int64_t z{};
    friend constexpr bool operator==(const Signature&, const Signature&) noexcept = default;
};

struct Component {
    Vec2 position;
    Fixed facing;
    Fixed frame;
    Fixed velocity;
    int op{op_none};
    int previous{-1};
    Fixed distance; // to the target (the remaining-estimate distance, AV-12), kept for the children
};

struct Header {
    int component{};
    Fixed path_cost;
    Fixed total_cost;
};

struct Record {
    bool closed{};
    Fixed cost;
};

// The cell table: open addressing in one array whose slots are kept between tries (#520). It is
// only looked up and filled, never iterated, so its order cannot reach a path.
class CellTable final {
public:
    void clear() {
        for (const auto index : used_) slots_[index].used = false;
        used_.clear();
    }
    // The cell's record, or null; valid until the next put.
    [[nodiscard]] Record* find(const Signature& key) {
        if (slots_.empty()) return nullptr;
        for (std::size_t index = slot_of(key);; index = (index + 1U) & mask_) {
            Slot& slot = slots_[index];
            if (!slot.used) return nullptr;
            if (slot.key == key) return &slot.value;
        }
    }
    // Sets the cell's record, adding the cell when it is new.
    void put(const Signature& key, const Record& value) {
        if ((used_.size() + 1U) * 2U > slots_.size()) grow();
        for (std::size_t index = slot_of(key);; index = (index + 1U) & mask_) {
            Slot& slot = slots_[index];
            if (!slot.used) {
                slot = {key, value, true};
                used_.push_back(index);
                return;
            }
            if (slot.key == key) {
                slot.value = value;
                return;
            }
        }
    }

    [[nodiscard]] std::size_t held_bytes() const noexcept {
        return slots_.capacity() * sizeof(Slot) + used_.capacity() * sizeof(std::size_t);
    }

private:
    struct Slot {
        Signature key;
        Record value;
        bool used{};
    };
    [[nodiscard]] std::size_t slot_of(const Signature& key) const noexcept {
        std::uint64_t hash = static_cast<std::uint64_t>(key.x) * 0x9e3779b97f4a7c15ULL;
        hash = (hash ^ static_cast<std::uint64_t>(key.y)) * 0xbf58476d1ce4e5b9ULL;
        hash = (hash ^ static_cast<std::uint64_t>(key.z)) * 0x94d049bb133111ebULL;
        return static_cast<std::size_t>(hash ^ (hash >> 31U)) & mask_;
    }
    void grow() {
        std::vector<Slot> old = std::exchange(slots_, std::vector<Slot>(slots_.empty() ? 8192U : slots_.size() * 2U));
        mask_ = slots_.size() - 1U;
        const std::vector<std::size_t> used = std::exchange(used_, {});
        for (const auto index : used) put(old[index].key, old[index].value);
    }
    std::vector<Slot> slots_;
    std::vector<std::size_t> used_;
    std::size_t mask_{};
};

// The path finder's priority queue (debug build): a 1-based binary heap on the total cost with no
// tie-break. Insert sifts up
// while the parent is strictly greater; extract-min moves the last entry to the root and
// sifts down, taking the left child when it is strictly smaller, then the right child when it
// is strictly smaller than that.
class Heap final {
public:
    void insert(const Header& header) {
        items_.push_back(header);
        std::size_t index = items_.size();
        while (index > 1 && items_[index / 2 - 1].total_cost > header.total_cost) {
            items_[index - 1] = items_[index / 2 - 1];
            index /= 2;
        }
        items_[index - 1] = header;
    }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    void clear() noexcept { items_.clear(); }
    Header extract() {
        const Header top = items_.front();
        items_.front() = items_.back();
        items_.pop_back();
        std::size_t index = 1;
        const std::size_t count = items_.size();
        while (true) {
            const std::size_t left = index * 2;
            const std::size_t right = left + 1;
            std::size_t smallest = index;
            if (left <= count && items_[left - 1].total_cost < items_[smallest - 1].total_cost) smallest = left;
            if (right <= count && items_[right - 1].total_cost < items_[smallest - 1].total_cost) smallest = right;
            if (smallest == index) break;
            std::swap(items_[index - 1], items_[smallest - 1]);
            index = smallest;
        }
        return top;
    }

    [[nodiscard]] std::size_t held_bytes() const noexcept { return items_.capacity() * sizeof(Header); }

private:
    std::vector<Header> items_;
};

// A search's nodes, open set and cell table, kept per thread between calls (#520) so that a
// search does not allocate and zero them again. Every try clears them and nothing reads their
// capacity, so a result never depends on the searches before it.
struct SearchScratch {
    std::vector<Component> components;
    CellTable records;
    Heap open;
};

[[nodiscard]] SearchScratch& search_scratch() {
    thread_local SearchScratch scratch;
    return scratch;
}

// Uniform_Float_To_Int: truncation for x >= 0, truncation of x - 1 below (research E71-04).
[[nodiscard]] std::int64_t uniform_int(const Fixed value) noexcept {
    if (value.raw() >= 0) return value.raw() / Fixed::scale;
    return (value.raw() - Fixed::scale) / Fixed::scale;
}

class Finder final {
public:
    Finder(const MotionTable& table, const AvoidanceRules& rules, const MotionProfile& limits, const Footprint& footprint,
        const CollisionWorld& world, const EntityId entity, const Fixed now, const Vec3 start, const Fixed start_yaw,
        const Fixed start_speed, const Vec3 end, Work& work, Boxes& boxes, SearchScratch& scratch)
        : table_(table), rules_(rules), limits_(limits), footprint_(footprint), world_(world), entity_(entity),
          now_(now), start_(start), start_yaw_(start_yaw), start_speed_(start_speed), end_(end), work_(work),
          components_(scratch.components), boxes_(boxes), records_(scratch.records), open_(scratch.open) {
        layer_ = dynamic_layer_index(footprint.layer);
    }

    enum class Step : std::uint8_t { found, failed, running };

    // Starts one path finder try (AV-14): `found` or `failed` when it ends at once (the trivial path,
    // MV-12), `running` with the origin open otherwise; resume() runs it. `weight` is the
    // estimate weight below the distance cutoff (research E71-23).
    Step begin(const Config& config, const Fixed weight, std::vector<PathNode>& nodes) {
        config_ = config;
        weight_ = weight;
        ++work_.stats.tries;
        initialize();
        if (!calc_.ok()) return Step::failed;
        const Signature start = signature({start_.x, start_.y}, clamp180(start_yaw_));
        end_signature_ = signature({end_.x, end_.y}, Fixed{});
        if (start.x == end_signature_.x && start.y == end_signature_.y) {
            // Make_Trivial_Path (MV-12).
            auto trivial = motion_detail::trivial_path(calc_, config_.max_speed, forward_speed_, now_, start_,
                start_yaw_, start_speed_, end_);
            if (!trivial) {
                nodes = {{now_, start_, clamp180(start_yaw_), start_speed_},
                    {calc_.add(now_, calc_.div(length_, config_.max_speed)), {end_.x, end_.y, start_.z},
                        calc_.atan2_deg(calc_.sub(end_.y, start_.y), calc_.sub(end_.x, start_.x)), Fixed{}}};
            } else {
                nodes = std::move(*trivial);
            }
            return calc_.ok() ? Step::found : Step::failed;
        }
        components_.clear();
        records_.clear();
        open_.clear();
        Component origin{{start_.x, start_.y}, clamp180(start_yaw_), now_, start_speed_, op_none, -1, {}};
        origin.distance = distance3(origin.position);
        components_.push_back(origin);
        open_.insert({0, Fixed{}, Fixed{}});
        records_.put(start, {false, Fixed{}});
        expansions_ = 0;
        last_op_ = config_.allow_emergency ? op_emergency_right : op_wait;
        return Step::running;
    }

    // Runs the started call until it ends, or until the search's expansions (every try's) reach
    // `until` (PC-08): the count is read before each parent, so a slice ends between two
    // parents and running the call in slices takes the same steps as running it at once.
    Step resume(Fixed& farthest, std::vector<PathNode>& nodes, const std::uint64_t until) {
        while (!open_.empty() && expansions_ < config_.max_expansions && calc_.ok()) {
            if (work_.stats.expansions >= until) return Step::running;
            auto mark = work_.now();
            const Header header = open_.extract();
            const Component parent = components_[static_cast<std::size_t>(header.component)];
            work_.stats.set_ticks += work_.now() - mark;
            const Signature here = signature(parent.position, parent.facing);
            if (reached(here, parent.op)) {
                build_final(header.component, nodes);
                return calc_.ok() ? Step::found : Step::failed;
            }
            mark = work_.now();
            if (Record* const found = records_.find(here); found != nullptr) found->closed = true;
            work_.stats.set_ticks += work_.now() - mark;
            const auto h = heading_of(parent.facing);
            for (int op = op_forward; op <= last_op_ && calc_.ok(); ++op) {
                Component child;
                if (!expand(op, parent, h, child)) continue;
                ++work_.stats.children;
                const Signature cell = signature(child.position, child.facing);
                mark = work_.now();
                Record* const record = records_.find(cell);
                work_.stats.set_ticks += work_.now() - mark;
                if (record != nullptr && record->closed && culls(op)) continue;
                // the remaining-estimate distance (AV-12), which an arc's cost also reads (#520: once).
                child.distance = distance3(child.position);
                LinearQuery query;
                query.start = parent.position;
                query.start_frame = parent.frame;
                query.end = child.position;
                query.end_frame = child.frame;
                query.x_extent = config_.x_extent;
                query.y_extent = config_.y_extent;
                query.ignore = entity_;
                // The query's facing, computed when an exact test first needs it (#520).
                std::optional<Vec2> facing;
                const auto query_facing = [&]() -> const Vec2& {
                    if (!facing) {
                        facing = query.start != query.end ? unit(calc_, minus(calc_, query.end, query.start))
                                                          : Vec2{heading_of(child.facing).x, heading_of(child.facing).y};
                    }
                    return *facing;
                };
                Fixed cost = calc_.sub(child.frame, parent.frame);
                if (!linear_cost(cost, op, query, parent.velocity, child.distance, query_facing)) continue;
                const Fixed path_cost = calc_.add(cost, header.path_cost);
                const bool counts = !(config_.only_finish_on_end && cell.x == end_signature_.x && cell.y == end_signature_.y);
                if (record != nullptr) {
                    if (record->cost <= path_cost && culls(op)) continue;
                    if (counts) record->cost = path_cost;
                }
                // The remaining estimate (AV-12): straight distance over the maximum speed.
                const Fixed estimate = calc_.div(child.distance, config_.max_speed);
                const Fixed reach_distance = calc_.mul(calc_.sub(child.frame, now_), config_.max_speed);
                if (reach_distance > farthest) farthest = reach_distance;
                const Fixed w = config_.distance_cutoff <= reach_distance ? whole(1) : weight_;
                child.op = op;
                child.previous = header.component;
                const Fixed total = calc_.add(path_cost, calc_.mul(calc_.mul(w, config_.estimate_weight), estimate));
                mark = work_.now();
                components_.push_back(child);
                open_.insert({static_cast<int>(components_.size() - 1), path_cost, total});
                if (record == nullptr && counts) records_.put(cell, {false, path_cost});
                work_.stats.set_ticks += work_.now() - mark;
                ++expansions_;
                ++work_.stats.expansions;
            }
            if (last_op_ == op_emergency_right) last_op_ = op_wait; // emergency turns only from the start
        }
        return Step::failed;
    }

    [[nodiscard]] bool ok() const noexcept { return calc_.ok(); }
    [[nodiscard]] core::Diagnostic error() const { return calc_.error("path finder"); }
    [[nodiscard]] Calc& calc() noexcept { return calc_; }

    // The collision bits of a query in the unit's layer and the static layer (the two-layer collision check);
    // `facing()` gives the query's facing.
    template <typename Facing>
    [[nodiscard]] std::uint8_t dual_layer(const LinearQuery& query, Facing&& facing) {
        if (!layer_) return 0;
        const auto mark = work_.now();
        ++work_.stats.queries;
        std::uint8_t hits = 0;
        if (const auto* view = world_.layers[*layer_]; view != nullptr) {
            hits = find_in_layer(calc_, *view, world_.interval, query, boxes_, work_.stats, facing);
        }
        if ((config_.mask & 0x3e) != 0 && world_.statics != nullptr) {
            // The static layer's one window; its objects never move, so its frames do not matter.
            LinearQuery widened = query;
            widened.y_extent = calc_.add(widened.y_extent, static_bonus);
            hits = static_cast<std::uint8_t>(hits
                | detect(calc_, *world_.statics, boxes_.statics(calc_, *world_.statics), boxes_, widened, widened.start,
                    widened.end, Fixed{}, Fixed{}, work_.stats, facing));
        }
        work_.stats.query_ticks += work_.now() - mark;
        return hits;
    }

private:
    [[nodiscard]] Signature signature(const Vec2 position, const Fixed facing) {
        return {uniform_int(calc_.mul(calc_.sub(position.x, start_.x), signature_scale_)),
            uniform_int(calc_.mul(calc_.sub(position.y, start_.y), signature_scale_)),
            uniform_int(calc_.mul(facing, angle_scale_))};
    }

    [[nodiscard]] Fixed distance3(const Vec2 position) {
        return calc_.length(calc_.sub(position.x, end_.x), calc_.sub(position.y, end_.y));
    }

    // heading() through a small cache (#520): a search's facings repeat (the arcs step by the
    // arc angle), and a heading is a function of the facing alone.
    [[nodiscard]] const motion_detail::Heading& heading_of(const Fixed yaw) {
        auto& slot = headings_[static_cast<std::size_t>(static_cast<std::uint64_t>(yaw.raw()) * 0x9e3779b97f4a7c15ULL >> 56U)];
        if (!slot.used || slot.yaw != yaw) {
            slot.value = heading(calc_, yaw);
            slot.yaw = yaw;
            slot.used = true;
        }
        return slot.value;
    }

    // Initialize_Constants (research E71-03) and Calculate_Emergency_Turn_Angle (E71-09).
    void initialize() {
        const Fixed rotations = calc_.div(rules_.max_rotations, config_.rotation_coefficient);
        const Fixed vmax = config_.max_speed;
        const Fixed rot = config_.rate_of_turn;
        length_ = calc_.length(calc_.sub(end_.x, start_.x), calc_.sub(end_.y, start_.y));
        Fixed step = calc_.mul(table_.rules.expansion_distance, config_.forward_coefficient);
        if (length_ < calc_.mul(whole(2), step)) {
            step = std::max(motion_detail::min_expansion_distance, calc_.div(length_, whole(2)));
        }
        const Fixed frames_forward = std::max(whole(1), calc_.div(step, vmax));
        const Fixed frames_turn = calc_.div(calc_.div(whole(360), rotations), rot);
        theta_ = calc_.mul(frames_turn, rot);
        while (theta_ >= whole(360)) theta_ = calc_.sub(theta_, whole(360));
        forward_speed_ = calc_.mul(frames_forward, vmax);
        turn_speed_ = calc_.mul(frames_turn, vmax);
        signature_scale_ = calc_.div(signature_cells, forward_speed_);
        angle_scale_ = calc_.div(calc_.add(rotations, whole(1)), whole(360));
        turn_radius_ = calc_.mul(calc_.div(whole(360), theta_), calc_.div(turn_speed_, motion_detail::two_pi));
        const Fixed half_theta = calc_.div(theta_, whole(2));
        const Fixed chord = calc_.mul(calc_.mul(turn_radius_, whole(2)), calc_.sin_deg(half_theta));
        x_advance_ = calc_.mul(chord, calc_.cos_deg(half_theta));
        y_advance_ = calc_.abs(calc_.mul(chord, calc_.sin_deg(half_theta)));
        slow_speed_ = calc_.mul(vmax, rules_.wait_speed);
        emergency_left_ = Fixed{};
        emergency_right_ = Fixed{};
        if (!layer_ || !calc_.ok()) return;
        // Rays of the turn radius plus the distance to reach full speed, 10 degrees apart.
        const Fixed ray = calc_.add(turn_radius_, calc_.div(calc_.abs(calc_.sub(calc_.mul(vmax, vmax),
            calc_.mul(start_speed_, start_speed_))), calc_.mul(whole(2), limits_.acceleration)));
        const Fixed ray_frames = calc_.div(ray, vmax);
        const auto probe = [&](const Fixed direction) {
            for (Fixed angle{}; calc_.abs(angle) < probe_limit && calc_.ok();
                angle = direction.raw() > 0 ? calc_.add(angle, probe_step) : calc_.sub(angle, probe_step)) {
                const auto h = heading(calc_, calc_.add(start_yaw_, angle));
                LinearQuery query;
                query.start = {start_.x, start_.y};
                query.end = {calc_.add(start_.x, calc_.mul(h.x, ray)), calc_.add(start_.y, calc_.mul(h.y, ray))};
                query.facing = {h.x, h.y};
                query.start_frame = calc_.add(now_, calc_.div(calc_.abs(angle), rot));
                query.end_frame = calc_.add(query.start_frame, ray_frames);
                query.x_extent = footprint_.x_extent;
                query.y_extent = footprint_.y_extent;
                query.ignore = entity_;
                if ((dual_layer(query, facing_of(query)) & config_.mask) == 0) return angle;
            }
            return Fixed{};
        };
        emergency_left_ = probe(whole(1));
        emergency_right_ = probe(whole(-1));
    }

    // Have_Reached_Destination without a final facing.
    [[nodiscard]] bool reached(const Signature& here, const int op) const noexcept {
        if (config_.only_finish_on_end && op != op_end) return false;
        return here.x == end_signature_.x && here.y == end_signature_.y;
    }

    // Compute_Frame_And_Velocity (research E71-05).
    void frame_and_velocity(const Fixed frame, const Fixed v, const Fixed distance, const int op, Component& child) {
        const Fixed target = is_slow(op) ? slow_speed_ : config_.max_speed;
        if (v == target) {
            child.frame = calc_.add(frame, calc_.div(distance, v));
            child.velocity = v;
            return;
        }
        const Fixed rate = target < v ? limits_.deceleration : limits_.acceleration;
        const Fixed change = calc_.div(calc_.abs(calc_.sub(calc_.mul(v, v), calc_.mul(target, target))),
            calc_.mul(whole(2), rate));
        if (distance <= change) {
            const Fixed delta = calc_.sqrt(calc_.mul(calc_.mul(whole(2), rate), distance));
            child.velocity = target <= v ? calc_.sub(v, delta) : calc_.add(v, delta);
            child.frame = calc_.add(frame, calc_.div(calc_.abs(calc_.sub(child.velocity, v)), rate));
        } else {
            child.velocity = target;
            child.frame = calc_.add(calc_.add(frame, calc_.div(calc_.abs(calc_.sub(target, v)), rate)),
                calc_.div(calc_.sub(distance, change), std::max(v, target)));
        }
    }

    // Build_Expansion, unaligned set (research E71-02).
    bool expand(const int op, const Component& parent, const motion_detail::Heading& h, Component& child) {
        if (is_wait(op) && !config_.allow_wait) return false;
        const Fixed v = parent.velocity;
        if (!is_emergency(op)) {
            const bool at_full = calc_.abs(calc_.sub(v, config_.max_speed)) < limits_.acceleration;
            const bool at_slow = calc_.abs(calc_.sub(v, slow_speed_)) < limits_.acceleration;
            if (!at_full && !is_slow(op) && op != op_full_speed) return false;
            if (!at_slow && is_slow(op) && op != op_slow_speed && !is_unbounded(op)) return false;
            if (parent.op == op_slow_speed && op != op_wait) return false;
            if (parent.op == op_wait && op != op_wait && op != op_full_speed) return false;
            if (parent.op == op_none && !at_full && !at_slow && op != op_full_speed && op != op_slow_speed) return false;
        }
        const Vec2 forward{h.x, h.y};
        const Vec2 left{calc_.neg(h.y), h.x};
        child.facing = parent.facing;
        child.frame = parent.frame;
        child.velocity = v;
        const auto along = [&](const Fixed distance) {
            return Vec2{calc_.add(parent.position.x, calc_.mul(forward.x, distance)),
                calc_.add(parent.position.y, calc_.mul(forward.y, distance))};
        };
        switch (op) {
        case op_forward: {
            const Fixed distance = std::min(parent.distance, forward_speed_);
            // Project (AV-U7): on the target a forward step has no length and no cost, and in
            // Q24 it would repeat until the expansion limit; FoC's binary32 steps do not stay on
            // the target (the S-10 recording finds its detour), so the remake drops it.
            if (distance.raw() == 0) return false;
            child.position = along(distance);
            frame_and_velocity(parent.frame, v, distance, op, child);
            return true;
        }
        case op_match:
            return match(parent, h, child);
        case op_left:
        case op_right: {
            const Fixed side = op == op_left ? y_advance_ : calc_.neg(y_advance_);
            child.position = {calc_.add(calc_.add(parent.position.x, calc_.mul(forward.x, x_advance_)), calc_.mul(left.x, side)),
                calc_.add(calc_.add(parent.position.y, calc_.mul(forward.y, x_advance_)), calc_.mul(left.y, side))};
            child.facing = clamp180(op == op_left ? calc_.add(parent.facing, theta_) : calc_.sub(parent.facing, theta_));
            frame_and_velocity(parent.frame, v, turn_speed_, op, child);
            return true;
        }
        case op_end:
            if (parent.op != op_match) return false;
            child.position = {end_.x, end_.y};
            frame_and_velocity(parent.frame, v, parent.distance, op, child);
            return true;
        case op_full_speed:
        case op_slow_speed: {
            const Fixed target = op == op_slow_speed ? slow_speed_ : config_.max_speed;
            if (v == target) return false;
            const Fixed rate = target <= v ? limits_.deceleration : limits_.acceleration;
            const Fixed distance = calc_.div(calc_.abs(calc_.sub(calc_.mul(v, v), calc_.mul(target, target))),
                calc_.mul(whole(2), rate));
            child.position = along(distance);
            child.frame = calc_.add(parent.frame, calc_.div(calc_.abs(calc_.sub(target, v)), rate));
            child.velocity = target;
            return true;
        }
        case op_wait: {
            const Fixed frames = calc_.div(rules_.wait_frames, config_.max_speed);
            const Fixed distance = calc_.mul(calc_.mul(frames, config_.max_speed), rules_.wait_speed);
            child.position = along(distance);
            frame_and_velocity(parent.frame, v, distance, op, child);
            return true;
        }
        case op_emergency_left:
        case op_emergency_right: {
            const Fixed angle = op == op_emergency_left ? emergency_left_ : emergency_right_;
            if (angle.raw() == 0) return false;
            child.position = parent.position;
            child.facing = clamp180(calc_.add(parent.facing, angle));
            child.frame = calc_.add(parent.frame, calc_.div(calc_.abs(angle), config_.rate_of_turn));
            child.velocity = Fixed{};
            return true;
        }
        default:
            return false;
        }
    }

    // Compute_Unaligned_Forward_Match_Expansion: the turn on the turn circle to the tangent that
    // points at the target (MV-15), legal for a bearing difference in [0.01, theta].
    bool match(const Component& parent, const motion_detail::Heading& h, Component& child) {
        // A node on the target has no bearing to turn to (FoC's facing of a zero vector).
        if (parent.position.x == end_.x && parent.position.y == end_.y) return false;
        const Fixed bearing = calc_.atan2_deg(calc_.sub(end_.y, parent.position.y), calc_.sub(end_.x, parent.position.x));
        const Fixed difference = calc_.abs(clamp180(calc_.sub(bearing, parent.facing)));
        if (difference > theta_ || difference < motion_detail::aligned_degrees) return false;
        const Vec2 toward = unit(calc_, {calc_.sub(end_.x, parent.position.x), calc_.sub(end_.y, parent.position.y)});
        const bool left_turn = calc_.sub(calc_.mul(h.x, toward.y), calc_.mul(h.y, toward.x)).raw() >= 0;
        const Fixed side = left_turn ? turn_radius_ : calc_.neg(turn_radius_);
        const Vec2 centre{calc_.add(parent.position.x, calc_.mul(calc_.neg(h.y), side)),
            calc_.add(parent.position.y, calc_.mul(h.x, side))};
        const Vec2 back = minus(calc_, centre, {end_.x, end_.y});
        const Fixed distance = calc_.length(back.x, back.y);
        if (!(turn_radius_ < distance)) return false;
        // Rotate the target-to-centre direction by asin(R / d) toward the turn.
        const Fixed tangent = calc_.sqrt(calc_.sub(calc_.mul(distance, distance), calc_.mul(turn_radius_, turn_radius_)));
        const Fixed opening = calc_.sub(whole(90), calc_.atan2_deg(tangent, turn_radius_));
        const Fixed rotate = std::clamp(opening, Fixed{}, whole(90));
        const Fixed base = calc_.atan2_deg(back.y, back.x);
        const Fixed direction = left_turn ? calc_.add(base, rotate) : calc_.sub(base, rotate);
        const auto d = heading_of(direction);
        child.position = {calc_.add(end_.x, calc_.mul(d.x, tangent)), calc_.add(end_.y, calc_.mul(d.y, tangent))};
        child.facing = clamp180(calc_.add(direction, whole(180)));
        const Fixed turn = calc_.abs(clamp180(calc_.sub(child.facing, parent.facing)));
        const Fixed arc = calc_.div(calc_.mul(config_.max_speed, turn), config_.rate_of_turn);
        frame_and_velocity(parent.frame, parent.velocity, arc, op_match, child);
        return true;
    }

    // Compute_Linear_Expansion_Cost (research E71-06); the map-edge rule is not modelled
    // (AV-U4). `distance` is the step end's distance to the target.
    template <typename Facing>
    bool linear_cost(Fixed& cost, const int op, const LinearQuery& query, const Fixed parent_velocity,
        const Fixed distance, Facing&& facing) {
        switch (op) {
        case op_match:
            cost = calc_.mul(cost, match_factor);
            break;
        case op_left:
        case op_right: {
            // The distance includes the unit's height against the query's z of 0 (a retail quirk);
            // at height 0 that length is the planar one exactly.
            const Fixed spatial = end_.z.raw() == 0 ? distance : calc_.length(distance, end_.z);
            const Fixed scaled = std::max(whole(1), calc_.div(spatial, arc_distance_scale));
            cost = calc_.mul(cost, calc_.add(calc_.div(half, scaled), whole(1)));
            break;
        }
        case op_end:
            cost = calc_.mul(cost, end_factor);
            break;
        case op_wait:
            cost = calc_.mul(cost, rules_.wait_cost);
            break;
        default:
            break;
        }
        if (!layer_) return true;
        const std::uint8_t hits = collision_check(query, op, parent_velocity, facing);
        const Fixed penalty = calc_.div(rules_.min_obstacle_cost, rules_.path_cost_coefficient);
        if ((hits & config_.mask) == 0) {
            if (config_.penalize_filtered && (hits & 0x1c) != 0) cost = calc_.mul(cost, penalty);
            return true;
        }
        if ((hits & static_cast<std::uint8_t>(~collision_moving)) != 0) {
            // Near the start a blocked step only costs more; beyond it, or an end leg, is dropped.
            const Fixed near = calc_.add(turn_radius_, calc_.mul(config_.radius, rules_.occupation_radius));
            const Fixed from_start = calc_.length(calc_.length(calc_.sub(query.start.x, start_.x),
                calc_.sub(query.start.y, start_.y)), start_.z);
            if ((near < from_start && config_.cull_bounded) || is_unbounded(op)) return false;
        }
        cost = calc_.mul(cost, penalty);
        return true;
    }

    // The collision check: an end leg is split into its constant-speed and braking parts, both
    // with the whole leg's facing.
    template <typename Facing>
    std::uint8_t collision_check(const LinearQuery& query, const int op, const Fixed v, Facing&& facing) {
        if (op != op_end || !(brake_margin < v)) return dual_layer(query, facing);
        const Fixed braking = calc_.div(calc_.mul(v, v), calc_.mul(whole(2), limits_.deceleration));
        const Fixed leg = calc_.length(calc_.sub(query.end.x, query.start.x), calc_.sub(query.end.y, query.start.y));
        if (!(braking < leg)) return dual_layer(query, facing);
        const Vec2 direction = unit(calc_, minus(calc_, query.end, query.start));
        const Fixed cruise = calc_.sub(leg, braking);
        const Vec2 split{calc_.add(query.start.x, calc_.mul(direction.x, cruise)),
            calc_.add(query.start.y, calc_.mul(direction.y, cruise))};
        const Fixed split_frame = calc_.add(query.start_frame, calc_.div(cruise, v));
        if (!(split_frame < query.end_frame)) return dual_layer(query, facing);
        LinearQuery first = query;
        first.end = split;
        first.end_frame = split_frame;
        if (const auto hits = dual_layer(first, facing); hits != 0) return hits;
        LinearQuery second = query;
        second.start = split;
        second.start_frame = split_frame;
        return dual_layer(second, facing);
    }

    // Build_Final_Path (research E71-10): one node per component; an emergency turn stops the
    // ship at the start first.
    void build_final(const int last, std::vector<PathNode>& nodes) {
        std::vector<int> chain;
        for (int index = last; index >= 0; index = components_[static_cast<std::size_t>(index)].previous) {
            chain.push_back(index);
        }
        std::reverse(chain.begin(), chain.end());
        nodes.clear();
        for (const int index : chain) {
            const auto& component = components_[static_cast<std::size_t>(index)];
            nodes.push_back({component.frame, {component.position.x, component.position.y, start_.z},
                clamp180(component.facing), component.velocity});
            if (is_emergency(component.op) && nodes.size() >= 2) nodes[nodes.size() - 2].speed = Fixed{};
        }
        MotionProfile profile = limits_;
        motion_detail::finish_path(calc_, profile, theta_, nodes);
    }

    const MotionTable& table_;
    const AvoidanceRules& rules_;
    const MotionProfile& limits_;
    const Footprint& footprint_;
    const CollisionWorld& world_;
    EntityId entity_;
    Fixed now_;
    Vec3 start_;
    Fixed start_yaw_;
    Fixed start_speed_;
    Vec3 end_;
    std::optional<std::size_t> layer_;
    Calc calc_;
    Config config_;
    Fixed length_;
    Fixed theta_;
    Fixed forward_speed_;
    Fixed turn_speed_;
    Fixed signature_scale_;
    Fixed angle_scale_;
    Fixed turn_radius_;
    Fixed x_advance_;
    Fixed y_advance_;
    Fixed slow_speed_;
    Fixed emergency_left_;
    Fixed emergency_right_;
    Signature end_signature_;
    // The started call's state between resume() calls.
    Fixed weight_;
    std::uint32_t expansions_{};
    int last_op_{op_wait};
    Work& work_;
    std::vector<Component>& components_;
    Boxes& boxes_;
    CellTable& records_;
    Heap& open_;
    struct HeadingSlot {
        Fixed yaw;
        motion_detail::Heading value{};
        bool used{};
    };
    std::array<HeadingSlot, 256> headings_{};
};

// The formation centre path's maximum speed reduction for short moves (research
// E71-22): the planning speed drops until the target lies outside both turn circles (or ahead
// and at least a turn diameter away) and the ship can brake plus turn before it.
[[nodiscard]] Fixed reduced_speed(Calc& calc, const MotionProfile& limits, const Vec3 position, const Fixed yaw,
    const Fixed speed, const Vec3 target) {
    const Fixed original = limits.max_speed;
    const Fixed rot_turns = calc.div(limits.rate_of_turn, degrees_per_radian_turn); // turns per frame
    const Fixed rot_radians = calc.mul(rot_turns, motion_detail::two_pi);
    const auto h = heading(calc, yaw);
    const Vec2 target_xy{target.x, target.y};
    const Fixed to_target = calc.length(calc.sub(position.x, target.x), calc.sub(position.y, target.y));
    Fixed v = original;
    for (int round = 0; round < 64 && calc.ok(); ++round) {
        Fixed candidate = v;
        const Fixed radius = calc.div(v, rot_radians);
        Vec2 point{position.x, position.y};
        if (speed < v && limits.acceleration.raw() > 0) {
            const Fixed gain = calc.div(calc.abs(calc.sub(calc.mul(speed, speed), calc.mul(v, v))),
                calc.mul(whole(2), limits.acceleration));
            point = {calc.add(point.x, calc.mul(h.x, gain)), calc.add(point.y, calc.mul(h.y, gain))};
        }
        const Vec2 left{calc.mul(calc.neg(h.y), radius), calc.mul(h.x, radius)};
        const Fixed to_left = calc.length(calc.sub(target_xy.x, calc.add(point.x, left.x)),
            calc.sub(target_xy.y, calc.add(point.y, left.y)));
        const Fixed to_right = calc.length(calc.sub(target_xy.x, calc.sub(point.x, left.x)),
            calc.sub(target_xy.y, calc.sub(point.y, left.y)));
        const Fixed distance = calc.length(calc.sub(target_xy.x, point.x), calc.sub(target_xy.y, point.y));
        const bool behind = calc.dot(calc.sub(target_xy.x, point.x), calc.sub(target_xy.y, point.y), h.x, h.y).raw() < 0;
        const Fixed diameter = calc.mul(whole(2), radius);
        if (!(radius <= to_left && radius <= to_right && (!behind || diameter <= distance))) {
            const Fixed factor = std::max(tenth, calc.div(calc.mul(distance, nine_tenths), diameter));
            candidate = std::min(calc.mul(factor, v), candidate);
            candidate = std::max(candidate, calc.mul(original, tenth));
        }
        if (limits.acceleration.raw() > 0) {
            // FoC reads the acceleration here, not the deceleration.
            Fixed braking_speed = v;
            Fixed needed = calc.add(calc.div(calc.mul(v, v), limits.acceleration), radius);
            while (to_target < needed && calc.ok()) {
                braking_speed = calc.mul(braking_speed, nine_tenths);
                if (!(calc.mul(v, tenth) <= braking_speed)) break;
                needed = calc.add(calc.div(calc.mul(braking_speed, braking_speed), limits.acceleration),
                    calc.div(braking_speed, rot_radians));
            }
            candidate = std::min(braking_speed, candidate);
        }
        if (!(calc.abs(calc.sub(candidate, v)) > speed_settle)) break;
        v = candidate;
    }
    return v;
}

// ---- Destination clip (AV-19, AV-20; research E266-01 to E266-05) ----

// Get_Nearest_Open_Position's retail constants: 40 rings (motion_internal.hpp), the first of
// radius 1.34 times the caller's start radius (0 from Find_Center_Path), and a query to frame 2^32 (binary32 4.2949673e9).
constexpr Fixed quarter = ratio(1, 4);
constexpr Fixed forever = whole(std::int64_t{1} << 32);

// The call of Find_Center_Path and of the formation slot mapping (#344): no turn allowance,
// start radius 0 and the SCT_ALL filter of a plain move. `ignore_group` is the slot mapping's
// ignore list (empty for Find_Center_Path); formation.cpp tests its placed slots, which FoC
// measures from the destination, not from the ring point.
[[nodiscard]] Vec2 open_position(Calc& calc, Work& work, Boxes& boxes, const AvoidanceRules& rules, const Footprint& footprint,
    const CollisionWorld& world, const EntityId entity, const Fixed now, const Vec2 position, const Vec2 destination,
    const std::span<const EntityId> ignore_group = {}) {
    // A point query: a square of the soft radius times OccupationRadiusCoefficientSpace, facing +X.
    const Fixed occupation = calc.mul(footprint.radius, rules.occupation_radius);
    LinearQuery query;
    query.start_frame = now;
    query.end_frame = forever;
    query.facing = {whole(1), Fixed{}};
    query.x_extent = occupation;
    query.y_extent = occupation;
    query.ignore = entity;
    query.ignore_group = ignore_group;
    const auto layer = dynamic_layer_index(footprint.layer);
    const TrackingLayerView* view = layer ? world.layers[*layer] : nullptr;
    const auto open = [&](const Vec2 point) {
        query.start = point;
        query.end = point;
        const auto mark = work.now();
        ++work.stats.queries;
        std::uint8_t hits = view != nullptr ? find_in_layer(calc, *view, world.interval, query, boxes, work.stats, facing_of(query)) : 0;
        if ((hits & collision_all) == 0 && world.statics != nullptr) {
            hits = static_cast<std::uint8_t>(hits
                | detect(calc, *world.statics, boxes.statics(calc, *world.statics), boxes, query, point, point, Fixed{},
                    Fixed{}, work.stats, facing_of(query)));
        }
        work.stats.query_ticks += work.now() - mark;
        return (hits & collision_all) == 0;
    };
    // Angle 0 points from the destination to the unit (a zero vector when it is there).
    const Vec2 toward = unit(calc, minus(calc, position, destination));
    Fixed radius{};
    for (int ring = 0; ring < motion_detail::destination_search_rings && calc.ok(); ++ring) {
        // The cap is not FoC's: validated footprints never reach it (motion_internal.hpp).
        const std::int64_t count = std::min(motion_detail::ring_points(calc, radius, occupation), motion_detail::max_ring_points);
        for (std::int64_t index = 0; index < count && calc.ok(); ++index) {
            // Counter-clockwise by index / count of a turn, at ((cos + 1) / 4 + 1 / 2) of the radius:
            // the full radius toward the unit, half of it on the far side.
            const Fixed turn = calc.div(whole(index), whole(count));
            const Fixed c = math::cos_turn(turn);
            const Fixed s = math::sin_turn(turn);
            const Vec2 direction{calc.sub(calc.mul(c, toward.x), calc.mul(s, toward.y)),
                calc.add(calc.mul(s, toward.x), calc.mul(c, toward.y))};
            const Fixed reach = calc.mul(calc.add(calc.mul(calc.add(c, whole(1)), quarter), half), radius);
            const Vec2 point{calc.add(destination.x, calc.mul(direction.x, reach)),
                calc.add(destination.y, calc.mul(direction.y, reach))};
            if (open(point) && calc.ok()) return point;
        }
        // With no direction every later point is the destination again: FoC queries it for
        // 40 rings and keeps it, so the search stops after ring 0.
        if (toward.x.raw() == 0 && toward.y.raw() == 0) break;
        radius = calc.add(radius, rules.destination_search_increment);
    }
    return destination;
}

} // namespace

std::optional<std::size_t> dynamic_layer_index(const SpaceLayer layer) noexcept {
    switch (layer) {
    case SpaceLayer::capital:
        return 0;
    case SpaceLayer::frigate:
        return 1;
    case SpaceLayer::corvette:
        return 2;
    case SpaceLayer::super_capital:
        return 3;
    default:
        return std::nullopt;
    }
}

core::Result<std::uint8_t> find_linear_collision(
    const TrackingLayerView& layer, const std::uint32_t interval, const LinearQuery& query) {
    Calc calc;
    PathSearchStats stats;
    Boxes boxes;
    const auto hits = find_in_layer(calc, layer, interval, query, boxes, stats, facing_of(query));
    if (!calc.ok()) return core::Result<std::uint8_t>::failure(calc.error("collision query"));
    return core::Result<std::uint8_t>::success(hits);
}

core::Result<std::uint8_t> find_dual_collision(
    const CollisionWorld& world, const SpaceLayer layer, const LinearQuery& query) {
    Calc calc;
    PathSearchStats stats;
    Boxes boxes;
    std::uint8_t hits = 0;
    if (const auto index = dynamic_layer_index(layer)) {
        if (const auto* view = world.layers[*index]; view != nullptr) {
            hits = find_in_layer(calc, *view, world.interval, query, boxes, stats, facing_of(query));
        }
    }
    if (world.statics != nullptr) {
        LinearQuery widened = query;
        widened.y_extent = calc.add(widened.y_extent, static_bonus);
        hits = static_cast<std::uint8_t>(
            hits | detect(calc, *world.statics, boxes.statics(calc, *world.statics), boxes, widened, widened.start,
                widened.end, Fixed{}, Fixed{}, stats, facing_of(widened)));
    }
    if (!calc.ok()) return core::Result<std::uint8_t>::failure(calc.error("collision query"));
    return core::Result<std::uint8_t>::success(hits);
}

core::Result<Prediction> predict(const MotionState& state, const std::uint64_t frame, const Vec3 position, const Fixed yaw) {
    if (state.kind == MotionKind::none || state.nodes.size() < 2) {
        return core::Result<Prediction>::success({position, yaw});
    }
    std::uint64_t at = std::max(frame, state.start_tick);
    if (state.kind == MotionKind::path) {
        // A path is over at its last node's frame; the unit stays where the frame before put it.
        const auto last = state.nodes.back().frame.raw();
        const auto end_tick = static_cast<std::uint64_t>((last + Fixed::scale - 1) / Fixed::scale);
        if (end_tick > state.start_tick && at >= end_tick) at = end_tick - 1;
    }
    auto sample = sample_motion(state, at, state.start_position, state.start_yaw);
    if (!sample) return core::Result<Prediction>::failure(sample.error());
    Vec3 where = sample.value().position;
    if (state.kind == MotionKind::turn) where = position;
    where.z = position.z;
    return core::Result<Prediction>::success({where, sample.value().yaw});
}

core::Result<Vec2> nearest_open_position(const AvoidanceRules& rules, const Footprint& footprint,
    const CollisionWorld& world, const EntityId entity, const std::uint64_t tick, const Vec2 position,
    const Vec2 destination, const std::span<const EntityId> ignore_group) {
    Calc calc;
    if (tick > max_ticks) return core::Result<Vec2>::success(destination);
    Work work;
    work.stats.entity = entity;
    work.stats.slot = true;
    const auto began = work.now();
    Boxes boxes;
    const Vec2 open = open_position(calc, work, boxes, rules, footprint, world, entity, whole(static_cast<std::int64_t>(tick)),
        position, destination, ignore_group);
    work.stats.total_ticks = work.now() - began;
    if (work.probe != nullptr) work.probe->searched(work.stats);
    if (!calc.ok()) return core::Result<Vec2>::failure(calc.error("nearest open position"));
    return core::Result<Vec2>::success(open);
}

namespace {

// One plan_space_move call as a resumable search (PC-08, #520). The prelude (the target's open
// position, the 40-unit rule, the speed reduction) runs at the first run(); then FoC's tries
// (AV-14, research E71-11, E71-22), in PC-06's segments for the bounded form. run() stops when
// the search ends or its expansions reach a count, only ever between two parents, so running
// it at once or in slices gives the same result bit for bit.
class Search final {
public:
    Search(const MotionTable& table, const MotionProfile& limits, const Footprint& footprint, const CollisionWorld& world,
        const EntityId entity, const std::uint64_t tick, const Vec3 position, const Fixed yaw, const Fixed speed,
        const Vec3 target, const PathSearchMode mode, SearchScratch& scratch)
        : table_(table), limits_(limits), footprint_(footprint), world_(world), entity_(entity), tick_(tick),
          position_(position), yaw_(yaw), speed_(speed), target_(target), mode_(mode), scratch_(scratch) {
        work_.stats.entity = entity;
    }
    Search(const Search&) = delete;
    Search& operator=(const Search&) = delete;

    // Runs until the search ends (true) or its expansions reach `until` (false).
    bool run(const std::uint64_t until) {
        if (!started_) {
            started_ = true;
            prelude();
        }
        while (!outcome_) {
            if (running_) {
                const auto step = finder_->resume(farthest_, nodes_, until);
                if (step == Finder::Step::running) return false;
                running_ = false;
                if (step == Finder::Step::found) {
                    finish(true);
                } else if (!finder_->ok()) {
                    finish(false);
                } else {
                    next_try();
                }
                continue;
            }
            if (segment_ == segment_count_) {
                finish(false);
                continue;
            }
            const auto& segment = segments_[segment_];
            if (attempt_ == segment.tries) {
                if (++segment_ < segment_count_) start_segment();
                continue;
            }
            const Fixed weight = calc_.div(whole(1), previous_coefficient_);
            if (attempt_ >= segment.first_try) {
                const auto step = finder_->begin(config_, weight, nodes_);
                if (step == Finder::Step::found) {
                    finish(true);
                    continue;
                }
                if (step == Finder::Step::running) {
                    running_ = true;
                    continue;
                }
                if (!finder_->ok()) {
                    finish(false);
                    continue;
                }
            }
            next_try();
        }
        return true;
    }

    [[nodiscard]] bool ended() const noexcept { return outcome_.has_value(); }
    [[nodiscard]] const core::Result<MotionState>& outcome() const { return *outcome_; }
    [[nodiscard]] Work& work() noexcept { return work_; }

private:
    struct Segment {
        Config config;
        std::uint32_t first_try{};
        std::uint32_t tries{};
    };

    void prelude() {
        if (!table_.avoidance || !motion_detail::within_coordinates(position_) || !motion_detail::within_coordinates(target_)
            || tick_ > max_ticks) {
            outcome_ = core::Result<MotionState>::success(MotionState{});
            return;
        }
        const auto& rules = *table_.avoidance;
        // A group's centre path first moves the target to its nearest open position (AV-19), then
        // plans to it in the unit's height (MV-11); the 40-unit rule measures the moved target.
        open_ = open_position(calc_, work_, boxes_, rules, footprint_, world_, entity_,
            whole(static_cast<std::int64_t>(tick_)), {position_.x, position_.y}, {target_.x, target_.y});
        const Vec3 end{open_.x, open_.y, position_.z};
        length_ = calc_.length(calc_.sub(end.x, position_.x), calc_.sub(end.y, position_.y));
        if (!calc_.ok()) {
            outcome_ = core::Result<MotionState>::failure(calc_.error("move plan"));
            return;
        }
        if (!motion_detail::within_coordinates(end) || length_ < min_move_length) {
            // research E71-22: no path under 40 units, the move ends
            outcome_ = core::Result<MotionState>::success(MotionState{});
            return;
        }
        start_yaw_ = clamp180(yaw_);
        Config config;
        config.max_speed = reduced_speed(calc_, limits_, position_, start_yaw_, speed_, end);
        config.rate_of_turn = limits_.rate_of_turn;
        config.radius = footprint_.radius;
        config.x_extent = footprint_.x_extent;
        config.y_extent = footprint_.y_extent;
        config.max_expansions = rules.max_expansions;
        if (!calc_.ok()) {
            outcome_ = core::Result<MotionState>::failure(calc_.error("move speed"));
            return;
        }
        finder_.emplace(table_, rules, limits_, footprint_, world_, entity_, whole(static_cast<std::int64_t>(tick_)),
            position_, start_yaw_, speed_, end, work_, boxes_, scratch_);
        if (mode_ == PathSearchMode::exact) {
            segments_[0] = {config, 0, rules.tries};
            segment_count_ = 1;
        } else {
            // PC-06: FoC's first try up to bounded_first_expansions; FoC's second try as it is
            // (it reads nothing of the first); FoC's later tries with the estimate weighted.
            Config first = config;
            first.max_expansions = std::min(config.max_expansions, bounded_first_expansions);
            Config weighted = config;
            weighted.estimate_weight = bounded_estimate_weight;
            segments_[0] = {first, 0, 1};
            segments_[1] = {config, 1, 2};
            segments_[2] = {weighted, bounded_weighted_try, rules.tries};
            segment_count_ = 3;
        }
        start_segment();
    }

    // A segment runs the tries from its first_try (the earlier ones only set up the later
    // ones' settings) up to its tries, from its configuration.
    void start_segment() {
        attempt_ = 0;
        previous_coefficient_ = whole(1);
        config_ = segments_[segment_].config;
    }

    // The settings of the try after a failed one (AV-14). `farthest_` is the tries' farthest
    // reach, which the fourth try's cutoff reads; the segments share it.
    void next_try() {
        const auto& rules = *table_.avoidance;
        previous_coefficient_ = config_.path_cost_coefficient;
        config_.allow_wait = false;
        config_.only_finish_on_end = false;
        if (attempt_ != 0) {
            config_.rotation_coefficient = calc_.add(config_.rotation_coefficient, rules.failure_rotation);
            config_.forward_coefficient = calc_.add(config_.forward_coefficient, rules.failure_forward);
            config_.max_expansions = static_cast<std::uint32_t>(
                calc_.mul(rules.failure_expansions, whole(config_.max_expansions)).raw() / Fixed::scale);
            if (attempt_ > 1) {
                config_.distance_cutoff = std::max(
                    calc_.add(config_.distance_cutoff, calc_.mul(length_, rules.failure_cutoff)), farthest_);
                config_.path_cost_coefficient = calc_.mul(config_.path_cost_coefficient, rules.path_cost_coefficient);
                if (attempt_ > 2) {
                    config_.penalize_filtered = true;
                    config_.mask = static_cast<std::uint8_t>(collision_static | collision_moving);
                    if (attempt_ > 3) config_.max_speed = calc_.mul(config_.max_speed, half);
                }
            }
        }
        ++attempt_;
        if (!calc_.ok()) finish(false);
    }

    void finish(const bool found) {
        if (!finder_->ok()) {
            outcome_ = core::Result<MotionState>::failure(finder_->error());
            return;
        }
        if (!calc_.ok()) {
            outcome_ = core::Result<MotionState>::failure(calc_.error("move retry"));
            return;
        }
        MotionState state;
        if (found && nodes_.size() >= 2) {
            state.kind = MotionKind::path;
            state.start_tick = tick_;
            state.start_position = position_;
            state.start_yaw = start_yaw_;
            state.start_speed = speed_;
            state.target = {open_.x, open_.y, target_.z};
            state.nodes = std::move(nodes_);
        } // else every try failed: the unit stays (AV-U5)
        outcome_ = core::Result<MotionState>::success(std::move(state));
    }

    const MotionTable& table_;
    const MotionProfile& limits_;
    const Footprint& footprint_;
    const CollisionWorld& world_;
    EntityId entity_;
    std::uint64_t tick_;
    Vec3 position_;
    Fixed yaw_;
    Fixed speed_;
    Vec3 target_;
    PathSearchMode mode_;
    SearchScratch& scratch_;
    Calc calc_;
    Work work_;
    Boxes boxes_;
    std::optional<Finder> finder_;
    bool started_{};
    bool running_{};
    Vec2 open_{};
    Fixed length_;
    Fixed start_yaw_;
    std::array<Segment, 3> segments_{};
    std::size_t segment_count_{};
    std::size_t segment_{};
    std::uint32_t attempt_{};
    Fixed previous_coefficient_ = whole(1);
    Config config_;
    Fixed farthest_{};
    std::vector<PathNode> nodes_;
    std::optional<core::Result<MotionState>> outcome_;
};

constexpr std::uint64_t unlimited = ~std::uint64_t{0};

// Runs `search` until it ends or reaches `until` and reports the call's work (PathSearchStats).
bool run_reported(Search& search, const std::uint64_t until, const PathSearchPart part, PathSearchStats* const stats) {
    auto& work = search.work();
    const auto before = work.stats;
    const auto began = work.now();
    const bool ended = search.run(until);
    work.stats.total_ticks += work.now() - began;
    PathSearchStats call = work.stats;
    if (part == PathSearchPart::slice) {
        // A slice reports its own share of the search's work.
        call.tries -= before.tries;
        call.expansions -= before.expansions;
        call.children -= before.children;
        call.queries -= before.queries;
        call.windows -= before.windows;
        call.leaves -= before.leaves;
        call.narrow -= before.narrow;
        call.total_ticks -= before.total_ticks;
        call.query_ticks -= before.query_ticks;
        call.set_ticks -= before.set_ticks;
    }
    call.part = part == PathSearchPart::slice && ended ? PathSearchPart::last_slice
        : part == PathSearchPart::whole && !ended      ? PathSearchPart::abandoned
                                                       : part;
    if (work.probe != nullptr) work.probe->searched(call);
    if (stats != nullptr) *stats = call;
    return ended;
}

} // namespace

void set_path_search_probe(PathSearchProbe* const probe) noexcept {
    installed_probe.store(probe, std::memory_order_release);
}

core::Result<MotionState> plan_space_move(const MotionTable& table, const MotionProfile& limits,
    const Footprint& footprint, const CollisionWorld& world, const EntityId entity, const std::uint64_t tick,
    const Vec3 position, const Fixed yaw, const Fixed speed, const Vec3 target, PathSearchStats* const stats,
    const PathSearchMode mode) {
    Search search(table, limits, footprint, world, entity, tick, position, yaw, speed, target, mode, search_scratch());
    static_cast<void>(run_reported(search, unlimited, PathSearchPart::whole, stats));
    return search.outcome();
}

core::Result<std::optional<MotionState>> plan_space_move_within(const MotionTable& table, const MotionProfile& limits,
    const Footprint& footprint, const CollisionWorld& world, const EntityId entity, const std::uint64_t tick,
    const Vec3 position, const Fixed yaw, const Fixed speed, const Vec3 target, const std::uint64_t budget,
    PathSearchStats* const stats) {
    Search search(
        table, limits, footprint, world, entity, tick, position, yaw, speed, target, PathSearchMode::bounded, search_scratch());
    if (!run_reported(search, budget, PathSearchPart::whole, stats)) {
        return core::Result<std::optional<MotionState>>::success(std::nullopt);
    }
    const auto& outcome = search.outcome();
    if (!outcome) return core::Result<std::optional<MotionState>>::failure(outcome.error());
    return core::Result<std::optional<MotionState>>::success(std::optional<MotionState>(outcome.value()));
}

// The sliced search owns copies of what it reads: the unit's layer view and the static layer
// as they were when it started, its limits and footprint, and its own scratch.
struct SlicedPathSearch::State {
    State(const MotionTable& table, const MotionProfile& limits_in, const Footprint& footprint_in,
        const CollisionWorld& world_in, const EntityId entity, const std::uint64_t tick, const Vec3 position, const Fixed yaw,
        const Fixed speed, const Vec3 target)
        : limits(limits_in), footprint(footprint_in),
          search(table, limits, footprint, world, entity, tick, position, yaw, speed, target, PathSearchMode::bounded,
              scratch) {
        world.interval = world_in.interval;
        if (const auto index = dynamic_layer_index(footprint.layer); index && world_in.layers[*index] != nullptr) {
            layer = *world_in.layers[*index];
            world.layers[*index] = &layer;
        }
        if (world_in.statics != nullptr) {
            statics = *world_in.statics;
            world.statics = &statics;
        }
    }
    MotionProfile limits;
    Footprint footprint;
    TrackingLayerView layer;
    std::vector<TrackedLeaf> statics;
    CollisionWorld world;
    SearchScratch scratch;
    Search search;
};

SlicedPathSearch::SlicedPathSearch(const MotionTable& table, const MotionProfile& limits, const Footprint& footprint,
    const CollisionWorld& world, const EntityId entity, const std::uint64_t tick, const Vec3 position, const Fixed yaw,
    const Fixed speed, const Vec3 target)
    : state_(std::make_unique<State>(table, limits, footprint, world, entity, tick, position, yaw, speed, target)) {}

SlicedPathSearch::SlicedPathSearch(SlicedPathSearch&&) noexcept = default;
SlicedPathSearch& SlicedPathSearch::operator=(SlicedPathSearch&&) noexcept = default;
SlicedPathSearch::~SlicedPathSearch() = default;

bool SlicedPathSearch::run(const std::uint64_t expansions, PathSearchStats* const stats) {
    auto& search = state_->search;
    if (search.ended()) return true;
    const auto spent = search.work().stats.expansions;
    const auto until = expansions > unlimited - spent ? unlimited : spent + expansions;
    return run_reported(search, until, PathSearchPart::slice, stats);
}

bool SlicedPathSearch::ended() const noexcept { return state_->search.ended(); }

core::Result<MotionState> SlicedPathSearch::result() const {
    if (!state_->search.ended()) {
        return core::Result<MotionState>::failure(
            core::Diagnostic{"sim.tactical.path_search_running", core::Severity::error, "the sliced path search has not ended", {}, {}, {}, {}});
    }
    return state_->search.outcome();
}

std::uint64_t SlicedPathSearch::expansions() const noexcept { return state_->search.work().stats.expansions; }

std::size_t SlicedPathSearch::held_bytes() const noexcept {
    std::size_t bytes = sizeof(State) + state_->statics.capacity() * sizeof(TrackedLeaf);
    for (const auto& window : state_->layer.windows) bytes += window.capacity() * sizeof(TrackedLeaf);
    bytes += state_->layer.windows.capacity() * sizeof(std::vector<TrackedLeaf>);
    bytes += state_->scratch.components.capacity() * sizeof(Component);
    bytes += state_->scratch.records.held_bytes() + state_->scratch.open.held_bytes();
    return bytes;
}

} // namespace eawr::sim::tactical
