#pragma once
#include "eawr/sim/galactic/observer.hpp"
#include "eawr/sim/galactic/types.hpp"
#include <algorithm>

namespace eawr::sim::galactic {
struct ObservedBlackMarketOffer {
    std::uint32_t index{}, item{};
    int tech{};
    math::Fixed price;
    bool owned{}, affordable{};
    bool affordability_available{true}; // false: fixed-point rounding overflow, not a known denial
    friend bool operator==(const ObservedBlackMarketOffer&, const ObservedBlackMarketOffer&) = default;
};
struct ObservedBlackMarketMenu {
    PlayerId player{};
    EntityId buyer{};
    PlanetId planet{};
    math::Fixed absence, credits;
    std::vector<ObservedBlackMarketOffer> offers;
    std::uint64_t preparation{}; // live host-issued identity; zero for synthetic display fixtures
    friend bool operator==(const ObservedBlackMarketMenu&, const ObservedBlackMarketMenu&) = default;
};
// WGEP-140/141: only copied active local-human menus. No preparation or pricing.
// Derived presentation facts do not contribute to canonical replay state.
inline std::vector<ObservedBlackMarketMenu> observed_black_market_menus(
    const EconomyCorruptionState& state, const PlayerEconomySetup& accounts,
    PlayerId local, std::span<const ObservedPlanet> observations) {
    std::vector<ObservedBlackMarketMenu> result;
    const auto player = std::find_if(state.players.begin(), state.players.end(),
        [local](const auto& row) { return row.player == local && row.human; });
    const auto account = std::find_if(accounts.players.begin(), accounts.players.end(),
        [local](const auto& row) { return row.player == local; });
    if (player == state.players.end() || account == accounts.players.end()) return result;
    for (const auto& menu : state.menus) {
        if (!menu.active || !std::any_of(state.buyers.begin(), state.buyers.end(),
            [&](const auto& buyer) { return buyer.buyer == menu.buyer && buyer.owner == local; })
            || !std::any_of(observations.begin(), observations.end(), [&](const auto& row) {
                return row.player == local && row.planet == menu.planet && row.discovered;
            })) continue;
        ObservedBlackMarketMenu value{local, menu.buyer, menu.planet, menu.absence, account->credits, {}};
        for (std::size_t index = 0; index < menu.offers.size(); ++index) {
            const auto& offer = menu.offers[index];
            const auto item = std::find_if(state.items.begin(), state.items.end(),
                [&](const auto& row) { return row.id == offer.item; });
            if (item == state.items.end()) continue; // Unresolved registry identity is unavailable.
            const auto enabled = corruption_fixture::menu_enabled(offer, account->credits);
            value.offers.push_back({static_cast<std::uint32_t>(index), offer.item, item->tech,
                offer.price, offer.owned, enabled && enabled.value(), static_cast<bool>(enabled)});
        }
        result.push_back(std::move(value));
    }
    return result;
}
} // namespace eawr::sim::galactic
