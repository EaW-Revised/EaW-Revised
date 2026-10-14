#include "galactic_support.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/sim/galactic/income.hpp"
#include "eawr/sim/galactic/observer.hpp"
#include "eawr/sim/galactic/fleet_intelligence.hpp"
#include "eawr/sim/galactic/victory.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <cstdlib>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <stdexcept>

namespace {
using namespace galactic_test;
namespace cp = eawr::checkpoint;
namespace campaign = eawr::campaign;
using eawr::sim::math::Fixed;
template <class T> T take(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}
Fixed f(std::int64_t value) { return take(Fixed::from_integer(value)); }
galactic::GalacticSetup setup() {
    galactic::SyntheticGalaxyOptions options; options.players = 1; options.planets = 4;
    options.fleets_per_player = 1; options.members_per_fleet = 1; options.debug_commands = true;
    auto config = galactic::synthetic_galaxy(options);
    config.rules.maximum_fleet_movement_distance = f(10000); config.rules.fiscal_cycle_frames = 3;
    config.fleet_control = galactic::FleetControlSetup{{}, {{1000, false, true, false}}, {}};
    config.player_economy = galactic::PlayerEconomySetup{{{1, f(250), f(10000), 2, 5}}};
    config.fiscal = galactic::FiscalSetup{f(2500), {{1, true, f(1)}}, {}};
    config.production = galactic::ProductionSetup{5, {{1, true, false, true, f(50), f(0), {}, {}, {}}}, {}};
    for (const auto& planet : config.planets) {
        config.fiscal->planets.push_back({planet.id, f(10)});
        galactic::PlanetProduction production;
        production.planet = planet.id; production.target_owner = 1; production.starbase_owner = 1;
        production.ground_occupier = 1; production.space_level = 1; production.maximum_space_level = 5;
        production.ground_slots = 3; production.space_slots = 3; production.maximum_ground_forces = 10;
        production.enabled = true; production.surface_accessible = true;
        galactic::ProductionOffer type; type.type = 1000; type.price = type.authored_price = f(100);
        type.seconds = take(Fixed::from_decimal("0.1")); type.tech = 2; type.population = f(1);
        type.affiliated = true; type.direct_build_allowed = true;
        production.offers.push_back(type);
        type.type = 1001; type.kind = galactic::ProductionKind::structure; type.population = f(0);
        production.offers.push_back(type);
        config.production->planets.push_back(production);
    }
    return config;
}
galactic::GameCommand buy(std::uint64_t tick, std::uint64_t sequence, galactic::PlanetId planet = 1,
    galactic::TypeId type = 1000, galactic::ProductionDomain domain = galactic::ProductionDomain::ground) {
    return {{tick, {1}, sequence}, galactic::ProductionCommand{planet, type, galactic::ProductionAction::buy, domain, 0}};
}
galactic::GameCommand cancel(std::uint64_t tick, std::uint64_t sequence, std::uint32_t index,
    galactic::TypeId type = 1000, galactic::ProductionAction action = galactic::ProductionAction::cancel_index) {
    return {{tick, {1}, sequence}, galactic::ProductionCommand{1, type, action, galactic::ProductionDomain::ground, index}};
}
void submit(galactic::GalacticSession& game, galactic::GameCommand command) {
    if (auto result = game.submit(command); !result) throw std::runtime_error(result.error().message);
}
void ground_structure_sale() {
    namespace gc = galactic;
    auto config = setup(); auto& planet = config.production->planets.front();
    auto& offer = planet.offers.front(); offer.kind = gc::ProductionKind::structure;
    offer.authored_price = f(101); offer.population = {};
    planet.objects = {{9001, offer.type, 1, f(101), gc::ProductionDomain::ground, gc::ProductionKind::structure},
        {9002, offer.type, 1, {}, gc::ProductionDomain::ground, gc::ProductionKind::structure}};
    config.production->players.front().counts = {{1000, 3, 3}}; // Two structures plus the original fleet member.
    expect(gc::ground_sale_refund(planet.objects[0], offer) == f(51), "paid odd price rounds half up");
    expect(gc::ground_sale_refund(planet.objects[1], offer) == f(50), "starting object uses truncated half authored price");
    auto cheap = planet.objects[0]; cheap.paid_cost = Fixed::from_raw(Fixed::scale / 2);
    expect(gc::ground_sale_refund(cheap, offer) == f(0), "positive sub-credit payment refunds zero");
    cheap.paid_cost = f(1);
    expect(gc::ground_sale_refund(cheap, offer) == f(0), "one-credit payment refunds zero");
    expect(gc::ground_sale_admission(planet, 2, 9001) == gc::ProductionAdmission::ownership, "foreign object cannot sell");
    expect(gc::ground_sale_admission(planet, 1, 9999) == gc::ProductionAdmission::stale_entry, "sale guards exact identity");
    offer.sellable = false;
    expect(gc::ground_sale_admission(planet, 1, 9001) == gc::ProductionAdmission::unavailable, "authored never-sale gate");
    offer.sellable = true;
    const auto sale_offer = offer;
    auto queued = offer; queued.type = 2000; queued.domain = gc::ProductionDomain::space;
    queued.required_structures = {{1000}}; planet.offers.push_back(queued);
    planet.queues[1].push_back({2000, 1, 9990, f(100), f(100), f(100), 0, 3000, false});
    expect(gc::ground_sale_admission(planet, 1, 9001) == gc::ProductionAdmission::locked, "space queue also occupies ground factory");
    planet.queues[1].clear();
    eawr::sim::InlineExecutor serial;
    {
        auto blocked = take(gc::GalacticSession::create(config));
        submit(blocked, buy(0, 1, planet.planet, 2000, gc::ProductionDomain::space));
        (void)take(blocked.step(serial));
        submit(blocked, {{1, {1}, 2}, gc::SellObject{planet.planet, 9001}});
        (void)take(blocked.step(serial));
        expect(blocked.snapshot()->production()->planets.front().objects.size() == 2, "queued prerequisite prevents commanded sale");
        expect(blocked.snapshot()->events().front().value == static_cast<std::uint64_t>(gc::ProductionAdmission::locked), "busy sale has typed rejection");
    }
    const gc::CommandPayload payload = gc::SellObject{planet.planet, 9001};
    {
        auto free = config; free.rules.fiscal_cycle_frames = 1000;
        free.production->planets.front().objects.front().paid_cost = f(1);
        gc::VictoryContext context; context.players = {{1, "Synthetic", false, true, true, false, f(250)}};
        context.relations = {{1, 1, true}};
        for (const auto& world : free.planets)
            context.planets.push_back({world.id, 100, world.owner, world.owner, world.owner, false});
        gc::VictoryAuthoring authoring; authoring.human = {gc::VictoryKind::credits}; authoring.human_parameters.credits = 200;
        free.victory = gc::install_victory(authoring, context, 1);
        auto game = take(gc::GalacticSession::create(free));
        submit(game, {{0, {1}, 1}, payload}); (void)take(game.step(serial));
        expect(game.snapshot()->player_economy()->players.front().credits == f(250), "one-credit structure refunds zero");
        expect(!game.victory()->pending, "zero-refund sale does not admit an unrelated already-held credit goal");
    }
    {
        auto multiplied = config; multiplied.fiscal->players.front().positive_credit_multiplier = f(2);
        auto game = take(gc::GalacticSession::create(multiplied));
        submit(game, {{0, {1}, 1}, payload}); (void)take(game.step(serial));
        expect(game.snapshot()->player_economy()->players.front().credits == f(352), "recipient multiplier applies after the displayed 51-credit refund");
    }
    const auto bytes = gc::encode_payload(payload);
    expect(gc::decode_payload(gc::CommandLayer::galactic, gc::galactic_opcodes::sell_object, bytes) == payload,
        "sale identity payload round trip");
    auto truncated = bytes; truncated.pop_back();
    expect(!gc::decode_payload(gc::CommandLayer::galactic, gc::galactic_opcodes::sell_object, truncated), "truncated sale refuses");
    std::vector<std::string> baseline;
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = take(gc::GalacticSession::create(config));
        submit(game, {{0, {1}, 1}, payload});
        // The second command names the removed object; it must not sell the same-type neighbour.
        submit(game, {{0, {1}, 2}, payload});
        const auto first = take(game.step(executor));
        expect(game.snapshot()->production()->planets.front().objects.size() == 1
            && game.snapshot()->production()->planets.front().objects.front().id == 9002, "sale removes one exact object");
        expect(game.snapshot()->player_economy()->players.front().credits == f(301), "sale credits the issuing player once");
        expect(first.snapshot->events().back().value == static_cast<std::uint64_t>(gc::ProductionAdmission::stale_entry), "duplicate sale rejects stale identity");
        submit(game, {{1, {1}, 3}, gc::SellObject{planet.planet, 9002}});
        const auto second = take(game.step(executor));
        expect(game.snapshot()->production()->planets.front().objects.empty(), "starting structure sells");
        expect(game.snapshot()->player_economy()->players.front().credits == f(351), "starting structure refunds authored half");
        const auto& count = game.snapshot()->production()->players.front().counts.front();
        expect(count.owned == 1 && count.historical == 3, "sale releases current count without rewriting historical production");
        const std::vector<std::string> hashes{first.state_hash.get(), second.state_hash.get()};
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "sale commands remain equal on 1/2/4/8 workers");
    }
    eawr::units::GalacticType type;
    type.build_cost_credits = f(101); type.build_time_seconds = f(1); type.named_hero = true;
    campaign::ProductionPreparation prep;
    prep.price_multiplier = prep.corruption_price_multiplier = prep.tech_purchase_multiplier = f(1);
    prep.time_multiplier = prep.base_tech_multiplier = prep.ai_time_multiplier = prep.corruption_time_multiplier = f(1);
    prep.factory_count = 1;
    expect(!take(campaign::prepare_production_offer(type, sale_offer, prep)).sellable, "default named hero sale excluded");
    type.sellable = "Sellable_Always";
    expect(take(campaign::prepare_production_offer(type, sale_offer, prep)).sellable, "authored always overrides hero default");
    type.sellable = "Sellable_Never";
    expect(!take(campaign::prepare_production_offer(type, sale_offer, prep)).sellable, "authored never forbids sale");
    type.sellable.clear(); type.named_hero = false; type.category_mask = {"Super"};
    expect(!take(campaign::prepare_production_offer(type, sale_offer, prep)).sellable, "default super category sale excluded");
}
void admission() {
    eawr::sim::InlineExecutor serial; auto config = setup();
    auto game = take(galactic::GalacticSession::create(config));
    submit(game, buy(0, 1)); submit(game, buy(0, 2, 2)); submit(game, buy(0, 3, 3));
    const auto tick = take(game.step(serial));
    expect(tick.work.production_producers == 2 && tick.work.production_entries == 2, "work is bounded by active queues, not all planets/offers");
    expect(game.snapshot()->player_economy()->players[0].credits == f(50), "two planets atomically share the account");
    expect(game.snapshot()->production()->planets[2].queues[0].empty(), "third buy cannot overspend");
    expect(tick.snapshot->events().back().reason == galactic::GalacticRejectReason::production_admission, "typed admission rejection");
    const auto check = [&](auto mutate, galactic::ProductionAdmission why, galactic::TypeId type = 1000) {
        auto input = setup(); mutate(input); auto session = take(galactic::GalacticSession::create(input));
        const auto& producer = input.production->planets[0];
        const auto offer = std::find_if(producer.offers.begin(), producer.offers.end(),
            [type](const auto& row) { return row.type == type; });
        const auto query = galactic::production_admission({*input.production, producer,
            input.production->players[0], input.player_economy->players[0], input.fleets,
            input.planets[0].owner, nullptr, 10000, 1}, &*offer, galactic::ProductionDomain::ground);
        expect(query == why, "read-only admission agrees with enqueue rejection");
        submit(session, buy(0, 1, 1, type)); auto result = take(session.step(serial));
        expect(result.snapshot->production()->planets[0].queues[0].empty(), "failed admission leaves queue empty");
        expect(result.snapshot->player_economy()->players[0].credits == f(250), "failed admission leaves ledger unchanged");
        expect(result.snapshot->events().back().value == static_cast<std::uint64_t>(why), "correct admission detail");
    };
    check([](auto& s) { s.player_economy->players[0].current_tech = 1; }, galactic::ProductionAdmission::tech);
    check([](auto& s) { s.production->players[0].locked = {1000}; }, galactic::ProductionAdmission::locked);
    check([](auto& s) { s.production->players[0].population_used = f(60); }, galactic::ProductionAdmission::population);
    check([](auto& s) { s.production->planets[0].ground_slots = 0; }, galactic::ProductionAdmission::slots, 1001);
    check([](auto& s) { s.production->planets[0].offers[0].current_limit = 0; }, galactic::ProductionAdmission::current_limit);
    check([](auto& s) { s.production->planets[0].offers[0].lifetime_limit = 0; }, galactic::ProductionAdmission::lifetime_limit);
    check([](auto& s) { s.production->planets[0].offers[0].required_structures = {{1001}}; }, galactic::ProductionAdmission::prerequisites);
    auto zero_pop = setup(); zero_pop.production->players[0].population_used = f(60);
    auto structures = take(galactic::GalacticSession::create(zero_pop)); submit(structures, buy(0, 1, 1, 1001));
    (void)take(structures.step(serial)); expect(structures.snapshot()->production()->planets[0].queues[0].size() == 1, "zero-pop structure over cap allowed");
}
void offer_projection() {
    auto config = setup();
    // Foreign observer facts still require a fully listed faction provider.
    config.players.push_back({2, 2, 0});
    config.player_economy->players.push_back({2, f(250), f(10000), 2, 5});
    config.fiscal->players.push_back({2, true, f(1)});
    config.production->players.push_back({2, false, true, false, f(50), f(0), {}, {}, {}});
    galactic::ObserverSetup observer;
    observer.local_player = 1; observer.reveal_delay_seconds = f(1);
    observer.players = {{1, false, false, 0, 1, {}}, {2, true, false, 0, 1, {}}};
    observer.types = {{1000, false, false, false}};
    for (const auto& planet : config.planets) {
        galactic::ObserverPlanet facts;
        facts.planet = planet.id; facts.allegiance = facts.target_owner = 1;
        facts.current_owner = facts.current_controller = 1;
        observer.planets.push_back(facts);
        observer.records.push_back({1, planet.id, true, 0, 0, 0, {}});
    }
    // Nonlocal and undiscovered payloads are deliberately still populated.
    observer.planets[1].allegiance = observer.planets[1].current_controller = 2;
    observer.records[2].discovered = false;
    for (const auto& planet : config.planets)
        observer.records.push_back({2, planet.id, true, 0, 0, 0, {}});
    config.observer = observer;
    std::vector<galactic::ObservedProductionOffer> baseline;
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(galactic::GalacticSession::create(config));
        const auto initial = game.snapshot();
        const auto offers = initial->production_offers();
        expect(offers.size() == 4, "only discovered local-controlled producer offers published");
        for (const auto& row : offers) {
            expect(row.player == 1 && (row.planet == 1 || row.planet == 4), "secret producers have no rows");
            expect(row.current && row.admission == galactic::ProductionAdmission::allowed && row.price == f(100), "query publishes command price and list membership");
            expect(!row.eventual.has_value(), "unsourced eventual semantics remain unknown");
            expect(row.base_price == f(100), "authored price published beside the command price (GHUD-S57)");
            expect(row.population_fits, "room for the population is published (GHUD-S57)");
        }
        submit(game, buy(0, 1)); const auto tick = take(game.step(pool));
        expect(tick.work.observer_projection_records == observer.records.size(),
            "production offer publication rebuilds the observer projection only once");
        const auto fresh = galactic::observed_planets(*tick.snapshot->observer(),
            tick.snapshot->fleets(), tick.snapshot->movement());
        expect(std::ranges::equal(tick.snapshot->observations(), fresh),
            "production admission consumes the current observer projection");
        const auto after = game.snapshot()->production_offers();
        expect(after.front().already_in_production, "queued type presence published");
        expect(initial->production_offers().front().already_in_production == false, "older publication immutable");
        if (baseline.empty()) baseline.assign(after.begin(), after.end());
        else expect(std::equal(after.begin(), after.end(), baseline.begin(), baseline.end()), "offer projection equal on 1/2/4/8 workers");
    }
    {
        // WGUI-170: renamed/synthetic ground, space and station offers retain
        // their identity and price while fiscal income changes admission.
        auto poor = config;
        poor.player_economy->players[0].credits = f(50);
        poor.fleet_control->types.push_back({1002, false, true, false});
        for (auto& planet : poor.production->planets) {
            auto type = planet.offers.front();
            type.type = 1002; type.domain = galactic::ProductionDomain::space;
            planet.offers.push_back(type);
            type.type = 1003; type.kind = galactic::ProductionKind::starbase;
            type.population = {}; type.base_level = 2;
            planet.offers.push_back(type);
            type.type = 1004; type.kind = galactic::ProductionKind::structure;
            planet.offers.push_back(type);
        }
        std::vector<galactic::ObservedProductionOffer> reference;
        std::vector<std::string> hashes;
        for (auto workers : eawr::platform::determinism_worker_counts()) {
            eawr::platform::ThreadWorkerAdapter pool(workers);
            auto game = take(galactic::GalacticSession::create(poor));
            const auto initial = game.snapshot();
            expect(initial->production_offers().size() == 10, "both observed producers retain all five poor offers");
            for (const auto& row : initial->production_offers())
                expect(row.current && row.admission == galactic::ProductionAdmission::credits
                    && row.price == f(100), "poor offer stays listed, disabled, with its cost");
            submit(game, buy(0, 1));
            for (std::uint64_t tick = 0; tick < 7; ++tick) {
                const auto result = take(game.step(pool));
                if (workers == 1) hashes.push_back(result.state_hash.get());
                else expect(result.state_hash.get() == hashes[static_cast<std::size_t>(tick)], "income and refusals equal on 1/2/4/8");
                const bool funded = result.snapshot->player_economy()->players.front().credits >= f(100);
                for (const auto& row : result.snapshot->production_offers())
                    expect(row.current && row.price == f(100)
                        && row.admission == (funded ? galactic::ProductionAdmission::allowed : galactic::ProductionAdmission::credits),
                        "credits arriving re-enable the same list entries at the fiscal boundary");
            }
            expect(game.snapshot()->production()->planets.front().queues[0].empty(), "poor direct purchase neither queues nor debits");
            expect(game.snapshot()->player_economy()->players.front().credits == f(130), "only income changed the poor account");
            const auto rows = game.snapshot()->production_offers();
            if (reference.empty()) reference.assign(rows.begin(), rows.end());
            else expect(std::ranges::equal(rows, reference), "list facts equal on 1/2/4/8");
            expect(initial->production_offers().front().admission == galactic::ProductionAdmission::credits, "retained poor publication stays immutable");
        }
        const auto hidden = [&](auto mutate, galactic::TypeId type = 1000) {
            auto blocked = poor; mutate(blocked);
            auto game = take(galactic::GalacticSession::create(blocked));
            const auto rows = game.snapshot()->production_offers();
            const auto found = std::ranges::find_if(rows, [type](const auto& row) { return row.planet == 1 && row.type == type; });
            expect(found != rows.end(), "secondary blocker has an observed offer");
            if (found == rows.end()) return;
            const auto& row = *found;
            expect(row.admission == galactic::ProductionAdmission::credits && !row.current,
                "credits never conceal a secondary list blocker");
        };
        hidden([](auto& s) { s.player_economy->players[0].current_tech = 1; });
        hidden([](auto& s) { s.production->players[0].locked = {1000}; });
        hidden([](auto& s) { s.production->planets[0].offers[0].required_structures = {{1001}}; });
        hidden([](auto& s) { s.production->planets[0].offers[0].current_limit = 0; });
        hidden([](auto& s) { s.production->planets[0].offers[0].lifetime_limit = 0; });
        hidden([](auto& s) { s.production->planets[0].offers[0].kind = galactic::ProductionKind::structure;
            s.production->planets[0].ground_slots = 0; });
        hidden([](auto& s) { s.production->planets[0].offers[1].planet_limit = 0; }, 1001);
        hidden([](auto& s) { s.production->planets[0].offers[3].base_level = 3; }, 1003);
        hidden([](auto& s) { s.production->planets[0].maximum_space_level = 1; }, 1003);
        hidden([](auto& s) { s.production->planets[0].target_owner = 2; });
        hidden([](auto& s) { s.production->planets[0].starbase_owner = 2; }, 1002);
        hidden([](auto& s) { s.production->planets[0].offers[0].required_ground_level = 1; });
        hidden([](auto& s) { s.production->planets[0].offers[2].required_space_level = 2; }, 1002);
        auto bounded = config;
        bounded.fleets.front().id = std::numeric_limits<eawr::sim::EntityId>::max() - 3;
        auto bounded_game = take(galactic::GalacticSession::create(bounded));
        expect(bounded_game.snapshot()->production_offers().size() == 4, "ID-bound scenario retains observed choices");
        for (const auto& row : bounded_game.snapshot()->production_offers())
            expect(row.current && row.admission == galactic::ProductionAdmission::resource_limit,
                "project ID bound disables a capable listed offer");
        auto full = config;
        full.production->maximum_human_queue = 1;
        full.player_economy->players[0].credits = f(150);
        auto game = take(galactic::GalacticSession::create(full));
        eawr::sim::InlineExecutor serial;
        submit(game, buy(0, 1, 1, 1001)); (void)take(game.step(serial));
        expect(game.snapshot()->production_offers().front().current, "full queue plus low funds keeps otherwise capable type listed");
        auto station_slot = poor;
        station_slot.player_economy->players[0].credits = f(150);
        auto other_station = station_slot.production->planets[0].offers[3];
        other_station.type = 1005;
        station_slot.production->planets[0].offers.push_back(other_station);
        auto stations = take(galactic::GalacticSession::create(station_slot));
        submit(stations, buy(0, 1, 1, 1003, galactic::ProductionDomain::space));
        (void)take(stations.step(serial));
        const auto station_rows = stations.snapshot()->production_offers();
        const auto other = std::ranges::find_if(station_rows, [](const auto& row) { return row.type == 1005; });
        expect(other != station_rows.end() && !other->current && !other->already_in_production
            && other->admission == galactic::ProductionAdmission::credits,
            "an occupied starbase queue slot hides the other station type despite low funds");
    }
    {
        // GHUD-S57: admission reports credits before population; the population fit is published on its own.
        auto poor = config;
        poor.player_economy->players[0].credits = f(50);
        const auto rows = [](const galactic::GalacticSetup& built) {
            auto game = take(galactic::GalacticSession::create(built));
            const auto snapshot = game.snapshot(); const auto view = snapshot->production_offers();
            return std::vector<galactic::ObservedProductionOffer>(view.begin(), view.end());
        };
        auto offers = rows(poor);
        expect(offers.size() == 4 && std::ranges::all_of(offers, [](const auto& row) {
            return row.current && row.admission == galactic::ProductionAdmission::credits && row.population_fits; }),
            "credits short, population room: the credits reason and a population fit");
        poor.production->players[0].population_used = f(50); // at the cap: type 1000 needs 1, type 1001 needs none
        offers = rows(poor);
        expect(offers.size() == 4 && std::ranges::all_of(offers, [](const auto& row) {
            return row.current && row.admission == galactic::ProductionAdmission::credits && row.population_fits == (row.type == 1001); }),
            "credits and population short: admission still says credits, the population fit says no");
        // A mod's non-1.0 uncorrupted factor is already inside the offer price; the base stays the authored cost.
        auto raised = config;
        for (auto& planet : raised.production->planets) planet.offers[0].price = f(120);
        offers = rows(raised);
        expect(std::ranges::any_of(offers, [](const auto& row) { return row.type == 1000; })
            && std::ranges::all_of(offers, [](const auto& row) {
                return row.type != 1000 || (row.price == f(120) && row.base_price == f(100)); }),
            "scaled price published against the unscaled authored cost");
    }
    config.observer.reset();
    expect(take(galactic::GalacticSession::create(config)).snapshot()->production_offers().empty(), "missing observer never invents offer knowledge");

    galactic::IncomeSetup income;
    galactic::EconomicType source; source.type = 900;
    source.reductions = {{1000, 1, take(Fixed::from_decimal("0.5")), true, false}};
    income.types = {source}; income.sources = {{1, 900, 1, 1, true, true}};
    for (const auto& planet : config.planets) income.planets.push_back({planet.id, false, false, false});
    income.offers = {{1, 1000, f(101), f(10)}};
    const auto adjusted = take(galactic::prepared_production_offer(config.production->planets[0].offers[0],
        &income, config.planets, 1, 1));
    expect(adjusted.price == f(51) && adjusted.seconds == f(10), "income query rounds once after location reduction");
    expect(!galactic::prepared_production_offer(config.production->planets[0].offers[1], &income,
        config.planets, 1, 1), "missing income basis cannot publish guessed price");
}
// WGEP-40/WGEP-66, GQP-11: a completed orbit unit is its own fleet at slot 0 and
// is not merged into the planet's existing slot-0 fleet. The observer projection
// must still show it: one slot card whose count covers both fleets.
void orbit_completion_shares_slot_card() {
    auto config = setup();
    config.production->planets[0].offers[0].domain = galactic::ProductionDomain::space;
    galactic::ObserverSetup observer;
    observer.local_player = 1; observer.reveal_delay_seconds = f(1);
    observer.players = {{1, false, false, 0, 1, {}}};
    observer.types = {{1000, false, false, false}};
    for (const auto& planet : config.planets) {
        galactic::ObserverPlanet facts;
        facts.planet = planet.id; facts.allegiance = facts.target_owner = 1;
        facts.current_owner = facts.current_controller = 1;
        observer.planets.push_back(facts);
        observer.records.push_back({1, planet.id, true, 0, 0, 0, {}});
    }
    config.observer = observer;
    const std::vector<galactic::FleetDisplayType> traits{{1000, false, false, false, false, {}, 0}};
    const std::vector<galactic::FleetDisplayVisibility> sight{{1, true}};
    std::optional<galactic::FleetIntelligence> baseline;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(galactic::GalacticSession::create(config));
        const auto before = galactic::observed_fleets(*game.snapshot(), 1, traits, 8, sight);
        expect(before.cards.size() == 1 && before.cards[0].count == 1 && before.cards[0].orbit_slot == 0,
            "starting fleet publishes a slot-0 card");
        submit(game, buy(0, 1, 1, 1000, galactic::ProductionDomain::space));
        for (int i = 0; i < 4; ++i) (void)take(game.step(pool));
        const auto fleets = game.snapshot()->fleets();
        expect(fleets.size() == 2 && fleets[0].orbit == 1 && fleets[1].orbit == 1
            && fleets[0].orbit_slot == 0 && fleets[1].orbit_slot == 0 && fleets[0].members.size() == 1
            && fleets[1].members.size() == 1, "completed unit is a new slot-0 fleet beside the existing one");
        const auto after = galactic::observed_fleets(*game.snapshot(), 1, traits, 8, sight);
        expect(after.cards.size() == 1 && after.cards[0].fleet == fleets[0].id && after.cards[0].count == 2
            && after.cards[0].groups.size() == 1 && after.cards[0].groups[0].count == 2,
            "purchased ship is counted on the slot-0 card instead of hidden");
        if (!baseline) baseline = after;
        else expect(*baseline == after, "slot card equal on 1/2/4/8 workers");
    }
}
void technology_price_production() {
    auto config = setup();
    galactic::EconomyCorruptionState corruption;
    galactic::CorruptionEconomyPlayer player; player.player = 1; player.faction = 1; player.playable = true;
    player.price = {f(1), take(Fixed::from_decimal("0.9"))};
    player.time = {f(1), take(Fixed::from_decimal("0.95"))};
    player.technology_price_modifiers = {{900, take(Fixed::from_decimal("-0.5")), 2}};
    player.tech_purchase_multiplier = take(Fixed::from_decimal("0.5"));
    corruption.players = {player};
    for (const auto& planet : config.planets) corruption.planets.push_back({planet.id, {}, false, false, galactic::corruption_fixture::one, {}, {}, {}});
    auto research = config.production->planets[0].offers[0];
    research.price = f(101); research.seconds = f(10);
    research.kind = galactic::ProductionKind::technology; research.target_tech = 3; research.population = {};
    galactic::IncomeSetup basis; basis.offers = {{1, research.type, f(101), f(10)}};
    const auto prepare = [&](galactic::ProductionOffer original, const galactic::IncomeSetup* income) {
        return take(galactic::prepared_production_offer(original, income, config.planets, 1, 1, &corruption));
    };
    expect(prepare(research, &basis).price == f(51) && prepare(research, &basis).seconds == f(10),
        "research-only price factor precedes rounding and leaves duration intact");
    auto ordinary = research; ordinary.kind = galactic::ProductionKind::unit; ordinary.target_tech = 0;
    expect(prepare(ordinary, &basis).price == f(101), "ordinary production ignores technology-price factor");
    corruption.planets[0].corruption = galactic::corruption_fixture::Type::racketeering;
    // The authored decimal factor is represented in Q24 before multiplication;
    // duration retains that exact result instead of rounding back to decimal.
    const auto reduced_seconds = Fixed::from_raw(10 * player.time[1].raw());
    expect(prepare(research, &basis).price == f(45) && prepare(research, nullptr).price == f(45)
        && prepare(research, &basis).seconds == reduced_seconds,
        "corruption and technology factors combine before a single rounding step");
    corruption.players[0].tech_purchase_multiplier = take(Fixed::from_decimal("0.0001"));
    expect(prepare(research, &basis).price == f(1), "sourced fallback keeps production positive subcredit minimum");
    corruption = {}; corruption.players = {player};
    for (const auto& planet : config.planets) corruption.planets.push_back({planet.id, {}, false, false, galactic::corruption_fixture::one, {}, {}, {}});
    config.economy_corruption = corruption;
    for (auto& planet : config.production->planets) {
        auto& offer = planet.offers[0]; offer = research; offer.price = f(100); offer.seconds = f(100);
    }
    eawr::sim::InlineExecutor executor;
    auto game = take(galactic::GalacticSession::create(config));
    submit(game, buy(0, 1)); (void)take(game.step(executor));
    expect(game.snapshot()->production()->planets[0].queues[0].front().price == f(50)
        && game.snapshot()->player_economy()->players[0].credits == f(200), "research purchase records the shared factor once");
    (void)take(game.step(executor));
    expect(game.economy_corruption()->players[0].tech_purchase_multiplier == f(1)
        && game.snapshot()->production()->planets[0].queues[0].front().price == f(50), "expiry leaves stored queue price intact");
    submit(game, cancel(2, 2, 0)); (void)take(game.step(executor));
    expect(game.snapshot()->player_economy()->players[0].credits == f(250), "cancel refunds stored discounted price after expiry");
}
void cancel_identity() {
    auto old = cancel(0, 1, 1);
    const auto legacy = galactic::encode_payload(old.payload);
    expect(legacy.size() == 22, "old production cancel retains its exact encoding");
    const auto decoded = galactic::decode_payload(galactic::CommandLayer::galactic,
        galactic::galactic_opcodes::production, legacy);
    expect(decoded && *decoded == old.payload, "old cancel has no implicit identity guard");
    auto guarded = old;
    std::get<galactic::ProductionCommand>(guarded.payload).target_build_id = 1234;
    const auto bytes = galactic::encode_payload(guarded.payload);
    expect(bytes.size() == 31 && std::equal(legacy.begin(), legacy.end(), bytes.begin()), "versioned identity appends without changing base bytes");
    expect(galactic::decode_payload(galactic::CommandLayer::galactic,
        galactic::galactic_opcodes::production, bytes) == std::optional(guarded.payload), "guarded cancel payload round trip");
    const auto malformed = [&](std::vector<std::uint8_t> input) {
        expect(!galactic::decode_payload(galactic::CommandLayer::galactic,
            galactic::galactic_opcodes::production, input), "malformed cancel identity rejected");
    };
    auto input = bytes; input[22] = 2; malformed(input);
    input = bytes; std::fill(input.begin() + 23, input.end(), std::uint8_t{0}); malformed(input);
    input = bytes; input.pop_back(); malformed(input);
    input = bytes; input.push_back(0); malformed(input);
    input = bytes; input[16] = 0; malformed(input);

    std::string expected_hash;
    for (auto workers : eawr::platform::determinism_worker_counts()) {
    eawr::platform::ThreadWorkerAdapter serial(workers);
    auto config = setup();
    config.player_economy->players[0].credits = f(400);
    auto game = take(galactic::GalacticSession::create(config));
    submit(game, buy(0, 1)); submit(game, buy(0, 2)); (void)take(game.step(serial));
    const auto target = game.snapshot()->production()->planets[0].queues[0][1].build_id;
    for (int i = 0; i < 3; ++i) (void)take(game.step(serial));
    expect(game.snapshot()->production()->planets[0].queues[0].front().build_id == target, "completed head shifts the displayed target");
    submit(game, buy(4, 3));
    auto stale = cancel(4, 4, 1);
    std::get<galactic::ProductionCommand>(stale.payload).target_build_id = target;
    submit(game, stale); (void)take(game.step(serial));
    const auto& queue = game.snapshot()->production()->planets[0].queues[0];
    expect(queue.size() == 2 && queue[1].build_id != target, "stale same-type indexed cancel cannot remove the replacement");
    expect(game.snapshot()->events().back().value == static_cast<std::uint64_t>(galactic::ProductionAdmission::stale_entry), "stale identity has typed rejection detail");
    submit(game, cancel(5, 5, 1)); (void)take(game.step(serial));
    expect(game.snapshot()->production()->planets[0].queues[0].size() == 1, "unguarded old cancel preserves its indexed behavior");
    auto correct = cancel(6, 6, 0);
    std::get<galactic::ProductionCommand>(correct.payload).target_build_id = target;
    submit(game, correct); (void)take(game.step(serial));
    expect(game.snapshot()->production()->planets[0].queues[0].empty(), "matching guarded cancel removes its actual target");
    const auto hash = game.snapshot()->sha256();
    if (expected_hash.empty()) expected_hash = hash;
    else expect(hash == expected_hash, "guarded and legacy cancel sequence equal on 1/2/4/8 workers");
    }

}
void timing_and_cancel() {
    eawr::sim::InlineExecutor serial; auto game = take(galactic::GalacticSession::create(setup()));
    submit(game, buy(0, 1)); submit(game, buy(0, 2)); (void)take(game.step(serial));
    const auto build = game.snapshot()->production()->planets[0].queues[0][1].build_id;
    submit(game, cancel(1, 3, 0, 1001)); (void)take(game.step(serial));
    expect(game.snapshot()->production()->planets[0].queues[0].size() == 2, "wrong indexed type does not mutate");
    submit(game, cancel(2, 4, 0)); (void)take(game.step(serial));
    expect(game.snapshot()->player_economy()->players[0].credits == f(150), "front cancel refunds stored prepaid price");
    expect(game.snapshot()->production()->planets[0].queues[0].front().build_id == build, "successor retains build identity");
    for (int i = 0; i < 3; ++i) (void)take(game.step(serial));
    expect(game.snapshot()->production()->planets[0].objects.size() == 1, "promoted front receives its full duration");
    expect(game.snapshot()->production()->planets[0].objects[0].paid_cost == f(100), "surface result records paid cost");
}
void inactive_work_budget() {
    eawr::sim::InlineExecutor serial; auto game = take(galactic::GalacticSession::create(setup()));
    (void)take(game.step(serial));
    const auto idle = take(game.step(serial));
    expect(idle.work.production_producers == 0 && idle.work.production_entries == 0, "idle queues run no production phase");
    expect(idle.work.sections_reused >= 8, "idle production reuses its state digest");
}
void refund_recipient() {
#ifdef NDEBUG
    // WGEP-64 release semantics for the debug-invalid builder/recipient mismatch.
    // The debug ownership command reaches this case without mutating queue bytes.
    auto config = setup(); config.rules.fiscal_cycle_frames = 1000;
    config.players.push_back({2, 1, 0});
    config.seats.push_back({{2}, 2, galactic::SeatKind::human, 1, true, 2});
    config.player_economy->players.push_back({2, f(25), f(10000), 2, 5});
    config.fiscal->players.push_back({2, true, f(1)});
    config.production->players.push_back({2, true, false, true, f(50), f(0), {}, {}, {}});
    eawr::sim::InlineExecutor serial; auto game = take(galactic::GalacticSession::create(config));
    submit(game, buy(0, 1)); (void)take(game.step(serial));
    submit(game, owner(1, {1}, 2, 1, 2));
    auto order = cancel(1, 1, 0); order.key.seat = {2}; submit(game, order);
    (void)take(game.step(serial));
    expect(game.snapshot()->production()->planets[0].queues[0].empty(), "builder mismatch does not reject release cancellation");
    expect(game.snapshot()->player_economy()->players[0].credits == f(150), "refund does not credit the recorded builder");
    expect(game.snapshot()->player_economy()->players[1].credits == f(125), "refund credits the supplied recipient");
#endif // The mismatched builder intentionally violates the WGEP-64 debug assertion.
}
void queue_policies() {
    eawr::sim::InlineExecutor serial; auto config = setup(); config.player_economy->players[0].credits = f(2000);
    auto game = take(galactic::GalacticSession::create(config));
    for (std::uint64_t i = 1; i <= 6; ++i) submit(game, buy(0, i));
    const auto filled = take(game.step(serial));
    expect(game.snapshot()->production()->planets[0].queues[0].size() == 5, "human queue cap counts all entries");
    expect(filled.snapshot->events().back().value == static_cast<std::uint64_t>(galactic::ProductionAdmission::queue_full), "sixth buy reports queue full");
    submit(game, cancel(1, 7, 4)); (void)take(game.step(serial));
    expect(game.snapshot()->production()->planets[0].queues[0].front().start_frame == 0, "tail cancellation does not restart the front");
    const auto balance = game.snapshot()->player_economy()->players[0].credits;
    submit(game, cancel(2, 8, 0, 0, galactic::ProductionAction::stop)); (void)take(game.step(serial));
    expect(game.snapshot()->production()->planets[0].queues[0].empty() && game.snapshot()->player_economy()->players[0].credits == balance, "bulk stop clears without direct refund");
    auto typed = take(galactic::GalacticSession::create(config));
    submit(typed, buy(0, 1)); submit(typed, buy(0, 2, 1, 1001)); submit(typed, buy(0, 3)); (void)take(typed.step(serial));
    const auto first = typed.snapshot()->production()->planets[0].queues[0].front().build_id;
    submit(typed, cancel(1, 4, 0, 1000, galactic::ProductionAction::cancel_type)); (void)take(typed.step(serial));
    expect(typed.snapshot()->production()->planets[0].queues[0].size() == 2 && typed.snapshot()->production()->planets[0].queues[0].front().build_id == first, "type cancel removes last matching entry");
    config.production->planets[0].offers[1].required_orbiting_units = {1000};
    auto lost = take(galactic::GalacticSession::create(config)); submit(lost, buy(0, 1, 1, 1001)); submit(lost, buy(0, 2, 1, 1001));
    (void)take(lost.step(serial)); const auto old_balance = lost.snapshot()->player_economy()->players[0].credits;
    submit(lost, orbit(1, {1}, 3, config.fleets[0].id, 2)); const auto removed = take(lost.step(serial));
    expect(lost.snapshot()->production()->planets[0].queues[0].empty() && lost.snapshot()->player_economy()->players[0].credits == old_balance, "lost prerequisites cancel without direct refund");
    std::vector<std::uint64_t> cancelled;
    for (const auto& event : removed.snapshot->events()) if (event.kind == galactic::GalacticEventKind::production_cancelled) cancelled.push_back(event.sequence);
    expect(cancelled.size() == 2 && cancelled[0] > cancelled[1], "prerequisite sweep cancels backwards");
    config = setup(); config.production->players[0].pay_as_you_go = true; config.player_economy->players[0].credits = f(0);
    auto suspended = take(galactic::GalacticSession::create(config)); submit(suspended, buy(0, 1));
    for (int i = 0; i < 3; ++i) (void)take(suspended.step(serial));
    expect(suspended.snapshot()->production()->planets[0].queues[0].front().last_remaining == config.production->planets[0].offers[0].seconds, "shortage resets timer to last paid service frontier");
}
void content_and_placement() {
    eawr::units::GalacticType source; source.build_cost_credits = take(Fixed::from_decimal("0.5"));
    source.build_time_seconds = f(6); source.tech_level = 2; source.population_value = f(1);
    auto prototype = setup().production->planets[0].offers[0];
    campaign::ProductionPreparation factors;
    expect(!campaign::prepare_production_offer(source, prototype, factors), "missing modifier providers are refused");
    factors.price_multiplier = f(1); factors.corruption_price_multiplier = f(1); factors.tech_purchase_multiplier = f(1);
    factors.time_multiplier = f(1); factors.base_tech_multiplier = f(1); factors.ai_time_multiplier = f(1);
    factors.corruption_time_multiplier = f(1); factors.factory_count = 2;
    const auto prepared = take(campaign::prepare_production_offer(source, prototype, factors));
    expect(prepared.price == f(1) && prepared.seconds == f(3), "XML price/time prepared with verified floor and factory divisor");
    for (std::int64_t target = 2; target <= 5; ++target) {
        source.build_advances_tech_level = target;
        const auto technology = take(campaign::prepare_production_offer(source, prototype, factors));
        expect(technology.kind == galactic::ProductionKind::technology && technology.target_tech == target,
            "WGEP-42 offer retains each numeric target 2-5");
        auto config = setup(); config.production->planets.front().offers.front() = technology;
        auto game = take(campaign::GameSession::create(config));
        const auto identity = cp::game_identities(config);
        const auto saved = take(cp::save_game_checkpoint(game, identity));
        eawr::sim::InlineExecutor executor;
        auto restored = take(cp::load_game_checkpoint(saved, identity, {}, executor));
        expect(restored.session.galaxy().snapshot()->production()->planets.front().offers.front().target_tech == target
            && restored.session.galaxy().snapshot()->production()->planets.front().offers.front().authored_price == source.build_cost_credits
            && take(cp::save_game_checkpoint(restored.session, identity)) == saved,
            "numeric technology offer survives canonical checkpoint round trip");
    }
    source.build_advances_tech_level = -1;
    expect(!campaign::prepare_production_offer(source, prototype, factors), "negative technology offer target refuses");
    source.build_advances_tech_level.reset();
    {
        // GHUD-S57 with a mod's uncorrupted price factor 1.2 (stock FoC authors 1.0 for non-benefiting factions): the
        // prepared price carries it, the authored price does not, so the card compares against the raw cost.
        auto modded = source; modded.build_cost_credits = f(100);
        auto scaled = factors; scaled.factory_count = 1; scaled.corruption_price_multiplier = take(Fixed::from_decimal("1.2"));
        const auto offer = take(campaign::prepare_production_offer(modded, prototype, scaled));
        expect(offer.price == f(120) && offer.authored_price == f(100), "uncorrupted factor in the price, not in the authored cost");
        // A corrupted planet (0.9) replaces the uncorrupted factor: 100 x 0.9 = 90 is a discount against the raw 100.
        auto config = setup();
        galactic::EconomyCorruptionState corruption;
        galactic::CorruptionEconomyPlayer player; player.player = 1; player.faction = 1; player.playable = true;
        player.price = {take(Fixed::from_decimal("1.2")), take(Fixed::from_decimal("0.9"))}; player.time = {f(1), f(1)};
        player.tech_purchase_multiplier = f(1);
        corruption.players = {player};
        for (const auto& planet : config.planets) corruption.planets.push_back({planet.id, {}, false, false, galactic::corruption_fixture::one, {}, {}, {}});
        const auto at = [&] { return take(galactic::prepared_production_offer(offer, nullptr, config.planets, 1, 1, &corruption)); };
        expect(at().price == f(120) && at().authored_price == f(100), "uncorrupted planet keeps the raised price");
        corruption.planets[0].corruption = galactic::corruption_fixture::Type::racketeering;
        expect(at().price == f(90) && at().authored_price == f(100), "corrupted planet price is discounted against the raw cost");
    }
    auto omitted = source; omitted.build_cost_credits.reset(); omitted.build_time_seconds.reset();
    const auto zero = take(campaign::prepare_production_offer(omitted, prototype, factors));
    expect(zero.price == Fixed{} && zero.seconds == Fixed{}
        && !omitted.build_cost_credits && !omitted.build_time_seconds,
        "WGEP-92 omission defaults consume zero without changing authored facts");
    factors.tutorial = true;
    expect(take(campaign::prepare_production_offer(source, prototype, factors)).seconds == f(5), "tutorial ignores ordinary time modifiers");
    expect(take(galactic::production_frames(take(Fixed::from_decimal("0.01")))) == 0, "sub-frame duration truncates to zero");
    eawr::sim::InlineExecutor serial;
    auto config = setup(); config.production->planets[0].offers[0].domain = galactic::ProductionDomain::space;
    auto orbit_game = take(galactic::GalacticSession::create(config));
    submit(orbit_game, buy(0, 1, 1, 1000, galactic::ProductionDomain::space));
    for (int i = 0; i < 4; ++i) (void)take(orbit_game.step(serial));
    expect(orbit_game.snapshot()->fleets().size() == config.fleets.size() + 1, "completed ship uses fleet creation route");
    expect(orbit_game.snapshot()->fleet_control()->detached_units.empty(), "produced identity attached once");
    expect(orbit_game.snapshot()->production()->planets[0].objects[0].paid_cost == f(100), "orbiting result retains paid cost");
    config = setup(); config.production->players[0].pay_as_you_go = true;
    auto gradual = take(galactic::GalacticSession::create(config)); submit(gradual, buy(0, 1));
    (void)take(gradual.step(serial)); expect(gradual.snapshot()->player_economy()->players[0].credits == f(250), "pay-as-you-go does not precharge");
    (void)take(gradual.step(serial));
    const auto charged = gradual.snapshot()->player_economy()->players[0].credits;
    expect(charged < f(250) && charged > f(200), "unfinished service pays a fractional bill");
    submit(gradual, cancel(2, 2, 0)); (void)take(gradual.step(serial));
    // Elapsed/full versus 1-remaining/full can differ by one Q24 ratio unit,
    // multiplied by the stored 100-credit price; bound both directions.
    const auto restored = gradual.snapshot()->player_economy()->players[0].credits.raw();
    expect(restored >= f(250).raw() - 100 && restored <= f(250).raw() + 100, "cancel refunds stored serviced fraction");
}
void base_and_tech_completion() {
    eawr::sim::InlineExecutor serial; auto config = setup();
    auto& planet = config.production->planets[0]; planet.space_level = 0;
    auto station = planet.offers[0]; station.domain = galactic::ProductionDomain::space;
    station.kind = galactic::ProductionKind::starbase; station.base_level = 1; station.population = f(0);
    planet.offers[0] = station; planet.offers[1].domain = galactic::ProductionDomain::space;
    planet.objects.push_back({10000, 1001, 1, f(100), galactic::ProductionDomain::space, galactic::ProductionKind::structure});
    config.production->players[0].counts = {{1001, 1, 1}};
    auto base = take(galactic::GalacticSession::create(config));
    submit(base, buy(0, 1, 1, 1000, galactic::ProductionDomain::space));
    for (int i = 0; i < 4; ++i) (void)take(base.step(serial));
    const auto& completed = base.snapshot()->production()->planets[0];
    expect(completed.space_level == 1 && completed.starbase_type == 1000 && completed.starbase_owner == 1, "station completion records authored level/type/owner");
    expect(completed.objects.empty(), "level-one station clears orbital facilities");
    const auto& counts = base.snapshot()->production()->players[0].counts;
    expect(counts[0].owned == 1 && counts[0].historical == 1 && counts[1].owned == 0 && counts[1].historical == 1, "station creation and facility removal update owned counts without deleting history");
    config = setup(); auto& tech = config.production->planets[0].offers[0];
    tech.kind = galactic::ProductionKind::technology; tech.target_tech = 4; tech.population = f(0);
    auto& unlocked = config.production->planets[0].offers[1]; unlocked.tech = 3;
    unlocked.initially_locked = true; unlocked.slicer_unlockable = true;
    config.production->technology_types = {{1001, 3, true, true, {1}}, {2000, 0, true, true, {1}}};
    auto research = take(galactic::GalacticSession::create(config)); submit(research, buy(0, 1));
    for (int i = 0; i < 4; ++i) (void)take(research.step(serial));
    expect(research.snapshot()->player_economy()->players[0].current_tech == 4, "technology sets authored target rather than adding one");
    expect(research.snapshot()->production()->players[0].unlocked == std::vector<galactic::TypeId>{1001, 2000}, "WGEP-147 completion includes registered types absent from all offers");
}

