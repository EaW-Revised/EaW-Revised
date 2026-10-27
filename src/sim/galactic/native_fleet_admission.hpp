#pragma once

#include "eawr/sim/galactic/session.hpp"

#include "eawr/sim/galactic/fleets.hpp"
#include "paused_admission.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace eawr::sim::galactic::session_detail {
struct FleetData;

// One admission kernel serves the live owner and sparse native player layers.
template<class Owner>
void execute_native_fleet(Owner& owner, const GameCommand& command, const std::uint64_t tick,
    std::vector<GalacticEvent>& events, const bool pending_local_conflict, const bool production_placement,
    const PausedKind kind = PausedKind::fleet_management) {
    GalacticEvent event{tick, GalacticEventKind::command_executed, command.key.seat, 0,
        command.key.sequence, 0, GalacticRejectReason::none};
    const auto reject = [&](const GalacticRejectReason reason) {
        event.kind = GalacticEventKind::command_rejected; event.reason = reason; events.push_back(event);
    };
    const auto emit = [&](const GalacticEventKind kind, const EntityId subject, const std::uint64_t value = 0) {
        auto notification = event; notification.kind = kind; notification.subject = subject;
        notification.value = value; events.push_back(notification);
    };
    const auto* issuer = owner.seat(command.key.seat);
    if (!issuer || (!issuer->active && !production_placement)) return reject(GalacticRejectReason::inactive_seat);
    if (!owner.setup.fleet_control) return reject(GalacticRejectReason::unknown_value);
    const auto& gates = owner.setup.fleet_control->gates;
    const auto humans = std::count_if(owner.seats.begin(), owner.seats.end(), [](const auto& row) { return row.kind == SeatKind::human; });
    if (!production_placement && (owner_paused_refusal(owner, kind, pending_local_conflict, issuer->kind == SeatKind::human)
            || (humans <= 1 && pending_local_conflict && (gates.queued_campaign_conflict || gates.active_speech))))
        return reject(GalacticRejectReason::paused);
    const auto changed = [&] {
        owner.population_stale = true;
        owner.dirty[static_cast<std::size_t>(StateSection::fleets)] = true;
        owner.invalidate_observer_projection();
        if (owner.setup.movement) owner.dirty[static_cast<std::size_t>(StateSection::movement)] = true;
        owner.dirty[static_cast<std::size_t>(StateSection::fleet_control)] = true;
    };
    const auto destroy = [&](const FleetId id) {
        owner.lose_actor(id);
        owner.registry.destroy(owner.fleets.at(id)); owner.fleets.erase(id); --owner.live_object_count;
        emit(GalacticEventKind::fleet_destroyed, id);
    };
    const auto check_detached = [&](const auto& members, const PlanetId planet) -> GalacticRejectReason {
        for (const auto id : members) {
            const auto unit = owner.detached_units.find(id);
            if (unit == owner.detached_units.end()) return GalacticRejectReason::invalid_membership;
            if (unit->second.owner != issuer->player) return GalacticRejectReason::not_authorized;
            if (unit->second.planet != planet) return GalacticRejectReason::incompatible_location;
        }
        return GalacticRejectReason::none;
    };
    const auto attach = [&](const auto& members, const FleetId id) {
        auto& target = owner.registry.template get<FleetData>(owner.fleets.at(id));
        for (const auto member : members) {
            target.members.push_back(owner.detached_units.at(member).member);
            owner.relocate_actor(member, target.owner, target.orbit, id);
            owner.detached_units.erase(member); emit(GalacticEventKind::member_attached, member, id);
        }
        changed(); owner.refresh_fleet_combat_skip(id);
    };
    const auto* transfer = std::get_if<TransferFleetMembers>(&command.payload);
    if (transfer) {
        const auto& refs = transfer->members;
        // A single ordered command commit validates the complete group before
        // detaching anything. This atomicity is ours (GHUD-S86), not a retail claim.
        PlanetId planet{};
        std::map<FleetId, std::set<EntityId>> sources;
        for (const auto& ref : refs) sources[ref.fleet].insert(ref.member);
        std::map<EntityId, FleetMember> moved_members;
        std::map<FleetId, std::vector<FleetMember>> retained_members;
        for (const auto& [id, requested] : sources) {
            event.subject = id;
            if (!owner.fleets.contains(id)) return reject(GalacticRejectReason::unknown_subject);
            const auto& source = owner.registry.template get<FleetData>(owner.fleets.at(id));
            if (source.owner != issuer->player) return reject(GalacticRejectReason::not_authorized);
            if (!source.orbit || source.motion.transit || (planet && source.orbit != planet))
                return reject(GalacticRejectReason::incompatible_location);
            std::size_t found{};
            for (const auto& member : source.members) {
                if (requested.contains(member.id)) { moved_members.emplace(member.id, member); ++found; }
                else retained_members[id].push_back(member);
            }
            if (found != requested.size()) return reject(GalacticRejectReason::invalid_membership);
            planet = source.orbit;
        }
        FleetId target = transfer->target;
        if (sources.contains(target)) return reject(GalacticRejectReason::invalid_membership);
        if (target) {
            if (!owner.fleets.contains(target)) return reject(GalacticRejectReason::unknown_subject);
            const auto& destination = owner.registry.template get<FleetData>(owner.fleets.at(target));
            if (destination.owner != issuer->player) return reject(GalacticRejectReason::not_authorized);
            if (destination.orbit != planet || destination.motion.transit)
                return reject(GalacticRejectReason::incompatible_location);
        } else {
            if (owner.next_id == std::numeric_limits<EntityId>::max() || owner.live_object_count >= galactic_max_objects)
                return reject(GalacticRejectReason::resource_limit);
            target = owner.next_id++;
            owner.insert(Fleet{target, issuer->player, planet, {}, transfer->slot}); ++owner.live_object_count;
            owner.record_fleet_shell(target, issuer->player, planet);
            owner.dirty[static_cast<std::size_t>(StateSection::session)] = true;
            emit(GalacticEventKind::fleet_created, target, planet);
        }
        event.subject = target;
        event.value = planet;
        events.push_back(event);
        for (const auto& [id, requested] : sources) {
            (void)requested;
            owner.registry.template get<FleetData>(owner.fleets.at(id)).members = std::move(retained_members[id]);
        }
        for (const auto& ref : refs) {
            emit(GalacticEventKind::member_detached, ref.member, ref.fleet);
            owner.registry.template get<FleetData>(owner.fleets.at(target)).members.push_back(moved_members.at(ref.member));
            owner.relocate_actor(ref.member, issuer->player, planet, target);
            emit(GalacticEventKind::member_attached, ref.member, target);
        }
        for (const auto& [id, members] : sources) {
            (void)members;
            if (owner.registry.template get<FleetData>(owner.fleets.at(id)).members.empty()) destroy(id);
            else owner.refresh_fleet_combat_skip(id);
        }
        owner.refresh_fleet_combat_skip(target);
        changed();
        return;
    }
    if (const auto* create = std::get_if<CreateFleet>(&command.payload)) {
        event.subject = create->planet;
        if (!owner.planets.contains(create->planet)) return reject(GalacticRejectReason::unknown_value);
        const auto reason = check_detached(create->members, create->planet);
        if (reason != GalacticRejectReason::none) return reject(reason);
        if (owner.next_id == std::numeric_limits<EntityId>::max() || owner.live_object_count >= galactic_max_objects)
            return reject(GalacticRejectReason::resource_limit);
        const auto id = owner.next_id++;
        // WGM-196: retail writes the requested slot without an occupancy test; so do we.
        owner.insert(Fleet{id, issuer->player, create->planet, {}, create->slot}); ++owner.live_object_count;
        owner.record_fleet_shell(id, issuer->player, create->planet);
        owner.dirty[static_cast<std::size_t>(StateSection::session)] = true;
        emit(GalacticEventKind::command_executed, id, create->planet);
        emit(GalacticEventKind::fleet_created, id, create->planet); attach(create->members, id);
        return;
    }
    if (const auto* merge = std::get_if<MergeFleets>(&command.payload)) {
        event.subject = merge->source; event.value = merge->target;
        if (merge->source == merge->target) return reject(GalacticRejectReason::invalid_membership);
        if (!owner.fleets.contains(merge->source) || !owner.fleets.contains(merge->target)) return reject(GalacticRejectReason::unknown_subject);
        auto& source = owner.registry.template get<FleetData>(owner.fleets.at(merge->source));
        auto& target = owner.registry.template get<FleetData>(owner.fleets.at(merge->target));
        if (source.owner != issuer->player || source.owner != target.owner) return reject(GalacticRejectReason::not_authorized);
        if (source.orbit == 0 || source.motion.transit || target.motion.transit || source.orbit != target.orbit) return reject(GalacticRejectReason::incompatible_location);
        events.push_back(event);
        while (!source.members.empty()) {
            const auto member = source.members.back(); source.members.pop_back();
            emit(GalacticEventKind::member_detached, member.id, merge->source);
            target.members.push_back(member); emit(GalacticEventKind::member_attached, member.id, merge->target);
            owner.relocate_actor(member.id, target.owner, target.orbit, merge->target);
        }
        destroy(merge->source); changed(); owner.refresh_fleet_combat_skip(merge->target); return;
    }
    FleetId id{};
    if (const auto* add = std::get_if<AddFleetMembers>(&command.payload)) id = add->fleet;
    if (const auto* remove = std::get_if<RemoveFleetMembers>(&command.payload)) id = remove->fleet;
    if (const auto* slot = std::get_if<SetFleetOrbitSlot>(&command.payload)) id = slot->fleet;
    if (const auto* slot = std::get_if<SetFleetPathfinder>(&command.payload)) id = slot->fleet;
    event.subject = id;
    if (!owner.fleets.contains(id)) return reject(GalacticRejectReason::unknown_subject);
    auto& fleet = owner.registry.template get<FleetData>(owner.fleets.at(id));
    if (fleet.owner != issuer->player) return reject(GalacticRejectReason::not_authorized);
    if (fleet.orbit == 0 || fleet.motion.transit) return reject(GalacticRejectReason::incompatible_location);
    if (const auto* slot = std::get_if<SetFleetPathfinder>(&command.payload)) {
        if (issuer->kind != SeatKind::human || production_placement) return reject(GalacticRejectReason::not_authorized);
        if (slot->member) {
            const auto member = std::find_if(fleet.members.begin(), fleet.members.end(), [&](const auto& row) { return row.id == slot->member; });
            if (member == fleet.members.end()) return reject(GalacticRejectReason::invalid_membership);
            const auto& types = owner.setup.fleet_control->types;
            const auto type = std::lower_bound(types.begin(), types.end(), member->type, [](const auto& row, auto value) { return row.type < value; });
            if (type == types.end() || type->type != member->type || !type->space_pathfinder)
                return reject(GalacticRejectReason::invalid_membership);
        }
        // WGT-40 / GHUD-R44: replacing the slot clears its previous occupant.
        // One ordered command commit validates before changing any member.
        for (auto& member : fleet.members) member.pathfinder = member.id == slot->member;
        changed(); event.value = slot->member; events.push_back(event); return;
    }
    if (const auto* add = std::get_if<AddFleetMembers>(&command.payload)) {
        const auto reason = check_detached(add->members, fleet.orbit);
        if (reason != GalacticRejectReason::none) return reject(reason);
        events.push_back(event); attach(add->members, id); return;
    }
    if (const auto* remove = std::get_if<RemoveFleetMembers>(&command.payload)) {
        for (const auto member : remove->members)
            if (std::none_of(fleet.members.begin(), fleet.members.end(), [&](const auto& row) { return row.id == member; }))
                return reject(GalacticRejectReason::invalid_membership);
        events.push_back(event);
        for (const auto member : remove->members) {
            const auto found = std::find_if(fleet.members.begin(), fleet.members.end(), [&](const auto& row) { return row.id == member; });
            owner.detached_units.emplace(member, DetachedUnit{*found, fleet.owner, fleet.orbit});
            owner.relocate_actor(member, fleet.owner, fleet.orbit, 0);
            fleet.members.erase(found); emit(GalacticEventKind::member_detached, member, id);
        }
        if (fleet.members.empty()) destroy(id);
        changed(); owner.refresh_fleet_combat_skip(id); return;
    }
    if (const auto* slot = std::get_if<SetFleetOrbitSlot>(&command.payload)) {
        const auto allowed = fleet_move_without_ability(Fleet{id, fleet.owner, fleet.orbit, fleet.members, fleet.orbit_slot},
            owner.setup.fleet_control->types, owner.movement_inputs.get());
        if (!allowed) { event.value = 1; return reject(GalacticRejectReason::invalid_membership); }
        if (!allowed.value()) return reject(GalacticRejectReason::movement_ability_required);
        event.value = slot->slot; events.push_back(event);
        if (fleet.orbit_slot == slot->slot) return;
        fleet.orbit_slot = slot->slot; changed(); emit(GalacticEventKind::fleet_slot_changed, id, slot->slot);
        return;
    }
    reject(GalacticRejectReason::unknown_value);
}
} // namespace eawr::sim::galactic::session_detail
