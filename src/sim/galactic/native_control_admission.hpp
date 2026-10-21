#pragma once

#include "eawr/sim/galactic/session.hpp"

#include "eawr/sim/galactic/control.hpp"
#include "encoding.hpp"

#include <algorithm>
#include <cassert>
#include <limits>

namespace eawr::sim::galactic::session_detail {
struct FleetData;
struct PlanetData;

template<class T> core::Result<T> native_control_error(const std::string& message) {
    return core::Result<T>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, message));
}
template<class Rows, class Key> const typename Rows::value_type* native_find_row(const Rows& rows, const auto id, Key key) {
    const auto at = std::lower_bound(rows.begin(), rows.end(), id, [&](const auto& row, auto value) { return key(row) < value; });
    return at != rows.end() && key(*at) == id ? &*at : nullptr;
}
template<class Control> bool native_allied(const Control& control, PlayerId player, PlayerId other) {
    const auto* row = native_find_row(control.relations, std::pair{player, other},
        [](const auto& item) { return std::pair{item.player, item.other}; });
    return row && row->allied;
}
template<class Control> const ControlType* native_control_type(const Control& control, TypeId id) {
    return native_find_row(control.types, id, [](const auto& row) { return row.type; });
}
template<class Actors> std::uint32_t native_landed_count(const ControlPlanet& planet, const Actors& actors) {
    std::uint32_t count{};
    for (const auto& unit : planet.landed) {
        if (!unit.id) continue;
        const auto actual = actors.find(unit.id);
        if (actual == actors.end() || !actual->second.delete_pending) ++count;
    }
    return count;
}
template<class Owner, class Admit>
core::Result<GalacticSession::NativeFleetReceipt> launch_native_surface_units(Owner& native_owner, PlayerId player,
    std::span<const EntityId> units, FleetId target, std::uint8_t slot, const Admit& admit,
    bool& admission_ran) {
    using NativeFleetReceipt = GalacticSession::NativeFleetReceipt;
    using Result = core::Result<NativeFleetReceipt>;
    admission_ran = false;
    if (native_owner.failed || !native_owner.control || !native_owner.setup.fleet_control || units.empty())
        return native_control_error<NativeFleetReceipt>("native launch requires its live control owner");
    struct Surface { std::size_t planet{}; EntityId id{}; TypeId type{}; };
    std::vector<Surface> found;
    for (const auto id : units) {
        std::size_t planet_index = native_owner.control->planets.size(), at{};
        for (std::size_t i = 0; i < native_owner.control->planets.size() && planet_index == native_owner.control->planets.size(); ++i) {
            const auto& landed = native_owner.control->planets[i].landed;
            const auto row = std::find_if(landed.begin(), landed.end(), [&](const auto& unit) { return unit.id == id; });
            if (row != landed.end()) { planet_index = i; at = static_cast<std::size_t>(row - landed.begin()); }
        }
        if (planet_index == native_owner.control->planets.size()) return Result::success({false, id, GalacticRejectReason::unknown_subject});
        const auto& planet = native_owner.control->planets[planet_index];
        const auto& unit = planet.landed[at];
        if (unit.owner != player) return Result::success({false, id, GalacticRejectReason::not_authorized});
        const auto* trait = native_find_row(native_owner.setup.fleet_control->types, unit.type, [](const auto& row) { return row.type; });
        if (!trait || !trait->transport
            || std::any_of(found.begin(), found.end(), [&](const auto& row) { return row.id == id; }))
            return Result::success({false, id, GalacticRejectReason::invalid_membership});
        const auto* state = native_find_row(native_owner.control->state, planet.planet, [](const auto& row) { return row.planet; });
        if (!state || state->blockaded) return Result::success({false, id, GalacticRejectReason::movement_blocked});
        found.push_back({planet_index, id, unit.type});
    }
    // Only links this launch created are released: a pre-existing link on the same identity is not ours to erase.
    std::vector<EntityId> linked;
    const auto release = [&] { for (const auto id : linked) native_owner.detached_units.erase(id); };
    // Reuse the ordinary atomic fleet admission. Do not remove any surface link
    // until it succeeds; a rejected launch leaves every actor intact.
    for (const auto& row : found) {
        if (!native_owner.detached_units.emplace(row.id, DetachedUnit{{row.id, row.type}, player, native_owner.control->planets[row.planet].planet}).second) {
            release();
            return native_control_error<NativeFleetReceipt>("launch actor already has a detached link");
        }
        linked.push_back(row.id);
    }
    const std::vector<EntityId> members(units.begin(), units.end());
    const CommandPayload payload = target ? CommandPayload{AddFleetMembers{target, members}}
        : CommandPayload{CreateFleet{native_owner.control->planets[found.front().planet].planet, members, slot}};
    admission_ran = true;
    auto accepted = admit(payload);
    if (!accepted || !accepted.value().admitted) { release(); return accepted; }
    for (const auto& row : found) {
        auto& landed = native_owner.control->planets[row.planet].landed;
        landed.erase(std::find_if(landed.begin(), landed.end(), [&](const auto& unit) { return unit.id == row.id; }));
    }
    native_owner.dirty[static_cast<std::size_t>(StateSection::control)] = true;
    if (native_owner.observer) {
        // WGUI-41/103: the native_owner.observer's live transport references leave the surface with the link, so
        // roster, count and content gates read the post-launch planet at the next publication.
        for (const auto& row : found) {
            const auto planet_id = native_owner.control->planets[row.planet].planet;
            const auto shown = std::find_if(native_owner.observer->planets.begin(), native_owner.observer->planets.end(),
                [planet_id](const auto& value) { return value.planet == planet_id; });
            if (shown == native_owner.observer->planets.end()) continue;
            std::erase_if(shown->landed_transports, [&](const auto& value) { return value.id == row.id; });
        }
        native_owner.dirty[static_cast<std::size_t>(StateSection::observer)] = true;
    }
    native_owner.population_stale = true; native_owner.invalidate_observer_projection();
    return accepted;
}

