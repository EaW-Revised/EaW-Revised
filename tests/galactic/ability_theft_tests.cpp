#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "ability_fixture.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/ability_host.hpp"
#include "eawr/sim/galactic/income.hpp"
#include "eawr/sim/galactic/observer.hpp"

#include <exception>
#include <utility>
using eawr::testing::take;

namespace {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::checkpoint;
using galactic_test::expect;
using eawr::sim::math::Fixed;

Fixed f(std::int64_t n) { return take(Fixed::from_integer(n)); }
using ability_fixture::base_setup;
eawr::sim::EntityId buyer(const gc::GalacticSetup& s) { return s.fleets[1].members[0].id; }
void order(eawr::campaign::GameSession& game, std::uint64_t sequence, gc::CommandPayload payload) {
    take(game.submit({{game.completed_tick(), {2}, sequence}, std::move(payload)}));
}

constexpr gc::TypeId first_type = (std::uint64_t{1} << 40) + 7;
gc::GalacticSetup setup(bool succeeds = true, bool multiple = false) {
    auto s = base_setup(false); s.seed = 1;
    auto& profile = s.ability_host->profiles[0]; profile.has_market = false; profile.theft.emplace();
    auto& ability = *profile.theft; ability.initially_enabled = true;
    ability.availability = {{f(0), f(1)}, {f(5), f(1)}};
    ability.ground = ability.space = {{f(0), f(0)}, {f(5), f(0)}};
    ability.difficulty = {{f(0), f(succeeds ? 0 : -1)}, {f(5), f(succeeds ? 0 : -1)}};
    ability.absence_min = ability.absence_max = {f(2), f(2), f(2), f(2), f(2)};
    s.ability_host->technologies = {{first_type, f(100), false}};
    s.production->technology_types = {{first_type, 2, true, true, {2}}};
    if (multiple) {
        s.ability_host->technologies.insert(s.ability_host->technologies.begin(), {first_type + 1, f(200), true});
        s.production->technology_types.push_back({first_type + 1, 2, true, true, {2}});
    }
    auto& control = ability_fixture::add_control(s);
    control.types = {{1000, 0, false, false}};
    for (const auto& planet : s.planets) {
        gc::ControlPlanet row; row.planet = planet.id; row.type = 1000;
        row.surface_accessible = true; row.ground_base = {planet.owner, 1}; row.starbase = {planet.owner, 1};
        control.planets.push_back(row);
        s.ability_host->theft_planets.push_back({planet.id, f(3), f(1), {}, false});
    }
    return s;
}
gc::PreparedAbility prepare(eawr::campaign::GameSession& game, const gc::GalacticSetup& config,
    const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    order(game, 1, gc::PrepareGalacticAbility{buyer(config), config.planets[0].id, gc::AbilityKind::technology_theft});
    hashes.push_back(take(game.step(pool)).state_hash.get());
    expect(game.galaxy().ability_state()->preparations.size() == 1, "ordinary theft preparation retains exact authored facts");
    return gc::prepared_ability(game.galaxy().ability_state()->preparations.at(0), 0);
}
void cases(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    for (const bool multiple : {false, true}) {
        const auto config = setup(true, multiple); auto game = take(eawr::campaign::GameSession::create(config));
        const auto prepared = prepare(game, config, pool, hashes);
        expect(prepared.kind == gc::AbilityKind::technology_theft && prepared.theft.technology == first_type + (multiple ? 1 : 0)
            && prepared.theft.advance == !multiple && prepared.absence == f(2), "registration order, full64 identity and whole-list advancement");
        const auto payload = gc::encode_payload(gc::ExecutePreparedAbility{prepared});
        expect(gc::decode_payload(gc::CommandLayer::galactic, gc::galactic_opcodes::special_execute, payload)
            == gc::CommandPayload{gc::ExecutePreparedAbility{prepared}}, "full64 theft command round trip");
        auto forged = prepared; forged.theft.chance = {};
        order(game, 2, gc::ExecutePreparedAbility{forged}); hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(1000) && game.return_state().heroes.empty(), "forged chance cannot buy or consume");
        const auto identities = cp::game_identities(config); const auto saved = take(cp::save_game_checkpoint(game, identities));
        auto fresh = take(cp::load_game_checkpoint(saved, identities, {}, pool));
        expect(take(cp::save_game_checkpoint(fresh.session, identities)) == saved, "prepared theft fresh restore retains draws and prices");
        game = std::move(fresh.session); order(game, 3, gc::ExecutePreparedAbility{prepared});
        hashes.push_back(take(game.step(pool)).state_hash.get());
        const auto* snapshot = game.galaxy().snapshot().get();
        expect(snapshot->player_economy()->players[1].credits == f(multiple ? 800 : 900)
            && snapshot->player_economy()->players[1].current_tech == (multiple ? 2 : 3)
            && snapshot->production()->players[1].unlocked == std::vector<gc::TypeId>{prepared.theft.technology}, "real debit, unlock and one-option tech advancement");
        expect(game.return_state().heroes.size() == 1 && game.return_state().heroes[0].deadline == 62,
            "successful theft schedules shared return before canonical actor loss");
        const auto settled = take(cp::save_game_checkpoint(game, identities)); auto restored = take(cp::load_game_checkpoint(settled, identities, {}, pool));
        expect(take(cp::save_game_checkpoint(restored.session, identities)) == settled, "settled theft does not repeat payment, unlock or scheduling on load");
        hashes.push_back(restored.session.state_sha256());
    }
    for (const auto& [seed, branch] : {std::pair{3U, 1U}, std::pair{2U, 2U}, std::pair{1U, 3U}}) {
        auto config = setup(false); config.seed = seed;
        auto game = take(eawr::campaign::GameSession::create(config)); const auto prepared = prepare(game, config, pool, hashes);
        order(game, 2, gc::ExecutePreparedAbility{prepared}); hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(branch == 3 ? 900 : 1000)
            && game.galaxy().snapshot()->production()->players[1].unlocked.empty(), "three failure outcomes preserve sourced payment and no-unlock distinctions");
        expect(game.return_state().heroes.size() == 1 && game.return_state().heroes[0].deadline == (branch == 1 ? 121U : 61U),
            "false theft consumes with doubled or ordinary absence");
        expect(game.galaxy().ability_state()->results.at(0).outcome == gc::AbilityOutcome::refused,
            "consumed failure publishes a coarse result");
    }
    {
        auto config = setup(); config.player_economy->players[1].maximum_tech = 2;
        auto game = take(eawr::campaign::GameSession::create(config)); const auto prepared = prepare(game, config, pool, hashes);
        order(game, 2, gc::ExecutePreparedAbility{prepared}); hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(game.galaxy().snapshot()->player_economy()->players[1].current_tech == 2
            && game.galaxy().snapshot()->player_economy()->players[1].credits == f(900)
            && game.return_state().heroes.size() == 1, "maximum tech skips increment while retaining successful purchase and consumption");
    }
    {
        auto config = setup(); config.production->players[1].unlocked = {first_type};
        auto game = take(eawr::campaign::GameSession::create(config));
        order(game, 1, gc::PrepareGalacticAbility{buyer(config), config.planets[0].id, gc::AbilityKind::technology_theft});
        hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(game.galaxy().ability_state()->preparations.empty() && game.galaxy().snapshot()->player_economy()->players[1].current_tech == 2
            && game.return_state().heroes.empty(), "initially empty list refuses before free technology or consumption");
    }
}
void lifecycle(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    auto config = setup(); auto changed = config.ability_host->profiles[0]; changed.type = 1002;
    changed.theft->availability = {{f(0), f(2)}, {f(5), f(2)}};
    changed.theft->difficulty = {{f(0), f(-1)}, {f(5), f(-1)}};
    config.ability_host->profiles.push_back(changed); config.census->types.push_back({1002, f(1), 0});
    config.fleet_control->types.push_back({1002, false, true, false}); config.control->types.push_back({1002, 0, false, false});
    // A representation change needs the constructor's dispatch facts for the new type (no former-role carry-over).
    ability_fixture::add_movement(config); ability_fixture::add_nested_creation(config, {1000, 1002});
    auto game = take(eawr::campaign::GameSession::create(config)); const auto prepared = prepare(game, config, pool, hashes);
    take(game.change_actor_current_type(buyer(config), 1002));
    take(game.set_ability_lifecycle(buyer(config), false, true, gc::AbilityKind::technology_theft));
    const auto identities = cp::game_identities(config); const auto saved = take(cp::save_game_checkpoint(game, identities));
    auto fresh = take(cp::load_game_checkpoint(saved, identities, {}, pool));
    expect(take(cp::save_game_checkpoint(fresh.session, identities)) == saved, "changed theft profile and service flags reconstruct at native barriers");
    game = std::move(fresh.session); order(game, 2, gc::ExecutePreparedAbility{prepared}); hashes.push_back(take(game.step(pool)).state_hash.get());
    expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(1000) && game.return_state().heroes.empty(), "disabled/cancelled theft service refuses without consumption");
    take(game.set_ability_lifecycle(buyer(config), true, false, gc::AbilityKind::technology_theft));
    order(game, 3, gc::ExecutePreparedAbility{prepared}); hashes.push_back(take(game.step(pool)).state_hash.get());
    expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(900)
        && game.galaxy().snapshot()->production()->players[1].unlocked == std::vector<gc::TypeId>{first_type},
        "restored execution uses old price and chance, not changed profile values");
}
void observe(gc::GalacticSetup& config, gc::PlayerId local) { ability_fixture::add_observer(config, local, {1000}); }
void observer_and_idle() {
    eawr::sim::InlineExecutor pool; auto config = setup(); observe(config, 2);
    auto game = take(eawr::campaign::GameSession::create(config)); std::vector<std::string> unused;
    const auto prepared = prepare(game, config, pool, unused);
    const auto* menu = game.galaxy().snapshot()->hero_action_menus();
    expect(menu && menu->preparation == prepared.preparation && menu->theft->at(0).live_prepared == prepared,
        "live discovered owner menu carries the exact64 typed command, not a fixture");
    order(game, 2, gc::PauseGalaxy{}); (void)take(game.step(pool));
    const auto before = game.galaxy().ability_work(); menu = game.galaxy().snapshot()->hero_action_menus();
    for (unsigned i = 0; i < 10000; ++i) {
        (void)take(game.step(pool));
        expect(game.galaxy().snapshot()->hero_action_menus() == menu, "idle publication reuses the immutable menu allocation");
    }
    expect(game.galaxy().ability_work() == before, "10000 idle paused ticks perform no ability preparation, registry sweep, execution or publication rebuild");
    auto foreign_config = setup(); observe(foreign_config, 1);
    auto foreign = take(eawr::campaign::GameSession::create(foreign_config));
    const auto choice = prepare(foreign, foreign_config, pool, unused);
    expect(!foreign.galaxy().snapshot()->hero_action_menus(), "foreign observer receives no owned theft menu");
    order(foreign, 2, gc::ExecutePreparedAbility{choice}); (void)take(foreign.step(pool));
    expect(foreign.galaxy().snapshot()->ability_results().empty(), "foreign observer receives no theft entitlement or receipt");
}
void parent_loss(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    auto config = setup(); const auto parent = buyer(config); const auto target = config.planets[0].id;
    constexpr eawr::sim::EntityId hero = 50000, companion = 50001;
    config.fleets[1].members[0].type = 2000;
    config.census->objects[1].original_type = config.census->objects[1].current_type = 2000;
    config.census->objects.push_back(ability_fixture::census_actor(hero, 1000, 2, target, parent));
    config.census->objects.push_back(ability_fixture::census_actor(companion, 3000, 2, target, parent));
    config.census->types.push_back({2000, {}, 0}); config.census->types.push_back({3000, f(1), 0});
    config.fleet_control->types.push_back({2000, true, true, false}); config.fleet_control->types.push_back({3000, false, true, false});
    config.control->types.push_back({2000, 0, false, false}); config.control->types.push_back({3000, 0, false, false});
    auto& movement = ability_fixture::add_movement(config);
    movement.types[1].transport = true; // the transport type that carries the hero and companion
    movement.contents = {{parent, {{hero, 1000}, {companion, 3000}}}};
    config.ability_host->profiles[0].ground_activated = {gc::AbilityKind::technology_theft};
    auto game = take(eawr::campaign::GameSession::create(config));
    order(game, 1, gc::DeployGroundAbility{parent, target}); hashes.push_back(take(game.step(pool)).state_hash.get());
    const auto prepared = gc::prepared_ability(game.galaxy().ability_state()->preparations.at(0), 0);
    order(game, 2, gc::ExecutePreparedAbility{prepared}); hashes.push_back(take(game.step(pool)).state_hash.get());
    const auto records = game.galaxy().snapshot()->actor_records();
    expect(std::none_of(records.begin(), records.end(), [&](const auto& row) { return row.id == hero || row.id == parent || row.id == companion; }),
        "shared theft removal destroys the immediate transport and real carried identities");
    expect(game.return_state().heroes.size() == 1 && game.return_state().heroes[0].defeated == hero,
        "parent-check bypass schedules the selected hero without granting companion respawns");
    const auto identities = cp::game_identities(config); const auto saved = take(cp::save_game_checkpoint(game, identities));
    auto fresh = take(cp::load_game_checkpoint(saved, identities, {}, pool));
    expect(take(cp::save_game_checkpoint(fresh.session, identities)) == saved, "parent-loss cleanup reconstructs in canonical saved owners");
}
gc::GalacticSetup spy_setup(bool consumes = true) {
    auto s = setup();
    auto& p = s.ability_host->profiles[0]; p.theft.reset(); p.named = false;
    p.spy = gc::SpyAbilityProfile{"SYNTHETIC_SPY", "synthetic probe", 7, f(1), gc::bit(gc::Intelligence::fleet_contents), true, consumes, true};
    p.ground_activated = {gc::AbilityKind::system_spy};
    s.ability_host->activated_spy_abilities = {"SYNTHETIC_SPY"};
    s.seats.push_back({{3}, 3, gc::SeatKind::script, 2, true, 0});
    gc::ObserverSetup o; o.human_observers = {1, 2};
    for (const auto& player : s.players) o.players.push_back({player.player_id, false, false, 0, 0, {}});
    for (const auto& planet : s.planets) {
        o.planets.push_back({planet.id, planet.owner, planet.owner, 0, 0, 0, 0, false, false, false, false, {}});
        o.planets.back().current_owner = o.planets.back().current_controller = planet.owner;
    }
    o.types = {{1000, false, false, false}};
    for (const auto& player : s.players) for (const auto& planet : s.planets)
        o.records.push_back({player.player_id, planet.id, true, 0, 0, 0, {}});
    s.observer = std::move(o); return s;
}
void first_appropriate_bearer(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    auto config = spy_setup(false); const auto parent = buyer(config), target = config.planets[0].id;
    constexpr eawr::sim::EntityId slicer = 51000, spy = 51001;
    auto& host = *config.ability_host;
    gc::GalacticAbilityProfile theft; theft.type = 2000; theft.has_market = false; theft.theft.emplace();
    theft.ground_activated = {gc::AbilityKind::technology_theft}; host.profiles.push_back(theft);
    host.technologies.clear(); // first contained actor has no appropriate slice target
    config.fleets[1].members[0].type = 3000;
    auto& root = config.census->objects[1]; root.current_type = root.original_type = 3000;
    root.combat_role = gc::ActorCombatRole::transport;
    config.census->objects.push_back(ability_fixture::census_actor(slicer, 2000, 2, target, parent));
    config.census->objects.push_back(ability_fixture::census_actor(spy, 1000, 2, target, parent));
    for (const auto type : {2000U, 3000U}) {
        config.census->types.push_back({type, {}, 0}); config.fleet_control->types.push_back({type, type == 3000, true, false});
        config.control->types.push_back({type, 0, false, false}); config.observer->types.push_back({type, false, false, false});
    }
    auto& movement = ability_fixture::add_movement(config); movement.types[2].transport = true;
    movement.contents = {{parent, {{slicer, 2000}, {spy, 1000}}}};
    auto game = take(eawr::campaign::GameSession::create(config));
    order(game, 1, gc::DeployGroundAbility{parent, target}); hashes.push_back(take(game.step(pool)).state_hash.get());
    expect(game.galaxy().ability_state()->results.size() == 1 && game.galaxy().ability_state()->results[0].actor == spy
        && game.galaxy().ability_state()->results[0].kind == gc::AbilityKind::system_spy,
        "live container scan skips inappropriate empty theft and invokes the first appropriate bearer");
}
void consumed_contained_spy(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    for (const bool escort : {false, true}) {
    auto config = spy_setup(); const auto parent = buyer(config), target = config.planets[0].id;
    constexpr eawr::sim::EntityId spy = 52000;
    config.fleets[1].members[0].type = 2000;
    auto& root = config.census->objects[1]; root.current_type = root.original_type = 2000;
    root.combat_role = gc::ActorCombatRole::transport;
    config.census->objects.push_back(ability_fixture::census_actor(spy, 1000, 2, target, parent));
    config.census->types.push_back({2000, {}, 0}); config.fleet_control->types.push_back({2000, true, true, false});
    config.control->types.push_back({2000, 0, false, false}); config.observer->types.push_back({2000, false, false, false});
    auto& movement = ability_fixture::add_movement(config); movement.types[1].transport = true;
    movement.contents = {{parent, {{spy, 1000}}}};
    constexpr eawr::sim::EntityId companion = 52001;
    if (escort) {
        config.census->objects.push_back(ability_fixture::census_actor(companion, 1000, 2, target, parent));
        movement.contents[0].children.push_back({companion, 1000});
    }
    auto game = take(eawr::campaign::GameSession::create(config));
    order(game, 1, gc::DeployGroundAbility{parent, target}); hashes.push_back(take(game.step(pool)).state_hash.get());
    const auto records = game.galaxy().snapshot()->actor_records();
    expect(std::none_of(records.begin(), records.end(), [&](const auto& r) { return r.id == spy; })
        && std::any_of(records.begin(), records.end(), [&](const auto& r) { return r.id == parent; }) == escort,
        "consumed spy removes an empty transport and retains a non-empty carrier");
    expect(std::any_of(records.begin(), records.end(), [&](const auto& r) { return r.id == companion; }) == escort,
        "consumed spy leaves the carrier's other passenger unchanged");
    expect(std::any_of(game.galaxy().snapshot()->fleets().begin(), game.galaxy().snapshot()->fleets().end(),
        [&](const auto& fleet) { return fleet.id == config.fleets[1].id; }) == escort,
        "ordinary loss cleanup removes the empty fleet shell and preserves an occupied fleet");
    const auto observer = game.galaxy().snapshot()->observer();
    expect(std::any_of(observer->records.begin(), observer->records.end(), [&](const auto& r) {
        return r.player == 2 && r.planet == target && r.sources.size() == 1 && r.sources[0].source == spy;
    }), "contained consumed probe retains its finite intelligence source");
    const auto ids = cp::game_identities(config); const auto saved = take(cp::save_game_checkpoint(game, ids));
    auto loaded = take(cp::load_game_checkpoint(saved, ids, {}, pool));
    expect(take(cp::save_game_checkpoint(loaded.session, ids)) == saved, "carrier cleanup and finite effect restore together");
    }
}
void spy_cases(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    for (const bool consumes : {false, true}) {
        auto config = spy_setup(consumes); auto game = take(eawr::campaign::GameSession::create(config));
        const auto actor = buyer(config), planet = config.planets[0].id;
        const gc::CommandPayload request = gc::DeployGroundAbility{actor, planet};
        expect(gc::opcode_of(request) == gc::galactic_opcodes::ground_deploy
            && gc::decode_payload(gc::CommandLayer::galactic, gc::galactic_opcodes::ground_deploy, gc::encode_payload(request)) == request,
            "generic deployment wire retains only supplied actor and planet");
        auto malformed = gc::encode_payload(request); malformed.push_back(0);
        expect(!gc::decode_payload(gc::CommandLayer::galactic, gc::galactic_opcodes::ground_deploy, malformed), "deployment rejects trailing forged fields");
        // Wrong owner refuses before applying intelligence.
        take(game.submit({{game.completed_tick(), {1}, 1}, request}));
        hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(game.galaxy().ability_state()->results.empty(), "foreign seat cannot deploy a spy");
        take(game.set_ability_lifecycle(actor, true, true, gc::AbilityKind::system_spy));
        order(game, 2, request); hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(game.galaxy().ability_state()->results.empty(), "cancelled spy service refuses deployment");
        take(game.set_ability_lifecycle(actor, true, false, gc::AbilityKind::system_spy));
        const auto ids = cp::game_identities(config); const auto saved = take(cp::save_game_checkpoint(game, ids));
        auto loaded = take(cp::load_game_checkpoint(saved, ids, {}, pool)); game = std::move(loaded.session);
        unsigned land_notices{};
        game.set_story_notice_handler([&](const eawr::campaign::GameStoryNotice& notice) {
            if (const auto* travel = std::get_if<gc::story::TravelNotice>(&notice.payload); travel && travel->kind == gc::story::Travel::land) {
                expect(notice.player == gc::PlayerId{2} && notice.observer_planet == planet && travel->hero == "synthetic probe",
                    "spy land notice uses current owner and authored source");
                ++land_notices;
            }
            return eawr::core::Result<void>::success();
        });
        order(game, 3, request); hashes.push_back(take(game.step(pool)).state_hash.get());
        const auto* state = game.galaxy().ability_state();
        expect(state->results.size() == 1 && state->results[0].kind == gc::AbilityKind::system_spy
            && state->results[0].outcome == gc::AbilityOutcome::purchased && land_notices == 1, "spy invocation settles after successful insertion and story notice");
        const auto observer = game.galaxy().snapshot()->observer();
        const auto row = std::find_if(observer->records.begin(), observer->records.end(), [&](const auto& r) { return r.player == 2 && r.planet == planet; });
        expect(row != observer->records.end() && row->sources.size() == 1 && row->sources[0].source == actor
            && row->sources[0].ability == 7 && row->sources[0].mask == gc::bit(gc::Intelligence::fleet_contents), "authored source-keyed intelligence applied once");
        expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(1000) && game.return_state().heroes.empty(),
            "spy has no debit or random theft/unnamed-hero respawn");
        const auto records = game.galaxy().snapshot()->actor_records();
        expect(std::any_of(records.begin(), records.end(), [&](const auto& r) { return r.id == actor; }) == !consumes,
            "Causes_Despawn controls success-only actor consumption");
        const auto settled = take(cp::save_game_checkpoint(game, ids)); auto restored = take(cp::load_game_checkpoint(settled, ids, {}, pool));
        expect(take(cp::save_game_checkpoint(restored.session, ids)) == settled, "spy service/registered targets/timed effect restore without reapplication");
        hashes.push_back(restored.session.state_sha256());
        if (consumes) {
            const auto deadline = row->sources[0].final_frame;
            for (const auto frame : {deadline, deadline + 1}) {
                gc::ObserverCommand refresh; refresh.action = gc::ObserverAction::refresh; refresh.gameplay_frame = frame;
                take(restored.session.submit({{restored.session.completed_tick(), {3}, frame}, refresh}));
                hashes.push_back(take(restored.session.step(pool)).state_hash.get());
                const auto intel = restored.session.galaxy().snapshot()->observer();
                const auto effect = std::find_if(intel->records.begin(), intel->records.end(), [&](const auto& r) { return r.player == 2 && r.planet == planet; });
                expect(effect != intel->records.end() && effect->sources.empty() == (frame > deadline),
                    "consumed finite probe survives through its deadline and expires strictly afterward");
            }
        }
        if (!consumes) {
            auto standalone = take(gc::GalacticSession::create(config));
            take(standalone.submit({{0, {2}, 1}, request})); const auto invoked = take(standalone.step(pool));
            const auto events = invoked.snapshot->events();
            expect(std::count_if(events.begin(), events.end(), [&](const auto& event) {
                return event.kind == gc::GalacticEventKind::intelligence_refreshed && event.subject == planet;
            }) == static_cast<std::ptrdiff_t>(config.players.size()), "spy insertion refreshes every extant player's target visibility");
            const std::array gone{actor}; take(standalone.apply_battle_losses(gone));
            const auto remaining = standalone.snapshot()->observer();
            expect(std::all_of(remaining->records.begin(), remaining->records.end(), [](const auto& r) { return r.sources.empty(); }),
                "ordinary spy source loss cleans registered intelligence");
        }
    }
    for (const bool allied : {false, true}) {
        auto config = spy_setup(); if (allied) config.fleets[1].orbit = config.planets[1].id;
        if (allied) config.census->objects[1].planet = config.planets[1].id;
        auto game = take(eawr::campaign::GameSession::create(config));
        const auto target = config.planets[allied ? 1 : 2].id;
        order(game, 1, gc::DeployGroundAbility{buyer(config), target}); hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(game.galaxy().ability_state()->results.empty(), "allied target or remote invocation refuses without consumption");
    }
}
// WGUI-19/101: a unit with an automatic spy informs its current owner at its planet without any activation.
void auto_spy_cases(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    auto config = spy_setup(false); const auto actor = buyer(config);
    const auto planet = std::find_if(config.census->objects.begin(), config.census->objects.end(), [&](const auto& o) { return o.id == actor; })->planet;
    auto& profile = config.ability_host->profiles[0];
    profile.spy.reset(); profile.ground_activated.clear(); config.ability_host->activated_spy_abilities.clear();
    profile.auto_spies = {{"automatic probe", 9, Fixed::from_raw(-Fixed::scale), {},
        gc::bit(gc::Intelligence::fleet_contents)}};
    auto game = take(eawr::campaign::GameSession::create(config));
    hashes.push_back(take(game.step(pool)).state_hash.get());
    const auto holders = [&](const gc::ObserverSetup& observer) {
        std::vector<std::pair<gc::PlayerId, gc::PlanetId>> rows;
        for (const auto& r : observer.records) if (std::any_of(r.sources.begin(), r.sources.end(), [&](const auto& s) { return s.source == actor && s.ability == 9; }))
            rows.emplace_back(r.player, r.planet);
        return rows;
    };
    const auto before = holders(*game.galaxy().snapshot()->observer());
    if (!(before.size() == 1 && before[0] == std::pair{gc::PlayerId{2}, planet})) {
        const auto observer = game.galaxy().snapshot()->observer();
        const auto row = std::find_if(config.census->objects.begin(), config.census->objects.end(), [&](const auto& o) { return o.id == actor; });
        std::cerr << "auto spy diag: rows=" << before.size() << " actor=" << static_cast<unsigned long long>(actor) << " owner=" << static_cast<unsigned long long>(row->owner) << " planet=" << static_cast<unsigned long long>(row->planet)
                  << " type=" << static_cast<unsigned long long>(row->current_type) << " profile_type=" << static_cast<unsigned long long>(profile.type) << " auto_spies=" << profile.auto_spies.size()
                  << " players=" << observer->players.size() << " planets=" << observer->planets.size() << " records=" << observer->records.size()
                  << " fps=" << observer->logical_fps;
        for (const auto& r : before) std::cerr << " row(" << static_cast<unsigned long long>(r.first) << "," << static_cast<unsigned long long>(r.second) << ")";
        std::cerr << std::endl;
    }
    expect(before.size() == 1 && before[0] == std::pair{gc::PlayerId{2}, planet}, "automatic spy informs only its owner at its own planet");
    const auto ids = cp::game_identities(config); const auto saved = take(cp::save_game_checkpoint(game, ids));
    auto loaded = take(cp::load_game_checkpoint(saved, ids, {}, pool));
    expect(take(cp::save_game_checkpoint(loaded.session, ids)) == saved
        && holders(*loaded.session.galaxy().snapshot()->observer()) == before, "automatic spy restores without a second application");
    hashes.push_back(loaded.session.state_sha256());
    auto standalone = take(gc::GalacticSession::create(config));
    const std::array gone{actor}; take(standalone.apply_battle_losses(gone));
    expect(holders(*standalone.snapshot()->observer()).empty(), "automatic spy source loss removes its intelligence");
}
} // namespace
int case_galactic_ability_theft_tests() {
    ability_fixture::arm_watchdog();
    try {
        std::vector<std::string> baseline; eawr::platform::ThreadWorkerAdapter one(1); cases(one, baseline); lifecycle(one, baseline); parent_loss(one, baseline); spy_cases(one, baseline); first_appropriate_bearer(one, baseline); consumed_contained_spy(one, baseline); auto_spy_cases(one, baseline);
        for (std::size_t workers : {2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter pool(workers); std::vector<std::string> hashes; cases(pool, hashes); lifecycle(pool, hashes); parent_loss(pool, hashes); spy_cases(pool, hashes); first_appropriate_bearer(pool, hashes); consumed_contained_spy(pool, hashes); auto_spy_cases(pool, hashes);
            expect(hashes == baseline, "theft effects and restore hashes equal on1/2/4/8");
        }
        observer_and_idle();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