void shared_technology_writer() {
    auto config = setup(); config.rules.fiscal_cycle_frames = 1000;
    config.production->players[0].locked = {2000};
    config.production->technology_types = {{1001, 2, true, true, {1}}, {2000, 0, true, true, {1}},
        {2001, 4, true, true, {1}}, {2002, 0, true, true, {}}, {2003, 0, false, true, {1}},
        {2004, 0, true, false, {1}}};
    auto account = config.player_economy->players[0]; auto player = config.production->players[0];
    auto assigned = take(galactic::assign_technology(account, player, config.production->technology_types, 2, true,
        [&](auto id, auto tech) {
            expect(id == 1 && tech == account.current_tech && player.unlocked.empty(),
                "WGEP-130 same-level assignment notifies after scalar write and before enumeration");
            return eawr::core::Result<void>::success();
        }));
    expect(assigned == 1 && player.unlocked == std::vector<galactic::TypeId>{2000} && player.locked == std::vector<galactic::TypeId>{2000},
        "WGEP-147 strict boundary, affiliations and independent explicit locks");
    std::string baseline;
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(campaign::GameSession::create(config)); auto& galaxy = game.galaxy();
        std::vector<std::int64_t> notices;
        game.set_technology_notice_handler([&](const auto& notice) {
            expect(notice.kind == galactic::GalacticSession::TechnologyNotice::Kind::assigned,
                "no local observer means no presentation sound request");
            notices.push_back(notice.tech); return eawr::core::Result<void>::success();
        });
        const auto old = galaxy.snapshot();
        expect(take(game.set_technology({1, 2, true, true})) == 2, "direct same-level writer stays active");
        expect(old->production()->players[0].unlocked.empty(), "old publication remains immutable");
        expect(take(game.set_technology({1, 99, false, false})) == 5, "target clamps without changing maximum");
        expect(galaxy.snapshot()->production()->players[0].unlocked == std::vector<galactic::TypeId>{2000}, "explicit unlock flag respected");
        const auto before = galaxy.native_effects().size();
        expect(take(game.increment_technology(1)) == 5 && galaxy.native_effects().size() == before && notices.size() == 2,
            "WGEP-148 maximum increment has no setter/story/unlock pass");
        expect(take(game.set_technology({1, 5, true, true})) == 5 && notices.size() == 3,
            "same-level direct setter differs from maximum increment");
        expect(galaxy.snapshot()->production()->players[0].unlocked == std::vector<galactic::TypeId>{1001, 2000, 2001},
            "bulk unlock covers the whole registry, never just producer offers");
        expect(galaxy.snapshot()->production()->planets[0].space_level == 1, "global technology never changes station level");
        (void)take(game.step(pool));
        const auto identities = cp::game_identities(config);
        const auto saved = take(cp::save_game_checkpoint(game, identities));
        auto restored = take(cp::load_game_checkpoint(saved, identities, {}, pool));
        expect(take(cp::save_game_checkpoint(restored.session, identities)) == saved,
            "writer journal, maximum and complete registry survive canonical reconstruction");
        expect(take(restored.session.increment_technology(1)) == 5, "first restored increment still skips setter at maximum");
        (void)take(game.step(pool)); (void)take(restored.session.step(pool));
        expect(game.galaxy().snapshot()->sha256() == restored.session.galaxy().snapshot()->sha256(), "continued tech readers agree after load");
        const auto hash = galaxy.snapshot()->sha256();
        if (baseline.empty()) baseline = hash;
        else expect(hash == baseline, "shared technology writer equal on 1/2/4/8 workers");
    }
}
galactic::GalacticSetup blocker_setup() {
    auto config = setup(); config.rules.fiscal_cycle_frames = 1000;
    // Both players share a human side: WGEP-24 checks different ownership, not hostility.
    config.players.push_back({2, 1, 0});
    config.seats.push_back({{2}, 2, galactic::SeatKind::human, 1, true, 2});
    config.player_economy->players.push_back({2, f(250), f(10000), 2, 5});
    config.fiscal->players.push_back({2, true, f(1)});
    config.production->players.push_back({2, true, false, true, f(50), f(0), {}, {}, {}});
    auto& station = config.production->planets[0].offers[0];
    station.domain = galactic::ProductionDomain::space;
    station.kind = galactic::ProductionKind::starbase; station.base_level = 2;
    station.population = f(0); station.seconds = take(Fixed::from_decimal("0.11"));
    config.fleets[0].owner = 2; config.fleets[0].orbit = 1;
    config.production->fleet_blockers = std::vector<galactic::FleetProductionBlocker>{{config.fleets[0].id, false, false, true}};
    return config;
}
void live_starbase_blockers() {
    eawr::sim::InlineExecutor serial;
    const auto check = [&](auto mutate, bool blocked) {
        auto config = blocker_setup(); mutate(config);
        auto game = take(galactic::GalacticSession::create(config));
        submit(game, buy(0, 1, 1, 1000, galactic::ProductionDomain::space));
        const auto result = take(game.step(serial));
        const auto& planet = game.snapshot()->production()->planets[0];
        expect(planet.queues[1].empty() == blocked, "WGEP-24 declaration and live owner/orbit admission");
        expect(game.snapshot()->player_economy()->players[0].credits == f(blocked ? 250 : 150), "blocked admission has no debit");
        if (blocked) expect(result.snapshot->events().back().value == static_cast<std::uint64_t>(galactic::ProductionAdmission::slots), "blocker uses existing slot rejection");
    };
    check([](auto&) {}, true);
    check([](auto& c) { c.fleets[0].owner = 1; }, false);
    check([](auto& c) { c.fleets[0].orbit = 2; }, false);
    check([](auto& c) { c.production->fleet_blockers->front().pending_deletion = true; }, false);
    check([](auto& c) { c.production->fleet_blockers->front().skips_space_combat = true; }, false);
    check([](auto& c) { c.production->fleet_blockers->front().cancels_starbase_production = false; }, false);
    check([](auto& c) { c.production->fleet_blockers->clear(); c.production->planets[0].starbase_blocked = true; }, false);
    check([](auto& c) { c.production->fleet_blockers.reset(); c.production->planets[0].starbase_blocked = true; }, true);
    // Admission observes departure immediately, rather than retaining a planet flag.
    auto departed = blocker_setup(); auto departure_game = take(galactic::GalacticSession::create(departed));
    submit(departure_game, orbit(0, {2}, 1, departed.fleets[0].id, 2));
    (void)take(departure_game.step(serial));
    submit(departure_game, buy(1, 1, 1, 1000, galactic::ProductionDomain::space));
    (void)take(departure_game.step(serial));
    expect(departure_game.snapshot()->production()->planets[0].queues[1].size() == 1, "departed fleet releases admission blocker");
    auto travelling = blocker_setup(); travelling.movement.emplace();
    auto& movement = *travelling.movement; movement.mode_modifier = f(1);
    movement.types = {{1000, f(1), f(100), 0, true, false, false, false}};
    movement.players = {{1, f(1), f(0), false}, {2, f(1), f(0), false}};
    for (const auto& planet : travelling.planets) movement.planets.push_back({planet.id, f(1), false});
    for (const auto& player : travelling.players) for (const auto& planet : travelling.planets)
        movement.access.push_back({player.player_id, planet.id, true, false});
    auto transit_game = take(galactic::GalacticSession::create(travelling));
    submit(transit_game, {{0, {2}, 1}, galactic::MoveFleet{travelling.fleets[0].id, {1, 2}, {}}});
    (void)take(transit_game.step(serial));
    expect(transit_game.snapshot()->fleets()[0].motion.transit.has_value(), "real departure retains an in-transit fleet");
    submit(transit_game, buy(1, 1, 1, 1000, galactic::ProductionDomain::space));
    (void)take(transit_game.step(serial));
    expect(transit_game.snapshot()->production()->planets[0].queues[1].size() == 1, "in-transit fleet does not block its retained source orbit");
    auto successor = blocker_setup(); successor.fleets[0].orbit = 2;
    successor.production->planets[0].offers[1].domain = galactic::ProductionDomain::space;
    auto promoted = take(galactic::GalacticSession::create(successor));
    submit(promoted, buy(0, 1, 1, 1000, galactic::ProductionDomain::space));
    submit(promoted, buy(0, 2, 1, 1001, galactic::ProductionDomain::space));
    (void)take(promoted.step(serial));
    submit(promoted, orbit(1, {2}, 1, successor.fleets[0].id, 1));
    (void)take(promoted.step(serial));
    const auto& front = promoted.snapshot()->production()->planets[0].queues[1].front();
    expect(front.type == 1001 && front.start_frame == 1 && front.last_remaining == front.full_seconds, "cancellation starts successor at full duration and returns before billing");
    auto invalid = blocker_setup(); invalid.production->fleet_blockers->push_back(invalid.production->fleet_blockers->front());
    expect(!galactic::validate_setup(invalid), "duplicate blocker identity rejected");
    invalid = blocker_setup(); invalid.production->fleet_blockers->front().fleet = 999999;
    expect(!galactic::validate_setup(invalid), "unknown blocker identity rejected");
    for (bool pending : {false, true}) {
        auto config = blocker_setup(); config.fleets[0].orbit = 2;
        auto game = take(galactic::GalacticSession::create(config));
        submit(game, buy(0, 1, 1, 1000, galactic::ProductionDomain::space));
        submit(game, buy(0, 2, 1, 1001, galactic::ProductionDomain::ground));
        (void)take(game.step(serial));
        // Arrive on the expiry frame: cancellation must precede starbase completion.
        submit(game, orbit(3, {2}, 1, config.fleets[0].id, 1));
        (void)take(game.step(serial)); (void)take(game.step(serial));
        const auto result = take(game.step(serial, pending));
        const auto& planet = result.snapshot->production()->planets[0];
        expect(planet.space_level == (pending ? 2 : 1), "WGEP-38 skips orbit cancellation only with pending conflict");
        expect(planet.queues[1].empty(), "expired front completed or cancelled once");
        expect(result.snapshot->player_economy()->players[0].credits == f(50), "automatic cancellation has no direct refund");
        expect(std::count_if(result.snapshot->events().begin(), result.snapshot->events().end(), [](const auto& e) {
            return e.kind == galactic::GalacticEventKind::production_cancelled;
        }) == (pending ? 0 : 1), "automatic cancellation notification emitted exactly once");
    }
}
// The fixtures directory comes from CMake (argv[1]): __FILE__ is not a checkout path on every build host.
std::filesystem::path fixture_root;
void production_corpus_bytes() {
    // Current production payloads retain an absent optional blocker declaration table.
    const auto directory = fixture_root / "production-corpus";
    eawr::sim::InlineExecutor serial;
    for (std::string_view name : {"queues-basic", "queues-orbit", "queues-paygo"}) {
        std::ifstream file(directory / (std::string(name) + ".eawr-gcreplay"), std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (bytes.empty()) throw std::runtime_error("missing production corpus recording " + (directory / (std::string(name) + ".eawr-gcreplay")).string());
        const auto replay = take(galactic::parse_replay(bytes));
        expect(!replay.setup.production->fleet_blockers, "production retains absent declaration table");
        expect(take(galactic::write_replay(replay)) == bytes, "current production replay bytes round trip");
        std::ifstream pins(directory / (std::string(name) + ".hashes.csv")); std::string line;
        std::getline(pins, line); auto game = take(campaign::GameSession::from_replay(replay));
        while (std::getline(pins, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto hash = line.substr(line.find(',') + 1);
            expect(take(game.step(serial)).state_hash.get() == hash, "current production state pins match");
        }
        expect(game.completed_tick() == replay.final_tick_count, "all current production pins checked");
    }
    auto config = blocker_setup(); config.production->fleet_blockers->clear();
    galactic::GalacticReplay empty{config, 0, {}};
    expect(take(galactic::parse_replay(take(galactic::write_replay(empty)))) == empty, "present-empty live declaration table round trip");
}
void blocker_parity_checkpoint() {
    auto config = blocker_setup(); config.fleets[0].orbit = 2;
    galactic::GalacticReplay replay{config, 8, {buy(0, 1, 1, 1000, galactic::ProductionDomain::space),
        orbit(2, {2}, 1, config.fleets[0].id, 1), orbit(3, {2}, 2, config.fleets[0].id, 2),
        buy(4, 2, 1, 1000, galactic::ProductionDomain::space)}};
    const auto bytes = take(galactic::write_replay(replay));
    expect(take(galactic::parse_replay(bytes)) == replay, "blocker declaration replay round trip");
    for (std::size_t i = 0; i < bytes.size(); i += 29)
        expect(!galactic::parse_replay(std::span(bytes).first(i)), "blocker replay truncation rejected");
    eawr::sim::InlineExecutor serial; auto baseline = take(campaign::GameSession::from_replay(replay));
    std::vector<std::string> hashes;
    for (std::uint64_t i = 0; i < replay.final_tick_count; ++i) hashes.push_back(take(baseline.step(serial)).state_hash.get());
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        for (std::uint64_t barrier : {1U, 3U, 5U}) {
            auto game = take(campaign::GameSession::from_replay(replay));
            while (game.completed_tick() < barrier) (void)take(game.step(pool));
            const auto identity = cp::game_identities(replay.setup);
            const auto saved = take(cp::save_game_checkpoint(game, identity));
            auto loaded = take(cp::load_game_checkpoint(saved, identity, {}, pool));
            expect(take(cp::save_game_checkpoint(loaded.session, identity)) == saved, "blocker checkpoint canonical round trip");
            expect(loaded.session.galaxy().snapshot()->production()->fleet_blockers == config.production->fleet_blockers, "checkpoint retains blocker declarations");
            while (loaded.session.completed_tick() < replay.final_tick_count) {
                const auto tick = loaded.session.completed_tick(); loaded.session.scramble_storage_for_testing();
                expect(take(loaded.session.step(pool)).state_hash.get() == hashes[tick], "blocker arrival/departure 1/2/4/8 equality after restore");
            }
        }
    }
}
galactic::GalacticReplay corpus() {
    auto config = setup(); config.player_economy->players[0].credits = f(2000);
    std::vector<galactic::GameCommand> orders{buy(0, 1), buy(0, 2), buy(0, 3, 2, 1001),
        cancel(1, 4, 1), buy(2, 5), {{3, {1}, 6}, galactic::PauseGalaxy{}},
        {{6, {1}, 7}, galactic::ResumeGalaxy{}}, cancel(7, 8, 0, 1000, galactic::ProductionAction::cancel_type), buy(8, 9)};
    return {config, 16, orders};
}
galactic::GalacticSetup census_setup() {
    auto config = setup(); config.player_economy->players[0].credits = f(2000);
    config.census = galactic::PopulationCensus{{{1000, f(1), 0}, {1001, f(0), 0}}, {{1, 2, false, {}}}, {}};
    return config;
}
void unit_sale() {
    namespace gc = galactic;
    const auto config_for = [&](bool landed) {
        auto config = census_setup(); config.rules.fiscal_cycle_frames = 1000;
        config.census->players.front().base_capacity = 20;
        config.fleet_control->types.front().transport = true;
        config.census->types.push_back({1002, f(3), 0, {1000, 1001}, true, 1000});
        config.census->types.back().authored_price = f(101);
        config.movement.emplace(); auto& movement = *config.movement;
        movement.mode_modifier = f(1);
        movement.types = {{1000, f(3), {}, 0, true, false, false, false, true},
            {1001, {}, {}, 0, false, false, false, false}};
        movement.players = {{1, f(1), {}, false}};
        for (const auto& planet : config.planets) {
            movement.planets.push_back({planet.id, {}, false}); movement.access.push_back({1, planet.id, true, false});
        }
        const auto fleet = config.fleets.front(); const auto carrier = fleet.members.front().id;
        movement.contents = {{carrier, {{30000, 1001}}}};
        config.census->objects = {{carrier, 1002, 1, fleet.orbit, fleet.id, false, false, 1000},
            {30000, 1001, 1, fleet.orbit, carrier, false, false, 1001}};
        config.census->objects.front().paid_cost = f(151); // Discount/markup differs from authored 101.
        if (landed) {
            config.fleets.clear(); config.census->objects.front().container = fleet.orbit;
            config.control.emplace(); auto& control = *config.control;
            control.change_seconds = f(7); control.max_ground_forces = 10;
            control.players = {{1, true, false, config.players.front().faction_id}};
            control.relations = {{1, 1, true}}; control.types = {{1000, 0, false, true}};
            for (const auto& planet : config.planets) {
                gc::ControlPlanet row; row.planet = planet.id; row.type = planet.id; row.surface_accessible = true;
                if (planet.id == fleet.orbit) row.landed.push_back({carrier, 1000, 1});
                control.planets.push_back(row);
            }
            for (const auto& route : config.routes) control.routes.push_back({route.id, f(1)});
        }
        return config;
    };
    const auto query = [&](const auto& config) {
        auto game = take(gc::GalacticSession::create(config)); const auto snapshot = game.snapshot();
        const auto& actor = config.census->objects.front();
        return gc::object_sale(*snapshot->production(), snapshot->sale_types(), snapshot->actor_records(), snapshot->fleets(),
            snapshot->control(), snapshot->movement(), 1, actor.planet, actor.id);
    };
    {
        auto built = config_for(true);
        built.census->objects.clear(); built.movement->contents.clear();
        for (auto& planet : built.control->planets) { planet.landed.clear(); planet.ground_base = {1, 1}; }
        built.production->nested_creation.emplace(); auto& nested = *built.production->nested_creation;
        nested.fleet_type = 1000;
        nested.types = {{1000, 0, gc::NestedCreationRole::transport, {}, false, false, gc::ActorCombatRole::unavailable, {}},
            {1001, 0, gc::NestedCreationRole::other, {}, false, false, gc::ActorCombatRole::unavailable, {}},
            {1002, 1000, gc::NestedCreationRole::company, {1001}, false, false, gc::ActorCombatRole::unavailable, {}}};
        nested.relations = {{1, 1, false}};
        const auto planet_id = built.planets.front().id;
        auto& producer = built.production->planets.front(); auto product = producer.offers.front();
        product.type = 1002; product.population = f(3); product.price = f(151); product.authored_price = f(101);
        producer.offers.push_back(product);
        eawr::sim::InlineExecutor executor;
        auto game = take(campaign::GameSession::create(built));
        (void)take(game.submit(buy(0, 1, planet_id, 1002)));
        (void)take(game.step(executor));
        const auto started = game.galaxy().snapshot();
        for (const auto& event : started->events()) if (event.kind == gc::GalacticEventKind::command_rejected)
            std::cerr << "nested fixture command rejection: " << static_cast<unsigned>(event.reason) << "/" << event.value << '\n';
        expect(started->production()->planets.front().queues[0].size() == 1, "nested sale fixture admits its company purchase");
        expect(started->player_economy()->players.front().credits == f(1849), "nested company purchase debits its actual price");
        for (int i = 0; i < 3; ++i) (void)take(game.step(executor));
        const auto snapshot = game.galaxy().snapshot();
        expect(snapshot->production()->planets.front().queues[0].empty(), "nested sale fixture finishes production before querying its identity");
        const auto actor = std::find_if(snapshot->actor_records().begin(), snapshot->actor_records().end(),
            [](const auto& row) { return row.original_type == 1002; });
        expect(actor != snapshot->actor_records().end() && actor->paid_cost == f(151), "nested completion retains the actual paid cost on the company identity");
        if (actor != snapshot->actor_records().end()) {
            const auto sale = gc::object_sale(*snapshot->production(), snapshot->sale_types(), snapshot->actor_records(), snapshot->fleets(),
                snapshot->control(), snapshot->movement(), 1, planet_id, actor->id);
            expect(sale.admission == gc::ProductionAdmission::allowed && sale.refund == f(76), "newly built company sale uses actual cost, not authored price");
            const auto identity = cp::game_identities(built);
            const auto saved = take(cp::save_game_checkpoint(game, identity));
            auto restored = take(cp::load_game_checkpoint(saved, identity, {}, executor));
            expect(take(cp::save_game_checkpoint(restored.session, identity)) == saved, "nested-produced paid cost checkpoint round trip is canonical");
            const gc::GameCommand sell{{game.completed_tick(), {1}, 2}, gc::SellObject{planet_id, actor->id}};
            (void)take(game.submit(sell)); (void)take(restored.session.submit(sell));
            (void)take(game.step(executor)); (void)take(restored.session.step(executor));
            expect(game.galaxy().snapshot()->player_economy()->players.front().credits == f(1925), "nested company sale credits retained paid cost after production");
            expect(game.galaxy().snapshot()->sha256() == restored.session.galaxy().snapshot()->sha256(), "nested cost refund after restore equals uninterrupted sale");
        }
    }
    auto config = config_for(false);
    expect(query(config).refund == f(76), "unit refund uses retained actual cost, half rounded up");
    auto hero = config; hero.census->types[1].named_hero = true;
    expect(query(hero).admission == gc::ProductionAdmission::unavailable, "default hero cargo prevents sale");
    hero.census->types[1].sale_policy = 1;
    expect(query(hero).admission == gc::ProductionAdmission::allowed, "authored Always permits hero cargo");
    hero.census->types[1].sale_policy = 2;
    expect(query(hero).admission == gc::ProductionAdmission::unavailable, "authored Never cargo refuses sale");
    auto never = config; never.census->types.back().sale_policy = 2;
    expect(query(never).admission == gc::ProductionAdmission::unavailable, "true company Never overrides sellable transport");
    auto free = config; free.census->objects.front().paid_cost = {};
    expect(query(free).refund == f(50), "starting company refund uses original authored integer price");
    auto expensive = free; expensive.census->types.back().authored_price = f(100000000);
    expect(query(expensive).refund == f(50000000), "a starting type absent from production offers keeps its represented authored price without the paid-cost limit");
    auto foreign = config; foreign.census->objects.front().owner = 0;
    expect(query(foreign).admission != gc::ProductionAdmission::allowed, "unowned identity refuses sale");
    std::vector<std::string> baseline;
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        std::vector<std::string> hashes;
        for (bool landed : {false, true}) {
            auto source = config_for(landed); const auto actor = source.census->objects.front();
            gc::GalacticReplay replay{source, 3, {{{1, {1}, 1}, gc::SellObject{actor.planet, actor.id}},
                {{1, {1}, 2}, gc::SellObject{actor.planet, actor.id}}}};
            expect(take(gc::parse_replay(take(gc::write_replay(replay)))) == replay, "unit-sale replay retains costs and command");
            auto game = take(campaign::GameSession::from_replay(replay)); (void)take(game.step(pool));
            const auto identity = cp::game_identities(source); const auto bytes = take(cp::save_game_checkpoint(game, identity));
            auto restored = take(cp::load_game_checkpoint(bytes, identity, {}, pool));
            expect(take(cp::save_game_checkpoint(restored.session, identity)) == bytes, "pre-sale checkpoint retains actual cost canonically");
            const auto old = game.galaxy().snapshot();
            (void)take(game.step(pool)); (void)take(restored.session.step(pool));
            const auto snapshot = game.galaxy().snapshot();
            expect(snapshot->player_economy()->players.front().credits == f(2076), "unit and all cargo refund only the selected identity once");
            expect(snapshot->population().front().used == f(0), "sale releases company population");
            expect(snapshot->fleets().empty() && snapshot->movement()->contents.empty(), "sale releases payload and empty fleet shell");
            expect(std::none_of(snapshot->actor_records().begin(), snapshot->actor_records().end(),
                [&](const auto& row) { return row.id == actor.id || row.id == 30000; }), "sale removes cargo actors");
            expect(old->movement()->contents.size() == 1, "sale preserves prior snapshot payload");
            if (landed) expect(std::all_of(snapshot->control()->planets.begin(), snapshot->control()->planets.end(),
                [](const auto& row) { return row.landed.empty(); }), "landed sale clears surface presence");
            expect(snapshot->events().back().kind == gc::GalacticEventKind::command_rejected, "second sale rejects stale identity");
            expect(snapshot->sha256() == restored.session.galaxy().snapshot()->sha256(), "sale after restore matches uninterrupted run");
            hashes.push_back(snapshot->sha256());
        }
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "ground and orbital sale deterministic on 1/2/4/8");
    }
}
void census_admission() {
    eawr::sim::InlineExecutor serial;
    auto config = census_setup(); auto game = take(galactic::GalacticSession::create(config));
    expect(game.snapshot()->population()[0] == galactic::PopulationTotal{1, f(2), f(1), f(0), true}, "initial live fleet counts before first purchase");
    submit(game, buy(0, 1)); submit(game, buy(0, 2, 2));
    const auto tick = take(game.step(serial));
    expect(tick.snapshot->events().back().value == static_cast<std::uint64_t>(galactic::ProductionAdmission::population), "second planet cannot over-reserve shared cap");
    expect(game.snapshot()->population()[0].reserved == f(1), "queued unit reserves population once");
    submit(game, cancel(1, 3, 0)); submit(game, buy(1, 4, 2)); (void)take(game.step(serial));
    expect(game.snapshot()->production()->planets[1].queues[0].size() == 1, "cancel releases room for later command in same frame");
    expect(game.snapshot()->population()[0].reserved == f(1), "replacement reservation remains once");
    for (int i = 0; i < 3; ++i) (void)take(game.step(serial));
    expect(game.snapshot()->population()[0].used == f(2) && game.snapshot()->population()[0].reserved == f(0), "completion converts reservation into live usage");
    submit(game, buy(5, 5)); const auto full = take(game.step(serial));
    expect(full.snapshot->events().back().value == static_cast<std::uint64_t>(galactic::ProductionAdmission::population), "at-cap admission refused after completion");
    const auto idle = take(game.step(serial));
    expect(idle.work.population_objects == 0 && idle.work.population_producers == 0, "unchanged idle frame does no census scan");
    config = census_setup(); config.census->players[0].base_capacity = 0;
    auto over = take(galactic::GalacticSession::create(config)); submit(over, buy(0, 1, 1, 1001));
    (void)take(over.step(serial)); expect(over.snapshot()->production()->planets[0].queues[0].size() == 1, "zero-pop structure admitted while over cap");
    config = census_setup(); config.production->planets[0].offers[0].current_limit = 1;
    auto limited = take(galactic::GalacticSession::create(config)); submit(limited, buy(0, 1));
    const auto refused = take(limited.step(serial));
    expect(refused.snapshot->events().back().value == static_cast<std::uint64_t>(galactic::ProductionAdmission::current_limit), "current limit sees initial live fleet without supplied counts");
    config = census_setup(); config.production->planets[0].offers[0].domain = galactic::ProductionDomain::space;
    auto orbital = take(galactic::GalacticSession::create(config)); submit(orbital, buy(0, 1, 1, 1000, galactic::ProductionDomain::space));
    for (int i = 0; i < 4; ++i) (void)take(orbital.step(serial));
    expect(orbital.snapshot()->population()[0].used == f(2), "orbital paid-result mirror and fleet member share one census identity");
}
void census_campaign_retained_counts() {
    namespace tactical = eawr::sim::tactical;
    auto config = census_setup();
    config.players.push_back({2, 2, 0});
    config.seats.push_back({{2}, 2, galactic::SeatKind::ai, 0, true, 0});
    config.player_economy->players.push_back({2, f(2000), f(10000), 2, 5});
    config.fiscal->players.push_back({2, false, f(1)});
    config.production->players.push_back({2, false, true, true, f(50), {}, {}, {}, {}});
    config.census->players[0].base_capacity = 100;
    config.census->players.push_back({2, 100, false, {}});
    config.fleets[0].members.push_back({30000, 1000});
    galactic::Fleet defender_fleet;
    defender_fleet.id = 40000; defender_fleet.owner = 2; defender_fleet.orbit = config.fleets[0].orbit;
    defender_fleet.members = {{40001, 1000}};
    config.fleets.push_back(std::move(defender_fleet));
    for (auto& planet : config.production->planets) planet.offers[0].current_limit = 2;

    // WGEP-78: campaign retained and reserve records refer to these same owned
    // IDs, not extra entries in a type-only purchase reinforcement pool.
    campaign::ConflictRecord conflict;
    auto frontier = take(galactic::GalacticSession::create(config));
    conflict.id = frontier.next_entity_id(); conflict.planet = config.fleets[0].orbit;
    conflict.invader = 1; conflict.defender = 2;
    conflict.forces = {{config.fleets[0].members[0].id, 1000, 1, config.fleets[0].id, campaign::ForceRole::retained},
        {30000, 1000, 1, config.fleets[0].id, campaign::ForceRole::reinforcement},
        {40001, 1000, 2, 40000, campaign::ForceRole::deployed}};
    conflict.seats = {{{1}, 1}, {{2}, 2}};
    conflict.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
    conflict.tactical.first_free_entity_id = conflict.id + 1;
    tactical::UnitState defender; defender.entity_id = 40001; defender.type_id = 1000; defender.owner = 2;
    conflict.tactical.units.push_back(defender);
    tactical::TacticalContent content;
    tactical::DurabilityProfile profile; profile.type_id = 1000; profile.max_hull = f(100);
    content.durability.profiles.push_back(profile);
    content.victory.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    content.victory.contenders = {1, 2}; content.victory.controlled_players = {1, 2};
    content.victory.humans = {1}; content.victory.relevant_types = {1000}; content.victory.countdown_frames = 1000;
    cp::GameLoadOptions options;
    options.content = [content](const auto&) { return eawr::core::Result<tactical::TacticalContent>::success(content); };
    const auto identity = cp::game_identities(config);
    const auto owned = [](const campaign::GameSession& game) {
        const auto& counts = game.galaxy().snapshot()->production()->players[0].counts;
        const auto at = std::find_if(counts.begin(), counts.end(), [](const auto& row) { return row.type == 1000; });
        return at == counts.end() ? 0 : at->owned;
    };
    const auto blocked = [](const campaign::GameTick& tick) {
        return std::any_of(tick.galaxy->events().begin(), tick.galaxy->events().end(), [](const auto& event) {
            return event.kind == galactic::GalacticEventKind::command_rejected
                && event.value == static_cast<std::uint64_t>(galactic::ProductionAdmission::current_limit);
        });
    };
    std::vector<std::string> reference;
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(campaign::GameSession::create(config));
        expect(owned(game) == 2, "campaign retained and reserve fleet identities count once, owner only");
        if (auto result = game.submit(buy(0, 1)); !result) throw std::runtime_error(result.error().message);
        auto tick = take(game.step(pool));
        expect(blocked(tick) && game.galaxy().snapshot()->production()->planets[0].queues[0].empty(),
            "retained campaign force reaches current limit before tactical entry");
        std::vector<std::string> hashes{tick.state_hash.get()};
        auto saved = take(cp::save_game_checkpoint(game, identity));
        auto restored = take(cp::load_game_checkpoint(saved, identity, {}, pool));
        expect(take(cp::save_game_checkpoint(restored.session, identity)) == saved, "retained owned census checkpoint is canonical");
        for (auto* session : {&game, &restored.session})
            if (auto result = session->submit(buy(1, 2)); !result) throw std::runtime_error(result.error().message);
        tick = take(game.step(pool)); const auto loaded_tick = take(restored.session.step(pool));
        expect(blocked(tick) && blocked(loaded_tick) && loaded_tick.state_hash.get() == tick.state_hash.get(),
            "retained campaign identity still blocks a purchase after load");
        hashes.push_back(tick.state_hash.get());
        if (auto staged = game.stage_battle(conflict, content); !staged) throw std::runtime_error(staged.error().message);
        hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(game.mode() == campaign::SessionMode::battle && owned(game) == 2,
            "active retained and reserve conflict records do not double-count galactic identities");
        saved = take(cp::save_game_checkpoint(game, identity));
        auto battle_loaded = take(cp::load_game_checkpoint(saved, identity, options, pool));
        expect(battle_loaded.session.mode() == campaign::SessionMode::battle && owned(battle_loaded.session) == 2,
            "active conflict load retains both roles and their exact galactic owned count");
        expect(take(cp::save_game_checkpoint(battle_loaded.session, identity)) == saved,
            "active retained-force checkpoint is canonical");
        for (int i = 0; i < 3; ++i) {
            const auto next = take(game.step(pool)); hashes.push_back(next.state_hash.get());
            expect(take(battle_loaded.session.step(pool)).state_hash.get() == hashes.back(), "active retained-force load continuation equality");
        }
        if (workers == 1) reference = hashes; else expect(reference == hashes, "campaign retained census 1/2/4/8 equality");
    }
    config.production->planets[0].offers[0].current_limit = 3;
    eawr::sim::InlineExecutor serial;
    auto room = take(galactic::GalacticSession::create(config)); submit(room, buy(0, 1)); (void)take(room.step(serial));
    expect(room.snapshot()->production()->planets[0].queues[0].size() == 1,
        "two campaign-backed identities leave one slot at limit three");
}
void company_original_binding() {
    auto config = census_setup();
    config.fleet_control->types[0].transport = true;
    config.census->types.push_back({1002, f(3), 0, {1000, 1001}, true, 1000});
    config.movement.emplace(); auto& movement = *config.movement;
    movement.mode_modifier = f(1);
    movement.types = {{1000, f(3), {}, 0, true, false, false, false, true},
        {1001, {}, {}, 0, false, false, false, false}};
    movement.players = {{1, f(1), {}, false}};
    for (const auto& planet : config.planets) {
        movement.planets.push_back({planet.id, {}, false});
        movement.access.push_back({1, planet.id, true, false});
    }
    const auto& fleet = config.fleets[0]; const auto carrier = fleet.members[0].id;
    movement.contents = {{carrier, {{30000, 1001}}}};
    config.census->objects = {{carrier, 1002, 1, fleet.orbit, fleet.id, false, false},
        {30000, 1001, 1, fleet.orbit, carrier, false, false}};
    expect(bool(galactic::validate_setup(config)), "exact authored company/transport binding admits retained original company identity");
    auto invalid = config; invalid.census->types.back().company = false; invalid.census->types.back().transport = 0;
    expect(!galactic::validate_setup(invalid), "ordinary census identity overrides still refuse");
    invalid = config; invalid.census->types.back().transport = 1001;
    expect(!galactic::validate_setup(invalid), "another authored member is not the exact runtime transport");
    invalid = config; invalid.census->types.back().transport = 9999;
    expect(!galactic::validate_setup(invalid), "unbound company transport refuses");
    invalid = config; invalid.movement->types[0].transport = false; invalid.movement->contents.clear();
    invalid.census->objects[0].original_type = 1000; invalid.census->objects.pop_back();
    expect(!galactic::validate_setup(invalid), "dormant company binding cannot name an ordinary movement type");
    invalid = config; invalid.movement->contents[0].children.clear(); invalid.census->objects.pop_back();
    expect(!galactic::validate_setup(invalid), "company original identity requires nonempty explicit transport contents");
    invalid = config; invalid.census->types.push_back({1003, {}, 0});
    invalid.movement->types.push_back({1003, {}, {}, 0, false, false, false, false});
    invalid.movement->contents[0].children[0].type = 1003; invalid.census->objects[1].original_type = 1003;
    expect(!galactic::validate_setup(invalid), "unrelated carried type cannot acquire company original identity");
    galactic::GalacticReplay replay{config, 4, {}};
    const auto bytes = take(galactic::write_replay(replay));
    expect(take(galactic::parse_replay(bytes)) == replay, "company original/runtime binding tail round trips");
    const auto integer = [&](std::size_t offset, std::size_t width) {
        std::uint32_t value{};
        for (std::size_t i = 0; i < width; ++i) value |= std::uint32_t{bytes.at(offset + i)} << (8 * i);
        return value;
    };
    std::size_t offset = 100;
    for (std::uint32_t i = 0; i < integer(96, 4); ++i) {
        const auto tag = integer(offset, 2), size = integer(offset + 2, 4);
        if (tag == 10) {
            const auto tail = offset + 6 + size - 21;
            auto corrupt = bytes; corrupt[tail] = 2;
            expect(!galactic::parse_replay(corrupt), "unknown census company-tail version refuses");
            corrupt = bytes; corrupt[tail + 1] = 0;
            expect(!galactic::parse_replay(corrupt), "empty census company-tail count refuses");
            corrupt = bytes; corrupt[tail + 5] = 99;
            expect(!galactic::parse_replay(corrupt), "unbound census original company type refuses");
        }
        offset += 6 + size;
    }
    std::vector<std::string> reference;
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(campaign::GameSession::from_replay(replay));
        expect(game.galaxy().snapshot()->population()[0].used == f(3), "population reads original company, not its runtime transport type");
        const auto identity = cp::game_identities(config);
        const auto saved = take(cp::save_game_checkpoint(game, identity));
        auto loaded = take(cp::load_game_checkpoint(saved, identity, {}, pool));
        expect(take(cp::save_game_checkpoint(loaded.session, identity)) == saved, "tick-zero company checkpoint is canonical");
        std::vector<std::string> hashes;
        for (unsigned i = 0; i < 4; ++i) {
            hashes.push_back(take(game.step(pool)).state_hash.get());
            expect(take(loaded.session.step(pool)).state_hash.get() == hashes.back(), "company original metadata survives restored continuation");
        }
        if (workers == 1) reference = hashes; else expect(reference == hashes, "company original/runtime census equal on 1/2/4/8 workers");
    }
    eawr::units::GalacticType type; type.name = "Company"; type.behavior = {"DUMMY_GROUND_COMPANY"};
    type.company_transport = "Carrier"; type.company_units = {"Troop"};
    const std::vector<campaign::FleetTypeBinding> bindings{{1000, "Carrier"}, {1001, "Troop"}};
    const auto prepared = take(campaign::prepare_census_type(type, 1002, bindings));
    expect(prepared.company && prepared.transport == 1000, "company binding is sourced from original behaviour and authored transport link");
    type.company_transport.clear();
    expect(!campaign::prepare_census_type(type, 1002, bindings), "missing authored company transport cannot become a guessed binding");
}
void census_container_and_capacity() {
    eawr::sim::InlineExecutor serial; auto config = census_setup();
    config.census->types.push_back({2000, f(3), 5});
    config.census->types.push_back({2001, f(0), 2});
    config.census->types.push_back({2002, f(0), 7});
    config.census->objects = {
        {1, 2001, 1, 1, 0, false, false},
        {30000, 2000, 1, 1, 0, false, false},
        {30001, 1000, 1, 1, 30000, false, false},
        {30002, 2001, 1, 1, 30000, false, false},
        {30003, 1000, 1, 1, 30002, false, false},
        {30004, 2000, 1, 1, 0, true, false},
        {30005, 2000, 1, 1, 0, false, true},
        {30006, 1000, 1, 1, 30004, false, false}};
    config.production->planets[0].starbase_type = 2002;
    auto game = take(galactic::GalacticSession::create(config));
    const auto total = game.snapshot()->population()[0];
    expect(total.used == f(11), "dead/delete-pending wrappers still count; child suppression reads immediate original only");
    expect(total.capacity == f(18), "capacity skips dead/deleting sources; counts owned planet, live containers and station");
    const auto& counts = game.snapshot()->production()->players[0].counts;
    const auto wrapper_count = std::find_if(counts.begin(), counts.end(), [](const auto& row) { return row.type == 2000; });
    expect(wrapper_count != counts.end() && wrapper_count->owned == 3, "WGEP-78 current counts retain live, dead and delete-pending owned identities");
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto parallel = take(galactic::GalacticSession::create(config));
        expect(take(parallel.step(pool)).state_hash.get() == take(game.step(serial)).state_hash.get(), "container census deterministic across partition executors");
        game = take(galactic::GalacticSession::create(config));
    }
    config.seats[0].kind = galactic::SeatKind::ai;
    config.seats[0].human_side_id = 0;
    config.production->players[0].human = false; config.production->players[0].pay_as_you_go = true;
    config.census->players[0].unusable_planets = {1};
    auto ai = take(galactic::GalacticSession::create(config));
    expect(ai.snapshot()->population()[0].capacity == f(0) && ai.snapshot()->population()[0].used == f(0)
        && ai.snapshot()->population()[0].reserved == f(0) && !ai.snapshot()->population()[0].uses_cap
        && galactic::population_admitted(ai.snapshot()->population()[0], f(100)), "omitted AI cap false reports zero capacity and usage while bypassing admission");
    config.census->players[0].ai_uses_cap = true;
    auto capped = take(galactic::GalacticSession::create(config));
    expect(capped.snapshot()->population()[0].used == f(0) && capped.snapshot()->population()[0].capacity == f(2), "capped AI skips objects and station under unusable planet");
    expect(!galactic::population_admitted(capped.snapshot()->population()[0], f(3)), "enabled AI cap applies room check");
    config.census->objects[4].planet = 2; // Grandchild must follow its two containers to planet 1.
    config.census->objects[4].original_type = 2000;
    auto enclosed = take(galactic::GalacticSession::create(config));
    expect(enclosed.snapshot()->population()[0].used == f(0)
        && enclosed.snapshot()->population()[0].capacity == f(2), "AI enclosing-planet traversal is independent of immediate population suppression");
    config.census->objects[1].container = 30003;
    expect(!galactic::GalacticSession::create(config), "cyclic census containment rejected without an unbounded traversal");
    config = census_setup();
    config.census->types.push_back({2000, f(1), 5});
    config.census->objects = {{1, 1001, 1, 1, 30000, false, false},
        {30000, 2000, 1, 1, 0, false, false}};
    auto contained_producer = take(galactic::GalacticSession::create(config));
    submit(contained_producer, buy(0, 1)); (void)take(contained_producer.step(serial));
    expect(contained_producer.snapshot()->population()[0].reserved == f(0)
        && contained_producer.snapshot()->population()[0].capacity == f(7), "positive immediate parent suppresses producer queues while retaining live capacity");
    config = census_setup(); config.fleet_control->types[0].transport = true;
    config.census->types[1].population = f(3);
    for (auto& planet : config.production->planets) planet.offers[1].population = f(3);
    const auto transport_id = config.fleets[0].members[0].id;
    config.census->objects = {{transport_id, 1000, 1, 1, 0, false, false},
        {30000, 1001, 1, 1, transport_id, false, false}};
    auto transport = take(galactic::GalacticSession::create(config));
    expect(transport.snapshot()->population()[0].used == f(1), "positive original transport population suppresses its cargo");
    submit(transport, {{0, {1}, 1}, galactic::RemoveFleetMembers{config.fleets[0].id, {transport_id}}});
    (void)take(transport.step(serial));
    expect(transport.snapshot()->population()[0].used == f(1), "detaching transport keeps one identity and valid immediate cargo container");
    config.census->types[0].population = f(0);
    for (auto& planet : config.production->planets) planet.offers[0].population = f(0);
    auto empty_wrapper = take(galactic::GalacticSession::create(config));
    expect(empty_wrapper.snapshot()->population()[0].used == f(3), "zero original transport population leaves cargo counted");
}
void census_rounding_and_data() {
    eawr::sim::InlineExecutor serial; auto config = census_setup();
    const auto fraction = take(Fixed::from_decimal("0.4"));
    config.census->types[0].population = fraction;
    for (auto& planet : config.production->planets) planet.offers[0].population = fraction;
    auto same = take(galactic::GalacticSession::create(config)); submit(same, buy(0, 1)); submit(same, buy(0, 2));
    (void)take(same.step(serial)); expect(same.snapshot()->population()[0].reserved == f(1), "producer sum rounds after all queue entries");
    for (auto& planet : config.production->planets) planet.offers[1].population = fraction;
    config.census->types[1].population = fraction;
    config.production->planets[0].offers[1].domain = galactic::ProductionDomain::space;
    auto both = take(galactic::GalacticSession::create(config)); submit(both, buy(0, 1));
    submit(both, buy(0, 2, 1, 1001, galactic::ProductionDomain::space)); (void)take(both.step(serial));
    expect(both.snapshot()->population()[0].reserved == f(1), "ground and space queue sums round together");
    auto separate = take(galactic::GalacticSession::create(config)); submit(separate, buy(0, 1)); submit(separate, buy(0, 2, 2));
    (void)take(separate.step(serial)); expect(separate.snapshot()->population()[0].reserved == f(0), "different producer fractional sums round separately");
    config.census->objects.push_back({30000, 1000, 1, 1, 0, false, false});
    auto fractional_live = take(galactic::GalacticSession::create(config));
    expect(fractional_live.snapshot()->population()[0].used == f(1), "global live usage rounds once after summation");
    eawr::units::GalacticType type; type.population_value = f(3); type.additional_population_capacity = 7;
    expect(take(campaign::prepare_census_type(type, 10)) == galactic::CensusType{10, f(3), 7}, "original type adapter consumes loaded values");
    type.population_value = take(Fixed::from_decimal("3.9"));
    expect(take(campaign::prepare_census_type(type, 10)).population == f(3), "authored population uses the source integer overlay");
    type.population_value.reset(); type.additional_population_capacity.reset();
    expect(take(campaign::prepare_census_type(type, 10)) == galactic::CensusType{10, f(0), 0}, "WGEP-69 omitted type values default zero after inheritance");
    eawr::data::galactic::Planet planet; planet.population_value = 2; planet.additional_population_capacity = 8;
    expect(take(campaign::prepare_census_planet(planet, 11)) == galactic::CensusType{11, f(2), 8}, "planet original population and capacity use their own loaded table");
    eawr::data::galactic::Faction faction;
    expect(campaign::prepare_census_player(faction, 1, {}).base_capacity == 0
        && !campaign::prepare_census_player(faction, 1, {}).ai_uses_cap, "verified omitted faction base/AI defaults");
    faction.base_population_capacity = 12; faction.ai_uses_galactic_pop_cap = true;
    expect(campaign::prepare_census_player(faction, 1, {}).base_capacity == 12
        && campaign::prepare_census_player(faction, 1, {}).ai_uses_cap, "explicit faction policy wins");
    config.census->objects[0].container = 999999;
    expect(!galactic::validate_setup(config), "missing immediate container is rejected");
}
void census_stock_composite_boundary() {
    std::string root;
#if defined(_MSC_VER)
    char* value = nullptr; std::size_t size = 0;
    if (_dupenv_s(&value, &size, "EAWR_EAW_GAME_ROOT") == 0 && value) root = value;
    std::free(value);
#else
    if (const auto* value = std::getenv("EAWR_EAW_GAME_ROOT")) root = value;
#endif
    if (root.empty()) return;
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(root) / folder / "Data")).mount);
    auto filesystem = take(eawr::vfs::Vfs::mount(mounts));
    const auto catalog = take(eawr::data::load_catalog(filesystem, eawr::data::Profile::foc));
    const auto types = eawr::units::galactic_type_table(catalog.catalog);
    const auto* squadron = types.find("Rebel_X-Wing_Squadron");
    expect(squadron && squadron->squadron_units.size() > 1, "stock fighter squadron supplies composite type links");
    if (!squadron) return;
    const std::vector<campaign::FleetTypeBinding> binding{{1000, squadron->name}};
    auto traits = take(campaign::build_fleet_type_traits(types, binding));
    expect(traits[0].requires_live_children, "stock composite fighter needs the unrepresented child service");
    auto config = census_setup(); config.fleet_control->types = std::move(traits);
    config.production->planets[0].offers[0].domain = galactic::ProductionDomain::space;
    const auto admitted = galactic::validate_production(config);
    expect(!admitted && admitted.error().message.find("orbital result requires supported original fleet traits") != std::string::npos,
        "stock composite production cannot reach current atomic census with an incorrect per-component count");
    auto& denied = config.production->planets[0].offers[0]; denied.direct_build_allowed = false;
    expect(static_cast<bool>(galactic::validate_production(config)), "retained denied composite offer validates without atomic completion traits");
    expect(galactic::production_admission({*config.production, config.production->planets[0],
        config.production->players[0], config.player_economy->players[0], config.fleets,
        config.planets[0].owner, nullptr, 10000, 1}, &denied, galactic::ProductionDomain::space)
        == galactic::ProductionAdmission::unavailable, "shared admission refuses retained composite offer before completion");
    const auto* hero = types.find("Luke_Skywalker_Jedi_Team");
    expect(hero && hero->build_limit_current_per_player && !hero->company_units.empty(), "stock hero team carries a current build limit and children");
    if (!hero) return;
    std::vector<campaign::FleetTypeBinding> hero_bindings;
    for (const auto& row : types.types) hero_bindings.push_back({static_cast<galactic::TypeId>(hero_bindings.size() + 1), row.name});
    const auto hero_id = [&](const std::string& name) {
        const auto* original = types.find(name);
        return static_cast<galactic::TypeId>(original - types.types.data() + 1);
    };
    auto stock = census_setup(); stock.fleets.clear(); stock.census->objects.clear(); stock.census->types.clear();
    std::set<galactic::TypeId> pending{hero_id(hero->name)}, gathered;
    while (!pending.empty()) {
        const auto id = *pending.begin(); pending.erase(pending.begin());
        if (!gathered.insert(id).second) continue;
        auto row = take(campaign::prepare_census_type(types.types[id - 1], id, hero_bindings));
        pending.insert(row.members.begin(), row.members.end()); stock.census->types.push_back(std::move(row));
    }
    std::sort(stock.census->types.begin(), stock.census->types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    // Keep synthetic offer population sourced from its own original fixture types.
    const auto team = hero_id(hero->name);
    stock.production->planets[0].offers = {{}};
    auto& hero_offer = stock.production->planets[0].offers[0];
    hero_offer.type = team; hero_offer.price = f(1); hero_offer.seconds = f(1);
    hero_offer.affiliated = true; hero_offer.direct_build_allowed = true;
    hero_offer.population = take(campaign::prepare_census_type(*hero, team, hero_bindings)).population; hero_offer.current_limit = *hero->build_limit_current_per_player;
    for (std::size_t i = 1; i < stock.production->planets.size(); ++i) stock.production->planets[i].offers.clear();
    stock.census->objects.push_back({30000, hero_id(hero->company_units.front()), 1, 1, 0, false, false});
    eawr::sim::InlineExecutor serial;
    auto hero_game = take(galactic::GalacticSession::create(stock));
    submit(hero_game, buy(0, 1, 1, team));
    expect(take(hero_game.step(serial)).snapshot->events()[0].value == static_cast<std::uint64_t>(galactic::ProductionAdmission::current_limit), "stock hero existing only as children cannot exceed its current limit");
}
void census_composite_counts() {
    auto config = census_setup(); config.census->objects.clear(); config.fleets.clear();
    config.census->types = {{1000, f(1), 0, {1001, 1002}}, {1001, f(0), 0, {1003}}, {1002, f(0), 0}, {1003, f(0), 0}};
    config.census->objects = {{30000, 1000, 1, 1, 0, true, true}, {30001, 1002, 1, 1, 0, false, false},
        {30002, 1003, 1, 1, 30000, true, true}, {30003, 1003, 1, 1, 30000, false, false}};
    config.census->players[0].base_capacity = 100;
    for (auto& planet : config.production->planets) planet.offers[0].current_limit = 3;
    galactic::GalacticReplay replay{config, 4, {buy(0, 1)}};
    const auto bytes = take(galactic::write_replay(replay));
    expect(take(galactic::parse_replay(bytes)) == replay, "composite member tail round trips");
    for (std::size_t n = bytes.size() - 20; n < bytes.size(); ++n) expect(!galactic::parse_replay(std::span(bytes).first(n)), "truncated member tail rejected");
    std::vector<std::string> hashes;
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(campaign::GameSession::from_replay(replay));
        const auto& counts = game.galaxy().snapshot()->production()->players[0].counts;
        const auto at = std::find_if(counts.begin(), counts.end(), [](const auto& row) { return row.type == 1000; });
        expect(at != counts.end() && at->owned == 3, "direct original plus recursive MAX counts dead and suppressed children independently of usage");
        const auto tick = take(game.step(pool));
        expect(tick.galaxy->events()[0].kind == galactic::GalacticEventKind::command_rejected
            && tick.galaxy->events()[0].value == static_cast<std::uint64_t>(galactic::ProductionAdmission::current_limit), "composite current limit rejects purchase");
        const auto identity = cp::game_identities(config);
        const auto saved = take(cp::save_game_checkpoint(game, identity));
        auto loaded = take(cp::load_game_checkpoint(saved, identity, {}, pool));
        expect(take(cp::save_game_checkpoint(loaded.session, identity)) == saved, "composite checkpoint canonical round trip");
        std::vector<std::string> actual{tick.state_hash.get()};
        for (int i = 1; i < 4; ++i) {
            const auto next = take(game.step(pool)); actual.push_back(next.state_hash.get());
            expect(take(loaded.session.step(pool)).state_hash.get() == actual.back(), "composite checkpoint continuation equality");
        }
        if (workers == 1) hashes = actual; else expect(hashes == actual, "composite 1/2/4/8 equality");
    }
    auto invalid = config; invalid.census->types[3].members = {1000};
    expect(!galactic::validate_setup(invalid), "cyclic composite graph refused");
    invalid = config; invalid.census->types[3].members = {9999};
    expect(!galactic::validate_setup(invalid), "missing composite member type refused");
    invalid = config; invalid.census->types[0].members = {1002, 1001};
    expect(!galactic::validate_setup(invalid), "unordered composite member graph refused");
    config.production->planets[0].offers[0].current_limit = 4;
    eawr::sim::InlineExecutor serial; auto room = take(galactic::GalacticSession::create(config));
    submit(room, buy(0, 1)); submit(room, buy(0, 2));
    const auto tick = take(room.step(serial));
    expect(room.snapshot()->production()->planets[0].queues[0].size() == 1, "MAX differs from SUM and separately queued parent reserves final limit slot");
    expect(std::any_of(tick.snapshot->events().begin(), tick.snapshot->events().end(), [](const auto& event) {
        return event.kind == galactic::GalacticEventKind::command_rejected && event.value == static_cast<std::uint64_t>(galactic::ProductionAdmission::current_limit);
    }), "second queued composite purchase reaches limit");
}
void census_corpus_checkpoint() {
    eawr::sim::InlineExecutor serial;
    auto config = census_setup();
    galactic::GalacticReplay replay{config, 10, {buy(0, 1), buy(0, 2, 2), cancel(1, 3, 0), buy(1, 4, 2), buy(5, 5)}};
    const auto bytes = take(galactic::write_replay(replay));
    expect(take(galactic::parse_replay(bytes)) == replay, "census replay extension round trip");
    for (std::size_t i = 0; i < bytes.size(); i += 31) expect(!galactic::parse_replay(std::span(bytes).first(i)), "truncated census replay refused");
    auto baseline = take(campaign::GameSession::from_replay(replay)); std::vector<std::string> hashes;
    for (int i = 0; i < 10; ++i) hashes.push_back(take(baseline.step(serial)).state_hash.get());
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        for (std::uint64_t barrier : {1U, 2U, 5U}) {
            auto game = take(campaign::GameSession::from_replay(replay));
            while (game.completed_tick() < barrier) (void)take(game.step(pool));
            const auto identity = cp::game_identities(replay.setup);
            const auto saved = take(cp::save_game_checkpoint(game, identity));
            auto restored = take(cp::load_game_checkpoint(saved, identity, {}, pool));
            expect(take(cp::save_game_checkpoint(restored.session, identity)) == saved, "checkpoint retains live population and queued reservations canonically");
            while (restored.session.completed_tick() < 10) {
                const auto index = restored.session.completed_tick(); restored.session.scramble_storage_for_testing();
                expect(take(restored.session.step(pool)).state_hash.get() == hashes[index], "census replay 1/2/4/8 parity after reservation load");
            }
        }
    }
}
void parity_checkpoint_case(bool seat_policy, bool census = false) {
    auto replay = corpus();
    if (seat_policy) {
        replay.setup.seat_control.emplace();
        replay.setup.seat_control->teams.push_back({1, {1}, false});
        replay.setup.seat_control->controllers.push_back({1, galactic::FactionControl::human,
            galactic::FactionControl::ai, {}, {}});
        replay.setup.movement.emplace();
        auto& movement = *replay.setup.movement;
        movement.mode_modifier = f(1); // explicit synthetic inputs, not retail defaults
        movement.types = {{1000, f(2), f(100), 0, true, false, false, false}};
        movement.players = {{1, f(1), f(0), false}};
        for (const auto& planet : replay.setup.planets) {
            movement.planets.push_back({planet.id, f(1), false});
            movement.access.push_back({1, planet.id, true, false});
        }
        for (auto& planet : replay.setup.production->planets) planet.offers[0].domain = galactic::ProductionDomain::space;
        for (auto& command : replay.commands) if (auto* order = std::get_if<galactic::ProductionCommand>(&command.payload); order && order->type == 1000)
            order->domain = galactic::ProductionDomain::space;
        auto nested = replay.setup;
        const auto container = nested.fleets[0].members[0].id;
        nested.fleets[0].members[0].type = 9000;
        nested.fleet_control->types.push_back({9000, false, true, true});
        nested.movement->types.push_back({9000, f(0), f(100), 0, false, false, true, false});
        nested.movement->contents.push_back({container, {{10000, 1000}}});
        expect(static_cast<bool>(galactic::validate_setup(nested)), "production and valid movement children coexist");
        nested.census = galactic::PopulationCensus{{{1000, f(1), 0}, {1001, f(0), 0}, {9000, f(0), 0}}, {{1, 2, false, {}}}, {}};
        auto nested_game = take(galactic::GalacticSession::create(nested));
        expect(nested_game.snapshot()->population()[0].used == f(1), "census gathers movement child originals under their zero-population parent");
        nested.census->types[2].population = f(2);
        auto suppressed_game = take(galactic::GalacticSession::create(nested));
        expect(suppressed_game.snapshot()->population()[0].used == f(2), "positive original movement parent suppresses its immediate child population");
        nested.production->planets[0].objects.push_back({10000, 1001, 1, f(100), galactic::ProductionDomain::ground, galactic::ProductionKind::structure});
        expect(!galactic::validate_setup(nested), "production identities cannot reuse a movement child ID");
    }
    if (census) replay.setup.census = galactic::PopulationCensus{
        {{1000, f(1), 0}, {1001, f(0), 0}}, {{1, 50, false, {}}}, {}};
    if (seat_policy && census) {
        // Live control reads every initial member once production publishes actor facts.
        for (const auto& fleet : replay.setup.fleets) for (const auto& member : fleet.members) {
            galactic::CensusObject actor;
            actor.id = member.id; actor.original_type = actor.current_type = member.type;
            actor.owner = fleet.owner; actor.planet = fleet.orbit; actor.container = fleet.id;
            actor.combat_role = galactic::ActorCombatRole::ordinary;
            replay.setup.census->objects.push_back(actor);
        }
        std::sort(replay.setup.census->objects.begin(), replay.setup.census->objects.end(),
            [](const auto& a, const auto& b) { return a.id < b.id; });
        replay.setup.control.emplace();
        auto& control = *replay.setup.control;
        control.change_seconds = f(7); control.max_ground_forces = 10;
        // Synthetic one-player control provider: its sole player is neutral.
        control.players = {{1, true, false, replay.setup.players[0].faction_id}};
        control.relations = {{1, 1, true}};
        control.types = {{1000, 1, false, false}};
        for (const auto& planet : replay.setup.planets) {
            galactic::ControlPlanet row; row.planet = planet.id; row.type = planet.id;
            row.surface_accessible = true; row.ground_base = {1, 1}; row.starbase = {1, 1};
            control.planets.push_back(row);
        }
        for (const auto& route : replay.setup.routes) control.routes.push_back({route.id, f(1)});
        auto& constructors = replay.setup.production->nested_creation.emplace();
        constructors.fleet_type = 1000;
        constructors.types = {{1000, 0, {}, {}, false, false, galactic::ActorCombatRole::unavailable, {}},
            {1001, 0, {}, {}, false, false, galactic::ActorCombatRole::unavailable, {}}};
        constructors.relations = {{1, 1, false}};
        for (auto& type : constructors.types) type.combat_role = galactic::ActorCombatRole::ordinary;
    }
    // The seat-policy+census case enables every currently supported header tag:
    // graph, fleet, economy, movement, seats, fiscal, production, control and census.
    const auto bytes = take(galactic::write_replay(replay));
    expect(take(galactic::parse_replay(bytes)) == replay, "production setup/action replay round trip");
    for (std::size_t i = 0; i < bytes.size(); i += 29) expect(!galactic::parse_replay(std::span(bytes).first(i)), "truncation is rejected");
    eawr::sim::InlineExecutor serial; auto baseline = take(campaign::GameSession::from_replay(replay));
    std::vector<std::string> hashes;
    for (int i = 0; i < 16; ++i) hashes.push_back(take(baseline.step(serial)).state_hash.get());
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        for (std::uint64_t barrier : {2U, 4U, 7U}) {
            auto game = take(campaign::GameSession::from_replay(replay));
            while (game.completed_tick() < barrier) (void)take(game.step(pool));
            const auto identity = cp::game_identities(replay.setup); auto bytes_saved = take(cp::save_game_checkpoint(game, identity));
            auto loaded = take(cp::load_game_checkpoint(bytes_saved, identity, {}, pool));
            expect(take(cp::save_game_checkpoint(loaded.session, identity)) == bytes_saved, "half-built queue checkpoint canonical round trip");
            while (loaded.session.completed_tick() < 16) {
                loaded.session.scramble_storage_for_testing(); const auto index = loaded.session.completed_tick();
                const auto tick = take(loaded.session.step(pool));
                expect(tick.state_hash.get() == hashes[index], "1/2/4/8 equality after pause, cancellation and load");
            }
        }
    }
}
void parity_checkpoint() { parity_checkpoint_case(false); parity_checkpoint_case(true); parity_checkpoint_case(true, true); }
void write_corpus(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    for (std::string_view name : {"queues-basic", "queues-orbit", "queues-paygo"}) {
        auto replay = corpus();
        if (name == "queues-orbit") {
            for (auto& planet : replay.setup.production->planets) planet.offers[0].domain = galactic::ProductionDomain::space;
            for (auto& command : replay.commands) if (auto* order = std::get_if<galactic::ProductionCommand>(&command.payload); order && order->type == 1000) order->domain = galactic::ProductionDomain::space;
        }
        if (name == "queues-paygo") replay.setup.production->players[0].pay_as_you_go = true;
        const auto path = directory / (std::string(name) + ".eawr-gcreplay");
        const auto pin_path = directory / (std::string(name) + ".hashes.csv");
        if (std::filesystem::exists(path) || std::filesystem::exists(pin_path)) throw std::runtime_error("refusing to replace existing corpus pins");
        const auto bytes = take(galactic::write_replay(replay));
        std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        auto game = take(campaign::GameSession::from_replay(replay)); eawr::sim::InlineExecutor serial;
        std::ostringstream csv; csv << "tick,sha256\n";
        for (std::uint64_t i = 0; i < replay.final_tick_count; ++i) {
            const auto tick = take(game.step(serial)); csv << tick.completed_tick << ',' << tick.state_hash.get() << '\n';
        }
        std::ofstream pins(pin_path, std::ios::binary); pins << csv.str();
        if (!file || !pins) throw std::runtime_error("could not author new queue corpus");
    }
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--offer-projection") {
            offer_projection(); return failures == 0 ? 0 : 1;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--ground-sale") {
            ground_structure_sale(); unit_sale(); return failures == 0 ? 0 : 1;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--retained-census") {
            census_campaign_retained_counts(); return failures == 0 ? 0 : 1;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--write-corpus") { write_corpus(argv[2]); return 0; }
        else if (argc != 2) throw std::runtime_error("expected <fixtures directory> or --write-corpus <new directory>");
        fixture_root = argv[1];
        ground_structure_sale(); unit_sale();
        admission(); offer_projection(); orbit_completion_shares_slot_card(); technology_price_production(); cancel_identity(); timing_and_cancel(); inactive_work_budget(); refund_recipient(); queue_policies(); content_and_placement(); base_and_tech_completion(); live_starbase_blockers(); production_corpus_bytes(); blocker_parity_checkpoint(); parity_checkpoint();
        shared_technology_writer(); census_admission(); census_campaign_retained_counts(); company_original_binding(); census_container_and_capacity(); census_rounding_and_data(); census_composite_counts(); census_stock_composite_boundary(); census_corpus_checkpoint();
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return failures == 0 ? 0 : 1;
}
