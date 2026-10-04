#include "eawr/sim/tactical/fighters.hpp"

#include "eawr/sim/math/math.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "../math/wide.hpp"
#include "fighters_internal.hpp"
#include "motion_internal.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>

#include "fighters_algorithms.hpp"

namespace eawr::sim::tactical {
using namespace fighters_detail;

core::Result<std::vector<GroupSlot>> squadron_group_slots(const std::span<const GroupSquadron> squadrons,
    const Vec3& destination) {
    Calc calc;
    std::vector<GroupSlot> slots(squadrons.size(), GroupSlot{destination, std::nullopt});
    std::vector<Fixed> distance(squadrons.size());
    std::vector<std::size_t> pool(squadrons.size());
    for (std::size_t index = 0; index < squadrons.size(); ++index) {
        pool[index] = index;
        const auto& position = squadrons[index].position;
        distance[index] = calc.length(Vec3{calc.sub(destination.x, position.x), calc.sub(destination.y, position.y),
            calc.sub(destination.z, position.z)});
    }
    const Fixed third = Fixed::from_raw(Fixed::scale * 33 / 100);
    const Fixed ratio = Fixed::from_raw(Fixed::scale * 9 / 10);
    const Fixed cell = whole(20);
    // FO-07: the squadron farthest from the destination (input order on ties) starts a formation.
    // Its box is its radius doubled, and in the plane at least a third of its distance to the
    // destination; every later squadron whose own radius overlaps that box, and whose speeds
    // suit it (each one's minimum at most 0.9 of the other's maximum), joins it.
    while (!pool.empty() && calc.ok()) {
        auto farthest = pool.begin();
        for (auto it = pool.begin(); it != pool.end(); ++it) {
            if (distance[*it] > distance[*farthest]) farthest = it;
        }
        const auto self = *farthest;
        pool.erase(farthest);
        const auto& own = squadrons[self];
        const Fixed reach = std::max(calc.mul(own.radius, whole(2)), calc.mul(distance[self], third));
        std::vector<std::size_t> formation{self};
        std::vector<std::size_t> rest;
        for (const auto index : pool) {
            const auto& other = squadrons[index];
            const Fixed limit = calc.add(reach, other.radius);
            const bool overlaps = calc.abs(calc.sub(other.position.x, own.position.x)) <= limit
                && calc.abs(calc.sub(other.position.y, own.position.y)) <= limit;
            const bool speeds = own.max_speed.raw() > 0 && other.max_speed.raw() > 0
                && calc.div(other.min_speed, own.max_speed) <= ratio && calc.div(own.min_speed, other.max_speed) <= ratio;
            (overlaps && speeds ? formation : rest).push_back(index);
        }
        pool = std::move(rest);
        if (formation.size() == 1) {
            // WSQ-10/WSQ-17: a move alone still has a path. In open space its single
            // segment starts at the container and has no formation slot offset.
            const Vec3 delta{calc.sub(destination.x, own.position.x),
                calc.sub(destination.y, own.position.y), Fixed{}};
            const Fixed span = calc.length(delta);
            if (span.raw() == 0) continue; // FO-02: an empty move has already arrived.
            const Vec3 direction{calc.div(delta.x, span), calc.div(delta.y, span), Fixed{}};
            slots[self].lane = SquadronLane{static_cast<EntityId>(self), own.position, direction, Fixed{}, Fixed{}};
            continue;
        }

        // FO-08: the formation faces from its centre to the destination (project: +x when they
        // meet), along its one straight path (FO-10). In cells of 20 units a squadron is two
        // radii wide and deep, and a row is the square root of the squadrons' summed areas times
        // 1.1 wide.
        Fixed cx{};
        Fixed cy{};
        for (const auto index : formation) {
            cx = calc.add(cx, squadrons[index].position.x);
            cy = calc.add(cy, squadrons[index].position.y);
        }
        const Fixed count = whole(static_cast<std::int64_t>(formation.size()));
        const Vec3 origin{calc.div(cx, count), calc.div(cy, count), Fixed{}};
        const Fixed dx = calc.sub(destination.x, origin.x);
        const Fixed dy = calc.sub(destination.y, origin.y);
        const Fixed span = calc.length(dx, dy);
        const Fixed fx = span.raw() != 0 ? calc.div(dx, span) : whole(1);
        const Fixed fy = span.raw() != 0 ? calc.div(dy, span) : Fixed{};
        const Fixed sx = calc.neg(fy); // the left of the facing
        const Fixed sy = fx;
        const auto footprint = [&](const std::size_t index) { return calc.div(calc.mul(squadrons[index].radius, whole(2)), cell); };
        Fixed area{};
        for (const auto index : formation) area = calc.add(area, calc.mul(footprint(index), footprint(index)));
        const Fixed width = calc.mul(calc.sqrt(area), Fixed::from_raw(Fixed::scale * 11 / 10));
        const auto project = [&](const std::size_t index, const Fixed ax, const Fixed ay) {
            return calc.add(calc.mul(squadrons[index].position.x, ax), calc.mul(squadrons[index].position.y, ay));
        };
        // FO-09: a row takes squadrons of one order class, one type; the classes go by their
        // attack distance, shortest first (project: the craft's distance and then the type ID,
        // where FoC reads the team container's distance and then its type name).
        const auto order = [&](const std::size_t a, const std::size_t b) {
            if (squadrons[a].type == squadrons[b].type) return 0;
            if (squadrons[a].attack_distance != squadrons[b].attack_distance) {
                return squadrons[a].attack_distance < squadrons[b].attack_distance ? -1 : 1;
            }
            return squadrons[a].type < squadrons[b].type ? -1 : 1;
        };
        // Within its class the frontmost squadrons (along the facing; input order on ties) fill
        // a row while their widths fit, the first always; a row runs from the right of the move
        // to its left in the order its squadrons stand, spread with equal gaps. Each row lies
        // half its depth behind the last, and the block is centred on the destination.
        std::vector<std::size_t> waiting = formation;
        std::stable_sort(waiting.begin(), waiting.end(), [&](const std::size_t a, const std::size_t b) {
            const int by_class = order(a, b);
            return by_class != 0 ? by_class < 0 : project(a, fx, fy) > project(b, fx, fy);
        });
        std::vector<std::pair<std::size_t, std::pair<Fixed, Fixed>>> placed;
        Fixed x{};
        Fixed front_half{};
        Fixed depth{};
        bool first = true;
        while (!waiting.empty() && calc.ok()) {
            const auto leading = waiting.front();
            std::vector<std::size_t> row;
            std::vector<std::size_t> later;
            Fixed used{};
            Fixed deepest{};
            for (const auto index : waiting) {
                const Fixed size = footprint(index);
                if (order(index, leading) == 0 && (row.empty() || calc.add(used, size) <= width)) {
                    row.push_back(index);
                    used = calc.add(used, size);
                    deepest = std::max(deepest, size);
                } else {
                    later.push_back(index);
                }
            }
            waiting = std::move(later);
            std::stable_sort(row.begin(), row.end(),
                [&](const std::size_t a, const std::size_t b) { return project(a, sx, sy) < project(b, sx, sy); });
            const Fixed half = calc.div(deepest, whole(2));
            if (x.raw() < 0) x = calc.sub(x, half);
            const Fixed gap = calc.div(calc.sub(width, used), whole(static_cast<std::int64_t>(row.size() + 1)));
            Fixed along = gap;
            for (const auto index : row) {
                const Fixed own_half = calc.div(footprint(index), whole(2));
                along = calc.add(along, own_half);
                placed.push_back({index, {x, calc.sub(along, calc.div(width, whole(2)))}});
                along = calc.add(along, calc.add(own_half, gap));
            }
            if (first) front_half = half;
            first = false;
            if (waiting.empty()) depth = calc.add(calc.neg(calc.sub(x, half)), front_half);
            x = calc.sub(x, half);
        }
        const Fixed shift = calc.sub(calc.div(depth, whole(2)), front_half);
        for (const auto& [index, offset] : placed) {
            const Fixed ahead = calc.mul(calc.add(offset.first, shift), cell);
            const Fixed aside = calc.mul(offset.second, cell);
            slots[index].point = {calc.add(destination.x, calc.add(calc.mul(fx, ahead), calc.mul(sx, aside))),
                calc.add(destination.y, calc.add(calc.mul(fy, ahead), calc.mul(sy, aside))), destination.z};
            slots[index].lane = SquadronLane{static_cast<EntityId>(self), origin, Vec3{fx, fy, Fixed{}}, ahead, aside};
        }
    }
    if (!calc.ok()) {
        return core::Result<std::vector<GroupSlot>>::failure(calc.error("squadron group move"));
    }
    return core::Result<std::vector<GroupSlot>>::success(std::move(slots));
}

core::Result<std::vector<LaneFlight>> formation_lane_flight(const std::span<const LaneMember> members,
    const Fixed side_error_min, const Fixed side_error_max) {
    Calc calc;
    std::vector<LaneFlight> flights(members.size());
    if (members.empty()) return core::Result<std::vector<LaneFlight>>::success(std::move(flights));
    // FO-11: the formation flies at its slowest member's `Max_Speed` (project: FoC's formation
    // maximum was not read), and a squadron ahead of its place slows toward the largest
    // `Min_Speed` of the formation (debug build: the formation's minimum speed).
    Fixed top = members.front().max_speed;
    Fixed floor = members.front().min_speed;
    for (const auto& member : members) {
        top = std::min(top, member.max_speed);
        floor = std::max(floor, member.min_speed);
    }
    std::vector<Fixed> forward(members.size());
    for (std::size_t index = 0; index < members.size(); ++index) {
        const auto& member = members[index];
        const auto& lane = member.lane;
        const Fixed rx = calc.sub(member.position.x, lane.origin.x);
        const Fixed ry = calc.sub(member.position.y, lane.origin.y);
        forward[index] = calc.add(calc.mul(rx, lane.direction.x), calc.mul(ry, lane.direction.y));
        // FO-10: the side error is the lane's offset less how far left of the path the squadron
        // flies; within the minimum there is no steer, beyond it the rest over the maximum.
        const Fixed side = calc.sub(calc.mul(ry, lane.direction.x), calc.mul(rx, lane.direction.y));
        const Fixed error = calc.sub(lane.aside, side);
        auto& flight = flights[index];
        flight.direction = lane.direction;
        flight.individual_speed = members.size() == 1;
        if (side_error_max.raw() > 0 && calc.abs(error) > side_error_min) {
            const Fixed beyond = error.raw() > 0 ? calc.sub(error, side_error_min) : calc.add(error, side_error_min);
            flight.shift = calc.div(beyond, side_error_max);
        }
    }
    // FO-11: a squadron's deviance is the mean, over the formation's other squadrons at least 20
    // units off (debug build: the space threshold), of how far each stands ahead of it less how
    // far its slot lies ahead.
    std::vector<Fixed> deviance(members.size());
    Fixed most{};
    Fixed least{};
    for (std::size_t index = 0; index < members.size(); ++index) {
        Fixed sum{};
        std::int64_t counted = 0;
        for (std::size_t other = 0; other < members.size(); ++other) {
            if (other == index) continue;
            const Fixed wanted = calc.sub(members[other].lane.ahead, members[index].lane.ahead);
            const Fixed term = calc.sub(calc.sub(forward[other], forward[index]), wanted);
            if (calc.abs(term) < whole(20)) continue;
            sum = calc.add(sum, term);
            ++counted;
        }
        deviance[index] = counted != 0 ? calc.div(sum, whole(counted)) : Fixed{};
        most = std::max(most, deviance[index]);
        least = std::min(least, deviance[index]);
    }
    for (std::size_t index = 0; index < members.size(); ++index) {
        const Fixed own = members[index].max_speed;
        const Fixed off = deviance[index];
        Fixed speed = top;
        // FO-11: behind its place a squadron speeds up toward its own `Max_Speed`, ahead of it it
        // slows toward the formation's minimum, by its deviance over the largest one (at least
        // 12 units, debug build).
        if (off.raw() > 0 && most.raw() > 0) {
            const Fixed share = calc.div(off, std::max(whole(12), most));
            speed = calc.add(calc.mul(share, own), calc.mul(calc.sub(whole(1), share), top));
        } else if (off.raw() < 0 && least.raw() < 0) {
            const Fixed share = calc.div(off, std::min(calc.neg(whole(12)), least));
            speed = calc.add(calc.mul(share, floor), calc.mul(calc.sub(whole(1), share), top));
        }
        // FO-11: steering aside it keeps its pace along the path, never faster than its own
        // `Max_Speed`.
        const Fixed shift = flights[index].shift;
        speed = calc.mul(speed, calc.sqrt(calc.add(whole(1), calc.mul(shift, shift))));
        flights[index].speed = std::clamp(speed, Fixed{}, own);
    }
    if (!calc.ok()) {
        return core::Result<std::vector<LaneFlight>>::failure(calc.error("squadron formation flight"));
    }
    return core::Result<std::vector<LaneFlight>>::success(std::move(flights));
}


} // namespace eawr::sim::tactical
