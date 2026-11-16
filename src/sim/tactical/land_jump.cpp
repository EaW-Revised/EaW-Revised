#include "session_impl.hpp"
#include "session_land_formation.hpp"
#include "session_land_garrison.hpp"
#include "tactical_internal.hpp"
#include "eawr/sim/tactical/land_placement.hpp"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

// The land jet-pack flight (WLAB-16, #7084; docs/behaviour/walks/land-abilities.md). The ability part (the point rule, the
// recharge, the pending request) is the land ability effect of #3239; this file is its consumer. A jump is the squad's, not one
// member's: the order reaches every member, and the squad lifts off when each of its live members waits for it.
//
//   phase 1  the request waits the type's take-off wait, standing where it is (the squad is stopped);
//   lift-off the squad's movement body is moved at once to the landing point (the point, or the nearest legal position when
//            the ground there is not open: the original lands on the nearest walkable ground), keeping its formation offsets;
//   phase 2  each member flies level toward its body's landing position: the planar speed rises at the member's own walking
//            acceleration to the cruise speed (Alternate_Max_Speed), the height follows the ground at Hover_Offset and moves at
//            the lift rate (Max_Lift); nothing on the ground stops it;
//   phase 3  at the landing position the planar motion stops and the height comes down at the lift rate; then the member is an
//            ordinary walker again on its primary locomotor.
// The service runs after the land publication and before combat (session_land.cpp): the flying pose it writes is the one combat and
// fog read this tick. Serial, ascending unit id, fixed point only.
namespace eawr::sim::tactical {
namespace session_detail {

namespace {

[[nodiscard]] const LandSessionContent::Jump* jump_row(const LandSession& land, const TypeId type) {
    const auto& rows = land.content.jumps;
    const auto found = std::lower_bound(rows.begin(), rows.end(), type,
        [](const LandSessionContent::Jump& row, const TypeId key) { return row.type < key; });
    return found != rows.end() && found->type == type ? &*found : nullptr;
}

[[nodiscard]] const LandSessionContent::Jump* jump_row_of(const LandSession& land, const EntityId unit) {
    const auto bound = land.bindings.find(unit);
    return bound != land.bindings.end() ? jump_row(land, bound->second.first) : nullptr;
}

// The motion record of a member goes back to walking: no jump, the primary locomotor.
void end_jump(LandSession& land, const EntityId unit) {
    const auto found = land.ability_motion.find(unit);
    if (found == land.ability_motion.end()) return;
    auto& record = found->second;
    record.jump_phase = 0; record.jump_frames = 0; record.jump_speed = math::Fixed{};
    record.jump_point = math::Vec3{}; record.jump_position = math::Vec3{};
    record.locomotor = 0;
    if (record.idle()) land.ability_motion.erase(found);
}

// The squad stops where it stands: it leaves any formation or search and takes the stop order (as DEPLOY does).
void stop_jumping_squad(LandSession& land, std::vector<LiveUnit>& live, const EntityId squad_id, const std::size_t index,
    const std::uint64_t tick) {
    release_land_formation_member(land, squad_id);
    land.searches.erase(squad_id);
    static_cast<void>(land.movement.drop_order(index));
    static_cast<void>(land.movement.set_formation(index, 0, {}));
    auto* squad = land_live_unit(live, squad_id);
    if (squad == nullptr) return;
    squad->state.order = Order{};
    squad->state.order.issued_tick = tick;
    squad->state.order.kind = OrderKind::stop;
    for (const auto& member : land.movement.members()) {
        if (member.squad != index) continue;
        if (auto* child = land_live_unit(live, member.object)) child->state.order = squad->state.order;
    }
}

// Moves the squad's movement body to the landing position and starts the flight of its members. False: nothing was started and
// the squad stands as it did (the caller drops the request).
[[nodiscard]] bool begin_flight(LandSession& land, std::vector<LiveUnit>& live, const EntityId squad_id, const std::uint64_t tick) {
    const auto squads = land.movement.squads();
    const auto at = std::find_if(squads.begin(), squads.end(), [&](const LandMovingSquad& row) { return row.object == squad_id; });
    const auto request = land.requests.find(squad_id);
    if (at == squads.end() || request == land.requests.end()) return false;
    const auto index = static_cast<std::size_t>(at - squads.begin());
    const auto from = at->position;
    std::vector<EntityId> ids;
    for (const auto& member : land.movement.members()) if (member.squad == index) ids.push_back(member.object);
    std::sort(ids.begin(), ids.end());
    if (ids.empty()) return false;
    const auto lead = land.ability_motion.find(ids.front());
    if (lead == land.ability_motion.end()) return false;
    const auto point = lead->second.jump_point;
    std::vector<math::Vec3> starts;
    for (const auto id : ids) {
        const auto* unit = land_live_unit(live, id);
        if (unit == nullptr) return false;
        starts.push_back(unit->state.position);
    }
    LandPlacementQuery query;
    query.mask = request->second.mask; query.from = from; query.desired = {point.x, point.y}; query.radius = request->second.radius;
    const auto placement = land_find_open_position(land.occupancy, query);
    if (placement.status == LandPlacementStatus::none) return false;
    stop_jumping_squad(land, live, squad_id, index, tick);
    auto detached = land.movement.detach_squad(squad_id);
    if (!detached) return false;
    auto spare = *detached;
    if (!land.movement.restore_squad(std::move(*detached), placement.position)) {
        // The squad must not vanish: it goes back where it stood.
        static_cast<void>(land.movement.restore_squad(std::move(spare), from));
        return false;
    }
    request->second.start = placement.position;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        auto& record = land.ability_motion[ids[i]];
        record.jump_phase = 2; record.jump_frames = 0; record.jump_speed = math::Fixed{};
        record.jump_point = math::Vec3{}; record.jump_position = starts[i];
    }
    return true;
}

// `value` moves toward `target` by at most `step`.
[[nodiscard]] math::Fixed approach_height(const math::Fixed value, const math::Fixed target, const math::Fixed step) {
    math::Fixed moved;
    if (value < target) return math::try_add(value, step, moved) && moved < target ? moved : target;
    return math::try_subtract(value, step, moved) && target < moved ? moved : target;
}

} // namespace

// Whether a member of the squad is jumping: it has a flight row and is waiting for the lift-off, flying or coming down.
bool land_squad_jumping(const LandSession& land, const std::size_t squad) {
    if (land.ability_motion.empty()) return false;
    for (const auto& member : land.movement.members()) {
        if (member.squad != squad) continue;
        const auto found = land.ability_motion.find(member.object);
        if (found == land.ability_motion.end() || found->second.jump_phase == 0) continue;
        if (found->second.jump_phase >= 2 || jump_row_of(land, member.object) != nullptr) return true;
    }
    return false;
}

} // namespace session_detail

