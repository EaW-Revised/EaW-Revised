#include "land_phase_internal.hpp"
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
// WLPA-54: the authored repush distance (data evidence): how far past touching a pushed container is sent.
constexpr auto repush_distance = Fixed::from_raw(6 * Fixed::scale);
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
auto key(const LandContact& c) { return std::pair{c.first, c.second}; }
bool soft_group(units::LandCollisionGroup group) { return land_soft_collision_class(group); }
bool deploying_state(LandWalkState state) {
    return state == LandWalkState::deploying || state == LandWalkState::deployed || state == LandWalkState::undeploying;
}
bool order_active(LandMoveOrder order) { return order == LandMoveOrder::following || order == LandMoveOrder::recovering; }
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
    if (settings.prediction_seconds < zero || settings.minimum_prediction_distance < zero
        || settings.stopped_overlap < zero || settings.stopped_overlap > one) return false;
    state_.contact_settings = settings;
    return true;
}

core::Result<void> LandMovement::service_contacts(const LandOccupancy& grid,
    const PartitionExecutor& executor, std::uint64_t frame, std::span<const std::int64_t> squad_sides) {
    const auto overflow = [] { return core::Result<void>::failure(core::Diagnostic{
        std::string(math::diagnostic_codes::overflow), core::Severity::error,
        "Land contact prediction is outside fixed-point range.", {}, {}, {}, {}}); };
    if (!squad_sides.empty() && squad_sides.size() != state_.squads.size()) return core::Result<void>::failure(core::Diagnostic{
        std::string(math::diagnostic_codes::overflow), core::Severity::error,
        "Land contact sides do not match the squads.", {}, {}, {}, {}});
    // Calling the same late barrier twice cannot reset a quota or service a deadlock twice.
    if (state_.contacts_serviced && frame <= state_.last_contact_frame) return core::Result<void>::success();
    const auto& settings = state_.contact_settings;
    if (settings.reset_interval < 2 || frame % settings.reset_interval == 0) state_.contact_complexity = 0;
    refresh_bodies();
    for (const auto& body : bodies_) if (!body.valid) return overflow();
    const auto body_count = bodies_.size();
    const auto member_index = [&](EntityId id) -> std::size_t {
        const auto at = std::lower_bound(member_order_.begin(), member_order_.end(), id,
            [&](auto i, auto value) { return state_.members[i].object < value; });
        return at != member_order_.end() && state_.members[*at].object == id ? *at : state_.members.size();
    };
    // The squad a contact endpoint acts for: a generic member's squad or the container's own.
    const auto release = [&](LandContact& c) {
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
            // A container that a contact pushed to an open spot has no base route: its move is complete.
            if (!s.generic_team && s.path.nodes.empty() && order_active(s.order)) s.order = LandMoveOrder::reached;
            for (const auto member : squad_members_[index]) state_.members[member].collision_lock = 0;
        }
        c.first_stopped = false; c.second_stopped = false;
        c.first_speed_limit.reset(); c.second_speed_limit.reset();
    };
    // Resolved entries from the preceding invocation are removed before service; newly resolved
    // entries below are cleaned before discovery, preserving LR1-09's late discovery boundary.
    for (auto& c : state_.contacts) if (c.state == LandContactState::resolved) release(c);
    std::erase_if(state_.contacts, [](const auto& c) { return c.state == LandContactState::resolved; });

    // WLPA-82: where a body is heading in the prediction window. A container walks its squad route; a generic member its own.
    const auto predict = [this](Arithmetic& a, const Body& body, const LandContactSettings& config) -> Vec2 {
        if (!body.moving) return body.position;
        const auto& s = state_.squads[body.squad];
        const auto& path = s.temporary_first ? s.temporary_path : s.path;
        const auto next = body.container ? (s.temporary_first ? s.temporary_next : s.next_node)
            : (s.temporary_first ? state_.members[body.member].temporary_next : state_.members[body.member].next_node);
        if (next >= path.nodes.size()) return body.position;
        const auto delta = a.sub(path.nodes[next], body.position);
        const auto length = a.length(delta);
        if (length == zero) return body.position;
        const auto travel = std::min(length, std::max(config.minimum_prediction_distance,
            a.mul(body.maximum_speed, config.prediction_seconds)));
        return a.add(body.position, a.mul(delta, a.div(travel, length)));
    };
    const auto collides = [&predict](Arithmetic& a, const Body& x, const Body& y, const LandContactSettings& config) {
        // Two idle bodies where one is a container: the oriented boxes must overlap by more than the stopped-pair
        // coefficient (WLPA-95). Every other pair is the swept disc test on the hard radii.
        if (!x.moving && !y.moving && (x.container || y.container)) return box_overlap(x, y) > config.stopped_overlap;
        const auto radius = a.add(x.radius, y.radius);
        if (radius <= zero) return false;
        const auto relative = a.sub(x.position, y.position);
        const auto delta = a.sub(a.sub(predict(a, x, config), x.position), a.sub(predict(a, y, config), y.position));
        const auto length = a.dot(delta, delta);
        const auto t = length > zero ? std::clamp(a.div(a.sub(zero, a.dot(relative, delta)), length), zero, one) : zero;
        return a.length(a.add(relative, a.mul(delta, t))) < radius;
    };
    // WLPA-57/83: the repulsion row completes when the peer is strictly behind the mover's facing plane by the summed
    // radii (1.3 times for a moving pair), or when the pair is out of steering reach and no longer predicted to meet.
    const auto repulsion_done = [](Arithmetic& a, const Body& x, const Body& y, bool predicts) {
        const auto sum = a.add(x.radius, y.radius);
        const auto reach = a.mul(sum, Fixed::from_ratio(13, 10).value());
        const auto behind = [&](const Body& mover, const Body& peer, Fixed factor) {
            const auto axis = math::sin_cos_turn(mover.facing);
            return a.dot(a.sub(peer.position, mover.position), {axis.cosine, axis.sine}) < a.sub(zero, a.mul(sum, factor));
        };
        if (x.moving && y.moving) {
            if (behind(x, y, Fixed::from_ratio(13, 10).value()) || behind(y, x, Fixed::from_ratio(13, 10).value())) return true;
        } else if (behind(x.moving ? x : y, x.moving ? y : x, one)) return true;
        return !predicts && a.length(a.sub(x.position, y.position)) >= reach;
    };

    // Copy pair observations on workers before the ordered quota/installation decisions. A
    // contact never reads a partially installed neighbor route to decide its initial role.
    contact_observations_.resize(state_.contacts.size());
    // A phase lambda captures at most two pointers: libstdc++ keeps only 16 bytes inside a std::function and allocates for more,
    // where the MSVC STL keeps 48; the rest travels in this context.
    struct PairContext { const LandContactSettings& settings; const decltype(collides)& collision_test; } pairs{settings, collides};
    auto observed = land_phase(executor, "land-member-proposals", tick_partition_count,
        [this, &pairs](std::size_t partition) {
        const auto range = partition_range(partition, state_.contacts.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto& contact = state_.contacts[i]; auto& proposal = contact_observations_[i];
            proposal = {}; proposal.kind = contact.kind;
            const auto x = body_index(contact.first), y = body_index(contact.second);
            if (x == bodies_.size() || y == bodies_.size()) continue;
            const auto& a = bodies_[x]; const auto& b = bodies_[y];
            proposal.kind = a.moving && b.moving ? LandContactKind::moving_moving
                : a.moving || b.moving ? LandContactKind::moving_stopped : LandContactKind::stopped_stopped;
            Arithmetic arithmetic;
            // WLPA-97: a pair the sweep held in the latest step still meets, whatever the prediction says (the prediction stops
            // at the next path node, the step may turn the corner after it), so the row that owns the hold is kept.
            proposal.predicts = pairs.collision_test(arithmetic, a, b, pairs.settings)
                || pair_blocker_[x] == b.object || pair_blocker_[y] == a.object;
            proposal.score = arithmetic.length(arithmetic.sub(a.position, b.position));
            // LE-OCCUPY: a pad has no squad; its heading is its own fixed pose.
            const auto first_facing = a.pad ? a.facing : state_.squads[a.squad].facing;
            const auto second_facing = b.pad ? b.facing : state_.squads[b.squad].facing;
            proposal.same_direction = same_heading(first_facing, second_facing);
            proposal.first_behind = !ahead_of(a, b);
            proposal.maneuver = a.squad != b.squad && !proposal.same_direction;
            auto primary = x;
            if (proposal.kind == LandContactKind::moving_stopped) primary = a.moving ? y : x;
            else if (a.rank != b.rank) primary = a.rank > b.rank ? x : y;
            else if (a.pad || b.pad) primary = a.pad ? x : y; // equal class: the pad, which cannot act, is the primary
            else if (state_.squads[a.squad].has_formation && state_.squads[b.squad].has_formation) {
                const auto& as = state_.squads[a.squad]; const auto& bs = state_.squads[b.squad];
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
            } else primary = a.maximum_speed <= b.maximum_speed ? x : y;
            proposal.primary = bodies_[primary].object;
            proposal.valid = arithmetic.valid;
        }
    });
    if (!observed) return observed;
    for (const auto& proposal : contact_observations_) if (!proposal.valid) return overflow();

    // The terrain footprint of a body: a container tests with its squad's common mask and widest soldier (the squad's own
    // route request); a generic member with its own.
    const auto terrain_mask = [&](const Body& body) {
        if (!body.container) return state_.members[body.member].mask;
        std::uint32_t mask = std::numeric_limits<std::uint32_t>::max();
        for (const auto index : squad_members_[body.squad]) mask &= state_.members[index].mask;
        return mask;
    };
    const auto terrain_radius = [&](const Body& body) {
        if (!body.container) return state_.members[body.member].profile.radius;
        Fixed radius;
        for (const auto index : squad_members_[body.squad]) radius = std::max(radius, state_.members[index].profile.radius);
        return radius;
    };

    // Every leg of a contact route must be a step the movement admits (one segment test, #4840 B1). The planner's sight
    // lines run between cell centres, so a leg from an off-centre start, or to an off-centre end, can cut a blocked corner
    // that the centres' line does not: such a route first walks to its start cell's centre, or ends through its end cell's
    // centre. A route that still has a refused leg is no route; installed, it would hold its body for ever (C4). The legs are
    // tested from the request start with the profile that walks them from there (review #4840 F5, R2-1): a container steps
    // with its widest soldier's radius and the common mask (the request's own); a generic team's position is its leader
    // member's at every commit, and the leader walks with its own radius and mask. Other generic members walk their own legs
    // to the route nodes with their own profile, as before; a refused one gets its own terrain contact.
    const auto admissible_route = [&](LandPath& route, Fixed radius, std::uint32_t mask) {
        auto& nodes = route.nodes;
        if (nodes.size() < 2) return false;
        const auto pitch = grid.terrain().spacing();
        const auto centre_of = [&](Vec2 point) -> std::optional<Vec2> {
            const auto cx = land_cell_of(point.x, pitch, grid.terrain().width());
            const auto cy = land_cell_of(point.y, pitch, grid.terrain().height());
            if (!cx || !cy) return std::nullopt;
            return Vec2{land_cell_centre(*cx, pitch), land_cell_centre(*cy, pitch)};
        };
        bool changed = false;
        if (!land_segment_admitted(grid, nodes[0], nodes[1], radius, mask)) {
            const auto centre = centre_of(nodes[0]);
            if (!centre || *centre == nodes[0] || *centre == nodes[1]) return false;
            nodes.insert(nodes.begin() + 1, *centre); changed = true;
        }
        if (nodes.size() > 2 && !land_segment_admitted(grid, nodes[nodes.size() - 2], nodes.back(), radius, mask)) {
            const auto centre = centre_of(nodes.back());
            if (!centre || *centre == nodes.back() || *centre == nodes[nodes.size() - 2]) return false;
            nodes.insert(nodes.end() - 1, *centre); changed = true;
        }
        for (std::size_t node = 1; node < nodes.size(); ++node)
            if (!land_segment_admitted(grid, nodes[node - 1], nodes[node], radius, mask)) return false;
        if (changed) {
            Arithmetic geo;
            Fixed total;
            for (std::size_t node = 1; node < nodes.size(); ++node)
                total = geo.add(total, geo.length(geo.sub(nodes[node], nodes[node - 1])));
            if (!geo.valid) return false;
            route.length = total;
        }
        return true;
    };

    // LR3-13: graph construction and search are event work. Ordinary retained service has no search
    // allocations. A successful L4 search alone never installs or displaces a body.
    const auto path_around = [&](LandContact& c, std::size_t actor, std::size_t blocker) {
        const auto& body = bodies_[actor];
        auto& m = state_.members[body.holder];
        auto& squad = state_.squads[body.squad];
        c.route_actor = body.object; c.search_passes = 0;
        c.search_succeeded = false; c.installed = false;
        if (squad.path.nodes.empty()) { c.search = LandContactSearch::no_route; return false; }
        const auto pitch = grid.terrain().spacing();
        const auto columns = grid.terrain().width(), rows = grid.terrain().height();
        LandPathRequest request;
        request.entity = body.object; request.mask = m.mask;
        request.start = squad.position; request.end = squad.path.nodes.back();
        request.radius = m.profile.radius;
        // The search widens to the whole squad: the common mask and the widest member (WLPA-70 project choice; a
        // container's own hard radius would shut it out of the corridors its soldiers use).
        for (const auto index : squad_members_[body.squad]) {
            request.mask &= state_.members[index].mask;
            request.radius = std::max(request.radius, state_.members[index].profile.radius);
        }
        // Hard counterpart cells versus soft other moving contacts. Rasterization uses only copied
        // committed occupations, never another worker's proposals or a zone-cache rebuild.
        for (const auto& occupation : grid.occupations()) {
            const auto index = member_index(occupation.object);
            if (index == state_.members.size() || state_.members[index].squad == body.squad) continue;
            const auto& other = state_.members[index];
            const bool blocking = blocker < body_count && (bodies_[blocker].container
                ? other.squad == bodies_[blocker].squad : index == bodies_[blocker].member);
            auto& cells = blocking ? request.blocked_cells : request.collision_cells;
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
        // The planner searches around the blocker's whole body, not just its soldiers' cells (the pair sweep holds at the
        // summed box radii): every cell whose centre lies within the summed radii of the blocker is closed, which is where
        // a body centred on that cell would be held. The actor's own start cell stays open (the planner exempts it).
        if (blocker < body_count) {
            const auto& other = bodies_[blocker];
            Arithmetic geo;
            const auto reach = geo.add(other.radius, body.radius);
            const auto cx = land_cell_of(other.position.x, pitch, columns), cy = land_cell_of(other.position.y, pitch, rows);
            if (!geo.valid) { c.search = LandContactSearch::no_route; return false; }
            if (cx && cy) {
                const auto span = geo.div(reach, pitch).ceil_to_integer() + 1;
                for (auto row = std::max<std::int64_t>(0, static_cast<std::int64_t>(*cy) - span);
                    row <= std::min<std::int64_t>(rows - 1, static_cast<std::int64_t>(*cy) + span); ++row)
                    for (auto col = std::max<std::int64_t>(0, static_cast<std::int64_t>(*cx) - span);
                        col <= std::min<std::int64_t>(columns - 1, static_cast<std::int64_t>(*cx) + span); ++col) {
                        const Vec2 centre{Fixed::from_raw(col * pitch.raw()), Fixed::from_raw(row * pitch.raw())};
                        if (geo.length(geo.sub(centre, other.position)) < reach)
                            request.blocked_cells.push_back(static_cast<std::uint32_t>(row * columns + col));
                    }
                if (!geo.valid) { c.search = LandContactSearch::no_route; return false; }
            }
        }
        // If the counterpart currently occupies the base destination, escape to its side and then
        // resume the retained base tracker. Do not claim that reaching this temporary endpoint reaches
        // the blocked base order. Geometry follows the alternative cell planner, not retail graph pins.
        if (blocker < body_count) {
            const auto goal_x = land_cell_of(request.end.x, pitch, columns);
            const auto goal_y = land_cell_of(request.end.y, pitch, rows);
            const auto goal = goal_x && goal_y ? *goal_y * columns + *goal_x : std::numeric_limits<std::uint32_t>::max();
            if (std::find(request.blocked_cells.begin(), request.blocked_cells.end(), goal) != request.blocked_cells.end()) {
                // An idle blocker on the destination is not walked round (the base order would only return to the same goal
                // and circle it): the search fails so that the push runs. A moving blocker gets a side goal.
                if (!bodies_[blocker].moving) { c.search = LandContactSearch::no_route; return false; }
                Arithmetic arithmetic;
                const auto axis = math::sin_cos_turn(body.facing);
                const auto clearance = arithmetic.add(pitch, arithmetic.add(request.radius, bodies_[blocker].radius));
                request.end = arithmetic.add(bodies_[blocker].position,
                    arithmetic.mul({arithmetic.sub(zero, axis.sine), axis.cosine}, clearance));
                if (!arithmetic.valid) { c.search = LandContactSearch::no_route; return false; }
            }
        }
        // The lock and route-ownership admission is known before any search: a route this squad could not install costs no
        // quota and no search (#4840 liveness, B2; a refused retry used to charge a full graph every 30 visits).
        const auto peer = blocker < body_count ? bodies_[blocker].object : body.object;
        bool lock_admitted = m.path_around_admitted;
        if (c.route == LandContactRoute::escape_aligned) lock_admitted &= m.escape_aligned_admitted;
        if (c.route == LandContactRoute::escape_opposed) lock_admitted &= m.escape_opposed_admitted;
        for (const auto index : squad_members_[body.squad])
            lock_admitted &= state_.members[index].collision_lock == 0 || state_.members[index].collision_lock == peer;
        if (!lock_admitted || (squad.temporary_first
            && (squad.temporary_first != c.first || squad.temporary_second != c.second))) {
            c.search = LandContactSearch::install_rejected; return false;
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
            if (body.maximum_speed <= zero || (!squad.generic_team && squad.maximum_speed <= zero)) continue;
            request.allow_collision_edges = pass != 0;
            route = LandPathService::plan(grid, *contact_arena_, request);
            if (route.status == LandPathStatus::found && route.nodes.size() > 1) break;
        }
        if (route.status != LandPathStatus::found || route.nodes.size() <= 1) {
            c.search = LandContactSearch::no_route; return false;
        }
        // A deadlock escape is the only way out for a hard pair stopped nose to nose. The cell planner exempts the actor's
        // start cell, so a blocker standing in the same cell can leave the cell behind it open and the planned first leg runs
        // straight through the blocker, which the pair sweep holds for ever. When the first leg passes closer than the summed
        // radii, step sideways first (one pitch plus the summed radii, as the blocked-destination case above clears), to the
        // left of the actor's facing or else the right. The step and both legs around it must be on ground the actor admits
        // and clear of the blocker, or the search fails (and the escape retries). Project choice: retail adds no such node
        // (it plans around the blocker's box); applies to generic teams too.
        if ((c.route == LandContactRoute::escape_aligned || c.route == LandContactRoute::escape_opposed) && blocker < body_count) {
            Arithmetic geo;
            const auto& other = bodies_[blocker];
            const auto reach = geo.add(body.radius, other.radius);
            const auto segment_gap = [&](Vec2 from, Vec2 to) {
                const auto leg = geo.sub(to, from);
                const auto off = geo.sub(other.position, from);
                const auto along = geo.dot(leg, leg);
                const auto t = along > zero ? std::clamp(geo.div(geo.dot(off, leg), along), zero, one) : zero;
                return geo.length(geo.sub(off, geo.mul(leg, t)));
            };
            // Each leg must be a step the movement admits (one segment test, C4): a weaker point test would place a leg the
            // next step refuses, and the escape would hold for ever.
            const auto leg_open = [&](Vec2 from, Vec2 to) {
                return land_segment_admitted(grid, from, to, request.radius, request.mask);
            };
            if (geo.valid && segment_gap(route.nodes[0], route.nodes[1]) < reach) {
                const auto axis = math::sin_cos_turn(body.facing);
                const auto clearance = geo.add(pitch, reach);
                const Vec2 left{geo.sub(zero, axis.sine), axis.cosine}, right{axis.sine, geo.sub(zero, axis.cosine)};
                bool placed = false;
                for (const auto& direction : {left, right}) {
                    const auto step = geo.add(route.nodes[0], geo.mul(direction, clearance));
                    if (!geo.valid || !leg_open(route.nodes[0], step) || !leg_open(step, route.nodes[1])
                        || segment_gap(step, route.nodes[1]) < reach) continue;
                    route.nodes.insert(route.nodes.begin() + 1, step);
                    Fixed total;
                    for (std::size_t node = 1; node < route.nodes.size(); ++node)
                        total = geo.add(total, geo.length(geo.sub(route.nodes[node], route.nodes[node - 1])));
                    if (geo.valid) route.length = total;
                    placed = true;
                    break;
                }
                if (!placed || !geo.valid) { c.search = LandContactSearch::no_route; return false; }
            }
        }
        {
            const auto& walker = squad.generic_team ? state_.members[squad_members_[body.squad].front()] : m;
            const auto leg_radius = squad.generic_team ? walker.profile.radius : request.radius;
            const auto leg_mask = squad.generic_team ? walker.mask : request.mask;
            if (!admissible_route(route, leg_radius, leg_mask)) { c.search = LandContactSearch::no_route; return false; }
        }
        c.search_succeeded = true; c.search = LandContactSearch::found;
        squad.temporary_path = std::move(route); squad.temporary_next = 1;
        squad.temporary_first = c.first; squad.temporary_second = c.second;
        if (squad.generic_team) for (const auto index : squad_members_[body.squad]) state_.members[index].temporary_next = 1;
        if (squad.order == LandMoveOrder::yielded) squad.order = LandMoveOrder::recovering;
        for (const auto index : squad_members_[body.squad]) state_.members[index].collision_lock = peer;
        c.search = LandContactSearch::installed; c.installed = true; c.retry_remaining = land_contact_retry_visits;
        return true;
    };

    // WLPA-57: the mover's move ends on the stopped body when the destination is still ahead of the mover (signed distance
    // along its facing at least -0.1) and a square at the destination, with half side the mover's larger box extent, intersects
    // the stopped body's oriented box. Only the contact's peer is tested (retail tests every stopped body near the destination).
    const auto ends_on = [&](const Body& mover, const Body& stopped) {
        const auto& s = state_.squads[mover.squad];
        const auto& path = s.temporary_first ? s.temporary_path : s.path;
        if (path.nodes.empty()) return false;
        Arithmetic a;
        const auto end = path.nodes.back();
        const auto axis = math::sin_cos_turn(mover.facing);
        const auto ahead = a.add(a.mul(a.sub(end.x, mover.position.x), axis.cosine), a.mul(a.sub(end.y, mover.position.y), axis.sine));
        if (!a.valid || ahead < Fixed::from_ratio(-1, 10).value()) return false;
        Body probe = mover;
        probe.position = end;
        probe.half_x = probe.half_y = std::max(mover.half_x, mover.half_y);
        return box_overlap(probe, stopped) > zero;
    };
    // WLPA-57: cut the move short at the last point before the mover would touch the stopped body, and stop there.
    // A vehicle against infantry or a bike may not truncate; neither may a mover that already walks a contact route.
    const auto truncate = [&](std::size_t mover_index, std::size_t stopped_index, const LandContact& self) {
        const auto& mover = bodies_[mover_index]; const auto& stopped = bodies_[stopped_index];
        auto& s = state_.squads[mover.squad];
        if (s.generic_team || s.temporary_first != 0 || !order_active(s.order) || s.next_node >= s.path.nodes.size()) return false;
        // A mover that holds a moving/moving contact is refused.
        for (const auto& other : state_.contacts) {
            if (&other == &self || other.state == LandContactState::resolved
                || (other.first != mover.object && other.second != mover.object)) continue;
            if (other.kind == LandContactKind::moving_moving) return false;
        }
        const bool vehicle = mover.group == units::LandCollisionGroup::vehicle
            || mover.group == units::LandCollisionGroup::large_vehicle || mover.group == units::LandCollisionGroup::giant_vehicle;
        if (vehicle && (stopped.group == units::LandCollisionGroup::infantry || stopped.group == units::LandCollisionGroup::bike)) return false;
        Arithmetic a;
        const auto reach = a.add(mover.radius, stopped.radius);
        const auto step = Fixed::from_raw(std::max<std::int64_t>(1, grid.terrain().spacing().raw() / 2));
        Vec2 cursor = s.position;
        if (a.length(a.sub(cursor, stopped.position)) < reach) {
            // Already touching: the move ends where the mover stands.
            s.path.nodes.resize(s.next_node);
            s.path.nodes.push_back(s.position);
            return a.valid;
        }
        for (auto node = s.next_node; node < s.path.nodes.size(); ++node) {
            const auto leg = a.sub(s.path.nodes[node], cursor);
            const auto length = a.length(leg);
            if (!a.valid) return false;
            const auto samples = std::max<std::int64_t>(1, a.div(length, step).ceil_to_integer());
            Vec2 safe = cursor;
            for (std::int64_t sample = 1; sample <= samples; ++sample) {
                const auto point = a.add(cursor, a.mul(leg, Fixed::from_ratio(sample, samples).value()));
                if (!a.valid) return false;
                if (a.length(a.sub(point, stopped.position)) < reach) {
                    s.path.nodes.resize(node);
                    s.path.nodes.push_back(safe);
                    return true;
                }
                safe = point;
            }
            cursor = s.path.nodes[node];
        }
        return false;
    };
    // WLPA-57: send the idle container to a nearby open spot clear of the other body: the landing point is the stopped
    // body's own position moved away from the other body by the missing separation plus the repush distance.
    const auto push = [&](LandContact& c, std::size_t pushed_index, std::size_t other_index) {
        const auto& pushed = bodies_[pushed_index]; const auto& other = bodies_[other_index];
        auto& squad = state_.squads[pushed.squad];
        c.search_succeeded = false; c.installed = false;
        if (!pushed.container || squad.generic_team || order_active(squad.order) || squad.order == LandMoveOrder::yielded
            || squad.temporary_first != 0 || pushed.maximum_speed <= zero) { c.search = LandContactSearch::no_route; return false; }
        Arithmetic a;
        auto direction = a.sub(pushed.position, other.position);
        auto length = a.length(direction);
        if (length == zero) {
            const auto axis = math::sin_cos_turn(other.facing);
            direction = {axis.cosine, axis.sine}; length = one;
        }
        const auto sum = a.add(pushed.radius, other.radius);
        auto distance = a.add(a.sub(sum, std::min(length, sum)), repush_distance);
        const auto desired = a.add(pushed.position, a.mul(direction, a.div(distance, length)));
        if (!a.valid) { c.search = LandContactSearch::no_route; return false; }
        LandPlacementQuery query;
        query.mask = terrain_mask(pushed); query.from = pushed.position; query.desired = desired;
        query.radius = terrain_radius(pushed);
        const auto placement = land_find_open_position(grid, query);
        if (placement.status == LandPlacementStatus::none || placement.position == pushed.position) {
            c.search = LandContactSearch::no_route; return false;
        }
        // The open-position search rings around the desired point and tries the mover's side first, so when the desired
        // point is closed the ring that reaches back to the mover lands on the mover's own spot (give or take a rounding
        // step). A spot that does not leave the other body by at least a unit is no push: no route, no zero-length escape
        // that completes at once and is installed again (#6115).
        {
            Arithmetic gap;
            const auto before = gap.length(gap.sub(pushed.position, other.position));
            const auto after = gap.length(gap.sub(placement.position, other.position));
            if (!gap.valid || after < gap.add(before, one)) { c.search = LandContactSearch::no_route; return false; }
        }
        // As in the path-around: a lock that refuses the install is known before the search is charged (B2).
        for (const auto index : squad_members_[pushed.squad])
            if (state_.members[index].collision_lock != 0 && state_.members[index].collision_lock != other.object) {
                c.search = LandContactSearch::install_rejected; return false;
            }
        LandPathRequest request;
        request.entity = pushed.object; request.mask = query.mask; request.radius = query.radius;
        request.start = pushed.position; request.end = placement.position;
        const auto nodes = static_cast<std::uint64_t>(grid.cells().size());
        const auto room = std::numeric_limits<std::uint64_t>::max() - state_.contact_complexity;
        state_.contact_complexity += std::min(nodes, room);
        c.charged_nodes += std::min(nodes, std::numeric_limits<std::uint64_t>::max() - c.charged_nodes);
        if (!contact_arena_) contact_arena_ = std::make_unique<LandPathArena>();
        auto route = LandPathService::plan(grid, *contact_arena_, request);
        if (route.status != LandPathStatus::found || route.nodes.size() <= 1
            || !admissible_route(route, request.radius, request.mask)) { c.search = LandContactSearch::no_route; return false; }
        c.search_succeeded = true; c.search = LandContactSearch::found; c.route_actor = pushed.object;
        squad.temporary_path = std::move(route); squad.temporary_next = 1;
        squad.temporary_first = c.first; squad.temporary_second = c.second;
        // The pushed container is idle: its base order is complete, so only the temporary route is walked.
        squad.next_node = squad.path.nodes.size();
        squad.order = LandMoveOrder::recovering;
        squad.following_start = std::min(squad.following_start, state_.tick);
        for (const auto index : squad_members_[pushed.squad]) state_.members[index].collision_lock = other.object;
        c.search = LandContactSearch::installed; c.installed = true; c.retry_remaining = land_contact_retry_visits;
        return true;
    };

    // WLPA-97: a squad whose contact has no way out ends its move where it stands, as an arrival would: a container's path is
    // complete (its soldiers finish passive slot recovery), a generic team releases its path. A temporary route the squad
    // walks is released with the row that owns it.
    const auto end_order = [&](std::size_t squad_index) {
        auto& s = state_.squads[squad_index];
        if (!order_active(s.order) && s.order != LandMoveOrder::yielded) return;
        if (s.temporary_first) for (auto& owner : state_.contacts)
            if (owner.first == s.temporary_first && owner.second == s.temporary_second && owner.state != LandContactState::resolved) {
                owner.state = LandContactState::resolved; release(owner);
            }
        s.order = LandMoveOrder::reached; s.yielding_to = 0;
        s.next_node = s.path.nodes.size();
        if (s.generic_team) {
            s.path = {}; s.next_node = 0;
            for (const auto member : squad_members_[squad_index]) {
                state_.members[member].next_node = 0; state_.members[member].temporary_next = 0;
            }
        }
    };
    // WLPA-97: the second failed resolution attempt without pair progress takes the terminal rung, which ends every hold of
    // the row: a terrain row, or a mover against an idle body it cannot path round or push, ends that move where it stands
    // (an idle body is never walked through); every other pair takes no_action (no maneuver, no veto between the two, the
    // soldiers steer).
    // WHZ-51: two bodies whose squads are on different sides (both owned by non-neutral players of different teams).
    const auto hostile = [&](const Body& one, const Body& other) {
        if (squad_sides.empty()) return false;
        const auto first = squad_sides[one.squad], second = squad_sides[other.squad];
        return first >= 0 && second >= 0 && first != second;
    };
    // WLPA-97 real progress: a body's straight-line distance to the end of its order (land_contact_no_gap without a moving
    // order: no baseline, not a zero distance). A pair row keeps, in its terrain_target (a terrain field no pair row uses),
    // each body's lowest distance at a failed attempt or at the progress that cleared one: x for the first body, y for the
    // second.
    const auto body_gap = [this](Arithmetic& arithmetic, const Body& body) {
        if (!body.moving || body.pad) return land_contact_no_gap; // LE-OCCUPY: a pad has no squad and no order
        const auto& path = state_.squads[body.squad].path;
        if (path.nodes.empty()) return land_contact_no_gap;
        return arithmetic.length(arithmetic.sub(path.nodes.back(), body.position));
    };
    const auto goal_gap = [&](Arithmetic& arithmetic, std::size_t index) {
        return index < body_count ? body_gap(arithmetic, bodies_[index]) : land_contact_no_gap;
    };
    const auto failed = [&](LandContact& c) {
        if (c.retry_failed) return true;
        c.retry_failed = true;
        if (c.second) {
            Arithmetic arithmetic;
            const auto first = goal_gap(arithmetic, body_index(c.first)), second = goal_gap(arithmetic, body_index(c.second));
            if (arithmetic.valid)
                c.terrain_target = {std::min(c.terrain_target.x, first), std::min(c.terrain_target.y, second)};
        }
        return false;
    };
    const auto exhaust = [&](LandContact& c, std::size_t x, std::size_t y) {
        c.first_stopped = false; c.second_stopped = false;
        c.first_speed_limit.reset(); c.second_speed_limit.reset();
        if (c.kind == LandContactKind::terrain) {
            end_order(bodies_[x].squad);
            c.state = LandContactState::resolved;
            return;
        }
        if (c.kind == LandContactKind::moving_stopped) {
            end_order(bodies_[bodies_[x].moving ? x : y].squad);
            c.state = LandContactState::resolved; release(c);
            return;
        }
        c.state = LandContactState::no_action;
    };

    bool service_valid = true;
    const auto service = [&](LandContact& c, bool pathfinding) {
        // A row that another row's terminal rung resolved earlier in this pass keeps that outcome.
        if (c.state == LandContactState::resolved) return;
        const auto x = body_index(c.first), y = c.second ? body_index(c.second) : body_count;
        if (x == body_count || (c.second && y == body_count)) {
            c.state = LandContactState::resolved; release(c); return;
        }
        const auto& a = bodies_[x];
        const bool first_progressed = a.progress > c.first_progress;
        const bool second_progressed = y < body_count && bodies_[y].progress > c.second_progress;
        c.first_progress = a.progress;
        if (y < body_count) c.second_progress = bodies_[y].progress;
        if (c.installed) {
            const auto actor = body_index(c.route_actor);
            if (actor == body_count) { c.state = LandContactState::resolved; release(c); return; }
            const auto route_index = bodies_[actor].squad;
            const auto& route_squad = state_.squads[route_index];
            bool actor_progressed = c.route_actor == c.first ? first_progressed : second_progressed;
            // A generic team's route lasts until its last member's tracker ends, so the whole team is watched: any member
            // that committed a moving speed or is turning in place is progress (#5996, B4). This reads committed state only.
            if (route_squad.generic_team) for (const auto member : squad_members_[route_index]) {
                const auto& m = state_.members[member];
                actor_progressed |= m.collision_speed > zero || m.state == LandWalkState::turning;
            }
            c.motion_observed |= actor_progressed;
            if (route_squad.temporary_first != c.first || route_squad.temporary_second != c.second
                || route_squad.temporary_next == route_squad.temporary_path.nodes.size()) {
                c.state = LandContactState::resolved; release(c);
            } else if (actor_progressed) {
                // WLPA-97: walking a contact route keeps a failed attempt. A route that creeps and stalls, chosen again
                // after a failure, takes the terminal rung when it is released (the partner it holds used to wait for ever
                // while each new route cleared the failure with its first step).
                c.retry_remaining = land_contact_retry_visits;
            } else if (c.retry_remaining > 0 && --c.retry_remaining == 0) {
                // WLPA-96: an installed route whose team makes no progress for the retry count is released. WLPA-97: the row
                // is kept and the release is a failed attempt, so the same route chosen again and failing again ends the row
                // instead of starting over with no history (C5).
                release(c);
                c.installed = false; c.retry_remaining = land_contact_retry_visits;
                if (c.kind == LandContactKind::terrain) c.state = LandContactState::unresolvable;
                else if (c.kind == LandContactKind::stopped_stopped) c.state = LandContactState::retry_later;
                else if (c.state == LandContactState::recovery) {
                    c.state = LandContactState::deadlocked; // a failed escape: the deadlock is not entered afresh
                } else {
                    c.state = LandContactState::unresolvable; c.first_stopped = true; c.second_stopped = true;
                }
                if (failed(c)) exhaust(c, x, y);
            }
            return;
        }
        // WLPA-97: only real progress clears a failed attempt: a body at least one cell pitch closer to the end of its order
        // than its lowest distance at the last failed attempt (or the last clear). A nudge or a turn in place is not enough
        // (a released route used to turn into a speed row, count a nudge as progress and be chosen again for ever). Each
        // clear lowers the baseline by a pitch, so a pair clears a bounded number of times. A deadlocked row keeps it
        // (LR3-07 escape admission); a terrain row ends when its step is admitted.
        if (c.retry_failed && c.second && c.state != LandContactState::deadlocked) {
            Arithmetic arithmetic;
            const auto pitch = grid.terrain().spacing();
            const auto first = goal_gap(arithmetic, x), second = goal_gap(arithmetic, y);
            // A body idle at the failure has no baseline: the first visit it moves sets one and clears nothing (review R1).
            const auto progressed = [&](Fixed now, Fixed& baseline) {
                if (now == land_contact_no_gap) return false;
                if (baseline == land_contact_no_gap) { baseline = now; return false; }
                return arithmetic.add(now, pitch) <= baseline;
            };
            const bool first_real = progressed(first, c.terrain_target.x);
            const bool second_real = progressed(second, c.terrain_target.y);
            if (arithmetic.valid && (first_real || second_real)) {
                c.retry_failed = false;
                c.terrain_target = {std::min(c.terrain_target.x, first), std::min(c.terrain_target.y, second)};
            }
        }
        if (c.kind == LandContactKind::terrain) {
            // A body without a moving order takes no step, so it has no refused step to resolve.
            if (!a.moving) { c.state = LandContactState::resolved; return; }
            // Keep denied terrain requests even when their zero speed prevents a fresh physical
            // collision. Recheck the retained blocked segment against the current committed grid with the movement's own
            // segment test, so the row is cleared only when the next step is admitted (C4).
            {
                // An endpoint outside the fixed-point range is a diagnostic, not a route outcome.
                Arithmetic range;
                static_cast<void>(range.length(range.sub(c.terrain_target, a.position)));
                if (!range.valid) { service_valid = false; return; }
            }
            if (land_segment_admitted(grid, a.position, c.terrain_target, terrain_radius(a), terrain_mask(a))) {
                c.state = LandContactState::resolved; return;
            }
            if (c.state == LandContactState::unresolvable) {
                if (first_progressed) c.retry_remaining = land_contact_retry_visits;
                else if (c.retry_remaining > 0) --c.retry_remaining;
                if (c.retry_remaining > 0) return;
            }
            if (!pathfinding) { c.search = LandContactSearch::quota_denied; return; }
            c.retry_remaining = land_contact_retry_visits;
            // WLPA-36: the terrain record acts on the colliding body (a container or a generic member). A failed search is
            // unresolvable and searches again after the WLPA-92 retry count (quota still applies).
            if (path_around(c, x, body_count)) { c.state = LandContactState::resolving; return; }
            c.state = LandContactState::unresolvable;
            if (failed(c)) exhaust(c, x, body_count);
            return;
        }
        const auto& b = bodies_[y];
        Arithmetic arithmetic;
        const auto& observation = contact_observations_[static_cast<std::size_t>(&c - state_.contacts.data())];
        if (a.pad || b.pad) {
            // LE-OCCUPY (WLPA-66, FD-82): a pad is the stopped counterpart of a body that can never move, yield or be pushed.
            const auto mover = a.pad ? y : x, pad = a.pad ? x : y;
            const auto& walker = bodies_[mover];
            if (!observation.predicts) { c.state = LandContactState::resolved; release(c); return; }
            c.score = observation.score;
            const auto& walker_squad = state_.squads[walker.squad];
            c.primary = bodies_[pad].object;
            if (walker_squad.temporary_first) {
                // Another contact already owns this squad's temporary tracker: observe it (as for any pair).
                c.state = LandContactState::existing_path;
                c.first_stopped = false; c.second_stopped = false;
                c.first_speed_limit.reset(); c.second_speed_limit.reset();
                return;
            }
            if (c.state == LandContactState::existing_path) { c.state = LandContactState::initial; return; }
            if (c.kind == LandContactKind::stopped_stopped) {
                // The mover stopped: an idle body beside a pad has nothing to resolve (see discovery).
                c.state = LandContactState::resolved; release(c);
                return;
            }
            const bool progressed = first_progressed || second_progressed;
            const bool searched_before = c.state == LandContactState::unresolvable;
            if (searched_before) {
                if (progressed) c.retry_remaining = land_contact_retry_visits;
                else if (c.retry_remaining > 0) --c.retry_remaining;
                if (c.retry_remaining > 0) return;
            }
            if (!pathfinding) { c.search = LandContactSearch::quota_denied; return; }
            c.retry_remaining = land_contact_retry_visits;
            c.first_speed_limit.reset(); c.second_speed_limit.reset();
            c.route = LandContactRoute::ordinary;
            c.first_stopped = false; c.second_stopped = false;
            // WLPA-57 as for any stopped body: a move that ends on the pad is cut short before it touches it.
            if (ends_on(walker, bodies_[pad]) && truncate(mover, pad, c)) { c.state = LandContactState::resolved; release(c); return; }
            if (path_around(c, mover, pad)) { c.state = LandContactState::path_around; return; }
            // WLPA-97 (#4840): a mover against an idle body it cannot path round ends that move where it stands (an idle body
            // is never walked through, and a pad has no push to fall back on). The first failed attempt retries after the retry
            // count; the second one without real progress takes the terminal rung, which ends the mover's order.
            c.state = LandContactState::unresolvable;
            c.first_stopped = true; c.second_stopped = true;
            if (failed(c)) exhaust(c, x, y);
            return;
        }
        const auto& as = state_.squads[a.squad];
        const auto& bs = state_.squads[b.squad];
        // LR3-09 prediction recheck precedes all role or maneuver selection. A retained repulsion row is the steering
        // gate (WLPA-59): it lives until the pair is past each other or out of reach, not only while a hit is predicted.
        if (c.state == LandContactState::repulsion && straggler_repulsion_) {
            if (repulsion_done(arithmetic, a, b, observation.predicts)) { c.state = LandContactState::resolved; release(c); return; }
        } else if (!observation.predicts) {
            c.state = LandContactState::resolved; release(c); return;
        }
        c.score = observation.score;
        if (c.state == LandContactState::no_action) {
            // WLPA-97 terminal rung: no maneuver and no hold until the pair no longer meets (the recheck above).
            c.first_stopped = false; c.second_stopped = false;
            c.first_speed_limit.reset(); c.second_speed_limit.reset();
            return;
        }
        const auto& holder_a = state_.members[a.holder]; const auto& holder_b = state_.members[b.holder];
        if (as.temporary_first || bs.temporary_first) {
            // Another contact already owns this squad's temporary tracker. Observe that
            // path rather than adding a second lock or stopping the admitted maneuver.
            c.state = LandContactState::existing_path;
            c.first_stopped = false; c.second_stopped = false;
            c.first_speed_limit.reset(); c.second_speed_limit.reset();
            return;
        }
        if (c.state == LandContactState::existing_path) { c.state = LandContactState::initial; return; }
        if (c.kind == LandContactKind::stopped_stopped) {
            // WLPA-57: two idle bodies overlapping by more than the coefficient: one is chosen (not deployed, then the
            // quicker escape) and its whole container walks to a nearby open spot. Attack state is not visible here.
            const auto deployed = [&](const Body& body) {
                if (!body.container) return deploying_state(state_.members[body.member].state);
                for (const auto index : squad_members_[body.squad]) if (deploying_state(state_.members[index].state)) return true;
                return false;
            };
            if (c.state == LandContactState::retry_later) {
                if (first_progressed || second_progressed) c.retry_remaining = land_contact_retry_visits;
                else if (c.retry_remaining > 0) --c.retry_remaining;
                if (c.retry_remaining > 0) return;
            }
            const bool a_free = a.container && !deployed(a), b_free = b.container && !deployed(b);
            if (!a_free && !b_free) {
                c.state = LandContactState::retry_later; c.retry_remaining = land_contact_retry_visits;
                if (failed(c)) exhaust(c, x, y);
                return;
            }
            if (!pathfinding) { c.search = LandContactSearch::quota_denied; return; }
            c.retry_remaining = land_contact_retry_visits;
            const bool move_a = a_free && (!b_free || a.maximum_speed >= b.maximum_speed);
            c.primary = move_a ? b.object : a.object;
            if (push(c, move_a ? x : y, move_a ? y : x)) { c.state = LandContactState::infantry_push; return; }
            c.state = LandContactState::retry_later;
            if (failed(c)) exhaust(c, x, y);
            return;
        }
        if (a.squad == b.squad) {
            if (holder_a.offset_compatible && holder_b.offset_compatible && holder_a.slot != holder_b.slot) {
                c.state = LandContactState::offset_exchange;
                c.first_stopped = false; c.second_stopped = false;
            } else {
                c.state = LandContactState::follower_speed;
                c.first_stopped = holder_a.slot.x < holder_b.slot.x || (holder_a.slot.x == holder_b.slot.x && a.object < b.object);
                c.second_stopped = !c.first_stopped;
            }
            return;
        }
        // WLPA-83: two moving soft bodies of different squads, opposed or crossing, both keep their speed and their
        // soldiers steer apart; the row is only the steering gate.
        if (c.kind == LandContactKind::moving_moving && soft_opposed(a, b, straggler_repulsion_)) {
            c.state = LandContactState::repulsion;
            c.first_speed_limit.reset(); c.second_speed_limit.reset();
            c.first_stopped = false; c.second_stopped = false;
            return;
        }
        if (c.kind == LandContactKind::moving_moving && observation.same_direction) {
            c.state = LandContactState::same_direction_speed;
            c.first_speed_limit = observation.first_behind ? std::optional<Fixed>{bs.maximum_speed} : std::nullopt;
            c.second_speed_limit = observation.first_behind ? std::nullopt : std::optional<Fixed>{as.maximum_speed};
            c.first_stopped = false; c.second_stopped = false;
            return;
        }
        c.primary = observation.primary;
        const bool soft_pair = straggler_repulsion_ && soft_group(a.group) && soft_group(b.group);
        // LR3-17 / WLPA-57: soft bodies use local repulsion against a stopped soft body, unless the move would end on it.
        // WLPA-82: the authored class of the two bodies is the eligibility, not the object kind.
        if (soft_pair && c.kind == LandContactKind::moving_stopped
            && !ends_on(a.moving ? a : b, a.moving ? b : a)
            && (pathfinding || c.state == LandContactState::repulsion)) {
            c.state = LandContactState::repulsion;
            c.first_speed_limit.reset(); c.second_speed_limit.reset();
            c.first_stopped = false; c.second_stopped = false;
            return;
        }
        const auto primary = c.primary == c.first ? x : y;
        const bool progressed = first_progressed || second_progressed;
        bool entered = false;
        if (c.kind == LandContactKind::moving_moving
            && land_deadlock_visit(c, a.assigned_speed, b.assigned_speed, a.collision_speed,
                b.collision_speed, a.acceleration, b.acceleration, true, progressed)) {
            // WLPA-97 (#6845): entering the deadlock keeps a failed attempt of the same ladder (the path-around that stalled, the
            // search that failed), so the first escape that fails after it is the second failure and takes the terminal rung. The
            // entry visit itself escapes at once and is ordinary service (LR3-07 first escape, quota exempt): `entered` is local,
            // nothing is saved, and a restore always lands after it.
            entered = c.state != LandContactState::deadlocked;
            if (entered) c.retry_remaining = land_contact_retry_visits;
            c.state = LandContactState::deadlocked;
        }
        if (c.state == LandContactState::deadlocked) {
            c.first_speed_limit.reset(); c.second_speed_limit.reset();
            // LR3-12/13: only retained deadlocking moving-pair contacts count as blocking sources.
            std::size_t sources = 0;
            for (const auto& peer : state_.contacts)
                if (peer.kind == LandContactKind::moving_moving
                    && (peer.state == LandContactState::deadlocked || peer.state == LandContactState::recovery)
                    && (peer.first == c.primary || peer.second == c.primary)) ++sources;
            c.first_stopped = c.primary != c.first; c.second_stopped = c.primary != c.second;
            if (sources != 1) {
                // LR3-12/13 gives no escape to a primary with several deadlocked sources. WLPA-97: each retry count of visits
                // without pair progress is a failed attempt, and the second one takes the terminal rung (#5995, C1).
                if (progressed) c.retry_remaining = land_contact_retry_visits;
                else if (c.retry_remaining > 0) --c.retry_remaining;
                if (c.retry_remaining > 0) return;
                c.retry_remaining = land_contact_retry_visits;
                if (failed(c)) exhaust(c, x, y);
                return;
            }
            if (c.retry_failed && !entered) {
                // A failed escape waits like an unresolvable row (WLPA-92): it searches again only after the retry count of
                // visits without pair progress, and then through quota admission (#4840 liveness, B3: it used to search on
                // every admitted tick and could take the only admission of a large map for ever).
                // The count runs in ordinary service; the retry itself is queued (needs_path) once the count is spent.
                if (progressed) { c.retry_remaining = land_contact_retry_visits; return; }
                if (c.retry_remaining > 0) { --c.retry_remaining; return; }
                if (!pathfinding) { c.search = LandContactSearch::quota_denied; return; }
            }
            const auto actor = c.primary == c.first ? x : y;
            const auto aa = math::sin_cos_turn(a.facing), ba = math::sin_cos_turn(b.facing);
            const bool opposed = arithmetic.dot({aa.cosine, aa.sine}, {ba.cosine, ba.sine}) <= zero;
            c.route = opposed ? LandContactRoute::escape_opposed : LandContactRoute::escape_aligned;
            if (path_around(c, actor, actor == x ? y : x)) {
                c.state = LandContactState::recovery;
            } else {
                c.retry_remaining = land_contact_retry_visits;
                if (failed(c)) exhaust(c, x, y);
            }
            return;
        }
        if (c.state == LandContactState::unresolvable) {
            if (progressed) c.retry_remaining = land_contact_retry_visits;
            else if (c.retry_remaining > 0) --c.retry_remaining;
            if (c.retry_remaining > 0) return;
        }
        if (!pathfinding) {
            c.search = LandContactSearch::quota_denied;
            return;
        }
        c.retry_remaining = land_contact_retry_visits;
        c.first_speed_limit.reset(); c.second_speed_limit.reset();
        const bool pass = holder_a.pass_admitted && holder_b.pass_admitted;
        c.route = pass ? LandContactRoute::pass : LandContactRoute::ordinary;
        const auto actor = pass ? primary : primary == x ? y : x;
        const auto blocker = actor == x ? y : x;
        c.first_stopped = false; c.second_stopped = false;
        // WLPA-57: a moving body whose move ends on a stopped body first tries to cut the move short (the mover then
        // stops at once), then a path around it, then pushes the stopped container away, then retries at zero speed.
        if (c.kind == LandContactKind::moving_stopped && actor == (a.moving ? x : y)
            && ends_on(bodies_[actor], bodies_[blocker]) && truncate(actor, blocker, c)) {
            c.state = LandContactState::resolved; release(c);
            return;
        }
        if (path_around(c, actor, blocker)) {
            c.state = pass ? LandContactState::pass : LandContactState::path_around;
            return;
        }
        // LR3-10: opposing moving actors can swap roles without rechecking quota within setup.
        // A lock refusal is known before the search (B2) and says nothing about a route, so after any failed path-around the
        // role swap and the push run (review #4840 F1): as on main when no route existed; where main found a route and was
        // then refused by the lock it skipped them.
        if (c.kind == LandContactKind::moving_moving && path_around(c, blocker, actor)) {
            c.primary = bodies_[actor].object; c.state = LandContactState::path_around; return;
        }
        // WLPA-57: a moving body never pushes a hostile idle body aside (#6062); the pair goes on to retry and then the terminal rung.
        if (c.kind == LandContactKind::moving_stopped && !hostile(a, b)) {
            const auto mover = a.moving ? x : y, stopped = a.moving ? y : x;
            if (push(c, stopped, mover)) { c.state = LandContactState::infantry_push; return; }
        }
        // WLPA-57: after path-around and push fail, two soft classes go to repulsion (the mover walks on and the soldiers
        // steer); only a harder pair retries at zero speed. A hostile soft pair does not (#6062): the mover would walk on
        // through the enemy body it may not push, so it retries and its move ends in front of it (WLPA-97). Retail never
        // uses repulsion when the move ends on the idle body (truncate, path-around, push, else retry at zero speed); the
        // soft fallthrough is the remake's, and it is kept only for friendly and neutral pairs.
        if (soft_pair && c.kind == LandContactKind::moving_stopped && !hostile(a, b)) {
            c.state = LandContactState::repulsion;
            c.first_stopped = false; c.second_stopped = false;
            return;
        }
        c.state = LandContactState::unresolvable;
        c.first_stopped = true; c.second_stopped = true;
        if (failed(c)) exhaust(c, x, y);
    };

    for (std::size_t i = 0; i < state_.contacts.size(); ++i) {
        auto& c = state_.contacts[i];
        const auto& observation = contact_observations_[i];
        // A row whose participant is no longer a body (a retired pad, FD-82) has no observation; its service resolves it.
        if (c.second && c.kind != observation.kind && body_index(c.first) < body_count && body_index(c.second) < body_count) {
            c.kind = observation.kind;
            // WLPA-97: no_action is only for two moving bodies; a pair that is no longer moving/moving starts again (the
            // failed attempt is kept, so its next failure takes the terminal rung and an idle body is never walked through).
            if (!c.installed && (c.state != LandContactState::no_action || c.kind != LandContactKind::moving_moving)) {
                // A live subtype change gets its new dispatcher without discarding pair identity or
                // the quota ledger. It is a new maneuver qualification, not elapsed-time recovery. WLPA-97: a failed
                // attempt is kept (a subtype change is no progress), and a terminal row stays terminal while the pair meets.
                c.state = LandContactState::initial; c.search = LandContactSearch::none;
                c.first_stopped = false; c.second_stopped = false;
                c.first_speed_limit.reset(); c.second_speed_limit.reset();
                c.deadlock_remaining = 30; c.recorded_member = 0;
                c.retry_remaining = land_contact_retry_visits;
            }
        }
        if (c.second) c.score = observation.score;
    }
    contact_order_.clear();
    for (std::size_t i = 0; i < state_.contacts.size(); ++i) {
        auto& c = state_.contacts[i];
        bool soft_row = false;
        if (c.kind == LandContactKind::moving_moving && c.second) {
            const auto first = body_index(c.first), second = body_index(c.second);
            soft_row = first < body_count && second < body_count && soft_opposed(bodies_[first], bodies_[second], straggler_repulsion_);
        }
        const bool maneuver = contact_observations_[i].maneuver && !soft_row;
        const bool needs_path = (c.kind == LandContactKind::terrain
                && (c.state == LandContactState::initial || c.state == LandContactState::unresolvable))
            || c.kind == LandContactKind::moving_stopped
            || (c.kind == LandContactKind::moving_moving && c.state == LandContactState::initial && maneuver)
            || (c.kind == LandContactKind::moving_moving && c.state == LandContactState::unresolvable)
            || (c.kind == LandContactKind::stopped_stopped && !c.installed
                && (c.state == LandContactState::initial || (c.state == LandContactState::retry_later && c.retry_remaining == 0)))
            || (c.retry_failed && c.retry_remaining == 0);
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

    refresh_bodies();
    for (const auto& body : bodies_) if (!body.valid) return overflow();
    if (!reference_pair_scans_) {
        core::tick_profile::Scope phase("land-contact-index");
        struct PredictContext { const LandOccupancy& grid; const decltype(predict)& prediction_fn; } prediction{grid, predict};
        auto prepared = land_phase(executor, "land-member-proposals", tick_partition_count,
            [this, &prediction](std::size_t partition) {
                const auto range = partition_range(partition, bodies_.size());
                const auto occupations = prediction.grid.occupations();
                for (auto b = range.begin; b < range.end; ++b) {
                    const auto& body = bodies_[b];
                    // A container holds no occupation of its own; its soldiers do.
                    bool present = true;
                    if (!body.container && !body.pad) {
                        const auto object = state_.members[body.member].object;
                        const auto at = std::lower_bound(occupations.begin(), occupations.end(), object,
                            [](const auto& o, EntityId id) { return o.object < id; });
                        present = at != occupations.end() && at->object == object;
                    }
                    Arithmetic a;
                    const auto end = prediction.prediction_fn(a, body, state_.contact_settings);
                    // The scratch index needs the union of the sweeps; exact pair policy above uses the same prediction.
                    neighbours_.bodies[b] = {body.position, end, body.radius, present, a.valid};
                }
            });
        if (!prepared) return prepared;
        // Late service can install a temporary route, so refresh prediction bounds here
        // rather than reusing the earlier movement sweep. Both use the same retained index.
        core::tick_profile::Scope build("land-contact-index-build");
        neighbours_.build(grid.terrain().spacing());
    }
    const bool indexed = !reference_pair_scans_ && neighbours_.safe();
    // Per-body discovery reads only committed occupancy and writes disjoint retained scratch lists.
    // New contacts are not serviced until the next late invocation (LR1-09).
    struct DiscoveryContext {
        const LandOccupancy& grid; const LandContactSettings& settings; bool indexed; const decltype(collides)& collision_test;
        const decltype(body_gap)& gap;
    } discovery{grid, settings, indexed, collides, body_gap};
    auto result = land_phase(executor, "land-member-proposals", tick_partition_count,
        [this, &discovery](std::size_t partition) {
        const auto& occupancy = discovery.grid;
        const auto& profile = discovery.settings;
        const auto range = partition_range(partition, bodies_.size());
        for (auto rank = range.begin; rank < range.end; ++rank) {
            const auto& a = bodies_[rank];
            auto& found = discovered_[rank]; found.clear();
            Arithmetic arithmetic;
            if (!a.pad) {
                const auto& s = state_.squads[a.squad];
                if (a.container ? s.terrain_blocked : state_.members[a.member].terrain_blocked) {
                    LandContact c; c.first = a.object;
                    c.terrain_target = a.container ? s.terrain_target : state_.members[a.member].terrain_target;
                    found.push_back(c);
                }
            }
            const auto visit = [&](std::size_t j) {
                if (j >= bodies_.size() || bodies_[j].object <= a.object) return;
                const auto& b = bodies_[j];
                // LE-OCCUPY: a pad only ever meets a moving body; an idle body beside a pad has nothing to resolve (project
                // choice: nothing authored says an idle body must leave a pad, and the pad cannot be pushed away).
                if ((a.pad || b.pad) && !a.moving && !b.moving) return;
                if (!discovery.collision_test(arithmetic, a, b, profile)) return;
                LandContact c; c.first = a.object; c.second = b.object;
                c.kind = a.moving && b.moving ? LandContactKind::moving_moving
                    : a.moving || b.moving ? LandContactKind::moving_stopped : LandContactKind::stopped_stopped;
                c.score = arithmetic.length(arithmetic.sub(a.position, b.position));
                c.first_progress = a.progress; c.second_progress = b.progress;
                c.terrain_target = {discovery.gap(arithmetic, a), discovery.gap(arithmetic, b)};
                found.push_back(c);
            };
            // WLPA-97: a pair the sweep held in the latest step always has a row, even where the prediction misses it.
            if (const auto blocker = pair_blocker_[rank]; blocker) {
                const auto j = body_index(blocker);
                if (j < bodies_.size()) {
                    const auto& b = bodies_[j];
                    LandContact c;
                    c.first = std::min(a.object, b.object); c.second = std::max(a.object, b.object);
                    c.kind = a.moving && b.moving ? LandContactKind::moving_moving
                        : a.moving || b.moving ? LandContactKind::moving_stopped : LandContactKind::stopped_stopped;
                    c.score = arithmetic.length(arithmetic.sub(a.position, b.position));
                    const auto& low = a.object < b.object ? a : b; const auto& high = a.object < b.object ? b : a;
                    c.first_progress = low.progress; c.second_progress = high.progress;
                    c.terrain_target = {discovery.gap(arithmetic, low), discovery.gap(arithmetic, high)};
                    found.push_back(c);
                }
            }
            if (discovery.indexed) {
                auto& candidates = neighbour_candidates_[rank];
                neighbours_.query(rank, candidates);
                for (const auto candidate : candidates) visit(candidate);
            } else {
                const auto occupied = [&](const Body& body) {
                    if (body.container || body.pad) return true;
                    const auto object = state_.members[body.member].object;
                    const auto at = std::lower_bound(occupancy.occupations().begin(), occupancy.occupations().end(), object,
                        [](const auto& o, EntityId id) { return o.object < id; });
                    return at != occupancy.occupations().end() && at->object == object;
                };
                for (std::size_t j = rank + 1; j < bodies_.size(); ++j) if (occupied(bodies_[j])) visit(j);
            }
            discovery_valid_[rank] = arithmetic.valid ? 1 : 0;
        }
    });
    if (!result) return result;
    for (const auto valid : discovery_valid_) if (!valid) return overflow();
    for (std::size_t rank = 0; rank < bodies_.size(); ++rank) for (const auto& c : discovered_[rank]) {
        const auto at = std::lower_bound(state_.contacts.begin(), state_.contacts.end(), key(c),
            [](const auto& entry, const auto& value) { return key(entry) < value; });
        if (at == state_.contacts.end() || key(*at) != key(c)) state_.contacts.insert(at, c);
    }
    state_.contacts_serviced = true; state_.last_contact_frame = frame;
    return core::Result<void>::success();
}
} // namespace eawr::sim::tactical
