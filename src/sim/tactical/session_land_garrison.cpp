#include "session_land_garrison.hpp"
#include "session_impl.hpp"
#include "session_land_formation.hpp"
#include "session_tick.hpp"
#include "tactical_internal.hpp"
#include "eawr/core/tick_profile.hpp"
#include "eawr/sim/math/trig.hpp"

#include <algorithm>

namespace eawr::sim::tactical::session_detail {
namespace {
using math::Fixed;
using math::Vec2;

// The host's pose in the plane: position, heading in turns and the forward unit vector (cosine, sine).
struct Pose {
    Vec2 position;
    Fixed turns;
    math::SinCos direction;
};

std::optional<Pose> state_pose(const UnitState& state) {
    const auto yaw = yaw_degrees(state.rotation);
    if (!yaw) return std::nullopt;
    const auto turns = math::divide(yaw.value(), Fixed::from_raw(360 * Fixed::scale));
    if (!turns) return std::nullopt;
    return Pose{{state.position.x, state.position.y}, turns.value(), math::sin_cos_turn(turns.value())};
}

Pose saved_pose(const LandGarrisonEntry& entry) {
    return Pose{entry.host_position, entry.host_facing, math::sin_cos_turn(entry.host_facing)};
}

bool offset_point(const Vec2 from, const Vec2 direction, const Fixed distance, Vec2& result) {
    Fixed dx, dy;
    return math::try_multiply(direction.x, distance, dx) && math::try_multiply(direction.y, distance, dy)
        && math::try_add(from.x, dx, result.x) && math::try_add(from.y, dy, result.y);
}

// LG-BONE: a bone point is the model-space rest pose scaled, turned a quarter turn into the plane and rotated by the host
// heading; the animated skeleton is not consulted.
std::optional<Vec2> bone_point(const units::LandGarrisonData& data, const Pose& pose, const std::uint32_t index) {
    if (index >= data.bones.size() || !data.bones[index].position) return std::nullopt;
    const auto& model = *data.bones[index].position;
    Fixed scaled_x, scaled_y, first, second, east, north, world_x, world_y;
    if (!math::try_multiply(model.x, data.scale, scaled_x) || !math::try_multiply(model.y, data.scale, scaled_y))
        return std::nullopt;
    const Fixed turned_x = Fixed::from_raw(-scaled_y.raw()), turned_y = scaled_x;
    if (!math::try_multiply(turned_x, pose.direction.cosine, first) || !math::try_multiply(turned_y, pose.direction.sine, second)
        || !math::try_subtract(first, second, east) || !math::try_add(pose.position.x, east, world_x)
        || !math::try_multiply(turned_x, pose.direction.sine, first) || !math::try_multiply(turned_y, pose.direction.cosine, second)
        || !math::try_add(first, second, north) || !math::try_add(pose.position.y, north, world_y)) return std::nullopt;
    return Vec2{world_x, world_y};
}

// The unit vector from `from` to `to`, or nullopt when they coincide or the arithmetic overflows.
std::optional<Vec2> unit_toward(const Vec2 from, const Vec2 to) {
    Vec2 delta, direction;
    Fixed length;
    if (!math::try_subtract(to.x, from.x, delta.x) || !math::try_subtract(to.y, from.y, delta.y)
        || !math::try_length(delta, length) || length <= Fixed{}) return std::nullopt;
    if (!math::try_divide(delta.x, length, direction.x) || !math::try_divide(delta.y, length, direction.y)) return std::nullopt;
    return direction;
}

// G-04: the entrance point is the chosen host bone in the world, pushed out from the host centre by the entrance distance
// when it is positive; a host without that bone is entered at its centre.
std::optional<Vec2> entrance_point(const units::LandGarrisonData& data, const Pose& pose, const std::uint32_t bone) {
    const auto point = bone_point(data, pose, bone);
    if (!point) return pose.position;
    if (data.enter_distance <= Fixed{}) return point;
    const auto outward = unit_toward(pose.position, *point);
    if (!outward) return point;
    Vec2 pushed;
    if (!offset_point(*point, *outward, data.enter_distance, pushed)) return std::nullopt;
    return pushed;
}

// G-07: the exit starts at the host centre or at the occupant's own bone and runs along the host heading (or from the centre
// to that bone) by the exit distance, the host extent length when none is authored.
struct ExitPlan {
    Vec2 start, destination;
};
std::optional<ExitPlan> exit_plan(const units::LandGarrisonData& data, const Pose& pose, const std::uint32_t bone,
    const Fixed extent) {
    Vec2 start = pose.position;
    Vec2 direction{pose.direction.cosine, pose.direction.sine};
    if (const auto point = bone_point(data, pose, bone)) {
        start = *point;
        if (const auto outward = unit_toward(pose.position, *point)) direction = *outward;
    }
    const Fixed distance = data.exit_distance > Fixed{} ? data.exit_distance : extent;
    ExitPlan plan;
    plan.start = start;
    if (!offset_point(start, direction, distance, plan.destination)) return std::nullopt;
    return plan;
}

Fixed host_extent(const CombatTable& table, const TypeId type) {
    const auto* footprint = table.footprint(type);
    Fixed length;
    if (footprint == nullptr || !math::try_length({footprint->x_extent, footprint->y_extent}, length)) return Fixed{};
    return length;
}

const LandMovingMember* find_member(const LandSession& land, const EntityId object) {
    const auto members = land.movement.members();
    const auto found = std::lower_bound(members.begin(), members.end(), object,
        [](const LandMovingMember& row, const EntityId id) { return row.object < id; });
    return found != members.end() && found->object == object ? &*found : nullptr;
}

// A host that is not a member of a moving squad (a structure) always stands still. A transport must be stopped or deployed
// before its occupants leave (G-07).
bool host_stationary(const LandSession& land, const EntityId host) {
    const auto* member = find_member(land, host);
    return member == nullptr || member->state == LandWalkState::stopped || member->state == LandWalkState::deployed;
}

bool walk_finished(const LandSession& land, const LandMovingSquad& squad) {
    if (land.searches.contains(squad.object)) return false;
    if (squad.formation != 0 && land.formations.contains(squad.formation)) return false;
    return squad.order == LandMoveOrder::idle || squad.order == LandMoveOrder::dropped
        || squad.order == LandMoveOrder::reached;
}

Fixed entrant_extent(const LandSession& land, const std::size_t squad_index) {
    Fixed largest;
    for (const auto& member : land.movement.members())
        if (member.squad == squad_index) largest = std::max(largest, member.profile.radius);
    return largest;
}

bool resident(const LandGarrisonEntry& entry) {
    return entry.phase == LandGarrisonPhase::inside || entry.phase == LandGarrisonPhase::waiting_exit;
}

// G-03: the slot cost of the host's committed occupants. Pending entrants are not counted.
std::uint64_t occupied_slots(const LandSession& land, const EntityId host) {
    std::uint64_t used{};
    for (const auto& pair : land.garrison_entries)
        if (pair.second.host == host && resident(pair.second)) used += pair.second.value;
    return used;
}

void order_live(std::vector<LiveUnit>& live, const EntityId object, const OrderKind kind, const std::uint64_t tick) {
    if (auto* unit = land_live_unit(live, object)) {
        unit->state.order = Order{};
        unit->state.order.kind = kind;
        unit->state.order.issued_tick = tick;
    }
}

// The squad leaves its walk: no pending search, formation or path (the same acts as a stop order).
void stop_squad(LandSession& land, std::vector<LiveUnit>& live, const EntityId squad_id, const std::uint64_t tick) {
    const auto squads = land.movement.squads();
    const auto found = std::find_if(squads.begin(), squads.end(),
        [squad_id](const LandMovingSquad& row) { return row.object == squad_id; });
    if (found == squads.end()) return;
    const auto index = static_cast<std::size_t>(found - squads.begin());
    release_land_formation_member(land, squad_id);
    land.searches.erase(squad_id);
    static_cast<void>(land.movement.drop_order(index));
    static_cast<void>(land.movement.set_formation(index, 0, {}));
    order_live(live, squad_id, OrderKind::stop, tick);
    for (const auto& member : land.movement.members())
        if (member.squad == index) order_live(live, member.object, OrderKind::stop, tick);
}

// G-04: a transport host stops moving before anything enters or leaves it.
void stop_host(LandSession& land, std::vector<LiveUnit>& live, const EntityId host, const std::uint64_t tick) {
    const auto* member = find_member(land, host);
    if (member == nullptr) return;
    const auto squads = land.movement.squads();
    if (member->squad >= squads.size()) return;
    stop_squad(land, live, squads[member->squad].object, tick);
}

} // namespace

const units::LandGarrisonData* land_garrison_data(const LandSession& land, const TypeId type) noexcept {
    const auto& rows = land.content.garrisons;
    const auto found = std::lower_bound(rows.begin(), rows.end(), type,
        [](const LandSessionContent::GarrisonBinding& row, const TypeId id) { return row.type < id; });
    return found != rows.end() && found->type == type ? &found->data : nullptr;
}

LiveUnit* land_live_unit(std::vector<LiveUnit>& live, const EntityId id) noexcept {
    const auto found = std::lower_bound(live.begin(), live.end(), id,
        [](const LiveUnit& row, const EntityId key) { return row.state.entity_id < key; });
    return found != live.end() && found->state.entity_id == id ? &*found : nullptr;
}

const LiveUnit* land_live_unit(const std::vector<LiveUnit>& live, const EntityId id) noexcept {
    const auto found = std::lower_bound(live.begin(), live.end(), id,
        [](const LiveUnit& row, const EntityId key) { return row.state.entity_id < key; });
    return found != live.end() && found->state.entity_id == id ? &*found : nullptr;
}

bool land_garrison_keeps_weapons(const LandSession& land, const LandGarrisonEntry& entry) noexcept {
    return land.static_roots.contains(entry.host);
}

LandOccupantFire land_garrison_splice_occupants(const LandSession& land, std::vector<LiveUnit>& roster) {
    LandOccupantFire fire;
    std::vector<LiveUnit> copies;
    for (const auto& [id, entry] : land.garrison_entries) {
        if (!resident(entry) || !land_garrison_keeps_weapons(land, entry)) continue;
        const auto* host = land_live_unit(roster, entry.host);
        if (host == nullptr || !entry.detached) continue;
        LandOccupantFire::Squad squad;
        squad.entrant = id;
        // The roster keeps the order the squad walked in with: its first member leads the shared target (Q1).
        for (const auto& member : entry.detached->members) {
            const auto slot = static_cast<std::size_t>(std::find_if(entry.hidden.begin(), entry.hidden.end(),
                [&member](const LiveUnit& row) { return row.state.entity_id == member.object; }) - entry.hidden.begin());
            if (slot >= entry.hidden.size() || !entry.hidden[slot].combat) continue;
            squad.roster.push_back(member.object);
            fire.ids.push_back({member.object, fire.squads.size(), slot});
            // LG-MUZZLE: a shot starts where the occupant is, at the host; the gun bones are not hosted.
            copies.push_back(entry.hidden[slot]);
            copies.back().state.position = host->state.position;
        }
        if (!squad.roster.empty()) fire.squads.push_back(std::move(squad));
    }
    if (copies.empty()) {
        fire.squads.clear();
        return fire;
    }
    std::vector<std::size_t> order(copies.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&copies](const std::size_t a, const std::size_t b) {
        return copies[a].state.entity_id < copies[b].state.entity_id; });
    std::vector<LandOccupantFire::Shooter> sorted;
    sorted.reserve(order.size());
    const auto old_size = roster.size();
    for (const auto i : order) {
        sorted.push_back(fire.ids[i]);
        roster.push_back(std::move(copies[i]));
    }
    fire.ids = std::move(sorted);
    std::inplace_merge(roster.begin(), roster.begin() + static_cast<std::ptrdiff_t>(old_size), roster.end(),
        [](const LiveUnit& a, const LiveUnit& b) { return a.state.entity_id < b.state.entity_id; });
    return fire;
}

