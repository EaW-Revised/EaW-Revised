#include <variant>
#include "../support/env.hpp"
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "eawr/sim/galactic/income.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/vfs/vfs.hpp"
#include "ability_ruins_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using eawr::testing::take;

namespace {
using namespace galactic_test;
using eawr::sim::math::Fixed;
namespace gc = eawr::sim::galactic;

Fixed f(std::int64_t n) { return take(Fixed::from_integer(n)); }

Fixed d(std::string_view n) { return take(Fixed::from_decimal(n)); }
gc::GalacticSetup setup() {
    gc::SyntheticGalaxyOptions options; options.players = 2; options.planets = 4;
    options.fleets_per_player = 1; options.members_per_fleet = 1; options.debug_commands = true;
    auto s = gc::synthetic_galaxy(options);
    s.rules.fiscal_cycle_frames = 3; s.rules.maximum_fleet_movement_distance = f(10000);
    s.fleet_control = gc::FleetControlSetup{{}, {{1000, false, true, false}}, {}};
    s.player_economy = gc::PlayerEconomySetup{{{1, f(1000), f(10000), 2, 5}, {2, f(1000), f(10000), 2, 5}}};
    s.fiscal = gc::FiscalSetup{f(2500), {{1, true, f(1)}, {2, true, f(1)}}, {}};
    s.production.emplace(); s.production->maximum_human_queue = 5;
    for (gc::PlayerId player = 1; player <= 2; ++player)
        s.production->players.push_back({player, true, false, true, f(100), {}, {}, {}, {}});
    s.income.emplace();
    gc::EconomicType planet; planet.type = 900;
    planet.reductions = {{2000, 1, d("0.2"), false, true}, {2000, 1, d("0.25"), true, true}};
    gc::EconomicType hero; hero.type = 1000; hero.mining = gc::PlanetIncomeAbility{f(2), {}, d("1.5")};
    hero.gambling = gc::GamblingIncomeAbility{d("0.8"), d("-0.2"), d("0.6"), 9};
    hero.reductions = {{2000, 1, d("0.4"), true, false}, {2000, 2, d("0.1"), true, false}};
    s.income->types = {planet, hero}; s.income->sources = {{1, 900, 1, 1, true, true}};
    for (const auto& row : s.planets) {
        s.fiscal->planets.push_back({row.id, f(10)});
        s.income->planets.push_back({row.id, row.id == 1, false, false});
        gc::PlanetProduction p; p.planet = row.id; p.target_owner = row.owner; p.starbase_owner = row.owner;
        p.enabled = true; p.surface_accessible = true; p.ground_slots = 5; p.space_slots = 5; p.maximum_ground_forces = 100;
        gc::ProductionOffer offer; offer.type = 2000; offer.price = f(100); offer.seconds = f(10);
        offer.kind = gc::ProductionKind::structure; offer.affiliated = true; offer.direct_build_allowed = true;
        p.offers.push_back(offer); s.production->planets.push_back(p);
        s.income->offers.push_back({row.id, 2000, f(100), f(10)});
    }
    return s;
}
gc::GameCommand buy(std::uint64_t tick, std::uint64_t sequence) {
    return {{tick, {1}, sequence}, gc::ProductionCommand{1, 2000, gc::ProductionAction::buy, gc::ProductionDomain::ground, 0, {}}};
}
void math_rules() {
    expect(take(gc::mining_income_bonus(f(100), f(2), {}, false, d("1.5"))) == f(200), "WGEP-10 two is plus 200 percent");
    expect(take(gc::mining_income_bonus(f(100), f(2), f(10), true, d("1.5"))) == f(315), "colony multiplies the whole bonus");
    const std::vector<gc::ProductionReduction> a{{1, 1, d("0.2")}, {2, 2, d("0.1")}};
    const std::vector<gc::ProductionReduction> b{{3, 1, d("0.4")}};
    expect(take(gc::production_reduction_multiplier(a, b, true)) == d("0.5"), "global/local category maximum then sum");
    const std::vector<gc::ProductionReduction> large{{1, 1, f(2)}};
    expect(take(gc::production_reduction_multiplier(large, {}, true)) == take(eawr::sim::math::subtract(f(1), d("0.999999"))), "price clamp only");
    expect(take(gc::production_reduction_multiplier(large, {}, false)) == f(-1), "time remains negative");
    gc::GamblingIncomeAbility gambling{f(1), d("-0.2"), d("0.6"), 900};
    expect(take(gc::gambling_income_sample(f(100), gambling, 0, 32767)) == take(eawr::sim::math::multiply(f(100), d("0.6"))), "signed winning endpoint");
    expect(take(gc::gambling_income_sample(f(100), gambling, 32767, 0)) == take(eawr::sim::math::multiply(f(100), d("-0.2"))), "chance equality loses");
    expect(!gc::gambling_income_sample(f(100), gambling, 32768, 0), "out of range synchronized sample rejected");
}
// WGEP-05/10/11, WGCH-143: live orbital sources can read a ruined planet;
// the ruins action's separate termination of existing sources is unchanged.
void ruined_income_readers() {
    for (const auto base : {7, 0}) for (const bool live_ruin : {false, true}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter pool(workers);
            auto config = ability_fixture::ruins_setup(false);
            const auto planet = config.planets[0].id;
            config.rules.fiscal_cycle_frames = 100000;
            config.economy_corruption->planets[0].corruption = gc::corruption_fixture::Type::none;
            config.economy_corruption->planets[0].siphon = {};
            config.control->planets[0].destroyed = !live_ruin;
            config.control->planets[0].surface_accessible = false;
            config.ability_host->ruined_income[0].credits = f(base);
            // WGUI-13: this contract observes the complete income breakdown.
            config.observer->local_player = config.planets[0].owner;
            const auto intelligence = gc::bit(gc::Intelligence::income_breakdown);
            for (auto& row : config.observer->players) row.default_intelligence = intelligence;
            for (auto& row : config.observer->records) row.mask = intelligence;
            config.income.emplace();
            gc::EconomicType source; source.type = ability_fixture::orbital_type;
            source.mining = gc::PlanetIncomeAbility{f(2), f(3), d("1.5")};
            source.gambling = gc::GamblingIncomeAbility{f(1), d("-0.2"), d("0.6"), 1};
            config.income->types = {source};
            for (const auto& row : config.planets)
                config.income->planets.push_back({row.id, row.id == planet, false, false});
            if (live_ruin) {
                // Keep only an orbital income source: no land-loss callback is needed.
                config.control->planets[0].landed.clear();
                config.movement->contents.clear();
                std::erase_if(config.census->objects, [](const auto& row) {
                    return row.id == ability_fixture::transport || row.id == ability_fixture::hero;
                });
                std::erase_if(config.production->planets[0].objects, [](const auto& row) {
                    return row.domain == gc::ProductionDomain::ground;
                });
                config.observer->planets[0].landed_transports.clear();
                for (auto& row : config.observer->records) row.sources.clear();
            }
            auto game = take(gc::GalacticSession::create(config));
            if (live_ruin) {
                const auto& warm = game.snapshot()->economy_forecast()->planets[0];
                const auto ordinary = take(gc::mining_income_bonus(f(100), f(2), f(3), true, d("1.5")));
                expect(warm.basic == f(100) && warm.modifiers == std::optional(
                    std::vector<gc::PlanetIncomeForecast::Modifier>{{source.type, ordinary}}),
                    "intact income breakdown warms the publication cache");
                expect(take(game.request_planet_ruins(planet)), "live ruins applies without ground income source losses");
            }
            const auto mining = take(gc::mining_income_bonus(f(base), f(2), f(3), true, d("1.5")));
            const auto& first = game.snapshot()->economy_forecast()->planets[0];
            expect(first.basic == f(base), "ruined forecast selects the authored base, including zero");
            expect(first.modifiers == std::optional(std::vector<gc::PlanetIncomeForecast::Modifier>{{source.type, mining}}),
                "ruined mining publication uses the authored base and colony multiplier");
            auto inputs = *config.income; inputs.planets[0].destroyed = true;
            inputs.sources = {{ability_fixture::orbital_structure, source.type, planet, config.planets[0].owner, false, true}};
            expect(take(gc::planet_income_bonus(inputs, planet, config.planets[0].owner, f(base))) == mining,
                "standalone mining reader accepts a ruined current planet");
            bool sampled = false;
            std::vector<std::string> hashes;
            for (int i = 0; i < 4; ++i) {
                const auto sample_frame = game.clock().galactic_frame;
                const auto result = take(game.step(pool)); hashes.push_back(result.state_hash.get());
                for (const auto& event : game.snapshot()->events()) {
                    if (event.kind != gc::GalacticEventKind::gambling_income_sampled) continue;
                    sampled = true;
                    eawr::sim::tactical::CombatRandom random(config.seed, sample_frame,
                        ability_fixture::orbital_structure, 0xfffe000dU); // existing WGEP-76 sample slot
                    const auto win = random.uniform(0, 32767), factor = random.uniform(0, 32767);
                    const auto expected = take(gc::gambling_income_sample(f(base), *source.gambling, win, factor));
                    expect(event.value == static_cast<std::uint64_t>(expected.raw()), "ruined gambling samples use destroyed base with synchronized draws");
                    const auto& forecast = game.snapshot()->economy_forecast()->planets[0];
                    const auto total = take(eawr::sim::math::add(take(eawr::sim::math::add(f(base), mining)), expected));
                    expect(forecast.actual == total, "ruined forecast retains base, mining and gambling contributions");
                }
            }
            expect(sampled, "live gambling source samples on a ruined planet");
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "ruined income is equal on 1/2/4/8 workers");
        }
    }
}
void nested_sources() {
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto config = setup();
        const auto fleet = config.fleets[0].id;
        const auto container = config.fleets[0].members[0].id;
        const eawr::sim::EntityId inner = 900000, hero = 900001, companion = 900002;
        config.fleets[0].members[0].type = 1100;
        config.fleets[0].members.push_back({companion, 1000});
        config.fleet_control->types.push_back({1100, false, true, false});
        config.movement.emplace(); auto& movement = *config.movement;
        movement.mode_modifier = f(1);
        movement.types = {{1000, f(2), f(100), 0, true, false, false, false},
            {1100, {}, {}, 0, false, false, true, false}};
        movement.contents = {{container, {{inner, 1100}}}, {inner, {{hero, 1000}}}};
        for (const auto& player : config.players) {
            movement.players.push_back({player.player_id, f(1), f(1), false});
            for (const auto& planet : config.planets)
                movement.access.push_back({player.player_id, planet.id, true, false});
        }
        for (const auto& planet : config.planets) movement.planets.push_back({planet.id, {}, false});
        config.census.emplace(); auto& census = *config.census;
        census.types = {{900, {}, 0}, {1000, {}, 0}, {1100, {}, 0}, {2000, {}, 0}};
        census.players = {{1, 100, false, {}}, {2, 100, false, {}}};
        for (const auto& row : config.fleets) for (const auto& member : row.members)
            census.objects.push_back({member.id, member.type, row.owner, row.orbit, row.id,
                false, false, member.type});
        census.objects.push_back({inner, 1100, 1, 1, container, false, false, 1100});
        census.objects.push_back({hero, 1000, 1, 1, inner, false, false, 1000});
        for (auto& actor : census.objects) {
            actor.combat_role = actor.original_type == 1100 ? gc::ActorCombatRole::fleet : gc::ActorCombatRole::ordinary;
            actor.abilities.present = true; actor.garrison = false;
        }
        std::sort(census.objects.begin(), census.objects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        config.observer.emplace(); auto& observer = *config.observer;
        observer.local_player = 1;
        const auto intelligence = gc::bit(gc::Intelligence::income_breakdown);
        for (const auto& player : config.players) observer.players.push_back({player.player_id, false, false, intelligence});
        for (const auto& planet : config.planets) {
            gc::ObserverPlanet row; row.planet = planet.id;
            row.allegiance = row.target_owner = row.current_owner = row.current_controller = planet.owner;
            observer.planets.push_back(row);
        }
        observer.types = {{1000}, {1100}};
        for (const auto& player : config.players) for (const auto& planet : config.planets) {
            gc::ObserverRecord record; record.player = player.player_id; record.planet = planet.id;
            record.discovered = true; record.mask = intelligence; observer.records.push_back(record);
        }
        config.income->types[1].gambling.reset();
        config.income->sources.push_back({hero, 1000, 1, 1, false, true});
        auto invalid = config; invalid.income->sources.back().planet = 3;
        expect(!gc::GalacticSession::create(invalid), "nested income binding rejects a location different from its live parent");
        auto game = take(eawr::campaign::GameSession::create(config));
        std::vector<std::string> hashes;
        const auto step = [&] { hashes.push_back(take(game.step(pool)).state_hash.get()); };
        const auto bonus = [&](gc::PlanetId planet) {
            const auto* forecast = game.galaxy().snapshot()->economy_forecast();
            const auto& rows = forecast->planets[planet - 1].modifiers;
            expect(rows.has_value(), "nested source breakdown is available");
            return rows ? std::count_if(rows->begin(), rows->end(), [](const auto& row) { return row.source_type == 1000; }) : 0;
        };
        expect(bonus(1) == 2, "carried hero and direct companion each contribute exactly once");
        take(game.submit(buy(0, 1))); step();
        expect(game.galaxy().snapshot()->production()->planets[0].queues[0][0].price == f(50),
            "nested discount uses category winners alongside the planet discount");
        take(game.submit({{1, {1}, 2}, gc::RemoveFleetMembers{fleet, {container}}})); step();
        expect(bonus(1) == 2, "detaching a team retains its nested income source");
        take(game.submit({{2, {1}, 3}, gc::AddFleetMembers{fleet, {container}}})); step();
        expect(bonus(1) == 2, "reattaching does not duplicate the nested source");
        step(); // GFD-04: the synthetic window is due before the fourth turn's timer advance.
        expect(game.galaxy().snapshot()->player_economy()->players[0].credits == f(1030),
            "fiscal payment includes both nested and direct mining income from the forecast");
        take(game.submit(orbit(4, {1}, 4, fleet, 3))); step();
        expect(bonus(1) == 0 && bonus(3) == 2, "nested sources follow the team to another owned planet");
        take(game.submit(buy(5, 5))); step();
        expect(game.galaxy().snapshot()->production()->planets[0].queues[0][0].price == f(50)
            && game.galaxy().snapshot()->production()->planets[0].queues[0][1].price == f(75),
            "departure changes new prices while existing queue entries retain their purchase price");
        take(game.submit({{6, {1}, 6}, gc::MoveFleet{fleet, {3, 4, 1}, {}}})); step();
        expect(bonus(1) == 0 && bonus(3) == 0, "nested and direct economic actors contribute nowhere in transit");
        const auto identities = eawr::checkpoint::game_identities(config);
        auto restored = take(eawr::checkpoint::load_game_checkpoint(
            take(eawr::checkpoint::save_game_checkpoint(game, identities)), identities, {}, pool));
        expect(*restored.session.galaxy().snapshot()->economy_forecast()
            == *game.galaxy().snapshot()->economy_forecast(), "checkpoint restores nested forecast exactly");
        auto loss_session = take(gc::GalacticSession::from_replay(game.recording().commands));
        for (int tick = 0; tick < 7; ++tick) (void)take(loss_session.step(pool));
        const std::vector<eawr::sim::EntityId> dead{hero};
        take(loss_session.apply_battle_losses(dead));
        take(loss_session.submit(orbit(7, {1}, 7, fleet, 1)));
        hashes.push_back(take(loss_session.step(pool)).state_hash.get());
        const auto& remaining = loss_session.snapshot()->economy_forecast()->planets[0].modifiers;
        expect(remaining && remaining->size() == 1 && remaining->front().source_type == 1000,
            "loss removes a nested actor without reviving it when the surviving team returns");
        take(game.submit(orbit(7, {1}, 7, fleet, 1)));
        take(restored.session.submit(orbit(7, {1}, 7, fleet, 1)));
        const auto next = take(game.step(pool)); hashes.push_back(next.state_hash.get());
        expect(bonus(1) == 2, "checkpoint continuation returns both surviving economic sources");
        expect(take(restored.session.step(pool)).state_hash.get() == next.state_hash.get(),
            "nested-source checkpoint continues equally after transit");
        if (reference.empty()) reference = hashes;
        else expect(reference == hashes, "nested income and price lifecycle is equal on 1/2/4/8 workers");
    }
}
void lifecycle_and_queues() {
    eawr::sim::InlineExecutor serial;
    auto config = setup();
    auto game = take(gc::GalacticSession::create(config));
    expect(game.snapshot()->income()->gambling.size() == 2, "initial live sources registered");
    for (const auto& row : game.snapshot()->income()->gambling)
        expect(row.scheduled && row.deadline <= 9 && row.income == Fixed{}, "inclusive first phase retained without activation sample");
    take(game.submit(buy(0, 1)));
    (void)take(game.step(serial));
    const auto entry = game.snapshot()->production()->planets[0].queues[0][0];
    const auto seconds = take(eawr::sim::math::multiply(f(10), take(eawr::sim::math::subtract(f(1), d("0.2")))));
    expect(entry.price == f(50) && entry.full_seconds == seconds, "local and global reductions applied at admission");
    const auto fleet = config.fleets[0].id;
    take(game.submit(orbit(1, {1}, 2, fleet, 3)));
    (void)take(game.step(serial));
    take(game.submit(buy(2, 3))); (void)take(game.step(serial));
    const auto& queue = game.snapshot()->production()->planets[0].queues[0];
    expect(queue[0].price == entry.price && queue[0].full_seconds == entry.full_seconds, "WGEP-44 retained entry values after moving hero");
    expect(queue[1].price == f(75), "moved hero stops contributing at old location");
    auto inputs = *config.income;
    auto owners = config.planets;
    owners[0].owner = 2;
    expect(take(gc::income_production_multiplier(inputs, owners, 2, 1, 2000, true)) == f(1), "capture removes old global recipient");
    expect(take(gc::income_production_multiplier(inputs, owners, 2, 2, 2000, true)) == d("0.75"), "capture rebinds global source");
    inputs.planets[0].intimidation = true;
    expect(take(gc::income_production_multiplier(inputs, owners, 2, 2, 2000, true)) == f(1), "intimidation rejects global application");
    inputs.planets[0].intimidation = false; inputs.planets[0].destroyed = true;
    expect(take(gc::income_production_multiplier(inputs, owners, 2, 2, 2000, true)) == f(1), "destroyed source rejected");
    inputs.planets[0].destroyed = false; inputs.types[0].reductions[1].galaxy_wide = false;
    expect(take(gc::income_production_multiplier(inputs, owners, 1, 2, 2000, true)) == d("0.75"), "local planet source retained on capture");
    // #3003: one gathered source list serves every planet/player/price query unchanged.
    inputs.planets[1].intimidation = true;
    const auto gathered = gc::production_reduction_sources(inputs, owners);
    for (const auto planet : {gc::PlanetId{1}, gc::PlanetId{2}}) for (const auto who : {gc::PlayerId{1}, gc::PlayerId{2}})
        for (const bool price : {true, false})
            expect(take(gc::income_production_multiplier(gathered, planet, who, 2000, price))
                == take(gc::income_production_multiplier(inputs, owners, planet, who, 2000, price)), "gathered reductions equal per-query reductions");
    // The AI can-advance-tech query prepares offers against one gathered list; both overloads give the same offer.
    const auto& authored = config.production->planets[0].offers[0];
    for (const auto planet : {gc::PlanetId{1}, gc::PlanetId{2}}) for (const auto who : {gc::PlayerId{1}, gc::PlayerId{2}}) {
        const auto per_offer = take(gc::prepared_production_offer(authored, &inputs, owners, planet, who, nullptr));
        const auto shared = take(gc::prepared_production_offer(authored, &inputs, gathered, planet, who, nullptr));
        expect(per_offer.price == shared.price && per_offer.seconds == shared.seconds, "prepared offer equals with gathered reductions");
    }
    config = setup(); config.income->types[1].gambling.reset();
    auto mine = take(gc::GalacticSession::create(config));
    for (int i = 0; i < 4; ++i) (void)take(mine.step(serial));
    expect(mine.snapshot()->player_economy()->players[0].credits == f(1050), "basic plus 30 mining bonus plus other owned planet income");
    config = setup(); config.planets[0].owner = 2;
    config.income->types[1].gambling->chance = f(1);
    auto foreign = take(gc::GalacticSession::create(config));
    bool sampled = false;
    for (int i = 0; i < 20; ++i) {
        const auto tick = take(foreign.step(serial)); (void)tick;
        for (const auto& event : foreign.snapshot()->events())
            sampled = sampled || (event.kind == gc::GalacticEventKind::gambling_income_sampled
                && event.subject == config.fleets[0].members[0].id);
    }
    expect(sampled, "WGEP-76 gambling service uses current planet without a source-owner gate");
    config = setup();
    auto captured = take(gc::GalacticSession::create(config));
    const auto source_id = config.fleets[0].members[0].id;
    const auto contribution = [&] {
        const auto& rows = captured.snapshot()->income()->gambling;
        return *std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.source == source_id; });
    };
    for (int i = 0; i < 30 && contribution().income.raw() == 0; ++i) (void)take(captured.step(serial));
    expect(contribution().income.raw() != 0, "gambling contribution installed before capture");
    const auto deadline = contribution().deadline;
    take(captured.submit(owner(captured.completed_tick(), {1}, 1, 1, 2)));
    (void)take(captured.step(serial));
    expect(contribution().income.raw() == 0 && contribution().deadline == deadline,
        "WGEP-77 capture removes old contribution without activation roll or deadline reset");
    while (captured.clock().galactic_frame <= deadline) (void)take(captured.step(serial));
    expect(contribution().deadline > deadline, "WGEP-76 next due service still runs on the source current planet");
    auto invalid = setup(); invalid.income->planets.pop_back(); expect(!gc::validate_setup(invalid), "incomplete economic planet table refused");
    invalid = setup(); invalid.income->sources.push_back({invalid.fleets[0].members[0].id, 900, 1, 1, false, true});
    expect(!gc::validate_setup(invalid), "declared nonplanet profile must match its owning lifecycle identity");
    config = setup(); config.income->types[1].gambling->interval_frames = 0;
    auto disabled = take(gc::GalacticSession::create(config));
    for (const auto& row : disabled.snapshot()->income()->gambling)
        expect(!row.scheduled && row.deadline == std::numeric_limits<std::uint32_t>::max(), "missing service entry is unsigned maximum, never due zero");
    config = setup();
    config.income->offers[0].price = d("201.5");
    auto fractional = take(gc::GalacticSession::create(config)); take(fractional.submit(buy(0, 1)));
    (void)take(fractional.step(serial));
    expect(fractional.snapshot()->production()->planets[0].queues[0][0].price == f(101), "discount precedes a single price rounding step");
    config = setup(); config.income->types[0].reductions.clear();
    config.income->types[1].mining.reset(); config.income->types[1].gambling.reset(); config.income->types[1].reductions.clear();
    gc::EconomicType built; built.type = 2000; built.mining = gc::PlanetIncomeAbility{f(2), {}, d("1.5")};
    config.income->types.push_back(built);
    config.income->offers[0].seconds = d("0.1"); config.production->planets[0].offers[0].seconds = d("0.1");
    auto placed = take(gc::GalacticSession::create(config)); take(placed.submit(buy(0, 1)));
    for (int i = 0; i < 4; ++i) (void)take(placed.step(serial));
    expect(placed.snapshot()->player_economy()->players[0].credits == f(950), "GFD-08 completion attaches mining source before same-frame fiscal read");
}
void filters() {
    eawr::units::GalacticTypeTable types;
    for (const auto& name : {"Own", "Foreign", "Explicit", "Zero"}) {
        eawr::units::GalacticType type; type.name = name; type.category_mask = {"Vehicle"};
        type.affiliation = {std::string(name) == "Own" ? "Side" : "Other"};
        type.build_cost_credits = std::string(name) == "Zero" ? Fixed{} : f(100); types.types.push_back(type);
    }
    eawr::units::GalacticEconomicType source; source.object_class = "HeroUnit";
    eawr::units::GalacticReductionAbility ability; ability.price = true; ability.galaxy_wide = false;
    ability.category = 1; ability.reduction = d("0.2"); ability.included_categories = {"Vehicle"};
    ability.excluded_types = {"Explicit"}; ability.included_types = {"Explicit", "Zero"}; source.reductions = {ability};
    const std::vector<eawr::campaign::EconomicTypeBinding> bindings{{"Own", 1}, {"Foreign", 2}, {"Explicit", 3}, {"Zero", 4}};
    auto profile = take(eawr::campaign::prepare_economic_type(source, 900, "Side", types, bindings));
    expect(profile.reductions.size() == 2 && profile.reductions[0].type == 1 && profile.reductions[1].type == 3,
        "WGEP-73 category affiliation and explicit append bypass; zero-price guard");
    auto all = source; all.reductions[0].included_categories = {"All"};
    types.types[0].category_mask = {"Corvette"};
    expect(take(eawr::campaign::prepare_economic_type(all, 900, "Side", types, bindings)).reductions == profile.reductions,
        "authored All includes category-specific owned types while retaining affiliation and explicit-type gates");
    all.reductions[0].excluded_categories = {"aLl"};
    const auto excluded_all = take(eawr::campaign::prepare_economic_type(all, 900, "Side", types, bindings));
    expect(excluded_all.reductions.size() == 1 && excluded_all.reductions[0].type == 3,
        "excluded All rejects category matches but explicit types still append separately");
    types.types[0].category_mask.clear(); all.reductions[0].excluded_categories.clear();
    expect(take(eawr::campaign::prepare_economic_type(all, 900, "Side", types, bindings)).reductions.size() == 1,
        "full category mask does not intersect an empty type mask");
    types.types[0].category_mask = {"All"};
    expect(take(eawr::campaign::prepare_economic_type(source, 900, "Side", types, bindings)).reductions == profile.reductions,
        "an authored full type mask intersects a specific included category");
    types.types[0].category_mask = {"Vehicle"};
    source.object_class = "Planet";
    expect(take(eawr::campaign::prepare_economic_type(source, 900, "Side", types, bindings)).reductions.size() == 3,
        "planet category selection bypasses affiliation");
    auto greater = source.reductions[0]; greater.reduction = d("0.4");
    auto global = greater; global.galaxy_wide = true;
    source.reductions.push_back(greater); source.reductions.push_back(global);
    const auto combined = take(eawr::campaign::prepare_economic_type(source, 900, "Side", types, bindings));
    expect(combined.reductions.size() == 6 && combined.reductions[0].reduction == d("0.4")
        && !combined.reductions[0].galaxy_wide && combined.reductions[1].galaxy_wide,
        "same-category duplicates retain maximum and distinct local/global footprints");
    auto admitted = setup(); admitted.income->types[0] = combined; take(gc::validate_income(admitted));
    source.reductions.resize(1);
    source.reductions[0].price = false;
    expect(take(eawr::campaign::prepare_economic_type(source, 900, "Side", types, bindings)).reductions.size() == 4,
        "local time selection has no positive-price guard");
    source.reductions[0].initially_enabled = false;
    expect(take(eawr::campaign::prepare_economic_type(source, 900, "Side", types, bindings)).reductions.empty(),
        "WGEP-93 disabled reduction does not apply at fresh startup");
    source.reductions[0].initially_enabled = true; source.reductions[0].activation_style = "User_Input";
    expect(take(eawr::campaign::prepare_economic_type(source, 900, "Side", types, bindings)).reductions.empty(),
        "manual reduction waits for its action owner");
    source.reductions[0].activation_style = "unknown";
    expect(!eawr::campaign::prepare_economic_type(source, 900, "Side", types, bindings), "unknown reduction activation refuses");
    source.reductions[0].activation_style = "Galactic_Automatic"; source.reductions[0].initially_enabled.reset();
    expect(!eawr::campaign::prepare_economic_type(source, 900, "Side", types, bindings), "malformed enabled flag refuses");
}
void replay_checkpoint() {
    auto config = setup();
    const auto replay = gc::GalacticReplay{config, 30, {buy(0, 1), orbit(4, {1}, 2, config.fleets[0].id, 3)}};
    const auto bytes = take(gc::write_replay(replay));
    expect(take(gc::parse_replay(bytes)) == replay, "income replay extension round trip");
    for (std::size_t n = 0; n < bytes.size(); n += 53) expect(!gc::parse_replay(std::span(bytes).first(n)), "truncated income tables refused");
    std::vector<std::string> hashes;
    for (std::size_t workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter pool(workers, eawr::platform::ThreadWorkerAdapter::Dispatch::always_pool);
        auto game = take(eawr::campaign::GameSession::create(config));
        for (const auto& command : replay.commands) take(game.submit(command));
        for (std::size_t i = 0; i < 30; ++i) {
            auto result = take(game.step(pool));
            if (workers == 1) hashes.push_back(result.state_hash.get());
            else expect(result.state_hash.get() == hashes[i], "income source phase 1/2/4/8 equality");
            if (i == 2) {
                const auto identities = eawr::checkpoint::game_identities(config);
                const auto saved = take(eawr::checkpoint::save_game_checkpoint(game, identities));
                auto restored = take(eawr::checkpoint::load_game_checkpoint(saved, identities, {}, pool));
                expect(take(eawr::checkpoint::save_game_checkpoint(restored.session, identities)) == saved, "pending gambling deadline checkpoint round trip");
                game = std::move(restored.session);
            }
        }
    }
}
// Planet reductions must reach the same published card and accepted queue values.
// Use arbitrary planet/type identities so this contract also covers mod authoring.
std::string planet_reduction_card(const gc::EconomicReduction& reduction, std::size_t workers) {
    auto config = setup();
    config.income->types.resize(1);
    const auto& row = reduction;
    // Preserve the profile's target binding through publication and admission.
    for (auto& planet : config.production->planets)
        for (auto& offer : planet.offers) offer.type = row.type;
    for (auto& offer : config.income->offers) offer.type = row.type;
    config.income->types[0].reductions = {row};
    config.income->sources[0].owner = config.planets[0].owner;
    config.observer.emplace(); auto& observer = *config.observer;
    observer.local_player = 1;
    observer.players = {{1, false, false, 0, 1, {}}, {2, true, false, 0, 1, {}}};
    observer.types = {{1000, false, false, false}};
    for (const auto& planet : config.planets) {
        gc::ObserverPlanet facts; facts.planet = planet.id;
        facts.allegiance = facts.target_owner = facts.current_owner = facts.current_controller = planet.owner;
        observer.planets.push_back(facts);
        for (const auto& player : config.players)
            observer.records.push_back({player.player_id, planet.id, true, 0, 0, 0, {}});
    }
    std::sort(observer.records.begin(), observer.records.end(), [](const auto& a, const auto& b) {
        return std::pair{a.player, a.planet} < std::pair{b.player, b.planet};
    });
    const std::vector<gc::ProductionReduction> reductions{{1, row.category, row.reduction}};
    const auto factor = take(gc::production_reduction_multiplier(reductions, {}, row.price));
    const auto raw_price = row.price ? take(eawr::sim::math::multiply(f(100), factor)) : f(100);
    const auto price = raw_price > Fixed{} && raw_price < f(1) ? f(1)
        : f(take(eawr::sim::math::add(raw_price, d("0.5"))).trunc_to_integer());
    const auto seconds = row.price ? f(10) : take(eawr::sim::math::multiply(f(10), factor));
    eawr::platform::ThreadWorkerAdapter pool(workers);
    auto game = take(eawr::campaign::GameSession::create(config));
    const auto cards = game.galaxy().snapshot()->production_offers();
    const auto card = std::find_if(cards.begin(), cards.end(), [&](const auto& offer) {
        return offer.planet == 1 && offer.type == row.type;
    });
    expect(card != cards.end() && card->price == price && card->seconds == seconds,
        "planet reduction is visible on the live production card");
    // A global bonus must also reach another same-owner producer, while a local
    // bonus must leave its base price/time alone.
    const auto other = std::find_if(cards.begin(), cards.end(), [&](const auto& offer) {
        return offer.type == row.type && offer.planet != 1 && config.planets[offer.planet - 1].owner == 1;
    });
    expect(other != cards.end() && other->price == (row.galaxy_wide ? price : f(100))
        && other->seconds == (row.galaxy_wide ? seconds : f(10)), "planet card local/global footprint");
    auto purchase = buy(0, 1);
    std::get<gc::ProductionCommand>(purchase.payload).type = row.type;
    take(game.submit(purchase)); (void)take(game.step(pool));
    const auto& queue = game.galaxy().snapshot()->production()->planets[0].queues[0];
    expect(!queue.empty() && queue[0].type == row.type && queue[0].price == price && queue[0].full_seconds == seconds,
        "accepted queue uses exactly the published planet reduction price/time");
    return game.state_sha256();
}
void planet_reduction_cards() {
    for (const auto& row : std::vector<gc::EconomicReduction>{{2000, 9, d("0.2"), true, true},
            {2000, 9, d("0.99999"), true, false}, {2000, 9, d("-0.15"), false, false},
            {2000, 9, d("0.25"), false, true}}) {
        std::string reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const auto hash = planet_reduction_card(row, workers);
            if (reference.empty()) reference = hash;
            else expect(hash == reference, "planet reduction cards and purchase state equal on 1/2/4/8 workers");
        }
    }
}
gc::EconomicType technology_profile(std::string_view percentage) {
    eawr::units::GalacticEconomicType source; source.name = "Renamed_Research_Planet"; source.object_class = "Planet";
    eawr::units::GalacticTechnologyPriceAbility ability; ability.name = "Authored_Discount"; ability.reduction = d(percentage);
    source.technology_prices = {ability};
    return take(eawr::campaign::prepare_economic_type(source, 900, {}, {}, {}));
}
gc::GalacticSetup technology_setup(const gc::EconomicType& profile, gc::ProductionOffer research = {}) {
    auto config = setup(); config.rules.fiscal_cycle_frames = 100000;
    config.fiscal->credit_cap_per_planet = f(10000000);
    config.fleet_control->types.push_back({2000, false, true, false});
    config.income->types = {profile}; config.income->sources = {{1, profile.type, 1, 1, true, true}};
    if (!research.type) {
        research.type = 3000; research.price = f(100); research.seconds = f(10);
        research.kind = gc::ProductionKind::technology; research.tech = 2; research.target_tech = 3;
        research.domain = gc::ProductionDomain::ground; research.affiliated = research.direct_build_allowed = true;
    }
    for (auto& account : config.player_economy->players) {
        account.credits = f(1000000); account.credit_cap = f(10000000); account.current_tech = research.tech;
    }
    auto& economy = config.economy_corruption.emplace(); economy.factor_min = economy.factor_max = f(1);
    for (const auto& player : config.players) {
        gc::CorruptionEconomyPlayer row; row.player = player.player_id; row.faction = row.player * 10;
        row.human = row.playable = true; economy.players.push_back(row);
    }
    config.observer.emplace(); auto& observer = *config.observer; observer.local_player = 1;
    observer.players = {{1, false, false, 0, 1, {}}, {2, true, false, 0, 1, {}}};
    observer.types = {{1000, false, false, false}};
    for (auto& location : config.production->planets) {
        location.ground_level = std::max<std::int64_t>(1, research.required_ground_level);
        location.space_level = research.required_space_level;
        location.maximum_space_level = std::max(location.maximum_space_level, location.space_level);
        location.offers[0].kind = gc::ProductionKind::unit;
        location.offers[0].domain = gc::ProductionDomain::space;
        location.offers.push_back(research);
        config.income->offers.push_back({location.planet, research.type, research.price, research.seconds});
        economy.planets.push_back({location.planet});
        gc::ObserverPlanet facts; facts.planet = location.planet;
        facts.allegiance = facts.target_owner = facts.current_owner = facts.current_controller = location.target_owner;
        observer.planets.push_back(facts);
    }
    for (const auto& player : config.players) for (const auto& planet : config.planets)
        observer.records.push_back({player.player_id, planet.id, true, 0, 0, 0, {}});
    std::sort(config.income->offers.begin(), config.income->offers.end(), [](const auto& a, const auto& b) {
        return std::pair{a.planet, a.type} < std::pair{b.planet, b.type};
    });
    return config;
}
std::string technology_price_card(const gc::EconomicType& profile, std::size_t workers, gc::ProductionOffer research = {}) {
    auto config = technology_setup(profile, research);
    const auto& original = config.production->planets[0].offers[1];
    const auto factor = take(eawr::sim::math::subtract(f(1), *profile.technology_price_reduction));
    const auto raw = take(eawr::sim::math::multiply(original.price, factor));
    const auto expected = raw > Fixed{} && raw < f(1) ? f(1) : f(take(eawr::sim::math::add(raw, d("0.5"))).trunc_to_integer());
    eawr::platform::ThreadWorkerAdapter pool(workers);
    auto game = take(eawr::campaign::GameSession::create(config));
    const auto cards = game.galaxy().snapshot()->production_offers();
    const auto card = std::find_if(cards.begin(), cards.end(), [&](const auto& row) { return row.planet == 1 && row.type == original.type; });
    expect(card != cards.end() && card->price == expected, "authored planet source changes a real published research offer");
    expect(card != cards.end() && card->admission == gc::ProductionAdmission::allowed,
        "authored research offer is eligible at its exact current technology level");
    const auto ordinary = std::find_if(cards.begin(), cards.end(), [](const auto& row) { return row.planet == 1 && row.type == 2000; });
    expect(ordinary != cards.end() && ordinary->price == f(100), "technology source leaves ordinary production price unchanged");
    auto purchase = buy(0, 1); std::get<gc::ProductionCommand>(purchase.payload).type = original.type;
    take(game.submit(purchase)); (void)take(game.step(pool));
    const auto& queue = game.galaxy().snapshot()->production()->planets[0].queues[0];
    expect(!queue.empty() && queue[0].type == original.type && queue[0].price == expected
        && game.galaxy().snapshot()->player_economy()->players[0].credits == take(eawr::sim::math::subtract(f(1000000), expected)),
        "actual paid research price and retained queue equal the published offer");
    return game.state_sha256();
}
void technology_price_planets() {
    const auto profile = technology_profile("0.37");
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        std::vector<std::string> hashes{technology_price_card(profile, workers)};
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto config = technology_setup(profile);
        auto game = take(eawr::campaign::GameSession::create(config));
        const auto factor = take(eawr::sim::math::subtract(f(1), d("0.37")));
        expect(game.galaxy().economy_corruption()->players[0].tech_purchase_multiplier == factor, "initial owner registers authored source automatically");
        take(game.submit({{0, {1}, 1}, gc::DebugSetPlanetOwner{1, 2}})); (void)take(game.step(pool));
        const auto& captured = game.galaxy().economy_corruption()->players;
        expect(captured[0].technology_price_modifiers.empty() && captured[0].tech_purchase_multiplier == f(1)
            && captured[1].technology_price_modifiers.size() == 1 && captured[1].tech_purchase_multiplier == factor,
            "capture removes old recipient and installs source on the current owner");
        const auto identities = eawr::checkpoint::game_identities(config);
        const auto saved = take(eawr::checkpoint::save_game_checkpoint(game, identities));
        auto loaded = take(eawr::checkpoint::load_game_checkpoint(saved, identities, {}, pool));
        expect(take(eawr::checkpoint::save_game_checkpoint(loaded.session, identities)) == saved, "authored profile and capture reconstruct checkpoint exactly");
        for (auto* session : {&game, &loaded.session}) {
            take(session->submit({{1, {1}, 2}, gc::DebugSetPlanetOwner{1, 2}}));
            take(session->submit({{2, {1}, 3}, gc::DebugSetPlanetOwner{1, 1}}));
        }
        for (int tick = 1; tick < 4; ++tick) {
            hashes.push_back(take(game.step(pool)).state_hash.get());
            expect(take(loaded.session.step(pool)).state_hash.get() == hashes.back(), "checkpoint continuation and source replacement agree");
        }
        expect(game.galaxy().economy_corruption()->players[0].technology_price_modifiers.size() == 1
            && game.galaxy().economy_corruption()->players[0].tech_purchase_multiplier == factor,
            "duplicate refresh and recapture never compound the same planet source");
        auto market_config = technology_setup(profile);
        market_config.economy_corruption->players[0].benefits = true;
        market_config.economy_corruption->planets[1].corruption = gc::corruption_fixture::Type::black_market;
        market_config.economy_corruption->items = {{50, 20, 1, 1, f(100), f(40), f(1)}};
        gc::BlackMarketBuyer buyer; buyer.buyer = 10000; buyer.owner = 1;
        buyer.opposing_planet = buyer.causes_despawn = true;
        buyer.absence_min = buyer.absence_max = {f(1), f(1), f(1), f(1), f(1)};
        market_config.economy_corruption->buyers = {buyer};
        auto market = take(eawr::campaign::GameSession::create(market_config));
        take(market.submit({{0, {1}, 1}, gc::PrepareBlackMarket{10000, 2}})); (void)take(market.step(pool));
        const auto market_price = market.galaxy().economy_corruption()->menus.front().offers.front().price;
        expect(market_price == take(eawr::sim::math::multiply(f(100), factor)), "ordinary black market consumes the authored planet's same technology ledger");
        take(market.submit({{1, {1}, 2}, gc::DebugSetPlanetOwner{1, 2}}));
        take(market.submit({{1, {1}, 3}, gc::BuyBlackMarket{10000, 0}})); (void)take(market.step(pool));
        expect(market.galaxy().snapshot()->player_economy()->players[0].credits == take(eawr::sim::math::subtract(f(1000000), market_price)),
            "prepared market purchase retains its exact copied price after bonus recipient changes");
        hashes.push_back(market.state_sha256());
        market_config.economy_corruption->tutorial = true;
        auto tutorial = take(gc::GalacticSession::create(market_config));
        take(tutorial.submit({{0, {1}, 1}, gc::PrepareBlackMarket{10000, 2}})); (void)take(tutorial.step(pool));
        expect(tutorial.economy_corruption()->menus.front().offers.front().price == f(40), "tutorial market route retains its sourced technology-factor bypass");
        auto stacked = technology_setup(profile); stacked.income->sources.push_back({3, profile.type, 3, 1, true, true});
        auto two = take(gc::GalacticSession::create(stacked));
        expect(two.economy_corruption()->players[0].tech_purchase_multiplier == take(eawr::sim::math::subtract(f(1), take(eawr::sim::math::add(d("0.37"), d("0.37"))))),
            "different planet object identities add independently");
        auto intimidated = technology_setup(profile); intimidated.economy_corruption->planets[0].corruption = gc::corruption_fixture::Type::intimidation;
        auto suppressed = take(gc::GalacticSession::create(intimidated));
        expect(suppressed.economy_corruption()->players[0].tech_purchase_multiplier == f(1), "initial intimidation suppresses ordinary registration");
        take(suppressed.submit({{0, {1}, 1}, gc::DebugSetPlanetOwner{1, 2}})); (void)take(suppressed.step(pool));
        expect(suppressed.economy_corruption()->players[1].tech_purchase_multiplier == factor, "sourced ownership callback directly registers even under intimidation");
        auto absent = technology_setup(profile); absent.income->sources[0].enabled = false;
        expect(take(gc::GalacticSession::create(absent)).economy_corruption()->players[0].tech_purchase_multiplier == f(1), "disabled source contributes nothing");
        absent = technology_setup(profile); absent.economy_corruption->planets[0].destroyed = true;
        expect(take(gc::GalacticSession::create(absent)).economy_corruption()->players[0].tech_purchase_multiplier == f(1), "destroyed planet never registers");
        absent = technology_setup(profile); absent.income->types[0].type = config.fleets[0].members[0].type;
        absent.income->sources[0] = {config.fleets[0].members[0].id, config.fleets[0].members[0].type, 1, 1, false, true};
        const auto refused = gc::GalacticSession::create(absent);
        expect(!refused && refused.error().code == gc::diagnostic_codes::invalid_setup
            && refused.error().message == "invalid galactic economic profiles or source bindings",
            "the economic binding validator refuses a traveling planetary technology source");
        absent.income->types[0].technology_price_reduction.reset();
        expect(static_cast<bool>(gc::GalacticSession::create(absent)),
            "the same bound traveling source is valid when it has no planetary technology ability");
        hashes.push_back(suppressed.state_sha256()); hashes.push_back(two.state_sha256());
        if (baseline.empty()) baseline = hashes; else expect(hashes == baseline, "technology planet offers, lifecycle and replay hashes equal on 1/2/4/8 workers");
    }
    eawr::units::GalacticEconomicType authored; authored.object_class = "Planet";
    eawr::units::GalacticTechnologyPriceAbility ability; ability.reduction = f(2); ability.activation_style = "User_Input";
    authored.technology_prices = {ability};
    expect(take(eawr::campaign::prepare_economic_type(authored, 900, {}, {}, {})).technology_price_reduction == f(1), "specific validation forces automatic activation and clamps high authored percentage");
    authored.technology_prices[0].initially_enabled = false;
    expect(!take(eawr::campaign::prepare_economic_type(authored, 900, {}, {}, {})).technology_price_reduction, "disabled authored ability has no live profile");
    authored.technology_prices[0].initially_enabled = true; authored.technology_prices[0].reduction.reset();
    expect(!eawr::campaign::prepare_economic_type(authored, 900, {}, {}, {}), "malformed percentage refuses setup");
    (void)technology_price_card(technology_profile("-0.25"), 1);
}
void technology_price_callbacks() {
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto config = ability_fixture::ruins_setup(false);
        config.economy_corruption->planets[0].corruption = gc::corruption_fixture::Type::none;
        auto profile = technology_profile("0.37"); profile.type = 3000;
        config.income.emplace(); config.income->types = {profile};
        config.income->sources = {{config.planets[0].id, profile.type, config.planets[0].id, 1, true, true}};
        for (const auto& planet : config.planets) config.income->planets.push_back({planet.id});
        const auto factor = take(eawr::sim::math::subtract(f(1), d("0.37")));
        auto world = take(gc::GalacticSession::create(config));
        expect(world.economy_corruption()->players[0].tech_purchase_multiplier == factor, "initial source registers alongside live control and corruption");
        expect(take(world.begin_corruption(config.planets[0].id, gc::corruption_fixture::Type::intimidation, Fixed{})), "intimidation transition admitted");
        std::vector<std::string> hashes;
        for (int i = 0; i < 3; ++i) hashes.push_back(take(world.step(pool)).state_hash.get());
        expect(world.economy_corruption()->players[0].technology_price_modifiers.empty(), "live intimidation callback removes the source");
        expect(take(world.begin_corruption(config.planets[0].id, gc::corruption_fixture::Type::none, Fixed{})), "intimidation removal admitted");
        const auto removal_end = world.ability_state()->transitions[0].transition.end;
        while (world.clock().galactic_frame <= removal_end)
            hashes.push_back(take(world.step(pool)).state_hash.get());
        expect(world.economy_corruption()->players[0].tech_purchase_multiplier == factor, "live corruption removal callback restores the source once");
        for (const bool destroy : {false, true}) {
            auto game = take(eawr::campaign::GameSession::create(config));
            const auto original = game.galaxy().snapshot();
            expect(take(game.request_planet_ruins(config.planets[0].id, destroy)), "ruins source cleanup admitted");
            expect(game.galaxy().economy_corruption()->players[0].technology_price_modifiers.empty()
                && game.galaxy().economy_corruption()->players[0].tech_purchase_multiplier == f(1)
                && original->economy_corruption()->players[0].tech_purchase_multiplier == factor,
                "full destruction and no-flag ruins remove technology sources without mutating older snapshots");
            const auto identities = eawr::checkpoint::game_identities(config);
            const auto saved = take(eawr::checkpoint::save_game_checkpoint(game, identities));
            auto loaded = take(eawr::checkpoint::load_game_checkpoint(saved, identities, {}, pool));
            expect(take(eawr::checkpoint::save_game_checkpoint(loaded.session, identities)) == saved, "native ruins journal restores removed planet technology source");
            hashes.push_back(take(game.step(pool)).state_hash.get());
            expect(take(loaded.session.step(pool)).state_hash.get() == hashes.back(), "ruins continuation keeps source removed");
        }
        if (baseline.empty()) baseline = hashes; else expect(hashes == baseline, "technology corruption and ruins callbacks equal on 1/2/4/8 workers");
    }
}
void installed_data() {
    std::string root = eawr::testing::game_root().value_or("");
    if (root.empty()) return;
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(root) / folder / "Data")).mount);
    auto filesystem = take(eawr::vfs::Vfs::mount(mounts));
    const auto catalog = take(eawr::data::load_catalog(filesystem, eawr::data::Profile::foc));
    const auto economy = eawr::units::galactic_economic_table(catalog.catalog);
    const auto mine = std::find_if(economy.types.begin(), economy.types.end(), [](const auto& row) { return row.name == "Empire_Ground_Mining_Facility"; });
    expect(mine != economy.types.end() && mine->mining_percentage == f(2) && mine->mining_multiplier == d("1.5"), "stock mining authored values retained");
    const auto arena = std::find_if(economy.types.begin(), economy.types.end(), [](const auto& row) { return row.gambling; });
    expect(arena != economy.types.end() && arena->gambling_chance == d("0.8") && arena->gambling_interval == f(30), "stock gambling authored values retained");
    const auto hypori = std::find_if(economy.types.begin(), economy.types.end(), [](const auto& row) { return row.name == "Hypori"; });
    expect(hypori != economy.types.end() && !hypori->reductions.empty() && hypori->reductions[0].galaxy_wide == true, "planet global reductions loaded separately from tactical types");
    expect(hypori != economy.types.end() && hypori->reductions[0].initially_enabled == true
        && hypori->reductions[0].activation_style == "Galactic_Automatic", "WGEP-93 stock omitted activation uses specific constructor defaults");
    const auto types = eawr::units::galactic_type_table(catalog.catalog);
    std::vector<eawr::campaign::EconomicTypeBinding> bindings;
    for (const auto& type : types.types) bindings.push_back({type.name, static_cast<gc::TypeId>(bindings.size() + 1)});
    const auto maw = std::find_if(economy.types.begin(), economy.types.end(), [](const auto& row) { return row.name == "The_Maw"; });
    expect(maw != economy.types.end() && maw->technology_prices.size() == 1
        && maw->technology_prices[0].reduction == d("0.15"), "installed The_Maw retains authored technology percentage");
    if (maw != economy.types.end()) {
        const auto profile = take(eawr::campaign::prepare_economic_type(*maw, 900, {}, types, bindings));
        const auto* research = types.find("DS_Primary_Hyperdrive");
        expect(research && research->build_cost_credits && research->build_time_seconds && research->build_advances_tech_level,
            "installed research binds real authored cost, duration and technology advancement");
        if (research && research->build_advances_tech_level) {
            gc::ProductionOffer admission; admission.type = 3000; admission.kind = gc::ProductionKind::technology;
            admission.affiliated = admission.direct_build_allowed = true; admission.target_tech = *research->build_advances_tech_level;
            eawr::campaign::ProductionPreparation quantities;
            quantities.price_multiplier = quantities.time_multiplier = quantities.tech_purchase_multiplier = f(1);
            quantities.corruption_price_multiplier = quantities.corruption_time_multiplier = f(1);
            quantities.base_tech_multiplier = quantities.ai_time_multiplier = f(1); quantities.factory_count = 1;
            const auto offer = take(eawr::campaign::prepare_production_offer(*research, admission, quantities));
            (void)technology_price_card(profile, 1, offer);
            std::cout << "The_Maw installed authored research card/debit ran: DS_Primary_Hyperdrive base="
                << offer.price.trunc_to_integer() << " reduction=0.15\n";
        }
    }
    std::size_t prices = 0, times = 0;
    bool kuat_card = false, category_price_card = false;
    for (const auto& planet : economy.types) {
        if (planet.object_class != "Planet") continue;
        for (const auto& ability : planet.reductions) {
            auto single = planet; single.reductions = {ability};
            const auto profile = take(eawr::campaign::prepare_economic_type(single, 900, {}, types, bindings));
            expect(!profile.reductions.empty(), "stock planet reduction resolves at least one target: " + planet.name);
            ability.price ? ++prices : ++times;
            for (const auto& name : ability.included_types) {
                const auto binding = std::find_if(bindings.begin(), bindings.end(), [&](const auto& value) { return value.name == name; });
                expect(binding != bindings.end() && std::any_of(profile.reductions.begin(), profile.reductions.end(),
                    [&](const auto& row) { return row.type == binding->type; }), "every stock explicit reduction target resolves: " + planet.name + "/" + name);
            }
            for (const auto& name : ability.excluded_types) {
                const auto binding = std::find_if(bindings.begin(), bindings.end(), [&](const auto& value) { return value.name == name; });
                expect(binding != bindings.end() && std::none_of(profile.reductions.begin(), profile.reductions.end(),
                    [&](const auto& row) { return row.type == binding->type; }), "stock category exclusions do not get a reduction: " + planet.name + "/" + name);
            }
            if (!ability.included_categories.empty()) for (const auto& target : types.types) {
                const bool matches = std::any_of(ability.included_categories.begin(), ability.included_categories.end(), [&](const auto& category) {
                    return std::find(target.category_mask.begin(), target.category_mask.end(), category) != target.category_mask.end();
                });
                if (!matches || std::find(ability.excluded_types.begin(), ability.excluded_types.end(), target.name) != ability.excluded_types.end()
                    || (ability.price && target.build_cost_credits.value_or(Fixed{}) <= Fixed{})) continue;
                const auto binding = std::find_if(bindings.begin(), bindings.end(), [&](const auto& value) { return value.name == target.name; });
                expect(binding != bindings.end() && std::any_of(profile.reductions.begin(), profile.reductions.end(),
                    [&](const auto& row) { return row.type == binding->type; }), "no stock category-selected target is omitted: " + planet.name + "/" + target.name);
            }
            for (const auto& row : profile.reductions) {
                expect(row.price == ability.price && row.reduction == ability.reduction && row.category == ability.category
                    && row.galaxy_wide == ability.galaxy_wide, "stock reduction preserves authored application values: " + planet.name);
                if (!ability.included_categories.empty()) {
                    const auto& target = types.types[static_cast<std::size_t>(row.type - 1)];
                    expect(std::any_of(ability.included_categories.begin(), ability.included_categories.end(), [&](const auto& category) {
                        return std::find(target.category_mask.begin(), target.category_mask.end(), category) != target.category_mask.end();
                    }), "every stock category-selected target has the authored category: " + planet.name + "/" + target.name);
                }
                (void)planet_reduction_card(row, 1);
                const auto target = std::find_if(bindings.begin(), bindings.end(), [&](const auto& binding) {
                    return binding.type == row.type;
                });
                expect(target != bindings.end(), "live card retains a resolved stock target binding");
                if (planet.name == "Kuat" && target->name == "Star_Destroyer") {
                    expect(row.price && row.galaxy_wide && row.reduction == d("0.2"),
                        "Kuat publishes and queues its stock Star_Destroyer target at 80 credits from a 100-credit base");
                    kuat_card = true;
                }
                if (row.price && !ability.included_categories.empty()) category_price_card = true;
            }
        }
    }
    expect(prices == 21 && times == 4, "all 25 stock planet price/time ability records exercised");
    expect(kuat_card && category_price_card, "stock explicit and category price targets reach live cards and queues");
    std::cout << "income installed_data ran: mining, gambling and planet-global reduction contracts\n";
}
void publication_cache() {
    auto config = setup();
    config.income->types[1].gambling.reset();
    config.observer.emplace();
    auto& observer = *config.observer;
    observer.local_player = 1;
    // This source-publication fixture needs breakdown knowledge even after capture.
    // Discovery alone does not admit an income breakdown (WGUI-13).
    const auto intelligence = gc::bit(gc::Intelligence::income_breakdown);
    for (const auto& player : config.players) observer.players.push_back({player.player_id, false, false, intelligence});
    for (const auto& planet : config.planets) {
        gc::ObserverPlanet row; row.planet = planet.id;
        row.allegiance = row.target_owner = row.current_owner = row.current_controller = planet.owner;
        observer.planets.push_back(row);
    }
    observer.types.push_back({1000});
    for (const auto& player : config.players) for (const auto& planet : config.planets) {
        gc::ObserverRecord record; record.player = player.player_id; record.planet = planet.id;
        record.discovered = true; record.mask = intelligence;
        observer.records.push_back(record);
    }
    std::vector<gc::EconomyForecast> reference;
    std::vector<std::string> reference_hashes;
    std::optional<gc::EconomyForecast> completed_reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(gc::GalacticSession::create(config));
        std::size_t evidence_index = 0;
        const auto facts = [&]() -> const gc::EconomyForecast& { return *game.snapshot()->economy_forecast(); };
        const auto modifiers = [&](const gc::PlanetId id) {
            const auto& rows = facts().planets;
            return std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.planet == id; })->modifiers;
        };
        const auto record = [&]() {
            const auto hash = game.state_sha256();
            if (workers == 1) { reference.push_back(facts()); reference_hashes.push_back(hash); }
            else {
                expect(facts() == reference[evidence_index], "income publication exact page facts on 1/2/4/8");
                expect(hash == reference_hashes[evidence_index], "income publication state hash on 1/2/4/8");
            }
            ++evidence_index;
        };
        expect(modifiers(1) == std::optional(std::vector<gc::PlanetIncomeForecast::Modifier>{{1000, f(30)}}),
            "mining colony publication retains source amount before rounding");
        record();
        const auto before = facts(); const auto state = game.state_sha256();
        for (int i = 0; i < 3; ++i) take(game.apply_session_controls({}));
        expect(facts() == before && game.state_sha256() == state, "unchanged barriers reuse exact facts without state mutation");
        auto step = take(game.step(pool));
        expect(step.work.income_sources_gathered == 0 && step.work.income_modifier_rebuilds == 0,
            "unchanged income publication performs no source gather or modifier rebuild");
        take(game.submit(owner(game.completed_tick(), {1}, 1, 1, 2)));
        step = take(game.step(pool));
        expect(step.work.income_sources_gathered > 0 && step.work.income_modifier_rebuilds == 1,
            "ownership commit invalidates the income cache once");
        expect(modifiers(1) && modifiers(1)->empty(), "captured planet removes foreign mining source but stays known empty");
        record();
        take(game.submit(orbit(game.completed_tick(), {1}, 2, config.fleets[0].id, 3)));
        step = take(game.step(pool));
        expect(modifiers(3) == std::optional(std::vector<gc::PlanetIncomeForecast::Modifier>{{1000, f(20)}}),
            "moving a mining source updates its destination breakdown");
        record();
        const auto old_snapshot = game.snapshot();
        const std::vector<eawr::sim::EntityId> dead{config.fleets[0].members[0].id};
        take(game.apply_battle_losses(dead));
        expect(modifiers(3) && modifiers(3)->empty(), "battle-return deletion invalidates before publication");
        expect(old_snapshot->economy_forecast()->planets[2].modifiers
            && old_snapshot->economy_forecast()->planets[2].modifiers->size() == 1,
            "retained old snapshot owns its original modifier values");
        record();
        step = take(game.step(pool));
        expect(step.work.income_sources_gathered == 0 && step.work.income_modifier_rebuilds == 0,
            "post-return unchanged frame reuses the rebuilt cache");
        auto built_config = config;
        gc::EconomicType structure; structure.type = 2000;
        structure.mining = gc::PlanetIncomeAbility{{}, f(5), f(1)};
        built_config.income->types.push_back(structure);
        for (auto& planet : built_config.production->planets) planet.offers[0].seconds = d("0.1");
        for (auto& offer : built_config.income->offers) offer.seconds = d("0.1");
        auto built = take(gc::GalacticSession::create(built_config));
        bool notified = false;
        built.set_native_production_outcome_handler([&](const gc::GalacticSession::NativeProductionOutcome& result) {
            if (!result.completed) return;
            notified = true;
            // GFD-A05/07: the callback follows the world commit. A publication here
            // must already see the created source, before the ordinary final snapshot.
            (void)take(built.add_credits(1, {}, f(1)));
            const auto& modifiers_at_commit = built.snapshot()->economy_forecast()->planets[0].modifiers;
            expect(modifiers_at_commit && modifiers_at_commit->size() == 2
                && modifiers_at_commit->back() == gc::PlanetIncomeForecast::Modifier{2000, f(5)},
                "completion callback publication sees the newly committed economic source");
        });
        take(built.submit(buy(0, 1)));
        bool completed = false;
        for (int tick = 0; tick < 8 && !completed; ++tick) {
            const auto produced = take(built.step(pool));
            completed = !built.snapshot()->production()->planets[0].objects.empty();
            if (completed) expect(produced.work.income_sources_gathered > 0
                && produced.work.income_modifier_rebuilds == 1, "production completion invalidates before publication");
        }
        expect(completed && notified, "economic structure completes and notifies in bounded synthetic fixture");
        expect(built.snapshot()->economy_forecast()->planets[0].modifiers
            == std::optional(std::vector<gc::PlanetIncomeForecast::Modifier>{{1000, f(30)}, {2000, f(5)}}),
            "new economic structure is added to the retained source breakdown");
        if (!completed_reference) completed_reference = *built.snapshot()->economy_forecast();
        else expect(*built.snapshot()->economy_forecast() == *completed_reference,
            "production page facts are exact on 1/2/4/8");
        const auto unchanged = take(built.step(pool));
        expect(unchanged.work.income_sources_gathered == 0 && unchanged.work.income_modifier_rebuilds == 0,
            "completed structure does not cause repeated source gathers");
    }
}
void political_income_retention() {
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto config = setup(); config.income->types.clear(); config.income->sources.clear();
        config.fiscal->players[1].earns_planet_income = false;
        config.control.emplace(); auto& control = *config.control;
        control.change_seconds = f(7); control.max_ground_forces = 10;
        control.players = {{1, false, true, 1}, {2, true, false, 2}};
        control.relations = {{1, 1, true}, {1, 2, false}, {2, 1, false}, {2, 2, true}};
        control.types = {{1000, 0, false, false}};
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
        for (const auto& route : config.routes) {
            control.routes.push_back({route.id, {}}); rules.routes.push_back({route.id, {}});
        }
        auto game = take(eawr::campaign::GameSession::create(config));
        std::vector<std::string> hashes;
        for (int tick = 0; tick < 2; ++tick) hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(game.galaxy().snapshot()->control()->state[0].political_income == f(-7),
            "pure publication leaves the stored political source unchanged before collection");
        const auto identities = eawr::checkpoint::game_identities(config);
        auto restored = take(eawr::checkpoint::load_game_checkpoint(
            take(eawr::checkpoint::save_game_checkpoint(game, identities)), identities, {}, pool));
        bool collected = false;
        for (int tick = 0; tick < 4; ++tick) {
            const auto value = take(game.step(pool)); hashes.push_back(value.state_hash.get());
            expect(take(restored.session.step(pool)).state_hash.get() == value.state_hash.get(),
                "political source checkpoint continues equally across collection");
            collected |= value.galaxy_work.fiscal_cycles != 0;
        }
        expect(collected && game.galaxy().snapshot()->control()->state[0].political_income == f(-5),
            "WGEP-08 positive curve replaces the retained source using unmultiplied basic income");
        expect(game.galaxy().snapshot()->control()->state[1].political_income == f(-7),
            "WGEP-75 neutral owner retains its prior source");
        auto zero = config; zero.fiscal->contributions->political_curve[1].y = {};
        auto retained = take(gc::GalacticSession::create(zero));
        for (int tick = 0; tick < 5; ++tick) (void)take(retained.step(pool));
        expect(retained.snapshot()->control()->state[0].political_income == f(-7),
            "nonpositive curve returns retain the old source instead of deleting it");
        auto unit_curve = config; unit_curve.fiscal->contributions->political_curve[1].y = f(1);
        auto removed = take(gc::GalacticSession::create(unit_curve));
        for (int tick = 0; tick < 5; ++tick) (void)take(removed.step(pool));
        expect(removed.snapshot()->control()->state[0].political_income == Fixed{},
            "positive curve producing zero contribution removes the prior source");
        if (reference.empty()) reference = hashes; else expect(hashes == reference, "political income is deterministic on 1/2/4/8 workers");
    }
}
void land_return_publication() {
    auto config = setup();
    config.income->types[1].gambling.reset();
    config.observer.emplace();
    config.observer->local_player = 1;
    const auto intelligence = gc::bit(gc::Intelligence::income_breakdown);
    for (const auto& player : config.players) config.observer->players.push_back({player.player_id, false, false, intelligence});
    for (const auto& planet : config.planets) {
        gc::ObserverPlanet row; row.planet = planet.id;
        row.allegiance = row.target_owner = row.current_owner = row.current_controller = planet.owner;
        config.observer->planets.push_back(row);
    }
    config.observer->types.push_back({1000});
    for (const auto& player : config.players) for (const auto& planet : config.planets) {
        gc::ObserverRecord record; record.player = player.player_id; record.planet = planet.id;
        record.discovered = true; record.mask = intelligence;
        config.observer->records.push_back(record);
    }
    const auto source = config.fleets[0].members[0];
    config.fleets.erase(config.fleets.begin());
    config.control.emplace();
    auto& control = *config.control;
    control.change_seconds = f(7); control.max_ground_forces = 10;
    for (const auto& player : config.players)
        control.players.push_back({player.player_id, player.player_id == 2, player.player_id == 1, player.faction_id});
    for (const auto& a : config.players) for (const auto& b : config.players)
        control.relations.push_back({a.player_id, b.player_id, a.player_id == b.player_id});
    control.types = {{1000, 0, false, true}};
    for (const auto& planet : config.planets) {
        gc::ControlPlanet row; row.planet = planet.id; row.type = 900; row.surface_accessible = true;
        control.planets.push_back(row);
    }
    for (const auto& route : config.routes) control.routes.push_back({route.id, f(1)});
    control.planets[0].landed = {{source.id, source.type, 1}};
    std::optional<gc::EconomyForecast> reference;
    std::string reference_hash;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(gc::GalacticSession::create(config));
        (void)take(game.step(pool));
        expect(game.snapshot()->economy_forecast()->planets[0].modifiers
            == std::optional(std::vector<gc::PlanetIncomeForecast::Modifier>{{1000, f(30)}}),
            "land-return fixture warms the defender's landed mining source");
        bool notified = false;
        game.set_control_effects_handler([&](auto, auto) {
            notified = true;
            (void)take(game.add_credits(1, {}, f(1)));
            const auto& facts = *game.snapshot()->economy_forecast();
            expect(facts.planets[0].modifiers && facts.planets[0].modifiers->empty(),
                "land-return callback publication excludes the removed defender source");
            if (!reference) { reference = facts; reference_hash = game.state_sha256(); }
            else {
                expect(facts == *reference, "land-return callback exact page facts on 1/2/4/8");
                expect(game.state_sha256() == reference_hash, "land-return callback state hash on 1/2/4/8");
            }
            return eawr::core::Result<void>::success();
        });
        gc::ControlBattleReturn returned; returned.planet = 1;
        returned.invader = returned.winner = 2; returned.defender = 1; returned.land = true;
        take(game.apply_control_battle(returned));
        expect(notified, "land victory invokes the control callback");
    }
}
} // namespace
// #3003: the gathered query's inline category buffer keeps the general per-category maximum,
// with repeated categories and past the buffer's size.
void gathered_reduction_categories() {
    std::vector<gc::Planet> planets(1); planets[0].id = 1; planets[0].owner = 1;
    for (const std::uint32_t rows : {6U, 12U, 20U}) {
        gc::IncomeSetup income; income.planets = {{1, false, false, false}};
        gc::EconomicType type; type.type = 900;
        std::vector<gc::ProductionReduction> expected;
        for (std::uint32_t row = 0; row < rows; ++row) {
            const auto category = (row * 7) % 10; // out of order; categories repeat from the 11th row
            const auto value = Fixed::from_raw(Fixed::scale / 1000 * static_cast<std::int64_t>((row * 13) % 17 + 1));
            type.reductions.push_back({2000, category, value, true, false});
            expected.push_back({1, category, value});
        }
        income.types = {type}; income.sources = {{1, 900, 1, 1, false, true}};
        for (const bool price : {true, false})
            expect(take(gc::income_production_multiplier(income, planets, 1, 1, 2000, price))
                == take(gc::production_reduction_multiplier(price ? expected : std::vector<gc::ProductionReduction>{}, {}, price)),
                "gathered category maximum equals the general reduction multiplier");
    }
}
int case_galactic_income_tests() {
    try { math_rules(); filters(); planet_reduction_cards(); technology_price_planets(); technology_price_callbacks(); lifecycle_and_queues(); replay_checkpoint(); installed_data(); publication_cache(); political_income_retention(); land_return_publication();
        gathered_reduction_categories(); nested_sources(); ruined_income_readers(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return failures ? 1 : 0;
}
