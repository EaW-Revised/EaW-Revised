#include "eawr/sim/tactical/formation.hpp"

#include "eawr/sim/math/math.hpp"
#include "motion_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

// docs/behaviour/space-movement.md, "Formations": the rules are cited there (FM-nn) with their
// research evidence; this file follows FoC's space movement coordinator step by step, in Q24
// where FoC uses binary32.
namespace eawr::sim::tactical {
namespace {

using math::Fixed;
using math::Vec2;
using math::Vec3;
using motion_detail::Calc;
using motion_detail::clamp180;
using motion_detail::whole;

[[nodiscard]] constexpr Fixed ratio(const std::int64_t numerator, const std::int64_t denominator) noexcept {
    return Fixed::from_raw(numerator * Fixed::scale / denominator);
}

constexpr Fixed half = ratio(1, 2);
constexpr Fixed quarter = ratio(1, 4);
constexpr Fixed min_line_step = whole(50);
constexpr std::uint32_t line_candidates = 30;
// The slot probe's end frame: binary32 4.2949673e9, which is 2^32.
constexpr Fixed probe_end = Fixed::from_raw((std::int64_t{1} << 32) * Fixed::scale);
// The layers' processing order (FM-02, research E344-02).
constexpr std::array<SpaceLayer, 4> bucket_order{
    SpaceLayer::capital, SpaceLayer::frigate, SpaceLayer::corvette, SpaceLayer::super_capital};

[[nodiscard]] Fixed length3(Calc& calc, const Vec3 value) { return calc.length(value); }

[[nodiscard]] Vec3 minus3(Calc& calc, const Vec3 a, const Vec3 b) {
    return {calc.sub(a.x, b.x), calc.sub(a.y, b.y), calc.sub(a.z, b.z)};
}

// A unit XY direction, or nullopt for the zero vector.
[[nodiscard]] std::optional<Vec2> direction(Calc& calc, const Fixed x, const Fixed y) {
    if (x.raw() == 0 && y.raw() == 0) return std::nullopt;
    const Fixed length = calc.length(x, y);
    if (length.raw() == 0) return std::nullopt;
    return Vec2{calc.div(x, length), calc.div(y, length)};
}

struct Mapped {
    std::size_t member{};
    Vec3 destination{};
};

// The debug build's nearest open position search for a slot (FM-05, research E344-05,
// E344-06, E266-01): the plain move's search (AV-19) with the layer's group ignored. FoC tests
// the mapped slots against `point` itself, not against the ring point, so a point closer to a
// mapped slot than the two occupation radii rejects every ring and is kept (FM-05a).
[[nodiscard]] Vec3 nearest_open(Calc& calc, const FormationMember& member, const Vec3 point, const CollisionWorld& world,
    const std::uint64_t frame, std::span<const EntityId> bucket_ids, const std::vector<std::pair<Vec3, Fixed>>& placed,
    const AvoidanceRules& rules, std::optional<core::Diagnostic>& failure, const std::uint8_t filter) {
    for (const auto& [other, other_radius] : placed) {
        if (length3(calc, minus3(calc, point, other)) < calc.add(member.occupation_radius, other_radius)) return point;
    }
    Footprint footprint;
    footprint.layer = member.layer;
    footprint.radius = member.soft_radius;
    const auto open = nearest_open_position(rules, footprint, world, member.entity, frame,
        {member.position.x, member.position.y}, {point.x, point.y}, bucket_ids, filter);
    if (!open) {
        failure = open.error();
        return point;
    }
    return {open.value().x, open.value().y, point.z};
}

// The search along a slot's ray (FM-05, research E344-06): the first free point on the ray from
// `anchor` along `d`, one step of max(50, r / 4) at a time, 30 points at most; else the open
// position nearest the anchor.
[[nodiscard]] Vec3 along_line(Calc& calc, const FormationMember& member, const Vec3 anchor, const Vec2 d,
    const std::vector<std::pair<Vec3, Fixed>>& placed, const CollisionWorld& world, const std::uint64_t frame,
    std::span<const EntityId> bucket_ids, const AvoidanceRules& rules, std::optional<core::Diagnostic>& failure,
    const std::uint8_t filter) {
    const Fixed r = member.occupation_radius;
    const Fixed step = std::max(min_line_step, calc.mul(r, quarter));
    const Vec3 advance{calc.mul(d.x, step), calc.mul(d.y, step), Fixed{}};
    Vec3 point = anchor;
    for (std::uint32_t candidate = 0; candidate < line_candidates && calc.ok(); ++candidate) {
        LinearQuery query;
        query.start = {point.x, point.y};
        query.end = query.start;
        query.start_frame = whole(static_cast<std::int64_t>(frame));
        query.end_frame = probe_end;
        query.facing = {whole(1), Fixed{}};
        query.x_extent = r;
        query.y_extent = r;
        query.ignore_group = bucket_ids;
        const auto hits = find_dual_collision(world, member.layer, query);
        if (!hits) {
            failure = hits.error();
            return anchor;
        }
        bool free = (hits.value() & filter) == 0;
        for (const auto& [other, other_radius] : placed) {
            if (!free) break;
            free = !(length3(calc, minus3(calc, point, other)) < calc.add(r, other_radius));
        }
        if (free) return point;
        point = {calc.add(point.x, advance.x), calc.add(point.y, advance.y), point.z};
    }
    return nearest_open(calc, member, anchor, world, frame, bucket_ids, placed, rules, failure, filter);
}

} // namespace

core::Result<Fixed> occupation_radius(
    const Footprint& footprint, const Fixed max_speed, const Fixed rate_of_turn, const AvoidanceRules& rules) {
    Calc calc;
    const Fixed hard = calc.length(footprint.x_extent, footprint.y_extent);
    // (max speed / rate of turn) * 57.29578: the turn radius, degrees turned into radians.
    const Fixed turn = calc.div(calc.mul(max_speed, whole(360)), calc.mul(rate_of_turn, motion_detail::two_pi));
    const Fixed radius = calc.add(calc.mul(hard, rules.occupation_radius), calc.mul(turn, rules.wait_speed));
    if (!calc.ok()) return core::Result<Fixed>::failure(calc.error("occupation radius"));
    return core::Result<Fixed>::success(radius);
}

core::Result<Fixed> time_to_reach(const FormationMember& member, const Vec3 target) {
    Calc calc;
    const Vec3 offset = minus3(calc, target, member.position);
    const Fixed distance = length3(calc, offset);
    Fixed bearing{};
    if (offset.x.raw() != 0 || offset.y.raw() != 0) bearing = calc.atan2_deg(offset.y, offset.x);
    const Fixed turn = calc.abs(clamp180(calc.sub(bearing, member.yaw)));
    const Fixed time = calc.add(calc.div(calc.mul(turn, half), member.rate_of_turn), calc.div(distance, member.max_speed));
    if (!calc.ok()) return core::Result<Fixed>::failure(calc.error("group move time"));
    return core::Result<Fixed>::success(time);
}

core::Result<std::vector<FormationSlot>> map_group_move(const std::span<const FormationMember> members,
    const Vec3 target, const CollisionWorld& world, const std::uint64_t frame, const AvoidanceRules& rules) {
    using Slots = core::Result<std::vector<FormationSlot>>;
    Calc calc;
    // FM-02: one formation per ship, nearest to the target first.
    std::vector<std::size_t> created;
    created.reserve(members.size());
    std::vector<Fixed> to_target(members.size());
    std::vector<Fixed> times(members.size());
    std::vector<std::uint8_t> filters(members.size(), collision_all);
    Fixed slowest{};
    for (std::size_t index = 0; index < members.size(); ++index) {
        // FM-09a: a ship with no speed gets no formation and no time; the others still map.
        if (members[index].max_speed.raw() <= 0) continue;
        Footprint footprint;
        footprint.asteroid_damage = members[index].asteroid_damage;
        const auto filter = movement_collision_filter(world, footprint, members[index].entity, frame,
            members[index].position, target, members[index].through_hazards);
        if (!filter) return Slots::failure(filter.error());
        filters[index] = filter.value();
        created.push_back(index);
        to_target[index] = length3(calc, minus3(calc, target, members[index].position));
        auto time = time_to_reach(members[index], target);
        if (!time) return Slots::failure(time.error());
        times[index] = time.value();
        slowest = std::max(slowest, times[index]);
    }
    std::stable_sort(created.begin(), created.end(),
        [&](const std::size_t left, const std::size_t right) { return to_target[left] < to_target[right]; });

    std::vector<FormationSlot> slots;
    slots.reserve(members.size());
    std::optional<core::Diagnostic> failure;
    for (const auto layer : bucket_order) {
        std::vector<std::size_t> bucket;
        for (const auto index : created) {
            if (members[index].layer == layer) bucket.push_back(index);
        }
        if (bucket.empty()) continue;
        std::vector<EntityId> bucket_ids;
        for (const auto index : bucket) bucket_ids.push_back(members[index].entity);
        std::sort(bucket_ids.begin(), bucket_ids.end());

        // FM-03: the layer's centroid; the ships nearest to it map first.
        Vec3 sum{};
        for (const auto index : bucket) {
            sum = {calc.add(sum.x, members[index].position.x), calc.add(sum.y, members[index].position.y),
                calc.add(sum.z, members[index].position.z)};
        }
        const Fixed count = whole(static_cast<std::int64_t>(bucket.size()));
        const Vec3 centroid{calc.div(sum.x, count), calc.div(sum.y, count), calc.div(sum.z, count)};
        std::vector<Fixed> to_centroid(members.size());
        for (const auto index : bucket) to_centroid[index] = length3(calc, minus3(calc, members[index].position, centroid));
        std::stable_sort(bucket.begin(), bucket.end(),
            [&](const std::size_t left, const std::size_t right) { return to_centroid[left] < to_centroid[right]; });

        // FM-04, FM-05: each ship keeps its direction from the centroid; the first sits one
        // occupation radius out from the target, the others on their rays from there outward.
        std::vector<Vec3> destination(members.size(), target);
        std::vector<std::pair<Vec3, Fixed>> placed;
        Vec3 anchor = target;
        for (std::size_t order = 0; order < bucket.size() && calc.ok(); ++order) {
            const auto& member = members[bucket[order]];
            auto d = direction(calc, calc.sub(member.position.x, centroid.x), calc.sub(member.position.y, centroid.y));
            if (!d) d = direction(calc, calc.sub(member.position.x, target.x), calc.sub(member.position.y, target.y));
            if (!d) continue; // on the centroid and the target: it keeps the group's target
            Vec3 slot = target;
            if (order == 0) {
                Vec3 base = target;
                if (bucket.size() > 1) {
                    base = {calc.add(target.x, calc.mul(d->x, member.occupation_radius)),
                        calc.add(target.y, calc.mul(d->y, member.occupation_radius)), target.z};
                }
                // The later slots line up from the target shifted as the first slot was.
                slot = nearest_open(calc, member, base, world, frame, bucket_ids, placed, rules, failure, filters[bucket[order]]);
                if (failure) return Slots::failure(*failure);
                anchor = {calc.add(target.x, calc.sub(slot.x, base.x)), calc.add(target.y, calc.sub(slot.y, base.y)),
                    calc.add(target.z, calc.sub(slot.z, base.z))};
            } else {
                slot = along_line(calc, member, anchor, *d, placed, world, frame, bucket_ids, rules, failure, filters[bucket[order]]);
                if (failure) return Slots::failure(*failure);
            }
            destination[bucket[order]] = slot;
            placed.emplace_back(slot, member.occupation_radius);
        }

        // FM-07: the ships plan front first, along the group's shift from the centroid.
        const Vec3 shift = minus3(calc, anchor, centroid);
        Vec2 facing{};
        if (shift.x.raw() != 0 || shift.y.raw() != 0 || shift.z.raw() != 0) {
            const Fixed length = length3(calc, shift);
            if (length.raw() != 0) facing = {calc.div(shift.x, length), calc.div(shift.y, length)};
        }
        std::vector<Fixed> ahead(members.size());
        for (const auto index : bucket) {
            ahead[index] = calc.dot(calc.sub(members[index].position.x, centroid.x),
                calc.sub(members[index].position.y, centroid.y), facing.x, facing.y);
        }
        std::stable_sort(bucket.begin(), bucket.end(),
            [&](const std::size_t left, const std::size_t right) { return ahead[right] < ahead[left]; });

        // FM-08, FM-09: two frames apart per ship of the layer; speeds matched to the slowest.
        for (std::size_t order = 0; order < bucket.size(); ++order) {
            const auto index = bucket[order];
            Fixed speed = members[index].max_speed;
            if (slowest.raw() > 0 && times[index] < slowest) {
                speed = calc.mul(speed, std::clamp(calc.div(times[index], slowest), Fixed{}, whole(1)));
            }
            slots.push_back(FormationSlot{members[index].entity, destination[index], speed,
                static_cast<std::uint32_t>(order) * pathfind_frame_delay});
        }
    }
    if (!calc.ok()) return Slots::failure(calc.error("group move"));
    return Slots::success(std::move(slots));
}

} // namespace eawr::sim::tactical