void land_garrison_restore_occupants(LandSession& land, std::vector<LiveUnit>& roster, const LandOccupantFire& fire) {
    std::size_t next = 0, write = 0;
    for (std::size_t read = 0; read < roster.size(); ++read) {
        const auto id = roster[read].state.entity_id;
        while (next < fire.ids.size() && fire.ids[next].id < id) ++next;
        if (next < fire.ids.size() && fire.ids[next].id == id) {
            const auto& shooter = fire.ids[next];
            const auto entry = land.garrison_entries.find(fire.squads[shooter.squad].entrant);
            if (entry != land.garrison_entries.end() && shooter.slot < entry->second.hidden.size()
                && entry->second.hidden[shooter.slot].state.entity_id == id) {
                // The record keeps the host position it fired from; the exit sets the real one.
                entry->second.hidden[shooter.slot] = std::move(roster[read]);
            }
            continue;
        }
        if (write != read) roster[write] = std::move(roster[read]);
        ++write;
    }
    roster.erase(roster.begin() + static_cast<std::ptrdiff_t>(write), roster.end());
}

std::vector<EntityId> admit_land_garrison(LandSession& land, std::vector<LiveUnit>& live, const PlayerId issuer,
    const PlayerCommand& command, const LandGarrisonPayload& payload, const std::uint64_t tick) {
    std::vector<EntityId> accepted;
    const auto* host = land_live_unit(live, payload.host);
    const auto* host_data = host != nullptr ? land_garrison_data(land, host->state.type_id) : nullptr;
    if (host_data == nullptr || !host_data->host || host->state.owner != issuer) return accepted;
    const auto host_pose = state_pose(host->state);
    if (!host_pose) return accepted;
    const auto* host_member = find_member(land, payload.host);
    const auto host_type = host->state.type_id;
    for (const auto id : command.units) {
        const auto* unit = land_live_unit(live, id);
        const auto* data = unit != nullptr ? land_garrison_data(land, unit->state.type_id) : nullptr;
        const auto squads = land.movement.squads();
        const auto squad = std::find_if(squads.begin(), squads.end(),
            [id](const LandMovingSquad& row) { return row.object == id; });
        if (data == nullptr || !data->entrant || unit->state.owner != issuer || id == payload.host
            || squad == squads.end() || squad->maximum_speed <= Fixed{} || land.garrison_entries.contains(id)
            || std::find(accepted.begin(), accepted.end(), id) != accepted.end()
            || !units::land_garrison_compatible(*host_data, *data)) continue;
        // A squad cannot enter a vehicle it is itself part of.
        if (host_member != nullptr && host_member->squad == static_cast<std::size_t>(squad - squads.begin())) continue;
        const auto request = land.requests.find(id);
        if (request == land.requests.end()) continue;
        // G-02: a hovering host must stand on ground its entrants can leave onto.
        if (host_data->hovering) {
            LandPlacementQuery query;
            query.mask = request->second.mask; query.from = host_pose->position; query.desired = host_pose->position;
            query.radius = request->second.radius;
            if (land_find_open_position(land.occupancy, query).status != LandPlacementStatus::exact) continue;
        }
        LandGarrisonEntry entry;
        entry.entrant = id; entry.host = payload.host; entry.host_type = host_type; entry.bone = payload.bone;
        entry.value = data->value; entry.destroy_with_host = host_data->hovering;
        entry.host_position = host_pose->position; entry.host_facing = host_pose->turns;
        land.garrison_entries.emplace(id, std::move(entry));
        accepted.push_back(id);
    }
    if (!accepted.empty()) stop_host(land, live, payload.host, tick);
    return accepted;
}

