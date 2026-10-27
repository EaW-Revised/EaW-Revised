#include "eawr/sim/tactical/land_movement.hpp"
#include "eawr/sim/math/trig.hpp"
#include "eawr/core/tick_profile.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <tuple>

namespace eawr::sim::tactical {
namespace {
using math::Fixed;
using math::Vec2;
constexpr Fixed zero{};
constexpr auto one = Fixed::from_raw(Fixed::scale);
struct Arithmetic {
    bool valid{true};
    Fixed add(Fixed a, Fixed b) { Fixed r; valid &= math::try_add(a, b, r); return r; }
    Fixed sub(Fixed a, Fixed b) { Fixed r; valid &= math::try_subtract(a, b, r); return r; }
    Fixed mul(Fixed a, Fixed b) { Fixed r; valid &= math::try_multiply(a, b, r); return r; }
    Fixed div(Fixed a, Fixed b) { Fixed r; valid &= math::try_divide(a, b, r); return r; }
    Vec2 add(Vec2 a, Vec2 b) { return {add(a.x, b.x), add(a.y, b.y)}; }
    Vec2 sub(Vec2 a, Vec2 b) { return {sub(a.x, b.x), sub(a.y, b.y)}; }
    Vec2 mul(Vec2 a, Fixed b) { return {mul(a.x, b), mul(a.y, b)}; }
    Fixed dot(Vec2 a, Vec2 b) { return add(mul(a.x, b.x), mul(a.y, b.y)); }
    Fixed length(Vec2 a) { Fixed r; valid &= math::try_length(a, r); return r; }
};
// WLPA-70: terrain path-around reach is the object's hard radius plus this advance distance.
constexpr auto detour_reach = Fixed::from_raw(40 * Fixed::scale);
auto key(const LandContact& c) { return std::pair{c.first, c.second}; }
bool moving(const LandMovingSquad& s) {
    return s.order == LandMoveOrder::following || s.order == LandMoveOrder::recovering || s.order == LandMoveOrder::yielded;
}
Vec2 prediction(Arithmetic& a, const LandMovingMember& m, const LandMovingSquad& s,
    const LandContactSettings& settings, bool slot_recovery = false) {
    if (!moving(s)) return m.position;
    if (!s.generic_team && m.detour_next < m.detour_path.nodes.size()) {
        // WLPA-69: a member on its own terrain detour is predicted toward the detour node.
        const auto delta = a.sub(m.detour_path.nodes[m.detour_next], m.position);
        const auto length = a.length(delta);
        if (length == zero) return m.position;
        const auto travel = std::min(length, std::max(settings.minimum_prediction_distance,
            a.mul(m.profile.maximum_speed, settings.prediction_seconds)));
        return a.add(m.position, a.mul(delta, a.div(travel, length)));
    }
    const auto& path = s.temporary_first ? s.temporary_path : s.path;
    const auto next = s.generic_team ? (s.temporary_first ? m.temporary_next : m.next_node)
        : (s.temporary_first ? s.temporary_next : s.next_node);
    Vec2 delta;
    if (!s.generic_team && slot_recovery && s.speed == zero && s.order != LandMoveOrder::yielded) {
        const auto axis = math::sin_cos_turn(s.facing);
        const auto slot = a.add(a.mul({axis.cosine, axis.sine}, m.slot.x),
            a.mul({a.sub(zero, axis.sine), axis.cosine}, m.slot.y));
        delta = a.sub(a.add(s.position, slot), m.position);
    } else {
        if (next >= path.nodes.size()) return m.position;
        delta = a.sub(path.nodes[next], s.generic_team ? m.position : s.position);
    }
    const auto length = a.length(delta);
    if (length == zero) return m.position;
    const auto travel = std::min(length, std::max(settings.minimum_prediction_distance,
        a.mul(m.profile.maximum_speed, settings.prediction_seconds)));
    return a.add(m.position, a.mul(delta, a.div(travel, length)));
}
bool collides(Arithmetic& a, const LandMovingMember& x, const LandMovingSquad& xs,
    const LandMovingMember& y, const LandMovingSquad& ys, const LandContactSettings& settings, bool repulsion) {
    const auto radius = a.add(x.profile.radius, y.profile.radius);
    if (radius <= zero) return false;
    const auto relative = a.sub(x.position, y.position);
    // WLPA-60: this bounded pair must observe the actual passive slot approach,
    // otherwise it loses its contact while a clamped parent's straggler is blocked.
    const bool slot_recovery = repulsion && x.squad != y.squad && moving(xs) != moving(ys)
        && land_soft_collision_class(x.profile.collision_class) && land_soft_collision_class(y.profile.collision_class);
    const auto delta = a.sub(a.sub(prediction(a, x, xs, settings, slot_recovery), x.position),
        a.sub(prediction(a, y, ys, settings, slot_recovery), y.position));
    const auto length = a.dot(delta, delta);
    const auto t = length > zero ? std::clamp(a.div(a.sub(zero, a.dot(relative, delta)), length), zero, one) : zero;
    return a.length(a.add(relative, a.mul(delta, t))) < radius;
}
}

// WLPA-83: two moving Infantry/Bike members of different squads, opposed or crossing, are a stateless soft pair:
// no contact row, search, lock or deadlock state exists for them. Same-direction pairs keep the speed-cap row.
// The contact layer and the movement pair guard share this one predicate.
bool land_soft_opposed(const LandMovingMember& x, const LandMovingSquad& xs,
    const LandMovingMember& y, const LandMovingSquad& ys, bool enabled) noexcept {
    const auto moves = [](const LandMovingSquad& s) {
        return s.order == LandMoveOrder::following || s.order == LandMoveOrder::recovering;
    };
    if (!enabled || x.squad == y.squad || xs.generic_team || ys.generic_team || !moves(xs) || !moves(ys)
        || !land_soft_collision_class(x.profile.collision_class) || !land_soft_collision_class(y.profile.collision_class)) return false;
    Arithmetic a;
    const auto xa = math::sin_cos_turn(xs.facing), ya = math::sin_cos_turn(ys.facing);
    return a.dot({xa.cosine, xa.sine}, {ya.cosine, ya.sine}) <= zero;
}

bool land_deadlock_visit(LandContact& c, Fixed x, Fixed y, Fixed cx, Fixed cy,
    Fixed ax, Fixed ay, bool pair_valid, bool progressed) noexcept {
    if (c.deadlock_remaining == 0) return true;
    c.recorded_member = 0;
    const auto fallback = Fixed::from_raw(Fixed::scale / 20); // LR3-11 code 0.05
    if (!pair_valid || progressed) return false;
    const auto tx = ax > zero ? ax : fallback;
    const auto ty = ay > zero ? ay : fallback;
    if (x < tx && y < ty && cx < tx && cy < ty) {
        c.recorded_member = c.first;
        --c.deadlock_remaining;
    }
    return c.deadlock_remaining == 0;
}

bool LandMovement::configure_contacts(LandContactSettings settings) {
    if (settings.prediction_seconds < zero || settings.minimum_prediction_distance < zero) return false;
    state_.contact_settings = settings;
    return true;
}

core::Result<void> LandMovement::service_contacts(const LandOccupancy& grid,
    const PartitionExecutor& executor, std::uint64_t frame) {
    const auto overflow = [] { return core::Result<void>::failure(core::Diagnostic{
        std::string(math::diagnostic_codes::overflow), core::Severity::error,
        "Land contact prediction is outside fixed-point range.", {}, {}, {}, {}}); };
    // Calling the same late barrier twice cannot reset a quota or service a deadlock twice.
    if (state_.contacts_serviced && frame <= state_.last_contact_frame) return core::Result<void>::success();
    const auto& settings = state_.contact_settings;
    if (settings.reset_interval < 2 || frame % settings.reset_interval == 0) state_.contact_complexity = 0;
    const auto member_index = [&](EntityId id) -> std::size_t {
        const auto at = std::lower_bound(member_order_.begin(), member_order_.end(), id,
            [&](auto i, auto value) { return state_.members[i].object < value; });
        return at != member_order_.end() && state_.members[*at].object == id ? *at : state_.members.size();
    };
    const auto release = [&](LandContact& c) {
        if (c.kind == LandContactKind::terrain && c.installed) {
            // WLPA-69: the terrain record resolves when the member's own detour path is gone.
            const auto at = member_index(c.first);
            if (at < state_.members.size() && !state_.members[at].detour_path.nodes.empty()) {
                auto& m = state_.members[at];
                m.detour_path.nodes.clear(); m.detour_next = 0;
                if (m.collision_lock == m.object) m.collision_lock = 0;
            }
        }
        for (std::size_t index = 0; index < state_.squads.size(); ++index) {
            auto& s = state_.squads[index];
            if (s.temporary_first != c.first || s.temporary_second != c.second) continue;
            // A temporary full-destination route rejoins the original order only on completion.
            if (s.temporary_next == s.temporary_path.nodes.size() && !s.path.nodes.empty()
                && s.temporary_path.nodes.back() == s.path.nodes.back()) s.next_node = s.path.nodes.size();
            if (s.generic_team) for (const auto member : squad_members_[index]) {
                auto& m = state_.members[member];
                if (m.temporary_next == s.temporary_path.nodes.size() && !s.path.nodes.empty()
                    && s.temporary_path.nodes.back() == s.path.nodes.back()) m.next_node = s.path.nodes.size();
                m.temporary_next = 0;
            }
            s.temporary_first = 0; s.temporary_second = 0; s.temporary_next = 0;
            s.temporary_path.nodes.clear();
            for (const auto member : squad_members_[index]) state_.members[member].collision_lock = 0;
        }
        c.first_stopped = false; c.second_stopped = false;
        c.first_speed_limit.reset(); c.second_speed_limit.reset();
    };
    // Resolved entries from the preceding invocation are removed before service; newly resolved
    // entries below are cleaned before discovery, preserving LR1-09's late discovery boundary.
    for (auto& c : state_.contacts) if (c.state == LandContactState::resolved) release(c);
    std::erase_if(state_.contacts, [](const auto& c) { return c.state == LandContactState::resolved; });

    // Copy pair observations on workers before the ordered quota/installation decisions. A
    // contact never reads a partially installed neighbor route to decide its initial role.
    contact_observations_.resize(state_.contacts.size());
    struct PairContext { const LandContactSettings& settings; } pairs{settings};
    auto observed = executor.execute_phase("land-member-proposals", tick_partition_count, [this, &pairs](std::size_t partition) {
        const auto lookup = [this](EntityId id) -> std::size_t {
            const auto at = std::lower_bound(member_order_.begin(), member_order_.end(), id,
                [this](auto i, auto value) { return state_.members[i].object < value; });
            return at != member_order_.end() && state_.members[*at].object == id ? *at : state_.members.size();
        };
        const auto range = partition_range(partition, state_.contacts.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto& contact = state_.contacts[i]; auto& proposal = contact_observations_[i];
            proposal = {}; proposal.kind = contact.kind;
            const auto x = lookup(contact.first), y = lookup(contact.second);
            if (x == state_.members.size() || y == state_.members.size()) continue;
            const auto& a = state_.members[x]; const auto& b = state_.members[y];
            const auto& as = state_.squads[a.squad]; const auto& bs = state_.squads[b.squad];
            proposal.kind = moving(as) && moving(bs) ? LandContactKind::moving_moving
                : moving(as) || moving(bs) ? LandContactKind::moving_stopped : LandContactKind::stopped_stopped;
            Arithmetic arithmetic;
            proposal.predicts = collides(arithmetic, a, as, b, bs, pairs.settings, straggler_repulsion_);
            proposal.score = arithmetic.length(arithmetic.sub(a.position, b.position));
            const auto aa = math::sin_cos_turn(as.facing), ba = math::sin_cos_turn(bs.facing);
            const Vec2 axis{aa.cosine, aa.sine};
            proposal.same_direction = arithmetic.dot(axis, {ba.cosine, ba.sine}) > zero;
            proposal.first_behind = arithmetic.dot(arithmetic.sub(b.position, a.position), axis) >= zero;
            proposal.maneuver = a.squad != b.squad && !proposal.same_direction;
            auto primary = x;
            if (proposal.kind == LandContactKind::moving_stopped) primary = moving(as) ? y : x;
            else if (a.collision_class != b.collision_class) primary = a.collision_class > b.collision_class ? x : y;
            else if (as.has_formation && bs.has_formation) {
                if (as.formation_length != bs.formation_length) primary = as.formation_length > bs.formation_length ? x : y;
                else {
                    // Both actors were found in their squad rosters, so these first members exist.
                    // Preserve roster order independently of ascending discovery order; the explicit
                    // validity guard retains the first actor if a future roster adapter omits one.
                    const auto first_id = [this](std::size_t squad) {
                        return squad_members_[squad].empty() ? EntityId{}
                            : state_.members[squad_members_[squad].front()].object;
                    };
                    if (first_id(a.squad) && first_id(b.squad)) primary = first_id(a.squad) >= first_id(b.squad) ? x : y;
                }
            } else primary = a.profile.maximum_speed <= b.profile.maximum_speed ? x : y;
            proposal.primary = state_.members[primary].object;
            proposal.valid = arithmetic.valid;
        }
    });
    if (!observed) return observed;
    for (const auto& proposal : contact_observations_) if (!proposal.valid) return overflow();

    // LR3-13: graph construction and search are event work. Ordinary retained service has no search
    // allocations. A successful L4 search alone never installs or displaces a member.
    const auto path_around = [&](LandContact& c, std::size_t actor, std::size_t blocker) {
        auto& m = state_.members[actor];
        auto& squad = state_.squads[m.squad];
        c.route_actor = m.object; c.search_passes = 0;
        c.search_succeeded = false; c.installed = false;
        if (squad.path.nodes.empty()) { c.search = LandContactSearch::no_route; return false; }
        // WLPA-69: a terrain record acts on the colliding object. A non-generic member plans a short
        // local detour of its own; generic teams share their wrapper path and keep the squad route.
        // A refused parent step (mirrored onto every member) still re-routes the squad.
        const bool member_detour = c.kind == LandContactKind::terrain && !squad.generic_team && m.own_terrain_block;
        const auto pitch = grid.terrain().spacing();
        const auto columns = grid.terrain().width(), rows = grid.terrain().height();
        LandPathRequest request;
        request.entity = m.object; request.mask = m.mask;
        request.start = squad.position; request.end = squad.path.nodes.back();
        request.radius = m.profile.radius;
        if (member_detour) {
            Arithmetic arithmetic;
            const auto axis = math::sin_cos_turn(squad.facing);
            const Vec2 forward_axis{axis.cosine, axis.sine}, side_axis{arithmetic.sub(zero, axis.sine), axis.cosine};
            const auto slot = arithmetic.add(squad.position, arithmetic.add(
                arithmetic.mul(forward_axis, m.slot.x), arithmetic.mul(side_axis, m.slot.y)));
            const auto reach = arithmetic.add(m.profile.radius, detour_reach);
            const auto clear = [&](Vec2 point) {
                const auto px = land_cell_of(point.x, pitch, columns), py = land_cell_of(point.y, pitch, rows);
                if (!px || !py) return false;
                const auto footprint = land_footprint(m.profile.radius, pitch);
                const auto lx = static_cast<std::int64_t>(*px) + footprint.low, ly = static_cast<std::int64_t>(*py) + footprint.low;
                const auto hx = static_cast<std::int64_t>(*px) + footprint.high, hy = static_cast<std::int64_t>(*py) + footprint.high;
                return lx >= 0 && ly >= 0 && hx < static_cast<std::int64_t>(columns) && hy < static_cast<std::int64_t>(rows)
                    && grid.admits_footprint(static_cast<std::uint32_t>(lx), static_cast<std::uint32_t>(ly),
                        static_cast<std::uint32_t>(hx), static_cast<std::uint32_t>(hy), m.mask);
            };
            // The detour returns to the member's own slot when that point is clear, within reach and
            // not where the member already stands; otherwise to the parent point, which the squad
            // route already proves clear; the squad destination is the last resort (parent == member).
            const auto half_pitch = Fixed::from_raw(pitch.raw() / 2);
            const auto slot_distance = arithmetic.length(arithmetic.sub(slot, m.position));
            const auto parent_distance = arithmetic.length(arithmetic.sub(squad.position, m.position));
            request.start = m.position;
            request.end = slot_distance > half_pitch && slot_distance <= reach && clear(slot) ? slot : squad.position;
            if (parent_distance <= half_pitch) {
                // The member already stands at the parent point: aim one reach ahead on the squad route.
                auto from = squad.position;
                auto left = reach;
                request.end = squad.path.nodes.back();
                for (auto node = squad.next_node; node < squad.path.nodes.size(); ++node) {
                    const auto leg = arithmetic.sub(squad.path.nodes[node], from);
                    const auto length = arithmetic.length(leg);
                    if (!arithmetic.valid) break;
                    if (length >= left) {
                        request.end = length == zero ? from : arithmetic.add(from, arithmetic.mul(leg, arithmetic.div(left, length)));
                        break;
                    }
                    left = arithmetic.sub(left, length); from = squad.path.nodes[node];
                }
            }
            if (!arithmetic.valid) { c.search = LandContactSearch::no_route; return false; }
            // The planner's smoothing checks sight lines between cell centres, but the member keeps its exact
            // start point; a smoothed first leg can clip a blocked corner cell the stepper refuses, and the
            // installed detour would then never advance. The cell chain is walkable leg by leg (no corner cut).
            request.smooth = false;
        } else {
            for (const auto index : squad_members_[m.squad]) {
                request.mask &= state_.members[index].mask;
                request.radius = std::max(request.radius, state_.members[index].profile.radius);
            }
        }
        // Hard counterpart cells versus soft other moving contacts. Rasterization uses only copied
        // committed occupations, never another worker's proposals or a zone-cache rebuild.
        for (const auto& occupation : grid.occupations()) {
            const auto index = member_index(occupation.object);
            if (index == state_.members.size() || state_.members[index].squad == m.squad) continue;
            const auto& other = state_.members[index];
            auto& cells = index == blocker ? request.blocked_cells : request.collision_cells;
            const auto footprint = land_footprint(other.profile.radius, pitch);
            const auto append_cells = [&](const auto& source) {
                for (const auto cell : source) {
                    const auto x = static_cast<std::int64_t>(cell % columns);
                    const auto y = static_cast<std::int64_t>(cell / columns);
                    // Bound iteration by the actual grid, even for a mod-authored footprint far
                    // larger than the map. Outside cells cannot contribute to this request.
                    const auto low_y = std::max<std::int64_t>(0, y + footprint.low);
                    const auto high_y = std::min<std::int64_t>(rows - 1, y + footprint.high);
                    const auto low_x = std::max<std::int64_t>(0, x + footprint.low);
                    const auto high_x = std::min<std::int64_t>(columns - 1, x + footprint.high);
                    for (auto row = low_y; row <= high_y; ++row)
                        for (auto col = low_x; col <= high_x; ++col)
                            cells.push_back(static_cast<std::uint32_t>(row * columns + col));
                }
            };
            append_cells(occupation.primary_cells); append_cells(occupation.secondary_cells);
        }
        // If the counterpart currently occupies the base destination, escape to its side and then
        // resume the retained base tracker. Do not claim that reaching this temporary endpoint reaches
        // the blocked base order. Geometry follows the alternative cell planner, not retail graph pins.
        if (blocker < state_.members.size()) {
            const auto goal_x = land_cell_of(request.end.x, pitch, columns);
            const auto goal_y = land_cell_of(request.end.y, pitch, rows);
            const auto goal = goal_x && goal_y ? *goal_y * columns + *goal_x : std::numeric_limits<std::uint32_t>::max();
            if (std::find(request.blocked_cells.begin(), request.blocked_cells.end(), goal) != request.blocked_cells.end()) {
                Arithmetic arithmetic;
                const auto axis = math::sin_cos_turn(m.facing);
                const auto clearance = arithmetic.add(pitch, arithmetic.add(request.radius, state_.members[blocker].profile.radius));
                request.end = arithmetic.add(state_.members[blocker].position,
                    arithmetic.mul({arithmetic.sub(zero, axis.sine), axis.cosine}, clearance));
                if (!arithmetic.valid) { c.search = LandContactSearch::no_route; return false; }
            }
        }
        // LR3-02/03: charge the entire alternative planner graph, once per attempted pass,
        // before the zero-speed gate. This is a cell graph, not retail visibility-node units.
        const auto nodes = static_cast<std::uint64_t>(grid.cells().size());
        const auto charge = [&] {
            const auto room = std::numeric_limits<std::uint64_t>::max() - state_.contact_complexity;
            state_.contact_complexity += std::min(nodes, room);
            c.charged_nodes += std::min(nodes, std::numeric_limits<std::uint64_t>::max() - c.charged_nodes);
            ++c.search_passes;
        };
        if (!contact_arena_) contact_arena_ = std::make_unique<LandPathArena>();
        LandPath route;
        for (int pass = 0; pass < 2; ++pass) {
            charge();
            if (m.profile.maximum_speed <= zero || (!squad.generic_team && squad.maximum_speed <= zero)) continue;
            request.allow_collision_edges = pass != 0;
            route = LandPathService::plan(grid, *contact_arena_, request);
            if (route.status == LandPathStatus::found && route.nodes.size() > 1) break;
        }
        if (route.status != LandPathStatus::found || route.nodes.size() <= 1) {
            c.search = LandContactSearch::no_route; return false;
        }
        c.search_succeeded = true; c.search = LandContactSearch::found;
        const auto peer = blocker < state_.members.size() ? state_.members[blocker].object : m.object;
        bool lock_admitted = m.path_around_admitted;
        if (member_detour) {
            if (!lock_admitted || (m.collision_lock != 0 && m.collision_lock != m.object)
                || !m.detour_path.nodes.empty()) { c.search = LandContactSearch::install_rejected; return false; }
            m.detour_path = std::move(route); m.detour_next = 1; m.collision_lock = m.object;
            if (squad.order == LandMoveOrder::yielded) squad.order = LandMoveOrder::recovering;
            c.search = LandContactSearch::installed; c.installed = true;
            return true;
        }
        if (c.route == LandContactRoute::escape_aligned) lock_admitted &= m.escape_aligned_admitted;
        if (c.route == LandContactRoute::escape_opposed) lock_admitted &= m.escape_opposed_admitted;
        for (const auto index : squad_members_[m.squad])
            lock_admitted &= state_.members[index].collision_lock == 0 || state_.members[index].collision_lock == peer;
        if (!lock_admitted || (squad.temporary_first
            && (squad.temporary_first != c.first || squad.temporary_second != c.second))) {
            c.search = LandContactSearch::install_rejected; return false;
        }
        squad.temporary_path = std::move(route); squad.temporary_next = 1;
        squad.temporary_first = c.first; squad.temporary_second = c.second;
        if (squad.generic_team) for (const auto index : squad_members_[m.squad]) state_.members[index].temporary_next = 1;
        if (squad.order == LandMoveOrder::yielded) squad.order = LandMoveOrder::recovering;
        for (const auto index : squad_members_[m.squad]) state_.members[index].collision_lock = peer;
        c.search = LandContactSearch::installed; c.installed = true;
        return true;
    };

    bool service_valid = true;
    const auto service = [&](LandContact& c, bool pathfinding) {
        const auto x = member_index(c.first), y = member_index(c.second);
        if (x == state_.members.size() || (c.second && y == state_.members.size())) {
            c.state = LandContactState::resolved; release(c); return;
        }
        const auto& a = state_.members[x];
        const auto& as = state_.squads[a.squad];
        const bool first_progressed = a.progress > c.first_progress;
        const bool second_progressed = y < state_.members.size() && state_.members[y].progress > c.second_progress;
        c.first_progress = a.progress;
        if (y < state_.members.size()) c.second_progress = state_.members[y].progress;
        if (c.installed) {
            const auto actor = member_index(c.route_actor);
            if (actor == state_.members.size()) { c.state = LandContactState::resolved; release(c); return; }
            const auto& route_squad = state_.squads[state_.members[actor].squad];
            c.motion_observed |= c.route_actor == c.first ? first_progressed : second_progressed;
            if (c.kind == LandContactKind::terrain && !state_.members[actor].detour_path.nodes.empty()) {
                if (state_.members[actor].detour_next >= state_.members[actor].detour_path.nodes.size()) {
                    c.state = LandContactState::resolved; release(c);
                }
                return;
            }
            if (route_squad.temporary_first != c.first || route_squad.temporary_second != c.second
                || route_squad.temporary_next == route_squad.temporary_path.nodes.size()) {
                c.state = LandContactState::resolved; release(c);
            }
            return;
        }
        if (c.kind == LandContactKind::terrain) {
            // Keep denied terrain requests even when their zero speed prevents a fresh physical
            // collision. Recheck the retained blocked segment against the current committed grid.
            Arithmetic arithmetic;
            const auto delta = arithmetic.sub(c.terrain_target, a.position);
            const auto length = arithmetic.length(delta);
            const auto half_pitch = Fixed::from_raw(grid.terrain().spacing().raw() / 2);
            const auto samples = std::max<std::int64_t>(1, arithmetic.div(length, half_pitch).ceil_to_integer());
            if (!arithmetic.valid) { service_valid = false; return; }
            bool blocked = !land_cell_of(a.position.x, grid.terrain().spacing(), grid.terrain().width())
                || !land_cell_of(a.position.y, grid.terrain().spacing(), grid.terrain().height())
                || !land_cell_of(c.terrain_target.x, grid.terrain().spacing(), grid.terrain().width())
                || !land_cell_of(c.terrain_target.y, grid.terrain().spacing(), grid.terrain().height());
            const auto footprint = land_footprint(a.profile.radius, grid.terrain().spacing());
            for (std::int64_t sample = 0; !blocked && sample <= samples; ++sample) {
                const auto point = arithmetic.add(a.position, arithmetic.mul(delta, Fixed::from_ratio(sample, samples).value()));
                if (!arithmetic.valid) { service_valid = false; return; }
                const auto xcell = land_cell_of(point.x, grid.terrain().spacing(), grid.terrain().width());
                const auto ycell = land_cell_of(point.y, grid.terrain().spacing(), grid.terrain().height());
                if (!xcell || !ycell) { blocked = true; break; }
                const auto lx = static_cast<std::int64_t>(*xcell) + footprint.low;
                const auto ly = static_cast<std::int64_t>(*ycell) + footprint.low;
                const auto hx = static_cast<std::int64_t>(*xcell) + footprint.high;
                const auto hy = static_cast<std::int64_t>(*ycell) + footprint.high;
                if (lx < 0 || ly < 0 || hx >= grid.terrain().width() || hy >= grid.terrain().height()
                    || !grid.admits_footprint(static_cast<std::uint32_t>(lx), static_cast<std::uint32_t>(ly),
                        static_cast<std::uint32_t>(hx), static_cast<std::uint32_t>(hy), a.mask)) { blocked = true; break; }
            }
            if (!blocked && arithmetic.valid) { c.state = LandContactState::resolved; return; }
            if (c.state == LandContactState::unresolvable) return;
            if (!pathfinding) { c.search = LandContactSearch::quota_denied; c.first_stopped = true; return; }
            c.first_stopped = false;
            // A member's own failed detour tries again later (WLPA-69): its team would otherwise wait for
            // it for ever. Team-level and generic-team records keep the one-shot unresolvable outcome.
            const bool retry = !as.generic_team && a.own_terrain_block;
            c.state = path_around(c, x, state_.members.size()) ? LandContactState::resolving
                : retry ? LandContactState::initial : LandContactState::unresolvable;
            return;
        }
        const auto& b = state_.members[y];
        const auto& bs = state_.squads[b.squad];
        Arithmetic arithmetic;
        const auto& observation = contact_observations_[static_cast<std::size_t>(&c - state_.contacts.data())];
        // WLPA-83: a retained row of a pair that is now a stateless soft pair retires.
        if (land_soft_opposed(a, as, b, bs, straggler_repulsion_)) { c.state = LandContactState::resolved; release(c); return; }
        // LR3-09 prediction recheck precedes all role or maneuver selection.
        if (!observation.predicts) {
            c.state = LandContactState::resolved; release(c); return;
        }
        c.score = observation.score;
        if (as.temporary_first || bs.temporary_first) {
            // Another member contact already owns this squad's temporary tracker. Observe that
            // path rather than adding a second lock or stopping the admitted maneuver.
            c.state = LandContactState::existing_path;
            c.first_stopped = false; c.second_stopped = false;
            c.first_speed_limit.reset(); c.second_speed_limit.reset();
            return;
        }
        if (c.state == LandContactState::existing_path) { c.state = LandContactState::initial; return; }
        if (c.kind == LandContactKind::stopped_stopped) {
            c.state = LandContactState::retry_later; // no invented stopped-object repush distance
            return;
        }
        if (a.squad == b.squad) {
            if (a.offset_compatible && b.offset_compatible && a.slot != b.slot) {
                c.state = LandContactState::offset_exchange;
                c.first_stopped = false; c.second_stopped = false;
            } else {
                c.state = LandContactState::follower_speed;
                c.first_stopped = a.slot.x < b.slot.x || (a.slot.x == b.slot.x && a.object < b.object);
                c.second_stopped = !c.first_stopped;
            }
            return;
        }
        if (c.kind == LandContactKind::moving_moving && observation.same_direction) {
            c.state = LandContactState::same_direction_speed;
            c.first_speed_limit = observation.first_behind ? std::optional<Fixed>{bs.maximum_speed} : std::nullopt;
            c.second_speed_limit = observation.first_behind ? std::nullopt : std::optional<Fixed>{as.maximum_speed};
            c.first_stopped = false; c.second_stopped = false;
            return;
        }
        c.first_speed_limit.reset(); c.second_speed_limit.reset();
        c.primary = observation.primary;
        // LR3-17 / WLPA-57: infantry uses local repulsion against stopped infantry,
        // not a parent-owned detour that cannot advance while stragglers reform.
        // WLPA-82: the authored collision class of both members is the eligibility, not the object kind.
        if (straggler_repulsion_ && c.kind == LandContactKind::moving_stopped
            && land_soft_collision_class(a.profile.collision_class)
            && land_soft_collision_class(b.profile.collision_class)
            && (pathfinding || c.state == LandContactState::repulsion)) {
            c.state = LandContactState::repulsion;
            c.first_stopped = false; c.second_stopped = false;
            return;
        }
        const auto primary = c.primary == c.first ? x : y;
        const bool progressed = first_progressed || second_progressed;
        if (c.kind == LandContactKind::moving_moving
            && land_deadlock_visit(c, a.assigned_speed, b.assigned_speed, a.collision_speed,
                b.collision_speed, a.profile.acceleration, b.profile.acceleration, true, progressed)) {
            c.state = LandContactState::deadlocked;
        }
        if (c.state == LandContactState::deadlocked) {
            // LR3-12/13: only retained deadlocking moving-pair contacts count as blocking sources.
            std::size_t sources = 0;
            for (const auto& peer : state_.contacts)
                if (peer.kind == LandContactKind::moving_moving
                    && (peer.state == LandContactState::deadlocked || peer.state == LandContactState::recovery)
                    && (peer.first == c.primary || peer.second == c.primary)) ++sources;
            c.first_stopped = c.primary != c.first; c.second_stopped = c.primary != c.second;
            if (sources != 1) return;
            if (c.escape_retry_pending && !pathfinding) { c.search = LandContactSearch::quota_denied; return; }
            const auto actor = c.primary == c.first ? x : y;
            const auto aa = math::sin_cos_turn(a.facing), ba = math::sin_cos_turn(b.facing);
            const bool opposed = arithmetic.dot({aa.cosine, aa.sine}, {ba.cosine, ba.sine}) <= zero;
            c.route = opposed ? LandContactRoute::escape_opposed : LandContactRoute::escape_aligned;
            if (path_around(c, actor, actor == x ? y : x)) {
                c.state = LandContactState::recovery; c.escape_retry_pending = false;
            } else c.escape_retry_pending = true;
            return;
        }
        if (c.state == LandContactState::unresolvable || c.state == LandContactState::no_action) return;
        if (!pathfinding) {
            c.search = LandContactSearch::quota_denied;
            c.first_stopped = true; c.second_stopped = true;
            return;
        }
        const bool pass = a.pass_admitted && b.pass_admitted;
        c.route = pass ? LandContactRoute::pass : LandContactRoute::ordinary;
        const auto actor = pass ? primary : primary == x ? y : x;
        c.first_stopped = false; c.second_stopped = false;
        if (path_around(c, actor, actor == x ? y : x)) {
            c.state = pass ? LandContactState::pass : LandContactState::path_around;
            return;
        }
        // LR3-10: opposing moving actors can swap roles without rechecking quota within setup.
        if (c.kind == LandContactKind::moving_moving && c.search != LandContactSearch::install_rejected
            && path_around(c, actor == x ? y : x, actor)) {
            c.primary = state_.members[actor].object; c.state = LandContactState::path_around; return;
        }
        c.state = LandContactState::unresolvable;
        c.first_stopped = true; c.second_stopped = true;
    };

    for (std::size_t i = 0; i < state_.contacts.size(); ++i) {
        auto& c = state_.contacts[i];
        const auto& observation = contact_observations_[i];
        if (c.second && c.kind != observation.kind) {
            c.kind = observation.kind;
            if (!c.installed) {
                // A live subtype change gets its new dispatcher without discarding pair identity or
                // the quota ledger. It is a new maneuver qualification, not elapsed-time recovery.
                c.state = LandContactState::initial; c.search = LandContactSearch::none;
                c.first_stopped = false; c.second_stopped = false;
                c.first_speed_limit.reset(); c.second_speed_limit.reset();
                c.deadlock_remaining = 30; c.recorded_member = 0; c.escape_retry_pending = false;
            }
        }
        if (c.second) c.score = observation.score;
    }
    contact_order_.clear();
    for (std::size_t i = 0; i < state_.contacts.size(); ++i) {
        auto& c = state_.contacts[i];
        const bool maneuver = contact_observations_[i].maneuver;
        const bool needs_path = (c.kind == LandContactKind::terrain && c.state == LandContactState::initial)
            || c.kind == LandContactKind::moving_stopped
            || (c.kind == LandContactKind::moving_moving && c.state == LandContactState::initial && maneuver)
            || c.escape_retry_pending;
        if (needs_path) contact_order_.push_back(i);
        else service(c, true);
        if (!service_valid) return overflow();
    }
    std::sort(contact_order_.begin(), contact_order_.end(), [&](auto x, auto y) {
        const auto& a = state_.contacts[x]; const auto& b = state_.contacts[y];
        return std::tuple{a.score, a.first, a.second} < std::tuple{b.score, b.first, b.second};
    });
    for (const auto i : contact_order_) {
        service(state_.contacts[i], state_.contact_complexity < settings.complexity_quota);
        if (!service_valid) return overflow();
    }
    for (auto& c : state_.contacts) if (c.state == LandContactState::resolved) release(c);
    std::erase_if(state_.contacts, [](const auto& c) { return c.state == LandContactState::resolved; });

    if (!reference_pair_scans_) {
        core::tick_profile::Scope phase("land-contact-index");
        auto prepared = executor.execute_phase("land-member-proposals", tick_partition_count,
            [this, &grid](std::size_t partition) {
                const auto range = partition_range(partition, member_order_.size());
                const auto occupations = grid.occupations();
                for (auto rank = range.begin; rank < range.end; ++rank) {
                    const auto& m = state_.members[member_order_[rank]];
                    const auto at = std::lower_bound(occupations.begin(), occupations.end(), m.object,
                        [](const auto& o, EntityId id) { return o.object < id; });
                    Arithmetic a;
                    const auto end = prediction(a, m, state_.squads[m.squad], state_.contact_settings);
                    auto low = m.position, high = end;
                    const auto& squad = state_.squads[m.squad];
                    if (straggler_repulsion_ && land_soft_collision_class(m.profile.collision_class)
                        && squad.speed == zero && moving(squad) && squad.order != LandMoveOrder::yielded) {
                        const auto recovery = prediction(a, m, state_.squads[m.squad], state_.contact_settings, true);
                        low = {std::min({m.position.x, end.x, recovery.x}), std::min({m.position.y, end.y, recovery.y})};
                        high = {std::max({m.position.x, end.x, recovery.x}), std::max({m.position.y, end.y, recovery.y})};
                    }
                    // The scratch index needs the union of both sweeps; exact pair
                    // policy above still uses the old prediction for every other pair.
                    neighbours_.bodies[rank] = {low, high, m.profile.radius,
                        at != occupations.end() && at->object == m.object, a.valid};
                }
            });
        if (!prepared) return prepared;
        // Late service can install a temporary route, so refresh prediction bounds here
        // rather than reusing the earlier movement sweep. Both use the same retained index.
        core::tick_profile::Scope build("land-contact-index-build");
        neighbours_.build(grid.terrain().spacing());
    }
    const bool indexed = !reference_pair_scans_ && neighbours_.safe();
    // Per-object discovery reads only committed occupancy and writes disjoint retained scratch lists.
    // New contacts are not serviced until the next late invocation (LR1-09).
    struct DiscoveryContext { const LandOccupancy& grid; const LandContactSettings& settings; bool indexed; } discovery{grid, settings, indexed};
    auto result = executor.execute_phase("land-member-proposals", tick_partition_count, [this, &discovery](std::size_t partition) {
        const auto& occupancy = discovery.grid;
        const auto& profile = discovery.settings;
        const auto lookup_member = [this](EntityId id) -> std::size_t {
            const auto at = std::lower_bound(member_order_.begin(), member_order_.end(), id,
                [this](auto i, auto value) { return state_.members[i].object < value; });
            return at != member_order_.end() && state_.members[*at].object == id ? *at : state_.members.size();
        };
        const auto range = partition_range(partition, member_order_.size());
        for (auto rank = range.begin; rank < range.end; ++rank) {
            const auto i = member_order_[rank];
            const auto& a = state_.members[i]; const auto& as = state_.squads[a.squad];
            auto& found = discovered_[i]; found.clear();
            Arithmetic arithmetic;
            if (a.terrain_blocked) {
                LandContact c; c.first = a.object; c.terrain_target = a.terrain_target; found.push_back(c);
            }
            const auto visit = [&](std::size_t j) {
                if (j == state_.members.size() || state_.members[j].object <= a.object) return;
                const auto& b = state_.members[j]; const auto& bs = state_.squads[b.squad];
                if (land_soft_opposed(a, as, b, bs, straggler_repulsion_)) return;
                if (!collides(arithmetic, a, as, b, bs, profile, straggler_repulsion_)) return;
                LandContact c; c.first = a.object; c.second = b.object;
                c.kind = moving(as) && moving(bs) ? LandContactKind::moving_moving
                    : moving(as) || moving(bs) ? LandContactKind::moving_stopped : LandContactKind::stopped_stopped;
                c.score = arithmetic.length(arithmetic.sub(a.position, b.position));
                c.first_progress = a.progress; c.second_progress = b.progress;
                found.push_back(c);
            };
            if (discovery.indexed) {
                auto& candidates = neighbour_candidates_[i];
                neighbours_.query(rank, candidates);
                for (const auto candidate : candidates) visit(member_order_[candidate]);
            } else {
                for (const auto& occupation : occupancy.occupations()) {
                    if (occupation.object > a.object) visit(lookup_member(occupation.object));
                }
            }
            discovery_valid_[i] = arithmetic.valid ? 1 : 0;
        }
    });
    if (!result) return result;
    for (const auto valid : discovery_valid_) if (!valid) return overflow();
    for (const auto i : member_order_) for (const auto& c : discovered_[i]) {
        const auto at = std::lower_bound(state_.contacts.begin(), state_.contacts.end(), key(c),
            [](const auto& entry, const auto& value) { return key(entry) < value; });
        if (at == state_.contacts.end() || key(*at) != key(c)) state_.contacts.insert(at, c);
    }
    state_.contacts_serviced = true; state_.last_contact_frame = frame;
    return core::Result<void>::success();
}
} // namespace eawr::sim::tactical
