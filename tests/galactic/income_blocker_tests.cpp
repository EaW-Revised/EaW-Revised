#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include "../support/result.hpp"
// WGEP-05/163..165: saved source-scoped income blockers and the native credit-halt writer.
#include "galactic_support.hpp"
#include "eawr/sim/galactic/income.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "../../src/sim/galactic/encoding.hpp"

#include <exception>
using eawr::testing::take;

namespace {
using namespace galactic_test;
using eawr::sim::math::Fixed;
namespace gc = eawr::sim::galactic;

Fixed f(std::int64_t n) { return take(Fixed::from_integer(n)); }
Fixed d(std::string_view n) { return take(Fixed::from_decimal(n)); }

// Player 1 owns planets 1 and 3, player 2 planets 2 and 4. Each fleet carries three members:
// 1000 halts for 0.2 s (6 frames), 1001 for 0.4 s (12 frames), 1002 is a plain mining source.
gc::GalacticSetup setup() {
    gc::SyntheticGalaxyOptions options; options.players = 2; options.planets = 4;
    options.fleets_per_player = 1; options.members_per_fleet = 3; options.debug_commands = true;
    auto s = gc::synthetic_galaxy(options);
    s.rules.fiscal_cycle_frames = 3;
    s.player_economy = gc::PlayerEconomySetup{{{1, f(1000), f(10000), 2, 5}, {2, f(1000), f(10000), 2, 5}}};
    s.fiscal = gc::FiscalSetup{f(2500), {{1, true, f(1)}, {2, true, f(1)}}, {}};
    s.income.emplace();
    gc::EconomicType quick; quick.type = 1000; quick.credit_halt = gc::CreditHaltAbility{d("0.2")};
    gc::EconomicType slow; slow.type = 1001; slow.credit_halt = gc::CreditHaltAbility{d("0.4")};
    gc::EconomicType mining; mining.type = 1002; mining.mining = gc::PlanetIncomeAbility{f(1), {}, f(1)};
    s.income->types = {quick, slow, mining};
    for (const auto& row : s.planets) {
        s.fiscal->planets.push_back({row.id, f(10)});
        s.income->planets.push_back({row.id, false, false, false});
    }
    return s;
}
// The local player's forecast needs observer knowledge of income and its breakdown.
gc::GalacticSetup observed() {
    auto config = setup();
    config.observer.emplace();
    auto& observer = *config.observer;
    observer.local_player = 1;
    const auto intelligence = gc::bit(gc::Intelligence::income_breakdown);
    for (const auto& player : config.players) observer.players.push_back({player.player_id, false, false, intelligence});
    for (const auto& planet : config.planets) {
        gc::ObserverPlanet row; row.planet = planet.id;
        row.allegiance = row.target_owner = row.current_owner = row.current_controller = planet.owner;
        observer.planets.push_back(row);
    }
    for (const gc::TypeId type : {1000, 1001, 1002}) observer.types.push_back({type});
    for (const auto& player : config.players) for (const auto& planet : config.planets) {
        gc::ObserverRecord record; record.player = player.player_id; record.planet = planet.id;
        record.discovered = true; record.mask = intelligence;
        observer.records.push_back(record);
    }
    return config;
}
const std::vector<gc::IncomeBlocker>& ledger(const gc::GalacticSession& session) {
    return session.snapshot()->income()->blockers;
}
Fixed credits(const gc::GalacticSession& session, std::size_t index) {
    return session.snapshot()->player_economy()->players[index].credits;
}
std::size_t income_journal(const gc::GalacticSession& session) {
    const auto effects = session.native_effects();
    return static_cast<std::size_t>(std::count_if(effects.begin(), effects.end(),
        [](const auto& e) { return e.kind == gc::NativeEffectKind::income_block; }));
}

void authored_profile() {
    auto config = setup();
    std::vector<std::uint8_t> bytes; gc::detail::encode_income(bytes, *config.income);
    const auto decoded = gc::detail::decode_income(bytes);
    expect(decoded && *decoded == *config.income && decoded->types[0].credit_halt
        && decoded->types[0].credit_halt->seconds == d("0.2"), "authored credit-halt profile survives the setup encoding");
    auto invalid = config; invalid.income->types[0].credit_halt->seconds = f(-1);
    expect(!gc::validate_setup(invalid), "negative authored halt duration refused");
    invalid = config; invalid.income->types[0].credit_halt->seconds = Fixed::from_raw(std::int64_t{1} << 62);
    expect(!gc::validate_setup(invalid), "unbounded authored halt duration refused");
    eawr::units::GalacticEconomicType source; source.object_class = "HeroUnit";
    const std::vector<eawr::campaign::EconomicTypeBinding> bindings{};
    const eawr::units::GalacticTypeTable types{};
    expect(!take(eawr::campaign::prepare_economic_type(source, 900, "Side", types, bindings)).credit_halt,
        "no authored halt tag leaves the type without a halt profile");
    source.credit_halt = false; source.credit_halt_seconds = f(3);
    expect(!take(eawr::campaign::prepare_economic_type(source, 900, "Side", types, bindings)).credit_halt,
        "an authored false flag keeps the duration inert");
    source.credit_halt = true;
    const auto profile = take(eawr::campaign::prepare_economic_type(source, 900, "Side", types, bindings));
    expect(profile.credit_halt && profile.credit_halt->seconds == f(3), "authored duration is carried unchanged");
    source.credit_halt_seconds.reset();
    expect(take(eawr::campaign::prepare_economic_type(source, 900, "Side", types, bindings)).credit_halt->seconds == Fixed{},
        "an absent duration is an unqualified zero-length halt, never an invented value");
    source.credit_halt_seconds = f(-1);
    expect(!eawr::campaign::prepare_economic_type(source, 900, "Side", types, bindings), "negative authored seconds refuse");
}

void writer_contracts() {
    eawr::sim::InlineExecutor serial;
    auto config = observed();
    auto game = take(gc::GalacticSession::create(config));
    const auto& own = config.fleets[0];   // player 1 at planet 1
    const auto& foreign = config.fleets[1]; // player 2 at planet 2
    const auto quick = own.members[0].id, slow = own.members[1].id, mining = own.members[2].id;
    expect(ledger(game).empty() && income_journal(game) == 0, "no blocker rows at creation");
    const auto refused = [&](eawr::sim::EntityId actor, gc::PlanetId planet, std::string_view why) {
        const auto journal = game.native_effects().size(); const auto rows = ledger(game);
        expect(!game.halt_planet_credits(actor, planet) && game.native_effects().size() == journal && ledger(game) == rows, why);
    };
    refused(0, 1, "null actor refuses");
    refused(quick, 99, "unknown planet refuses");
    refused(mining, 1, "a type without an authored halt ability refuses");
    refused(foreign.members[0].id, 1, "an actor on another planet refuses");
    refused(own.id, 1, "a fleet identity is not a sabotage actor");
    refused(1, 1, "a planet identity is not a sabotage actor");
    expect(!game.remove_income_blocker(1, 0) && !take(game.remove_income_blocker(1, quick)),
        "removing an absent source is a quiet no-op without a journal");
    expect(income_journal(game) == 0, "refusals and no-ops write no journal");

    const auto first = take(game.halt_planet_credits(quick, 1));
    expect(first.changed && first.first_expired_frame == 6, "authored 0.2 s registers six frames from the current frame");
    expect(ledger(game) == std::vector<gc::IncomeBlocker>{{1, quick, 0, 6}}, "row retains source, start and first expired frame");
    expect(income_journal(game) == 1, "write is journaled once as native effect 18");
    const auto again = take(game.halt_planet_credits(quick, 1));
    expect(!again.changed && again.first_expired_frame == 6 && income_journal(game) == 1 && ledger(game).size() == 1,
        "a live source never duplicates or extends");
    // Overlap: a second unique source keeps the planet blocked after the first expires.
    for (int i = 0; i < 2; ++i) (void)take(game.step(serial));
    const auto second = take(game.halt_planet_credits(slow, 1));
    expect(second.changed && second.first_expired_frame == game.clock().galactic_frame + 12 && ledger(game).size() == 2,
        "second source overlaps under its own identity and authored duration");
    const auto forecast = [&]() -> const gc::EconomyForecast& { return *game.snapshot()->economy_forecast(); };
    expect(forecast().planets[0].actual == Fixed{} && forecast().planets[0].modifiers
        && forecast().planets[0].modifiers->empty(), "forecast reads the same blocked predicate");
    expect(forecast().gross_total == f(10), "pure forecast keeps only the unblocked player planets");
    while (game.clock().galactic_frame < 6) (void)take(game.step(serial));
    expect(ledger(game).size() == 1 && ledger(game)[0].source == slow, "first source expires at its first expired frame");
    expect(forecast().planets[0].actual == Fixed{}, "planet stays blocked while any source holds");
    expect(take(game.remove_income_blocker(1, slow)) && ledger(game).empty() && income_journal(game) == 3,
        "explicit removal journals once and unblocks");
    expect(forecast().planets[0].actual == f(20) && forecast().gross_total == f(30), "unblocked forecast restores base plus mining");
    expect(!take(game.remove_income_blocker(1, slow)) && income_journal(game) == 3, "repeated removal has no duplicate journal");
}

void collection_and_forecast() {
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter pool(workers, eawr::platform::ThreadWorkerAdapter::Dispatch::always_pool);
        const auto config = observed();
        auto base = take(gc::GalacticSession::create(config));
        auto halted = take(gc::GalacticSession::create(config));
        take(halted.halt_planet_credits(config.fleets[0].members[0].id, 1));
        const auto gross = [](const gc::GalacticSession& s) { return s.snapshot()->economy_forecast()->gross_total; };
        expect(gross(base) == f(30) && gross(halted) == f(10), "blocked and unblocked forecasts differ by the planet's actual income");
        std::vector<std::string> hashes;
        for (int tick = 0; tick < 8; ++tick) {
            const auto before_base = credits(base, 0), before_halted = credits(halted, 0);
            const auto expected = gross(halted);
            (void)take(base.step(pool));
            const auto step = take(halted.step(pool));
            hashes.push_back(step.state_hash.get());
            if (tick == 3) {
                expect(credits(base, 0) == take(eawr::sim::math::add(before_base, f(30))), "unblocked cycle pays base plus mining");
                expect(credits(halted, 0) == take(eawr::sim::math::add(before_halted, f(10))) && expected == f(10),
                    "blocked cycle pays exactly the pure forecast, without the planet");
                expect(credits(base, 1) == credits(halted, 1), "the other player's collection is unaffected");
            }
            if (tick == 6) expect(credits(halted, 0) == take(eawr::sim::math::subtract(credits(base, 0), f(20))),
                "after expiry collection resumes: only the one blocked cycle was lost");
            if (tick == 5) expect(ledger(halted).empty() && gross(halted) == f(30), "expiry unblocks the forecast before the next cycle");
        }
        if (workers == 1) reference = hashes; else expect(hashes == reference, "blocked collection equal on 1/2/4/8 workers");
    }
}