std::size_t admit_land_unload(LandSession& land, std::vector<LiveUnit>& live, const PlayerId issuer,
    const PlayerCommand& command, const LandUnloadPayload& payload, const std::uint64_t tick) {
    if (command.units.size() != 1) return 0;
    const auto host_id = command.units.front();
    const auto* host = land_live_unit(live, host_id);
    if (host == nullptr || host->state.owner != issuer) return 0;
    std::size_t accepted = 0;
    for (auto& pair : land.garrison_entries) {
        auto& entry = pair.second;
        if (entry.host != host_id || entry.phase != LandGarrisonPhase::inside) continue;
        if (!payload.all && pair.first != payload.occupant) continue;
        entry.phase = LandGarrisonPhase::waiting_exit;
        ++accepted;
    }
    if (accepted != 0) stop_host(land, live, host_id, tick);
    return accepted;
}

namespace {

// What the read half decided for one entry; the ordered commit carries it out.
struct Decision {
    enum class Kind : std::uint8_t { keep, cancel, fade, leave } kind{Kind::keep};
    Vec2 destination{};
};

// Worker half: pure reads of the retained session, the live roster and the combat table.
Decision evaluate_entry(const CombatTable& table, const LandSession& land, const std::vector<LiveUnit>& live,
    const LandGarrisonEntry& entry) {
    Decision decision;
    if (entry.phase == LandGarrisonPhase::inside) return decision;
    const auto* data = land_garrison_data(land, entry.host_type);
    const auto* host = land_live_unit(live, entry.host);
    if (entry.phase == LandGarrisonPhase::waiting_exit) {
        // A dead host is resolved by the death-phase release, not here.
        if (data == nullptr || host == nullptr || !host_stationary(land, entry.host)) return decision;
        const auto pose = state_pose(host->state);
        if (!pose) return decision;
        const auto plan = exit_plan(*data, *pose, entry.bone, host_extent(table, entry.host_type));
        if (!plan) return decision;
        // LG-EXITSPOT: the exit spot is legal when the land placement query accepts it as it stands.
        LandPlacementQuery query;
        query.mask = entry.request.mask; query.from = plan->start; query.desired = plan->destination;
        query.radius = entry.request.radius;
        if (land_find_open_position(land.occupancy, query).status == LandPlacementStatus::exact) {
            decision.kind = Decision::Kind::leave;
            decision.destination = plan->destination;
        }
        return decision;
    }
    const auto squads = land.movement.squads();
    const auto squad = std::find_if(squads.begin(), squads.end(),
        [&entry](const LandMovingSquad& row) { return row.object == entry.entrant; });
    // The host or the entrant is gone, or the host has no room for this entrant: the command is cleared (G-04).
    if (data == nullptr || host == nullptr || squad == squads.end()
        || occupied_slots(land, entry.host) + entry.value > data->slots) {
        decision.kind = Decision::Kind::cancel;
        return decision;
    }
    if (entry.phase == LandGarrisonPhase::fading) return decision;
    const auto pose = state_pose(host->state);
    const auto target = pose ? entrance_point(*data, *pose, entry.bone) : std::nullopt;
    Vec2 delta;
    Fixed distance;
    if (!target || !math::try_subtract(target->x, squad->position.x, delta.x)
        || !math::try_subtract(target->y, squad->position.y, delta.y) || !math::try_length(delta, distance)) {
        decision.kind = Decision::Kind::cancel;
        return decision;
    }
    const auto extent = entrant_extent(land, static_cast<std::size_t>(squad - squads.begin()));
    Fixed outside, allowance;
    if (!math::try_subtract(distance, extent, outside)) {
        decision.kind = Decision::Kind::cancel;
        return decision;
    }
    if (outside <= data->radius) {
        decision.kind = Decision::Kind::fade;
        return decision;
    }
    if (!walk_finished(land, *squad)) return decision;
    // LG-DOOR: a walk that ended short because the host's own footprint blocks the target counts as arrived when the squad
    // stands within the entrance radius plus the host's footprint. LG-REPATH: otherwise the walk is not re-ordered.
    if (math::try_add(data->radius, host_extent(table, entry.host_type), allowance) && outside <= allowance)
        decision.kind = Decision::Kind::fade;
    else decision.kind = Decision::Kind::cancel;
    return decision;
}

// G-05: the entrant leaves movement and the live roster. Its squad record and live rows are kept in the entry and its members
// are nested on the host as carried objects. Returns false (nothing changed) when the entrant cannot be taken in.
bool enter_host(LandSession& land, std::vector<LiveUnit>& live, LandGarrisonEntry& entry, const std::uint64_t tick,
    std::vector<EntityId>& purged) {
    const auto squads = land.movement.squads();
    const auto squad = std::find_if(squads.begin(), squads.end(),
        [&entry](const LandMovingSquad& row) { return row.object == entry.entrant; });
    if (squad == squads.end() || land_live_unit(live, entry.host) == nullptr) return false;
    const auto index = static_cast<std::size_t>(squad - squads.begin());
    std::vector<EntityId> ids{entry.entrant};
    for (const auto& member : land.movement.members()) if (member.squad == index) ids.push_back(member.object);
    for (const auto id : ids) if (land_live_unit(live, id) == nullptr) return false;
    stop_squad(land, live, entry.entrant, tick);
    auto detached = land.movement.detach_squad(entry.entrant);
    if (!detached) return false;
    if (const auto request = land.requests.find(entry.entrant); request != land.requests.end()) {
        entry.request = request->second;
        land.requests.erase(request);
    }
    if (const auto held = land.combat_state.targets.find(entry.entrant); held != land.combat_state.targets.end()) {
        entry.target = held->second;
        land.combat_state.targets.erase(held);
    }
    entry.hidden.clear();
    for (const auto id : ids) {
        const auto at = std::lower_bound(live.begin(), live.end(), id,
            [](const LiveUnit& row, const EntityId key) { return row.state.entity_id < key; });
        entry.hidden.push_back(std::move(*at));
        live.erase(at);
    }
    std::sort(entry.hidden.begin(), entry.hidden.end(),
        [](const LiveUnit& a, const LiveUnit& b) { return a.state.entity_id < b.state.entity_id; });
    // WHE-06/07: the members are limbo identities of the host; a structure keeps their combat state (PR 2 reads it).
    auto* host = land_live_unit(live, entry.host);
    const bool structure = land_garrison_keeps_weapons(land, entry);
    for (const auto& member : detached->members) {
        CarriedObject rider;
        rider.entity_id = member.object;
        const auto* hidden = std::find_if(entry.hidden.data(), entry.hidden.data() + entry.hidden.size(),
            [&member](const LiveUnit& row) { return row.state.entity_id == member.object; });
        rider.type_id = hidden->state.type_id;
        rider.named_hero = member.source.named_hero; rider.generic_hero = member.source.generic_hero;
        static_cast<void>(contain_object(host->state, rider, structure));
        purged.push_back(member.object);
    }
    purged.push_back(entry.entrant);
    entry.detached = std::move(detached);
    entry.phase = LandGarrisonPhase::inside;
    return true;
}

// G-07: the entrant returns to movement and the live roster at `destination`.
bool leave_host(LandSession& land, std::vector<LiveUnit>& live, LandGarrisonEntry& entry, const Vec2 destination) {
    if (!entry.detached) return false;
    std::vector<EntityId> riders;
    for (const auto& unit : entry.hidden) riders.push_back(unit.state.entity_id);
    auto copy = *entry.detached;
    if (!land.movement.restore_squad(std::move(copy), destination)) return false;
    entry.request.start = destination;
    land.requests[entry.entrant] = entry.request;
    land.combat_state.targets[entry.entrant] = entry.target;
    if (auto* host = land_live_unit(live, entry.host))
        std::erase_if(host->state.contained, [&riders](const CarriedObject& rider) {
            return std::find(riders.begin(), riders.end(), rider.entity_id) != riders.end(); });
    const auto& terrain = land.occupancy.terrain();
    const auto squads = land.movement.squads();
    for (auto& unit : entry.hidden) {
        Vec2 position;
        Fixed facing;
        if (const auto* member = find_member(land, unit.state.entity_id)) {
            position = member->position; facing = member->facing;
        } else {
            const auto root = std::find_if(squads.begin(), squads.end(),
                [&unit](const LandMovingSquad& row) { return row.object == unit.state.entity_id; });
            if (root == squads.end()) continue;
            position = root->position; facing = root->facing;
        }
        Fixed degrees;
        if (math::try_multiply(facing, Fixed::from_raw(360 * Fixed::scale), degrees)) {
            if (const auto rotation = yaw_rotation(degrees)) unit.state.rotation = rotation.value();
        }
        unit.state.position = {position.x, position.y, terrain.height_at(position.x, position.y).value_or(Fixed{})};
        unit.state.order = Order{};
        const auto at = std::lower_bound(live.begin(), live.end(), unit.state.entity_id,
            [](const LiveUnit& row, const EntityId key) { return row.state.entity_id < key; });
        live.insert(at, std::move(unit));
    }
    entry.hidden.clear();
    entry.detached.reset();
    return true;
}

} // namespace
} // namespace eawr::sim::tactical::session_detail

