#include "session_impl.hpp"
#include "eawr/sim/galactic/fleets.hpp"
#include "encoding.hpp"

#include <algorithm>
#include <limits>

namespace eawr::sim::galactic {
using session_detail::FleetData;

core::Result<GalacticSession::NativeFleetReceipt> GalacticSession::admit_native_fleet(
    PlayerId player, const CommandPayload& payload, bool pending_local_conflict) {
    using Result = core::Result<NativeFleetReceipt>;
    auto& impl = *impl_;
    const bool fleet_command = std::holds_alternative<CreateFleet>(payload)
        || std::holds_alternative<AddFleetMembers>(payload) || std::holds_alternative<RemoveFleetMembers>(payload)
        || std::holds_alternative<MergeFleets>(payload) || std::holds_alternative<SetFleetOrbitSlot>(payload)
        || std::holds_alternative<TransferFleetMembers>(payload);
    const bool movement = std::holds_alternative<MoveFleet>(payload);
    if (impl.failed || encode_payload(payload).size() > galactic_replay_max_payload - 38 || (!fleet_command && !movement)
        || decode_payload(layer_of(payload), opcode_of(payload), encode_payload(payload)) != std::optional(payload))
        return Result::failure(detail::diagnostic(diagnostic_codes::invalid_command, "invalid native fleet command boundary"));
    const auto issuer = std::find_if(impl.seats.begin(), impl.seats.end(), [&](const auto& seat) {
        return seat.player == player && seat.active;
    });
    if (issuer == impl.seats.end()) return Result::success({false, 0, GalacticRejectReason::inactive_seat});
    GameCommand command; command.key = {impl.completed_tick, issuer->seat_id, 0}; command.payload = payload;
    std::vector<GalacticEvent> events(impl.snapshot->events().begin(), impl.snapshot->events().end());
    const auto first = events.size();
    if (movement) impl.execute_movement(command, impl.completed_tick, events, pending_local_conflict);
    else impl.execute_fleet(command, impl.completed_tick, events, pending_local_conflict);
    NativeFleetReceipt receipt;
    for (auto i = first; i < events.size(); ++i) {
        if (events[i].kind == GalacticEventKind::command_rejected) receipt.reason = events[i].reason;
        if (events[i].kind == GalacticEventKind::command_executed) {
            receipt.admitted = true; receipt.object = events[i].subject;
        }
    }
    impl.snapshot = impl.make_snapshot(std::move(events));
    if (auto journaled = impl.journal_native_receipt(*this, NativeEffectKind::fleet, player, 0, 0,
        pending_local_conflict, &payload, receipt); !journaled) return Result::failure(journaled.error());
    // A launch owns the temporary detached row and publishes its final surface
    // removal itself. Do not deliver against that nested admission's view.
    if (!impl.native_applying) if (auto delivered = impl.deliver_actor_changes(); !delivered)
        return Result::failure(delivered.error());
    return Result::success(receipt);
}

core::Result<bool> fleet_move_without_ability(const Fleet& fleet, const std::span<const FleetTypeTraits> types,
    const MovementSetup* movement) {
    bool qualifying = false, nonempty = false;
    const auto trait = [&](const FleetMember& member) {
        const auto found = std::lower_bound(types.begin(), types.end(), member.type,
            [](const auto& row, const auto type) { return row.type < type; });
        return found == types.end() || found->type != member.type ? nullptr : &*found;
    };
    const auto inspect = [&](const auto& self, const FleetMember& member, unsigned depth) -> bool {
        const auto* found = trait(member);
        if (!found || depth > 32) return false;
        if (found->requires_live_children) {
            if (!movement) return false;
            const auto contents = std::lower_bound(movement->contents.begin(), movement->contents.end(), member.id,
                [](const auto& row, const auto id) { return row.container < id; });
            if (contents == movement->contents.end() || contents->container != member.id) return false;
            // WGM-83/101: inspect surviving craft, not the squadron/container template.
            for (const auto& child : contents->children) if (!self(self, child, depth + 1)) return false;
            return true;
        }
        nonempty = true;
        qualifying = qualifying || (!found->transport && found->can_hyperspace_without_ability);
        if (!found->transport || !movement) return true;
        // WGM-83 (debug build): the fleet breakdown also lists a transport's carried units, one level,
        // so troops on a transport supply the qualifying nontransport.
        const auto contents = std::lower_bound(movement->contents.begin(), movement->contents.end(), member.id,
            [](const auto& row, const auto id) { return row.container < id; });
        if (contents == movement->contents.end() || contents->container != member.id) return true;
        for (const auto& carried : contents->children) {
            const auto* carried_trait = trait(carried);
            if (!carried_trait) return false;
            qualifying = qualifying || (!carried_trait->transport && carried_trait->can_hyperspace_without_ability);
        }
        return true;
    };
    for (const auto& member : fleet.members) {
        if (!inspect(inspect, member, 0))
            return core::Result<bool>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
                "unverified boundary: fleet movement predicate requires original traits and live container children"));
    }
    return core::Result<bool>::success(!nonempty || qualifying);
}

