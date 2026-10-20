#include <algorithm>
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

#include <exception>
#include <utility>
using eawr::testing::take;

// WGCH-20..26/52/91..95/131..134: live hero neutralization on the shared ability host, over a fixture hero (the
// story bounty hunters are a later lane). Preparation prices each hostile hero and samples both absence times;
// execution removes the victim, counts the history, pays, then consumes the actor.
namespace {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::checkpoint;
using galactic_test::expect;
using eawr::sim::math::Fixed;

Fixed f(std::int64_t n) { return take(Fixed::from_integer(n)); }
using ability_fixture::base_setup;
eawr::sim::EntityId neutralizer(const gc::GalacticSetup& s) { return s.fleets[1].members[0].id; }
eawr::sim::EntityId victim(const gc::GalacticSetup& s) { return s.fleets[0].members[0].id; }
void order(eawr::campaign::GameSession& game, std::uint64_t sequence, gc::CommandPayload payload) {
    take(game.submit({{game.completed_tick(), {2}, sequence}, std::move(payload)}));
}

// The fixture victim is the opposing hero of the same type, parked in orbit of the same planet.
gc::GalacticSetup setup(bool hostile_present = true) {
    auto s = base_setup(false); s.seed = 1;
    auto& profile = s.ability_host->profiles[0]; profile.has_market = false;
    auto& n = profile.neutralize.emplace(); n.name = "NEU"; n.initially_enabled = true;
    n.base_curve = {{f(0), f(0)}, {f(2), f(1)}};          // level 1 -> 1/2
    n.history_curve = {{f(0), f(0)}, {f(1000), f(750)}};
    n.corruption_curve = {{f(0), f(0)}, {f(4), f(1)}};    // corrupted -> 1/4
    n.minor_modifier = f(1); n.major_modifier = f(2);
    n.actor_absence_min = f(120); n.actor_absence_max = f(150);
    n.target_absence_min = f(240); n.target_absence_max = f(300);
    n.can_minor = n.can_major = true;
    s.ability_host->activated_neutralize_abilities = {"NEU"};
    s.ability_host->neutralization_targets = {{1000, f(20), true, true}};
    auto& control = ability_fixture::add_control(s);
    control.types = {{1000, 0, false, false}};
    for (const auto& planet : s.planets) {
        gc::ControlPlanet row; row.planet = planet.id; row.type = 1000;
        row.surface_accessible = true; row.ground_base = {planet.owner, 1}; row.starbase = {planet.owner, 1};
        control.planets.push_back(row);
        s.ability_host->theft_planets.push_back({planet.id, f(3), f(1), {}, false});
    }
    // The synthetic galaxy may already park the opposing fleet here: place it explicitly either way.
    const auto home = hostile_present ? s.planets[0].id : s.planets[1].id;
    s.fleets[0].orbit = home;
    for (auto& row : s.census->objects) if (row.id == victim(s)) row.planet = home;
    return s;
}
gc::PreparedAbility prepare(eawr::campaign::GameSession& game, const gc::GalacticSetup& config,
    const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    order(game, 1, gc::PrepareGalacticAbility{neutralizer(config), config.planets[0].id, gc::AbilityKind::hero_neutralization});
    hashes.push_back(take(game.step(pool)).state_hash.get());
    expect(game.galaxy().ability_state()->preparations.size() == 1, "neutralization preparation is retained");
    return gc::prepared_ability(game.galaxy().ability_state()->preparations.at(0), 0);
}
Fixed credits(const eawr::campaign::GameSession& game) { return game.galaxy().snapshot()->player_economy()->players[1].credits; }
bool present(const eawr::campaign::GameSession& game, eawr::sim::EntityId id) {
    const auto records = game.galaxy().snapshot()->actor_records();
    return std::any_of(records.begin(), records.end(), [&](const auto& row) { return row.id == id; });
}

void cases(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    {   // Price (WGCH-22), absence samples (WGCH-21/93), forgery, restore, success (WGCH-26).
        const auto config = setup(); auto game = take(eawr::campaign::GameSession::create(config));
        const auto prepared = prepare(game, config, pool, hashes);
        // Named victim: base 20 x major 2 = 40; + 40 x 1/2 (starbase level 1, ally of the controller); no history;
        // - 40 x 1/4 (corrupted planet) = 50.
        expect(prepared.kind == gc::AbilityKind::hero_neutralization && prepared.target == victim(config) && prepared.valid
            && prepared.price == f(50) && prepared.company == 1000, "prepared target, company type and WGCH-22 price");
        expect(prepared.absence >= f(120) && prepared.absence <= f(150) && prepared.target_absence >= f(240)
            && prepared.target_absence <= f(300), "actor and target absence come from their own authored ranges");
        const auto payload = gc::encode_payload(gc::ExecutePreparedAbility{prepared});
        expect(gc::decode_payload(gc::CommandLayer::galactic, gc::galactic_opcodes::special_execute, payload)
            == gc::CommandPayload{gc::ExecutePreparedAbility{prepared}}, "neutralization command round trip");
        auto forged = prepared; forged.price = f(1);
        order(game, 2, gc::ExecutePreparedAbility{forged}); hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(credits(game) == f(1000) && present(game, victim(config)) && game.return_state().heroes.empty(), "forged price changes nothing");
        const auto identities = cp::game_identities(config); const auto saved = take(cp::save_game_checkpoint(game, identities));
        auto fresh = take(cp::load_game_checkpoint(saved, identities, {}, pool));
        expect(take(cp::save_game_checkpoint(fresh.session, identities)) == saved, "prepared neutralization restores with draws and prices");
        game = std::move(fresh.session);
        order(game, 3, gc::ExecutePreparedAbility{prepared}); hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(credits(game) == f(950), "the prepared price is debited once");
        expect(!present(game, victim(config)) && !present(game, neutralizer(config)), "victim and actor both leave through the canonical loss");
        expect(game.return_state().heroes.size() == 2, "both heroes are scheduled by the shared return owner");
        const auto* state = game.galaxy().ability_state();
        expect(state->neutralization_history.size() == 1 && state->neutralization_history[0].company == 1000
            && state->neutralization_history[0].attacker == 2 && state->neutralization_history[0].count == 1,
            "history is keyed by company type and attacking player (WGCH-94)");
        expect(state->results.size() == 1 && state->results[0].outcome == gc::AbilityOutcome::purchased
            && state->results[0].kind == gc::AbilityKind::hero_neutralization, "typed purchased result");
        const auto settled = take(cp::save_game_checkpoint(game, identities)); auto restored = take(cp::load_game_checkpoint(settled, identities, {}, pool));
        expect(take(cp::save_game_checkpoint(restored.session, identities)) == settled, "settled neutralization and history restore");
        hashes.push_back(restored.session.state_sha256());
    }
    {   // A disabled option (the ability may not neutralize major heroes) stays listed but cannot execute (WGCH-24/132).
        auto config = setup(); config.ability_host->profiles[0].neutralize->can_major = false;
        auto game = take(eawr::campaign::GameSession::create(config));
        const auto prepared = prepare(game, config, pool, hashes);
        expect(!prepared.valid && prepared.target == victim(config), "an invalid option is still prepared");
        order(game, 2, gc::ExecutePreparedAbility{prepared}); hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(credits(game) == f(1000) && present(game, victim(config)) && game.return_state().heroes.empty()
            && game.galaxy().ability_state()->neutralization_history.empty(), "an invalid option never charges or removes");
    }
    {   // An unaffordable price refuses (WGCH-24/92).
        auto config = setup(); config.player_economy->players[1].credits = f(10);
        auto game = take(eawr::campaign::GameSession::create(config));
        const auto prepared = prepare(game, config, pool, hashes);
        order(game, 2, gc::ExecutePreparedAbility{prepared}); hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(credits(game) == f(10) && present(game, victim(config)), "insufficient credits refuse before any mutation");
    }
    {   // No hostile hero at the planet: no menu (WGCH-20).
        const auto config = setup(false); auto game = take(eawr::campaign::GameSession::create(config));
        order(game, 1, gc::PrepareGalacticAbility{neutralizer(config), config.planets[0].id, gc::AbilityKind::hero_neutralization});
        hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(game.galaxy().ability_state()->preparations.empty(), "no hostile hero means no neutralization menu");
    }
    {   // An allied hero is never a target.
        auto config = setup();
        for (auto& relation : config.control->relations) if (relation.player == 1 && relation.other == 2) relation.allied = true;
        for (auto& relation : config.control->relations) if (relation.player == 2 && relation.other == 1) relation.allied = true;
        auto game = take(eawr::campaign::GameSession::create(config));
        order(game, 1, gc::PrepareGalacticAbility{neutralizer(config), config.planets[0].id, gc::AbilityKind::hero_neutralization});
        hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(game.galaxy().ability_state()->preparations.empty(), "an ally of the neutralizer is not offered");
    }
}
} // namespace
int case_galactic_ability_neutralize_tests() {
    ability_fixture::arm_watchdog();
    try {
        std::vector<std::string> baseline; eawr::platform::ThreadWorkerAdapter one(1); cases(one, baseline);
        for (std::size_t workers : {2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter pool(workers); std::vector<std::string> hashes; cases(pool, hashes);
            expect(hashes == baseline, "neutralization effects, draws and restore hashes equal on 1/2/4/8");
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
