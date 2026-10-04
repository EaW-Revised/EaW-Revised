#include "pathfind_internal.hpp"

namespace eawr::sim::tactical::pathfind_detail {

// ---- Destination clip (AV-19, AV-20; research E266-01 to E266-05) ----

// Get_Nearest_Open_Position's retail constants: 40 rings (motion_internal.hpp), the first of
// radius 1.34 times the caller's start radius (0 from Find_Center_Path), and a query to frame 2^32 (binary32 4.2949673e9).
constexpr Fixed quarter = ratio(1, 4);
constexpr Fixed forever = whole(std::int64_t{1} << 32);

std::uint8_t context_filter(Calc& calc, Work& work, Boxes& boxes, const CollisionWorld& world,
    const Footprint& footprint, const EntityId entity, const Fixed now, const Vec2 position,
    const Vec2 destination, const bool through_hazards) {
    std::uint8_t endpoint_hits = 0;
    if (world.statics != nullptr) {
        LinearQuery query;
        query.start_frame = now;
        query.end_frame = forever;
        query.facing = {whole(1), Fixed{}};
        query.ignore = entity;
        // WHZ-08a: centre XY, zero extents, static layer only. Hull/height do not
        // grant hazard entry, and ordinary moving/static/solid objects stay selected.
        for (const auto point : {destination, position}) {
            query.start = query.end = point;
            ++work.stats.queries;
            endpoint_hits = static_cast<std::uint8_t>(endpoint_hits | detect(calc, *world.statics,
                boxes.statics(calc, *world.statics), boxes, query, point, point, Fixed{}, Fixed{},
                work.stats, facing_of(query)));
        }
    }
    auto filter = static_cast<std::uint8_t>(collision_all & ~endpoint_hits);
    if (!footprint.asteroid_damage) filter = static_cast<std::uint8_t>(filter & ~collision_field);
    if (through_hazards) filter = static_cast<std::uint8_t>(filter & ~(collision_field | collision_storm | collision_nebula));
    return static_cast<std::uint8_t>(filter | collision_moving | collision_static | collision_impassable);
}

// The call of Find_Center_Path and of the formation slot mapping (#344): no turn allowance,
// start radius 0 and the context-derived movement filter. `ignore_group` is the slot mapping's
// ignore list (empty for Find_Center_Path); formation.cpp tests its placed slots, which FoC
// measures from the destination, not from the ring point.
[[nodiscard]] Vec2 open_position(Calc& calc, Work& work, Boxes& boxes, const AvoidanceRules& rules, const Footprint& footprint,
    const CollisionWorld& world, const EntityId entity, const Fixed now, const Vec2 position, const Vec2 destination,
    const std::span<const EntityId> ignore_group, const std::uint8_t filter) {
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
        if ((hits & filter) == 0 && world.statics != nullptr) {
            hits = static_cast<std::uint8_t>(hits
                | detect(calc, *world.statics, boxes.statics(calc, *world.statics), boxes, query, point, point, Fixed{},
                    Fixed{}, work.stats, facing_of(query)));
        }
        work.stats.query_ticks += work.now() - mark;
        return (hits & filter) == 0;
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

} // namespace eawr::sim::tactical::pathfind_detail

namespace eawr::sim::tactical {
using namespace pathfind_detail;

core::Result<std::uint8_t> find_linear_collision(
    const TrackingLayerView& layer, const std::uint32_t interval, const LinearQuery& query) {
    Calc calc;
    PathSearchStats stats;
    Boxes boxes;
    const auto hits = find_in_layer(calc, layer, interval, query, boxes, stats, facing_of(query));
    if (!calc.ok()) return core::Result<std::uint8_t>::failure(calc.error("collision query"));
    return core::Result<std::uint8_t>::success(hits);
}

core::Result<std::uint8_t> find_static_collision(const std::vector<TrackedLeaf>& statics, const LinearQuery& query) {
    Calc calc;
    PathSearchStats stats;
    Boxes boxes;
    const auto hits = detect(calc, statics, boxes.statics(calc, statics), boxes, query, query.start,
        query.end, Fixed{}, Fixed{}, stats, facing_of(query));
    if (!calc.ok()) return core::Result<std::uint8_t>::failure(calc.error("arrival static collision query"));
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

core::Result<Vec2> nearest_open_position(const AvoidanceRules& rules, const Footprint& footprint,
    const CollisionWorld& world, const EntityId entity, const std::uint64_t tick, const Vec2 position,
    const Vec2 destination, const std::span<const EntityId> ignore_group, const std::uint8_t filter) {
    Calc calc;
    if (tick > max_ticks) return core::Result<Vec2>::success(destination);
    Work work;
    work.stats.entity = entity;
    work.stats.slot = true;
    const auto began = work.now();
    Boxes boxes;
    const Vec2 open = open_position(calc, work, boxes, rules, footprint, world, entity, whole(static_cast<std::int64_t>(tick)),
        position, destination, ignore_group, filter);
    work.stats.total_ticks = work.now() - began;
    if (work.probe != nullptr) work.probe->searched(work.stats);
    if (!calc.ok()) return core::Result<Vec2>::failure(calc.error("nearest open position"));
    return core::Result<Vec2>::success(open);
}

core::Result<std::uint8_t> movement_collision_filter(const CollisionWorld& world, const Footprint& footprint,
    const EntityId entity, const std::uint64_t tick, const Vec3 position, const Vec3 destination, const bool through_hazards) {
    Calc calc;
    Work work;
    Boxes boxes;
    const auto filter = context_filter(calc, work, boxes, world, footprint, entity,
        whole(static_cast<std::int64_t>(tick)), {position.x, position.y}, {destination.x, destination.y}, through_hazards);
    if (!calc.ok()) return core::Result<std::uint8_t>::failure(calc.error("movement collision filter"));
    return core::Result<std::uint8_t>::success(filter);
}

} // namespace eawr::sim::tactical
