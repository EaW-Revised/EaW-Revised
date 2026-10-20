#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include "../support/check.hpp"
#include "eawr/sim/galactic/corruption_model.hpp"

#include <iostream>
#include <limits>
#include <exception>

using namespace eawr::sim::galactic::corruption_fixture;
namespace {
void check(bool value, const char* message) {
    if (!value) eawr::testing::require(value, message);
}
Fixed f(const char* value) { return Fixed::from_decimal(value).value(); }
void transitions() {
    for (unsigned i = 1; i <= 8; ++i) {
        Planet p; p.corrupting_player = 7; p.target_allegiance = 8;
        Player a; a.id = 7; a.credits = f("100");
        Player b; b.id = 8; b.credits = f("100");
        a.automatic_upgrades[i] = 40; b.automatic_upgrades[i] = 41;
        p.rewards[i] = {f("12"), 60, 70, f("1")};
        const auto type = static_cast<Type>(i);
        check(begin(p.transition, type, f("0.15"), 30, 10).value(), "increase admitted");
        check(p.transition.start == 10 && p.transition.end == 14 && p.transition.increasing, "truncated deadline");
        check(!begin(p.transition, type, {}, 30, 10).value(), "overlap rejected");
        std::vector<Player> players{a, b};
        check(complete(p, players, 13, Id{7}).value().empty(), "before deadline no effects");
        const auto events = complete(p, players, 14, Id{7}).value();
        const std::vector<Event> expected{
            {Effect::refresh_visuals}, {Effect::story_increased}, {Effect::local_sound, 7},
            {Effect::add_automatic_upgrade, 7, 40}, {Effect::add_automatic_upgrade, 8, 41},
            {Effect::credit_reward, 7}, {Effect::add_tactical_upgrade, 7, 60},
            {Effect::add_build_unlock, 7, 70}, {Effect::notify_abilities, 0, 0, Type::none, type}};
        check(events == expected, "ordered rewards for all players");
        check(players[0].credits == f("112") && players[1].credits == f("100"), "recipient-only money");
        check(p.transition == Transition{type, Type::none, false, false, 0, 0, 14}, "completion reset");
        check(!begin(p.transition, Type::piracy, one, 30, 14).value(), "cannot replace existing corruption");
        check(!remove(p, players[0], f("10"), one, {}, 30, 14).value(), "owner not target allegiance");
        check(remove(p, players[1], f("10"), one, {}, 30, 14).value(), "target allegiance removes");
        check(players[1].credits == f("90") && p.transition.end == 17, "zero-removal substitution");
        const auto removed = complete(p, players, 17, std::nullopt).value();
        check(removed[2] == Event{Effect::remove_automatic_upgrade, 7, 40}
            && removed[3] == Event{Effect::remove_automatic_upgrade, 8, 41}, "remove all players upgrades");
        check(p.tactical_upgrades.empty() && p.build_unlocks.empty(), "remove planet rewards");
        check(players[0].credits == f("112"), "removal has no reward");
        check(complete(p, players, 18, Id{7}).value().empty(), "completion only once");
    }
    Transition state;
    check(!begin(state, Type::none, {}, 30, 0).value(), "remove none rejected");
    check(!begin(state, Type::piracy, f("-1"), 30, 0), "negative duration invalid");
    check(!begin(state, Type::piracy, one, 0, 0), "zero fps invalid");
    check(!begin(state, Type::piracy, one, 30, std::numeric_limits<Frame>::max()), "deadline overflow invalid");
    check(state == Transition{}, "invalid duration does not mutate");
    check(begin(state, Type::piracy, {}, 30, 10).value() && state.end == 10, "zero increase immediate");
    Planet p; p.transition.current = Type::piracy; p.target_allegiance = 1;
    Player actor; actor.id = 1; actor.credits = f("1");
    check(!remove(p, actor, f("2"), one, one, 30, 0).value() && actor.credits == one, "insufficient removal no charge");
    check(remove(p, actor, f("-2"), one, one, 30, 0).value() && actor.credits == one, "negative cost zero");
    check(!remove(p, actor, one, one, one, 30, 0).value() && actor.credits == one, "failed start no charge");
}
void revolt_and_errors() {
    check(completion_sound({7, 8, true, true, true, true}) == Id{7}, "local owner sound precedence");
    check(completion_sound({7, 8, false, true, true, true}) == Id{8}, "local beneficiary sound fallback");
    check(!completion_sound({7, 8, true, true, false, true}), "absent owner event starts nothing");
    check(!completion_sound({7, 8, false, false, true, true}), "nonlocal completion silent");
    Planet p; p.transition.current = Type::militia; p.neutral_player = 99;
    p.orbital_owners = {8, 8, 3}; p.landed_owners = {3, 9, 8};
    p.ground_specials = 3; p.space_specials = 4; p.starbase_level = 5;
    p.political_modifiers = {one};
    check(begin(p.transition, Type::none, one, 30, 0).value(), "militia removal start");
    std::vector<Player> players;
    const auto events = complete(p, players, 30, std::nullopt).value();
    const std::vector<Event> expected{
        {Effect::refresh_visuals}, {Effect::story_decreased}, {Effect::notify_abilities, 0, 0, Type::militia, Type::none},
        {Effect::clear_revolt_structures},
        {Effect::surface_retreat, 8}, {Effect::orbital_retreat, 8},
        {Effect::surface_retreat, 3}, {Effect::orbital_retreat, 3},
        {Effect::surface_retreat, 9}, {Effect::orbital_retreat, 9},
        {Effect::clear_political_modifiers}, {Effect::neutral_allegiance, 99}};
    check(events == expected, "revolt owner dedup/traversal and surface-before-orbit");
    check(!p.ground_specials && !p.space_specials && !p.starbase_level
        && p.political_modifiers.empty() && p.target_allegiance == 99 && p.allegiance == one, "revolt fixture mutation");
    p = {}; p.corrupting_player = 1; p.rewards[2].credits = one;
    Player player; player.id = 1; player.credits = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    players = {player};
    check(begin(p.transition, Type::piracy, {}, 30, 10).value(), "overflow reward fixture start");
    const auto before = p.transition;
    check(!complete(p, players, 10, std::nullopt), "reward overflow reported");
    check(p.transition == before && players[0].credits == player.credits, "overflow leaves state unchanged");
    p.rewards[2] = {f("-2"), 6, 7, {}};
    check(!complete(p, players, 10, std::nullopt).value().empty(), "nonpositive rewards handled");
    check(p.build_unlocks.empty() && p.tactical_upgrades.contains(6), "positive-cost unlock gate");
}
void recurring_income() {
    IncomeInput input; input.owner = 1; input.pre_siphon_income = f("100");
    input.corruption = Type::racketeering; input.corruption_factors[4] = f("4.5");
    input.siphon = {true, 3, f("0.25"), 10}; input.now = 11; input.piracy_accrual = f("7");
    const std::vector<IncomePlayer> players{{1, false}, {2, true}, {3, false}, {4, true}};
    const std::array<const char*, 8> factors{"0.5", "0.8", "0.4", "4.5", "0.5", "0.5", "0.8", "1"};
    for (std::size_t i = 0; i < factors.size(); ++i) {
        auto fixture = input; fixture.corruption = static_cast<Type>(i + 1);
        fixture.corruption_factors[i + 1] = f(factors[i]); fixture.piracy_accrual = {};
        check(income(fixture, players, false).value().payments[1]
            == eawr::sim::math::multiply(f("100"), f(factors[i])).value(), "every authored corruption income slot");
    }
    const auto preview = income(input, players, false).value();
    check(preview.payments == std::vector<Fixed>{f("75"), f("457"), f("25"), f("457")}, "racketeering pre-siphon 4.5");
    check(preview.remaining_piracy == f("7") && preview.siphon_active, "preview retains piracy and corruption bypasses expiry");
    const auto collected = income(input, players, true).value();
    check(collected.payments == std::vector<Fixed>{f("75"), f("457"), f("25"), f("450")}
        && collected.remaining_piracy == Fixed{}, "collection first eligible beneficiary consumes piracy");
    input.corruption = Type::none; input.piracy_accrual = {};
    input.now = 10; check(income(input, players, false).value().payments[0] == f("75"), "siphon equality still active");
    input.now = 11; check(income(input, players, false).value().payments[0] == f("100"), "strict siphon expiry");
    input.now = 10; input.siphon.recipient = 77;
    check(income(input, players, false).value().payments[0] == f("75"), "non-sentinel recipient does not need a current player");
    input.siphon.recipient = 0;
    check(income(input, players, false).value().payments[0] == f("100"), "no-recipient sentinel zero siphon");
    input.siphon.fraction = f("1.01"); check(!income(input, players, false), "invalid siphon fraction rejected");
    input.siphon = {}; input.corruption = Type::piracy; input.corruption_factors[2] = f("-1");
    check(income(input, players, false).value().payments[1] == Fixed{}, "nonpositive corruption factor excluded");
    input.corruption_factors[2] = one; input.pre_siphon_income = f("-10");
    check(income(input, players, false).value().payments[1] == Fixed{}, "nonpositive beneficiary product excluded");
    check(next_income_factor(true, false, one, one, f("3"), f("0.25")).value() == f("1.5"), "post-payment reroll");
    check(next_income_factor(true, true, f("3"), one, f("3"), {}).value() == one, "destroyed reset");
    check(next_income_factor(false, true, f("2"), one, f("3"), {}).value() == f("2"), "unaffected unchanged");
    check(production_factor(false, true, one, f("0.9")) == f("0.9")
        && production_factor(true, true, one, f("0.9")) == one, "production corruption factors");
    const std::vector<Fixed> piracy_values{f("10"), f("20")};
    check(accrue_piracy(Type::piracy, false, false, f("2"), piracy_values).value() == f("32"), "original member piracy accrual");
    check(accrue_piracy(Type::piracy, true, false, f("2"), piracy_values).value() == f("2")
        && accrue_piracy(Type::piracy, false, true, f("2"), piracy_values).value() == f("2")
        && accrue_piracy(Type::militia, false, false, f("2"), piracy_values).value() == f("2"), "piracy departure gates");
}
void black_market() {
    MarketContext context; context.opposing_planet = true; context.playable_faction = true;
    context.black_market_corruption = true; context.owner_present = true; context.owner_faction = 10;
    context.buyer = 1; context.planet_modifier = f("10"); context.price_modifier = f("1.5");
    context.tech_purchase_multiplier = f("2");
    const std::vector<MarketItem> items{
        {50, 10, 3, 1, f("100"), f("7"), f("5.5")},
        {51, 20, 2, 2, f("80"), f("4"), one},
        {52, 10, 1, 1, f("10"), f("2"), one}};
    std::vector<MarketPlayer> players{{1, 30, 1}, {2, 10, 2}, {3, 20, 4}};
    auto result = offers(context, items, players, {50}).value();
    check(result.size() == 2 && result[0].item == 50 && result[1].item == 52, "authored ordering/faction filter");
    check(result[0].price == f("1815") && result[0].owned && result[1].price == f("60"), "ordinary uncommon and owned retained");
    check(!menu_enabled(result[0], f("9999")).value(), "owned menu disabled");
    context.multiplayer = true; context.human_buyer = true;
    check(offers(context, items, players, {}).value()[0].price == f("330"), "MP highest other tech");
    players[2].displayed_tech = 2;
    check(offers(context, items, players, {}).value()[0].price == f("1815"), "MP uncommon low highest");
    check(offers(context, items, std::span(players).first(1), {}).value()[0].price == f("1815"), "MP no other player uses maximum minus one");
    context.tutorial = true;
    check(offers(context, items, players, {}).value()[0].price == f("7"), "tutorial tech/cost skip ordinary factors");
    // WGEP-109: the header reads the item faction's player, or in multiplayer the highest other displayed tech.
    players = {{1, 30, 1}, {2, 10, 2}, {3, 20, 4}};
    check(menu_header_tech(false, 10, players) == 2, "solo header reads the item faction player");
    check(menu_header_tech(true, 10, players) == 4, "multiplayer header takes the highest other player");
    check(menu_header_tech(true, 20, players) == 2, "multiplayer header excludes the faction player itself");
    check(menu_header_tech(true, 10, std::span(players).subspan(1, 1)) == 2, "no other player keeps the faction value");
    check(!menu_header_tech(false, 99, players) && !menu_header_tech(true, 99, players), "unknown faction has no header");
    context.multiplayer = false; context.tutorial = false; context.planet_modifier = f("-1000");
    check(offers(context, items, players, {}).value()[0].price == Fixed{}, "negative price floored");
    context.opposing_planet = false; check(offers(context, items, players, {}).value().empty(), "target admission");
    Offer prepared{50, f("10.49"), false}; Player buyer; buyer.id = 1; buyer.credits = f("10.1");
    check(menu_enabled(prepared, buyer.credits).value(), "rounded UI admits");
    check(!purchase(buyer, prepared, true, true).value() && buyer.credits == f("10.1"), "exact execution rejects");
    buyer.credits = f("50");
    check(!purchase(buyer, prepared, false, true).value() && buyer.credits == f("50"), "buyer loss before debit");
    check(!purchase(buyer, prepared, true, false).value(), "target loss before debit");
    check(purchase(buyer, prepared, true, true).value(), "purchase success requests despawn");
    prepared.owned = true;
    check(purchase(buyer, prepared, true, true).value() && buyer.black_market_unlocks.size() == 1
        && buyer.credits == eawr::sim::math::subtract(eawr::sim::math::subtract(f("50"), prepared.price).value(), prepared.price).value(),
        "repeated offer pays deduplicated unlock");
    const std::vector<Fixed> low{f("10"), f("20"), f("30"), f("40"), f("50")};
    const std::vector<Fixed> high{f("12"), f("24"), f("36"), f("48"), f("60")};
    check(absence(2, low, high, f("0.5")).value() == f("22"), "buyer displayed tech minus one");
    check(absence(0, low, high, {}).value() == Fixed{}
        && absence(6, low, high, {}).value() == Fixed{}
        && absence(1, {}, {}, {}).value() == Fixed{}, "invalid/empty absence zero");
    check(!absence(1, low, high, f("2")), "invalid synchronized draw rejected");
}
} // namespace
int case_galactic_corruption_model_tests() {
    try { transitions(); revolt_and_errors(); recurring_income(); black_market(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "corruption fixture contracts passed\n";
    return 0;
}
