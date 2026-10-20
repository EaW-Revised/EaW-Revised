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
#include "eawr/sim/galactic/hero_actions.hpp"

#include <exception>
#include <utility>
using eawr::testing::take;

// WGCH-27..30/96..99/135: live special-structure sabotage on the shared ability host. Preparation lists ground then
// space structures with the escalated per-planet price; execution removes the exact structure, then pays, counts the
// planet's history and consumes the saboteur.
namespace {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::checkpoint;
using galactic_test::expect;
using eawr::sim::math::Fixed;

Fixed f(std::int64_t n) { return take(Fixed::from_integer(n)); }
using ability_fixture::base_setup;
eawr::sim::EntityId saboteur(const gc::GalacticSetup& s) { return s.fleets[1].members[0].id; }
constexpr eawr::sim::EntityId second_saboteur = 60001;
void order(eawr::campaign::GameSession& game, std::uint64_t sequence, gc::CommandPayload payload) {
    take(game.submit({{game.completed_tick(), {2}, sequence}, std::move(payload)}));
}

gc::GalacticSetup setup(bool two_saboteurs = false) {
    auto s = base_setup(false); s.seed = 1;
    auto& profile = s.ability_host->profiles[0]; profile.has_market = false;
    profile.sabotage = gc::SabotageAbilityProfile{"SAB", true, f(2)};
    s.ability_host->activated_sabotage_abilities = {"SAB"};
    // Type 2002 authors no sabotage price, so it is never a target (WGCH-97).
    s.ability_host->sabotage_costs = {{2000, f(100)}, {2001, f(50)}};
    for (const gc::TypeId type : {2000U, 2001U, 2002U}) s.census->types.push_back({type, {}, 0});
    auto& control = ability_fixture::add_control(s);
    control.types = {{1000, 0, false, false}};
    for (const auto& planet : s.planets) {
        gc::ControlPlanet row; row.planet = planet.id; row.type = 1000;
        row.surface_accessible = true; row.ground_base = {planet.owner, 1}; row.starbase = {planet.owner, 1};
        control.planets.push_back(row);
    }
    using Domain = gc::ProductionDomain;
    const auto structure = [](eawr::sim::EntityId id, gc::TypeId type, Domain domain) {
        return gc::ProductionObject{id, type, 1, {}, domain, gc::ProductionKind::structure};
    };
    s.production->planets[0].objects = {structure(9001, 2000, Domain::ground), structure(9002, 2001, Domain::ground),
        structure(9003, 2000, Domain::space), structure(9004, 2002, Domain::ground)};
    if (two_saboteurs) {
        s.fleets[1].members.push_back({second_saboteur, 1000});
        s.census->objects.push_back(ability_fixture::census_actor(second_saboteur, 1000, 2, s.planets[0].id, s.fleets[1].id));
        std::sort(s.census->objects.begin(), s.census->objects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    }
    return s;
}
gc::PreparedAbility prepare(eawr::campaign::GameSession& game, eawr::sim::EntityId actor, const gc::GalacticSetup& config,
    const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes, std::uint64_t sequence = 1) {
    order(game, sequence, gc::PrepareGalacticAbility{actor, config.planets[0].id, gc::AbilityKind::structure_sabotage});
    hashes.push_back(take(game.step(pool)).state_hash.get());
    const auto* state = game.galaxy().ability_state();
    const auto at = std::find_if(state->preparations.begin(), state->preparations.end(), [&](const auto& row) { return row.actor == actor; });
    expect(at != state->preparations.end() && at->open && at->kind == gc::AbilityKind::structure_sabotage, "sabotage preparation is retained and open");
    return gc::prepared_ability(*at, 0);
}
std::vector<eawr::sim::EntityId> structures(const eawr::campaign::GameSession& game) {
    std::vector<eawr::sim::EntityId> ids;
    for (const auto& object : game.galaxy().snapshot()->production()->planets[0].objects) ids.push_back(object.id);
    return ids;
}
Fixed credits(const eawr::campaign::GameSession& game) { return game.galaxy().snapshot()->player_economy()->players[1].credits; }

void cases(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    {   // Choice list, forged capability, restore, success.
        const auto config = setup(); auto game = take(eawr::campaign::GameSession::create(config));
        const auto first = prepare(game, saboteur(config), config, pool, hashes);
        const auto& menu = game.galaxy().ability_state()->preparations.at(0);
        expect(menu.target_choices.size() == 3 && menu.target_choices[0].target == 9001 && menu.target_choices[0].price == f(100)
            && menu.target_choices[1].target == 9002 && menu.target_choices[1].price == f(50)
            && menu.target_choices[2].target == 9003 && menu.target_choices[2].price == f(100),
            "ground specials then space specials, unpriced types omitted, prior count zero");
        expect(first.kind == gc::AbilityKind::structure_sabotage && first.target == 9001 && first.absence == Fixed{} && first.valid,
            "the typed capability carries the structure identity and zero menu absence");
        const auto payload = gc::encode_payload(gc::ExecutePreparedAbility{first});
        expect(gc::decode_payload(gc::CommandLayer::galactic, gc::galactic_opcodes::special_execute, payload)
            == gc::CommandPayload{gc::ExecutePreparedAbility{first}}, "sabotage command round trip");
        auto forged = first; forged.price = f(1);
        order(game, 2, gc::ExecutePreparedAbility{forged}); hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(credits(game) == f(1000) && structures(game).size() == 4 && game.return_state().heroes.empty(), "forged price cannot sabotage or consume");
        const auto identities = cp::game_identities(config); const auto saved = take(cp::save_game_checkpoint(game, identities));
        auto fresh = take(cp::load_game_checkpoint(saved, identities, {}, pool));
        expect(take(cp::save_game_checkpoint(fresh.session, identities)) == saved, "prepared sabotage restores with its prices");
        game = std::move(fresh.session);
        order(game, 3, gc::ExecutePreparedAbility{first}); hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(credits(game) == f(900), "the prepared price is debited once");
        expect((structures(game) == std::vector<eawr::sim::EntityId>{9002, 9003, 9004}), "exactly the chosen structure is removed");
        const auto* state = game.galaxy().ability_state();
        expect(state->sabotage_history.size() == 1 && state->sabotage_history[0].planet == config.planets[0].id && state->sabotage_history[0].count == 1,
            "the planet's sabotage history counts the action");
        expect(state->results.size() == 1 && state->results[0].outcome == gc::AbilityOutcome::purchased
            && state->results[0].kind == gc::AbilityKind::structure_sabotage, "typed purchased result");
        expect(game.return_state().heroes.size() == 1, "the saboteur is consumed through the shared return owner");
        const auto settled = take(cp::save_game_checkpoint(game, identities)); auto restored = take(cp::load_game_checkpoint(settled, identities, {}, pool));
        expect(take(cp::save_game_checkpoint(restored.session, identities)) == settled, "settled sabotage and its history restore");
        hashes.push_back(restored.session.state_sha256());
    }
    {   // A space structure is a target; history escalates the next price (WGCH-28).
        const auto config = setup(true); auto game = take(eawr::campaign::GameSession::create(config));
        const auto first = prepare(game, saboteur(config), config, pool, hashes);
        order(game, 2, gc::ExecutePreparedAbility{first}); hashes.push_back(take(game.step(pool)).state_hash.get());
        const auto next = prepare(game, second_saboteur, config, pool, hashes, 3);
        const auto& menu = game.galaxy().ability_state()->preparations.at(1);
        expect(menu.actor == second_saboteur && menu.target_choices.size() == 2 && menu.target_choices[0].target == 9002
            && menu.target_choices[0].price == f(100) && menu.target_choices[1].target == 9003 && menu.target_choices[1].price == f(200),
            "one prior action doubles every price, rounded to a whole credit");
        auto space = gc::prepared_ability(menu, 1);
        order(game, 4, gc::ExecutePreparedAbility{space}); hashes.push_back(take(game.step(pool)).state_hash.get());
        expect((structures(game) == std::vector<eawr::sim::EntityId>{9002, 9004}) && credits(game) == f(700),
            "the space special leaves with its escalated price");
        expect(game.galaxy().ability_state()->sabotage_history.at(0).count == 2 && next.target == 9002, "history reaches two");
    }
    {   // A missing structure refuses before any debit or consumption (WGCH-29).
        const auto config = setup(); auto game = take(eawr::campaign::GameSession::create(config));
        const auto first = prepare(game, saboteur(config), config, pool, hashes);
        auto stale = first; stale.target = 9999;
        order(game, 2, gc::ExecutePreparedAbility{stale}); hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(credits(game) == f(1000) && structures(game).size() == 4 && game.return_state().heroes.empty()
            && game.galaxy().ability_state()->sabotage_history.empty(), "an unknown structure identity changes nothing");
    }
    {   // WGCH-77: a clean planet is not a sabotage target.
        auto config = setup(); config.economy_corruption->planets[0].corruption = gc::corruption_fixture::Type::none;
        auto game = take(eawr::campaign::GameSession::create(config));
        order(game, 1, gc::PrepareGalacticAbility{saboteur(config), config.planets[0].id, gc::AbilityKind::structure_sabotage});
        hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(game.galaxy().ability_state()->preparations.empty(), "an uncorrupted planet refuses the menu");
    }
    {   // The routing list gates the ability (WGCH-97).
        auto config = setup(); config.ability_host->activated_sabotage_abilities = {"OTHER"};
        auto game = take(eawr::campaign::GameSession::create(config));
        order(game, 1, gc::PrepareGalacticAbility{saboteur(config), config.planets[0].id, gc::AbilityKind::structure_sabotage});
        hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(game.galaxy().ability_state()->preparations.empty(), "an ability outside Activated_Sabotage_Ability_Names is refused");
    }
    {   // The price helper is the fixture's: one shared escalation (WGCH-28).
        expect(take(gc::hero_actions::sabotage_price(f(100), f(2), 3)) == f(800), "escalation is cost times factor to the prior count");
    }
}
void lifecycle(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    const auto config = setup(); auto game = take(eawr::campaign::GameSession::create(config));
    const auto first = prepare(game, saboteur(config), config, pool, hashes);
    take(game.set_ability_lifecycle(saboteur(config), false, true, gc::AbilityKind::structure_sabotage));
    order(game, 2, gc::ExecutePreparedAbility{first}); hashes.push_back(take(game.step(pool)).state_hash.get());
    expect(credits(game) == f(1000) && structures(game).size() == 4, "a cancelled sabotage service refuses execution");
    take(game.set_ability_lifecycle(saboteur(config), true, false, gc::AbilityKind::structure_sabotage));
    order(game, 3, gc::ExecutePreparedAbility{first}); hashes.push_back(take(game.step(pool)).state_hash.get());
    expect(credits(game) == f(900), "a re-enabled service executes the retained preparation");
}
} // namespace
int case_galactic_ability_sabotage_tests() {
    ability_fixture::arm_watchdog();
    try {
        std::vector<std::string> baseline; eawr::platform::ThreadWorkerAdapter one(1); cases(one, baseline); lifecycle(one, baseline);
        for (std::size_t workers : {2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter pool(workers); std::vector<std::string> hashes; cases(pool, hashes); lifecycle(pool, hashes);
            expect(hashes == baseline, "sabotage effects and restore hashes equal on 1/2/4/8");
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