namespace eawr::sim::tactical {

// The named garrison service seam (frame order FS-47..FS-49, FC-79): one poll per entrant every logical frame (interval 1,
// first deadline 0), after the movement and occupancy commit and before combat. Stock order is per entrant (locomotion, team
// targeting, then garrison), so this single seam is the documented order seam FD-77..FD-79: it runs after all movement, which
// gives every occupant the new host state (FSC-87), and before all combat, which loses the earlier team-targeting boundary
// (FSC-84) and lets all later readers see one side of each containment (FSC-85/86). Per-entry reads run as a
// partitioned phase on the retained session (immutable inputs, disjoint decision slots); the commit is serial in ascending
// entrant id, which also decides a race for the last slot deterministically. Entries are sparse (only squads under a
// garrison order), so the serial commit is not a per-entity loop.
core::Result<void> TacticalSession::Impl::land_garrison_service(std::vector<LiveUnit>& live, const PartitionExecutor& executor) {
    using namespace session_detail;
    auto& state = *land;
    if (state.garrison_entries.empty()) return core::Result<void>::success();
    core::tick_profile::Scope scope("land-garrison");
    std::vector<EntityId> ids;
    ids.reserve(state.garrison_entries.size());
    for (const auto& pair : state.garrison_entries) ids.push_back(pair.first);
    std::vector<Decision> decisions(ids.size());
    {
        const auto& view = state;
        const auto& roster = live;
        const auto read = executor.execute_phase("land-garrison-read", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, ids.size());
            for (auto i = range.begin; i < range.end; ++i)
                decisions[i] = evaluate_entry(combat, view, roster, view.garrison_entries.at(ids[i]));
        });
        if (!read) return read;
    }
    const auto tick = completed_tick;
    std::vector<EntityId> purged;
    LandSpawnFilter taken; // exit spots given out this tick: the read half saw none of them
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto found = state.garrison_entries.find(ids[i]);
        if (found == state.garrison_entries.end()) continue;
        auto& entry = found->second;
        const auto& decision = decisions[i];
        if (resident(entry)) {
            if (const auto* host = land_live_unit(live, entry.host)) {
                if (const auto pose = state_pose(host->state)) { entry.host_position = pose->position; entry.host_facing = pose->turns; }
            }
            if (entry.phase == LandGarrisonPhase::waiting_exit && decision.kind == Decision::Kind::leave) {
                // Occupants leaving in the same tick must not share a spot: the next open spot clear of those placed.
                auto spot = decision.destination;
                if (!taken.bodies.empty()) {
                    taken.radius = entry.request.radius;
                    LandPlacementQuery query;
                    query.mask = entry.request.mask; query.from = spot; query.desired = spot;
                    query.radius = entry.request.radius; query.filter = &taken;
                    const auto placement = land_find_open_position(state.occupancy, query);
                    if (placement.status == LandPlacementStatus::none) continue;
                    spot = placement.position;
                }
                if (leave_host(state, live, entry, spot)) {
                    taken.bodies.push_back({spot, entry.request.radius});
                    state.garrison_entries.erase(found);
                }
            }
            continue;
        }
        if (decision.kind == Decision::Kind::cancel) {
            stop_squad(state, live, entry.entrant, tick);
            state.garrison_entries.erase(found);
            continue;
        }
        if (entry.phase == LandGarrisonPhase::moving) {
            if (decision.kind != Decision::Kind::fade) continue;
            stop_squad(state, live, entry.entrant, tick);
            entry.phase = LandGarrisonPhase::fading;
            entry.fade_frames = land_garrison_fade_frames;
            continue;
        }
        // Fading: the entrant disappears into the host when the fade ends and a slot is still free (LG-LASTSLOT: the entrant that
        // loses a same-frame race for the last slot is cleared instead of stranded).
        if (entry.fade_frames > 0) --entry.fade_frames;
        if (entry.fade_frames > 0) continue;
        const auto* data = land_garrison_data(state, entry.host_type);
        if (data == nullptr || occupied_slots(state, entry.host) + entry.value > data->slots
            || !enter_host(state, live, entry, tick, purged)) {
            stop_squad(state, live, entry.entrant, tick);
            state.garrison_entries.erase(found);
        }
    }
    if (!purged.empty()) {
        // The movement step preserves occupations it does not own as scenery: free the bodies that left.
        std::vector<LandOccupation> keep;
        for (const auto& occupation : state.occupancy.occupations())
            if (std::find(purged.begin(), purged.end(), occupation.object) == purged.end()) keep.push_back(occupation);
        return state.occupancy.commit(keep, executor);
    }
    return core::Result<void>::success();
}

