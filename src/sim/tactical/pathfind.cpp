#include "pathfind_search_internal.hpp"

// docs/behaviour/space-movement.md, "Avoidance": the rules are cited there (AV-nn) with their
// research evidence; this file follows FoC's space path finder and object tracking
// step by step, in Q24 where FoC uses binary32.
namespace eawr::sim::tactical {

using namespace pathfind_detail;

namespace pathfind_detail {
// The probe path_bench installs (#520); null reads no clock.
std::atomic<PathSearchProbe*> installed_probe{nullptr};

} // namespace pathfind_detail

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

std::uint8_t tracking_category(const Footprint& footprint, const bool moving) noexcept {
    if (footprint.asteroid_field) return collision_field;
    if (footprint.ion_storm) return collision_storm;
    if (footprint.nebula) return collision_nebula;
    if (footprint.impassable_asteroid) return collision_impassable;
    return moving ? collision_moving : collision_static;
}

core::Result<TrackedLeaf> tracking_leaf(const EntityId id, const Footprint& footprint,
    const Prediction& from, const Prediction& to) {
    Calc calc;
    const auto center = [&](const Prediction& prediction) {
        if (footprint.obstacle_offset == Vec2{}) return Vec2{prediction.position.x, prediction.position.y};
        const auto direction = heading(calc, prediction.yaw);
        return Vec2{calc.add(prediction.position.x, calc.sub(calc.mul(footprint.obstacle_offset.x, direction.x),
            calc.mul(footprint.obstacle_offset.y, direction.y))),
            calc.add(prediction.position.y, calc.add(calc.mul(footprint.obstacle_offset.x, direction.y),
            calc.mul(footprint.obstacle_offset.y, direction.x)))};
    };
    TrackedLeaf leaf;
    leaf.entity = id;
    leaf.start = center(from);
    leaf.end = center(to);
    if (footprint.obstacle) {
        leaf.facing = {whole(1), Fixed{}};
        leaf.x_extent = footprint.radius;
        leaf.y_extent = footprint.radius;
        leaf.reach = footprint.radius;
    } else {
        leaf.x_extent = footprint.x_extent;
        leaf.y_extent = footprint.y_extent;
        leaf.reach = std::max(footprint.x_extent, footprint.y_extent);
        if (leaf.start != leaf.end) {
            const auto dx = calc.sub(leaf.end.x, leaf.start.x);
            const auto dy = calc.sub(leaf.end.y, leaf.start.y);
            const auto length = calc.length(dx, dy);
            leaf.facing = {calc.div(dx, length), calc.div(dy, length)};
        } else {
            const auto direction = heading(calc, to.yaw);
            leaf.facing = {direction.x, direction.y};
        }
    }
    leaf.collision = tracking_category(footprint, leaf.start != leaf.end);
    if (!calc.ok()) return core::Result<TrackedLeaf>::failure(calc.error("tracking footprint"));
    return core::Result<TrackedLeaf>::success(leaf);
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

void set_path_search_probe(PathSearchProbe* const probe) noexcept {
    installed_probe.store(probe, std::memory_order_release);
}

core::Result<MotionState> plan_space_move(const MotionTable& table, const MotionProfile& limits,
    const Footprint& footprint, const CollisionWorld& world, const EntityId entity, const std::uint64_t tick,
    const Vec3 position, const Fixed yaw, const Fixed speed, const Vec3 target, PathSearchStats* const stats,
    const PathSearchMode mode, const bool through_hazards, const std::optional<Vec3> filter_destination) {
    Search search(table, limits, footprint, world, entity, tick, position, yaw, speed, target, mode,
        search_scratch(), through_hazards, filter_destination);
    static_cast<void>(run_reported(search, unlimited, PathSearchPart::whole, stats));
    return search.outcome();
}

core::Result<std::optional<MotionState>> plan_space_move_within(const MotionTable& table, const MotionProfile& limits,
    const Footprint& footprint, const CollisionWorld& world, const EntityId entity, const std::uint64_t tick,
    const Vec3 position, const Fixed yaw, const Fixed speed, const Vec3 target, const std::uint64_t budget,
    PathSearchStats* const stats, const bool through_hazards, const std::optional<Vec3> filter_destination) {
    Search search(
        table, limits, footprint, world, entity, tick, position, yaw, speed, target, PathSearchMode::bounded,
        search_scratch(), through_hazards, filter_destination);
    if (!run_reported(search, budget, PathSearchPart::whole, stats)) {
        return core::Result<std::optional<MotionState>>::success(std::nullopt);
    }
    const auto& outcome = search.outcome();
    if (!outcome) return core::Result<std::optional<MotionState>>::failure(outcome.error());
    return core::Result<std::optional<MotionState>>::success(std::optional<MotionState>(outcome.value()));
}

} // namespace eawr::sim::tactical
