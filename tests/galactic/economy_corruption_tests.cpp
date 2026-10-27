#include <algorithm>
#include <cstdint>
#include <exception>
#include <span>
#include <vector>
#include <iostream>
#include <cstddef>
#include <string_view>
#include <string>
#include "../support/env.hpp"
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "eawr/sim/galactic/economy_corruption.hpp"
#include "eawr/sim/galactic/income.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/campaign/planet_economy.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "../../src/sim/galactic/encoding.hpp"

#include <filesystem>
#include <fstream>
#include <chrono>
#include "../support/temp_dir.hpp"
#include <limits>
#include <stdexcept>
#include <utility>
using eawr::testing::take;

namespace {
namespace gc = eawr::sim::galactic;
namespace cf = gc::corruption_fixture;
using eawr::sim::math::Fixed;
using galactic_test::expect;

Fixed f(std::int64_t n) { return take(Fixed::from_integer(n)); }
Fixed d(std::string_view n) { return take(Fixed::from_decimal(n)); }
void technology_price_math() {
    expect(take(gc::technology_purchase_multiplier({})) == f(1), "fresh technology-price list is one");
    const std::vector<gc::TechnologyPriceModifier> additive{{1, d("-0.5"), {}}, {2, d("0.25"), {}}};
    expect(take(gc::technology_purchase_multiplier(additive)) == d("0.75"), "technology-price contributions add without stacking categories");
    const std::vector<gc::TechnologyPriceModifier> nonpositive{{1, f(-1), {}}};
    expect(take(gc::technology_purchase_multiplier(nonpositive)) == d("0.0001"), "nonpositive technology-price multiplier uses sourced fallback");
    auto bad = additive; bad[1].source = 1;
    expect(!gc::technology_purchase_multiplier(bad), "duplicate source refuses");
    bad = additive; bad[0].source = 0;
    expect(!gc::technology_purchase_multiplier(bad), "null source refuses");
    bad = additive; bad[0].first_expired_frame = gc::galactic_max_ticks + 1;
    expect(!gc::technology_purchase_multiplier(bad), "unbounded expiry refuses");
    bad = {{1, Fixed::from_raw(std::int64_t{1} << 48), {}}, {2, Fixed::from_raw(-(std::int64_t{1} << 48)), {}}};
    expect(!gc::technology_purchase_multiplier(bad), "cancelling entries cannot hide unsafe post-expiry subset");
}
void technology_price_decode() {
    // Standalone save-block decode must validate without a later setup validation.
    gc::EconomyCorruptionState state;
    gc::CorruptionEconomyPlayer player; player.player = 1; player.faction = 1;
    player.technology_price_modifiers = {{900, d("-0.5"), 4}, {901, d("0.25"), {}}};
    player.tech_purchase_multiplier = d("0.75"); state.players = {player};
    const auto decode = [](const auto& value) {
        std::vector<std::uint8_t> bytes; gc::detail::encode_economy_corruption(bytes, value);
        return gc::detail::decode_economy_corruption(bytes);
    };
    expect(decode(state) == state, "self-consistent v2 ledger decodes directly");
    auto damaged = state; damaged.players[0].tech_purchase_multiplier = d("0.8");
    expect(!decode(damaged), "standalone decode rejects damaged cached multiplier");
    damaged = state; damaged.players[0].technology_price_modifiers.clear();
    expect(!decode(damaged), "standalone decode rejects stale cache with empty ledger");
    damaged = state; damaged.players[0].technology_price_modifiers[0].additive = Fixed::from_raw(std::numeric_limits<std::int64_t>::min());
    expect(!decode(damaged), "standalone decode rejects out-of-bound additive before summing");
    damaged = state;
    damaged.players[0].technology_price_modifiers = {{900, Fixed::from_raw(std::int64_t{1} << 48), 4},
        {901, Fixed::from_raw(-(std::int64_t{1} << 48)), {}}};
    damaged.players[0].tech_purchase_multiplier = f(1);
    expect(!decode(damaged), "standalone decode rejects cancelling ledger with unsafe expiry subset");
    state.players[0].technology_price_modifiers = {{900, f(-1), {}}};
    state.players[0].tech_purchase_multiplier = d("0.0001");
    expect(decode(state) == state, "standalone decode accepts sourced nonpositive fallback cache");
    state.players[0].tech_purchase_multiplier = {};
    expect(!decode(state), "standalone decode refuses nonpositive uncorrected cache");
}
gc::GalacticSetup setup() {
    gc::SyntheticGalaxyOptions options; options.players = 2; options.planets = 4;
    options.fleets_per_player = 1; options.members_per_fleet = 1; options.debug_commands = true;
    auto s = gc::synthetic_galaxy(options);
    s.rules.fiscal_cycle_frames = 3;
    s.player_economy = gc::PlayerEconomySetup{{{1, f(1000), f(10000), 2, 5}, {2, f(1000), f(10000), 2, 5}}};
    s.fiscal = gc::FiscalSetup{f(10000), {{1, true, f(1)}, {2, true, f(1)}}, {}};
    auto& state = s.economy_corruption.emplace();
    state.factor_min = f(1); state.factor_max = f(3);
    state.income_factors = {Fixed{}, d("0.5"), d("0.8"), d("0.4"), d("4.5"), d("0.5"), d("0.5"), d("0.8"), f(1)};
    gc::CorruptionEconomyPlayer owner; owner.player = 1; owner.faction = 10; owner.playable = owner.human = true;
    owner.price[1] = d("0.9"); owner.time[1] = d("0.95");
    auto beneficiary = owner; beneficiary.player = 2; beneficiary.faction = 30; beneficiary.benefits = true;
    state.players = {owner, beneficiary};
    for (const auto& p : s.planets) { s.fiscal->planets.push_back({p.id, p.id == 1 ? f(100) : Fixed{}}); state.planets.push_back({p.id, {}, false, false, cf::one, {}, {}, {}}); }
    s.planets[0].owner = 1;
    state.planets[0].corruption = cf::Type::racketeering;
    state.planets[0].siphon = {true, 2, d("0.25"), 3}; state.planets[0].piracy = f(10);
    state.items = {{50, 10, 3, 1, f(100), f(7), d("5.5")}, {51, 20, 2, 2, f(80), f(4), f(1)}, {52, 10, 1, 1, f(10), f(2), f(1)}};
    gc::BlackMarketBuyer buyer; buyer.buyer = 10000; buyer.owner = 2; buyer.opposing_planet = true; buyer.causes_despawn = true;
    buyer.price_modifier = d("1.5"); buyer.absence_min = {f(10), f(20), f(30), f(40), f(50)};
    buyer.absence_max = {f(12), f(24), f(36), f(48), f(60)}; state.buyers.push_back(buyer);
    return s;
}
gc::GameCommand prepare(std::uint64_t tick = 0) { return {{tick, {2}, tick + 1}, gc::PrepareBlackMarket{10000, 1}}; }
gc::GameCommand buy(std::uint64_t tick = 1) { return {{tick, {2}, tick + 1}, gc::BuyBlackMarket{10000, 0}}; }
bool rejected(const gc::GalacticTick& t) { return std::any_of(t.snapshot->events().begin(), t.snapshot->events().end(), [](const auto& e) { return e.kind == gc::GalacticEventKind::command_rejected; }); }
std::string technology_price_owner(const eawr::sim::PartitionExecutor& pool) {
    auto config = setup(); config.rules.fiscal_cycle_frames = 1000;
    config.player_economy->players[1].credits = f(10000);
    config.economy_corruption->planets[0].corruption = cf::Type::black_market;
    auto mismatch = config; mismatch.economy_corruption->players[1].tech_purchase_multiplier = f(2);
    expect(!gc::GalacticSession::create(mismatch), "saved cache must match the actual technology-price ledger");
    auto game = take(eawr::campaign::GameSession::create(config));
    expect(take(game.set_technology_price_modifier({2, 900, d("-0.5"), 4})) == d("0.5"), "trusted game writer binds the canonical player owner");
    expect(take(game.set_technology_price_modifier({2, 901, d("0.25"), {}})) == d("0.75"), "second source adds");
    expect(take(game.set_technology_price_modifier({2, 901, d("0.5"), {}})) == f(1), "same source replaces");
    expect(take(game.set_technology_price_modifier({2, 901, {}, {}})) == d("0.5"), "source removal refreshes shared scalar");
    const auto retained = game.galaxy().snapshot();
    const auto journal_size = game.galaxy().native_effects().size();
    expect(take(game.set_technology_price_modifier({2, 901, {}, {}})) == d("0.5")
        && game.galaxy().native_effects().size() == journal_size, "unchanged source removal has no duplicate journal");
    expect(!game.set_technology_price_modifier({2, 900, d("-0.5"), 0})
        && game.galaxy().native_effects().size() == journal_size, "past expiry refuses before journal or mutation");
    // Market preparation uses the same player scalar; copied menus remain prepared.
    take(game.submit(prepare())); (void)take(game.step(pool));
    const auto copied_price = game.galaxy().economy_corruption()->menus.front().offers.front().price;
    expect(copied_price == d("412.5"), "market preparation consumes shared technology-price factor");
    const auto identity = eawr::checkpoint::game_identities(config);
    const auto saved = take(eawr::checkpoint::save_game_checkpoint(game, identity));
    auto loaded = take(eawr::checkpoint::load_game_checkpoint(saved, identity, {}, pool));
    expect(take(eawr::checkpoint::save_game_checkpoint(loaded.session, identity)) == saved,
        "v2 ledger and typed writer reconstruct checkpoint exactly");
    for (auto* session : {&game, &loaded.session}) {
        expect(take(session->set_technology_price_modifier({2, 900, d("-0.25"), 6})) == d("0.75"), "replacement cancels old expiry");
        take(session->submit({{1, {2}, 2}, gc::PauseGalaxy{}}));
        take(session->submit({{3, {2}, 3}, gc::ResumeGalaxy{}}));
    }
    while (game.galaxy().clock().galactic_frame < 6) {
        (void)take(game.step(pool)); (void)take(loaded.session.step(pool));
        const auto frame = game.galaxy().clock().galactic_frame;
        const auto& player = game.galaxy().economy_corruption()->players[1];
        expect(player.tech_purchase_multiplier == (frame < 6 ? d("0.75") : f(1)), "host first-expired frame applies after active GC time only");
        expect(game.galaxy().state_sha256() == loaded.session.galaxy().state_sha256(), "restored expiry calendar continues like uninterrupted owner");
        expect(game.galaxy().cached_state_hash().get() == game.galaxy().state_sha256(), "expiry invalidates cached canonical hash");
    }
    expect(game.galaxy().economy_corruption()->players[1].technology_price_modifiers.empty()
        && retained->economy_corruption()->players[1].tech_purchase_multiplier == d("0.5"), "expiry prunes ledger while older snapshots remain immutable");
    expect(game.galaxy().economy_corruption()->menus.front().offers.front().price == copied_price, "expiry does not reprice copied market menu");
    const auto tick = game.galaxy().completed_tick();
    for (auto* session : {&game, &loaded.session}) take(session->submit(buy(tick)));
    (void)take(game.step(pool)); (void)take(loaded.session.step(pool));
    expect(game.galaxy().snapshot()->player_economy()->players[1].credits == take(eawr::sim::math::subtract(f(10000), copied_price)),
        "purchase after expiry pays exact copied price");
    expect(game.galaxy().state_sha256() == loaded.session.galaxy().state_sha256(), "post-expiry debit restore continuation");
    return game.galaxy().state_sha256();
}
void income_contracts(const eawr::sim::PartitionExecutor& pool) {
    auto config = setup(); auto state = *config.economy_corruption;
    std::vector<Fixed> pre{f(100), {}, {}, {}};
    const auto original = state;
    auto payments = take(gc::corruption_payments(state, config.planets, config.fiscal->players, pre, 3, pool, false));
    expect(payments == std::vector<Fixed>{f(75), f(485)} && state == original, "WGEP-04/12/13 preview: owner, 4.5 pre-siphon benefit, assigned siphon and unconsumed piracy");
    state.players[0].benefits = true;
    payments = take(gc::corruption_payments(state, config.planets, config.fiscal->players, pre, 3, pool, false));
    expect(payments == std::vector<Fixed>{f(85), f(485)}, "preview exposes piracy to every eligible beneficiary including owner");
    payments = take(gc::corruption_payments(state, config.planets, config.fiscal->players, pre, 3, pool, true));
    expect(payments == std::vector<Fixed>{f(85), f(475)}, "collection first eligible beneficiary consumes piracy in player order");
    state = original; config.fiscal->players[0].earns_planet_income = false;
    payments = take(gc::corruption_payments(state, config.planets, config.fiscal->players, pre, 3, pool, true));
    expect(payments == std::vector<Fixed>{Fixed{}, f(485)}, "ineligible player has no income route");
    config.fiscal->players[0].earns_planet_income = true; state = original;
    payments = take(gc::corruption_payments(state, config.planets, config.fiscal->players, pre, 99, pool, true));
    expect(payments[1] == f(485) && state.planets[0].piracy == Fixed{} && state.planets[0].siphon.active, "collection consumes piracy; corruption bypasses expiry");
    state.planets[0].corruption = cf::Type::none;
    payments = take(gc::corruption_payments(state, config.planets, config.fiscal->players, pre, 3, pool, false));
    expect(payments == std::vector<Fixed>{f(75), f(25)}, "siphon alive at equality");
    payments = take(gc::corruption_payments(state, config.planets, config.fiscal->players, pre, 4, pool, true));
    expect(payments == std::vector<Fixed>{f(100), {}} && !state.planets[0].siphon.active, "strict greater expiry commits only on collection");
    state = original; state.planets[0].siphon.recipient = 99;
    expect(take(gc::corruption_payments(state, config.planets, config.fiscal->players, pre, 3, pool, false))[0] == f(75), "non-sentinel recipient does not require a current player");
    state.planets[0].siphon.recipient = 0;
    expect(take(gc::corruption_payments(state, config.planets, config.fiscal->players, pre, 3, pool, false))[0] == f(100), "no-recipient sentinel has no owner debit");
    state = original; pre[0] = f(-100);
    payments = take(gc::corruption_payments(state, config.planets, config.fiscal->players, pre, 3, pool, false));
    expect(payments[1] == f(-15), "negative product gives no corruption benefit; assigned siphon stays independent");
    state = original; state.planets[0].siphon.fraction = d("1.01");
    expect(!gc::corruption_payments(state, config.planets, config.fiscal->players, pre, 3, pool, true), "invalid fraction refuses");
    auto game = take(gc::GalacticSession::create(config));
    std::vector<gc::GalacticSession::CreditNotification> notifications;
    game.set_credit_notification_handler([&](const auto& n) { notifications.push_back(n); return eawr::core::Result<void>::success(); });
    for (int i = 0; i < 4; ++i) (void)take(game.step(pool));
    expect(game.snapshot()->player_economy()->players[0].credits == f(1075)
        && game.snapshot()->player_economy()->players[1].credits == f(1485), "live fiscal uses shared accounts");
    expect(notifications.size() == 2 && notifications[1].kind == gc::GalacticSession::CreditNotification::Kind::fiscal
        && notifications[1].amount == f(485), "AI fiscal receives gross aggregate income");
    config.economy_corruption->planets[0].influenced = true;
    config.economy_corruption->planets[1].influenced = true;
    config.economy_corruption->planets[1].destroyed = true; config.economy_corruption->planets[1].factor = f(8);
    game = take(gc::GalacticSession::create(config));
    expect(game.economy_corruption()->random_draws == 0, "no startup income reroll");
    for (int i = 0; i < 4; ++i) (void)take(game.step(pool));
    const auto* live = game.economy_corruption();
    expect(game.snapshot()->player_economy()->players[0].credits == f(1075) && live->random_draws == 1
        && live->planets[0].factor >= f(1) && live->planets[0].factor < f(3)
        && live->planets[1].factor == f(1), "post-payment reroll uses next cycle; destroyed reset without draw");
    const auto factor = live->planets[0].factor;
    for (int i = 0; i < 3; ++i) (void)take(game.step(pool));
    const auto actual = take(eawr::sim::math::multiply(f(100), factor));
    auto expected = take(eawr::sim::math::subtract(actual, take(eawr::sim::math::multiply(actual, d("0.25")))));
    expect(game.snapshot()->player_economy()->players[0].credits == take(eawr::sim::math::add(f(1075), expected)), "next payment uses retained factor");
}
void market_contracts(const eawr::sim::PartitionExecutor& pool) {
    auto s = setup(); s.rules.fiscal_cycle_frames = 1000;
    auto& state = *s.economy_corruption; state.planets[0].corruption = cf::Type::black_market; state.planets[0].market_modifier = f(10);
    state.players[1].tech_purchase_multiplier = f(2);
    state.players[1].technology_price_modifiers = {{100, f(1), {}}};
    s.player_economy->players[1].credits = f(10000);
    auto game = take(gc::GalacticSession::create(s));
    std::vector<gc::GalacticSession::CreditNotification> notes;
    game.set_credit_notification_handler([&](const auto& n) { notes.push_back(n); return eawr::core::Result<void>::success(); });
    take(game.submit(prepare())); auto tick = take(game.step(pool));
    const auto menu = game.economy_corruption()->menus[0];
    expect(!rejected(tick) && menu.offers.size() == 2 && menu.offers[0].price == f(1815)
        && menu.offers[1].item == 52 && menu.absence >= f(20) && menu.absence < f(24), "faction filter, uncommon price and buyer tech absence");
    expect(game.snapshot()->player_economy()->players[1].credits == f(10000), "preparation is not a debit");
    take(game.submit(buy())); tick = take(game.step(pool));
    expect(!rejected(tick) && game.snapshot()->player_economy()->players[1].credits == f(8185)
        && game.economy_corruption()->players[1].unlocks.contains(50)
        && game.economy_corruption()->buyers[0].despawn_requested
        && game.economy_corruption()->buyers[0].prepared_absence == menu.absence,
        "purchase debit/unlock/despawn request, no production setup or queue");
    expect(notes.size() == 1 && notes[0].kind == gc::GalacticSession::CreditNotification::Kind::windfall
        && notes[0].amount == f(-1815), "market debit joins retained funding windfall path");
    take(game.submit(buy(2))); (void)take(game.step(pool));
    expect(game.snapshot()->player_economy()->players[1].credits == f(6370)
        && game.economy_corruption()->players[1].unlocks.size() == 1, "direct repeated execution still pays, unlock deduplicates");
    auto low_cash = s; low_cash.player_economy->players[1].credits = f(10);
    auto refused = take(gc::GalacticSession::create(low_cash)); take(refused.submit(prepare())); (void)take(refused.step(pool));
    take(refused.submit(buy())); tick = take(refused.step(pool));
    expect(rejected(tick) && refused.snapshot()->player_economy()->players[1].credits == f(10)
        && !refused.economy_corruption()->buyers[0].despawn_requested, "insufficient exact cash: no debit/unlock/despawn");
    auto lost = s; lost.economy_corruption->menus = {menu}; lost.economy_corruption->buyers[0].present = false;
    refused = take(gc::GalacticSession::create(lost)); take(refused.submit(buy(0))); tick = take(refused.step(pool));
    expect(rejected(tick) && refused.snapshot()->player_economy()->players[1].credits == f(10000), "buyer loss before debit");
    auto owned = s; owned.economy_corruption->players[1].unlocks.insert(50);
    game = take(gc::GalacticSession::create(owned)); take(game.submit(prepare())); (void)take(game.step(pool));
    expect(game.economy_corruption()->menus[0].offers[0].owned
        && !take(cf::menu_enabled(game.economy_corruption()->menus[0].offers[0], f(10000))), "owned offers retained, UI disabled");
    for (const auto multiplayer : {false, true}) {
        auto altered = s; altered.economy_corruption->multiplayer = multiplayer;
        altered.player_economy->players[0].current_tech = 5;
        game = take(gc::GalacticSession::create(altered)); take(game.submit(prepare())); (void)take(game.step(pool));
        expect(game.economy_corruption()->menus[0].offers[0].price == f(330), "common offer and MP highest displayed tech");
    }
    auto adjusted_tech = s;
    adjusted_tech.economy_corruption->players[0].displayed_tech_adjustment = 1;
    adjusted_tech.economy_corruption->players[1].displayed_tech_adjustment = 1;
    game = take(gc::GalacticSession::create(adjusted_tech)); take(game.submit(prepare())); (void)take(game.step(pool));
    expect(game.economy_corruption()->menus[0].offers[0].price == f(330)
        && game.economy_corruption()->menus[0].absence >= f(30)
        && game.economy_corruption()->menus[0].absence < f(36), "faction adjustment drives uncommon pricing and buyer absence");
    auto empty_absence = s;
    empty_absence.economy_corruption->buyers[0].absence_min.clear();
    empty_absence.economy_corruption->buyers[0].absence_max.clear();
    game = take(gc::GalacticSession::create(empty_absence)); take(game.submit(prepare())); (void)take(game.step(pool));
    expect(game.economy_corruption()->menus[0].absence == Fixed{}
        && game.economy_corruption()->random_draws == 0, "empty absence arrays return zero without RNG");
    auto tutorial = s; tutorial.economy_corruption->tutorial = true;
    game = take(gc::GalacticSession::create(tutorial)); take(game.submit(prepare())); (void)take(game.step(pool));
    expect(game.economy_corruption()->menus[0].offers[0].price == f(7), "tutorial tech and base cost bypass ordinary factors");
    tutorial.economy_corruption->items[0].tutorial_tech = 3;
    game = take(gc::GalacticSession::create(tutorial)); take(game.submit(prepare())); (void)take(game.step(pool));
    expect(game.economy_corruption()->menus[0].offers[0].price == d("38.5"), "tutorial still applies uncommon multiplier");
    for (int gate = 0; gate < 3; ++gate) {
        auto invalid = s;
        if (gate == 0) invalid.economy_corruption->buyers[0].opposing_planet = false;
        if (gate == 1) invalid.economy_corruption->players[0].playable = false;
        if (gate == 2) invalid.economy_corruption->planets[0].corruption = cf::Type::none;
        game = take(gc::GalacticSession::create(invalid)); take(game.submit(prepare())); tick = take(game.step(pool));
        expect(rejected(tick) && game.economy_corruption()->random_draws == 0, "market target admission before RNG/debit");
    }
    gc::ProductionOffer offer; offer.price = f(100); offer.seconds = f(10);
    auto adjusted = take(gc::prepared_production_offer(offer, nullptr, s.planets, 1, 1, &state));
    expect(adjusted.price == f(90) && adjusted.seconds == take(eawr::sim::math::multiply(f(10), d("0.95"))), "corruption production price/time factors");
    adjusted = take(gc::prepared_production_offer(offer, nullptr, s.planets, 1, 2, &state));
    expect(adjusted.price == f(100) && adjusted.seconds == f(10), "beneficiary production factors are one");
}
void replay_and_checkpoint() {
    technology_price_math();
    technology_price_decode();
    auto config = setup(); config.economy_corruption->planets[0].corruption = cf::Type::black_market;
    config.economy_corruption->planets[0].influenced = true;
    config.player_economy->players[1].credits = f(10000);
    gc::GalacticReplay replay{config, 12, {prepare(), buy()}};
    const auto bytes = take(gc::write_replay(replay));
    expect(take(gc::parse_replay(bytes)) == replay, "canonical economy/RNG/prepared commands round trip");
    for (std::size_t n = 0; n < bytes.size(); n += 97) expect(!gc::parse_replay(std::span(bytes).first(n)), "truncated economy replay refuses");
    std::vector<std::string> hashes;
    std::string price_baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        income_contracts(pool); market_contracts(pool);
        const auto price_hash = technology_price_owner(pool);
        if (price_baseline.empty()) price_baseline = price_hash;
        else expect(price_hash == price_baseline, "technology-price writer, expiry and continuation 1/2/4/8 equality");
        auto game = take(eawr::campaign::GameSession::create(config));
        take(game.submit(prepare())); take(game.submit(buy()));
        const auto identity = eawr::checkpoint::game_identities(config);
        for (std::uint64_t i = 0; i < 12; ++i) {
            const auto tick = take(game.step(pool));
            if (workers == 1) hashes.push_back(tick.state_hash.get());
            else expect(tick.state_hash.get() == hashes[i], "canonical state equal on 1/2/4/8 workers");
            if (i == 0 || i == 5) {
                const auto saved = take(eawr::checkpoint::save_game_checkpoint(game, identity));
                auto restored = take(eawr::checkpoint::load_game_checkpoint(saved, identity, {}, pool));
                expect(take(eawr::checkpoint::save_game_checkpoint(restored.session, identity)) == saved,
                    "checkpoint retains prepared offers/unlocks/current factor/RNG before and after payment");
                game = std::move(restored.session);
            }
        }
    }
}
void authored_load_guards() {
    namespace cp = eawr::campaign;
    namespace dg = eawr::data::galactic;
    const auto directory = std::filesystem::temp_directory_path() / ("gc-load-guards-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { eawr::testing::remove_temp_directory(path); } } cleanup{directory};
    std::filesystem::create_directories(directory / "XML");
    const auto write = [&](const char* file, std::string_view xml) { std::ofstream out(directory / "XML" / file); out << xml; };
    write("GameObjectFiles.xml", "<Game_Object_Files><File>Objects.xml</File></Game_Object_Files>");
    write("FactionFiles.xml", "<Faction_Files><File>Factions.xml</File></Faction_Files>");
    write("CampaignFiles.xml", "<Campaign_Files><File>Campaigns.xml</File></Campaign_Files>");
    write("HardpointDataFiles.xml", "<Hard_Point_Files/>"); write("SFXEventFiles.xml", "<SFXEvent_Files/>");
    write("Objects.xml", R"xml(<Types><Planet Name="Place"><Behavior>PLANET</Behavior><Galactic_Position>0,0,0</Galactic_Position>
        <Corruption_1_Type>CORRUPTION_PIRACY</Corruption_1_Type><Corruption_1_Required_Unit_Type>Missing</Corruption_1_Required_Unit_Type>
        <Corruption_2_Type>CORRUPTION_BLACK_MARKET</Corruption_2_Type><Corruption_2_Tactical_Game_Mode>Unknown</Corruption_2_Tactical_Game_Mode>
        </Planet><UniqueUnit Name="Buyer"><Abilities><Black_Market_Ability><Min_Respawn_Times>1 2</Min_Respawn_Times>
        <Max_Respawn_Times>2 3 4 5 6</Max_Respawn_Times></Black_Market_Ability></Abilities></UniqueUnit></Types>)xml");
    write("Factions.xml", R"xml(<Factions><Faction Name="Side"><Is_Playable>No</Is_Playable></Faction></Factions>)xml");
    write("Campaigns.xml", R"xml(<Campaigns><Campaign Name="Tutorial"><Locations>Place</Locations><Tutorial>No</Tutorial></Campaign></Campaigns>)xml");
    const std::vector<eawr::vfs::MountSpec> mounts{{"guard-fixture", directory, "data", {}}};
    const auto files = take(eawr::vfs::Vfs::mount(mounts));
    const auto loaded = take(eawr::data::load_catalog(files, eawr::data::Profile::foc));
    dg::GalacticData data;
    dg::Faction faction; faction.name = "Side"; faction.is_neutral = true; data.factions.push_back(faction);
    dg::Planet planet; planet.name = "Place"; planet.behaviors = {"PLANET"}; planet.galactic_position = dg::Position{}; data.planets.push_back(planet);
    dg::Campaign campaign; campaign.name = "Tutorial"; campaign.locations = {"Place"}; campaign.planets = {"Place"}; data.campaigns.push_back(campaign);
    data.constants = {{"MaximumFleetMovementDistance", "10", f(10)}, {"Fiscal_Cycle_Time_In_Secs", "1", f(1)}};
    cp::CampaignSetupOptions options; options.players = {{{1, 1, 0}, "Side", true}};
    const auto plan = take(cp::plan_campaign_setup(data, {}, "Tutorial", options));
    auto config = plan.galaxy();
    eawr::data::XmlDocument constants, market;
    const auto xml = [](std::string name, std::string value) { eawr::data::XmlNode n; n.name = std::move(name); n.raw_text = std::move(value);
        n.source = {"data/xml/blackmarketitems.xml", "guard-fixture", "guard-fixture", 7, 1}; return n; };
    constants.root.children = {xml("Corruption_Choice_Income_Percentage", "1 1 1 1 1 1 1 1"),
        xml("Black_Market_Income_Mult_Min", "1"), xml("Black_Market_Income_Mult_Max", "1")};
    auto item = xml("Item", ""); item.children = {xml("Faction", "Side"), xml("Tech_Level", "3"), xml("Base_Cost_Credits", "27")};
    auto ordinary_default = xml("Item", ""); ordinary_default.children = {xml("Faction", "Side")};
    auto unbound = xml("Item", ""); unbound.children = {xml("Faction", "Missing")};
    market.root.children = {item, ordinary_default, unbound};
    eawr::core::LoadNoteBuffer buffer;
    const auto economy = take(cp::prepare_corruption_economy(constants, market, loaded.catalog, plan, config, &buffer));
    expect(!economy.tutorial && economy.items.size() == 2, "ordinary market loads, skipping only an unbound item");
    expect(economy.items[0].tech == 3 && economy.items[0].base_cost == f(27), "ordinary item values survive loading");
    expect(economy.items[1].tech == 1 && economy.items[1].base_cost == Fixed{} && economy.items[1].uncommon_multiplier == Fixed{}, "B WGEP-105 defaults are 1/0/0");
    const auto buyer = take(cp::prepare_black_market_buyer(loaded.catalog, "Buyer", 10000, 2, true, &buffer));
    expect(buyer.absence_min.empty() && buyer.absence_max.empty(), "B WGEP-107 clears both malformed respawn lists");
    config.control.emplace(); config.census.emplace(); config.economy_corruption = economy;
    gc::ControlPlanet political; political.planet = config.planets[0].id; political.type = 1; config.control->planets.push_back(political);
    gc::GalacticAbilitySetup host;
    take(cp::prepare_corruption_host(loaded.catalog, plan, config, host, [](std::string_view) { return gc::TypeId{}; }, &buffer));
    expect(host.corruption_planets.size() == 1 && host.corruption_planets[0].slots[0].type == cf::Type::none
        && host.corruption_planets[0].slots[1].type == cf::Type::none, "C unresolved slot and unknown mode become empty slots");
    eawr::core::LoadNoteCollector notes; notes.merge(buffer); const auto snapshot = notes.snapshot();
    const auto has = [&](std::string_view code, eawr::core::LoadCategory category, std::string_view tag) {
        return std::any_of(snapshot.begin(), snapshot.end(), [&](const auto& note) { return note.code == code && note.classification == category && note.tag == tag; });
    };
    expect(has("EAWR-LOAD-1200", eawr::core::LoadCategory::retail_accepts, "Tech_Level")
        && has("EAWR-LOAD-1207", eawr::core::LoadCategory::retail_accepts, "Min_Respawn_Times/Max_Respawn_Times")
        && has("EAWR-LOAD-1205", eawr::core::LoadCategory::unknown_degrade, "Faction")
        && has("EAWR-LOAD-1221", eawr::core::LoadCategory::unknown_degrade, "Corruption_2_Tactical_Game_Mode"), "family contracts retain B/C classifications and tags");
    const auto default_note = std::find_if(snapshot.begin(), snapshot.end(), [](const auto& note) { return note.tag == "Tech_Level"; });
    expect(default_note != snapshot.end() && default_note->file == "data/xml/blackmarketitems.xml" && default_note->line == 7
        && default_note->layer.mod_id == "guard-fixture", "omitted ordinary field retains enclosing item provenance");
    auto consumer = setup(); consumer.economy_corruption->items = economy.items;
    consumer.economy_corruption->tutorial = false;
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = take(gc::GalacticSession::create(consumer)); std::vector<std::string> hashes;
        for (unsigned tick = 0; tick < 3; ++tick) hashes.push_back(take(game.step(executor)).state_hash.get());
        if (workers == 1) baseline = hashes; else expect(hashes == baseline, "degraded market continues deterministically on 1/2/4/8 workers");
    }
}
std::string game_root() {
    return eawr::testing::game_root(eawr::testing::EmptyEnvironment::present).value_or("");
}
void installed() {
    const auto root = game_root(); if (root.empty()) throw std::runtime_error("installed economy check requires game root");
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(root) / folder / "Data")).mount);
    auto filesystem = take(eawr::vfs::Vfs::mount(mounts));
    const auto loaded = take(eawr::data::load_catalog(filesystem, eawr::data::Profile::foc));
    const auto data = take(eawr::data::galactic::load_galactic_data(filesystem, loaded.catalog));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    eawr::campaign::CampaignSetupOptions options; options.seed = 42;
    gc::PlayerId consortium{}, empire{};
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        if (faction.name == "Underworld") consortium = id;
        if (faction.name == "Empire") empire = id;
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({{id}, id, faction.name == "Underworld" ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, faction.name == "Underworld" ? id : 0});
    }
    for (const auto& type : types.types) options.types.push_back({options.types.size() + 1, type.name});
    auto plan = take(eawr::campaign::plan_live_campaign_setup(filesystem, data, types,
        "Sandbox_Equal_Footing_Underworld", options, "Normal_Default"));
    auto s = plan.galaxy(); s.fiscal = plan.fiscal(); s.fiscal->contributions.reset();
    s.rules.fiscal_cycle_frames = 3; s.fiscal->credit_cap_per_planet = f(1000000);
    s.player_economy.emplace();
    for (const auto& p : s.players) s.player_economy->players.push_back({p.player_id, f(10000), f(1000000), 5, 5});
    for (std::size_t i = 0; i < s.planets.size(); ++i) s.planets[i].owner = s.players[i % s.players.size()].player_id;
    s.planets[0].owner = empire;
    s.economy_corruption = take(eawr::campaign::prepare_corruption_economy(filesystem, loaded.catalog, plan, s));
    expect(consortium && empire && s.economy_corruption->income_factors[4] == d("4.5"), "installed Consortium and racketeering authoring");
    {
        // GCMP-24: the corruption economy (WGEP-100, and WGCH-08 through the ability host) takes the setup mode, not
        // the campaign's Is_Multiplayer menu tag: the same solo-authored campaign planned as multiplayer flips it.
        auto multiplayer_options = options; multiplayer_options.mode = eawr::campaign::CampaignSetupMode::multiplayer;
        const auto multiplayer_plan = take(eawr::campaign::plan_live_campaign_setup(filesystem, data, types,
            "Sandbox_Equal_Footing_Underworld", multiplayer_options, "Normal_Default"));
        expect(!s.economy_corruption->multiplayer
            && take(eawr::campaign::prepare_corruption_economy(filesystem, loaded.catalog, multiplayer_plan, s)).multiplayer,
            "corruption economy multiplayer flag follows the setup mode");
    }
    auto& market = *s.economy_corruption;
    market.planets[0].corruption = cf::Type::racketeering;
    eawr::sim::InlineExecutor pool;
    auto game = take(gc::GalacticSession::create(s));
    for (int i = 0; i < 4; ++i) (void)take(game.step(pool));
    const auto account = std::find_if(game.snapshot()->player_economy()->players.begin(), game.snapshot()->player_economy()->players.end(), [&](const auto& p) { return p.player == consortium; });
    expect(account->credits > f(10000), "installed stock Consortium fiscal collection");
    market.planets[0].corruption = cf::Type::black_market;
    market.buyers.push_back(take(eawr::campaign::prepare_black_market_buyer(loaded.catalog, "Tyber_Zann", 10000, consortium, true)));
    game = take(gc::GalacticSession::create(s));
    take(game.submit({{0, {consortium}, 1}, gc::PrepareBlackMarket{10000, s.planets[0].id}})); (void)take(game.step(pool));
    expect(!game.economy_corruption()->menus.empty(), "installed stock market offer preparation");
    const auto price = game.economy_corruption()->menus[0].offers[0].price;
    take(game.submit({{1, {consortium}, 2}, gc::BuyBlackMarket{10000, 0}})); (void)take(game.step(pool));
    const auto purchased = std::find_if(game.snapshot()->player_economy()->players.begin(), game.snapshot()->player_economy()->players.end(), [&](const auto& p) { return p.player == consortium; });
    expect(purchased->credits == take(eawr::sim::math::subtract(f(10000), price))
        && !game.economy_corruption()->players[consortium - 1].unlocks.empty()
        && game.economy_corruption()->buyers[0].despawn_requested, "installed stock black-market debit/unlock/absence handoff");
    std::cout << "economy_corruption stock ran: Consortium collection and stock Tyber black-market purchase\n";
}
} // namespace
int main(int argc, char**) {
    try { if (argc > 1) installed(); else { authored_load_guards(); replay_and_checkpoint(); } }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