template<class Owner> GalacticRejectReason validate_native_landing(Owner& native_owner,
    PlayerId player, FleetId id, PlanetId planet, bool pending_local_conflict) {
    if (std::none_of(native_owner.seats.begin(), native_owner.seats.end(), [&](const auto& row) { return row.player == player && row.active; })) return GalacticRejectReason::inactive_seat;
    if (!native_owner.control || !native_owner.setup.fleet_control) return GalacticRejectReason::unknown_subject;
    if (native_owner.clock.pause_depth) return GalacticRejectReason::paused;
    const auto humans = std::count_if(native_owner.seats.begin(), native_owner.seats.end(), [](const auto& row) { return row.kind == SeatKind::human; });
    const auto& gates = native_owner.setup.fleet_control->gates;
    if (humans == 1 && pending_local_conflict && (gates.queued_campaign_conflict || gates.active_speech)) return GalacticRejectReason::movement_blocked;
    const auto found = native_owner.fleets.find(id); if (found == native_owner.fleets.end()) return GalacticRejectReason::unknown_subject;
    const auto& fleet = native_owner.registry.template get<session_detail::FleetData>(found->second);
    if (fleet.owner != player) return GalacticRejectReason::not_authorized;
    const auto* supplied = native_find_row(native_owner.control->planets, planet, [](const auto& row) { return row.planet; });
    if (!supplied) return GalacticRejectReason::unknown_value;
    if (fleet.orbit != planet || fleet.motion.transit || !supplied->surface_accessible) return GalacticRejectReason::incompatible_location;
    return GalacticRejectReason::none;
}

