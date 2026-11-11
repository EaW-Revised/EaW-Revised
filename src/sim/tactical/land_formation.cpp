#include "eawr/sim/tactical/land_formation.hpp"
#include "eawr/sim/math/trig.hpp"

#include <algorithm>
#include <deque>
#include <limits>
#include <numeric>

namespace eawr::sim::tactical {
namespace {
using math::Fixed;
using math::Vec2;

constexpr auto one = Fixed::from_raw(Fixed::scale);
// LMF-02: the number of position refinements of the area clusters. The engine's iteration limit is unread (unverified).
constexpr int cluster_iterations = 8;
// LMF-03: the placement sequence (40 tries per formation, 8 to a cycle, reach growing by 7 per push try, 28 less on the
// forward part of a cardinal try) and the anchor search (40 ring points, 20 units a ring, 45 degrees apart).
constexpr int placement_tries = 40;
constexpr int anchor_tries = 40;
constexpr std::int64_t reach_step = 7;
constexpr std::int64_t cardinal_forward_pull = 28;
constexpr std::int64_t anchor_ring_step = 20;
// How far a try can stand from the click: the last anchor ring, then a push of the longest reach plus a cardinal step of it.
constexpr std::int64_t placement_travel = ((anchor_tries + 7) / 8) * anchor_ring_step + 2 * (placement_tries / 2) * reach_step;
// LMF-04/05: a destination test accepts overlap of at most this fraction of the box area (15 percent).
constexpr std::int64_t overlap_percent = 15;
// LMF-07: an offset node narrows toward the centre line in this many equal steps.
constexpr std::int64_t row_squeeze_steps = 4;

// Arithmetic that remembers an overflow: a plan with any overflow is refused as a whole.
struct Calc {
    bool ok{true};
    Fixed add(Fixed a, Fixed b) { Fixed r; ok &= math::try_add(a, b, r); return r; }
    Fixed sub(Fixed a, Fixed b) { Fixed r; ok &= math::try_subtract(a, b, r); return r; }
    Fixed mul(Fixed a, Fixed b) { Fixed r; ok &= math::try_multiply(a, b, r); return r; }
    Fixed div(Fixed a, Fixed b) { Fixed r; if (b == Fixed{}) { ok = false; return r; } ok &= math::try_divide(a, b, r); return r; }
    Vec2 add(Vec2 a, Vec2 b) { return {add(a.x, b.x), add(a.y, b.y)}; }
    Vec2 sub(Vec2 a, Vec2 b) { return {sub(a.x, b.x), sub(a.y, b.y)}; }
    Vec2 scale(Vec2 a, Fixed b) { return {mul(a.x, b), mul(a.y, b)}; }
    Fixed dot(Vec2 a, Vec2 b) { return add(mul(a.x, b.x), mul(a.y, b.y)); }
    Fixed length(Vec2 a) { Fixed r; ok &= math::try_length(a, r); return r; }
    Fixed absolute(Fixed a) { return a < Fixed{} ? sub(Fixed{}, a) : a; }
    // A unit vector, or `fallback` for a zero vector.
    Vec2 unit(Vec2 v, Vec2 fallback) {
        const auto len = length(v);
        if (len == Fixed{}) return fallback;
        return {div(v.x, len), div(v.y, len)};
    }
    Fixed integer(std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }
};

Vec2 side_of(Vec2 axis) { return {Fixed::from_raw(-axis.y.raw()), axis.x}; }
// A local (forward, side) vector in the frame of the forward unit vector `axis`.
Vec2 to_world(Calc& c, Vec2 axis, Vec2 local) {
    const auto side = side_of(axis);
    return c.add(c.scale(axis, local.x), c.scale(side, local.y));
}

struct Box {
    Vec2 origin{}, axis{one, {}};
    Fixed hx{}, hy{};
};

bool contains(Calc& c, const Box& box, Vec2 point) {
    const auto d = c.sub(point, box.origin);
    return c.absolute(c.dot(d, box.axis)) <= box.hx && c.absolute(c.dot(d, side_of(box.axis))) <= box.hy;
}

bool intersects(Calc& c, const Box& a, const Box& b) {
    const Vec2 axes[4] = {a.axis, side_of(a.axis), b.axis, side_of(b.axis)};
    const auto d = c.sub(b.origin, a.origin);
    for (const auto& axis : axes) {
        const auto distance = c.absolute(c.dot(d, axis));
        const auto ra = c.add(c.mul(a.hx, c.absolute(c.dot(a.axis, axis))), c.mul(a.hy, c.absolute(c.dot(side_of(a.axis), axis))));
        const auto rb = c.add(c.mul(b.hx, c.absolute(c.dot(b.axis, axis))), c.mul(b.hy, c.absolute(c.dot(side_of(b.axis), axis))));
        if (distance > c.add(ra, rb)) return false;
    }
    return true;
}

Fixed area_of(Calc& c, const Box& box) { return c.mul(c.mul(box.hx, box.hy), c.integer(4)); }

// Can_Occupy_Point analogue: the footprint of `radius` around the cell of `at` is admitted by `mask`, inside the playable area.
bool point_admitted(const LandOccupancy& occupancy, std::uint32_t mask, Fixed radius, Vec2 at) {
    const auto& terrain = occupancy.terrain();
    const auto spacing = terrain.spacing();
    const auto columns = terrain.width(), rows = terrain.height();
    const auto column = land_cell_of(at.x, spacing, columns), row = land_cell_of(at.y, spacing, rows);
    if (!column || !row) return false;
    const auto playable = terrain.playable_centre(at.x, at.y);
    if (playable && !*playable) return false;
    const auto footprint = land_footprint(radius, spacing);
    const std::int64_t low_x = static_cast<std::int64_t>(*column) + footprint.low, high_x = static_cast<std::int64_t>(*column) + footprint.high;
    const std::int64_t low_y = static_cast<std::int64_t>(*row) + footprint.low, high_y = static_cast<std::int64_t>(*row) + footprint.high;
    if (low_x < 0 || low_y < 0 || high_x >= columns || high_y >= rows) return false;
    return occupancy.admits_footprint(static_cast<std::uint32_t>(low_x), static_cast<std::uint32_t>(low_y),
        static_cast<std::uint32_t>(high_x), static_cast<std::uint32_t>(high_y), mask);
}

// Is_Coord_Box_Passable analogue: every cell whose centre lies in the box (and the cell under its origin) is admitted.
bool box_passable(Calc& c, const LandOccupancy& occupancy, std::uint32_t mask, const Box& box) {
    const auto& terrain = occupancy.terrain();
    const auto spacing = terrain.spacing();
    const auto columns = terrain.width(), rows = terrain.height();
    const auto forward = c.scale(box.axis, box.hx), side = c.scale(side_of(box.axis), box.hy);
    Fixed low_x = box.origin.x, high_x = box.origin.x, low_y = box.origin.y, high_y = box.origin.y;
    for (int corner = 0; corner < 4; ++corner) {
        const auto a = c.add(box.origin, (corner & 1) != 0 ? forward : Vec2{c.sub(Fixed{}, forward.x), c.sub(Fixed{}, forward.y)});
        const auto p = c.add(a, (corner & 2) != 0 ? side : Vec2{c.sub(Fixed{}, side.x), c.sub(Fixed{}, side.y)});
        low_x = std::min(low_x, p.x); high_x = std::max(high_x, p.x);
        low_y = std::min(low_y, p.y); high_y = std::max(high_y, p.y);
    }
    if (!c.ok) return false;
    const auto first_column = land_cell_of(low_x, spacing, columns), last_column = land_cell_of(high_x, spacing, columns);
    const auto first_row = land_cell_of(low_y, spacing, rows), last_row = land_cell_of(high_y, spacing, rows);
    if (!first_column || !last_column || !first_row || !last_row) return false;
    const auto origin_column = land_cell_of(box.origin.x, spacing, columns), origin_row = land_cell_of(box.origin.y, spacing, rows);
    const auto cells = occupancy.cells();
    for (auto row = *first_row; row <= *last_row; ++row) {
        for (auto column = *first_column; column <= *last_column; ++column) {
            const Vec2 centre{land_cell_centre(column, spacing), land_cell_centre(row, spacing)};
            const bool origin_cell = origin_column && origin_row && column == *origin_column && row == *origin_row;
            if (!origin_cell && !contains(c, box, centre)) continue;
            const auto index = static_cast<std::size_t>(row) * columns + column;
            if (index >= cells.size() || (cells[index] & ~mask) != 0) return false;
        }
    }
    return c.ok;
}

// A straight line is clear when samples half a cell apart are all admitted for the footprint.
bool line_clear(Calc& c, const LandOccupancy& occupancy, std::uint32_t mask, Fixed radius, Vec2 from, Vec2 to) {
    const auto half = Fixed::from_raw(std::max<std::int64_t>(occupancy.terrain().spacing().raw() / 2, 1));
    const auto length = c.length(c.sub(to, from));
    if (!c.ok) return false;
    auto steps = c.div(length, half).ceil_to_integer();
    if (steps < 1) steps = 1;
    if (steps > 4096) steps = 4096;
    for (std::int64_t step = 0; step <= steps; ++step) {
        const auto t = Fixed::from_ratio(step, steps).value();
        const Vec2 at{c.add(from.x, c.mul(c.sub(to.x, from.x), t)), c.add(from.y, c.mul(c.sub(to.y, from.y), t))};
        if (!c.ok || !point_admitted(occupancy, mask, radius, at)) return false;
    }
    return c.ok;
}

std::uint32_t zone_at(const LandOccupancy& occupancy, const LandZones& zones, Vec2 at) {
    const auto& terrain = occupancy.terrain();
    const auto column = land_cell_of(at.x, terrain.spacing(), terrain.width());
    const auto row = land_cell_of(at.y, terrain.spacing(), terrain.height());
    if (!column || !row) return 0;
    return zones.zone_of(*row * terrain.width() + *column);
}

struct Zones {
    const LandOccupancy& occupancy;
    std::deque<LandZones> store;
    const LandZones& of(std::uint32_t mask) {
        for (const auto& zones : store) if (zones.mask() == mask) return zones;
        store.push_back(LandZones::build(occupancy, mask));
        return store.back();
    }
    // Same zone of the class `mask`, both points inside it.
    bool same(std::uint32_t mask, Vec2 a, Vec2 b) {
        const auto& zones = of(mask);
        const auto za = zone_at(occupancy, zones, a);
        return za != 0 && za == zone_at(occupancy, zones, b);
    }
};

struct Formation {
    std::vector<std::size_t> members; // indices into the object list; rank order after layout
    std::vector<Vec2> offsets;        // parallel to members
    std::uint32_t mask{};
    Fixed half_forward{}, half_side{}, radius{};
    Vec2 seed{};                      // the seed object's position (the formation's centre object)
    std::size_t seed_index{};
    Vec2 start{};
    bool solo{};
    bool infantry_bike{}, giant{}, team{};
};

} // namespace

std::optional<Fixed> land_formation_deviance(const std::span<const LandFormationObservation> observations) {
    // LMF-09: the land forward-error dead band is 0.1 world units.
    constexpr auto dead_band = Fixed::from_raw((Fixed::scale + 5) / 10);
    Calc c;
    Fixed sum{};
    std::int64_t count = 0;
    for (const auto& observation : observations) {
        const auto delta = c.sub(observation.forward_distance, observation.slot_difference);
        if (c.absolute(delta) >= dead_band) { sum = c.add(sum, delta); ++count; }
    }
    if (!c.ok || count > std::numeric_limits<std::int64_t>::max() / Fixed::scale) return std::nullopt;
    const auto result = count ? c.div(sum, c.integer(count)) : Fixed{};
    return c.ok ? std::optional<Fixed>{result} : std::nullopt;
}

std::optional<Fixed> land_formation_speed(const Fixed deviance, const Fixed maximum_deviance,
    const Fixed minimum_deviance, const Fixed formation_maximum, const Fixed object_maximum,
    const Fixed deceleration, const bool enforce_top_speed) {
    if (formation_maximum < Fixed{} || object_maximum < Fixed{} || deceleration < Fixed{}
        || maximum_deviance < Fixed{} || minimum_deviance > Fixed{}) return std::nullopt;
    if (enforce_top_speed) return object_maximum;
    // LMF-09: 12 world units sets the interpolation scale; 0.4 is the slow-speed floor.
    constexpr auto distance_scale = Fixed::from_raw(12 * Fixed::scale);
    constexpr auto slow_floor = Fixed::from_raw((4 * Fixed::scale + 5) / 10);
    Calc c;
    auto speed = formation_maximum;
    if (deviance > Fixed{} && maximum_deviance > Fixed{}) {
        const auto t = c.div(deviance, std::max(maximum_deviance, distance_scale));
        speed = c.add(c.mul(t, object_maximum), c.mul(c.sub(one, t), formation_maximum));
        const auto squared = c.mul(c.mul(c.integer(2), deceleration), deviance);
        Fixed braking{};
        c.ok &= math::try_sqrt(squared, braking);
        speed = std::min(speed, c.add(braking, formation_maximum));
    } else if (deviance < Fixed{} && minimum_deviance < Fixed{}) {
        const auto t = c.div(deviance, std::min(minimum_deviance, c.sub(Fixed{}, distance_scale)));
        const auto slow = c.mul(std::max(slow_floor, c.sub(one, c.div(c.absolute(minimum_deviance), distance_scale))), formation_maximum);
        speed = c.add(c.mul(t, slow), c.mul(c.sub(one, t), formation_maximum));
    }
    if (!c.ok) return std::nullopt;
    return std::clamp(speed, Fixed{}, object_maximum);
}

std::optional<Fixed> land_formation_forward_offset(const std::span<const Vec2> centre,
    std::uint32_t first_segment, Vec2 first, std::uint32_t second_segment, Vec2 second) {
    if (centre.size() < 2 || first_segment >= centre.size() - 1 || second_segment >= centre.size() - 1)
        return std::nullopt;
    bool reverse = false;
    if (first_segment > second_segment) {
        std::swap(first_segment, second_segment);
        std::swap(first, second);
        reverse = true;
    }
    Calc c;
    Fixed total{};
    for (auto segment = first_segment; segment < second_segment; ++segment) {
        const auto incoming = c.unit(c.sub(centre[segment + 1], centre[segment]), Vec2{});
        if (incoming == Vec2{}) continue;
        const auto outgoing = segment + 2 < centre.size()
            ? c.unit(c.sub(centre[segment + 2], centre[segment + 1]), incoming) : incoming;
        const auto bisector = c.add(incoming, outgoing);
        // LMF-09: a nearly reversed corner uses its incoming direction for the half-plane.
        const auto normal = c.length(bisector) < Fixed::from_raw((Fixed::scale + 50) / 100)
            ? incoming : c.unit(bisector, incoming);
        const auto denominator = c.dot(incoming, normal);
        if (!c.ok || denominator == Fixed{}) return std::nullopt;
        const auto distance = c.div(c.dot(c.sub(centre[segment + 1], first), normal), denominator);
        first = c.add(first, c.scale(incoming, distance));
        total = c.add(total, distance);
    }
    const auto direction = c.unit(c.sub(centre[second_segment + 1], centre[second_segment]), Vec2{});
    total = c.add(total, c.dot(c.sub(second, first), direction));
    if (reverse) total = c.sub(Fixed{}, total);
    if (!c.ok || direction == Vec2{}) return std::nullopt;
    return total;
}

std::optional<std::uint32_t> land_formation_advance_segment(const std::span<const Vec2> centre,
    std::uint32_t segment, const std::uint32_t limit, const Vec2 position) {
    if (centre.size() < 2 || segment >= centre.size() - 1 || limit >= centre.size() - 1 || segment > limit)
        return std::nullopt;
    Calc c;
    while (segment < limit) {
        const auto incoming = c.unit(c.sub(centre[segment + 1], centre[segment]), Vec2{});
        const auto outgoing = c.unit(c.sub(centre[segment + 2], centre[segment + 1]), incoming);
        const auto sum = c.add(incoming, outgoing);
        const auto normal = c.length(sum) < Fixed::from_raw((Fixed::scale + 50) / 100)
            ? incoming : c.unit(sum, incoming);
        if (!c.ok || normal == Vec2{}) return std::nullopt;
        if (c.dot(c.sub(position, centre[segment + 1]), normal) < Fixed{}) break;
        if (!c.ok) return std::nullopt;
        ++segment;
    }
    return segment;
}

std::optional<Vec2> land_formation_slot_position(const Vec2 destination, const Vec2 heading, const Vec2 slot) {
    Calc c;
    const auto world = c.add(destination, to_world(c, heading, slot));
    if (!c.ok) return std::nullopt;
    return world;
}

std::optional<std::vector<LandFormationPlan>> land_plan_formations(const LandOccupancy& occupancy,
    const LandFormationSettings& settings, const std::span<const LandFormationObject> objects,
    const std::span<const LandFormationObstacle> obstacles, const Vec2 click, LandFormationWork* const work) {
    if (objects.empty() || !settings.authored) return std::nullopt;
    Calc c;
    const auto count = objects.size();
    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) { return objects[x].id < objects[y].id; });
    for (std::size_t i = 1; i < count; ++i) if (objects[order[i]].id == objects[order[i - 1]].id) return std::nullopt;
    const auto mean = [&](const std::vector<std::size_t>& members) {
        Vec2 sum;
        for (const auto m : members) sum = c.add(sum, objects[m].position);
        return Vec2{c.div(sum.x, c.integer(static_cast<std::int64_t>(members.size()))), c.div(sum.y, c.integer(static_cast<std::int64_t>(members.size())))};
    };
    // The order's heading: from the group's centre toward the click (the straight bearing stands for the arrival heading).
    const auto group_centre = mean(order);
    const Vec2 heading = c.unit(c.sub(click, group_centre), Vec2{one, {}});
    if (!c.ok) return std::nullopt;
    Zones zones{occupancy, {}};

    // LMF-02: very large groups are cut into area clusters first.
    Fixed area_sum;
    for (const auto i : order) area_sum = c.add(area_sum, c.mul(c.mul(objects[i].half_forward, objects[i].half_side), c.integer(4)));
    auto clusters_wanted = c.div(c.div(area_sum, c.integer(400)), settings.max_area > Fixed{} ? settings.max_area : one).trunc_to_integer() + 1;
    if (!c.ok) return std::nullopt;
    if (clusters_wanted < 1 || static_cast<std::uint64_t>(clusters_wanted) > count) clusters_wanted = 1;
    const auto k = static_cast<std::size_t>(clusters_wanted);
    std::vector<std::size_t> cluster_of(count, 0);
    if (k > 1) {
        for (std::size_t i = 0; i < count; ++i) cluster_of[i] = i % k;
        for (int iteration = 0; iteration < cluster_iterations; ++iteration) {
            std::vector<Vec2> centres(k);
            std::vector<std::int64_t> sizes(k, 0);
            for (std::size_t i = 0; i < count; ++i) {
                centres[cluster_of[i]] = c.add(centres[cluster_of[i]], objects[order[i]].position);
                ++sizes[cluster_of[i]];
            }
            for (std::size_t j = 0; j < k; ++j) if (sizes[j] > 0) centres[j] = {c.div(centres[j].x, c.integer(sizes[j])), c.div(centres[j].y, c.integer(sizes[j]))};
            bool moved = false;
            for (std::size_t i = 0; i < count; ++i) {
                std::size_t best = cluster_of[i];
                Fixed best_distance;
                bool first = true;
                for (std::size_t j = 0; j < k; ++j) {
                    if (sizes[j] == 0) continue;
                    const auto d = c.length(c.sub(objects[order[i]].position, centres[j]));
                    if (first || d < best_distance) { best = j; best_distance = d; first = false; }
                }
                if (best != cluster_of[i]) { cluster_of[i] = best; moved = true; }
            }
            if (!c.ok) return std::nullopt;
            if (!moved) break;
        }
    }

    // LMF-02: grow formations inside each cluster from the object nearest its centre.
    std::vector<Formation> formations;
    for (std::size_t cluster = 0; cluster < k; ++cluster) {
        std::vector<std::size_t> remaining; // object indices, ascending id
        for (std::size_t i = 0; i < count; ++i) if (cluster_of[i] == cluster) remaining.push_back(order[i]);
        while (!remaining.empty()) {
            const auto centre = mean(remaining);
            std::size_t seed_at = 0;
            Fixed best;
            for (std::size_t r = 0; r < remaining.size(); ++r) {
                const auto d = c.length(c.sub(objects[remaining[r]].position, centre));
                if (r == 0 || d < best) { best = d; seed_at = r; }
            }
            if (!c.ok) return std::nullopt;
            Formation formation;
            const auto seed = remaining[seed_at];
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(seed_at));
            formation.members.push_back(seed);
            formation.mask = objects[seed].mask;
            formation.solo = objects[seed].grouping == units::LandFormationGrouping::solo;
            // LMF-02: a team container is one object like a vehicle. The split constant only separates soldiers that were
            // inserted without their container, and a squad is never inserted that way.
            if (!formation.solo) {
                for (bool joined = true; joined;) {
                    joined = false;
                    for (std::size_t r = 0; r < remaining.size(); ++r) {
                        const auto& candidate = objects[remaining[r]];
                        if (candidate.grouping == units::LandFormationGrouping::solo || candidate.speed <= Fixed{}) continue;
                        const auto common = formation.mask & candidate.mask;
                        if (!land_is_authored_class(occupancy.terrain(), common)) continue;
                        // The same zone of the common class as the seed, and the click reachable from it.
                        if (!zones.same(common, candidate.position, objects[seed].position)
                            || !zones.same(common, objects[seed].position, click)) continue;
                        Fixed nearest;
                        bool first = true;
                        for (const auto m : formation.members) {
                            const auto d = c.length(c.sub(candidate.position, objects[m].position));
                            if (first || d < nearest) { nearest = d; first = false; }
                        }
                        if (!c.ok || nearest > candidate.recruit) continue;
                        if (!line_clear(c, occupancy, common, candidate.radius, candidate.position, objects[seed].position)) continue;
                        formation.members.push_back(remaining[r]);
                        formation.mask = common;
                        remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(r));
                        joined = true;
                        break;
                    }
                }
            }
            formation.seed = objects[seed].position;
            formation.seed_index = seed;
            formations.push_back(std::move(formation));
        }
    }

    // LMF-06: rank, rows and extents of every formation.
    for (auto& formation : formations) {
        const auto centre = mean(formation.members);
        const auto& first = objects[formation.members.front()];
        std::vector<std::size_t> ranked = formation.members;
        std::vector<Fixed> projection(count);
        for (const auto m : ranked) projection[m] = c.dot(c.sub(objects[m].position, centre), heading);
        std::sort(ranked.begin(), ranked.end(), [&](std::size_t x, std::size_t y) {
            const auto& a = objects[x]; const auto& b = objects[y];
            if (a.hero != b.hero) return a.hero;
            if (a.range != b.range) return a.range < b.range;
            if (projection[x] != projection[y]) return projection[x] > projection[y];
            return a.id < b.id;
        });
        const auto n = ranked.size();
        std::size_t capacity = 1;
        while (capacity * capacity < n) ++capacity;
        // LMF-06: each rank has its own row band; a row also ends at `ceil(sqrt N)` objects.
        const auto same_rank = [&](std::size_t x, std::size_t y) { return objects[x].hero == objects[y].hero && objects[x].range == objects[y].range; };
        Fixed half_total, widest;
        // Rows, cells and offsets for the objects in the order of `ranked`. Cell widths and row depths come from the objects
        // that stand in them, so the layout runs again after the slots are assigned by proximity.
        const auto layout = [&]() {
            struct Row { std::vector<std::size_t> members; Fixed depth{}, width{}; };
            std::vector<Row> rows;
            for (std::size_t i = 0; i < n;) {
                Row row;
                std::size_t j = i;
                while (j < n && row.members.size() < capacity && same_rank(ranked[i], ranked[j])) row.members.push_back(ranked[j++]);
                i = j;
                const auto side = side_of(heading);
                std::vector<Fixed> lateral(count);
                for (const auto m : row.members) lateral[m] = c.dot(c.sub(objects[m].position, centre), side);
                std::sort(row.members.begin(), row.members.end(), [&](std::size_t x, std::size_t y) {
                    if (lateral[x] != lateral[y]) return lateral[x] < lateral[y];
                    return objects[x].id < objects[y].id;
                });
                for (const auto m : row.members) {
                    row.depth = std::max(row.depth, c.add(c.mul(objects[m].half_forward, c.integer(2)), objects[m].spacing));
                    row.width = c.add(row.width, c.add(c.mul(objects[m].half_side, c.integer(2)), objects[m].spacing));
                }
                rows.push_back(std::move(row));
            }
            Fixed total_depth;
            widest = Fixed{};
            for (const auto& row : rows) { total_depth = c.add(total_depth, row.depth); widest = std::max(widest, row.width); }
            // A lone object has no spacing around it: its box is its own.
            half_total = c.div(total_depth, c.integer(2));
            formation.members.clear(); formation.offsets.clear();
            Fixed ahead;
            for (const auto& row : rows) {
                const auto forward = c.sub(half_total, c.add(ahead, c.div(row.depth, c.integer(2))));
                Fixed taken;
                for (const auto m : row.members) {
                    const auto cell = c.add(c.mul(objects[m].half_side, c.integer(2)), objects[m].spacing);
                    const auto lateral_offset = c.sub(c.add(taken, c.div(cell, c.integer(2))), c.div(row.width, c.integer(2)));
                    formation.members.push_back(m);
                    formation.offsets.push_back({forward, lateral_offset});
                    taken = c.add(taken, cell);
                }
                ahead = c.add(ahead, row.depth);
            }
        };
        layout();
        // Within one rank an object takes the slot nearest to where it stands (greedy, ties by index), so the routes of
        // objects that start side by side do not cross: crossing members block one another at the start of the walk. The
        // rows are then laid out again for the new order, so every object stands in a cell of its own size.
        if (n > 1) {
            for (std::size_t band = 0; band < formation.members.size();) {
                std::size_t end = band + 1;
                while (end < formation.members.size() && same_rank(formation.members[band], formation.members[end])) ++end;
                const auto size = end - band;
                if (size > 1) {
                    std::vector<Fixed> cost(size * size); // [member][slot]
                    for (std::size_t m = 0; m < size; ++m) {
                        for (std::size_t s = 0; s < size; ++s) {
                            const auto spot = c.add(centre, to_world(c, heading, formation.offsets[band + s]));
                            const auto delta = c.sub(objects[formation.members[band + m]].position, spot);
                            cost[m * size + s] = c.dot(delta, delta);
                        }
                    }
                    std::vector<std::size_t> assigned(size, size);
                    std::vector<bool> slot_taken(size, false);
                    for (std::size_t round = 0; round < size; ++round) {
                        std::size_t best_m = size, best_s = size;
                        for (std::size_t m = 0; m < size; ++m) {
                            if (assigned[m] != size) continue;
                            for (std::size_t s = 0; s < size; ++s) {
                                if (slot_taken[s]) continue;
                                if (best_m == size || cost[m * size + s] < cost[best_m * size + best_s]) { best_m = m; best_s = s; }
                            }
                        }
                        assigned[best_m] = best_s; slot_taken[best_s] = true;
                    }
                    std::vector<std::size_t> reordered(size);
                    for (std::size_t m = 0; m < size; ++m) reordered[assigned[m]] = formation.members[band + m];
                    for (std::size_t s = 0; s < size; ++s) formation.members[band + s] = reordered[s];
                }
                band = end;
            }
            ranked = formation.members;
            layout();
        }
        if (!c.ok) return std::nullopt;
        if (n == 1) {
            formation.offsets[0] = {};
            formation.half_forward = objects[ranked[0]].half_forward;
            formation.half_side = objects[ranked[0]].half_side;
        } else {
            formation.half_forward = half_total;
            formation.half_side = c.div(widest, c.integer(2));
        }
        formation.radius = first.radius;
        bool infantry_bike = true, giant = true, team = true;
        for (const auto m : formation.members) {
            formation.radius = std::max(formation.radius, objects[m].radius);
            infantry_bike = infantry_bike && objects[m].infantry_class;
            giant = giant && objects[m].giant;
            team = team && objects[m].formation_team;
        }
        formation.infantry_bike = infantry_bike; formation.giant = giant; formation.team = team;
        // R-26: the centre path starts at the formation's centre when that stands in the class's zone with a clear line to
        // the seed, else at the seed object.
        formation.start = formation.seed;
        if (n > 1 && point_admitted(occupancy, formation.mask, formation.radius, centre)
            && zones.same(formation.mask, centre, formation.seed)
            && line_clear(c, occupancy, formation.mask, formation.radius, centre, formation.seed))
            formation.start = centre;
    }
    if (!c.ok) return std::nullopt;

    // LMF-03: nearest the group's centre first.
    Vec2 sum_centres;
    for (const auto& formation : formations) sum_centres = c.add(sum_centres, formation.seed);
    const Vec2 centroid{c.div(sum_centres.x, c.integer(static_cast<std::int64_t>(formations.size()))),
        c.div(sum_centres.y, c.integer(static_cast<std::int64_t>(formations.size())))};
    std::vector<Fixed> distance_from_centroid(formations.size());
    for (std::size_t i = 0; i < formations.size(); ++i) distance_from_centroid[i] = c.length(c.sub(formations[i].seed, centroid));
    std::vector<std::size_t> place_order(formations.size());
    std::iota(place_order.begin(), place_order.end(), std::size_t{0});
    std::sort(place_order.begin(), place_order.end(), [&](std::size_t x, std::size_t y) {
        if (distance_from_centroid[x] != distance_from_centroid[y]) return distance_from_centroid[x] < distance_from_centroid[y];
        return objects[formations[x].members.front()].id < objects[formations[y].members.front()].id;
    });
    if (!c.ok) return std::nullopt;

    // Only the obstacles a try can reach matter: the click, the farthest try, the box and its widening, and the obstacle's own box.
    Fixed biggest_box;
    for (const auto& formation : formations) biggest_box = std::max(biggest_box, c.add(formation.half_forward, formation.half_side));
    const auto reach = c.add(c.add(c.integer(placement_travel), biggest_box), c.add(settings.between_spacing, settings.query_extension));
    std::vector<LandFormationObstacle> near_obstacles;
    for (const auto& obstacle : obstacles) {
        const auto distance = c.length(c.sub(obstacle.position, click));
        if (distance <= c.add(reach, c.add(obstacle.half_forward, obstacle.half_side))) near_obstacles.push_back(obstacle);
    }
    if (!c.ok) return std::nullopt;
    if (work) work->obstacles_kept = near_obstacles.size();
    const bool several = formations.size() > 1;
    std::vector<Box> placed; // the placed boxes; deliberately not cleared between anchor tries (R-22)
    std::vector<LandFormationPlan> plans(formations.size());
    const Vec2 cardinal[4] = {{one, {}}, {{}, one}, {Fixed::from_raw(-Fixed::scale), {}}, {{}, Fixed::from_raw(-Fixed::scale)}};
    const auto padding = settings.between_spacing;
    const auto query_box = [&](const Formation& formation, Vec2 origin) {
        Box box; box.origin = origin; box.axis = heading;
        box.hx = c.add(formation.half_forward, padding); box.hy = c.add(formation.half_side, padding);
        return box;
    };

    // Place_Formation: 40 tries of the sequence; true when a try passed (or the placement is forced).
    const auto place = [&](const std::size_t index, const Vec2 anchor, const Vec2 push, const bool nearest, const bool force) {
        const auto& formation = formations[index];
        auto& plan = plans[index];
        Box box = query_box(formation, anchor);
        Box saved = box;
        bool blocked_state = false, terrain_mode = false, success = false;
        std::int64_t reach = 0;
        const Fixed half = Fixed::from_ratio(1, 2).value();
        for (int attempt = 0; attempt < placement_tries && c.ok; ++attempt) {
            Vec2 at = anchor;
            const auto cycle = attempt % 8;
            const auto reach_value = c.integer(reach);
            if (cycle < 4) {
                if (nearest && !blocked_state && !terrain_mode) {
                    const auto alternate = (attempt % 2 == 0) ? one : Fixed::from_ratio(-3, 4).value();
                    at = c.add(at, c.scale(c.scale(push, c.mul(reach_value, half)), alternate));
                } else at = c.add(at, c.scale(push, reach_value));
                reach += reach_step;
            } else {
                const auto forward_reach = c.integer(reach - cardinal_forward_pull);
                if (nearest && !blocked_state && !terrain_mode) at = c.add(at, c.scale(push, c.mul(forward_reach, half)));
                else at = c.add(at, c.scale(push, forward_reach));
                at = c.add(at, c.scale(cardinal[cycle - 4], reach_value));
            }
            box.origin = at;
            if (!c.ok) break;
            if (several && contains(c, box, anchor)) continue;
            bool overlaps_placed = false;
            for (const auto& other : placed) if (intersects(c, box, other)) { overlaps_placed = true; break; }
            if (overlaps_placed) continue;
            const bool can_occupy = point_admitted(occupancy, formation.mask, objects[formation.members.front()].radius, at);
            if (!can_occupy || !box_passable(c, occupancy, formation.mask, box)) {
                if (terrain_mode) { box = saved; break; }
                blocked_state = true;
                continue;
            }
            if (blocked_state) { blocked_state = false; terrain_mode = true; }
            if (terrain_mode) {
                if (!zones.same(formation.mask, anchor, at)) continue;
                if (!line_clear(c, occupancy, formation.mask, objects[formation.members.front()].radius, anchor, at)) continue;
            }
            saved = box;
            // LMF-05: objects outside the order whose box meets a member's slot box, within the query extension.
            Box extended = box;
            extended.hx = c.add(extended.hx, settings.query_extension); extended.hy = c.add(extended.hy, settings.query_extension);
            bool blocked = false;
            Fixed hit_area;
            for (const auto& obstacle : near_obstacles) {
                if (!formation.team && (formation.infantry_bike || formation.giant) && obstacle.infantry_class && !obstacle.formation_team) continue;
                if (work) ++work->obstacle_tests;
                Box obstacle_box; obstacle_box.origin = obstacle.position; obstacle_box.axis = obstacle.axis;
                obstacle_box.hx = obstacle.half_forward; obstacle_box.hy = obstacle.half_side;
                if (!intersects(c, extended, obstacle_box)) continue;
                bool hit = false;
                for (std::size_t m = 0; m < formation.members.size() && !hit; ++m) {
                    Box member_box; member_box.origin = c.add(at, to_world(c, heading, formation.offsets[m]));
                    member_box.axis = heading;
                    member_box.hx = objects[formation.members[m]].half_forward; member_box.hy = objects[formation.members[m]].half_side;
                    hit = intersects(c, member_box, obstacle_box);
                }
                if (!hit) continue;
                blocked = true;
                hit_area = c.add(hit_area, area_of(c, obstacle_box));
            }
            if (!c.ok) break;
            if (!blocked || hit_area <= c.div(c.mul(area_of(c, box), c.integer(overlap_percent)), c.integer(100))) { success = true; break; }
        }
        if (!c.ok) return false;
        if (!success && !force) return false;
        plan.destination = saved.origin;
        plan.forced = !success;
        Box stored = saved;
        stored.hx = c.sub(stored.hx, padding); stored.hy = c.sub(stored.hy, padding);
        placed.push_back(stored);
        return c.ok;
    };

    // Adjust_Destinations_Position: the anchor tries.
    const auto first_formation = place_order.front();
    const auto anchor_mask = formations[first_formation].mask;
    const auto anchor_radius = Fixed{};
    bool placed_all = false;
    Vec2 anchor = click;
    for (int attempt = 0; attempt <= anchor_tries + 1 && c.ok; ++attempt) {
        const bool force = attempt > anchor_tries;
        if (force) anchor = click;
        else if (attempt == 0) anchor = click;
        else {
            const auto ring = c.integer(static_cast<std::int64_t>((attempt + 7) / 8) * anchor_ring_step);
            const auto turn = Fixed::from_ratio(attempt % 8, 8).value();
            const auto direction = math::sin_cos_turn(turn);
            anchor = c.add(click, Vec2{c.mul(ring, direction.cosine), c.mul(ring, direction.sine)});
            if (!c.ok || !point_admitted(occupancy, anchor_mask, anchor_radius, anchor)
                || !zones.same(anchor_mask, anchor, click)
                || !line_clear(c, occupancy, anchor_mask, anchor_radius, anchor, click)) continue;
        }
        bool all = true;
        for (std::size_t rank = 0; rank < place_order.size() && all; ++rank) {
            const auto index = place_order[rank];
            Vec2 push = c.sub(formations[index].seed, centroid);
            if ((rank == 0 && !several) || (push.x == Fixed{} && push.y == Fixed{})) push = Vec2{Fixed::from_raw(-heading.x.raw()), Fixed::from_raw(-heading.y.raw())};
            push = c.unit(push, Vec2{one, {}});
            all = place(index, anchor, push, rank == 0, force);
        }
        if (all && c.ok) { placed_all = true; break; }
    }
    if (!c.ok || !placed_all) return std::nullopt;

    for (std::size_t i = 0; i < formations.size(); ++i) {
        const auto& formation = formations[i];
        auto& plan = plans[i];
        // The seed object leads the centre path even when ranking put it elsewhere in the rows.
        plan.lead = objects[formation.seed_index].id;
        for (std::size_t m = 0; m < formation.members.size(); ++m) plan.slots.push_back({objects[formation.members[m]].id, formation.offsets[m]});
        plan.start = formation.start;
        plan.heading = heading;
        plan.mask = formation.mask;
        plan.radius = formation.radius;
        plan.width = c.mul(formation.half_side, c.integer(2));
    }
    if (!c.ok) return std::nullopt;
    std::vector<LandFormationPlan> result;
    result.reserve(plans.size());
    for (const auto index : place_order) result.push_back(std::move(plans[index]));
    return result;
}

