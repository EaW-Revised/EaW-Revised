#include "land_phase_internal.hpp"
#include "eawr/sim/tactical/land_movement.hpp"
#include "eawr/sim/math/trig.hpp"
#include "eawr/core/tick_profile.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <numeric>
#include <span>
#include <string>

namespace eawr::sim::tactical {
namespace {
// Logging-only sink (see LandMoveTrace); written by the platform at start-up, read by the ordered commit and install_path.
std::atomic<LandMoveTraceSink> g_trace_sink{nullptr};
void emit_trace(const LandMoveTrace& event) {
    if (const auto sink = g_trace_sink.load(std::memory_order_acquire)) sink(event);
}
using math::Fixed;
using math::Vec2;
constexpr auto zero = Fixed{};
constexpr auto one = Fixed::from_raw(Fixed::scale);
constexpr auto ten = Fixed::from_raw(10 * Fixed::scale);
constexpr auto land_straggler_near_hold_divisor = 100; // WLPA-93: one percent of maximum speed
constexpr std::uint32_t land_reorder_lookahead_ticks = 30;
struct Arithmetic {
    bool valid{true};
    Fixed add(Fixed a, Fixed b) { Fixed r; valid &= math::try_add(a, b, r); return r; }
    Fixed sub(Fixed a, Fixed b) { Fixed r; valid &= math::try_subtract(a, b, r); return r; }
    Fixed mul(Fixed a, Fixed b) { Fixed r; valid &= math::try_multiply(a, b, r); return r; }
    Fixed div(Fixed a, Fixed b) { Fixed r; valid &= math::try_divide(a, b, r); return r; }
    Vec2 sub(Vec2 a, Vec2 b) { return {sub(a.x, b.x), sub(a.y, b.y)}; }
    Vec2 add(Vec2 a, Vec2 b) { return {add(a.x, b.x), add(a.y, b.y)}; }
    Vec2 mul(Vec2 a, Fixed b) { return {mul(a.x, b), mul(a.y, b)}; }
    Fixed length(Vec2 a) { Fixed r; valid &= math::try_length(a, r); return r; }
};
Fixed absolute(Fixed value) {
    return Fixed::from_raw(value.raw() == std::numeric_limits<std::int64_t>::min()
        ? std::numeric_limits<std::int64_t>::max() : value.raw() < 0 ? -value.raw() : value.raw());
}
Vec2 forward(Fixed facing) { const auto v = math::sin_cos_turn(facing); return {v.cosine, v.sine}; }
Vec2 side(Vec2 f) { return {Fixed::from_raw(-f.y.raw()), f.x}; }
Fixed dot(Arithmetic& a, Vec2 x, Vec2 y) { return a.add(a.mul(x.x, y.x), a.mul(x.y, y.y)); }
Fixed approach(Arithmetic& a, Fixed speed, Fixed desired, const LandLocomotionProfile& p, Fixed dt) {
    if (speed < desired) return std::min(desired, a.add(speed, a.mul(p.acceleration, dt)));
    return std::max(desired, a.sub(speed, a.mul(p.deceleration, dt)));
}
// WLPA-90/91: the retail corner and approach rules (see docs/behaviour/walks/land-pathing.md). A turn is 2 pi radians; the
// deviation clamp is the retail one (20 to 100 world units); a sine or (1 - cosine) below the resolution is a straight
// continuation (it would divide by zero).
constexpr auto land_turn_radians = Fixed::from_raw(105414357);
constexpr auto land_double = Fixed::from_raw(2 * Fixed::scale);
constexpr auto land_corner_deviation_low = Fixed::from_raw(20 * Fixed::scale);
constexpr auto land_corner_deviation_high = Fixed::from_raw(100 * Fixed::scale);
constexpr auto land_corner_resolution = Fixed::from_raw(16);
constexpr auto land_unlimited = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
// A heading vector (unit) of the nearest earlier segment of `nodes` that has length, else the start heading; then the
// corner fraction onto the segment that leaves `index`. A path end has no corner.
Fixed path_corner_fraction(std::span<const Vec2> nodes, std::size_t index, Fixed start_heading, const LandTurnLimits& limits);
// The speed from which a body brakes at `deceleration` to `end_speed` within `distance`: sqrt(end^2 + 2 a d).
Fixed braking_speed(Fixed end_speed, Fixed deceleration, Fixed distance) {
    Arithmetic a;
    const auto squared = a.add(a.mul(end_speed, end_speed), a.mul(a.mul(land_double, deceleration), distance));
    Fixed result;
    if (!a.valid || !math::try_sqrt(squared, result)) return land_unlimited;
    return result;
}
// The braking speed, but never one whose step in `seconds` rounds to zero: a body a few raw units from its target arrives in
// that step instead of stopping short (a tiny authored deceleration would otherwise freeze it).
Fixed brake_cap(Fixed end_speed, Fixed deceleration, Fixed distance, Fixed seconds, Fixed top) {
    auto cap = braking_speed(end_speed, deceleration, distance);
    Arithmetic a;
    if (a.mul(cap, seconds) <= zero) cap = std::min(top, a.div(distance, seconds));
    return a.valid ? cap : land_unlimited;
}
// The distance `speed` needs to brake to a stop at `deceleration`.
Fixed stopping_distance(Fixed speed, Fixed deceleration) {
    Arithmetic a;
    if (deceleration <= zero) return land_unlimited;
    const auto distance = a.div(a.mul(speed, speed), a.mul(land_double, deceleration));
    return a.valid ? distance : land_unlimited;
}
// Path length from `from` through nodes[index..], stopping once it exceeds `horizon` (past it nothing slows the body).
Fixed path_length_ahead(std::span<const Vec2> nodes, Vec2 from, std::size_t index, Fixed horizon) {
    Arithmetic a;
    Fixed total;
    auto at = from;
    for (auto n = index; n < nodes.size() && total <= horizon; ++n) {
        total = a.add(total, a.length(a.sub(nodes[n], at)));
        at = nodes[n];
    }
    return a.valid ? total : land_unlimited;
}
Fixed path_corner_fraction(std::span<const Vec2> nodes, std::size_t index, Fixed start_heading, const LandTurnLimits& limits) {
    if (!limits.turns || index + 1 >= nodes.size()) return one;
    Arithmetic a;
    auto heading = forward(start_heading);
    auto earlier = index;
    while (earlier > 0 && nodes[earlier - 1] == nodes[index]) --earlier;
    if (earlier > 0) {
        const auto before = a.sub(nodes[index], nodes[earlier - 1]);
        const auto length = a.length(before);
        if (a.valid && length > zero) heading = {a.div(before.x, length), a.div(before.y, length)};
    }
    const auto leaving = a.sub(nodes[index + 1], nodes[index]);
    if (!a.valid) return one;
    return land_corner_fraction(heading, leaving, limits);
}
// LR13-17: the start gate of a Walk_Transition member is open when the published gate says so, or (the adapter read its
// MOVESTART clip) when it has waited the clip's whole logical frames. The debug build shows that the active
// frame is at least the frame count minus 2 minus the animation's playback-rate multiplier (frames of the clip, which is one
// frame of playback whatever the rate); the adapter has already taken that off the wait. A member with no clip has nothing to wait for.
bool start_gate_open(const LandMovingMember& m) {
    if (m.gates.start_almost_finished) return true;
    const auto& p = m.profile;
    if (!p.start_clock) return false;
    return m.transition_frame.raw() >= static_cast<std::int64_t>(p.start_clip_ticks) * Fixed::scale;
}
bool valid_path(const LandMovingSquad& squad) {
    // A container that a contact pushed to an open spot walks its temporary route with no base path (WLPA-57).
    return squad.next_node <= squad.path.nodes.size()
        && ((squad.order != LandMoveOrder::following && squad.order != LandMoveOrder::recovering)
            || squad.temporary_first != 0
            || (squad.path.status == LandPathStatus::found && !squad.path.nodes.empty()));
}
// WLPA-82: a soft pair is two Infantry/Bike bodies.
bool soft_group(units::LandCollisionGroup group) { return land_soft_collision_class(group); }
bool moving_order(const LandMovingSquad& s) {
    return s.order == LandMoveOrder::following || s.order == LandMoveOrder::recovering || s.order == LandMoveOrder::yielded;
}
// A container that a contact pushed to an open spot has no base path: when its temporary route is cleared its move is
// complete (the rule `release` applies), or the state would be refused by from_checkpoint.
void settle_routeless(LandMovingSquad& s) {
    if (!s.generic_team && s.temporary_first == 0 && s.path.nodes.empty()
        && (s.order == LandMoveOrder::following || s.order == LandMoveOrder::recovering)) s.order = LandMoveOrder::reached;
}
// Validate every crossed cell rectangle, including diagonal corner cells. This is conservative at
// corners, like L4's no-corner-cut rule; it never tunnels through a blocked cell on a long step.
bool admitted(const LandOccupancy& grid, Vec2 from, Vec2 to, Fixed radius, std::uint32_t mask) {
    const auto pitch = grid.terrain().spacing();
    const auto footprint = land_footprint(radius, pitch);
    auto x = land_cell_of(from.x, pitch, grid.terrain().width());
    auto y = land_cell_of(from.y, pitch, grid.terrain().height());
    const auto ex = land_cell_of(to.x, pitch, grid.terrain().width());
    const auto ey = land_cell_of(to.y, pitch, grid.terrain().height());
    if (!x || !y || !ex || !ey) return false;
    Arithmetic a;
    const auto delta = a.sub(to, from);
    const auto extent = std::max(absolute(delta.x), absolute(delta.y));
    const auto count = std::max<std::int64_t>(1, a.div(extent, Fixed::from_raw(pitch.raw() / 2)).ceil_to_integer());
    if (!a.valid) return false;
    for (std::int64_t i = 0; i <= count; ++i) {
        const auto fraction = Fixed::from_ratio(i, count).value();
        const auto p = a.add(from, a.mul(delta, fraction));
        const auto cx = land_cell_of(p.x, pitch, grid.terrain().width());
        const auto cy = land_cell_of(p.y, pitch, grid.terrain().height());
        if (!cx || !cy) return false;
        const auto lo_x = static_cast<std::int64_t>(std::min(*x, *cx)) + footprint.low;
        const auto lo_y = static_cast<std::int64_t>(std::min(*y, *cy)) + footprint.low;
        const auto hi_x = static_cast<std::int64_t>(std::max(*x, *cx)) + footprint.high;
        const auto hi_y = static_cast<std::int64_t>(std::max(*y, *cy)) + footprint.high;
        if (lo_x < 0 || lo_y < 0 || hi_x >= grid.terrain().width() || hi_y >= grid.terrain().height()
            || !grid.admits_footprint(static_cast<std::uint32_t>(lo_x), static_cast<std::uint32_t>(lo_y),
                static_cast<std::uint32_t>(hi_x), static_cast<std::uint32_t>(hi_y), mask)) return false;
        x = cx; y = cy;
    }
    return a.valid;
}
bool active(LandMoveOrder order) { return order == LandMoveOrder::following || order == LandMoveOrder::recovering; }
// WLPA-85: the members of a reached non-generic squad keep finishing passive slot recovery after the container's move ended.
bool follows(const LandMovingSquad& squad) {
    return active(squad.order) || (squad.order == LandMoveOrder::reached && !squad.generic_team);
}
}

Fixed land_corner_fraction(Vec2 heading, Vec2 segment, const LandTurnLimits& limits) {
    if (!limits.turns || limits.maximum_speed <= zero) return one;
    // A local checker: a corner outside the fixed-point range simply does not limit; it is never a simulation failure.
    Arithmetic a;
    const auto length = a.length(segment);
    if (!a.valid || length <= zero) return one;
    const Vec2 unit{a.div(segment.x, length), a.div(segment.y, length)};
    const auto cosine = std::clamp(dot(a, heading, unit), Fixed::from_raw(-Fixed::scale), one);
    const auto sine = absolute(a.sub(a.mul(heading.x, unit.y), a.mul(heading.y, unit.x)));
    const auto omega = a.mul(limits.turn_rate, land_turn_radians);
    auto speed = limits.maximum_speed;
    if (sine > land_corner_resolution) speed = std::min(speed, a.div(a.mul(omega, length), a.mul(sine, land_double)));
    const auto bend = a.sub(one, cosine);
    if (bend > land_corner_resolution) {
        const auto deviation = std::clamp(a.div(length, land_double), land_corner_deviation_low, land_corner_deviation_high);
        speed = std::min(speed, a.div(a.mul(deviation, omega), bend));
    }
    const auto fraction = a.div(speed, limits.maximum_speed);
    if (!a.valid) return one;
    if (fraction < limits.minimum_fraction) return zero;
    return std::clamp(fraction, zero, one);
}

LandTurnLimits LandMovement::turn_limits(std::size_t index) const {
    const auto& s = state_.squads[index];
    LandTurnLimits limits;
    limits.maximum_speed = s.maximum_speed;
    limits.turns = !s.generic_team && s.maximum_speed > zero && !squad_members_[index].empty();
    bool first = true;
    for (const auto member : squad_members_[index]) {
        const auto& profile = state_.members[member].profile;
        if (profile.turn_rate <= zero) limits.turns = false;
        if (first || profile.turn_rate < limits.turn_rate) limits.turn_rate = profile.turn_rate;
        if (profile.deceleration > zero && (limits.deceleration == zero || profile.deceleration < limits.deceleration))
            limits.deceleration = profile.deceleration;
        limits.minimum_fraction = std::max(limits.minimum_fraction, profile.min_speed_fraction_for_turn);
        if (first || profile.close_enough_angle < limits.close_enough) limits.close_enough = profile.close_enough_angle;
        first = false;
    }
    if (limits.deceleration == zero) {
        // Retail: a team container with no authored deceleration brakes with its maximum speed per frame (a one-frame stop).
        Arithmetic a;
        limits.deceleration = a.mul(s.maximum_speed, Fixed::from_raw(static_cast<std::int64_t>(logical_frames_per_second) * Fixed::scale));
        if (!a.valid) limits.turns = false;
    }
    return limits;
}

std::optional<std::vector<Vec2>> land_rows_layout(std::span<const LandRowsInput> members) {
    const auto count = members.size();
    std::vector<Vec2> slots(count);
    if (count == 0) return slots;
    Arithmetic a;
    const auto two = Fixed::from_raw(2 * Fixed::scale);
    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) {
        if (members[x].forward != members[y].forward) return members[x].forward > members[y].forward;
        return members[x].id < members[y].id;
    });
    std::size_t columns = 1;
    while (columns * columns < count) ++columns;
    Fixed row_x, previous_depth;
    for (std::size_t begin = 0, row = 0; begin < count; begin += columns, ++row) {
        const auto end = std::min(count, begin + columns);
        std::sort(order.begin() + static_cast<std::ptrdiff_t>(begin), order.begin() + static_cast<std::ptrdiff_t>(end),
            [&](std::size_t x, std::size_t y) {
                if (members[x].side != members[y].side) return members[x].side < members[y].side;
                return members[x].id < members[y].id;
            });
        Fixed width, depth;
        for (auto k = begin; k < end; ++k) {
            const auto& m = members[order[k]];
            width = a.add(width, a.add(a.mul(two, m.hard_y), m.gap));
            depth = std::max(depth, a.add(a.mul(two, m.hard_x), m.gap));
        }
        if (row > 0) row_x = a.sub(row_x, a.div(a.add(previous_depth, depth), two));
        auto cursor = a.sub(zero, a.div(width, two));
        for (auto k = begin; k < end; ++k) {
            const auto& m = members[order[k]];
            const auto cell = a.add(a.mul(two, m.hard_y), m.gap);
            slots[order[k]] = {row_x, a.add(cursor, a.div(cell, two))};
            cursor = a.add(cursor, cell);
        }
        previous_depth = depth;
    }
    if (!a.valid) return std::nullopt;
    return slots;
}