template<class Owner> core::Result<GalacticSession::NativeInvasionReceipt> admit_native_invasion_rows(
    Owner& native_owner, PlayerId player, FleetId id, std::span<const EntityId> retained_transports, bool pending_local_conflict) {
    using NativeInvasion = GalacticSession::NativeInvasion;
    using NativeInvasionReceipt = GalacticSession::NativeInvasionReceipt;
    using Result = core::Result<NativeInvasionReceipt>;
    if (retained_transports.size() > (galactic_replay_max_payload - 72) / 8)
        return native_control_error<NativeInvasionReceipt>("retained invasion selection exceeds the journal budget");
    const auto fleet_at = native_owner.fleets.find(id);
    const auto planet = fleet_at == native_owner.fleets.end() ? PlanetId{} : native_owner.registry.template get<session_detail::FleetData>(fleet_at->second).orbit;
    const bool raid = fleet_at != native_owner.fleets.end()
        && native_owner.registry.template get<session_detail::FleetData>(fleet_at->second).raid_destination == planet;
    if (native_owner.failed || (!raid && !native_owner.native_invasions_enabled && !native_owner.native_applying)) return native_control_error<NativeInvasionReceipt>("native invasion requires its campaign conflict owner");
    if (const auto reason = validate_native_landing(native_owner, player, id, planet, pending_local_conflict); reason != GalacticRejectReason::none) return Result::success({false, 0, false, reason});
    const auto& fleet = native_owner.registry.template get<session_detail::FleetData>(fleet_at->second);
    NativeInvasion request; request.invader = player; request.fleet = id; request.planet = planet;
    const auto retain = [&](const FleetMember& member) {
        const auto* trait = native_find_row(native_owner.setup.fleet_control->types, member.type, [](const auto& row) { return row.type; });
        if (trait && trait->transport && std::none_of(request.transports.begin(), request.transports.end(),
            [&](const auto& row) { return row.id == member.id; }))
            request.transports.push_back(member);
    };
    if (retained_transports.empty()) for (const auto& member : fleet.members) retain(member);
    else for (const auto transport : retained_transports) {
        const auto member = std::find_if(fleet.members.begin(), fleet.members.end(), [&](const auto& row) { return row.id == transport; });
        if (member != fleet.members.end()) retain(*member);
    }
    if (request.transports.empty()) return Result::success({false, 0, false, GalacticRejectReason::invalid_membership});
    if (native_owner.native_invasions.size() >= galactic_max_objects || native_owner.next_native_invasion == std::numeric_limits<std::uint64_t>::max()) return native_control_error<NativeInvasionReceipt>("native invasion identity budget exhausted");
    if constexpr (requires { native_owner.native_invasion_range_end; }) {
        if (native_owner.next_native_invasion >= native_owner.native_invasion_range_end)
            return native_control_error<NativeInvasionReceipt>("native player invasion identity block exhausted");
    }
    const auto* surface = native_find_row(native_owner.control->planets, planet, [](const auto& row) { return row.planet; });
    // WGS-186: sparse adapters select the target state by planet identity.
    const auto* state = native_find_row(native_owner.control->state, planet, [](const auto& row) { return row.planet; });
    assert(state);
    request.defender = surface->ground_base.level ? surface->ground_base.owner : state->target_player;
    const auto* owner = native_find_row(native_owner.control->players, request.defender, [](const auto& row) { return row.player; });
    if (!surface->ground_base.level && (!request.defender || (owner && owner->neutral)) && !surface->landed.empty())
        request.defender = surface->landed.front().owner;
    const bool queued = request.defender != player;
    if (queued && request.transports.size() > galactic_max_objects - native_owner.native_invasion_transport_count) return native_control_error<NativeInvasionReceipt>("native invasion transport inventory exceeds queue budget");
    if (queued) native_owner.native_invasion_transport_count += request.transports.size();
    else request.transports.clear(); // equal owners inserted no explicit invasion queue
    request.state = queued ? NativeInvasion::State::pending : NativeInvasion::State::completed;
    request.request = native_owner.next_native_invasion++;
    const auto number = request.request;
    native_owner.native_invasions.emplace(number, std::move(request));
    if (queued) native_owner.native_invasion_queue.push_back(number);
    native_owner.dirty[static_cast<std::size_t>(StateSection::native_invasions)] = true;
    const NativeInvasionReceipt receipt{true, number, queued};
    return Result::success(receipt);
}

