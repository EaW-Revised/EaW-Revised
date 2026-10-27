#include "eawr/sim/tactical/land_movement.hpp"
#include "eawr/sim/math/trig.hpp"
#include "eawr/core/tick_profile.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <string>

namespace eawr::sim::tactical {
namespace {
using math::Fixed;
using math::Vec2;
constexpr auto zero = Fixed{};
constexpr auto one = Fixed::from_raw(Fixed::scale);
constexpr auto ten = Fixed::from_raw(10 * Fixed::scale);
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
bool valid_path(const LandMovingSquad& squad) {
    return squad.next_node <= squad.path.nodes.size()
        && ((squad.order != LandMoveOrder::following && squad.order != LandMoveOrder::recovering)
            || (squad.path.status == LandPathStatus::found && !squad.path.nodes.empty()));
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
        if (!squad.object || squad.maximum_speed < zero || squad.speed < zero || squad.rear_extent < zero
            || squad.formation_length < zero || squad.temporary_next > squad.temporary_path.nodes.size()
            || (squad.temporary_first && (squad.temporary_path.status != LandPathStatus::found
                || squad.temporary_path.nodes.size() < 2))
            || !valid_path(squad) || squad.order > LandMoveOrder::reached
            || (squad.order == LandMoveOrder::yielded && !squad.yielding_to)) return std::nullopt;
        ids.push_back(squad.object);
    }
    for (const auto& m : saved.members) {
        if (!m.object || m.squad >= saved.squads.size() || m.profile.maximum_speed < zero
            || m.profile.acceleration < zero || m.profile.deceleration < zero || m.profile.turn_rate < zero
            || m.profile.radius < zero || m.speed < zero || m.speed > m.profile.maximum_speed
            || m.transition_frame < zero || m.deployment_frame < zero
            || m.assigned_speed < zero || m.collision_speed < zero
            || m.state > LandWalkState::undeploying) return std::nullopt;
        ids.push_back(m.object);
        const auto& s = saved.squads[m.squad];
        if (m.next_node > s.path.nodes.size() || m.temporary_next > s.temporary_path.nodes.size()
            || m.detour_next > m.detour_path.nodes.size()
            || (!m.detour_path.nodes.empty() && (m.detour_path.status != LandPathStatus::found
                || m.detour_path.nodes.size() < 2 || s.generic_team))) return std::nullopt;
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return std::nullopt;
    if (saved.contact_settings.prediction_seconds < zero || saved.contact_settings.minimum_prediction_distance < zero)
        return std::nullopt;
    for (std::size_t i = 0; i < saved.contacts.size(); ++i) {
        const auto& c = saved.contacts[i];
        const auto member_exists = [&](EntityId id) {
            return std::any_of(saved.members.begin(), saved.members.end(), [&](const auto& m) { return m.object == id; });
        };
        if (!member_exists(c.first) || (c.second && (!member_exists(c.second) || c.second <= c.first))
            || ((c.kind == LandContactKind::terrain) != (c.second == 0))
            || c.kind > LandContactKind::moving_moving || c.state > LandContactState::recovery
            || c.route > LandContactRoute::escape_opposed
            || (c.first_speed_limit && *c.first_speed_limit < zero)
            || (c.second_speed_limit && *c.second_speed_limit < zero)
            || c.search > LandContactSearch::installed || c.deadlock_remaining > 30 || c.search_passes > 2
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
    for (const auto& c : saved.contacts) if (c.installed && c.state != LandContactState::resolved) {
        const auto actor = std::find_if(saved.members.begin(), saved.members.end(), [&](const auto& m) {
            return m.object == c.route_actor;
        });
        const auto& s = saved.squads[actor->squad];
        // WLPA-69: a non-generic terrain contact owns its member's detour, not the squad route.
        if (c.kind == LandContactKind::terrain && !actor->detour_path.nodes.empty()) continue;
        if (s.temporary_first != c.first || s.temporary_second != c.second) return std::nullopt;
    }
    // A retained detour always belongs to one installed terrain contact of that member.
    for (const auto& m : saved.members) if (!m.detour_path.nodes.empty()) {
        const auto contact = std::find_if(saved.contacts.begin(), saved.contacts.end(), [&](const auto& c) {
            return c.first == m.object && c.second == 0 && c.kind == LandContactKind::terrain && c.installed;
        });
        if (contact == saved.contacts.end()) return std::nullopt;
    }
    result.state_ = std::move(saved);
    result.proposals_.resize(result.state_.members.size());
    result.neighbours_.resize(result.state_.members.size());
    result.neighbour_candidates_.resize(result.state_.members.size());
    result.avoid_peers_.resize(result.state_.members.size());
    result.soft_candidates_.resize(result.state_.members.size());
    result.soft_index_.resize(result.state_.members.size());
    result.member_visits_.resize(result.state_.members.size());
    result.pair_visits_.resize(result.state_.members.size());
    result.squad_visits_.resize(result.state_.squads.size());
    result.member_rank_.resize(result.state_.members.size());
    result.squad_proposals_.resize(result.state_.squads.size());
    result.member_order_.resize(result.state_.members.size());
    result.squad_members_.resize(result.state_.squads.size());
    result.discovered_.resize(result.state_.members.size());
    result.discovery_valid_.resize(result.state_.members.size());
    result.pair_blocked_.resize(result.state_.members.size());
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
    for (std::size_t rank = 0; rank < result.member_order_.size(); ++rank) result.member_rank_[result.member_order_[rank]] = rank;
    return result;
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
        }
        if (squad.yielding_to == object || squad.yielding_to == squad_id) {
            squad.yielding_to = 0; squad.blocker_progress = 0;
            if (squad.order == LandMoveOrder::yielded) squad.order = LandMoveOrder::recovering;
        }
    }
    std::erase_if(state_.contacts, [object](const auto& row) { return row.first == object || row.second == object; });
    state_.members.erase(member);
    if (std::none_of(state_.members.begin(), state_.members.end(), [squad_index](const auto& row) { return row.squad == squad_index; })) {
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
        for (const auto member : squad_members_[index]) {
            state_.members[member].collision_lock = 0;
            state_.members[member].temporary_next = 0;
            state_.members[member].detour_path.nodes.clear(); state_.members[member].detour_next = 0;
        }
    };
    for (auto& contact : state_.contacts) {
        bool addressed = false;
        for (const auto member : squad_members_[squad]) {
            const auto id = state_.members[member].object;
            addressed |= contact.first == id || contact.second == id;
        }
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

bool LandMovement::install_path(std::size_t index, LandPath path, std::uint64_t start) {
    if (index >= state_.squads.size() || path.status != LandPathStatus::found || path.nodes.empty()
        || path.nodes.front() != state_.squads[index].position) return false;
    auto& s = state_.squads[index];
    const bool was_moving = active(s.order);
    invalidate_contacts(index);
    s.path = std::move(path); s.next_node = 1; s.following_start = start;
    s.order = LandMoveOrder::following; s.yielding_to = 0; s.blocker_progress = 0;
    // WLPA-68: a path accepted from a non-moving order first turns the squad in place; a squad already
    // moving keeps rotating while it translates. A squad with no positive turn rate never enters the state.
    bool can_turn = !squad_members_[index].empty();
    for (const auto member : squad_members_[index]) can_turn = can_turn && state_.members[member].profile.turn_rate > zero;
    s.turning = !s.generic_team && can_turn && !was_moving;
    if (s.generic_team) for (const auto member : squad_members_[index]) state_.members[member].next_node = 1;
    return true;
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
    const bool defer_occupancy) {
    const auto overflow = [] { return core::Result<void>::failure(core::Diagnostic{
        std::string(math::diagnostic_codes::overflow), core::Severity::error,
        "Land movement arithmetic is outside fixed-point range.", {}, {}, {}, {}}); };
    if (seconds <= zero) return overflow();
    // Reuse retained neighbour rows between queries as member-local repulsion peers.
    // This ordered metadata pass visits each contact once; proposal workers do not
    // scan unrelated contacts. Later neighbour queries overwrite the rows only after
    // the member-proposal barrier. Capacity is retained at each row's high-water mark.
    if (straggler_repulsion_) for (auto& peers : neighbour_candidates_) peers.clear();
    const auto member_index = [&](EntityId id) {
        const auto found = std::lower_bound(member_order_.begin(), member_order_.end(), id,
            [this](auto index, auto value) { return state_.members[index].object < value; });
        return found != member_order_.end() && state_.members[*found].object == id ? *found : state_.members.size();
    };
    if (straggler_repulsion_) for (const auto& contact : state_.contacts) {
        if (contact.state != LandContactState::repulsion || contact.kind != LandContactKind::moving_stopped) continue;
        const auto first = member_index(contact.first), second = member_index(contact.second);
        if (first == state_.members.size() || second == state_.members.size()) continue;
        neighbour_candidates_[first].push_back(second);
        neighbour_candidates_[second].push_back(first);
    }
    // Contact adjacency (CSR over member indices): the proposal phases and the pair guard visit only the rows that
    // touch a member instead of every retained row. Built here, in the ordered pre-pass, from the committed rows.
    {
        const auto members = state_.members.size();
        adjacency_offset_.assign(members + 1, 0);
        for (const auto& contact : state_.contacts) {
            const auto first = member_index(contact.first);
            if (first < members) ++adjacency_offset_[first + 1];
            if (contact.second) {
                const auto second = member_index(contact.second);
                if (second < members) ++adjacency_offset_[second + 1];
            }
        }
        for (std::size_t i = 0; i < members; ++i) adjacency_offset_[i + 1] += adjacency_offset_[i];
        adjacency_row_.resize(adjacency_offset_[members]);
        adjacency_cursor_.assign(adjacency_offset_.begin(), adjacency_offset_.end() - 1);
        for (std::size_t row = 0; row < state_.contacts.size(); ++row) {
            const auto& contact = state_.contacts[row];
            const auto first = member_index(contact.first);
            if (first < members) adjacency_row_[adjacency_cursor_[first]++] = static_cast<std::uint32_t>(row);
            if (contact.second) {
                const auto second = member_index(contact.second);
                if (second < members) adjacency_row_[adjacency_cursor_[second]++] = static_cast<std::uint32_t>(row);
            }
        }
    }
    // The two barriers use the same named phase. Squads produce immutable parent inputs first;
    // members then propose into disjoint slots, independently of worker traversal order.
    struct Context { LandOccupancy& grid; Fixed seconds; bool soft_peers{}, soft_indexed{}; } context{grid, seconds};
    auto run = executor.execute_phase("land-member-proposals", tick_partition_count, [this, &context](std::size_t partition) {
        const auto range = partition_range(partition, state_.squads.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto& s = state_.squads[i];
            auto& p = squad_proposals_[i];
            squad_visits_[i] = 0;
            p = {true, s.position, s.facing, zero, s.next_node, s.progress};
            p.temporary_next = s.temporary_next;
            p.turning = s.turning;
            if (!active(s.order) || state_.tick < s.following_start) continue;
            // LC-83: start admission combines member gates. No parent displacement or
            // type-speed cap is integrated for this dispatch; members own ordinary motion.
            if (s.generic_team) {
                for (const auto member : squad_members_[i]) {
                    const auto& m = state_.members[member];
                    if ((m.state == LandWalkState::stopped || m.state == LandWalkState::starting)
                        && (m.gates.animation_prevents_movement
                            || (m.profile.walk_transition && !m.gates.start_almost_finished))) p.start_admitted = false;
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
                        p.valid = a.valid;
                        continue;
                    }
                    p.facing = *target;
                }
                p.turning = false;
                p.valid = a.valid;
                continue;
            }
            auto speed = s.maximum_speed;
            // Only rows that touch a member of this squad can match, so visit the members' adjacency lists.
            for (const auto index : squad_members_[i]) {
                const auto id = state_.members[index].object;
                for (auto k = adjacency_offset_[index]; k < adjacency_offset_[index + 1]; ++k) {
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
            // WLPA-30: slow for the largest excess lag beyond rear extent + code 2.
            for (const auto member : squad_members_[i]) {
                const auto& m = state_.members[member];
                const auto lag = a.sub(zero, dot(a, a.sub(m.position, s.position), axis));
                const auto excess = a.sub(lag, a.add(s.rear_extent, Fixed::from_raw(2 * Fixed::scale)));
                const auto factor = a.sub(one, std::clamp(a.div(excess, ten), zero, one));
                speed = std::min(speed, a.mul(s.maximum_speed, factor));
                // LR13-17/19/21: no parent runs away from a member awaiting an admitted gate.
                if (m.gates.animation_prevents_movement || m.state == LandWalkState::deploying
                    || m.state == LandWalkState::deployed || m.state == LandWalkState::undeploying
                    || (m.profile.walk_transition && !m.gates.start_almost_finished
                        && (m.state == LandWalkState::stopped || m.state == LandWalkState::starting))) speed = zero;
            }
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
                p.facing = heading.value();
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
            if (p.position != s.position) { p.speed = speed; ++p.progress; }
        }
    });
    if (!run) return run;
    for (const auto& p : squad_proposals_) if (!p.valid) return overflow();
    // WLPA-83: moving Infantry/Bike pairs of different squads are stateless soft pairs; the member steering finds its
    // moving soft peers from this index of the committed positions (reach 1.3 x the body radius, WLPA-59).
    context.soft_peers = straggler_repulsion_;
    if (straggler_repulsion_ && !reference_pair_scans_) {
        core::tick_profile::Scope phase("land-movement-index");
        run = executor.execute_phase("land-member-proposals", tick_partition_count, [this](std::size_t partition) {
            const auto range = partition_range(partition, member_order_.size());
            for (auto rank = range.begin; rank < range.end; ++rank) {
                const auto& m = state_.members[member_order_[rank]];
                Arithmetic a;
                const auto reach = a.mul(m.profile.radius, Fixed::from_ratio(13, 10).value());
                soft_index_.bodies[rank] = {m.position, m.position, reach, true, a.valid};
            }
        });
        if (!run) return run;
        core::tick_profile::Scope build("land-movement-index-build");
        soft_index_.build(grid.terrain().spacing());
        context.soft_indexed = soft_index_.safe();
    }
    run = executor.execute_phase("land-member-proposals", tick_partition_count, [this, &context](std::size_t partition) {
        const auto range = partition_range(partition, state_.members.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto& m = state_.members[i];
            const auto& s = state_.squads[m.squad];
            const auto& parent = squad_proposals_[m.squad];
            auto& p = proposals_[i];
            p = {true, false, m.position, m.facing, m.speed, m.stop_velocity, m.state};
            p.terrain_blocked = parent.terrain_blocked;
            p.terrain_target = parent.terrain_target;
            // WLPA-69: a member's own refused step stays 'own' while its terrain contact is live, so the late
            // service still plans the member detour after the member stopped re-testing the blocked cell.
            member_visits_[i] = 0;
            if (m.own_terrain_block) {
                bool live = false;
                for (auto k = adjacency_offset_[i]; k < adjacency_offset_[i + 1] && !live; ++k) {
                    ++member_visits_[i];
                    const auto& c = state_.contacts[adjacency_row_[k]];
                    live = c.first == m.object && c.second == 0 && c.kind == LandContactKind::terrain && c.state != LandContactState::resolved;
                }
                p.own_terrain_block = live;
            } else p.own_terrain_block = false;
            p.next_node = m.next_node; p.temporary_next = m.temporary_next; p.detour_next = m.detour_next;
            Arithmetic a;
            const auto axis = forward(parent.facing);
            Vec2 target;
            bool detouring = false;
            if (s.generic_team) {
                const auto& route = s.temporary_first ? s.temporary_path : s.path;
                auto& tracker = s.temporary_first ? p.temporary_next : p.next_node;
                while (tracker < route.nodes.size() && route.nodes[tracker] == m.position) ++tracker;
                target = tracker < route.nodes.size() ? route.nodes[tracker] : m.position;
            } else {
                target = a.add(parent.position, a.add(a.mul(axis, m.slot.x), a.mul(side(axis), m.slot.y)));
                // WLPA-69: while its own terrain detour is installed the member walks that path to the
                // end, then resumes slot following. The parent route and WLPA-30 stay untouched.
                const auto& detour = m.detour_path.nodes;
                while (p.detour_next < detour.size() && detour[p.detour_next] == m.position) ++p.detour_next;
                if (p.detour_next < detour.size()) { target = detour[p.detour_next]; detouring = true; }
            }
            const auto error = a.sub(target, m.position);
            const auto distance = a.length(error);
            auto direction = axis;
            // LR3-15 lateral correction, capped at 0.5. At parent stop, finish passive slot recovery.
            if ((s.generic_team || detouring || parent.speed == zero) && distance > zero) direction = a.mul(error, a.div(one, distance));
            else {
                const auto lateral = dot(a, error, side(axis));
                const auto blend = std::clamp(a.div(lateral, ten), Fixed::from_raw(-Fixed::scale / 2), Fixed::from_raw(Fixed::scale / 2));
                direction = a.add(axis, a.mul(side(axis), blend));
            }
            // WLPA-58/59: member-local avoidance remains available while the parent
            // waits for lagging slots. Read immutable contacts and write only this proposal.
            auto& repulsion_peers = avoid_peers_[i];
            repulsion_peers.clear();
            if (!s.generic_team && straggler_repulsion_ && active(s.order) && distance > zero) {
                // Retained idle peers (WLPA-57/60/81), then the moving soft peers of other squads (WLPA-83).
                for (const auto peer : neighbour_candidates_[i]) repulsion_peers.push_back(peer);
                if (context.soft_peers && land_soft_collision_class(m.profile.collision_class)) {
                    const auto consider = [&](std::size_t j) {
                        if (j == i) return;
                        const auto& other = state_.members[j];
                        if (other.squad == m.squad || !land_soft_collision_class(other.profile.collision_class)) return;
                        const auto& other_squad = state_.squads[other.squad];
                        if (other_squad.generic_team || !active(other_squad.order)) return;
                        repulsion_peers.push_back(j);
                    };
                    if (context.soft_indexed) {
                        soft_index_.query(member_rank_[i], soft_candidates_[i]);
                        for (const auto rank : soft_candidates_[i]) consider(member_order_[rank]);
                    } else for (const auto j : member_order_) consider(j);
                }
            }
            if (!repulsion_peers.empty()) {
                const auto length = a.length(direction);
                if (length > zero) direction = a.mul(direction, a.div(one, length));
                const auto requested = parent.speed == zero || detouring ? std::min(m.profile.maximum_speed, a.div(distance, context.seconds))
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
            bool allowed = active(s.order) && state_.tick >= s.following_start;
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
                if (m.gates.start_almost_finished) p.state = LandWalkState::moving;
                else allowed = false;
            }
            if (p.state == LandWalkState::turning && !m.gates.turn_almost_finished) allowed = false;
            Fixed desired;
            if (allowed && distance > zero) {
                desired = (s.generic_team || detouring || parent.speed == zero) ? std::min(m.profile.maximum_speed, a.div(distance, context.seconds))
                    : land_member_desired_speed(parent.speed, m.profile.maximum_speed, dot(a, error, axis), turning);
                if (turning) desired = zero;
            }
            {
                // The rows that touch this member, or (non-generic squads) any member of its squad: every other row
                // fails the identity test in the original full scan. follower/offset rows and generic teams act on
                // the member alone.
                const auto scan = [&](std::size_t holder) {
                    const auto id = state_.members[holder].object;
                    for (auto k = adjacency_offset_[holder]; k < adjacency_offset_[holder + 1]; ++k) {
                        ++member_visits_[i];
                        const auto& contact = state_.contacts[adjacency_row_[k]];
                        if (contact.state == LandContactState::resolved) continue;
                        if ((contact.state == LandContactState::follower_speed || contact.state == LandContactState::offset_exchange)
                            && holder != i) continue;
                        if ((id == contact.first && contact.first_stopped) || (id == contact.second && contact.second_stopped)) desired = zero;
                        if (id == contact.first && contact.first_speed_limit) desired = std::min(desired, *contact.first_speed_limit);
                        if (id == contact.second && contact.second_speed_limit) desired = std::min(desired, *contact.second_speed_limit);
                    }
                };
                if (s.generic_team) scan(i);
                else for (const auto holder : squad_members_[m.squad]) scan(holder);
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
                if (a.valid && admitted(context.grid, m.position, next, m.profile.radius, m.mask)) p.position = next;
                else { p.speed = zero; p.state = LandWalkState::stopping; p.terrain_blocked = true; p.own_terrain_block = true; p.terrain_target = next; }
            }
            p.valid = a.valid;
            p.at_slot = !detouring && p.position == target;
            if (detouring && p.position == target) ++p.detour_next;
            if (s.generic_team && p.at_slot) {
                const auto& route = s.temporary_first ? s.temporary_path : s.path;
                auto& tracker = s.temporary_first ? p.temporary_next : p.next_node;
                if (tracker < route.nodes.size()) ++tracker;
                p.at_slot = tracker == route.nodes.size();
            }
        }
    });
    if (!run) return run;
    for (const auto& p : proposals_) if (!p.valid) return overflow();
    if (!reference_pair_scans_) {
        core::tick_profile::Scope phase("land-movement-index");
        run = executor.execute_phase("land-member-proposals", tick_partition_count, [this](std::size_t partition) {
            const auto range = partition_range(partition, member_order_.size());
            for (auto rank = range.begin; rank < range.end; ++rank) {
                const auto i = member_order_[rank];
                neighbours_.bodies[rank] = {state_.members[i].position, proposals_[i].position, state_.members[i].profile.radius};
            }
        });
        if (!run) return run;
        core::tick_profile::Scope build("land-movement-index-build");
        neighbours_.build(grid.terrain().spacing());
    }
    const bool indexed = !reference_pair_scans_ && neighbours_.safe();
    // Swept pair checks read immutable proposals and write disjoint flags. The subsequent ordered
    // reduction stops blocked parents without cross-member writes from worker threads.
    run = executor.execute_phase("land-member-proposals", tick_partition_count, [this, indexed](std::size_t partition) {
        const auto range = partition_range(partition, member_order_.size());
        for (auto rank = range.begin; rank < range.end; ++rank) {
            const auto i = member_order_[rank];
            const auto& m = state_.members[i];
            const auto& p = proposals_[i];
            pair_blocked_[i] = 0;
            pair_visits_[i] = 0;
            if (m.profile.radius <= zero) continue;
            Arithmetic a;
            auto& candidates = neighbour_candidates_[i];
            if (indexed) neighbours_.query(rank, candidates);
            const auto count = indexed ? candidates.size() : member_order_.size();
            pair_visits_[i] = static_cast<std::uint32_t>(count);
            for (std::size_t candidate = 0; candidate < count; ++candidate) {
                const auto j = member_order_[indexed ? candidates[candidate] : candidate];
                const auto& other = state_.members[j];
                if (m.squad == other.squad || other.profile.radius <= zero) continue;
                const auto radius = a.add(m.profile.radius, other.profile.radius);
                const auto delta = a.sub(a.sub(p.position, m.position),
                    a.sub(proposals_[j].position, other.position));
                const auto offset = a.sub(m.position, other.position);
                const auto length_squared = dot(a, delta, delta);
                const auto t = length_squared > zero
                    ? std::clamp(a.div(a.sub(zero, dot(a, offset, delta)), length_squared), zero, one) : zero;
                const auto closest = a.length(a.add(offset, a.mul(delta, t)));
                if (closest < radius && p.position != m.position) {
                    // WLPA-83: two moving Infantry/Bike members of different squads with opposed or crossing headings
                    // never veto each other; the member steering (WLPA-59) separates them. Stateless: no contact row.
                    // Same-direction pairs keep the hard veto.
                    if (land_soft_opposed(m, state_.squads[m.squad], other, state_.squads[other.squad], straggler_repulsion_)) continue;
                    // WLPA-60/82: retained stopped-infantry repulsion is a soft pair. Eligibility is the authored
                    // collision class of BOTH members (Infantry or Bike), never the land object kind.
                    // Retail member displacement does not apply this extra hard-disc
                    // veto. Keep terrain admission and every other pair policy intact.
                    // Consult canonical contacts only for an otherwise blocked proposal.
                    if (straggler_repulsion_ && active(state_.squads[m.squad].order)
                        && !active(state_.squads[other.squad].order)
                        && state_.squads[other.squad].order != LandMoveOrder::yielded
                        && land_soft_collision_class(m.profile.collision_class)
                        && land_soft_collision_class(other.profile.collision_class)) {
                        // WLPA-81: the retained row is only a witness that this member is in straggler
                        // repulsion; a second idle peer inside the hard disc is soft as well, because the
                        // retained set need not list every adjacent peer (#4840 Dantooine member 1103).
                        bool repulsed = false;
                        for (auto k = adjacency_offset_[i]; k < adjacency_offset_[i + 1] && !repulsed; ++k) {
                            const auto& entry = state_.contacts[adjacency_row_[k]];
                            repulsed = entry.kind == LandContactKind::moving_stopped && entry.state == LandContactState::repulsion;
                        }
                        if (repulsed) continue;
                    }
                    // Existing overlap may separate, but never deepen.
                    if (a.length(offset) < radius
                        && a.length(a.sub(p.position, other.position)) > a.length(offset)) continue;
                    pair_blocked_[i] = 1; break;
                }
            }
            if (!a.valid) pair_blocked_[i] = 2;
        }
    });
    if (!run) return run;
    for (const auto flag : pair_blocked_) if (flag == 2) return overflow();
    for (const auto i : member_order_) if (pair_blocked_[i]) {
        auto& p = proposals_[i]; const auto& m = state_.members[i];
        p.position = m.position; p.speed = zero; p.state = LandWalkState::stopping;
        if (state_.squads[m.squad].generic_team) {
            p.next_node = m.next_node; p.temporary_next = m.temporary_next; p.at_slot = false;
            continue;
        }
        p.detour_next = m.detour_next;
        auto& parent = squad_proposals_[m.squad]; const auto& old = state_.squads[m.squad];
        parent.position = old.position; parent.speed = zero; parent.progress = old.progress;
        parent.next_node = old.next_node; parent.temporary_next = old.temporary_next;
    }
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
        if (m.position != p.position) ++m.progress;
        m.position = p.position; m.facing = p.facing; m.speed = p.speed;
        m.stop_velocity = p.stop_velocity; m.state = p.state;
        m.assigned_speed = p.assigned_speed; m.collision_speed = p.speed; m.terrain_blocked = p.terrain_blocked;
        m.terrain_target = p.terrain_target; m.own_terrain_block = p.own_terrain_block;
        m.next_node = p.next_node; m.temporary_next = p.temporary_next; m.detour_next = p.detour_next;
    }
    for (std::size_t i = 0; i < state_.squads.size(); ++i) {
        auto& s = state_.squads[i]; const auto& p = squad_proposals_[i];
        s.position = p.position; s.facing = p.facing; s.speed = p.speed;
        s.next_node = p.next_node; s.progress = p.progress;
        s.temporary_next = p.temporary_next; s.turning = p.turning;
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
        if (active(s.order) && s.next_node == s.path.nodes.size()) {
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