std::optional<std::vector<Vec2>> land_formation_member_path(const LandOccupancy& occupancy, const std::span<const Vec2> centre,
    const Vec2 member_start, const Vec2 slot, const std::uint32_t mask, const Fixed radius, const Vec2 final_heading,
    std::vector<std::uint32_t>* centre_segments) {
    if (centre.size() < 2) return std::nullopt;
    Calc c;
    const auto n = centre.size();
    // The heading of every node: the segment into it, averaged with the segment out of it at an interior node.
    std::vector<Vec2> headings(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto outgoing = i + 1 < n ? c.unit(c.sub(centre[i + 1], centre[i]), Vec2{}) : Vec2{};
        const auto incoming = i > 0 ? c.unit(c.sub(centre[i], centre[i - 1]), Vec2{}) : Vec2{};
        Vec2 heading = i == 0 ? outgoing : i + 1 == n ? incoming : c.unit(c.add(incoming, outgoing), outgoing);
        if (heading.x == Fixed{} && heading.y == Fixed{}) heading = Vec2{one, {}};
        headings[i] = heading;
    }
    if (!c.ok) return std::nullopt;
    // The centre path sampled every cell: a straight corridor has no interior node, yet it is where a column narrows.
    struct Sample { Vec2 at; Vec2 heading; std::uint32_t segment; };
    std::vector<Sample> samples;
    const auto sample_gap = Fixed::from_raw(std::max<std::int64_t>(occupancy.terrain().spacing().raw(), Fixed::scale));
    for (std::size_t i = 1; i < n; ++i) {
        const auto segment = c.sub(centre[i], centre[i - 1]);
        const auto length = c.length(segment);
        auto pieces = length == Fixed{} ? std::int64_t{1} : c.div(length, sample_gap).ceil_to_integer();
        if (pieces < 1) pieces = 1;
        if (pieces > 512) pieces = 512;
        const auto along = c.unit(segment, headings[i]);
        for (std::int64_t piece = 1; piece <= pieces; ++piece) {
            if (piece == pieces) { samples.push_back({centre[i], headings[i], static_cast<std::uint32_t>(std::min(i, n - 2))}); continue; }
            const auto t = Fixed::from_ratio(piece, pieces).value();
            samples.push_back({Vec2{c.add(centre[i - 1].x, c.mul(segment.x, t)), c.add(centre[i - 1].y, c.mul(segment.y, t))}, along,
                static_cast<std::uint32_t>(i - 1)});
        }
    }
    if (!c.ok || samples.empty()) return std::nullopt;
    // The end slot is laid out in the frame placement tested.
    samples.back().heading = c.unit(final_heading, samples.back().heading);
    if (!c.ok) return std::nullopt;
    // Narrowing: the whole slot (step 0), then three quarters, a half, a quarter, none. Each sample first takes the least
    // narrowing its own cell admits; then it takes at least as much narrowing as its neighbours, so the offset ramps in and
    // out of a gap instead of jumping across the wall beside it.
    const auto position_at = [&](const Sample& sample, std::int64_t step) {
        const auto factor = Fixed::from_ratio(row_squeeze_steps - step, row_squeeze_steps).value();
        const auto local = Vec2{c.mul(slot.x, factor), c.mul(slot.y, factor)};
        return c.add(sample.at, to_world(c, sample.heading, local));
    };
    std::vector<std::int64_t> narrowing(samples.size(), 0);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        std::int64_t step = 0;
        while (step <= row_squeeze_steps && !(c.ok && point_admitted(occupancy, mask, radius, position_at(samples[index], step)))) ++step;
        if (!c.ok || step > row_squeeze_steps) return std::nullopt;
        narrowing[index] = step;
    }
    std::vector<std::int64_t> smoothed(narrowing);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        if (index > 0) smoothed[index] = std::max(smoothed[index], narrowing[index - 1]);
        if (index + 1 < samples.size()) smoothed[index] = std::max(smoothed[index], narrowing[index + 1]);
    }
    std::vector<Vec2> nodes;
    std::vector<std::uint32_t> segments;
    nodes.push_back(member_start);
    segments.push_back(0);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const auto& sample = samples[index];
        std::optional<Vec2> chosen;
        for (std::int64_t step = smoothed[index]; step <= row_squeeze_steps && !chosen; ++step) {
            const auto at = position_at(sample, step);
            if (c.ok && point_admitted(occupancy, mask, radius, at)) chosen = at;
        }
        if (!chosen || !c.ok) return std::nullopt;
        if (*chosen == nodes.back()) continue;
        // No doubling back: while the step to this sample is behind the sample's own heading (not for the end slot, which
        // stands in the plan's frame) or turns by more than a right angle at the last node, that node lies on the inside of a
        // corner and is cut. The new sample is always kept, so one cut cannot turn every later sample away.
        const bool last = index + 1 == samples.size();
        while (nodes.size() > 1 && c.ok) {
            const auto step = c.sub(*chosen, nodes.back());
            const auto before = c.sub(nodes.back(), nodes[nodes.size() - 2]);
            if ((last || c.dot(step, sample.heading) > Fixed{}) && c.dot(step, before) >= Fixed{}) break;
            // Only cut the corner when the shortcut itself is clear: across a wall tip it is not, and the member then keeps
            // the small step back rather than losing its whole shared route.
            if (!line_clear(c, occupancy, mask, radius, nodes[nodes.size() - 2], *chosen)) break;
            nodes.pop_back();
            segments.pop_back();
        }
        if (!c.ok) return std::nullopt;
        nodes.push_back(*chosen);
        segments.push_back(sample.segment);
    }
    if (nodes.size() < 2) return std::nullopt;
    for (std::size_t i = 1; i < nodes.size(); ++i)
        if (!line_clear(c, occupancy, mask, radius, nodes[i - 1], nodes[i])) return std::nullopt;
    if (centre_segments) *centre_segments = std::move(segments);
    return nodes;
}

} // namespace eawr::sim::tactical