Fixed land_member_desired_speed(Fixed parent, Fixed maximum, Fixed error, bool turning) noexcept {
    if (turning || maximum <= zero) return zero;
    Arithmetic a;
    parent = std::clamp(parent, zero, maximum);
    Fixed desired = parent;
    // LR3-15: inclusive one-unit deadband, lag / 10; overshoot uses its square.
    if (error > one) {
        const auto blend = std::clamp(a.div(error, ten), zero, one);
        desired = a.add(parent, a.mul(a.sub(maximum, parent), blend));
    } else if (error < Fixed::from_raw(-Fixed::scale)) {
        const auto blend = std::clamp(a.div(error, ten), Fixed::from_raw(-Fixed::scale), zero);
        desired = a.mul(parent, a.sub(one, a.mul(blend, blend)));
    }
    return a.valid ? std::clamp(desired, zero, maximum) : zero;
}

std::optional<LandMovement> LandMovement::from_checkpoint(Checkpoint saved) {
    LandMovement result;
    std::vector<EntityId> ids;
    for (const auto& squad : saved.squads) {
        if (!squad.object || squad.jiggle < zero || squad.assigned_speed < zero || squad.collision_speed < zero
            || squad.collision_class > units::LandCollisionGroup::giant_vehicle
            || (squad.generic_team && squad.collision_class != units::LandCollisionGroup::none)
            || squad.maximum_speed < zero || squad.speed < zero || squad.rear_extent < zero
            || squad.formation_length < zero || squad.straggler_wait_remaining > land_straggler_wait_visits
            || squad.temporary_next > squad.temporary_path.nodes.size()
            || (squad.temporary_first && (squad.temporary_path.status != LandPathStatus::found
                || squad.temporary_path.nodes.size() < 2))
            || !valid_path(squad) || squad.order > LandMoveOrder::reached
            || (squad.order == LandMoveOrder::yielded && !squad.yielding_to)) return std::nullopt;
        ids.push_back(squad.object);
    }
    for (const auto& m : saved.members) {
        if (!m.object || m.squad >= saved.squads.size() || m.profile.maximum_speed < zero
            || m.profile.acceleration < zero || m.profile.deceleration < zero || m.profile.turn_rate < zero
            || m.profile.min_speed_fraction_for_turn < zero || m.profile.min_speed_fraction_for_turn > one
            || m.profile.close_enough_angle < zero
            || m.profile.radius < zero || m.speed < zero || m.speed > m.profile.maximum_speed
            || m.transition_frame < zero || m.deployment_frame < zero || m.straggler_best_lag < Fixed::from_raw(-1)
            || m.assigned_speed < zero || m.collision_speed < zero
            || m.hard_x < zero || m.hard_y < zero || m.gap < zero
            || m.state > LandWalkState::undeploying) return std::nullopt;
        ids.push_back(m.object);
        const auto& s = saved.squads[m.squad];
        if (m.next_node > s.path.nodes.size() || m.temporary_next > s.temporary_path.nodes.size()) return std::nullopt;
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return std::nullopt;
    if (saved.contact_settings.prediction_seconds < zero || saved.contact_settings.minimum_prediction_distance < zero)
        return std::nullopt;
    // WLPA-82: a contact names collision bodies: a formation squad with a class (its object id) or a generic-team member.
    std::vector<EntityId> body_ids;
    std::vector<const LandMovingMember*> first_member(saved.squads.size(), nullptr);
    for (const auto& m : saved.members) {
        if (saved.squads[m.squad].generic_team) body_ids.push_back(m.object);
        if (!first_member[m.squad]) first_member[m.squad] = &m;
    }
    for (std::size_t i = 0; i < saved.squads.size(); ++i) {
        const auto& squad = saved.squads[i];
        if (squad.generic_team || !first_member[i]) continue;
        const auto group = squad.has_formation ? squad.collision_class : first_member[i]->profile.collision_class;
        if (group != units::LandCollisionGroup::none) body_ids.push_back(squad.object);
    }
    std::sort(body_ids.begin(), body_ids.end());
    for (std::size_t i = 0; i < saved.contacts.size(); ++i) {
        const auto& c = saved.contacts[i];
        const auto body_exists = [&](EntityId id) { return std::binary_search(body_ids.begin(), body_ids.end(), id); };
        if (!body_exists(c.first) || (c.second && (!body_exists(c.second) || c.second <= c.first))
            || ((c.kind == LandContactKind::terrain) != (c.second == 0))
            || c.kind > LandContactKind::moving_moving || c.state > LandContactState::recovery
            || c.route > LandContactRoute::escape_opposed
            || (c.first_speed_limit && *c.first_speed_limit < zero)
            || (c.second_speed_limit && *c.second_speed_limit < zero)
            || c.search > LandContactSearch::installed || c.deadlock_remaining > 30 || c.search_passes > 2
            || c.retry_remaining > land_contact_retry_visits
            || (c.primary && c.primary != c.first && c.primary != c.second)
            || (c.route_actor && c.route_actor != c.first && c.route_actor != c.second)
            || (c.installed && (!c.route_actor || !c.search_succeeded || c.search != LandContactSearch::installed))
            || (c.recorded_member && c.recorded_member != c.first && c.recorded_member != c.second)) return std::nullopt;
        if (i && std::pair{saved.contacts[i - 1].first, saved.contacts[i - 1].second} >= std::pair{c.first, c.second})
            return std::nullopt;
    }
    for (const auto& s : saved.squads) if (s.temporary_first) {
        const auto contact = std::find_if(saved.contacts.begin(), saved.contacts.end(), [&](const auto& c) {
            return c.first == s.temporary_first && c.second == s.temporary_second;
        });
        if (contact == saved.contacts.end() || !contact->installed) return std::nullopt;
    }
    // The squad a body id acts for: a generic member's squad, or the container's own squad.
    const auto squad_of_body = [&](EntityId id) -> std::size_t {
        for (const auto& m : saved.members) if (m.object == id) return m.squad;
        for (std::size_t i = 0; i < saved.squads.size(); ++i) if (saved.squads[i].object == id) return i;
        return saved.squads.size();
    };
    for (const auto& c : saved.contacts) if (c.installed && c.state != LandContactState::resolved) {
        const auto index = squad_of_body(c.route_actor);
        if (index >= saved.squads.size()) return std::nullopt;
        const auto& s = saved.squads[index];
        if (s.temporary_first != c.first || s.temporary_second != c.second) return std::nullopt;
    }
    result.state_ = std::move(saved);
    result.proposals_.resize(result.state_.members.size());
    result.member_motion_inputs_.resize(result.state_.members.size());
    result.staged_slot_.resize(result.state_.members.size());
    result.staged_straggler_lag_.resize(result.state_.members.size());
    result.member_visits_.resize(result.state_.members.size());
    result.squad_visits_.resize(result.state_.squads.size());
    result.squad_proposals_.resize(result.state_.squads.size());
    result.member_order_.resize(result.state_.members.size());
    result.squad_members_.resize(result.state_.squads.size());
    result.contact_order_.reserve(result.state_.contacts.size());
    result.contact_observations_.resize(result.state_.contacts.size());
    for (std::size_t i = 0; i < result.state_.members.size(); ++i)
        result.squad_members_[result.state_.members[i].squad].push_back(i);
    for (std::size_t i = 0; i < result.squad_members_.size(); ++i) if (result.state_.squads[i].generic_team) {
        auto& group = result.squad_members_[i];
        std::sort(group.begin(), group.end(), [&](auto x, auto y) {
            return result.state_.members[x].object < result.state_.members[y].object;
        });
    }
    for (std::size_t i = 0; i < result.state_.squads.size(); ++i)
        if (result.state_.squads[i].generic_team && result.squad_members_[i].empty()) return std::nullopt;
    std::iota(result.member_order_.begin(), result.member_order_.end(), std::size_t{});
    std::sort(result.member_order_.begin(), result.member_order_.end(), [&](auto x, auto y) {
        return result.state_.members[x].object < result.state_.members[y].object;
    });
    result.build_bodies();
    return result;
}


void LandMovement::build_bodies() {
    bodies_.clear();
    body_of_member_.assign(state_.members.size(), no_body);
    body_of_squad_.assign(state_.squads.size(), no_body);
    for (const auto i : member_order_) {
        const auto& m = state_.members[i];
        if (!state_.squads[m.squad].generic_team) continue;
        Body body;
        body.object = m.object; body.squad = m.squad; body.member = i; body.holder = i; body.generic = true;
        bodies_.push_back(body);
    }
    for (std::size_t i = 0; i < state_.squads.size(); ++i) {
        const auto& s = state_.squads[i];
        if (s.generic_team || squad_members_[i].empty()) continue;
        // WLPA-82: a formation team is a body by its container's class; a single object by its own.
        const auto group = s.has_formation ? s.collision_class : state_.members[squad_members_[i].front()].profile.collision_class;
        if (group == units::LandCollisionGroup::none) continue;
        Body body;
        body.object = s.object; body.squad = i; body.holder = squad_members_[i].front(); body.container = true;
        body.single = !s.has_formation;
        bodies_.push_back(body);
    }
    std::sort(bodies_.begin(), bodies_.end(), [](const Body& x, const Body& y) { return x.object < y.object; });
    for (std::size_t b = 0; b < bodies_.size(); ++b) {
        if (bodies_[b].container) body_of_squad_[bodies_[b].squad] = b;
        else body_of_member_[bodies_[b].member] = b;
    }
    const auto count = bodies_.size();
    neighbours_.resize(count);
    neighbour_candidates_.assign(count, {});
    body_peers_.assign(count, {});
    pair_visits_.assign(count, 0);
    discovered_.assign(count, {});
    discovery_valid_.assign(count, 1);
    pair_blocked_.assign(count, 0);
    body_proposed_.assign(count, {});
    refresh_bodies();
}

std::size_t LandMovement::body_index(EntityId id) const {
    const auto at = std::lower_bound(bodies_.begin(), bodies_.end(), id,
        [](const Body& body, EntityId value) { return body.object < value; });
    return at != bodies_.end() && at->object == id ? static_cast<std::size_t>(at - bodies_.begin()) : bodies_.size();
}

// WLPA-95: a formation squad's box is centred on the container, oriented by its facing, with half extents of half the
// span of the members' base offsets widened by their hard extents, plus the container's jiggle distance. The radius is
// the box diagonal. A member that authors no hard extent contributes its soft radius instead.
void LandMovement::refresh_bodies() {
    for (auto& body : bodies_) {
        const auto& s = state_.squads[body.squad];
        body.active = s.order == LandMoveOrder::following || s.order == LandMoveOrder::recovering;
        body.moving = moving_order(s);
        body.valid = true;
        if (!body.container) {
            const auto& m = state_.members[body.member];
            body.position = m.position; body.facing = m.facing; body.speed = m.speed;
            body.maximum_speed = m.profile.maximum_speed; body.acceleration = m.profile.acceleration;
            body.assigned_speed = m.assigned_speed; body.collision_speed = m.collision_speed;
            body.group = m.profile.collision_class; body.rank = static_cast<std::uint32_t>(body.group); body.progress = m.progress;
            body.radius = m.profile.radius;
            body.half_x = m.hard_x > zero ? m.hard_x : m.profile.radius;
            body.half_y = m.hard_y > zero ? m.hard_y : m.profile.radius;
            continue;
        }
        body.position = s.position; body.facing = s.facing; body.speed = s.speed;
        body.maximum_speed = s.maximum_speed; body.assigned_speed = s.assigned_speed; body.collision_speed = s.collision_speed;
        body.group = body.single ? state_.members[body.holder].profile.collision_class : s.collision_class;
        body.rank = static_cast<std::uint32_t>(body.group); // WLPA-73: the class order is the ordering rank (every body)
        body.progress = s.progress;
        Arithmetic a;
        Fixed low_x, high_x, low_y, high_y, acceleration;
        bool first = true;
        for (const auto index : squad_members_[body.squad]) {
            const auto& m = state_.members[index];
            const auto hx = m.hard_x > zero ? m.hard_x : m.profile.radius;
            const auto hy = m.hard_y > zero ? m.hard_y : m.profile.radius;
            const auto lx = a.sub(m.slot.x, hx), hix = a.add(m.slot.x, hx);
            const auto ly = a.sub(m.slot.y, hy), hiy = a.add(m.slot.y, hy);
            if (first || lx < low_x) low_x = lx;
            if (first || hix > high_x) high_x = hix;
            if (first || ly < low_y) low_y = ly;
            if (first || hiy > high_y) high_y = hiy;
            if (m.profile.acceleration > zero && (acceleration == zero || m.profile.acceleration < acceleration))
                acceleration = m.profile.acceleration;
            first = false;
        }
        const auto two = Fixed::from_raw(2 * Fixed::scale);
        body.half_x = a.add(a.div(a.sub(high_x, low_x), two), s.jiggle);
        body.half_y = a.add(a.div(a.sub(high_y, low_y), two), s.jiggle);
        body.acceleration = acceleration;
        body.radius = a.length({body.half_x, body.half_y});
        // The box rule (WLPA-95) is for formation teams: a single object (hero, bike, lone vehicle) keeps its own radius.
        if (body.single) body.radius = state_.members[body.holder].profile.radius;
        body.valid = a.valid;
    }
}

bool LandMovement::same_heading(Fixed x_facing, Fixed y_facing) noexcept {
    Arithmetic a;
    const auto xa = math::sin_cos_turn(x_facing), ya = math::sin_cos_turn(y_facing);
    return a.add(a.mul(xa.cosine, ya.cosine), a.mul(xa.sine, ya.sine)) > Fixed::from_ratio(1, 2).value();
}

bool LandMovement::soft_opposed(const Body& x, const Body& y, bool enabled) noexcept {
    if (!enabled || x.squad == y.squad || x.generic || y.generic || !x.active || !y.active
        || !soft_group(x.group) || !soft_group(y.group)) return false;
    if (!same_heading(x.facing, y.facing) || x.assigned_speed == zero || y.assigned_speed == zero) return true;
    // A leader that has stopped (collision speed zero: refused by terrain, held by a third body) leaves its follower
    // waiting for ever behind a same-direction row; only the body ahead is read, a held follower must not release itself.
    return (ahead_of(x, y) ? x : y).collision_speed == zero;
}

bool LandMovement::ahead_of(const Body& x, const Body& y) noexcept {
    Arithmetic a;
    const auto xa = math::sin_cos_turn(x.facing), ya = math::sin_cos_turn(y.facing);
    // One score for the pair, taken from the lower object id, so exactly one of the two is ahead.
    const bool x_low = x.object < y.object;
    const auto& low = x_low ? x : y; const auto& high = x_low ? y : x;
    const auto score = a.add(a.mul(a.sub(low.position.x, high.position.x), a.add(xa.cosine, ya.cosine)),
        a.mul(a.sub(low.position.y, high.position.y), a.add(xa.sine, ya.sine)));
    return x_low == (score >= zero);
}

bool LandMovement::leads(const Body& x, const Body& y, bool enabled) noexcept {
    if (!enabled || x.squad == y.squad || x.generic || y.generic || !x.container || !y.container || !x.active || !y.active
        || !soft_group(x.group) || !soft_group(y.group) || !same_heading(x.facing, y.facing)) return false;
    return ahead_of(x, y);
}

// Oriented-box intersection coefficient (the stopped-pair overlap measure): the least over the four box axes of
// 1 - |centre offset on the axis| / (summed projections), zero on any axis that separates, at most one.
Fixed LandMovement::box_overlap(const Body& x, const Body& y) {
    Arithmetic a;
    const auto xa = math::sin_cos_turn(x.facing), ya = math::sin_cos_turn(y.facing);
    const Vec2 xu{xa.cosine, xa.sine}, xv{a.sub(zero, xa.sine), xa.cosine};
    const Vec2 yu{ya.cosine, ya.sine}, yv{a.sub(zero, ya.sine), ya.cosine};
    const auto offset = a.sub(y.position, x.position);
    Fixed result = one;
    for (const auto axis : {xu, xv, yu, yv}) {
        const auto project = [&](const Body& body, Vec2 u, Vec2 v) {
            return a.add(absolute(a.mul(body.half_x, dot(a, axis, u))), absolute(a.mul(body.half_y, dot(a, axis, v))));
        };
        const auto total = a.add(project(x, xu, xv), project(y, yu, yv));
        Fixed coefficient;
        if (total > zero) {
            const auto distance = absolute(dot(a, axis, offset));
            if (distance <= total) coefficient = a.sub(one, a.div(distance, total));
        }
        result = std::min(result, coefficient);
    }
    return a.valid ? result : zero;
}

std::optional<LandMovement::DetachedSquad> LandMovement::detach_squad(const EntityId object) {
    const auto squad = std::find_if(state_.squads.begin(), state_.squads.end(),
        [object](const auto& row) { return row.object == object; });
    if (squad == state_.squads.end()) return std::nullopt;
    const auto index = static_cast<std::size_t>(squad - state_.squads.begin());
    DetachedSquad detached{*squad, {}};
    for (const auto& member : state_.members)
        if (member.squad == index) detached.members.push_back(member);
    if (detached.members.empty()) return std::nullopt;
    // Apply to a copy so an invalid intermediate state cannot lose the original roster.
    auto next = from_checkpoint(state_);
    if (!next) return std::nullopt;
    for (const auto& member : detached.members)
        if (!next->remove_member(member.object)) return std::nullopt;
    const bool reference = reference_pair_scans_, repulsion = straggler_repulsion_;
    *this = std::move(*next);
    reference_pair_scans_ = reference; straggler_repulsion_ = repulsion;
    return detached;
}

bool LandMovement::restore_squad(DetachedSquad detached, const math::Vec2 position) {
    if (detached.members.empty()) return false;
    auto saved = state_;
    auto& squad = detached.squad;
    math::Fixed dx, dy;
    if (!math::try_subtract(position.x, squad.position.x, dx)
        || !math::try_subtract(position.y, squad.position.y, dy)) return false;
    squad.position = position;
    squad.path = {}; squad.next_node = 0; squad.order = LandMoveOrder::idle;
    squad.speed = {}; squad.assigned_speed = {}; squad.collision_speed = {};
    squad.temporary_path = {}; squad.temporary_next = 0; squad.temporary_first = 0; squad.temporary_second = 0;
    squad.yielding_to = 0; squad.blocker_progress = 0; squad.formation = 0; squad.formation_slot = {};
    squad.terrain_blocked = false; squad.terrain_target = {}; squad.turning = false; squad.awaiting_splice = false;
    const auto index = saved.squads.size();
    saved.squads.push_back(std::move(squad));
    for (auto& member : detached.members) {
        if (!math::try_add(member.position.x, dx, member.position.x)
            || !math::try_add(member.position.y, dy, member.position.y)) return false;
        member.squad = index; member.speed = {}; member.stop_velocity = {};
        member.assigned_speed = {}; member.collision_speed = {}; member.collision_lock = 0;
        member.state = LandWalkState::stopped; member.next_node = 0; member.temporary_next = 0;
        member.terrain_blocked = false; member.terrain_target = {};
        saved.members.push_back(std::move(member));
    }
    std::sort(saved.members.begin(), saved.members.end(), [](const auto& a, const auto& b) { return a.object < b.object; });
    auto rebuilt = from_checkpoint(std::move(saved));
    if (!rebuilt) return false;
    const bool reference = reference_pair_scans_, repulsion = straggler_repulsion_;
    *this = std::move(*rebuilt);
    reference_pair_scans_ = reference; straggler_repulsion_ = repulsion;
    return true;
}

bool LandMovement::remove_member(const EntityId object) {
    const auto member = std::lower_bound(state_.members.begin(), state_.members.end(), object,
        [](const auto& row, EntityId id) { return row.object < id; });
    if (member == state_.members.end() || member->object != object) return false;
    const auto squad_index = member->squad;
    const auto squad_id = state_.squads[squad_index].object;
    // A removed endpoint cannot retain a late contact or a temporary route in another squad.
    for (std::size_t i = 0; i < state_.squads.size(); ++i) {
        auto& squad = state_.squads[i];
        if (squad.temporary_first == object || squad.temporary_second == object) {
            squad.temporary_path = {}; squad.temporary_next = 0;
            squad.temporary_first = 0; squad.temporary_second = 0;
            for (auto& survivor : state_.members) if (survivor.squad == i) survivor.temporary_next = 0;
            settle_routeless(squad);
        }
        if (squad.yielding_to == object || squad.yielding_to == squad_id) {
            squad.yielding_to = 0; squad.blocker_progress = 0;
            if (squad.order == LandMoveOrder::yielded) squad.order = LandMoveOrder::recovering;
        }
    }
    std::erase_if(state_.contacts, [object](const auto& row) { return row.first == object || row.second == object; });
    state_.members.erase(member);
    if (std::none_of(state_.members.begin(), state_.members.end(), [squad_index](const auto& row) { return row.squad == squad_index; })) {
        // The last soldier of a formation squad takes its container body with it.
        for (auto& squad : state_.squads) if (squad.temporary_first == squad_id || squad.temporary_second == squad_id) {
            squad.temporary_path = {}; squad.temporary_next = 0; squad.temporary_first = 0; squad.temporary_second = 0;
            settle_routeless(squad);
        }
        std::erase_if(state_.contacts, [squad_id](const auto& row) { return row.first == squad_id || row.second == squad_id; });
        state_.squads.erase(state_.squads.begin() + static_cast<std::ptrdiff_t>(squad_index));
        for (auto& survivor : state_.members) if (survivor.squad > squad_index) --survivor.squad;
    }
    // Death is a sparse ordered lifecycle operation. Rebuild scratch once per removal;
    // ordinary per-member movement and occupancy preparation remain partitioned.
    auto rebuilt = from_checkpoint(std::move(state_));
    if (!rebuilt) return false;
    const bool reference = reference_pair_scans_, repulsion = straggler_repulsion_;
    *this = std::move(*rebuilt);
    reference_pair_scans_ = reference; straggler_repulsion_ = repulsion;
    return true;
}

// WLPA-45: notification invalidates both endpoints immediately, before another ordinary turn.
void LandMovement::invalidate_contacts(std::size_t squad) {
    const auto clear_route = [&](std::size_t index) {
        auto& s = state_.squads[index];
        s.temporary_first = 0; s.temporary_second = 0; s.temporary_next = 0;
        s.temporary_path.nodes.clear();
        settle_routeless(s);
        for (const auto member : squad_members_[index]) {
            state_.members[member].collision_lock = 0;
            state_.members[member].temporary_next = 0;
        }
    };
    for (auto& contact : state_.contacts) {
        bool addressed = false;
        for (const auto& body : bodies_) if (body.squad == squad)
            addressed |= contact.first == body.object || contact.second == body.object;
        if (!addressed) continue;
        contact.state = LandContactState::resolved;
        contact.first_stopped = false; contact.second_stopped = false;
        contact.first_speed_limit.reset(); contact.second_speed_limit.reset();
        for (std::size_t index = 0; index < state_.squads.size(); ++index)
            if (state_.squads[index].temporary_first == contact.first
                && state_.squads[index].temporary_second == contact.second) clear_route(index);
    }
    clear_route(squad);
}

void set_land_move_trace_sink(LandMoveTraceSink sink) noexcept { g_trace_sink.store(sink, std::memory_order_release); }

bool LandMovement::install_path(std::size_t index, LandPath path, std::uint64_t start) {
    if (index >= state_.squads.size() || path.status != LandPathStatus::found || path.nodes.empty()
        || path.nodes.front() != state_.squads[index].position) return false;
    auto& s = state_.squads[index];
    const bool was_moving = active(s.order);
    invalidate_contacts(index);
    s.path = std::move(path); s.next_node = 1; s.following_start = start;
    s.turn_start_heading = s.facing; s.awaiting_splice = false; // WLPA-90: the first corner is measured from this heading
    s.order = LandMoveOrder::following; s.yielding_to = 0; s.blocker_progress = 0;
    s.straggler_wait_remaining = land_straggler_wait_visits;
    for (const auto member : squad_members_[index]) state_.members[member].straggler_best_lag = Fixed::from_raw(-1);
    // WLPA-68: a path accepted from a non-moving order first turns the squad in place; a squad already
    // moving keeps rotating while it translates. A squad already turning stays turning: a re-order only
    // replaces the heading it turns toward (the retail turning service re-reads the desired facing from the
    // current path every service; only the idle state enters turning). A squad with no positive turn rate
    // never enters the state.
    bool can_turn = !squad_members_[index].empty();
    for (const auto member : squad_members_[index]) can_turn = can_turn && state_.members[member].profile.turn_rate > zero;
    const bool was_turning = s.turning;
    s.turning = !s.generic_team && can_turn && (!was_moving || s.turning);
    if (s.turning && !was_turning && !s.has_formation) {
        // WLPA-91: a single object (no team container) walks off at once, rotating as it goes, when its first heading error
        // is within one frame's turn or within the move-start angle and its first corner keeps some speed. Only the team
        // container always turns in place first.
        const auto limits = turn_limits(index);
        Arithmetic a;
        for (std::size_t n = 1; n < s.path.nodes.size(); ++n) {
            const auto delta = a.sub(s.path.nodes[n], s.position);
            if (delta.x == zero && delta.y == zero) continue;
            const auto heading = math::atan2_turn(delta.y, delta.x);
            if (!a.valid || !heading) break;
            const auto error = absolute(math::wrap_turn(a.sub(heading.value(), s.facing)));
            const auto frame_turn = a.mul(limits.turn_rate, Fixed::from_ratio(1, logical_frames_per_second).value());
            const bool within_start_angle = error <= limits.close_enough
                && path_corner_fraction(s.path.nodes, 0, s.turn_start_heading, limits) > zero;
            if (a.valid && (error <= frame_turn || within_start_angle)) s.turning = false;
            break;
        }
    }
    if (s.turning) {
        // Logging only: a turn from standing, or a re-order that retargets a turn already under way.
        LandMoveTrace event;
        event.kind = was_turning ? LandMoveTrace::Kind::turn_retarget : LandMoveTrace::Kind::turn_enter;
        event.tick = state_.tick; event.squad = s.object; event.has_old_heading = true;
        event.old_heading = event.new_heading = s.facing;
        emit_trace(event);
    }
    if (s.generic_team) for (const auto member : squad_members_[index]) state_.members[member].next_node = 1;
    return true;
}
std::optional<Vec2> LandMovement::splice_anchor(std::size_t index) const {
    if (index >= state_.squads.size()) return std::nullopt;
    const auto& s = state_.squads[index];
    if (s.generic_team || s.order != LandMoveOrder::following || s.speed <= zero || s.turning || s.temporary_first != 0
        || state_.tick < s.following_start || s.path.status != LandPathStatus::found
        || s.next_node >= s.path.nodes.size()) return std::nullopt;
    return s.path.nodes[s.next_node];
}

std::optional<Vec2> LandMovement::bounded_splice_anchor(std::size_t index) const {
    const auto end = splice_anchor(index);
    if (!end) return std::nullopt;
    const auto& s = state_.squads[index];
    Arithmetic a;
    const auto delta = a.sub(*end, s.position);
    const auto length = a.length(delta);
    const auto seconds = Fixed::from_ratio(land_reorder_lookahead_ticks, logical_frames_per_second).value();
    const auto distance = a.mul(s.speed, seconds);
    if (!a.valid || distance >= length || distance <= zero) return end;
    const auto anchor = a.add(s.position, a.mul(delta, a.div(distance, length)));
    return a.valid && anchor != s.position ? std::optional<Vec2>{anchor} : end;
}

void LandMovement::cut_splice_anchor(std::size_t index, Vec2 anchor) {
    auto& s = state_.squads[index];
    s.path.nodes.resize(s.next_node + 1);
    s.path.nodes.back() = anchor;
    s.awaiting_splice = true; // WLPA-90: the cut end is not a destination; the squad does not brake for it
}

LandMovement::SpliceResult LandMovement::splice_path(std::size_t index, LandPath path) {
    const auto anchor = splice_anchor(index);
    if (!anchor || path.status != LandPathStatus::found || path.nodes.empty() || path.nodes.front() != *anchor)
        return SpliceResult::rejected;
    auto& s = state_.squads[index];
    // WLPA-89: the corner at the anchor keeps the walk when its turn speed (WLPA-90) is above zero. A single object (no
    // team container) also needs its heading error within one frame's turn or the move-start angle (WLPA-91). A squad
    // with a non-turning member never turns in place (install_path) and has no corner limit.
    Arithmetic a;
    const auto limits = turn_limits(index);
    if (limits.turns) {
        const auto incoming = a.sub(*anchor, s.position);
        const auto incoming_length = a.length(incoming);
        Vec2 heading_in = forward(s.facing);
        if (a.valid && incoming_length > zero)
            heading_in = {a.div(incoming.x, incoming_length), a.div(incoming.y, incoming_length)};
        for (std::size_t n = 1; n < path.nodes.size(); ++n) {
            const auto outgoing = a.sub(path.nodes[n], *anchor);
            if (outgoing.x == zero && outgoing.y == zero) continue;
            if (!a.valid) return SpliceResult::rejected;
            bool smooth = land_corner_fraction(heading_in, outgoing, limits) > zero;
            if (smooth && !s.has_formation) {
                // Retail compares the first heading with the object's current facing, not its travel direction.
                const auto out_angle = math::atan2_turn(outgoing.y, outgoing.x);
                if (!out_angle) return SpliceResult::rejected;
                const auto error = absolute(math::wrap_turn(a.sub(out_angle.value(), s.facing)));
                const auto frame_turn = a.mul(limits.turn_rate, Fixed::from_ratio(1, logical_frames_per_second).value());
                smooth = a.valid && (error <= frame_turn || error <= limits.close_enough);
            }
            if (!smooth) return SpliceResult::sharp;
            break;
        }
        if (!a.valid) return SpliceResult::rejected;
    }
    LandPath spliced = std::move(path);
    if (s.position != spliced.nodes.front()) {
        spliced.length = a.add(spliced.length, a.length(a.sub(spliced.nodes.front(), s.position)));
        spliced.nodes.insert(spliced.nodes.begin(), s.position);
        if (!a.valid) return SpliceResult::rejected;
    }
    invalidate_contacts(index);
    s.path = std::move(spliced); s.next_node = 1;
    s.turn_start_heading = s.facing; s.awaiting_splice = false;
    s.yielding_to = 0; s.blocker_progress = 0;
    s.straggler_wait_remaining = land_straggler_wait_visits;
    for (const auto member : squad_members_[index]) state_.members[member].straggler_best_lag = Fixed::from_raw(-1);
    return SpliceResult::installed;
}
bool LandMovement::yield(std::size_t index, EntityId blocker, std::uint64_t progress) {
    if (index >= state_.squads.size() || !blocker || blocker == state_.squads[index].object
        || !active(state_.squads[index].order)) return false;
    auto& s = state_.squads[index];
    s.order = LandMoveOrder::yielded; s.yielding_to = blocker; s.blocker_progress = progress;
    return true;
}
bool LandMovement::observe_progress(std::size_t index, EntityId blocker, std::uint64_t progress) {
    if (index >= state_.squads.size()) return false;
    auto& s = state_.squads[index];
    if (s.order != LandMoveOrder::yielded || s.yielding_to != blocker || progress <= s.blocker_progress) return false;
    s.blocker_progress = progress; s.order = LandMoveOrder::recovering;
    return true;
}
bool LandMovement::drop_order(std::size_t index) {
    if (index >= state_.squads.size()) return false;
    state_.squads[index].order = LandMoveOrder::dropped;
    invalidate_contacts(index);
    auto& s = state_.squads[index];
    if (s.generic_team) {
        s.path = {}; s.next_node = 0;
        for (const auto member : squad_members_[index]) state_.members[member].next_node = 0;
    }
    return true;
}
bool LandMovement::set_formation(std::size_t index, EntityId lead, Vec2 slot) {
    if (index >= state_.squads.size()) return false;
    state_.squads[index].formation = lead; state_.squads[index].formation_slot = lead ? slot : Vec2{};
    return true;
}
bool LandMovement::set_gates(EntityId object, LandMotionGates gates) {
    for (auto& m : state_.members) if (m.object == object) { m.gates = gates; return true; }
    return false;
}
bool LandMovement::set_walk_state(EntityId object, LandWalkState state) {
    if (state > LandWalkState::undeploying) return false;
    for (auto& m : state_.members) if (m.object == object) { m.state = state; return true; }
    return false;
}
bool LandMovement::set_animation_progress(EntityId object, Fixed transition, Fixed deployment) {
    if (transition < zero || deployment < zero) return false;
    for (auto& m : state_.members) if (m.object == object) {
        m.transition_frame = transition; m.deployment_frame = deployment; return true;
    }
    return false;
}

core::Result<void> LandMovement::step(LandOccupancy& grid, const PartitionExecutor& executor, Fixed seconds,
    const bool defer_occupancy, const std::span<const std::optional<Fixed>> formation_speeds) {
    const auto overflow = [] { return core::Result<void>::failure(core::Diagnostic{
        std::string(math::diagnostic_codes::overflow), core::Severity::error,
        "Land movement arithmetic is outside fixed-point range.", {}, {}, {}, {}}); };
    if (seconds <= zero || (!formation_speeds.empty() && formation_speeds.size() != state_.squads.size())) return overflow();
    refresh_bodies();
    for (const auto& body : bodies_) if (!body.valid) return overflow();
    const auto bodies = bodies_.size();
    // Contact adjacency (CSR over body indices): the proposal phases and the pair guard visit only the rows that touch
    // a body instead of every retained row. Built here, in the ordered pre-pass, from the committed rows.
    {
        adjacency_offset_.assign(bodies + 1, 0);
        for (const auto& contact : state_.contacts) {
            const auto first = body_index(contact.first);
            if (first < bodies) ++adjacency_offset_[first + 1];
            if (contact.second) {
                const auto second = body_index(contact.second);
                if (second < bodies) ++adjacency_offset_[second + 1];
            }
        }
        for (std::size_t i = 0; i < bodies; ++i) adjacency_offset_[i + 1] += adjacency_offset_[i];
        adjacency_row_.resize(adjacency_offset_[bodies]);
        adjacency_cursor_.assign(adjacency_offset_.begin(), adjacency_offset_.end() - 1);
        for (std::size_t row = 0; row < state_.contacts.size(); ++row) {
            const auto& contact = state_.contacts[row];
            const auto first = body_index(contact.first);
            if (first < bodies) adjacency_row_[adjacency_cursor_[first]++] = static_cast<std::uint32_t>(row);
            if (contact.second) {
                const auto second = body_index(contact.second);
                if (second < bodies) adjacency_row_[adjacency_cursor_[second]++] = static_cast<std::uint32_t>(row);
            }
        }
    }
    // WLPA-59/83: a soldier steers only while its container holds a contact in the right state: repulse for a moving
    // pair, any state for a moving/stopped pair, none for a follower pair, none in passive recovery (a squad whose
    // order is not active). Against a squad the peers are the other squad's soldiers, one by one; against any other
    // body the one body. A generic-team member steers only as a soft body (a hero or a bike).
    for (auto& peers : body_peers_) peers.clear();
    if (straggler_repulsion_) for (std::size_t b = 0; b < bodies; ++b) {
        const auto& body = bodies_[b];
        if (!body.active || ((body.generic || body.single) && !soft_group(body.group))) continue;
        for (auto k = adjacency_offset_[b]; k < adjacency_offset_[b + 1]; ++k) {
            const auto& contact = state_.contacts[adjacency_row_[k]];
            if (contact.second == 0) continue;
            const bool qualifies = contact.kind == LandContactKind::moving_stopped
                ? contact.state != LandContactState::resolved && contact.state != LandContactState::follower_speed
                    && contact.state != LandContactState::same_direction_speed
                : contact.kind == LandContactKind::moving_moving && contact.state == LandContactState::repulsion;
            if (!qualifies) continue;
            const auto other = body_index(contact.first == body.object ? contact.second : contact.first);
            if (other >= bodies) continue;
            const auto& peer = bodies_[other];
            if (peer.container) for (const auto member : squad_members_[peer.squad]) body_peers_[b].push_back(member);
            else body_peers_[b].push_back(peer.member);
        }
    }
    // The two barriers use the same named phase. Squads produce immutable parent inputs first;
    // members then propose into disjoint slots, independently of worker traversal order.
    struct Context { LandOccupancy& grid; Fixed seconds; bool generic_pass{}; std::span<const std::optional<Fixed>> formation_speeds; bool indexed{}; };
    Context context{grid, seconds, false, formation_speeds};
    auto run = land_phase(executor, "land-member-proposals", tick_partition_count, [this, &context](std::size_t partition) {
        const auto range = partition_range(partition, state_.squads.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto& s = state_.squads[i];
            auto& p = squad_proposals_[i];
            squad_visits_[i] = 0;
            p = {true, s.position, s.facing, zero, s.next_node, s.progress};
            p.temporary_next = s.temporary_next;
            p.turning = s.turning;
            p.straggler_wait_remaining = s.straggler_wait_remaining;
            for (const auto member : squad_members_[i]) staged_straggler_lag_[member] = state_.members[member].straggler_best_lag;
            p.has_mapped = s.has_mapped; p.mapped_axis = s.mapped_axis; p.rear_extent = s.rear_extent;
            if (!active(s.order) || state_.tick < s.following_start) continue;
            // LC-83: start admission combines member gates. No parent displacement or
            // type-speed cap is integrated for this dispatch; members own ordinary motion.
            if (s.generic_team) {
                for (const auto member : squad_members_[i]) {
                    const auto& m = state_.members[member];
                    if ((m.state == LandWalkState::stopped || m.state == LandWalkState::starting)
                        && (m.gates.animation_prevents_movement
                            || (m.profile.walk_transition && !start_gate_open(m)))) p.start_admitted = false;
                }
                continue;
            }
            Arithmetic a;
            if (p.turning) {
                // WLPA-68: the squad rotates at its slowest member turn rate with no translation,
                // then returns to moving once the first segment heading is met.
                // install_path only sets the flag for a non-empty squad whose members all turn at a positive rate.
                Fixed rate;
                bool first = true;
                for (const auto member : squad_members_[i]) {
                    const auto r = state_.members[member].profile.turn_rate;
                    if (first || r < rate) rate = r;
                    first = false;
                }
                const auto budget = a.mul(rate, context.seconds);
                std::optional<Fixed> target;
                for (auto n = s.next_node; n < s.path.nodes.size() && !target; ++n) {
                    const auto delta = a.sub(s.path.nodes[n], s.position);
                    if (delta.x == zero && delta.y == zero) continue;
                    const auto heading = math::atan2_turn(delta.y, delta.x);
                    if (!heading) { a.valid = false; break; }
                    target = heading.value();
                }
                if (target) {
                    const auto yaw = math::wrap_turn(a.sub(*target, s.facing));
                    if (absolute(yaw) > budget) {
                        p.facing = math::wrap_turn(a.add(s.facing, std::clamp(yaw, a.sub(zero, budget), budget)));
                        ++p.progress; // a turn in place is progress for the contact deadlock and retry tests (LR3-11)
                        p.valid = a.valid;
                        continue;
                    }
                    p.facing = *target;
                }
                p.turning = false;
                p.valid = a.valid;
                continue;
            }
            // LMF-09: the staged cap is computed from the previous committed formation state.
            auto speed = context.formation_speeds.empty() || !context.formation_speeds[i]
                ? s.maximum_speed : *context.formation_speeds[i];
            // WLPA-38: the contact rows belong to the container body; a soldier's own position holds none.
            if (const auto body = body_of_squad_[i]; body != no_body) {
                const auto id = bodies_[body].object;
                for (auto k = adjacency_offset_[body]; k < adjacency_offset_[body + 1]; ++k) {
                    ++squad_visits_[i];
                    const auto& contact = state_.contacts[adjacency_row_[k]];
                    if (contact.state == LandContactState::resolved) continue;
                    if (contact.state == LandContactState::follower_speed || contact.state == LandContactState::offset_exchange) continue;
                    if ((id == contact.first && contact.first_stopped) || (id == contact.second && contact.second_stopped))
                        speed = zero;
                    if (id == contact.first && contact.first_speed_limit) speed = std::min(speed, *contact.first_speed_limit);
                    if (id == contact.second && contact.second_speed_limit) speed = std::min(speed, *contact.second_speed_limit);
                }
            }
            const auto axis = forward(s.facing);
            auto lag_speed = s.maximum_speed;
            const auto near_hold_speed = a.div(s.maximum_speed, Fixed::from_raw(land_straggler_near_hold_divisor * Fixed::scale));
            bool straggler_progress = false;
            bool member_gate = false;
            // WLPA-30: slow for the largest excess lag beyond rear extent + code 2.
            for (const auto member : squad_members_[i]) {
                const auto& m = state_.members[member];
                const auto lag = a.sub(zero, dot(a, a.sub(m.position, s.position), axis));
                const auto excess = a.sub(lag, a.add(s.rear_extent, Fixed::from_raw(2 * Fixed::scale)));
                const auto factor = a.sub(one, std::clamp(a.div(excess, ten), zero, one));
                const auto member_lag_speed = a.mul(s.maximum_speed, factor);
                if (member_lag_speed < near_hold_speed) {
                    auto& best = staged_straggler_lag_[member];
                    if (best < zero) best = excess;
                    else if (excess < best) { best = excess; straggler_progress = true; }
                } else staged_straggler_lag_[member] = Fixed::from_raw(-1);
                lag_speed = std::min(lag_speed, member_lag_speed);
                // LR13-17/19/21: no parent runs away from a member awaiting an admitted gate.
                if (m.gates.animation_prevents_movement || m.state == LandWalkState::deploying
                    || m.state == LandWalkState::deployed || m.state == LandWalkState::undeploying
                    || (m.profile.walk_transition && !start_gate_open(m)
                        && (m.state == LandWalkState::stopped || m.state == LandWalkState::starting))) member_gate = true;
            }
            // WLPA-93: count only an otherwise movable leader nearly held by lag.
            if (lag_speed < near_hold_speed) {
                if (speed > zero && !member_gate && p.straggler_wait_remaining > 0) {
                    if (straggler_progress) {
                        p.straggler_wait_remaining = land_straggler_wait_visits;
                    } else --p.straggler_wait_remaining;
                }
                if (p.straggler_wait_remaining > 0) speed = zero;
            } else {
                p.straggler_wait_remaining = land_straggler_wait_visits;
                for (const auto member : squad_members_[i]) staged_straggler_lag_[member] = Fixed::from_raw(-1);
                speed = std::min(speed, lag_speed);
            }
            if (member_gate) speed = zero;
            // The speed the deadlock test (LR3-11) reads is the one an object reaches in a single step: a ramp from its last
            // collision-modified speed by the body's acceleration, as a soldier's own speed was. A container held by the pair
            // sweep therefore reads below its acceleration threshold on every visit, and a container moving freely does not.
            // WLPA-90/91: a squad on its own path turns toward each segment at its slowest member's rate while it walks; until
            // its heading is within one frame's turn the speed is capped at the corner's turn speed (a corner with no turn
            // speed stops the walk and turns in place), and the speed never exceeds what brakes to the next corner's turn
            // speed, or to a stop at the path end, at the squad's deceleration. A contact route keeps the plain walk.
            bool steered = false, heading_met = true;
            const auto speed_before_caps = speed;
            if (s.temporary_first == 0 && s.next_node < s.path.nodes.size()) {
                const auto limits = turn_limits(i);
                const std::span<const Vec2> nodes{s.path.nodes};
                auto ahead = s.next_node;
                while (ahead < nodes.size() && nodes[ahead] == s.position) ++ahead;
                if (limits.turns && ahead < nodes.size()) {
                    const auto delta = a.sub(nodes[ahead], s.position);
                    const auto heading = math::atan2_turn(delta.y, delta.x);
                    if (!heading) a.valid = false;
                    else {
                        steered = true;
                        const auto budget = a.mul(limits.turn_rate, context.seconds);
                        const auto error = math::wrap_turn(a.sub(heading.value(), s.facing));
                        const bool met = absolute(error) <= budget;
                        if (!met && ahead > 0) {
                            const auto turn_fraction = path_corner_fraction(nodes, ahead - 1, s.turn_start_heading, limits);
                            if (turn_fraction == zero) {
                                // The corner stops the walk: no speed is assigned, and the straggler wait is not spent.
                                p.turning = true; p.assigned_speed = zero;
                                p.straggler_wait_remaining = s.straggler_wait_remaining;
                                p.valid = a.valid; continue;
                            }
                            // WLPA-90: the position stays on the path, so the speed is also scaled by the cosine of the heading
                            // error (zero at a quarter turn or more): the body never runs along the path against its facing.
                            const auto turn_cosine = std::max(zero, math::sin_cos_turn(error).cosine);
                            speed = std::min(speed, a.mul(a.mul(turn_fraction, limits.maximum_speed), turn_cosine));
                        }
                        const auto onto = a.mul(path_corner_fraction(nodes, ahead, s.turn_start_heading, limits), limits.maximum_speed);
                        if (onto < speed)
                            speed = std::min(speed, brake_cap(onto, limits.deceleration, a.length(delta), context.seconds, limits.maximum_speed));
                        if (!s.awaiting_splice) {
                            const auto horizon = stopping_distance(limits.maximum_speed, limits.deceleration);
                            const auto left = path_length_ahead(nodes, s.position, ahead, horizon);
                            if (left <= horizon)
                                speed = std::min(speed, brake_cap(zero, limits.deceleration, left, context.seconds, limits.maximum_speed));
                        }
                        // Rotate every visit, also at zero speed: a cosine cap of zero would otherwise hold the heading error.
                        p.facing = met ? heading.value()
                            : math::wrap_turn(a.add(s.facing, std::clamp(error, a.sub(zero, budget), budget)));
                        // WLPA-84 maps the rows once the squad's facing has caught up with the path heading, so a slow swing
                        // through a corner is one re-map, not one per eighth of a turn.
                        heading_met = math::sin_cos_turn(math::wrap_turn(a.sub(heading.value(), p.facing))).cosine
                            > Fixed::from_ratio(99, 100).value();
                    }
                }
            }
            // WLPA-93: a corner or cosine cap that zeroes a speed the straggler gate had admitted spends no wait.
            if (speed_before_caps > zero && speed == zero) p.straggler_wait_remaining = s.straggler_wait_remaining;
            // LR3-11: the contact layer reads the speed after the corner and braking caps (retail applies them before the
            // collision modifier): a ramp from the last collision-modified speed by the body's acceleration.
            p.assigned_speed = speed;
            if (const auto body = body_of_squad_[i]; body != no_body && bodies_[body].acceleration > zero)
                p.assigned_speed = std::min(speed, a.add(s.collision_speed, a.mul(bodies_[body].acceleration, context.seconds)));
            auto remaining = a.mul(speed, context.seconds);
            std::uint32_t common_mask = std::numeric_limits<std::uint32_t>::max();
            Fixed radius;
            for (const auto member : squad_members_[i]) {
                common_mask &= state_.members[member].mask;
                radius = std::max(radius, state_.members[member].profile.radius);
            }
            const bool temporary = s.temporary_first != 0;
            const auto& route = temporary ? s.temporary_path : s.path;
            auto& tracker = temporary ? p.temporary_next : p.next_node;
            while (tracker < route.nodes.size()) {
                const auto delta = a.sub(route.nodes[tracker], p.position);
                const auto distance = a.length(delta);
                if (!a.valid) break;
                if (distance == zero) { ++tracker; continue; }
                if (remaining <= zero) break;
                const auto heading = math::atan2_turn(delta.y, delta.x);
                if (!heading) { a.valid = false; break; }
                if (!steered) p.facing = heading.value();
                const auto travel = std::min(distance, remaining);
                const auto next = travel == distance ? route.nodes[tracker]
                    : a.add(p.position, a.mul(delta, a.div(travel, distance)));
                if (!a.valid || !admitted(context.grid, p.position, next, radius, common_mask)) {
                    p.terrain_blocked = true; p.terrain_target = next; break;
                }
                p.position = next;
                remaining = a.sub(remaining, travel);
                if (travel == distance) ++tracker;
            }
            p.valid = a.valid;
            if (p.position != s.position) {
                p.speed = speed;
                // Progress is a real displacement: a rounding-sized step of a degenerate route is none (WLPA-96, #6115).
                Arithmetic moved;
                if (moved.length(moved.sub(p.position, s.position)) >= Fixed::from_raw(Fixed::scale / 256) || !moved.valid) ++p.progress;
            } else if (steered && p.facing != s.facing) {
                ++p.progress; // rotating at zero speed (the cosine cap) is progress, so it is no deadlock (LR3-11)
            }
            // WLPA-84 (retail Map_Team_Members): when the path heading leaves the last mapped axis by more than dot 0.99,
            // or the slots were never mapped, the members are re-assigned to rows behind the container once the facing has
            // caught up with the heading (the remake's facing lags it). The new slots
            // go to staged_slot_ (this squad's members only); nothing shared is written from the partition.
            if (p.position != s.position && s.has_formation && squad_members_[i].size() > 1) {
                bool extents = true;
                for (const auto member : squad_members_[i]) extents &= state_.members[member].hard_y > zero;
                const auto next_axis = forward(p.facing);
                const bool stale = !s.has_mapped
                    || dot(a, next_axis, forward(s.mapped_axis)) <= Fixed::from_ratio(99, 100).value();
                if (extents && stale && heading_met && a.valid) {
                    const auto side_axis = side(next_axis);
                    std::vector<LandRowsInput> inputs;
                    inputs.reserve(squad_members_[i].size());
                    for (const auto member : squad_members_[i]) {
                        const auto& m = state_.members[member];
                        const auto offset = a.sub(m.position, s.position);
                        inputs.push_back({m.object, dot(a, offset, next_axis), dot(a, offset, side_axis), m.hard_x, m.hard_y, m.gap});
                    }
                    const auto slots = a.valid ? land_rows_layout(inputs) : std::nullopt;
                    if (!slots) a.valid = false;
                    else {
                        Fixed rear;
                        for (std::size_t k = 0; k < squad_members_[i].size(); ++k) {
                            staged_slot_[squad_members_[i][k]] = (*slots)[k];
                            rear = std::max(rear, a.sub(zero, (*slots)[k].x));
                        }
                        p.remapped = true; p.has_mapped = true; p.mapped_axis = p.facing; p.rear_extent = rear;
                    }
                }
                p.valid = a.valid;
            }
        }
    });
    if (!run) return run;
    for (const auto& p : squad_proposals_) if (!p.valid) return overflow();
    // Generic-team members propose first (their motion is their own); the body pair sweep then reverts a blocked
    // container or generic member; formation soldiers follow the container's final position (second pass).
    const auto member_pass = [this, &context](std::size_t partition) {
        const auto range = partition_range(partition, state_.members.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto& m = state_.members[i];
            const auto& s = state_.squads[m.squad];
            if (s.generic_team != context.generic_pass) continue;
            const auto& parent = squad_proposals_[m.squad];
            auto& p = proposals_[i];
            p = {true, false, m.position, m.facing, m.speed, m.stop_velocity, m.state};
            p.transition_frame = m.transition_frame;
            // WLPA-36/69: only a generic-team member has a terrain record of its own.
            if (s.generic_team) { p.terrain_blocked = parent.terrain_blocked; p.terrain_target = parent.terrain_target; }
            member_visits_[i] = 0;
            p.next_node = m.next_node; p.temporary_next = m.temporary_next;
            Arithmetic a;
            const auto axis = forward(parent.facing);
            Vec2 target;
            if (s.generic_team) {
                const auto& route = s.temporary_first ? s.temporary_path : s.path;
                auto& tracker = s.temporary_first ? p.temporary_next : p.next_node;
                while (tracker < route.nodes.size() && route.nodes[tracker] == m.position) ++tracker;
                target = tracker < route.nodes.size() ? route.nodes[tracker] : m.position;
            } else {
                const auto slot = parent.remapped ? staged_slot_[i] : m.slot;
                target = a.add(parent.position, a.add(a.mul(axis, slot.x), a.mul(side(axis), slot.y)));
            }
            const auto error = a.sub(target, m.position);
            const auto distance = a.length(error);
            auto direction = axis;
            // LR3-15 lateral correction, capped at 0.5. At parent stop, finish passive slot recovery.
            if ((s.generic_team || parent.speed == zero) && distance > zero) direction = a.mul(error, a.div(one, distance));
            else {
                const auto lateral = dot(a, error, side(axis));
                const auto blend = std::clamp(a.div(lateral, ten), Fixed::from_raw(-Fixed::scale / 2), Fixed::from_raw(Fixed::scale / 2));
                direction = a.add(axis, a.mul(side(axis), blend));
            }
            // WLPA-58/59: member-local avoidance remains available while the parent waits for lagging slots, but only
            // while the body holds a qualifying contact (the peers were gathered from the container's rows).
            const std::vector<std::size_t>* peers = nullptr;
            {
                const auto body = s.generic_team ? body_of_member_[i] : body_of_squad_[m.squad];
                if (straggler_repulsion_ && body != no_body && distance > zero) peers = &body_peers_[body];
            }
            static const std::vector<std::size_t> no_peers;
            const auto& repulsion_peers = peers ? *peers : no_peers;
            if (!repulsion_peers.empty()) {
                const auto length = a.length(direction);
                if (length > zero) direction = a.mul(direction, a.div(one, length));
                const auto requested = parent.speed == zero ? std::min(m.profile.maximum_speed, a.div(distance, context.seconds))
                    : land_member_desired_speed(parent.speed, m.profile.maximum_speed, dot(a, error, axis), false);
                const auto speed = approach(a, m.speed, requested, m.profile, context.seconds);
                const auto avoidance = [&](std::size_t peer) -> Vec2 {
                    if (m.profile.maximum_speed <= zero) return {};
                    const auto& other = state_.members[peer];
                    if (other.squad == m.squad) return {};
                    const auto reach = a.mul(a.add(m.profile.radius, other.profile.radius), Fixed::from_ratio(13, 10).value());
                    const auto offset = a.sub(other.position, m.position);
                    if (reach <= zero || a.length(offset) >= reach) return {};
                    const auto lateral = a.mul(side(direction), reach);
                    const auto valid_side = [&](bool positive) {
                        const auto endpoint = positive ? a.add(other.position, lateral) : a.sub(other.position, lateral);
                        const auto forward_reach = a.mul(direction, reach);
                        return admitted(context.grid, other.position, endpoint, m.profile.radius, m.mask)
                            && admitted(context.grid, a.sub(endpoint, forward_reach), a.add(endpoint, forward_reach), m.profile.radius, m.mask);
                    };
                    const bool positive = dot(a, offset, side(direction)) < zero;
                    auto chosen = positive ? lateral : a.mul(lateral, Fixed::from_raw(-Fixed::scale));
                    if (!valid_side(positive)) {
                        if (!valid_side(!positive)) return {};
                        chosen = a.mul(chosen, Fixed::from_raw(-Fixed::scale));
                    }
                    return a.mul(chosen, std::clamp(a.div(speed, m.profile.maximum_speed), zero, one));
                };
                Vec2 best{};
                Fixed best_length;
                std::size_t best_index = repulsion_peers.size();
                for (std::size_t c = 0; c < repulsion_peers.size(); ++c) {
                    const auto candidate = avoidance(repulsion_peers[c]);
                    const auto candidate_length = a.length(candidate);
                    if (candidate_length > best_length) { best = candidate; best_length = candidate_length; best_index = c; }
                }
                auto correction = best;
                for (std::size_t c = 0; c < repulsion_peers.size(); ++c) if (c != best_index) {
                    const auto candidate = avoidance(repulsion_peers[c]);
                    if (dot(a, candidate, best) >= Fixed::from_raw(-Fixed::scale / 2)) correction = a.add(correction, candidate);
                }
                direction = a.add(direction, correction);
            }
            auto heading = s.generic_team ? m.facing : parent.facing;
            if (distance > zero || parent.speed > zero) {
                const auto angle = math::atan2_turn(direction.y, direction.x);
                if (angle) heading = angle.value(); else a.valid = false;
            }
            const auto yaw = math::wrap_turn(a.sub(heading, m.facing));
            const auto turn = a.mul(m.profile.turn_rate, context.seconds);
            // WLPA-79: members mirror the squad turning state; the avoidance-swung heading steers
            // facing through the rate clamp but never zeroes speed.
            const bool turning = !s.generic_team && parent.turning;
            p.facing = math::wrap_turn(a.add(m.facing, std::clamp(yaw, a.sub(zero, turn), turn)));
            p.motion_input.heading_error = yaw;
            p.motion_input.valid = true;
            bool allowed = follows(s) && state_.tick >= s.following_start;
            if (s.generic_team) {
                if (allowed && distance > zero && p.state == LandWalkState::stopped && m.profile.walk_transition)
                    p.state = LandWalkState::starting;
                allowed &= parent.start_admitted;
            }
            if (p.state == LandWalkState::deploying || p.state == LandWalkState::undeploying) {
                if (m.gates.turret_aligned && m.gates.deployment_almost_finished)
                    p.state = p.state == LandWalkState::deploying ? LandWalkState::deployed : LandWalkState::stopped;
                allowed = false;
            }
            if (p.state == LandWalkState::deployed || m.gates.animation_prevents_movement) allowed = false;
            if (allowed && (distance > zero || s.next_node < s.path.nodes.size()) && p.state == LandWalkState::stopped)
                p.state = m.profile.walk_transition ? LandWalkState::starting : LandWalkState::moving;
            if (p.state == LandWalkState::starting) {
                if (start_gate_open(m)) p.state = LandWalkState::moving;
                else {
                    allowed = false;
                    // LR13-17: the MOVESTART clip plays from its first frame while the member waits.
                    p.transition_frame = a.add(m.transition_frame, one);
                }
            }
            if (p.state != LandWalkState::starting) p.transition_frame = zero;
            if (p.state == LandWalkState::turning && !m.gates.turn_almost_finished) allowed = false;
            Fixed desired;
            if (allowed && distance > zero) {
                desired = (s.generic_team || parent.speed == zero) ? std::min(m.profile.maximum_speed, a.div(distance, context.seconds))
                    : land_member_desired_speed(parent.speed, m.profile.maximum_speed, dot(a, error, axis), turning);
                if (turning) desired = zero;
            }
            if (s.generic_team) {
                // WLPA-38: a generic-team member is its own body and reads its own rows. A formation soldier reads none:
                // the container's rows already shaped the parent speed it follows.
                const auto body = body_of_member_[i];
                const auto id = m.object;
                if (body != no_body) for (auto k = adjacency_offset_[body]; k < adjacency_offset_[body + 1]; ++k) {
                    ++member_visits_[i];
                    const auto& contact = state_.contacts[adjacency_row_[k]];
                    if (contact.state == LandContactState::resolved) continue;
                    if ((id == contact.first && contact.first_stopped) || (id == contact.second && contact.second_stopped)) desired = zero;
                    if (id == contact.first && contact.first_speed_limit) desired = std::min(desired, *contact.first_speed_limit);
                    if (id == contact.second && contact.second_speed_limit) desired = std::min(desired, *contact.second_speed_limit);
                }
            }
            if ((!allowed || desired == zero) && (p.state == LandWalkState::moving || p.state == LandWalkState::turning)) {
                p.state = LandWalkState::stopping; p.stop_velocity = m.speed;
            }
            p.speed = approach(a, m.speed, desired, m.profile, context.seconds);
            p.assigned_speed = desired;
            if (turning && allowed) p.state = LandWalkState::turning;
            else if (desired > zero && p.state != LandWalkState::starting) p.state = LandWalkState::moving;
            if (p.state == LandWalkState::stopping && p.speed == zero
                && (!m.profile.walk_transition || (m.gates.stop_point && m.gates.stop_almost_finished))) p.state = LandWalkState::stopped;
            // Walk start/turn/deploy gates suppress displacement separately from retained deceleration.
            if (allowed && !turning && p.state != LandWalkState::starting && distance > zero) {
                const auto travel = std::min(distance, a.mul(p.speed, context.seconds));
                const auto next = travel == distance ? target : a.add(m.position, a.mul(forward(p.facing), travel));
                // WLPA-81: a formation soldier's displacement is written straight through ground stabilization, with no
                // terrain veto (retail clips corners). A generic-team member and a single object (hero, bike, lone vehicle)
                // keep their own occupancy test (WLPA-36/69); a single's refusal reaches its container at the commit.
                if (!s.generic_team && s.has_formation) { if (a.valid) p.position = next; }
                else if (a.valid && admitted(context.grid, m.position, next, m.profile.radius, m.mask)) p.position = next;
                else { p.speed = zero; p.state = LandWalkState::stopping; p.terrain_blocked = true; p.terrain_target = next; }
            }
            // LR13-24: the ordinary moving path consumes a turn budget.
            p.motion_input.turn_fraction_valid = s.generic_team;
            if (s.generic_team && allowed && !turning && distance > zero && p.speed > zero && turn > zero)
                p.motion_input.turn_fraction = a.div(std::min(absolute(yaw), turn), turn);
            p.valid = a.valid;
            p.at_slot = p.position == target;
            if (s.generic_team && p.at_slot) {
                const auto& route = s.temporary_first ? s.temporary_path : s.path;
                auto& tracker = s.temporary_first ? p.temporary_next : p.next_node;
                if (tracker < route.nodes.size()) ++tracker;
                p.at_slot = tracker == route.nodes.size();
            }
        }
    };
    context.generic_pass = true;
    run = land_phase(executor, "land-member-proposals", tick_partition_count, member_pass);
    if (!run) return run;
    // The body positions the pair sweep reads: the container's proposed position, or a generic member's own.
    const auto proposed_position = [this](const Body& body) {
        return body.container ? squad_proposals_[body.squad].position : proposals_[body.member].position;
    };
    if (!reference_pair_scans_) {
        core::tick_profile::Scope phase("land-movement-index");
        run = land_phase(executor, "land-member-proposals", tick_partition_count, [this, &proposed_position](std::size_t partition) {
            const auto range = partition_range(partition, bodies_.size());
            for (auto b = range.begin; b < range.end; ++b) {
                const auto& body = bodies_[b];
                body_proposed_[b] = proposed_position(body);
                neighbours_.bodies[b] = {body.position, body_proposed_[b], body.radius};
            }
        });
        if (!run) return run;
        core::tick_profile::Scope build("land-movement-index-build");
        neighbours_.build(grid.terrain().spacing());
    } else {
        for (std::size_t b = 0; b < bodies_.size(); ++b) body_proposed_[b] = proposed_position(bodies_[b]);
    }
    const bool indexed = !reference_pair_scans_ && neighbours_.safe();
    context.indexed = indexed;
    // Swept pair checks read immutable proposals and write disjoint flags. The subsequent ordered
    // reduction stops blocked bodies without cross-body writes from worker threads.
    run = land_phase(executor, "land-member-proposals", tick_partition_count, [this, &context](std::size_t partition) {
        const auto range = partition_range(partition, bodies_.size());
        for (auto b = range.begin; b < range.end; ++b) {
            const auto& body = bodies_[b];
            pair_blocked_[b] = 0;
            pair_visits_[b] = 0;
            if (body.radius <= zero) continue;
            Arithmetic a;
            const auto proposed = body_proposed_[b];
            auto& candidates = neighbour_candidates_[b];
            if (context.indexed) neighbours_.query(b, candidates);
            const auto count = context.indexed ? candidates.size() : bodies_.size();
            pair_visits_[b] = static_cast<std::uint32_t>(count);
            for (std::size_t candidate = 0; candidate < count; ++candidate) {
                const auto j = context.indexed ? candidates[candidate] : candidate;
                const auto& other = bodies_[j];
                if (body.squad == other.squad || other.radius <= zero) continue;
                const auto radius = a.add(body.radius, other.radius);
                const auto delta = a.sub(a.sub(proposed, body.position), a.sub(body_proposed_[j], other.position));
                const auto offset = a.sub(body.position, other.position);
                const auto length_squared = dot(a, delta, delta);
                const auto t = length_squared > zero
                    ? std::clamp(a.div(a.sub(zero, dot(a, offset, delta)), length_squared), zero, one) : zero;
                const auto closest = a.length(a.add(offset, a.mul(delta, t)));
                if (closest < radius && proposed != body.position) {
                    // WLPA-83: two moving Infantry/Bike containers of different squads with opposed or crossing headings
                    // never veto each other; the contact row (repulse) and the soldier steering (WLPA-59) separate them.
                    // Same-direction pairs keep the hard veto.
                    if (soft_opposed(body, other, straggler_repulsion_)) continue;
                    // Same direction: the container ahead keeps moving; only the one behind is held by the pair.
                    if (leads(body, other, straggler_repulsion_)) continue;
                    // A formation speed cap can bring hard vehicles into the swept guard together. Let the leading
                    // member move away from the other's committed position; the trailing member still takes the veto.
                    // The positive dot product makes separation increase throughout this linear step.
                    if (!context.formation_speeds.empty() && body.container && other.container && body.active && other.active
                        && context.formation_speeds[body.squad] && context.formation_speeds[other.squad]
                        && state_.squads[body.squad].formation != 0
                        && state_.squads[body.squad].formation == state_.squads[other.squad].formation
                        && same_heading(body.facing, other.facing) && ahead_of(body, other)
                        && dot(a, offset, a.sub(proposed, body.position)) > zero) continue;
                    // WLPA-57/82: a moving soft body against an idle soft body is no hard pair either; the moving/stopped
                    // contact decides (repulsion, or truncate / path-around / push when the move ends on the idle body).
                    if (straggler_repulsion_ && body.active && !other.active
                        && state_.squads[other.squad].order != LandMoveOrder::yielded
                        && soft_group(body.group) && soft_group(other.group)) continue;
                    // Existing overlap may separate, but never deepen.
                    if (a.length(offset) < radius
                        && a.length(a.sub(proposed, other.position)) > a.length(offset)) continue;
                    pair_blocked_[b] = 1; break;
                }
            }
            if (!a.valid) pair_blocked_[b] = 2;
        }
    });
    if (!run) return run;
    for (const auto flag : pair_blocked_) if (flag == 2) return overflow();
    for (std::size_t b = 0; b < bodies_.size(); ++b) if (pair_blocked_[b]) {
        const auto& body = bodies_[b];
        if (body.container) {
            // The container stops; its soldiers follow the reverted position in the second pass.
            auto& parent = squad_proposals_[body.squad]; const auto& old = state_.squads[body.squad];
            parent.position = old.position; parent.speed = zero; parent.progress = old.progress;
            parent.next_node = old.next_node; parent.temporary_next = old.temporary_next;
            continue;
        }
        auto& p = proposals_[body.member]; const auto& m = state_.members[body.member];
        p.position = m.position; p.speed = zero; p.state = LandWalkState::stopping;
        p.motion_input.turn_fraction = zero; // the member does not move: no turn budget was consumed
        p.next_node = m.next_node; p.temporary_next = m.temporary_next; p.at_slot = false;
    }
    context.generic_pass = false;
    run = land_phase(executor, "land-member-proposals", tick_partition_count, member_pass);
    if (!run) return run;
    for (const auto& p : proposals_) if (!p.valid) return overflow();
    // Build a complete snapshot while preserving scenery. Retain cell-vector capacity at high water.
    std::size_t count = 0;
    const auto append = [&]() -> LandOccupation& {
        if (count == occupations_.size()) occupations_.emplace_back();
        auto& o = occupations_[count++];
        o.primary_cells.clear(); o.secondary_cells.clear();
        o.primary_passability = 0; o.secondary_passability = 0;
        return o;
    };
    for (const auto& old : grid.occupations()) {
        const auto own = std::lower_bound(member_order_.begin(), member_order_.end(), old.object,
            [&](auto index, auto id) { return state_.members[index].object < id; });
        if (own != member_order_.end() && state_.members[*own].object == old.object) continue;
        auto& o = append(); o = old;
    }
    for (const auto i : member_order_) {
        auto& o = append(); o.object = state_.members[i].object;
        const auto& p = proposals_[i];
        const auto& terrain = grid.terrain();
        const auto x = land_cell_of(p.position.x, terrain.spacing(), terrain.width());
        const auto y = land_cell_of(p.position.y, terrain.spacing(), terrain.height());
        if (x && y) o.primary_cells.push_back(*y * terrain.width() + *x);
    }
    occupations_.resize(count);
    // L1 internally orders the snapshot by object ID and transactionally commits its double buffer.
    if (!defer_occupancy) {
        run = commit_occupancy(grid, executor);
        if (!run) return run;
    }
    for (const auto i : member_order_) {
        auto& m = state_.members[i]; const auto& p = proposals_[i];
        member_motion_inputs_[i] = p.motion_input;
        if (m.position != p.position) ++m.progress;
        m.position = p.position; m.facing = p.facing; m.speed = p.speed;
        m.stop_velocity = p.stop_velocity; m.state = p.state;
        if (m.profile.start_clock) m.transition_frame = p.transition_frame;
        m.assigned_speed = p.assigned_speed; m.collision_speed = p.speed; m.terrain_blocked = p.terrain_blocked;
        m.terrain_target = p.terrain_target;
        m.next_node = p.next_node; m.temporary_next = p.temporary_next;
        m.straggler_best_lag = staged_straggler_lag_[i];
    }
    for (std::size_t i = 0; i < state_.squads.size(); ++i) {
        auto& s = state_.squads[i]; const auto& p = squad_proposals_[i];
        const auto old_facing = s.facing;
        const bool was_turning = s.turning;
        s.position = p.position; s.facing = p.facing; s.speed = p.speed;
        s.next_node = p.next_node; s.progress = p.progress;
        s.temporary_next = p.temporary_next; s.turning = p.turning;
        s.straggler_wait_remaining = p.straggler_wait_remaining;
        if (!s.generic_team) {
            s.terrain_blocked = p.terrain_blocked; s.terrain_target = p.terrain_target;
            s.assigned_speed = p.assigned_speed; s.collision_speed = p.speed;
            // A single object's own refused step is the container's terrain refusal: the container body is the one the
            // terrain contact re-routes. Until the new route is installed the WLPA-30 lag gate holds the container back.
            if (!s.has_formation && !squad_members_[i].empty()) {
                const auto& single = state_.members[squad_members_[i].front()];
                if (single.terrain_blocked && !s.terrain_blocked) { s.terrain_blocked = true; s.terrain_target = single.terrain_target; }
            }
        }
        if (was_turning && !s.turning) {
            // Logging only: the squad reached its first-segment heading and walks.
            LandMoveTrace event;
            event.kind = LandMoveTrace::Kind::turn_exit; event.tick = state_.tick; event.squad = s.object;
            event.has_old_heading = true; event.old_heading = old_facing; event.new_heading = s.facing;
            emit_trace(event);
        }
        if (p.remapped) {
            LandMoveTrace event; // logging only; read before the axis is replaced
            event.kind = LandMoveTrace::Kind::remap; event.tick = state_.tick; event.squad = s.object;
            event.has_old_heading = s.has_mapped; event.old_heading = s.mapped_axis; event.new_heading = p.mapped_axis;
            emit_trace(event);
            // WLPA-84: slots, mapped axis and rear extent become authoritative together at the end of the re-map step;
            // the WLPA-30 gate reads the new rear extent from the next step.
            for (const auto member : squad_members_[i]) state_.members[member].slot = staged_slot_[member];
            s.has_mapped = true; s.mapped_axis = p.mapped_axis; s.rear_extent = p.rear_extent;
        }
        if (s.generic_team) {
            // LC-83 leader seam: ascending object ID, independent of storage/worker order.
            const auto& leader = state_.members[squad_members_[i].front()];
            s.position = leader.position; s.facing = leader.facing; s.speed = leader.speed;
            s.next_node = leader.next_node; s.temporary_next = leader.temporary_next;
            s.progress = leader.progress;
            // Temporary route ownership lasts until every propagated member tracker ends.
            for (const auto member : squad_members_[i])
                s.temporary_next = std::min(s.temporary_next, state_.members[member].temporary_next);
        }
        // A container pushed to an open spot (WLPA-57) walks its temporary route first; its base path is empty.
        if (active(s.order) && s.next_node == s.path.nodes.size() && !s.generic_team && s.temporary_first == 0) {
            s.order = LandMoveOrder::reached; // WLPA-85: the container's move completes on arrival
        } else if (active(s.order) && s.next_node == s.path.nodes.size()) {
            bool settled = true;
            for (const auto member : squad_members_[i]) {
                const auto& m = state_.members[member];
                settled &= proposals_[member].at_slot && m.speed == zero && m.state == LandWalkState::stopped;
            }
            if (settled) {
                s.order = LandMoveOrder::reached;
                if (s.generic_team) {
                    // LC-83: release the wrapper path once all propagated trackers end.
                    s.path = {}; s.next_node = 0;
                    for (const auto member : squad_members_[i]) state_.members[member].next_node = 0;
                }
            }
        }
    }
    work_ = {};
    work_.contact_rows = state_.contacts.size();
    work_.bodies = bodies_.size();
    for (const auto v : member_visits_) work_.contact_visits += v;
    for (const auto v : squad_visits_) work_.contact_visits += v;
    for (const auto v : pair_visits_) work_.pair_candidates += v;
    ++state_.tick;
    return core::Result<void>::success();
}

core::Result<void> LandMovement::commit_occupancy(LandOccupancy& grid, const PartitionExecutor& executor) {
    return grid.commit(occupations_, executor);
}
} // namespace eawr::sim::tactical
