#include "soak_invariants.hpp"

#include "eawr/sim/math/fixed.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace eawr::soak {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;

namespace {

double to_double(const math::Fixed value) noexcept {
    return static_cast<double>(value.raw()) / static_cast<double>(math::Fixed::scale);
}

std::string describe(const eawr::sim::EntityId id, const tactical::TypeId type) {
    std::ostringstream text;
    text << "entity " << id << " (type " << type << ')';
    return text.str();
}

std::string number(const double value) {
    std::ostringstream text;
    text.setf(std::ios::fixed);
    text.precision(2);
    text << value;
    return text.str();
}

// The unit's facing in the plane: the rotation's +X axis projected on XY.
void facing(const tactical::UnitState& unit, double& cosine, double& sine) noexcept {
    const double x = to_double(unit.rotation.x);
    const double y = to_double(unit.rotation.y);
    const double z = to_double(unit.rotation.z);
    const double w = to_double(unit.rotation.w);
    const double forward_x = 1.0 - 2.0 * (y * y + z * z);
    const double forward_y = 2.0 * (x * y + w * z);
    const double length = std::hypot(forward_x, forward_y);
    if (length < 1e-9) {
        cosine = 1.0;
        sine = 0.0;
        return;
    }
    cosine = forward_x / length;
    sine = forward_y / length;
}

double projected_radius(const double axis_x, const double axis_y, const double cosine, const double sine,
    const double half_x, const double half_y) noexcept {
    // The box's local X axis is (cosine, sine), its local Y axis (-sine, cosine).
    return half_x * std::abs(axis_x * cosine + axis_y * sine) + half_y * std::abs(-axis_x * sine + axis_y * cosine);
}

using Pair = std::pair<eawr::sim::EntityId, eawr::sim::EntityId>;

Pair ordered(const eawr::sim::EntityId a, const eawr::sim::EntityId b) noexcept {
    return a < b ? Pair{a, b} : Pair{b, a};
}

// One step of the run bookkeeping of a pair: the tick its run began on (now, when it just
// began), and the run's length in ticks.
std::uint64_t run_length(std::map<Pair, std::uint64_t>& since, const Pair& pair, const std::uint64_t tick) {
    const auto [at, inserted] = since.try_emplace(pair, tick);
    static_cast<void>(inserted);
    return tick - at->second + 1;
}

// Drops the runs whose pair was not seen at this tick.
void end_unseen_runs(std::map<Pair, std::uint64_t>& since, std::set<Pair>& reported, const std::set<Pair>& seen) {
    for (auto at = since.begin(); at != since.end();) {
        if (seen.contains(at->first)) {
            ++at;
        } else {
            reported.erase(at->first);
            at = since.erase(at);
        }
    }
}

} // namespace

double rectangle_penetration(const double ax, const double ay, const double a_cos, const double a_sin,
    const double a_hx, const double a_hy, const double bx, const double by, const double b_cos, const double b_sin,
    const double b_hx, const double b_hy) noexcept {
    const double delta_x = bx - ax;
    const double delta_y = by - ay;
    const double axes[4][2] = {{a_cos, a_sin}, {-a_sin, a_cos}, {b_cos, b_sin}, {-b_sin, b_cos}};
    double depth = std::numeric_limits<double>::infinity();
    for (const auto& axis : axes) {
        const double distance = std::abs(delta_x * axis[0] + delta_y * axis[1]);
        const double reach = projected_radius(axis[0], axis[1], a_cos, a_sin, a_hx, a_hy)
            + projected_radius(axis[0], axis[1], b_cos, b_sin, b_hx, b_hy);
        const double overlap = reach - distance;
        if (overlap <= 0.0) return 0.0;
        depth = std::min(depth, overlap);
    }
    return depth;
}

Invariants::Invariants(const tactical::MotionTable& motion, std::optional<tactical::FogRules> map, Limits limits)
    : motion_(motion), map_(std::move(map)), limits_(limits) {}

void Invariants::add(Violation violation) {
    if (violations_.size() >= max_kept) {
        ++dropped_;
        return;
    }
    violations_.push_back(std::move(violation));
}

void Invariants::observe(const std::uint64_t tick, const std::span<const tactical::UnitState> units,
    const std::span<const tactical::Squadron> squadrons) {
    ++metrics_.ticks_observed;
    check_bounds(tick, units);
    check_speed(tick, units);
    if (tick > limits_.spawn_window) {
        check_hulls(tick, units);
    }
    check_slots(tick, units, squadrons);
    previous_.clear();
    for (const auto& unit : units) {
        previous_[unit.entity_id] = Body{unit.entity_id, unit.type_id, to_double(unit.position.x),
            to_double(unit.position.y), to_double(unit.position.z)};
    }
    last_tick_ = tick;
}

