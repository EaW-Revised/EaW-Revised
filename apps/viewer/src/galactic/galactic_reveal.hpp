#pragma once

#include "eawr/sim/galactic/observer.hpp"
#include "eawr/sim/galactic/snapshot.hpp"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace eawr::viewer::galactic {

// #3931: debug/preview galaxy reveal (--eawr-galactic-reveal on). The local
// faction's published observer is replaced by an omniscient one: every planet
// discovered, every intelligence category known. Every presentation reader
// (map, radar, planet and fleet HUDs; not the command paths) filters through the observer, so this one
// derived view reveals the galaxy for all of them.
//
// Draw-only. The result is a read view over the truth publication: the
// simulation, its AI observers, saves, digests and replays never see it. Keep
// the truth publication for anything that gates a player command (fleet and
// ground-launch commands, queue cancels, sales and their rosters); only planet
// selection follows the revealed discovery, so an undiscovered planet can be
// inspected (view only).
//
// Single-human only: the reveal would show one seat everything the other humans
// hide, so a multi-human (coop or LAN) campaign refuses it.
[[nodiscard]] inline const char* reveal_refusal(bool reveal, std::size_t human_seats) noexcept {
    return reveal && human_seats > 1 ? "--eawr-galactic-reveal is single-player only; refused in a multiplayer campaign" : nullptr;
}

[[nodiscard]] inline std::shared_ptr<const sim::galactic::GalacticSnapshot> reveal_snapshot(
    std::shared_ptr<const sim::galactic::GalacticSnapshot> truth) {
    if (!truth || !truth->observer()) return truth;
    auto observer = *truth->observer();
    for (auto& record : observer.records) {
        if (record.player != truth->view_player()) continue;
        record.discovered = true;
        record.mask = sim::galactic::all_intelligence;
    }
    // Stealth hides contents and ownership from a faction that lacks detection; the reveal lifts it.
    for (auto& planet : observer.planets) planet.space_stealth = planet.ground_stealth = false;
    for (auto& type : observer.types) type.stealth_object = type.stealth_hero = false;
    sim::galactic::NativeSnapshotChanges changes;
    changes.observations = std::make_shared<const std::vector<sim::galactic::ObservedPlanet>>(
        sim::galactic::observed_planets(observer, truth->fleets(), {truth->view_player()}, truth->movement(), truth->actor_records()));
    changes.observer = std::move(observer);
    return sim::galactic::GalacticSnapshot::native_view(std::move(truth), std::move(changes));
}

} // namespace eawr::viewer::galactic
