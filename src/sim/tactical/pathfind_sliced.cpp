#include "pathfind_search_internal.hpp"

namespace eawr::sim::tactical::pathfind_detail {

[[nodiscard]] Fixed reduced_speed(Calc& calc, const MotionProfile& limits, const Vec3 position, const Fixed yaw,
    const Fixed speed, const Vec3 target);

Search::Search(const MotionTable& table, const MotionProfile& limits, const Footprint& footprint, const CollisionWorld& world,
        const EntityId entity, const std::uint64_t tick, const Vec3 position, const Fixed yaw, const Fixed speed,
        const Vec3 target, const PathSearchMode mode, SearchScratch& scratch, const bool through_hazards,
        const std::optional<Vec3> filter_destination)
        : table_(table), limits_(limits), footprint_(footprint), world_(world), entity_(entity), tick_(tick),
          position_(position), yaw_(yaw), speed_(speed), target_(target), mode_(mode),
          through_hazards_(through_hazards), filter_destination_(filter_destination.value_or(target)), scratch_(scratch) {
        work_.stats.entity = entity;
    }

bool Search::run(const std::uint64_t until) {
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

void Search::prelude() {
        if (!table_.avoidance || !motion_detail::within_coordinates(position_) || !motion_detail::within_coordinates(target_)
            || tick_ > max_ticks) {
            outcome_ = core::Result<MotionState>::success(MotionState{});
            return;
        }
        const auto& rules = *table_.avoidance;
        original_filter_ = context_filter(calc_, work_, boxes_, world_, footprint_, entity_,
            whole(static_cast<std::int64_t>(tick_)), {position_.x, position_.y},
            {filter_destination_.x, filter_destination_.y}, through_hazards_);
        // A group's centre path first moves the target to its nearest open position (AV-19), then
        // plans to it in the unit's height (MV-11); the 40-unit rule measures the moved target.
        open_ = open_position(calc_, work_, boxes_, rules, footprint_, world_, entity_,
            whole(static_cast<std::int64_t>(tick_)), {position_.x, position_.y}, {target_.x, target_.y}, {}, original_filter_);
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
        config.mask = original_filter_;
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

void Search::start_segment() {
        attempt_ = 0;
        previous_coefficient_ = whole(1);
        config_ = segments_[segment_].config;
    }

void Search::next_try() {
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
                    config_.penalize_filtered = original_filter_ == collision_all;
                    config_.mask = static_cast<std::uint8_t>(collision_static | collision_moving);
                    if (attempt_ > 3) config_.max_speed = calc_.mul(config_.max_speed, half);
                }
            }
        }
        ++attempt_;
        if (!calc_.ok()) finish(false);
    }

void Search::finish(const bool found) {
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

} // namespace eawr::sim::tactical::pathfind_detail

namespace eawr::sim::tactical {
using namespace pathfind_detail;

// The sliced search owns copies of what it reads: the unit's layer view and the static layer
// as they were when it started, its limits and footprint, and its own scratch.
struct SlicedPathSearch::State {
    State(const MotionTable& table, const MotionProfile& limits_in, const Footprint& footprint_in,
        const CollisionWorld& world_in, const EntityId entity, const std::uint64_t tick, const Vec3 position, const Fixed yaw,
        const Fixed speed, const Vec3 target, const bool through_hazards, const std::optional<Vec3> filter_destination)
        : limits(limits_in), footprint(footprint_in),
          search(table, limits, footprint, world, entity, tick, position, yaw, speed, target, PathSearchMode::bounded,
              scratch, through_hazards, filter_destination) {
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
    const Fixed speed, const Vec3 target, const bool through_hazards, const std::optional<Vec3> filter_destination)
    : state_(std::make_unique<State>(table, limits, footprint, world, entity, tick, position, yaw, speed, target,
          through_hazards, filter_destination)) {}

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