// Guards: every position lies where the sim's coordinate range and the map's grid put it
// (space-movement AV-U3: a destination clipped near the map edge can lie a little off it).
void Invariants::check_bounds(const std::uint64_t tick, const std::span<const tactical::UnitState> units) {
    const double limit = static_cast<double>(tactical::max_motion_coordinate);
    double low_x = -limit;
    double high_x = limit;
    double low_y = -limit;
    double high_y = limit;
    if (map_) {
        low_x = to_double(map_->map_left);
        high_x = low_x + static_cast<double>(map_->cells_wide) * to_double(map_->cell_size);
        high_y = to_double(map_->map_top);
        low_y = high_y - static_cast<double>(map_->cells_tall) * to_double(map_->cell_size);
    }
    for (const auto& unit : units) {
        const double x = to_double(unit.position.x);
        const double y = to_double(unit.position.y);
        const double z = to_double(unit.position.z);
        const double excess = std::max({low_x - x, x - high_x, low_y - y, y - high_y, 0.0});
        metrics_.max_map_excess = std::max(metrics_.max_map_excess, excess);
        const bool off_map = excess > limits_.map_margin;
        const bool off_range = std::abs(x) >= limit || std::abs(y) >= limit || std::abs(z) >= limit;
        if ((off_map || off_range) && bounds_reported_.insert(unit.entity_id).second) {
            add({"out-of-bounds", "space-movement AV-U3 (positions stay on the map)", tick,
                describe(unit.entity_id, unit.type_id),
                "position (" + number(x) + ", " + number(y) + ", " + number(z) + ") lies " + number(excess)
                    + " units past the map's grid (allowed " + number(limits_.map_margin) + ")"});
        }
    }
}

// Guards: no unit moves faster than its type flies (space-movement MV-30 to MV-33 for ships,
// space-fighters FM-01 to FM-12 for craft). A jump like a teleport or a wrapped coordinate shows
// as a ratio far past one; abilities that speed a unit up and FM-12's catch-up stay under the factor.
void Invariants::check_speed(const std::uint64_t tick, const std::span<const tactical::UnitState> units) {
    if (!last_tick_ || tick <= *last_tick_) return;
    const auto elapsed = static_cast<double>(tick - *last_tick_);
    for (const auto& unit : units) {
        const auto before = previous_.find(unit.entity_id);
        if (before == previous_.end()) continue;
        double max_speed = 0.0;
        double allowed = limits_.speed_factor;
        bool craft_type = false;
        if (const auto* profile = motion_.find(unit.type_id)) {
            max_speed = to_double(profile->max_speed);
        } else if (const auto* craft = motion_.squadrons.find_craft(unit.type_id)) {
            max_speed = to_double(craft->max_speed);
            allowed = limits_.craft_speed_factor;
            craft_type = true;
        }
        if (max_speed <= 0.0) continue;
        const double moved = std::sqrt(std::pow(to_double(unit.position.x) - before->second.x, 2)
            + std::pow(to_double(unit.position.y) - before->second.y, 2)
            + std::pow(to_double(unit.position.z) - before->second.z, 2));
        const double ratio = moved / elapsed / max_speed;
        double& worst = craft_type ? metrics_.max_craft_speed_ratio : metrics_.max_speed_ratio;
        worst = std::max(worst, ratio);
        if (ratio > allowed && speed_reported_.insert(unit.entity_id).second) {
            add({"speed", "space-movement MV-30 to MV-33, space-fighters FM-01 (a unit flies at most its maximum speed)",
                tick, describe(unit.entity_id, unit.type_id),
                "moved " + number(moved / elapsed) + " units per tick, " + number(ratio) + " times its maximum speed "
                    + number(max_speed) + " (allowed " + number(allowed) + " times)"});
        }
    }
}

