#pragma once

#include "pathfind_internal.hpp"

namespace eawr::sim::tactical::pathfind_detail {

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


[[nodiscard]] SearchScratch& search_scratch();

// Uniform_Float_To_Int: truncation for x >= 0, truncation of x - 1 below (research E71-04).
[[nodiscard]] inline std::int64_t uniform_int(const Fixed value) noexcept {
    if (value.raw() >= 0) return value.raw() / Fixed::scale;
    return (value.raw() - Fixed::scale) / Fixed::scale;
}

class Finder final {
public:
    Finder(const MotionTable& table, const AvoidanceRules& rules, const MotionProfile& limits, const Footprint& footprint,
        const CollisionWorld& world, const EntityId entity, const Fixed now, const Vec3 start, const Fixed start_yaw,
        const Fixed start_speed, const Vec3 end, Work& work, Boxes& boxes, SearchScratch& scratch);

    enum class Step : std::uint8_t { found, failed, running };

    // Starts one path finder try (AV-14): `found` or `failed` when it ends at once (the trivial path,
    // MV-12), `running` with the origin open otherwise; resume() runs it. `weight` is the
    // estimate weight below the distance cutoff (research E71-23).
    Step begin(const Config& config, const Fixed weight, std::vector<PathNode>& nodes);

    // Runs the started call until it ends, or until the search's expansions (every try's) reach
    // `until` (PC-08): the count is read before each parent, so a slice ends between two
    // parents and running the call in slices takes the same steps as running it at once.
    Step resume(Fixed& farthest, std::vector<PathNode>& nodes, const std::uint64_t until);

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
    void initialize();

    // Have_Reached_Destination without a final facing.
    [[nodiscard]] bool reached(const Signature& here, const int op) const noexcept;

    // Compute_Frame_And_Velocity (research E71-05).
    void frame_and_velocity(const Fixed frame, const Fixed v, const Fixed distance, const int op, Component& child);

    // Build_Expansion, unaligned set (research E71-02).
    bool expand(const int op, const Component& parent, const motion_detail::Heading& h, Component& child);

    // Compute_Unaligned_Forward_Match_Expansion: the turn on the turn circle to the tangent that
    // points at the target (MV-15), legal for a bearing difference in [0.01, theta].
    bool match(const Component& parent, const motion_detail::Heading& h, Component& child);

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
    void build_final(const int last, std::vector<PathNode>& nodes);

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


class Search final {
public:
    Search(const MotionTable& table, const MotionProfile& limits, const Footprint& footprint, const CollisionWorld& world,
        const EntityId entity, const std::uint64_t tick, const Vec3 position, const Fixed yaw, const Fixed speed,
        const Vec3 target, const PathSearchMode mode, SearchScratch& scratch, bool through_hazards = false,
        std::optional<Vec3> filter_destination = std::nullopt);
    Search(const Search&) = delete;
    Search& operator=(const Search&) = delete;

    // Runs until the search ends (true) or its expansions reach `until` (false).
    bool run(const std::uint64_t until);

    [[nodiscard]] bool ended() const noexcept { return outcome_.has_value(); }
    [[nodiscard]] const core::Result<MotionState>& outcome() const { return *outcome_; }
    [[nodiscard]] Work& work() noexcept { return work_; }

private:
    struct Segment {
        Config config;
        std::uint32_t first_try{};
        std::uint32_t tries{};
    };

    void prelude();

    // A segment runs the tries from its first_try (the earlier ones only set up the later
    // ones' settings) up to its tries, from its configuration.
    void start_segment();

    // The settings of the try after a failed one (AV-14). `farthest_` is the tries' farthest
    // reach, which the fourth try's cutoff reads; the segments share it.
    void next_try();

    void finish(const bool found);

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
    bool through_hazards_{};
    Vec3 filter_destination_;
    std::uint8_t original_filter_{collision_all};
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

bool run_reported(Search& search, const std::uint64_t until, const PathSearchPart part, PathSearchStats* const stats);

} // namespace eawr::sim::tactical::pathfind_detail