void checkpoint_pause_and_overlap() {
    eawr::platform::ThreadWorkerAdapter pool(4, eawr::platform::ThreadWorkerAdapter::Dispatch::always_pool);
    const auto config = setup();
    auto game = take(eawr::campaign::GameSession::create(config));
    const auto hero = config.fleets[0].members[0].id;
    const auto receipt = take(game.halt_planet_credits(hero, 1));
    expect(receipt.changed && receipt.first_expired_frame == 6, "game writer binds the live galactic world");
    (void)take(game.step(pool));
    take(game.submit({{1, {1}, 1}, gc::PauseGalaxy{}}));
    (void)take(game.step(pool));
    expect(game.galaxy().clock().pause_depth > 0, "galaxy paused");
    const auto frame = game.galaxy().clock().galactic_frame;
    for (int i = 0; i < 6; ++i) (void)take(game.step(pool));
    expect(game.galaxy().clock().galactic_frame == frame && ledger(game.galaxy()).size() == 1,
        "ordinary pause owns the halt's time: nothing expires while paused");
    const auto identities = eawr::checkpoint::game_identities(config);
    const auto saved = take(eawr::checkpoint::save_game_checkpoint(game, identities));
    auto loaded = take(eawr::checkpoint::load_game_checkpoint(saved, identities, {}, pool));
    expect(take(eawr::checkpoint::save_game_checkpoint(loaded.session, identities)) == saved,
        "paused checkpoint re-encodes exactly");
    expect(ledger(loaded.session.galaxy()) == ledger(game.galaxy()) && loaded.session.galaxy().state_sha256() == game.galaxy().state_sha256(),
        "fresh restore reconstructs the exact ledger, never duplicating or extending");
    expect(loaded.session.galaxy().native_effects().size() == game.galaxy().native_effects().size(), "journal restored without duplicates");
    const auto repeated = take(loaded.session.halt_planet_credits(hero, 1));
    expect(!repeated.changed && repeated.first_expired_frame == 6, "restored live source cannot be extended by a repeat write");
    const auto resume_tick = game.galaxy().completed_tick();
    for (auto* session : {&game, &loaded.session}) take(session->submit({{resume_tick, {1}, 2}, gc::ResumeGalaxy{}}));
    for (int i = 0; i < 12; ++i) {
        (void)take(game.step(pool)); (void)take(loaded.session.step(pool));
        expect(game.galaxy().state_sha256() == loaded.session.galaxy().state_sha256(), "restored halt continues like the uninterrupted owner");
        expect(ledger(game.galaxy()).empty() == (game.galaxy().clock().galactic_frame >= 6),
            "the row holds exactly until its first expired active-time frame");
    }
    // After expiry the same actor may register a fresh halt under the same source.
    const auto fresh = take(game.halt_planet_credits(hero, 1)); const auto mirrored = take(loaded.session.halt_planet_credits(hero, 1));
    expect(fresh.changed && mirrored.changed && fresh.first_expired_frame == mirrored.first_expired_frame,
        "an expired source can write again");
}

