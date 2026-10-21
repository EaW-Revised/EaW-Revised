#pragma once

#include "hud_clock_fixture.hpp"

namespace eawr::viewer::galactic {
// Explicit synthetic inputs: the live session records these normally. The
// reference widget receives only its immutable, observer-authorized snapshot.
inline void configure_history_hud_fixture(sim::galactic::GalacticSetup& setup, sim::galactic::PlayerId local) {
    namespace gc = sim::galactic;
    configure_synthetic_hud_clock(setup);
    configure_synthetic_hud_production(setup, local);
    setup.census.emplace();
    for (const auto& player : setup.players) {
        setup.census->players.push_back({player.player_id, 50, false, {}});
        if (player.player_id != setup.players.back().player_id) setup.observer->history_players.push_back(player.player_id);
    }
    for (const auto id : {1ULL, 1000ULL, 1001ULL, 1002ULL, 1003ULL, 1004ULL}) {
        gc::CensusType type; type.type = id;
        type.population = sim::math::Fixed::from_raw((id == 1000 || id == 1003) ? sim::math::Fixed::scale : 0);
        type.history = gc::HistoryType{sim::math::Fixed::from_raw(id == 1 ? 7 * sim::math::Fixed::scale / 2 : 0), false, false, false};
        setup.census->types.push_back(type);
    }
    for (const auto& fleet : setup.fleets) for (const auto& member : fleet.members)
        setup.census->objects.push_back({member.id, member.type, fleet.owner, fleet.orbit, fleet.id,
            false, false, member.type, 0, gc::ActorCombatRole::ordinary, {true, false, false, {}}, false});
    std::sort(setup.census->objects.begin(), setup.census->objects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    for (auto& player : setup.player_economy->players)
        player.credits = sim::math::Fixed::from_raw((1000 + 250 * player.player) * sim::math::Fixed::scale + sim::math::Fixed::scale / 4);
}
} // namespace eawr::viewer::galactic
