#pragma once
#include "eawr/sim/galactic/snapshot.hpp"

namespace eawr::viewer::galactic {
inline bool black_market_fixture(std::string_view name) {
    return name == "market" || name == "market-poor" || name == "market-fractional"
        || name == "market-overflow" || name == "market-foreign";
}
// Controlled copied facts exercise the widget; they never execute a campaign action.
inline std::shared_ptr<const sim::galactic::GalacticSnapshot> black_market_fixture_snapshot(
    sim::galactic::PlayerId local, std::string_view name) {
    namespace g = sim::galactic;
    const auto f = [](int value) { return sim::math::Fixed::from_integer(value).value(); };
    g::EconomyCorruptionState state;
    g::CorruptionEconomyPlayer player; player.player = local; player.human = true;
    state.players.push_back(player);
    g::BlackMarketBuyer buyer; buyer.buyer = 10; buyer.owner = name == "market-foreign" ? local + 10 : local;
    state.buyers.push_back(buyer);
    g::BlackMarketMenu menu; menu.buyer = 10; menu.planet = 101; menu.active = true; menu.absence = f(60);
    // Registry ordinal IDs match the loaded fixture's authored metadata, not runtime item names.
    for (std::uint32_t index = 0; index < 5; ++index) {
        g::corruption_fixture::MarketItem item; item.id = index * 2 + 1; item.tech = static_cast<int>(index + 1);
        state.items.push_back(item);
        menu.offers.push_back({item.id, f(static_cast<int>((index + 1) * 250)), index == 1});
    }
    if (name == "market-fractional") menu.offers[0].price = sim::math::Fixed::from_decimal("250.4").value();
    if (name == "market-overflow") {
        auto duplicate = state.items.front(); duplicate.id = 2; state.items.push_back(duplicate);
        menu.offers.insert(menu.offers.begin(), {2, f(200), false});
    }
    state.menus.push_back(menu);
    g::PlayerEconomySetup economy;
    g::GalacticPlayerEconomy account; account.player = local;
    account.credits = name == "market-poor" ? f(1) : f(1000);
    if (name == "market-fractional") account.credits = sim::math::Fixed::from_decimal("250.3").value();
    economy.players.push_back(account);
    g::ObserverSetup observer; observer.human_observers = {local}; observer.solo_reference_human = local;
    g::ObservedPlanet row; row.player = local; row.planet = 101; row.discovered = true;
    return std::make_shared<const g::GalacticSnapshot>(0, g::GalacticClock{}, std::vector<g::GalacticPlayer>{},
        std::vector<g::Planet>{}, std::vector<g::Route>{}, std::vector<g::Fleet>{}, std::vector<g::GalacticEvent>{},
        std::nullopt, std::nullopt, nullptr, std::nullopt, std::vector<g::Seat>{}, economy, std::nullopt,
        std::nullopt, std::vector<g::PoliticalControlEffect>{}, std::vector<g::PopulationTotal>{}, observer,
        std::nullopt, std::nullopt, std::vector<g::ObservedProductionOffer>{},
        std::make_shared<const std::vector<g::ObservedPlanet>>(std::vector{row}), std::nullopt, nullptr,
        std::nullopt, std::nullopt, state);
}
} // namespace eawr::viewer::galactic
