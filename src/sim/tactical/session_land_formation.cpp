#include "session_land_formation.hpp"
#include "land_phase_internal.hpp"
#include "eawr/sim/math/trig.hpp"

#include <algorithm>

namespace eawr::sim::tactical::session_detail {
namespace {
using math::Fixed;
using math::Vec2;

Fixed absolute_value(const Fixed value) { return value < Fixed{} ? Fixed::from_raw(-value.raw()) : value; }

// The oriented box of a squad (WLPA-95): half the span of its members' slots widened by their hard extents, forward and
// side, plus the container's jiggle for a formation team. A member that authors no hard extent contributes its radius.
struct Extents {
    Fixed forward{}, side{}, spacing{}, radius{};
    bool ok{true};
};
Extents squad_extents(const LandMovement& movement, const std::size_t index) {
    Extents result;
    const auto& squad = movement.squads()[index];
    for (const auto& member : movement.members()) {
        if (member.squad != index) continue;
        const auto hard_x = member.hard_x > Fixed{} ? member.hard_x : member.profile.radius;
        const auto hard_y = member.hard_y > Fixed{} ? member.hard_y : member.profile.radius;
        Fixed forward, side;
        result.ok &= math::try_add(absolute_value(member.slot.x), hard_x, forward) && math::try_add(absolute_value(member.slot.y), hard_y, side);
        result.forward = std::max(result.forward, forward);
        result.side = std::max(result.side, side);
        result.spacing = std::max(result.spacing, member.gap);
        result.radius = std::max(result.radius, member.profile.radius);
    }
    if (squad.has_formation && !squad.generic_team) {
        result.ok &= math::try_add(result.forward, squad.jiggle, result.forward) && math::try_add(result.side, squad.jiggle, result.side);
    }
    return result;
}

bool bike_or_infantry(const units::LandCollisionGroup group) noexcept {
    return group == units::LandCollisionGroup::infantry || group == units::LandCollisionGroup::bike;
}

// LMF-02: the authored FormationGrouping of a member type (the first company that lists it).
units::LandFormationGrouping grouping_of(const LandSession& land, const TypeId type) {
    for (const auto& company : land.content.companies)
        for (std::size_t i = 0; i < company.members.size() && i < company.company.locomotion.size(); ++i)
            if (company.members[i].type == type) return company.company.locomotion[i].formation_grouping;
    return units::LandFormationGrouping::standard;
}

Vec2 forward_of(const Fixed facing) {
    const auto direction = math::sin_cos_turn(facing);
    return {direction.cosine, direction.sine};
}
} // namespace

std::optional<std::vector<LandFormationPlan>> plan_land_group_move(const LandSession& land, const std::span<const EntityId> squads,
    const Vec2 click, const std::map<EntityId, Fixed>& ranges) {
    const auto all = land.movement.squads();
    const auto members = land.movement.members();
    const auto& settings = land.content.formation;
    if (!settings.authored || squads.empty()) return std::nullopt;
    std::vector<LandFormationObject> objects;
    std::vector<LandFormationObstacle> obstacles;
    for (std::size_t index = 0; index < all.size(); ++index) {
        const auto& squad = all[index];
        const auto leader = std::find_if(members.begin(), members.end(), [index](const auto& row) { return row.squad == index; });
        if (leader == members.end()) continue;
        const auto extents = squad_extents(land.movement, index);
        if (!extents.ok) return std::nullopt;
        const bool team = squad.has_formation && !squad.generic_team;
        const auto group = team ? squad.collision_class : leader->profile.collision_class;
        const bool listed = std::find(squads.begin(), squads.end(), squad.object) != squads.end();
        if (listed) {
            const auto request = land.requests.find(squad.object);
            if (request == land.requests.end()) return std::nullopt;
            LandFormationObject object;
            object.id = squad.object; object.position = squad.position;
            object.half_forward = extents.forward; object.half_side = extents.side;
            object.radius = std::max(extents.radius, request->second.radius); object.spacing = extents.spacing;
            object.speed = squad.maximum_speed; object.mask = request->second.mask;
            const auto range = ranges.find(squad.object);
            object.range = range != ranges.end() ? range->second : Fixed{};
            object.infantry_class = bike_or_infantry(group); object.giant = group == units::LandCollisionGroup::giant_vehicle;
            object.formation_team = team;
            bool hero = false;
            for (const auto& member : members) if (member.squad == index) hero = hero || member.source.named_hero || member.source.generic_hero;
            object.hero = hero;
            const auto binding = land.bindings.find(leader->object);
            object.grouping = binding != land.bindings.end() ? grouping_of(land, binding->second.first) : units::LandFormationGrouping::standard;
            // LMF-02: a team container recruits by the longer distance; so does anything that is not plain infantry
            // (a bike is not).
            object.recruit = (!team && group == units::LandCollisionGroup::infantry) ? settings.infantry_recruit : settings.vehicle_recruit;
            objects.push_back(object);
        } else if (group != units::LandCollisionGroup::none) {
            // LMF-05: a body outside the order stands where its walk ends.
            LandFormationObstacle obstacle;
            const bool walking = squad.order == LandMoveOrder::following || squad.order == LandMoveOrder::recovering;
            obstacle.position = walking && !squad.path.nodes.empty() ? squad.path.nodes.back() : squad.position;
            obstacle.axis = forward_of(squad.facing);
            obstacle.half_forward = extents.forward; obstacle.half_side = extents.side;
            obstacle.infantry_class = bike_or_infantry(group); obstacle.formation_team = team;
            obstacles.push_back(obstacle);
        }
    }
    if (objects.size() != squads.size()) return std::nullopt;
    return land_plan_formations(land.occupancy, settings, objects, obstacles, click);
}

void begin_land_member_search(LandSession& land, const EntityId squad, const LandFormationOrder& order, const Vec2 slot) {
    const auto all = land.movement.squads();
    const auto at = std::find_if(all.begin(), all.end(), [squad](const auto& row) { return row.object == squad; });
    const auto base = land.requests.find(squad);
    if (at == all.end() || base == land.requests.end()) return;
    auto request = base->second;
    request.start = at->position;
    LandPlacementQuery query;
    query.mask = request.mask; query.from = request.start; query.radius = request.radius;
    query.desired = land_formation_slot_position(order.request.end, order.heading, slot).value_or(order.request.end);
    const auto placement = land_find_open_position(land.occupancy, query);
    request.end = placement.status == LandPlacementStatus::none ? order.request.end : placement.position;
    base->second = request;
    land.searches.erase(squad);
    land.searches.emplace(squad, LandPathService::begin(land.occupancy, land.arena, request));
}

void release_land_formation_member(LandSession& land, const EntityId squad, const std::span<const EntityId> keep,
    const std::span<const EntityId> renewed) {
    for (auto formation = land.walking_formations.begin(); formation != land.walking_formations.end();) {
        auto& slots = formation->second.slots;
        std::erase_if(slots, [squad](const auto& member) { return member.slot.id == squad; });
        if (slots.empty()) formation = land.walking_formations.erase(formation);
        else if (formation->first == squad) {
            // A departed lead must not collide with a new formation it may later lead.
            auto retained = std::move(formation->second);
            formation = land.walking_formations.erase(formation);
            const auto new_lead = retained.slots.front().slot.id;
            for (const auto& entry : retained.slots) {
                const auto& slot = entry.slot;
                const auto squads = land.movement.squads();
                const auto member = std::find_if(squads.begin(), squads.end(), [&](const auto& row) { return row.object == slot.id; });
                if (member != squads.end()) static_cast<void>(land.movement.set_formation(
                    static_cast<std::size_t>(member - squads.begin()), new_lead, slot.offset));
            }
            land.walking_formations.emplace(new_lead, std::move(retained));
        }
        else ++formation;
    }
    for (auto order = land.formations.begin(); order != land.formations.end();) {
        if (std::find(keep.begin(), keep.end(), order->first) != keep.end()) { ++order; continue; }
        auto& slots = order->second.slots;
        const auto slot = std::find_if(slots.begin(), slots.end(), [squad](const auto& row) { return row.id == squad; });
        if (slot == slots.end()) { ++order; continue; }
        if (order->first != squad) {
            slots.erase(slot);
            ++order;
            continue;
        }
        // The lead's search carries the centre path: it goes, and the others search alone.
        const auto released = std::move(order->second);
        land.searches.erase(order->first);
        order = land.formations.erase(order);
        for (const auto& other : released.slots) {
            if (other.id == squad || std::find(renewed.begin(), renewed.end(), other.id) != renewed.end()) continue;
            // They search alone now: none of them names the lead any more.
            const auto all = land.movement.squads();
            const auto member = std::find_if(all.begin(), all.end(), [&](const auto& row) { return row.object == other.id; });
            if (member != all.end()) static_cast<void>(land.movement.set_formation(static_cast<std::size_t>(member - all.begin()), 0, {}));
            begin_land_member_search(land, other.id, released, other.offset);
        }
    }
}

std::vector<std::pair<EntityId, Vec2>> install_land_formation(LandSession& land, const LandFormationOrder& order,
    const LandPath& centre, const std::uint64_t tick) {
    std::vector<std::pair<EntityId, Vec2>> alone;
    LandWalkingFormation walking;
    walking.centre = centre;
    for (const auto& slot : order.slots) {
        const auto all = land.movement.squads();
        const auto at = std::find_if(all.begin(), all.end(), [&](const auto& row) { return row.object == slot.id; });
        const auto request = land.requests.find(slot.id);
        if (at == all.end() || request == land.requests.end()) continue; // the squad died while the path was searched
        const auto index = static_cast<std::size_t>(at - all.begin());
        // The slot's own route first; when it cannot be built or installed (a corner or a wall tip next to the route), the
        // route at the slot's side offset only, then along the centre path itself. The member then ends near its slot, not
        // on it, instead of searching alone for a long time.
        const Vec2 variants[3] = {slot.offset, Vec2{Fixed{}, slot.offset.y}, Vec2{}};
        bool installed = false;
        std::vector<std::uint32_t> correspondence;
        for (const auto& variant : variants) {
            const auto nodes = land_formation_member_path(land.occupancy, centre.nodes, at->position, variant, request->second.mask,
                request->second.radius, order.heading, &correspondence);
            if (!nodes) continue;
            LandPath path;
            path.status = LandPathStatus::found; path.nodes = *nodes;
            bool measured = true;
            for (std::size_t i = 1; i < nodes->size() && measured; ++i) {
                Fixed dx, dy, segment;
                measured = math::try_subtract((*nodes)[i].x, (*nodes)[i - 1].x, dx) && math::try_subtract((*nodes)[i].y, (*nodes)[i - 1].y, dy)
                    && math::try_length({dx, dy}, segment) && math::try_add(path.length, segment, path.length);
            }
            if (measured && land.movement.install_path(index, std::move(path), tick)) { installed = true; break; }
        }
        if (!installed) {
            static_cast<void>(land.movement.set_formation(index, 0, {}));
            alone.emplace_back(slot.id, slot.offset);
        }
        else walking.slots.push_back({slot, std::move(correspondence), 0});
    }
    std::sort(walking.slots.begin(), walking.slots.end(), [](const auto& a, const auto& b) { return a.slot.id < b.slot.id; });
    if (!walking.slots.empty()) {
        const auto lead_kept = std::any_of(walking.slots.begin(), walking.slots.end(),
            [&](const auto& entry) { return entry.slot.id == order.lead; });
        const auto retained_lead = lead_kept ? order.lead : walking.slots.front().slot.id;
        if (!lead_kept) for (const auto& entry : walking.slots) {
            const auto squads = land.movement.squads();
            const auto member = std::find_if(squads.begin(), squads.end(), [&](const auto& row) { return row.object == entry.slot.id; });
            if (member != squads.end()) static_cast<void>(land.movement.set_formation(
                static_cast<std::size_t>(member - squads.begin()), retained_lead, entry.slot.offset));
        }
        land.walking_formations[retained_lead] = std::move(walking);
    }
    return alone;
}

core::Result<void> prepare_land_formation_speeds(LandSession& land, const PartitionExecutor& executor) {
    if (land.walking_formations.empty()) {
        land.formation_speeds.clear();
        return core::Result<void>::success();
    }
    auto squads = land.movement.squads();
    land.formation_finished.clear();
    land.formation_work.clear();
    for (auto& [lead, formation] : land.walking_formations) {
        static_cast<void>(lead);
        land.formation_work.push_back(&formation);
    }
    land.formation_scratch.resize(land.formation_work.size());
    struct Context { LandSession& land; std::span<const LandMovingSquad> squads; } context{land, squads};
    const auto lifecycle = land_phase(executor, "land-formation-lifecycle", tick_partition_count, [&context](std::size_t partition) {
        const auto range = partition_range(partition, context.land.formation_work.size());
        for (auto f = range.begin; f < range.end; ++f) {
            auto& finished = context.land.formation_scratch[f].finished;
            finished.clear();
            for (const auto& member : context.land.formation_work[f]->slots) {
                const auto at = std::find_if(context.squads.begin(), context.squads.end(),
                    [&](const auto& row) { return row.object == member.slot.id; });
                if (at == context.squads.end() || at->order == LandMoveOrder::reached || at->order == LandMoveOrder::dropped
                    || at->order == LandMoveOrder::idle) finished.push_back(member.slot.id);
            }
        }
    });
    if (!lifecycle) return lifecycle;
    for (const auto& scratch : land.formation_scratch)
        land.formation_finished.insert(land.formation_finished.end(), scratch.finished.begin(), scratch.finished.end());
    // Only completed memberships are committed serially, in formation and slot order.
    for (const auto id : land.formation_finished) {
        release_land_formation_member(land, id);
        squads = land.movement.squads();
        const auto at = std::find_if(squads.begin(), squads.end(), [id](const auto& row) { return row.object == id; });
        if (at != squads.end()) static_cast<void>(land.movement.set_formation(static_cast<std::size_t>(at - squads.begin()), 0, {}));
    }
    squads = land.movement.squads();
    land.formation_speeds.assign(squads.size(), std::nullopt);
    land.formation_work.clear();
    if (!land.content.formation.speed_authored) return core::Result<void>::success();
    for (auto& [lead, formation] : land.walking_formations) {
        static_cast<void>(lead);
        land.formation_work.push_back(&formation);
    }
    land.formation_scratch.resize(land.formation_work.size());
    context.squads = squads;
    const auto phase = land_phase(executor, "land-formation-speed", tick_partition_count, [&context](std::size_t partition) {
        const auto range = partition_range(partition, context.land.formation_work.size());
        for (auto f = range.begin; f < range.end; ++f) {
            const auto& formation = *context.land.formation_work[f];
            auto& scratch = context.land.formation_scratch[f];
            scratch.valid = true; scratch.indices.clear(); scratch.deviations.clear(); scratch.segments.clear(); scratch.speeds.clear();
            for (const auto& member : formation.slots) {
                const auto at = std::find_if(context.squads.begin(), context.squads.end(),
                    [&](const auto& row) { return row.object == member.slot.id; });
                if (at == context.squads.end() || member.centre_segments.empty()
                    || member.centre_segments.size() != at->path.nodes.size()) {
                    scratch.valid = false; break;
                }
                scratch.indices.push_back(static_cast<std::size_t>(at - context.squads.begin()));
                const auto limit = member.centre_segments[std::min(at->next_node, member.centre_segments.size() - 1)];
                const auto segment = land_formation_advance_segment(formation.centre.nodes, member.current_segment,
                    std::max(member.current_segment, limit), at->position);
                if (!segment) { scratch.valid = false; break; }
                scratch.segments.push_back(*segment);
            }
            if (!scratch.valid || scratch.indices.empty()) continue;
            Fixed maximum{}, minimum{}, common = context.squads[scratch.indices.front()].maximum_speed;
            for (const auto index : scratch.indices) common = std::min(common, context.squads[index].maximum_speed);
            for (std::size_t i = 0; i < scratch.indices.size(); ++i) {
                const auto& member = formation.slots[i];
                const auto& squad = context.squads[scratch.indices[i]];
                bool collision_slow = false;
                for (const auto& contact : context.land.movement.contacts()) {
                    if (contact.kind != LandContactKind::moving_moving) continue;
                    // LMF-09: the body causing a moving-pair slowdown keeps zero deviance. A published limit at or above
                    // the peer's maximum does not slow it. Pair-sweep holds are reflected in its committed collision speed.
                    const auto slowed = [&](EntityId id, const std::optional<Fixed>& limit) {
                        const auto peer = std::find_if(context.squads.begin(), context.squads.end(),
                            [id](const auto& row) { return row.object == id; });
                        return peer != context.squads.end() && ((limit && *limit < peer->maximum_speed)
                            || peer->collision_speed < peer->assigned_speed);
                    };
                    collision_slow = collision_slow || (contact.first == squad.object && slowed(contact.second, contact.second_speed_limit))
                        || (contact.second == squad.object && slowed(contact.first, contact.first_speed_limit));
                }
                scratch.observations.clear();
                if (!collision_slow) for (std::size_t j = 0; j < scratch.indices.size(); ++j) {
                    if (i == j) continue;
                    const auto& other = context.squads[scratch.indices[j]];
                    const auto& other_member = formation.slots[j];
                    const auto segment_i = scratch.segments[i];
                    const auto segment_j = scratch.segments[j];
                    const auto distance = land_formation_forward_offset(formation.centre.nodes, segment_i, squad.position, segment_j, other.position);
                    Fixed dx;
                    if (!distance || !math::try_subtract(other_member.slot.offset.x, member.slot.offset.x, dx)) {
                        scratch.valid = false; break;
                    }
                    scratch.observations.push_back({dx, *distance});
                }
                if (!scratch.valid) break;
                const auto deviance = land_formation_deviance(scratch.observations);
                if (!deviance) { scratch.valid = false; break; }
                scratch.deviations.push_back(*deviance);
                maximum = std::max(maximum, *deviance); minimum = std::min(minimum, *deviance);
            }
            if (!scratch.valid) continue;
            for (std::size_t i = 0; i < scratch.indices.size(); ++i) {
                const auto index = scratch.indices[i];
                const auto cap = land_formation_speed(scratch.deviations[i], maximum, minimum, common,
                    context.squads[index].maximum_speed, context.land.movement.formation_deceleration(index));
                if (!cap) { scratch.valid = false; break; }
                scratch.speeds.emplace_back(index, *cap);
            }
        }
    });
    if (!phase) return phase;
    for (std::size_t f = 0; f < land.formation_scratch.size(); ++f) {
        const auto& scratch = land.formation_scratch[f];
        if (!scratch.valid) return core::Result<void>::failure(core::Diagnostic{
            std::string(math::diagnostic_codes::overflow), core::Severity::error,
            "Land formation speed calculation failed.", {}, {}, {}, {}});
        for (std::size_t i = 0; i < scratch.segments.size(); ++i)
            land.formation_work[f]->slots[i].current_segment = scratch.segments[i];
        for (const auto& [index, speed] : scratch.speeds) land.formation_speeds[index] = speed;
    }
    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical::session_detail
