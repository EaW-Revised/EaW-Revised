#pragma once
// The advanced map's land tray model (GPD-14): which orbiting ground units the tray lists and whether its
// entries are enabled. The Godot nodes live in land_tray_hud.cpp.
#include "eawr/sim/galactic/fleet_intelligence.hpp"
#include "eawr/sim/galactic/control.hpp"
#include "map_input.hpp"
#include "planet_detail_layout.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace eawr::viewer::galactic {
// GPD-14: group locally controlled orbiting transports by disclosed type.
inline constexpr std::size_t land_tray_slots = 20;
[[nodiscard]] inline std::string land_tray_slot_name(const std::size_t slot) {
    return std::string("land_fleet_") + (slot < 10 ? "0" : "") + std::to_string(slot);
}
[[nodiscard]] inline bool land_tray_enabled(bool owner, const std::optional<std::size_t>& count,
    const std::optional<std::int64_t>& capacity) {
    return owner && count && capacity && *capacity >= 0 && *count < static_cast<std::size_t>(*capacity);
}
struct LandTrayEntry {
    sim::galactic::TypeId type{};
    std::size_t count{};
    bool enabled{};
    // The first disclosed company of this type and the orbit fleet that holds it: the unit a drop lands.
    ObjectId representative{}, fleet{};
};
[[nodiscard]] inline std::vector<LandTrayEntry> land_tray_entries(
    const std::vector<sim::galactic::ObservedFleet>& cards, ObjectId planet, bool enabled) {
    std::vector<LandTrayEntry> entries;
    for (const auto& card : cards) {
        if (!card.local || card.planet != planet || !card.orbit_slot || *card.orbit_slot > 2) continue;
        for (const auto& group : card.groups) {
            if (!group.transport || !group.type || !group.count) continue;
            auto found = std::find_if(entries.begin(), entries.end(),
                [&](const auto& entry) { return entry.type == *group.type; });
            if (found != entries.end()) found->count += group.count;
            else entries.push_back({.type = *group.type, .count = group.count, .enabled = enabled,
                .representative = group.representative.value_or(0), .fleet = group.fleet.value_or(0)});
        }
    }
    if (entries.size() > land_tray_slots) entries.resize(land_tray_slots);
    return entries;
}
[[nodiscard]] inline const LandTrayEntry* land_tray_entry(const std::vector<LandTrayEntry>& entries, std::size_t slot) {
    return slot < entries.size() && slot < land_tray_slots ? &entries[slot] : nullptr;
}
// GPD-22: a tray unit dropped on a unit pad is landed there. The drop is three ordinary commands, in
// this order: the pad's occupant is launched to orbit (GPD-23), the unit lands, and its marker is set.
// A refusal of the second or third command after the first one ran would cost the player the pad for
// nothing, so every admission the three commands make is checked here against the published snapshot
// first, and nothing is submitted unless all of them hold. The simulation stays the authority: it
// re-checks each command when it runs.
// Checked here (the refusals the three commands can return that the snapshot can answer): paused deploy and
// paused layout; the planet's true owner is the local player (SetGroundLayout); surface accessible; the unit is a
// local, landable transport in a non-transit orbit fleet at the planet; the occupant is a local landed transport on
// an unblockaded planet; capacity after the launch; the ground owner after the launch is allied with the lander
// (LandUnits); the marker is free once the occupant's entry is gone. Not answerable from a published snapshot, so
// they stay the documented residual race: the seat being active, the queued-conflict/speech gate on a single-human
// game, an exhausted object budget, and a detached-link clash.
enum class TrayDropResult : std::uint8_t {
    ok, view_only, paused, surface_closed, fixed_pad, no_unit, not_in_orbit, not_a_transport, occupant_missing,
    blockaded, no_capacity, marker_taken, not_planet_owner, alliance_blocked
};
struct TrayDropFacts {
    bool editable{};         // the local player owns the planet (detail_can_edit)
    bool deploy_allowed{};   // the pause policy admits a surface deploy (launch and land)
    bool layout_allowed{};   // the pause policy admits fleet management (marker)
};
struct TrayLanding {
    TrayDropResult result{TrayDropResult::no_unit};
    std::vector<sim::galactic::CommandPayload> commands; // empty unless result == ok
};
[[nodiscard]] inline TrayLanding plan_tray_landing(const sim::galactic::GalacticSnapshot& snapshot, PlayerId local,
    ObjectId planet, const LandTrayEntry& entry, const DetailPad& pad, const TrayDropFacts& facts) {
    using R = TrayDropResult;
    const auto refuse = [](R reason) { return TrayLanding{reason, {}}; };
    if (!facts.editable) return refuse(R::view_only);
    if (!facts.deploy_allowed || !facts.layout_allowed) return refuse(R::paused);
    const auto* control = snapshot.control();
    const auto& traits = snapshot.fleet_control();
    if (!control || !traits) return refuse(R::surface_closed);
    const auto surface = std::find_if(control->planets.begin(), control->planets.end(),
        [&](const auto& row) { return row.planet == planet; });
    if (surface == control->planets.end() || !surface->surface_accessible) return refuse(R::surface_closed);
    if (pad.fixed) return refuse(R::fixed_pad);
    const auto transport = [&](sim::galactic::TypeId type) {
        const auto row = std::find_if(traits->types.begin(), traits->types.end(),
            [&](const auto& value) { return value.type == type; });
        return row != traits->types.end() && row->transport;
    };
    // The unit: a disclosed company of a local fleet that is in orbit here and not in transit.
    if (!entry.representative || !entry.fleet) return refuse(R::no_unit);
    const auto fleets = snapshot.fleets();
    const auto fleet = std::find_if(fleets.begin(), fleets.end(), [&](const auto& row) { return row.id == entry.fleet; });
    if (fleet == fleets.end() || fleet->owner != local || fleet->orbit != planet || fleet->motion.transit)
        return refuse(R::not_in_orbit);
    const auto member = std::find_if(fleet->members.begin(), fleet->members.end(),
        [&](const auto& row) { return row.id == entry.representative; });
    if (member == fleet->members.end()) return refuse(R::not_in_orbit);
    const bool typed = std::any_of(control->types.begin(), control->types.end(),
        [&](const auto& row) { return row.type == member->type; });
    if (!typed || !transport(member->type)) return refuse(R::not_a_transport);
    if (!transport_can_land(snapshot, local, planet, *member)) return refuse(R::not_a_transport);
    const auto planets = snapshot.planets();
    const auto owner = std::find_if(planets.begin(), planets.end(), [&](const auto& row) { return row.id == planet; });
    if (owner == planets.end() || owner->owner != local) return refuse(R::not_planet_owner);
    // The occupant: launched first, so it must be launchable and its slot is free for the landing.
    std::optional<sim::EntityId> occupant;
    if (pad.object) {
        const auto landed = std::find_if(surface->landed.begin(), surface->landed.end(),
            [&](const auto& row) { return row.id == *pad.object; });
        if (landed == surface->landed.end() || landed->owner != local) return refuse(R::occupant_missing);
        if (!transport(landed->type)) return refuse(R::not_a_transport);
        const auto state = std::find_if(control->state.begin(), control->state.end(),
            [&](const auto& row) { return row.planet == planet; });
        if (state == control->state.end() || state->blockaded) return refuse(R::blockaded);
        occupant = landed->id;
    }
    // The landing is refused when the surface's ground owner (the base owner, else the first landed unit's owner) is
    // not allied with the lander. The first landed unit is read after the occupant has left.
    sim::galactic::PlayerId ground = surface->ground_base.level ? surface->ground_base.owner : 0;
    if (!ground) {
        const auto first = std::find_if(surface->landed.begin(), surface->landed.end(),
            [&](const auto& row) { return !occupant || row.id != *occupant; });
        if (first != surface->landed.end()) ground = first->owner;
    }
    if (ground) {
        const auto relation = std::find_if(control->relations.begin(), control->relations.end(),
            [&](const auto& row) { return row.player == local && row.other == ground; });
        if (relation == control->relations.end() || !relation->allied) return refuse(R::alliance_blocked);
    }
    // Capacity as the landing sees it, after the launch has run.
    const std::size_t on_surface = surface->landed.size() - (occupant ? 1U : 0U);
    if (control->max_ground_forces <= on_surface) return refuse(R::no_capacity);
    // The marker must be unique once the occupant's entry is gone.
    const auto taken = std::any_of(surface->ground_layout.begin(), surface->ground_layout.end(),
        [&](const auto& row) { return row.marker == pad.marker.record && (!occupant || row.object != *occupant); });
    if (taken) return refuse(R::marker_taken);
    TrayLanding plan{R::ok, {}};
    if (occupant) plan.commands.emplace_back(sim::galactic::LaunchFromSurface{{*occupant}, 0, 0});
    plan.commands.emplace_back(sim::galactic::LandUnits{planet, {entry.representative}});
    plan.commands.emplace_back(sim::galactic::SetGroundLayout{planet, {{entry.representative, pad.marker.record}}});
    return plan;
}
} // namespace eawr::viewer::galactic