FleetControlSetup GalacticSession::Impl::fleet_control_state() const {
    FleetControlSetup result; result.types = setup.fleet_control->types; result.gates = setup.fleet_control->gates;
    for (const auto& [id, row] : detached_units) { (void)id; result.detached_units.push_back(row); }
    return result;
}

void GalacticSession::Impl::execute_fleet(const GameCommand& command, const std::uint64_t tick,
    std::vector<GalacticEvent>& events, const bool pending_local_conflict, const bool production_placement) {
    GalacticEvent event{tick, GalacticEventKind::command_executed, command.key.seat, 0,
        command.key.sequence, 0, GalacticRejectReason::none};
    const auto reject = [&](const GalacticRejectReason reason) {
        event.kind = GalacticEventKind::command_rejected; event.reason = reason; events.push_back(event);
    };
    const auto emit = [&](const GalacticEventKind kind, const EntityId subject, const std::uint64_t value = 0) {
        auto notification = event; notification.kind = kind; notification.subject = subject;
        notification.value = value; events.push_back(notification);
    };
    const auto* issuer = seat(command.key.seat);
    if (!issuer || (!issuer->active && !production_placement)) return reject(GalacticRejectReason::inactive_seat);
    if (!setup.fleet_control) return reject(GalacticRejectReason::unknown_value);
    const auto& gates = setup.fleet_control->gates;
    const auto humans = std::count_if(seats.begin(), seats.end(), [](const auto& row) { return row.kind == SeatKind::human; });
    if (!production_placement && (clock.pause_depth != 0 || (humans <= 1 && pending_local_conflict
            && (gates.queued_campaign_conflict || gates.active_speech)))) return reject(GalacticRejectReason::paused);
    const auto changed = [&] {
        population_stale = true;
        dirty[static_cast<std::size_t>(StateSection::fleets)] = true;
        invalidate_observer_projection();
        if (setup.movement) dirty[static_cast<std::size_t>(StateSection::movement)] = true;
        dirty[static_cast<std::size_t>(StateSection::fleet_control)] = true;
    };
    const auto destroy = [&](const FleetId id) {
        lose_actor(id);
        registry.destroy(fleets.at(id)); fleets.erase(id); --live_object_count;
        emit(GalacticEventKind::fleet_destroyed, id);
    };
    const auto check_detached = [&](const auto& members, const PlanetId planet) -> GalacticRejectReason {
        for (const auto id : members) {
            const auto unit = detached_units.find(id);
            if (unit == detached_units.end()) return GalacticRejectReason::invalid_membership;
            if (unit->second.owner != issuer->player) return GalacticRejectReason::not_authorized;
            if (unit->second.planet != planet) return GalacticRejectReason::incompatible_location;
        }
        return GalacticRejectReason::none;
    };
    const auto attach = [&](const auto& members, const FleetId id) {
        auto& target = registry.get<FleetData>(fleets.at(id));
        for (const auto member : members) {
            target.members.push_back(detached_units.at(member).member);
            relocate_actor(member, target.owner, target.orbit, id);
            detached_units.erase(member); emit(GalacticEventKind::member_attached, member, id);
        }
        changed(); refresh_fleet_combat_skip(id);
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
            if (!fleets.contains(id)) return reject(GalacticRejectReason::unknown_subject);
            const auto& source = registry.get<FleetData>(fleets.at(id));
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
            if (!fleets.contains(target)) return reject(GalacticRejectReason::unknown_subject);
            const auto& destination = registry.get<FleetData>(fleets.at(target));
            if (destination.owner != issuer->player) return reject(GalacticRejectReason::not_authorized);
            if (destination.orbit != planet || destination.motion.transit)
                return reject(GalacticRejectReason::incompatible_location);
        } else {
            if (next_id == std::numeric_limits<EntityId>::max() || live_object_count >= galactic_max_objects)
                return reject(GalacticRejectReason::resource_limit);
            target = next_id++;
            insert(Fleet{target, issuer->player, planet, {}, transfer->slot}); ++live_object_count;
            record_fleet_shell(target, issuer->player, planet);
            dirty[static_cast<std::size_t>(StateSection::session)] = true;
            emit(GalacticEventKind::fleet_created, target, planet);
        }
        event.subject = target;
        event.value = planet;
        events.push_back(event);
        for (const auto& [id, requested] : sources) {
            (void)requested;
            registry.get<FleetData>(fleets.at(id)).members = std::move(retained_members[id]);
        }
        for (const auto& ref : refs) {
            emit(GalacticEventKind::member_detached, ref.member, ref.fleet);
            registry.get<FleetData>(fleets.at(target)).members.push_back(moved_members.at(ref.member));
            relocate_actor(ref.member, issuer->player, planet, target);
            emit(GalacticEventKind::member_attached, ref.member, target);
        }
        for (const auto& [id, members] : sources) {
            (void)members;
            if (registry.get<FleetData>(fleets.at(id)).members.empty()) destroy(id);
            else refresh_fleet_combat_skip(id);
        }
        refresh_fleet_combat_skip(target);
        changed();
        return;
    }
    if (const auto* create = std::get_if<CreateFleet>(&command.payload)) {
        event.subject = create->planet;
        if (!planets.contains(create->planet)) return reject(GalacticRejectReason::unknown_value);
        const auto reason = check_detached(create->members, create->planet);
        if (reason != GalacticRejectReason::none) return reject(reason);
        if (next_id == std::numeric_limits<EntityId>::max() || live_object_count >= galactic_max_objects)
            return reject(GalacticRejectReason::resource_limit);
        const auto id = next_id++;
        // WGM-196: retail writes the requested slot without an occupancy test; so do we.
        insert(Fleet{id, issuer->player, create->planet, {}, create->slot}); ++live_object_count;
        record_fleet_shell(id, issuer->player, create->planet);
        dirty[static_cast<std::size_t>(StateSection::session)] = true;
        emit(GalacticEventKind::command_executed, id, create->planet);
        emit(GalacticEventKind::fleet_created, id, create->planet); attach(create->members, id);
        return;
    }
    if (const auto* merge = std::get_if<MergeFleets>(&command.payload)) {
        event.subject = merge->source; event.value = merge->target;
        if (merge->source == merge->target) return reject(GalacticRejectReason::invalid_membership);
        if (!fleets.contains(merge->source) || !fleets.contains(merge->target)) return reject(GalacticRejectReason::unknown_subject);
        auto& source = registry.get<FleetData>(fleets.at(merge->source));
        auto& target = registry.get<FleetData>(fleets.at(merge->target));
        if (source.owner != issuer->player || source.owner != target.owner) return reject(GalacticRejectReason::not_authorized);
        if (source.orbit == 0 || source.motion.transit || target.motion.transit || source.orbit != target.orbit) return reject(GalacticRejectReason::incompatible_location);
        events.push_back(event);
        while (!source.members.empty()) {
            const auto member = source.members.back(); source.members.pop_back();
            emit(GalacticEventKind::member_detached, member.id, merge->source);
            target.members.push_back(member); emit(GalacticEventKind::member_attached, member.id, merge->target);
            relocate_actor(member.id, target.owner, target.orbit, merge->target);
        }
        destroy(merge->source); changed(); refresh_fleet_combat_skip(merge->target); return;
    }
    FleetId id{};
    if (const auto* add = std::get_if<AddFleetMembers>(&command.payload)) id = add->fleet;
    if (const auto* remove = std::get_if<RemoveFleetMembers>(&command.payload)) id = remove->fleet;
    if (const auto* slot = std::get_if<SetFleetOrbitSlot>(&command.payload)) id = slot->fleet;
    event.subject = id;
    if (!fleets.contains(id)) return reject(GalacticRejectReason::unknown_subject);
    auto& fleet = registry.get<FleetData>(fleets.at(id));
    if (fleet.owner != issuer->player) return reject(GalacticRejectReason::not_authorized);
    if (fleet.orbit == 0 || fleet.motion.transit) return reject(GalacticRejectReason::incompatible_location);
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
            detached_units.emplace(member, DetachedUnit{*found, fleet.owner, fleet.orbit});
            relocate_actor(member, fleet.owner, fleet.orbit, 0);
            fleet.members.erase(found); emit(GalacticEventKind::member_detached, member, id);
        }
        if (fleet.members.empty()) destroy(id);
        changed(); refresh_fleet_combat_skip(id); return;
    }
    if (const auto* slot = std::get_if<SetFleetOrbitSlot>(&command.payload)) {
        const auto allowed = fleet_move_without_ability(Fleet{id, fleet.owner, fleet.orbit, fleet.members, fleet.orbit_slot},
            setup.fleet_control->types, movement_inputs.get());
        if (!allowed) { event.value = 1; return reject(GalacticRejectReason::invalid_membership); }
        if (!allowed.value()) return reject(GalacticRejectReason::movement_ability_required);
        event.value = slot->slot; events.push_back(event);
        if (fleet.orbit_slot == slot->slot) return;
        fleet.orbit_slot = slot->slot; changed(); emit(GalacticEventKind::fleet_slot_changed, id, slot->slot);
        return;
    }
    reject(GalacticRejectReason::unknown_value);
}
} // namespace eawr::sim::galactic