// WGEP-05/08/75: the blocked collection returns before the refresh, so the retained political source stays.
void political_source_preserved() {
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter pool(workers, eawr::platform::ThreadWorkerAdapter::Dispatch::always_pool);
        auto config = setup();
        config.fiscal->players[1].earns_planet_income = false;
        config.control.emplace(); auto& control = *config.control;
        control.change_seconds = f(7); control.max_ground_forces = 10;
        control.players = {{1, false, true, 1}, {2, true, false, 2}};
        control.relations = {{1, 1, true}, {1, 2, false}, {2, 1, false}, {2, 2, true}};
        control.types = {{1000, 0, false, false}, {1001, 0, false, false}, {1002, 0, false, false}};
        config.fiscal->contributions.emplace(); auto& rules = *config.fiscal->contributions;
        rules.political_curve = {{}, {f(2), d("0.5")}, {f(5), f(1)}};
        rules.factions = {{1, gc::IncomeAlignment::neutral}, {2, gc::IncomeAlignment::neutral}};
        for (const auto& planet : config.planets) {
            gc::ControlPlanet row; row.planet = planet.id; row.type = 900;
            row.ground_base = {planet.owner, 1}; row.protected_control = true; control.planets.push_back(row);
            gc::PlanetControl state; state.planet = planet.id; state.initialized = true;
            state.current_player = state.target_player = planet.owner; state.current = state.target = f(2);
            state.political_income = f(-7); control.state.push_back(state);
            rules.planets.push_back({planet.id, gc::IncomeAlignment::neutral, {}});
        }
        for (const auto& route : config.routes) { control.routes.push_back({route.id, {}}); rules.routes.push_back({route.id, {}}); }
        auto base = take(eawr::campaign::GameSession::create(config));
        auto halted = take(eawr::campaign::GameSession::create(config));
        take(halted.halt_planet_credits(config.fleets[0].members[0].id, 1));
        std::vector<std::string> hashes;
        const auto source = [](const eawr::campaign::GameSession& s, std::size_t planet) {
            return s.galaxy().snapshot()->control()->state[planet].political_income;
        };
        for (int tick = 0; tick < 4; ++tick) {
            (void)take(base.step(pool)); hashes.push_back(take(halted.step(pool)).state_hash.get());
        }
        // The first cycle collected at frame 3: planet 1 is blocked, planet 3 refreshed.
        expect(source(base, 0) == f(-5) && source(halted, 0) == f(-7),
            "blocked collection leaves the retained political source unrewritten");
        expect(source(halted, 2) == f(-5) && source(halted, 1) == f(-7), "other planets follow the ordinary refresh and retention");
        expect(halted.galaxy().snapshot()->player_economy()->players[0].credits
            < base.galaxy().snapshot()->player_economy()->players[0].credits, "the blocked planet paid nothing");
        const auto identities = eawr::checkpoint::game_identities(config);
        auto restored = take(eawr::checkpoint::load_game_checkpoint(
            take(eawr::checkpoint::save_game_checkpoint(halted, identities)), identities, {}, pool));
        for (int tick = 0; tick < 6; ++tick) {
            (void)take(base.step(pool));
            const auto live = take(halted.step(pool));
            hashes.push_back(live.state_hash.get());
            expect(take(restored.session.step(pool)).state_hash.get() == live.state_hash.get(),
                "checkpoint continuation across expiry equals the uninterrupted run");
        }
        expect(source(halted, 0) == f(-5) && source(restored.session, 0) == f(-5),
            "after expiry the next collection refreshes the planet's political source");
        if (workers == 1) reference = hashes; else expect(hashes == reference, "retained-source blocking is exact on 1/2/4/8");
    }
}
} // namespace
int case_galactic_income_blocker_tests() {
    try { authored_profile(); writer_contracts(); collection_and_forecast(); checkpoint_pause_and_overlap(); political_source_preserved(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return failures ? 1 : 0;
}