// G-09, FS-50, FD-80, FSC-88: the death commit of the tick: a destroyed walking host or structure releases its occupants where it stood; a destroyed hovering host
// takes them with it. The shared destruction journal has already accounted for the carried members of a hovering host and
// skipped the others (Tick::destruction_event consults LandSession::garrison_hiding).
core::Result<void> session_detail::Tick::land_garrison_deaths(std::vector<LiveUnit>& live) {
    auto& state = *impl_->land;
    LandSpawnFilter taken; // spots given out in this death commit
    std::vector<EntityId> ids;
    for (const auto& pair : state.garrison_entries)
        if (resident(pair.second) && land_live_unit(live, pair.second.host) == nullptr) ids.push_back(pair.first);
    for (const auto id : ids) {
        const auto found = state.garrison_entries.find(id);
        auto& entry = found->second;
        const auto* data = land_garrison_data(state, entry.host_type);
        bool released = false;
        if (!entry.destroy_with_host && data != nullptr) {
            // U-G6-11 (unverified): the occupants are placed at the nearest legal spot to the normal exit point.
            const auto plan = exit_plan(*data, saved_pose(entry), entry.bone, host_extent(impl_->combat, entry.host_type));
            if (plan) {
                LandPlacementQuery query;
                query.mask = entry.request.mask; query.from = plan->start; query.desired = plan->destination;
                query.radius = entry.request.radius; query.filter = &taken;
                taken.radius = entry.request.radius;
                const auto placement = land_find_open_position(state.occupancy, query);
                if (placement.status != LandPlacementStatus::none && leave_host(state, live, entry, placement.position)) {
                    taken.bodies.push_back({placement.position, entry.request.radius});
                    released = true;
                }
            }
        }
        if (!released) {
            // A hovering host, or no legal spot at all (LG-DEADEXIT): the occupants are lost with the host. Carried members of a
            // hovering host were accounted by the destruction journal; the others and the squad roots are accounted here.
            for (const auto& unit : entry.hidden) {
                track_victory_change(&unit.state, nullptr, true);
                const bool root = unit.state.entity_id == entry.entrant;
                if (!entry.destroy_with_host && !root)
                    pending_losses_.push_back({unit.state.owner, unit.state.type_id, PlayerId{}, 1, tick});
                if (root || !entry.destroy_with_host)
                    impacts_->events->push_back(Event{tick, EventKind::unit_destroyed, unit.state.owner, 0, unit.state.entity_id});
            }
        }
        state.garrison_entries.erase(found);
    }
    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical
