#pragma once

#include "encoding.hpp"
#include "paused_admission.hpp"
#include "eawr/sim/galactic/session.hpp"

#include "eawr/sim/galactic/movement.hpp"
#include "eawr/sim/galactic/graph.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <set>

namespace eawr::sim::galactic::session_detail {
struct FleetData;
struct PlanetData;

// WGM-153/154: the live movement owner and native player frontier use the
// same relink/gather commit. Visit borrows fleet rows; only linked rows write.
template<class Owner, class Visit>
core::Result<void> link_native_movement_orbit(Owner& owner, Visit visit, FleetId id,
    PlanetId destination, std::uint64_t tick, std::vector<GalacticEvent>& events) {
    auto& fleet = owner.registry.template get<FleetData>(owner.fleets.at(id));
    auto slot = std::uint8_t{0};
    for (const auto candidate : {fleet.orbit_slot, std::uint8_t{1}, std::uint8_t{2}}) {
        bool occupied = false;
        visit([&](FleetId other_id, const auto& other) {
            if (other_id != id && other.orbit == destination && other.owner == fleet.owner && other.orbit_slot == candidate) occupied = true;
        });
        if (!occupied) { slot = candidate; break; }
    }
    std::uint64_t latest{};
    visit([&](FleetId, const auto& other) { latest = std::max(latest, other.motion.orbit_entry); });
    if (latest == std::numeric_limits<std::uint64_t>::max()) return core::Result<void>::failure(
        detail::diagnostic(diagnostic_codes::invalid_setup, "orbit order exhausted"));
    const auto& planet = owner.registry.template get<PlanetData>(owner.planets.at(destination));
    auto x = math::add(planet.position.x, math::Fixed::from_raw(18 * math::Fixed::scale));
    auto y = math::add(planet.position.y, math::Fixed::from_raw(10 * math::Fixed::scale));
    if (!x) return core::Result<void>::failure(x.error());
    if (!y) return core::Result<void>::failure(y.error());
    const auto transit = fleet.motion.transit;
    fleet.motion.orbit_entry = latest + 1;
    fleet.motion.position = {x.value(), y.value(), planet.position.z};
    fleet.motion.facing = {{}, math::Fixed::from_raw(math::Fixed::scale), {}};
    fleet.orbit_slot = slot; fleet.orbit = destination; fleet.motion.transit.reset();
    owner.relocate_actor(id, fleet.owner, destination, destination);
    for (const auto& member : fleet.members) owner.relocate_actor(member.id, fleet.owner, destination, id);
    if (transit) {
        events.push_back({tick, GalacticEventKind::movement_finished, transit->producer, id, transit->sequence, destination, GalacticRejectReason::none});
        if (owner.native_movement_listener) owner.native_movement_listener(events.back());
    }
    owner.dirty[static_cast<std::size_t>(StateSection::movement)] = true;
    owner.dirty[static_cast<std::size_t>(StateSection::fleets)] = true;
    owner.dirty[static_cast<std::size_t>(StateSection::fleet_control)] = true;
    owner.invalidate_observer_projection();
    return core::Result<void>::success();
}

template<class Owner, class Visit>
core::Result<void> gather_native_movement_orbit(Owner& owner, Visit visit, FleetId id,
    PlanetId destination, std::uint64_t tick, std::vector<GalacticEvent>& events) {
    const auto attacker = owner.registry.template get<FleetData>(owner.fleets.at(id)).owner;
    const auto defender = owner.registry.template get<PlanetData>(owner.planets.at(destination)).owner;
    core::Result<void> result = core::Result<void>::success();
    visit([&](FleetId other_id, const auto& other) {
        if (result && other_id != id && other.motion.transit && other.raid_destination != destination
            && (other.owner == attacker || other.owner == defender)
            && other.motion.transit->path.back() == destination
            && other.motion.transit->path.at(other.motion.transit->hop + 1) == destination)
            result = link_native_movement_orbit(owner, visit, other_id, destination, tick, events);
    });
    return result;
}

[[nodiscard]] core::Result<bool> native_orbit_conflict(const Fleet&,
    std::optional<std::span<const CensusObject>>, const MovementSetup&, const MovementSetup* contents = nullptr);

// WGM-84/85: the paid admission of the intermediate path planets. `planet_state` yields (owner, orbital occupant)
// of a path planet; an unowned planet (owner 0) has nobody to pay.
struct NativeBribeQuote {
    math::Fixed total{};
    std::vector<std::pair<PlayerId, math::Fixed>> recipients; // ascending player
    bool invalid{};
};
[[nodiscard]] NativeBribeQuote quote_native_bribe(PlayerId mover, std::span<const FleetMember> departing,
    std::span<const PlanetId> path, const MovementSetup&,
    const std::function<std::pair<PlayerId, PlayerId>(PlanetId)>& planet_state);
struct NativeMovementAdmission {
    GalacticRejectReason reason{GalacticRejectReason::none};
    FleetDeparture departure{};
    FleetMotion motion{};
};
[[nodiscard]] NativeMovementAdmission prepare_native_movement(const Fleet&, const GameCommand&,
    const MovementSetup&, const FleetControlSetup&, const PlanetGraph&, std::span<const Planet>,
    std::span<const Route>, EntityId, std::size_t, bool returning_player);
template<class Owner>
void execute_native_movement(Owner& owner, const GameCommand& command, std::uint64_t tick,
    std::vector<GalacticEvent>& events, bool pending_local_conflict, std::optional<PlayerId> returning_player) {
    const auto& move = std::get<MoveFleet>(command.payload);
    GalacticEvent event{tick, GalacticEventKind::command_executed, command.key.seat, move.fleet, command.key.sequence, move.path.back(), GalacticRejectReason::none};
    const auto emit = [&](GalacticEventKind kind, GalacticRejectReason reason = GalacticRejectReason::none) {
        auto e = event; e.kind = kind; e.reason = reason; events.push_back(e);
        if (owner.native_movement_listener) owner.native_movement_listener(e);
    };
    const auto reject = [&](GalacticRejectReason reason) { emit(GalacticEventKind::command_rejected, reason); emit(GalacticEventKind::movement_canceled, reason); };
    if (move.destination_slot && *move.destination_slot > 2) return reject(GalacticRejectReason::unknown_value);
    const auto* issuer = owner.seat(command.key.seat);
    if (!returning_player && (!issuer || !issuer->active)) return reject(GalacticRejectReason::inactive_seat);
    if (!owner.setup.movement) return reject(GalacticRejectReason::unknown_value);
    const auto& gates = owner.setup.fleet_control->gates;
    const auto humans = std::count_if(owner.seats.begin(), owner.seats.end(), [](const auto& r) { return r.kind == SeatKind::human; });
    if (!returning_player && (owner_paused_refusal(owner, PausedKind::fleet_move, pending_local_conflict, issuer->kind == SeatKind::human)
            || (humans <= 1 && pending_local_conflict && (gates.queued_campaign_conflict || gates.active_speech)))) return reject(GalacticRejectReason::paused);
    if (!owner.fleets.contains(move.fleet)) return reject(GalacticRejectReason::unknown_subject);
    auto& f = owner.registry.template get<session_detail::FleetData>(owner.fleets.at(move.fleet));
    if (f.owner != (returning_player ? *returning_player : issuer->player)) return reject(GalacticRejectReason::not_authorized);
    // WGM-84: a fleet with no orbit parent cannot be redirected.
    if (f.orbit == 0 || f.motion.transit) return reject(GalacticRejectReason::movement_blocked);
    if (move.raid && (!owner.raid_qualified(move.fleet, move.path.back()) || move.destination_slot))
        return reject(GalacticRejectReason::movement_blocked);
    auto prepared = prepare_native_movement(
        Fleet{move.fleet, f.owner, f.orbit, f.members, f.orbit_slot, f.motion}, command, *owner.movement_inputs,
        *owner.setup.fleet_control, *owner.graph, owner.sorted_planets(), owner.sorted_routes(), owner.next_id, owner.live_object_count, bool(returning_player));
    if (prepared.reason != GalacticRejectReason::none) return reject(prepared.reason);
    if (move.destination_actor) {
        if (move.raid) return reject(GalacticRejectReason::unknown_value);
        EntityId actor = move.destination_actor;
        bool attached = false;
        std::set<EntityId> visited;
        while (actor && visited.insert(actor).second) {
            const auto* live = owner.movement_actor(actor);
            if (!live || live->dead || live->delete_pending
                || live->owner != f.owner || live->planet != f.orbit) break;
            if (live->container == move.fleet) {
                attached = std::none_of(prepared.departure.left_behind.begin(), prepared.departure.left_behind.end(),
                    [&](auto left) { return left == actor; });
                break;
            }
            actor = live->container;
        }
        if (!attached) return reject(GalacticRejectReason::invalid_membership);
    }
    // WGM-84: the first corruption-benefiting mover pays the intermediate planets before anything moves. Retreat
    // endpoints are supplied directly and are not an ordinary path (WGM-198).
    if (!returning_player) {
        std::vector<FleetMember> departing;
        const auto& left = prepared.departure.left_behind;
        for (const auto& member : f.members)
            if (std::find(left.begin(), left.end(), member.id) == left.end()) departing.push_back(member);
        const auto quote = quote_native_bribe(f.owner, departing, move.path, *owner.movement_inputs,
            [&](PlanetId planet) { return owner.bribe_planet_state(planet); });
        if (quote.invalid) return reject(GalacticRejectReason::cannot_depart);
        if (quote.total > math::Fixed{})
            if (const auto reason = owner.settle_movement_bribe(f.owner, quote.recipients, quote.total); reason != GalacticRejectReason::none)
                return reject(reason);
    }
    const auto source = f.orbit; // WGM-150: the planet being left
    auto motion = std::move(prepared.motion);
    // Admission is complete: commit the remnant by existing identity, then unlink/depart.
    if (!prepared.departure.left_behind.empty()) {
        Fleet remnant{owner.next_id, f.owner, f.orbit, {}, f.orbit_slot, {}};
        for (const auto& member : f.members) if (std::find(prepared.departure.left_behind.begin(), prepared.departure.left_behind.end(), member.id) != prepared.departure.left_behind.end()) remnant.members.push_back(member);
        std::erase_if(f.members, [&](const auto& member) { return std::find(prepared.departure.left_behind.begin(), prepared.departure.left_behind.end(), member.id) != prepared.departure.left_behind.end(); });
        owner.insert(remnant); ++owner.next_id; ++owner.live_object_count;
        owner.record_fleet_shell(remnant.id, remnant.owner, remnant.orbit);
        for (const auto& member : remnant.members) owner.relocate_actor(member.id, remnant.owner, remnant.orbit, remnant.id);
        owner.dirty[static_cast<std::size_t>(StateSection::session)] = true;
        // EnTT insertion may relocate FleetData: reacquire it below.
        auto e = event; e.kind = GalacticEventKind::fleet_created; e.subject = remnant.id; e.value = remnant.orbit; events.push_back(e);
        for (const auto& member : remnant.members) {
            e.kind = GalacticEventKind::member_detached; e.subject = member.id; e.value = move.fleet; events.push_back(e);
            e.kind = GalacticEventKind::member_attached; e.value = remnant.id; events.push_back(e);
        }
        owner.dirty[static_cast<std::size_t>(StateSection::fleet_control)] = true;
    }
    auto& departing = owner.registry.template get<session_detail::FleetData>(owner.fleets.at(move.fleet));
    owner.accrue_departure_piracy(move.fleet, departing.owner, source, departing.members);
    departing.orbit = 0; departing.motion = std::move(motion);
    departing.raid_destination = move.raid ? std::optional<PlanetId>{move.path.back()} : std::nullopt;
    owner.relocate_actor(move.fleet, departing.owner, 0, 0);
    for (const auto& member : departing.members) owner.relocate_actor(member.id, departing.owner, 0, move.fleet);
    owner.dirty[static_cast<std::size_t>(StateSection::fleets)] = true;
    owner.invalidate_observer_projection();
    owner.dirty[static_cast<std::size_t>(StateSection::movement)] = true;
    emit(GalacticEventKind::command_executed); emit(GalacticEventKind::movement_begin);
}

} // namespace eawr::sim::galactic::session_detail