// Guards: the tracking system keeps ships apart (space-movement AV-01 to AV-05: one collision
// layer per SpaceLayer of rectangles of the hard half extents, and a query also reads the static
// layer). A pair of one layer, or a hull and a static object, is tested; ships of different
// layers pass through each other in the original game.
void Invariants::check_hulls(const std::uint64_t tick, const std::span<const tactical::UnitState> units) {
    std::vector<Hull> hulls;
    for (const auto& unit : units) {
        const auto* footprint = motion_.footprint(unit.type_id);
        if (footprint == nullptr || footprint->layer == tactical::SpaceLayer::none) continue;
        Hull hull;
        hull.id = unit.entity_id;
        hull.type = unit.type_id;
        hull.layer = footprint->layer;
        hull.obstacle = footprint->obstacle;
        hull.immobile = motion_.find(unit.type_id) == nullptr;
        hull.x = to_double(unit.position.x);
        hull.y = to_double(unit.position.y);
        facing(unit, hull.cosine, hull.sine);
        // A square of the radius for a static object, the hard rectangle for a ship.
        hull.half_x = to_double(footprint->obstacle ? footprint->radius : footprint->x_extent);
        hull.half_y = to_double(footprint->obstacle ? footprint->radius : footprint->y_extent);
        if (hull.half_x > 0.0 && hull.half_y > 0.0) hulls.push_back(hull);
    }
    std::set<Pair> seen;
    for (std::size_t first = 0; first < hulls.size(); ++first) {
        for (std::size_t second = first + 1; second < hulls.size(); ++second) {
            const auto& a = hulls[first];
            const auto& b = hulls[second];
            if (a.immobile && b.immobile) continue; // the map's own placement, not the sim's
            if (a.layer != b.layer && !a.obstacle && !b.obstacle) continue;
            const double reach = std::hypot(a.half_x, a.half_y) + std::hypot(b.half_x, b.half_y);
            if (std::hypot(b.x - a.x, b.y - a.y) >= reach) continue;
            const double depth = rectangle_penetration(a.x, a.y, a.cosine, a.sine, a.half_x, a.half_y, b.x, b.y,
                b.cosine, b.sine, b.half_x, b.half_y);
            const double smaller = std::min({a.half_x, a.half_y, b.half_x, b.half_y});
            const double share = depth / smaller;
            metrics_.max_hull_penetration = std::max(metrics_.max_hull_penetration, share);
            if (share <= limits_.hull_penetration) continue;
            const auto pair = ordered(a.id, b.id);
            seen.insert(pair);
            const auto run = run_length(hull_since_, pair, tick);
            metrics_.max_hull_run = std::max(metrics_.max_hull_run, run);
            if (run >= limits_.hull_ticks && hull_reported_.insert(pair).second) {
                add({"hull-overlap", "space-movement AV-01 to AV-05 (ships keep their hard rectangles apart)", tick,
                    describe(a.id, a.type) + " and " + describe(b.id, b.type),
                    "hulls overlap by " + number(share * 100.0) + " % of the smaller half extent ("
                        + number(depth) + " units), for " + std::to_string(run) + " ticks (allowed "
                        + number(limits_.hull_penetration * 100.0) + " % for " + std::to_string(limits_.hull_ticks)
                        + ")"});
            }
        }
    }
    end_unseen_runs(hull_since_, hull_reported_, seen);
}

// Guards: a squadron's craft hold their own formation slots (space-fighters FM-10, FM-14). Craft
// of one side that sit on each other for longer than `slot_ticks` mean two squadrons hold one
// volume. Craft of opposing sides in a dogfight may overlap (FD-11) and are not tested.
void Invariants::check_slots(const std::uint64_t tick, const std::span<const tactical::UnitState> units,
    const std::span<const tactical::Squadron> squadrons) {
    std::map<eawr::sim::EntityId, const tactical::UnitState*> by_id;
    for (const auto& unit : units) by_id[unit.entity_id] = &unit;
    struct Craft {
        const tactical::UnitState* unit{};
        eawr::sim::EntityId squadron{};
    };
    std::vector<Craft> craft;
    for (const auto& squadron : squadrons) {
        for (const auto member : squadron.members) {
            const auto at = by_id.find(member);
            if (at != by_id.end()) craft.push_back({at->second, squadron.container});
        }
    }
    std::set<Pair> seen;
    for (std::size_t first = 0; first < craft.size(); ++first) {
        for (std::size_t second = first + 1; second < craft.size(); ++second) {
            const auto& a = craft[first];
            const auto& b = craft[second];
            if (a.squadron == b.squadron || a.unit->owner != b.unit->owner) continue;
            const double distance = std::sqrt(std::pow(to_double(a.unit->position.x) - to_double(b.unit->position.x), 2)
                + std::pow(to_double(a.unit->position.y) - to_double(b.unit->position.y), 2)
                + std::pow(to_double(a.unit->position.z) - to_double(b.unit->position.z), 2));
            if (distance > limits_.slot_radius) continue;
            const auto pair = ordered(a.unit->entity_id, b.unit->entity_id);
            seen.insert(pair);
            const auto run = run_length(slot_since_, pair, tick);
            metrics_.max_slot_run = std::max(metrics_.max_slot_run, run);
            if (run >= limits_.slot_ticks && slot_reported_.insert(pair).second) {
                add({"slot-crowding", "space-fighters FM-10, FM-14 (each squadron's craft hold their own slots)", tick,
                    describe(a.unit->entity_id, a.unit->type_id) + " of squadron " + std::to_string(a.squadron) + " and "
                        + describe(b.unit->entity_id, b.unit->type_id) + " of squadron " + std::to_string(b.squadron),
                    "two craft of one side are " + number(distance) + " units apart, within " + number(limits_.slot_radius)
                        + ", for " + std::to_string(run) + " ticks (allowed " + std::to_string(limits_.slot_ticks) + ")"});
            }
        }
    }
    end_unseen_runs(slot_since_, slot_reported_, seen);
}

} // namespace eawr::soak