struct NativeLandingAdmission {
    GalacticSession::NativeFleetReceipt receipt{};
    bool published{};
};
template<class Owner>
core::Result<NativeLandingAdmission> land_native_transports_rows(Owner& native_owner, PlayerId player, FleetId id,
    bool pending_local_conflict, std::vector<GalacticEvent>& events) {
    using Result = core::Result<NativeLandingAdmission>;
    if (native_owner.failed || !native_owner.control || !native_owner.setup.fleet_control)
        return native_control_error<NativeLandingAdmission>("native landing requires its live control owner");
    if (std::none_of(native_owner.seats.begin(), native_owner.seats.end(), [&](const auto& seat) { return seat.player == player && seat.active; }))
        return Result::success({{false, id, GalacticRejectReason::inactive_seat}, false});
    if (native_owner.clock.pause_depth) return Result::success({{false, id, GalacticRejectReason::paused}, false});
    const auto humans = std::count_if(native_owner.seats.begin(), native_owner.seats.end(), [](const auto& seat) { return seat.kind == SeatKind::human; });
    const auto& gates = native_owner.setup.fleet_control->gates;
    if (humans == 1 && pending_local_conflict && (gates.queued_campaign_conflict || gates.active_speech))
        return Result::success({{false, id, GalacticRejectReason::movement_blocked}, false});
    const auto found = native_owner.fleets.find(id);
    if (found == native_owner.fleets.end()) return Result::success({{false, id, GalacticRejectReason::unknown_subject}, false});
    auto& fleet = native_owner.registry.template get<session_detail::FleetData>(found->second);
    if (fleet.owner != player) return Result::success({{false, id, GalacticRejectReason::not_authorized}, false});
    if (!fleet.orbit || fleet.motion.transit || fleet.members.empty())
        return Result::success({{false, id, GalacticRejectReason::incompatible_location}, false});
    const auto* supplied = native_find_row(native_owner.control->planets, fleet.orbit, [](const auto& row) { return row.planet; });
    if (!supplied) return native_control_error<NativeLandingAdmission>("native landing has no surface input");
    // WGA-234: an admitted empty landing block still returns its actor. The
    // ground-ability callback remains a named deployment capability.
    if (!supplied->surface_accessible || supplied->landed.size() == native_owner.control->max_ground_forces)
        return Result::success({{true, id}, false});
    const auto owner = native_owner.registry.template get<session_detail::PlanetData>(native_owner.planets.at(fleet.orbit)).owner;
    if (!native_allied(*native_owner.control, player, owner)) return Result::success({{true, id}, false});
    // WGS-187: mixed surface ownership does not refuse the link.
    auto& planet = native_owner.control->planets[static_cast<std::size_t>(supplied - native_owner.control->planets.data())];
    const auto landed_before = planet.landed.size();
    for (std::size_t i = fleet.members.size(); i != 0 && planet.landed.size() != native_owner.control->max_ground_forces;) {
        const auto member = fleet.members[--i];
        const auto* trait = native_find_row(native_owner.setup.fleet_control->types, member.type, [](const auto& row) { return row.type; });
        if (!trait || !trait->transport || !native_control_type(*native_owner.control, member.type)) continue;
        planet.landed.push_back({member.id, member.type, player});
        native_owner.relocate_actor(member.id, player, planet.planet, planet.planet);
        fleet.members.erase(fleet.members.begin() + static_cast<std::ptrdiff_t>(i));
        events.push_back({native_owner.completed_tick, GalacticEventKind::member_detached, {}, member.id, 0, id, GalacticRejectReason::none});
    }
    native_owner.publish_surface_arrivals(planet.planet,
        std::span<const SurfaceForce>(planet.landed).subspan(landed_before));
    if (fleet.members.empty()) {
        native_owner.registry.destroy(found->second); native_owner.fleets.erase(found); --native_owner.live_object_count;
        native_owner.lose_actor(id);
        events.push_back({native_owner.completed_tick, GalacticEventKind::fleet_destroyed, {}, id, 0, 0, GalacticRejectReason::none});
    }
    native_owner.dirty[static_cast<std::size_t>(StateSection::fleets)] = true;
    native_owner.dirty[static_cast<std::size_t>(StateSection::fleet_control)] = true;
    native_owner.dirty[static_cast<std::size_t>(StateSection::control)] = true;
    if (native_owner.setup.movement) native_owner.dirty[static_cast<std::size_t>(StateSection::movement)] = true;
    native_owner.population_stale = true; native_owner.invalidate_observer_projection();
    return Result::success({{true, id}, true});
}