void TacticalSession::Impl::land_jump_service(std::vector<LiveUnit>& live) {
    using namespace session_detail;
    auto& state = *land;
    auto& records = state.ability_motion;
    if (records.empty() || state.content.jumps.empty()) return;
    const auto tick = completed_tick;
    // A unit that died while flying leaves its record.
    for (auto record = records.begin(); record != records.end();) {
        if (record->second.jump_phase >= 2 && land_live_unit(live, record->first) == nullptr) record = records.erase(record);
        else ++record;
    }
    // Phase 1: count the wait; a squad whose live members all wait the whole time lifts off, a squad with a member that does not
    // wait drops the request (a part of a squad cannot lift off).
    for (auto& [id, record] : records) {
        if (record.jump_phase == 1 && jump_row_of(state, id) != nullptr) ++record.jump_frames;
    }
    std::vector<EntityId> lifting, dropped;
    {
        const auto squads = state.movement.squads();
        const auto members = state.movement.members();
        for (std::size_t index = 0; index < squads.size(); ++index) {
            std::size_t total = 0, waiting = 0, ready = 0;
            std::vector<EntityId> pending;
            for (const auto& member : members) {
                if (member.squad != index) continue;
                ++total;
                const auto found = records.find(member.object);
                const auto* row = jump_row_of(state, member.object);
                if (found == records.end() || found->second.jump_phase != 1 || row == nullptr) continue;
                ++waiting; pending.push_back(member.object);
                if (found->second.jump_frames >= row->takeoff_frames) ++ready;
            }
            if (waiting == 0) continue;
            if (waiting != total) dropped.insert(dropped.end(), pending.begin(), pending.end());
            else if (ready == total) lifting.push_back(squads[index].object);
        }
    }
    for (const auto id : dropped) end_jump(state, id);
    std::sort(lifting.begin(), lifting.end());
    for (const auto squad_id : lifting) {
        std::vector<EntityId> ids;
        {
            const auto squads = state.movement.squads();
            const auto at = std::find_if(squads.begin(), squads.end(), [&](const LandMovingSquad& row) { return row.object == squad_id; });
            if (at == squads.end()) continue;
            const auto index = static_cast<std::size_t>(at - squads.begin());
            for (const auto& member : state.movement.members()) if (member.squad == index) ids.push_back(member.object);
        }
        if (!begin_flight(state, live, squad_id, tick)) for (const auto id : ids) end_jump(state, id);
    }
    // Phases 2 and 3: every flying member, ascending id.
    const auto dt = math::Fixed::from_ratio(1, logical_frames_per_second).value();
    const auto& terrain = state.occupancy.terrain();
    const auto members = state.movement.members();
    std::vector<EntityId> landed;
    for (auto& [id, record] : records) {
        if (record.jump_phase < 2) continue;
        auto* unit = land_live_unit(live, id);
        const auto* row = jump_row_of(state, id);
        const auto member = std::lower_bound(members.begin(), members.end(), id,
            [](const LandMovingMember& entry, const EntityId key) { return entry.object < key; });
        if (unit == nullptr || row == nullptr || member == members.end() || member->object != id) { landed.push_back(id); continue; }
        math::Fixed cruise, lift, step_up, accelerated;
        if (!math::try_multiply(member->profile.maximum_speed, row->cruise_ratio, cruise)
            || !math::try_multiply(member->profile.maximum_speed, row->lift_ratio, lift)
            || !math::try_multiply(lift, dt, step_up)) { landed.push_back(id); continue; }
        auto position = record.jump_position;
        const auto ground = [&](const math::Fixed x, const math::Fixed y) { return terrain.height_at(x, y).value_or(math::Fixed{}); };
        if (record.jump_phase == 2) {
            math::Fixed dx, dy, distance, advance, hover_height;
            if (!math::try_subtract(member->position.x, position.x, dx) || !math::try_subtract(member->position.y, position.y, dy)
                || !math::try_length(math::Vec2{dx, dy}, distance)) { landed.push_back(id); continue; }
            // The planar speed rises at the member's own acceleration (a member with none flies at the cruise speed at once).
            if (member->profile.acceleration > math::Fixed{}
                && math::try_multiply(member->profile.acceleration, dt, accelerated) && math::try_add(record.jump_speed, accelerated, accelerated))
                record.jump_speed = std::min(cruise, accelerated);
            else record.jump_speed = cruise;
            if (!math::try_multiply(record.jump_speed, dt, advance)) { landed.push_back(id); continue; }
            if (!(advance < distance)) {
                position.x = member->position.x; position.y = member->position.y;
                record.jump_speed = math::Fixed{};
                record.jump_phase = 3;
            } else {
                math::Fixed ux, uy, mx, my;
                if (!math::try_divide(dx, distance, ux) || !math::try_divide(dy, distance, uy)
                    || !math::try_multiply(ux, advance, mx) || !math::try_multiply(uy, advance, my)
                    || !math::try_add(position.x, mx, position.x) || !math::try_add(position.y, my, position.y)) { landed.push_back(id); continue; }
            }
            if (!math::try_add(ground(position.x, position.y), row->hover, hover_height)) { landed.push_back(id); continue; }
            position.z = approach_height(position.z, record.jump_phase == 3 ? ground(position.x, position.y) : hover_height, step_up);
        } else {
            position.z = approach_height(position.z, ground(position.x, position.y), step_up);
        }
        record.jump_position = position;
        unit->state.position = position;
        if (record.jump_phase == 3 && position.z == ground(position.x, position.y)) landed.push_back(id);
    }
    for (const auto id : landed) end_jump(state, id);
}

} // namespace eawr::sim::tactical
