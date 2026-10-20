#pragma once

#include "eawr/campaign/conflict.hpp"
#include "eawr/sim/galactic/snapshot.hpp"
#include <algorithm>
#include <memory>

namespace eawr::campaign {
// WGP-88 separates prepared identities from visible admission. WGP-BS-U05
// still needs a retail recording; absence never enables the summary button.
struct ObservedInvasionOffer {
    sim::EntityId conflict{};
    sim::galactic::PlayerId observer{};
    std::vector<InvasionOffer> candidates;
    std::vector<sim::galactic::MovementContents> cargo;
    bool visible_admitted{};
    bool accept_permitted{};
};

[[nodiscard]] inline ObservedInvasionOffer observe_invasion_offer(const BattleResult& result,
    const ConflictRecord& conflict, const sim::galactic::GalacticSnapshot& snapshot,
    sim::galactic::PlayerId observer, bool native_available, bool summary_pause = false) {
    ObservedInvasionOffer out;
    if (!observer || !result.winner || result.conflict != conflict.id
        || conflict.layer != BattleLayer::space || !snapshot.control() || !snapshot.movement()
        || !snapshot.fleet_control()) return out;
    const bool allied_winner = std::any_of(snapshot.control()->relations.begin(), snapshot.control()->relations.end(),
        [&](const auto& row) { return row.player == observer && row.other == result.winner && row.allied; });
    if (result.winner != observer && !allied_winner) return out;
    const auto planet = std::find_if(snapshot.planets().begin(), snapshot.planets().end(),
        [&](const auto& row) { return row.id == conflict.planet; });
    const auto surface = std::find_if(snapshot.control()->planets.begin(), snapshot.control()->planets.end(),
        [&](const auto& row) { return row.planet == conflict.planet; });
    if (planet == snapshot.planets().end() || planet->owner == observer
        || surface == snapshot.control()->planets.end() || !surface->surface_accessible || surface->destroyed) return out;
    out.conflict = result.conflict; out.observer = observer;
    for (const auto& prepared : result.invasion_offers) {
        if (prepared.player != result.winner || prepared.planet != conflict.planet) continue;
        const auto fleet = std::find_if(snapshot.fleets().begin(), snapshot.fleets().end(),
            [&](const auto& row) { return row.id == prepared.fleet; });
        if (fleet == snapshot.fleets().end() || fleet->owner != result.winner || fleet->orbit != prepared.planet || fleet->motion.transit) continue;
        InvasionOffer current{prepared.planet, prepared.fleet, result.winner, {}};
        for (const auto id : prepared.transports) {
            const auto member = std::find_if(fleet->members.begin(), fleet->members.end(), [&](const auto& row) { return row.id == id; });
            if (member == fleet->members.end()) continue;
            const auto& types = snapshot.fleet_control()->types;
            const auto type = std::find_if(types.begin(), types.end(), [&](const auto& row) { return row.type == member->type; });
            const auto& contents = snapshot.movement()->contents;
            const auto cargo = std::find_if(contents.begin(), contents.end(), [&](const auto& row) { return row.container == id; });
            if (type != types.end() && type->transport && cargo != contents.end() && !cargo->children.empty()) {
                current.transports.push_back(id); out.cargo.push_back(*cargo);
            }
        }
        if (!current.transports.empty()) out.candidates.push_back(std::move(current));
    }
    // Allied preparation is sourced; permission to command another player's
    // retained transports remains with the command owner, never this projection.
    out.accept_permitted = result.winner == observer && native_available && !out.candidates.empty() && snapshot.clock().pause_depth == (summary_pause ? 1U : 0U);
    return out;
}

// Derived publication ownership only: no canonical state or simulation work.
// The snapshot generation covers ordered ownership/orbit/membership/observer
// commits; movement identity also covers replacement of direct cargo inputs.
class InvasionOfferPublication {
public:
    [[nodiscard]] std::shared_ptr<const ObservedInvasionOffer> publish(const BattleResult* result,
        const ConflictRecord* conflict, const sim::galactic::GalacticSnapshot& snapshot,
        sim::galactic::PlayerId observer, bool native_available, std::uint64_t selection_revision, bool summary_pause = false) {
        if (!result || !conflict) { publication_.reset(); valid_ = false; return {}; }
        const auto planet = std::lower_bound(snapshot.planets().begin(), snapshot.planets().end(), conflict->planet,
            [](const auto& row, const auto id) { return row.id < id; });
        const bool planet_present = planet != snapshot.planets().end() && planet->id == conflict->planet;
        bool surface_present{}, accessible{}, destroyed{}, allied_winner{};
        if (snapshot.control()) {
            const auto& planets = snapshot.control()->planets;
            const auto surface = std::lower_bound(planets.begin(), planets.end(), conflict->planet,
                [](const auto& row, const auto id) { return row.planet < id; });
            surface_present = surface != planets.end() && surface->planet == conflict->planet;
            if (surface_present) { accessible = surface->surface_accessible; destroyed = surface->destroyed; }
            const auto& relations = snapshot.control()->relations;
            const auto relation = std::lower_bound(relations.begin(), relations.end(), std::pair{observer, result->winner},
                [](const auto& row, const auto ids) { return std::pair{row.player, row.other} < ids; });
            allied_winner = relation != relations.end() && relation->player == observer
                && relation->other == result->winner && relation->allied;
        }
        const Key key{selection_revision, result->conflict, conflict->id, result->winner, conflict->planet, conflict->layer,
            snapshot.observer_input_generation(), snapshot.movement(), observer, native_available, snapshot.clock().pause_depth,
            planet_present, planet_present ? planet->owner : 0, surface_present, accessible, destroyed, allied_winner,
            snapshot.fleet_control().has_value(), summary_pause};
        if (valid_ && key == key_) return publication_;
        key_ = key; valid_ = true;
        ++gathers_;
        publication_ = std::make_shared<const ObservedInvasionOffer>(
            observe_invasion_offer(*result, *conflict, snapshot, observer, native_available, summary_pause));
        return publication_;
    }
    [[nodiscard]] std::uint64_t gathers() const noexcept { return gathers_; }
private:
    struct Key {
        std::uint64_t selection_revision{}, conflict{}, context{};
        sim::galactic::PlayerId winner{};
        sim::galactic::PlanetId planet{};
        BattleLayer layer{};
        std::uint64_t generation{};
        const sim::galactic::MovementSetup* movement{};
        sim::galactic::PlayerId observer{};
        bool native_available{};
        std::uint32_t pause_depth{};
        bool planet_present{};
        sim::galactic::PlayerId owner{};
        bool surface_present{}, accessible{}, destroyed{}, allied_winner{}, fleet_traits{}, summary_pause{};
        friend bool operator==(const Key&, const Key&) = default;
    };
    Key key_;
    bool valid_{};
    std::uint64_t gathers_{};
    std::shared_ptr<const ObservedInvasionOffer> publication_;
};
} // namespace eawr::campaign