struct NativeActorLandingAdmission {
    GalacticSession::NativeTransportLandingReceipt receipt{};
    bool published{};
};
template<class Owner>
core::Result<NativeActorLandingAdmission> land_native_transport_rows(Owner& native_owner, PlayerId player, EntityId actor,
    PlanetId id, bool pending_local_conflict, std::optional<std::uint32_t> live_landed, std::vector<GalacticEvent>& events) {
    using Result = core::Result<NativeActorLandingAdmission>;
    if (native_owner.failed || !native_owner.control || !native_owner.setup.fleet_control)
        return native_control_error<NativeActorLandingAdmission>("direct transport landing requires its live world owner");
    GalacticSession::NativeTransportLandingReceipt receipt{{false, actor}, false};
    const auto humans = std::count_if(native_owner.seats.begin(), native_owner.seats.end(), [](const auto& seat) { return seat.kind == SeatKind::human; });
    const auto& gates = native_owner.setup.fleet_control->gates;
    if (native_owner.clock.pause_depth || (humans <= 1 && pending_local_conflict && (gates.queued_campaign_conflict || gates.active_speech))) {
        receipt.event.reason = GalacticRejectReason::paused; return Result::success({receipt, false});
    }
    const auto actual = native_owner.actor_records.find(actor);
    if (!native_owner.planets.contains(id) || actual == native_owner.actor_records.end() || actual->second.delete_pending || actual->second.dead) {
        receipt.event.reason = GalacticRejectReason::unknown_subject; return Result::success({receipt, false});
    }
    const auto* trait = native_find_row(native_owner.setup.fleet_control->types, actual->second.current_type,
        [](const auto& row) { return row.type; });
    if (!trait || !trait->transport) { receipt.event.reason = GalacticRejectReason::invalid_membership; return Result::success({receipt, false}); }
    if (actual->second.owner != player) { receipt.event.reason = GalacticRejectReason::not_authorized; return Result::success({receipt, false}); }
    const auto supplied = native_find_row(native_owner.control->planets, id, [](const auto& row) { return row.planet; });
    if (!supplied) return native_control_error<NativeActorLandingAdmission>("direct landing surface input unavailable");
    auto& surface = native_owner.control->planets[static_cast<std::size_t>(supplied - native_owner.control->planets.data())];
    const auto actual_landed = native_landed_count(surface, native_owner.actor_records);
    assert(!live_landed || *live_landed == actual_landed);
    (void)live_landed;
    const auto detached = native_owner.detached_units.find(actor);
    // WGA-413: failed link leaves the earlier removal intact. No cargo or
    // original-type restriction is read, and formation-land still follows.
    if (actual_landed < native_owner.control->max_ground_forces && detached != native_owner.detached_units.end()
        && std::none_of(surface.landed.begin(), surface.landed.end(), [actor](const auto& unit) { return unit.id == actor; })) {
        surface.landed.push_back({actor, detached->second.member.type, player});
        native_owner.publish_surface_arrivals(id, std::span<const SurfaceForce>(surface.landed).last(1));
        native_owner.detached_units.erase(detached); native_owner.relocate_actor(actor, player, id, id);
        receipt.linked = true;
        native_owner.dirty[static_cast<std::size_t>(StateSection::fleet_control)] = true;
        native_owner.dirty[static_cast<std::size_t>(StateSection::control)] = true;
        if (native_owner.setup.movement) native_owner.dirty[static_cast<std::size_t>(StateSection::movement)] = true;
        native_owner.population_stale = true; native_owner.invalidate_observer_projection();
    }
    receipt.event.admitted = true;
    const auto seat = std::find_if(native_owner.seats.begin(), native_owner.seats.end(), [player](const auto& value) { return value.player == player; });
    events.push_back({native_owner.completed_tick, GalacticEventKind::formation_land,
        seat == native_owner.seats.end() ? SeatId{} : seat->seat_id, actor, 0, id, GalacticRejectReason::none});
    return Result::success({receipt, true});
}

} // namespace eawr::sim::galactic::session_detail
