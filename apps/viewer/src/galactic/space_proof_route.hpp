#pragma once

#include "eawr/sim/galactic/graph.hpp"
#include "eawr/sim/galactic/snapshot.hpp"

#include <algorithm>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace eawr::viewer::galactic {
struct SpaceProofRoute {
    sim::galactic::FleetId fleet{};
    std::vector<sim::galactic::PlanetId> path;
};

// Diagnostic driver only: inspect the authored publication, then submit the
// selected path through ordinary Move/Fight admission. Never change forces.
[[nodiscard]] inline std::optional<SpaceProofRoute> space_proof_route(
    const sim::galactic::GalacticSnapshot& snapshot, const sim::galactic::PlayerId local,
    const sim::galactic::PlayerId neutral) {
    namespace gc = sim::galactic;
    const auto hostile = [&](gc::PlayerId owner) {
        if (!owner || owner == local || owner == neutral) return false;
        if (const auto* control = snapshot.control())
            for (const auto& relation : control->relations)
                if (relation.player == local && relation.other == owner) return !relation.allied;
        return true;
    };
    const auto fleets = snapshot.fleets();
    const auto planets = snapshot.planets();
    const auto opposing_fleet = [&](gc::PlanetId planet) {
        return std::any_of(fleets.begin(), fleets.end(), [&](const auto& fleet) {
            return hostile(fleet.owner) && fleet.orbit == planet && !fleet.members.empty();
        });
    };
    // Preserve the established adjacent-fleet fixture's fleet/route ordering.
    for (const auto& fleet : fleets) {
        if (fleet.owner != local || fleet.members.empty() || fleet.motion.transit) continue;
        for (const auto& route : snapshot.routes()) {
            if (route.from != fleet.orbit && route.to != fleet.orbit) continue;
            const auto destination = route.from == fleet.orbit ? route.to : route.from;
            const auto target = std::find_if(planets.begin(), planets.end(),
                [&](const auto& row) { return row.id == destination; });
            if (target != planets.end() && target->owner != fleet.owner && target->owner != neutral
                && std::any_of(fleets.begin(), fleets.end(), [&](const auto& other) {
                    return other.owner != fleet.owner && other.owner != neutral
                        && other.orbit == destination && !other.members.empty();
                }))
                return SpaceProofRoute{fleet.id, {fleet.orbit, destination}};
        }
    }
    const auto* movement = snapshot.movement();
    if (!movement || !snapshot.maximum_movement_distance()) return std::nullopt;
    auto graph = gc::PlanetGraph::build(planets, snapshot.routes(), *snapshot.maximum_movement_distance());
    if (!graph) return std::nullopt;
    std::vector<gc::PathPlanet> access;
    for (const auto& node : graph.value().nodes()) {
        const auto admitted = std::find_if(movement->access.begin(), movement->access.end(),
            [&](const auto& row) { return row.player == local && row.planet == node.id; });
        gc::PathPlanet row; row.id = node.id;
        // This authored-state diagnostic has complete facts. The published
        // movement access still gates every hop and every ordinary order.
        row.revealed = admitted != movement->access.end() && admitted->accessible;
        row.fleet_presence_visible = admitted != movement->access.end() && admitted->potential_conflict;
        row.nonallied_fleet = row.fleet_presence_visible;
        access.push_back(row);
    }
    const auto space_defender = [&](gc::PlanetId planet) {
        // A station need not have a fleet row. This opt-in authored-campaign
        // diagnostic reads the published control/production station facts to
        // choose a test destination; ordinary path access and Move/Fight still
        // own admission. It neither exposes these facts in the player UI nor
        // manufactures forces to make a tactical battle possible.
        if (opposing_fleet(planet)) return true;
        if (const auto* control = snapshot.control())
            for (const auto& row : control->planets)
                if (row.planet == planet && row.starbase.level && hostile(row.starbase.owner)) return true;
        if (snapshot.production()) for (const auto& row : snapshot.production()->planets) {
            if (row.planet != planet) continue;
            if (row.starbase_type && hostile(row.starbase_owner)) return true;
            for (const auto& object : row.objects)
                if (object.domain == gc::ProductionDomain::space && object.kind != gc::ProductionKind::technology
                    && hostile(object.owner)) return true;
        }
        return false;
    };
    gc::PathOptions options; options.allow_bad_final = true;
    options.multiplayer = snapshot.victory() && snapshot.victory()->context.multiplayer;
    std::optional<SpaceProofRoute> best;
    sim::math::Fixed best_cost{};
    for (const auto& planet : planets) {
        if ((!hostile(planet.owner) && !opposing_fleet(planet.id)) || !space_defender(planet.id)) continue;
        for (const auto& fleet : fleets) {
            if (fleet.owner != local || !fleet.orbit || fleet.members.empty() || fleet.motion.transit
                || fleet.orbit == planet.id) continue;
            auto path = gc::find_planet_path(graph.value(), fleet.orbit, planet.id, access, options);
            if (!path || path.value().planets.size() < 2) continue;
            const auto key = std::tuple{path.value().cost, planet.id, fleet.id};
            if (best && key >= std::tuple{best_cost, best->path.back(), best->fleet}) continue;
            best_cost = path.value().cost;
            best = SpaceProofRoute{fleet.id, std::move(path).value().planets};
        }
    }
    return best;
}
} // namespace eawr::viewer::galactic
