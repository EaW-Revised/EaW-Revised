#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <vector>
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "ability_fixture.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/ability_host.hpp"
#include "eawr/sim/galactic/economy_corruption.hpp"
#include "eawr/sim/galactic/corruption_actions.hpp"
#include "eawr/sim/galactic/income.hpp"
#include "eawr/sim/tactical/admission.hpp"
#include "../../apps/viewer/src/galactic/ability_drop.hpp"

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
constexpr gc::TypeId unlock = (std::uint64_t{1} << 40) + 17;
gc::GalacticSetup setup(bool mission = false, std::uint32_t mission_mode = 2) {
    auto s = base_setup(); auto& host = *s.ability_host;
    host.profiles[0].has_market = false; host.profiles[0].corruption = gc::CorruptionAbilityProfile{"ACT", true};
    host.activated_corruption_abilities = {"ACT"};
    s.economy_corruption->planets[0].corruption = gc::corruption_fixture::Type::none;
    s.economy_corruption->players[0].benefits = true; // current beneficiary differs from acting owner
    auto& control = ability_fixture::add_control(s); control.types = {{1000, 0, false, false}};
    s.census->types.push_back({3000, {}, 0});
    for (const auto& planet : s.planets) {
        gc::ControlPlanet row; row.planet = planet.id; row.type = 3000; row.retains_residual = true;
        row.surface_accessible = true; row.ground_base = {planet.owner, 1}; row.starbase = {planet.owner, 1}; control.planets.push_back(row);
        gc::CorruptionPlanet p; p.planet = planet.id; p.type = 3000; p.retains_influence = true;
        p.slots[0].type = gc::corruption_fixture::Type::piracy; p.slots[0].price = f(100); p.slots[0].credit_reward = f(25);
        p.slots[0].build_unlock = unlock; p.slots[0].unlock_build_cost = f(50); p.slots[0].tactical_upgrade = unlock + 1;
        if (mission) { p.slots[0].mode = mission_mode; p.slots[0].map = "AUTHORED"; p.slots[0].script = "PLOT"; }
        p.slots[2].type = gc::corruption_fixture::Type::racketeering; p.slots[2].price = f(2000);
        host.corruption_planets.push_back(p);
    }
    for (const gc::PlayerId id : ability_fixture::all_players) {
        gc::CorruptionPlayer player; player.player = id; // the neutral player authors no automatic upgrade
        if (id != ability_fixture::neutral_player) player.automatic_upgrades[2] = unlock + 10 + id;
        host.corruption_players.push_back(player);
    }
    return s;
}
gc::PreparedAbility prepare(eawr::campaign::GameSession& game, const gc::GalacticSetup& config,
    const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    order(game, 1, gc::PrepareGalacticAbility{buyer(config), config.planets[0].id, gc::AbilityKind::corruption});
    hashes.push_back(take(game.step(pool)).state_hash.get());
    expect(game.galaxy().ability_state()->preparations.at(0).corruption_choices.size() == 2, "authored slot gaps and unaffordable corruption choices remain visible");
    return gc::prepared_ability(game.galaxy().ability_state()->preparations.at(0), 0);
}
void direct(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    const auto config = setup(); auto game = take(eawr::campaign::GameSession::create(config));
    const auto prepared = prepare(game, config, pool, hashes);
    expect(gc::decode_payload(gc::CommandLayer::galactic, gc::galactic_opcodes::special_execute,
        gc::encode_payload(gc::ExecutePreparedAbility{prepared})) == gc::CommandPayload{gc::ExecutePreparedAbility{prepared}}, "corruption prepared payload round trips");
    auto forged = prepared; forged.corruption.slot = 2;
    order(game, 2, gc::ExecutePreparedAbility{forged}); hashes.push_back(take(game.step(pool)).state_hash.get());
    expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(1000), "forged corruption choice refuses before debit");
    order(game, 3, gc::ExecutePreparedAbility{prepared}); hashes.push_back(take(game.step(pool)).state_hash.get());
    expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(900) && game.return_state().heroes.size() == 1,
        "direct corruption debits real actor owner and schedules shared default return");
    const auto state = game.galaxy().ability_state()->transitions[0].transition;
    expect(state.increasing && state.end - state.start == 150, "direct corruption uses five logical seconds");
    const auto identities = cp::game_identities(config); const auto saved = take(cp::save_game_checkpoint(game, identities));
    auto fresh = take(cp::load_game_checkpoint(saved, identities, {}, pool));
    expect(take(cp::save_game_checkpoint(fresh.session, identities)) == saved, "fresh reconstruction retains exact transition and shared return state");
    for (unsigned guard = 0; game.galaxy().clock().galactic_frame <= state.end; ++guard) {
        expect(guard < 600, "corruption transition completes within a bounded number of ticks");
        if (guard >= 600) break;
        hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(take(fresh.session.step(pool)).state_hash.get() == hashes.back(), "saved corruption continuation equals uninterrupted state");
    }
    const auto* done = game.galaxy().ability_state();
    expect(!done->transitions[0].transition.active() && done->transitions[0].transition.current == gc::corruption_fixture::Type::piracy,
        "completion clears transition before reward consumers");
    expect(game.galaxy().snapshot()->player_economy()->players[0].credits == f(1025), "current first benefiting faction receives one-time reward rather than acting owner");
    expect(done->planet_entitlements[0].build_unlocks == std::vector<gc::TypeId>{unlock}, "full64 build unlock is planet local");
    expect(game.return_state().upgrades.size() == 3, "both players automatic and beneficiary tactical upgrade reach shared planet-held return ledger");
    const auto cash = game.galaxy().snapshot()->player_economy()->players[0].credits;
    for (int i = 0; i < 5; ++i) (void)take(game.step(pool));
    expect(game.galaxy().snapshot()->player_economy()->players[0].credits == cash, "settled completion never rewards again");
}
// WGCH-09/128: a direct corruption that began notifies the local acting owner with the chosen slot, once, and nobody else.
void direct_voice(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    for (const std::uint32_t local : {2u, 1u}) {
        auto config = setup(); ability_fixture::add_observer(config, local, {1000, 3000});
        auto game = take(eawr::campaign::GameSession::create(config));
        std::vector<gc::GalacticFeedbackNotice> notices;
        game.set_feedback_notice_handler([&](const auto& notice) { notices.push_back(notice); });
        const auto prepared = prepare(game, config, pool, hashes);
        order(game, 2, gc::ExecutePreparedAbility{prepared}); hashes.push_back(take(game.step(pool)).state_hash.get());
        const auto voiced = std::count_if(notices.begin(), notices.end(), [](const auto& n) {
            return n.kind == gc::GalacticFeedbackNotice::Kind::corruption_success_voiced; });
        if (local == 2) {
            expect(voiced == 1, "a local acting owner hears the authored success speech once");
            const auto at = std::find_if(notices.begin(), notices.end(), [](const auto& n) {
                return n.kind == gc::GalacticFeedbackNotice::Kind::corruption_success_voiced; });
            expect(at != notices.end() && at->player == 2 && at->planet == config.planets[0].id && at->slot == prepared.corruption.slot,
                "the notice names the acting owner, the planet and the chosen slot");
        } else expect(voiced == 0, "a non-local acting owner voices nothing");
    }
}
void ai_invocation(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    // Each individual prerequisite can exclude the otherwise affordable piracy slot.
    for (unsigned gate = 0; gate < 5; ++gate) {
        auto config = setup(true); config.economy_corruption->players[1].human = false;
        auto& choice = config.ability_host->corruption_planets[0].slots[0];
        if (gate == 1) choice.price = f(1001);
        if (gate == 2) { choice.required_mission_type = gc::corruption_fixture::Type::bribery; choice.mission_count = 1; }
        if (gate == 3) {
            choice.unit_types = {1000}; choice.unit_count = 2;
            config.ability_host->corruption_prerequisites = {{1000, false, true}};
        }
        if (gate == 4) {
            expect(config.planets[0].owner != 2, "required planet refusal fixture belongs to the actor's enemy");
            config.census->types.push_back({3001, {}, 0});
            config.control->planets[0].type = 3001;
            config.ability_host->corruption_planets[0].type = 3001;
            choice.planet_types = {3001}; // the only active instance belongs to the enemy
        }
        auto game = take(eawr::campaign::GameSession::create(config));
        const auto before = game.galaxy().economy_corruption()->random_draws;
        order(game, 1, gc::PrepareGalacticAbility{buyer(config), config.planets[0].id, gc::AbilityKind::corruption});
        hashes.push_back(take(game.step(pool)).state_hash.get());
        const auto* state = game.galaxy().ability_state();
        if (gate == 0) {
            expect(state->transitions[0].transition.active() && state->missions.empty()
                && state->corruption_usage[1].uses[2] == 1 && game.return_state().heroes.size() == 1,
                "AI preparation executes direct corruption despite authored tactical map, with usage and default return");
            expect(game.galaxy().economy_corruption()->random_draws == before + 1,
                "one eligible AI choice consumes one synchronized selection draw");
            expect(state->preparations.size() == 1 && !state->preparations[0].open,
                "AI settles its retained capability without leaving a human dialog open");
        } else {
            expect(state->preparations.empty() && state->missions.empty() && !state->transitions[0].transition.active()
                && game.return_state().heroes.empty() && game.galaxy().snapshot()->player_economy()->players[1].credits == f(1000),
                "AI no-choice refusal applies credit, mission, unit and planet prerequisites before any debit or consumption");
            expect(game.galaxy().economy_corruption()->random_draws == before, "AI no-choice refusal consumes no selection draw");
        }
        const auto ids = cp::game_identities(config); const auto saved = take(cp::save_game_checkpoint(game, ids));
        auto fresh = take(cp::load_game_checkpoint(saved, ids, {}, pool));
        expect(take(cp::save_game_checkpoint(fresh.session, ids)) == saved, "AI settled/refused state reconstructs exactly");
        hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(take(fresh.session.step(pool)).state_hash.get() == hashes.back(), "AI invocation restore continuation matches");
    }
    bool selected_racketeering = false;
    for (std::uint64_t seed : {1ULL, 23ULL, 28ULL, 43ULL}) {
        auto config = setup(); config.seed = seed; config.economy_corruption->players[1].human = false;
        auto& slots = config.ability_host->corruption_planets[0].slots;
        slots[0].price = slots[2].price = f(100);
        slots[0].planet_types = {9999}; // absent active-galaxy types are omitted
        auto game = take(eawr::campaign::GameSession::create(config));
        auto stream = *game.galaxy().economy_corruption();
        const auto draw = take(gc::economy_draw(stream, config.seed));
        const auto piracy = gc::corruption_actions::ai_score(gc::corruption_fixture::Type::piracy, 0);
        const auto racketeering = gc::corruption_actions::ai_score(gc::corruption_fixture::Type::racketeering, 0);
        const auto threshold = take(eawr::sim::math::multiply(take(eawr::sim::math::add(piracy, racketeering)), draw));
        const auto expected = threshold < piracy ? gc::corruption_fixture::Type::piracy : gc::corruption_fixture::Type::racketeering;
        if (seed != 1) {
            const auto uniform = draw < take(Fixed::from_ratio(1, 2)) ? gc::corruption_fixture::Type::piracy : gc::corruption_fixture::Type::racketeering;
            expect(expected != uniform, "weighted selection fixture distinguishes WGCH-50 scores from a uniform pick");
        }
        selected_racketeering |= expected == gc::corruption_fixture::Type::racketeering;
        order(game, 1, gc::PrepareGalacticAbility{buyer(config), config.planets[0].id, gc::AbilityKind::corruption});
        hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(game.galaxy().ability_state()->transitions[0].transition.target == expected,
            "AI weighted selection uses authored slot order and synchronized seed");
    }
    expect(selected_racketeering, "weighted selection also exercises the later racketeering slot");
}
void human_refusal(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    for (unsigned gate = 0; gate < 3; ++gate) {
        auto config = setup();
        if (gate == 0) config.ability_host->tutorial_corruption_allowed = false;
        if (gate == 1) config.ability_host->activated_corruption_abilities.clear();
        if (gate == 2) config.economy_corruption->planets[0].destroyed = true;
        auto game = take(eawr::campaign::GameSession::create(config));
        order(game, 1, gc::PrepareGalacticAbility{buyer(config), config.planets[0].id, gc::AbilityKind::corruption});
        hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(game.galaxy().ability_state()->preparations.empty() && game.return_state().heroes.empty()
            && game.galaxy().snapshot()->player_economy()->players[1].credits == f(1000),
            "illegal human corruption preparation refuses before debit or hero removal");
    }
}
void missions(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    const auto base = setup(true); auto config = base; ability_fixture::add_observer(config, 2, {1000, 3000});
    auto direct = take(eawr::campaign::GameSession::create(config)); const auto prepared = prepare(direct, config, pool, hashes);
    // WGCH-199: with no live mission service the space mission can't be played, so the choice takes the original's direct
    // route (as AI and multiplayer do) instead of being disabled: offered, charged once, no mission, the timed transition.
    const auto* menu = direct.galaxy().snapshot()->corruption_menu();
    expect(menu && menu->choices[0].mission_available && menu->choices[1].mission_available,
        "a mission choice with no live mission service is still offered (it takes the direct route)");
    order(direct, 2, gc::ExecutePreparedAbility{prepared}); hashes.push_back(take(direct.step(pool)).state_hash.get());
    const auto state = direct.galaxy().ability_state()->transitions[0].transition;
    expect(direct.galaxy().snapshot()->player_economy()->players[1].credits == f(900) && direct.return_state().heroes.size() == 1
        && direct.galaxy().ability_state()->missions.empty() && state.increasing && state.end - state.start == 150
        && direct.galaxy().ability_state()->corruption_usage[1].uses[2] == 1,
        "a space mission with no service debits once, queues no mission, counts one use and starts the five second transition");
    auto world = take(gc::GalacticSession::create(base)); std::vector<gc::HeroReturnRecord> returns;
    world.set_ability_consumption_handler([&](const gc::HeroDefeat& defeat) {
        auto scheduled = gc::schedule_hero_return(returns, defeat, world.clock().galactic_frame, world.snapshot()->planets());
        if (!scheduled) return eawr::core::Result<void>::failure(scheduled.error());
        return world.apply_battle_losses(std::span<const eawr::sim::EntityId>(&defeat.id, 1));
    });
    world.set_corruption_mission_handler([](const auto&, const auto&) { return eawr::core::Result<bool>::success(true); });
    take(world.submit({{0, {2}, 1}, gc::PrepareGalacticAbility{buyer(config), config.planets[0].id, gc::AbilityKind::corruption}}));
    hashes.push_back(take(world.step(pool)).state_hash.get());
    auto chosen = gc::prepared_ability(world.ability_state()->preparations[0], 0);
    take(world.submit({{1, {2}, 2}, gc::ExecutePreparedAbility{chosen}})); hashes.push_back(take(world.step(pool)).state_hash.get());
    expect(returns.size() == 1 && !world.ability_state()->transitions[0].transition.active(), "accepted typed mission consumes actor but waits for actual result");
    const auto request = world.ability_state()->missions[0].request;
    take(world.finish_corruption_mission(request, gc::CorruptionMissionState::succeeded));
    hashes.push_back(take(world.step(pool)).state_hash.get());
    expect(world.ability_state()->transitions[0].transition.active() && world.ability_state()->corruption_usage[1].uses[2] == 1,
        "saved successful mission result begins transition and increments retained recipient usage");
    expect(!world.finish_corruption_mission(request, gc::CorruptionMissionState::succeeded), "duplicate live mission result refuses without journaling a second mutation");
}
// WGCH-198: a solo human land mission choice runs on the timed route (no mission service involved, unlike the space mode).
void land_timed(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    auto config = setup(true, 1); ability_fixture::add_observer(config, 2, {1000, 3000});
    auto game = take(eawr::campaign::GameSession::create(config));
    std::size_t voiced = 0;
    game.set_feedback_notice_handler([&](const auto& notice) {
        if (notice.kind == gc::GalacticFeedbackNotice::Kind::corruption_success_voiced) ++voiced; });
    const auto prepared = prepare(game, config, pool, hashes);
    const auto* menu = game.galaxy().snapshot()->corruption_menu();
    expect(menu && menu->choices[0].mission_available, "a land mission choice is offered available without a mission service");
    order(game, 2, gc::ExecutePreparedAbility{prepared}); hashes.push_back(take(game.step(pool)).state_hash.get());
    const auto state = game.galaxy().ability_state()->transitions[0].transition;
    expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(900) && game.return_state().heroes.size() == 1
        && game.galaxy().ability_state()->missions.empty() && state.increasing && state.end - state.start == 150
        && game.galaxy().ability_state()->corruption_usage[1].uses[2] == 1,
        "land corruption choice debits, queues no mission, counts one use and starts the five second transition");
    expect(voiced == 1, "a timed land choice takes the direct path and voices its success exactly once");
    for (unsigned guard = 0; game.galaxy().clock().galactic_frame <= state.end; ++guard) {
        expect(guard < 600, "land corruption transition completes within a bounded number of ticks");
        if (guard >= 600) break;
        hashes.push_back(take(game.step(pool)).state_hash.get());
    }
    expect(game.galaxy().ability_state()->transitions[0].transition.current == gc::corruption_fixture::Type::piracy
        && game.galaxy().snapshot()->player_economy()->players[0].credits == f(1025), "land corruption choice reaches its timed outcome");
}
void stale_units(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    auto config = setup(); auto& choice = config.ability_host->corruption_planets[0].slots[0];
    choice.unit_types = {1000}; choice.unit_count = 2; config.ability_host->corruption_prerequisites = {{1000, true, true}};
    auto game = take(eawr::campaign::GameSession::create(config)); const auto prepared = prepare(game, config, pool, hashes);
    order(game, 2, gc::ExecutePreparedAbility{prepared}); hashes.push_back(take(game.step(pool)).state_hash.get());
    expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(900) && game.return_state().heroes.empty(),
        "stale missing unit refusal is after debit and before actor consumption");
}
void representative(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    auto config = setup();
    // Alternative A exists only for the enemy. Eligibility passes via owned B;
    // the representative getter still chooses A and must not consume B.
    config.fleets[1].members[0].type = 1001;
    for (auto& row : config.census->objects) if (row.id == buyer(config)) row.original_type = row.current_type = 1001;
    config.census->types.insert(config.census->types.begin() + 1, {1001, f(1), 0});
    config.fleet_control->types.push_back({1001, false, true, false});
    config.control->types.push_back({1001, 0, false, false});
    auto profile = config.ability_host->profiles[0]; profile.type = 1001; config.ability_host->profiles.push_back(profile);
    auto& choice = config.ability_host->corruption_planets[0].slots[0]; choice.unit_types = {1000, 1001}; choice.unit_count = 1;
    config.ability_host->corruption_prerequisites = {{1000, true, false}, {1001, true, false}};
    auto game = take(eawr::campaign::GameSession::create(config)); const auto prepared = prepare(game, config, pool, hashes);
    order(game, 2, gc::ExecutePreparedAbility{prepared}); hashes.push_back(take(game.step(pool)).state_hash.get());
    expect(game.galaxy().ability_state()->transitions[0].transition.active() && game.return_state().heroes.size() == 1,
        "pooled eligibility and global representative selection remain separate; enemy alternative does not redirect consumption to owned B");
}
void unsuccessful_missions(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    for (const auto result : {gc::CorruptionMissionState::failed, gc::CorruptionMissionState::cancelled}) {
        const auto config = setup(true); auto world = take(gc::GalacticSession::create(config));
        world.set_ability_consumption_handler([&](const gc::HeroDefeat& defeat) {
            return world.apply_battle_losses(std::span<const eawr::sim::EntityId>(&defeat.id, 1));
        });
        world.set_corruption_mission_handler([](const auto&, const auto&) { return eawr::core::Result<bool>::success(true); });
        take(world.submit({{0, {2}, 1}, gc::PrepareGalacticAbility{buyer(config), config.planets[0].id, gc::AbilityKind::corruption}}));
        hashes.push_back(take(world.step(pool)).state_hash.get());
        const auto prepared = gc::prepared_ability(world.ability_state()->preparations[0], 0);
        take(world.submit({{1, {2}, 2}, gc::ExecutePreparedAbility{prepared}})); hashes.push_back(take(world.step(pool)).state_hash.get());
        take(world.finish_corruption_mission(world.ability_state()->missions[0].request, result));
        hashes.push_back(take(world.step(pool)).state_hash.get());
        expect(world.ability_state()->missions[0].state == result && !world.ability_state()->transitions[0].transition.active()
            && world.ability_state()->corruption_usage[1].uses[2] == 0, "failure and cancellation settle without corruption or mission use credit");
    }
}
void startup(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    auto config = setup(); auto& slot = config.ability_host->corruption_planets[0].slots[2]; slot.credit_reward = f(13);
    config.ability_host->corruption_players[0].automatic_upgrades[4] = unlock + 20;
    config.ability_host->corruption_players[1].automatic_upgrades[4] = unlock + 21;
    config.ability_host->initial_corruption = {{config.planets[0].id, gc::corruption_fixture::Type::racketeering, 2}};
    auto game = take(eawr::campaign::GameSession::create(config));
    expect(game.galaxy().economy_corruption()->planets[0].corruption == gc::corruption_fixture::Type::racketeering
        && game.galaxy().snapshot()->player_economy()->players[1].credits == f(1013), "startup uses supplied owner recipient rather than ordinary first beneficiary");
    expect(!game.galaxy().ability_state()->transitions[0].transition.active() && game.return_state().upgrades.size() == 2,
        "startup installs immediate corruption and all-player planet upgrades without a timer");
    const auto ids = cp::game_identities(config); const auto saved = take(cp::save_game_checkpoint(game, ids));
    auto restored = take(cp::load_game_checkpoint(saved, ids, {}, pool));
    expect(take(cp::save_game_checkpoint(restored.session, ids)) == saved, "zero-tick initialization reconstructs exactly once");
    hashes.push_back(take(game.step(pool)).state_hash.get());
}
void observer_idle() {
    eawr::sim::InlineExecutor pool; auto config = setup();
    ability_fixture::add_observer(config, 2, {1000, 3000});
    auto game = take(eawr::campaign::GameSession::create(config)); std::vector<std::string> unused;
    const auto prepared = prepare(game, config, pool, unused); const auto* menu = game.galaxy().snapshot()->corruption_menu();
    expect(menu && menu->choices[0].live_prepared == prepared && menu->choices[0].live_choice->build_unlock == unlock,
        "observer-local corruption publication retains full64 exact capability and authored reward IDs");
    order(game, 2, gc::PauseGalaxy{}); (void)take(game.step(pool)); menu = game.galaxy().snapshot()->corruption_menu();
    const auto work = game.galaxy().ability_work(); const auto frame = game.galaxy().clock().galactic_frame;
    for (unsigned i = 0; i < 10000; ++i) {
        (void)take(game.step(pool)); expect(game.galaxy().snapshot()->corruption_menu() == menu, "idle corruption menu shares one immutable allocation");
    }
    expect(game.galaxy().ability_work() == work && game.galaxy().clock().galactic_frame == frame,
        "paused idle ticks do not scan ability inputs, rebuild menus or service transition time");
    config.observer->human_observers = {1}; config.observer->solo_reference_human = 1;
    auto foreign = take(eawr::campaign::GameSession::create(config)); (void)prepare(foreign, config, pool, unused);
    expect(!foreign.galaxy().snapshot()->corruption_menu(), "foreign observer never receives an actor's corruption capability");
    {
        auto multiplayer = config;
        multiplayer.observer->human_observers = {1, 2}; multiplayer.observer->solo_reference_human = 0;
        auto shared = take(eawr::campaign::GameSession::create(multiplayer));
        (void)prepare(shared, multiplayer, pool, unused);
        const auto host = shared.galaxy().view_snapshot({1});
        const auto guest = shared.galaxy().view_snapshot({2});
        expect(!host->corruption_menu() && guest->corruption_menu() && guest->corruption_menu()->player == 2,
            "seat-local corruption capability belongs only to the acting guest");
        expect(shared.galaxy().view_snapshot({2})->corruption_menu() == guest->corruption_menu()
            && !shared.galaxy().view_snapshot({1})->corruption_menu(), "corruption menu caches remain isolated across seat switches");
    }
    config.observer->human_observers = {2}; config.observer->solo_reference_human = 2;
    auto visible = take(eawr::campaign::GameSession::create(config));
    const auto choice = prepare(visible, config, pool, unused);
    order(visible, 2, gc::ExecutePreparedAbility{choice}); (void)take(visible.step(pool));
    const auto publication = visible.galaxy().snapshot();
    const auto transitions = publication->corruption_transitions();
    const auto at = std::find_if(transitions.begin(), transitions.end(), [&](const auto& row) { return row.planet == config.planets[0].id; });
    expect(at != transitions.end() && at->transition == visible.galaxy().ability_state()->transitions[0].transition,
        "WGP-47 publication retains exact committed current/target/flags and logical deadlines");
    const auto bytes = publication->canonical_bytes();
    const auto native = gc::GalacticSnapshot::native_view(publication, {});
    expect(native->corruption_transitions().data() == transitions.data() && publication->canonical_bytes() == bytes,
        "read-only native view borrows transition storage without changing canonical bytes");
    gc::NativeSnapshotChanges other_view; other_view.observer = *publication->observer();
    other_view.observer->human_observers = {1}; other_view.observer->solo_reference_human = 1;
    const auto foreign_view = gc::GalacticSnapshot::native_view(publication, std::move(other_view));
    expect(foreign_view->corruption_transitions().empty(),
        "observer replacement never borrows another player's admitted transition facts");
    const auto stacked_foreign = gc::GalacticSnapshot::native_view(foreign_view, {});
    expect(stacked_foreign->observer() && stacked_foreign->observer()->solo_reference_human == 1
        && stacked_foreign->corruption_transitions().empty(),
        "flattened native views retain inherited observer admission for corruption transitions");
    const auto stacked_local = gc::GalacticSnapshot::native_view(native, {});
    expect(stacked_local->corruption_transitions().data() == transitions.data(),
        "stacked unchanged native views share the admitted transition allocation");
    order(visible, 3, gc::PauseGalaxy{}); (void)take(visible.step(pool));
    const auto* storage = visible.galaxy().snapshot()->corruption_transitions().data();
    for (unsigned i = 0; i < 10; ++i) {
        (void)take(visible.step(pool));
        expect(visible.galaxy().snapshot()->corruption_transitions().data() == storage,
            "idle transitions share one publication allocation");
    }
    const auto ids = cp::game_identities(config);
    const auto save = take(cp::save_game_checkpoint(visible, ids));
    auto loaded = take(cp::load_game_checkpoint(save, ids, {}, pool));
    const auto restored = loaded.session.galaxy().snapshot()->corruption_transitions();
    expect(std::equal(restored.begin(), restored.end(), visible.galaxy().snapshot()->corruption_transitions().begin(),
        visible.galaxy().snapshot()->corruption_transitions().end()), "restored committed transition republishes unchanged");
}
namespace tactical = eawr::sim::tactical;
namespace campaign = eawr::campaign;
// One unit per side on a bare authored space map; the invader wins by destroying the defender's unit.
tactical::TacticalContent mission_content() {
    tactical::TacticalContent c;
    c.motion.rules.arc_degrees = f(30); c.motion.rules.expansion_distance = f(10);
    c.motion.profiles.push_back({1000, f(1), f(1), f(1), f(1), f(1)});
    tactical::DurabilityProfile durability; durability.type_id = 1000; durability.max_hull = f(100);
    c.durability.profiles.push_back(durability);
    c.victory.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    c.victory.relevant_types = {1000}; c.victory.contenders = {1, 2}; c.victory.controlled_players = {1, 2};
    return c;
}
campaign::ConflictRecord mission_conflict(const gc::LiveCorruptionMission& mission, const gc::CorruptionChoice& choice, eawr::sim::EntityId id) {
    campaign::ConflictRecord c; c.id = id; c.planet = mission.planet; c.invader = mission.player; c.defender = 1;
    c.layer = campaign::BattleLayer::space; c.corruption_mission = true; c.map_identity = choice.map;
    c.seats = {{{1}, 1}, {{2}, 2}};
    c.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
    for (const std::uint32_t player : {2U, 1U}) {
        tactical::UnitState unit; unit.entity_id = id + (player == 2 ? 1 : 2); unit.type_id = 1000; unit.owner = player;
        c.tactical.units.push_back(unit);
    }
    c.tactical.first_free_entity_id = id + 3;
    return c;
}
gc::TacticalLayerCommand damage_order(const campaign::ConflictRecord& c, eawr::sim::EntityId target) {
    tactical::TacticalReplay replay; replay.setup.players = c.tactical.players;
    replay.final_tick_count = 1;
    replay.commands.push_back({{0, 2, 1}, {target}, tactical::DamagePayload{f(1000)}});
    const auto bytes = take(tactical::write_replay(replay));
    const auto header = static_cast<std::size_t>(bytes[10]) | (static_cast<std::size_t>(bytes[11]) << 8U);
    const auto start = header + replay.setup.players.size() * 24 + 4;
    return {c.id, bytes[start + 20], {bytes.begin() + static_cast<std::ptrdiff_t>(start), bytes.end()}};
}
// Composed GameSession path: accepted space mission -> child battle -> checkpoint while the child runs ->
// fresh reconstruction -> result -> corruption transition, usage and shared return state.
void accepted_space_mission(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    const auto config = setup(true);
    const auto install = [&](campaign::GameSession& game) {
        game.set_conflict_service([](const auto&, const auto&, auto) {
            return eawr::core::Result<std::optional<campaign::RecordedAdmission>>::success({});
        }, [](const campaign::ConflictRecord&) { return eawr::core::Result<tactical::TacticalContent>::success(mission_content()); }, {}, eawr::campaign::ScriptRequirement::none);
        game.set_corruption_mission_service([](const auto&, const auto& mission, const auto& choice, auto next) {
            campaign::Arbitration arbitration; arbitration.route = campaign::ConflictRoute::tactical; arbitration.forced_tactical = true;
            return eawr::core::Result<std::optional<campaign::RecordedAdmission>>::success(
                campaign::RecordedAdmission{0, mission_conflict(mission, choice, next), arbitration});
        });
    };
    auto game = take(campaign::GameSession::create(config)); install(game);
    const auto prepared = prepare(game, config, pool, hashes);
    order(game, 2, gc::ExecutePreparedAbility{prepared}); hashes.push_back(take(game.step(pool)).state_hash.get());
    // The queued admission reaches the conflict tail on a following tick; the forced route then starts the child.
    for (unsigned wait = 0; !game.battle() && wait < 10; ++wait) hashes.push_back(take(game.step(pool)).state_hash.get());
    if (!game.battle()) {
        std::cerr << "mission child absent: missions=" << game.galaxy().ability_state()->missions.size()
            << " pending=" << (game.pending_conflict() != nullptr) << " heroes=" << game.return_state().heroes.size() << std::endl;
    }
    expect(game.battle() && game.galaxy().ability_state()->missions.size() == 1
        && game.galaxy().ability_state()->missions[0].state == gc::CorruptionMissionState::queued,
        "accepted space mission starts the authored child and keeps the request open");
    expect(game.return_state().heroes.size() == 1 && !game.galaxy().ability_state()->transitions[0].transition.active(),
        "mission consumes the actor into the shared return state without starting the transition");
    const auto ids = cp::game_identities(config); const auto saved = take(cp::save_game_checkpoint(game, ids));
    cp::GameLoadOptions load; load.script_requirement = eawr::campaign::ScriptRequirement::none; load.configure_session = [&](campaign::GameSession& session) { install(session); return eawr::core::Result<void>::success(); };
    load.content = [](const campaign::ConflictRecord&) { return eawr::core::Result<tactical::TacticalContent>::success(mission_content()); };
    auto fresh = take(cp::load_game_checkpoint(saved, ids, load, pool));
    expect(fresh.session.battle() && take(cp::save_game_checkpoint(fresh.session, ids)) == saved,
        "fresh reconstruction retains the running mission child and request bit for bit");
    if (!game.battle() || !game.conflict()) return;
    const auto conflict = *game.conflict();
    const auto target = conflict.tactical.units[1].entity_id;
    const std::uint64_t first = game.completed_tick();
    for (auto* session : {&game, &fresh.session})
        take(session->submit({{session->completed_tick(), {2}, 3}, damage_order(conflict, target)}));
    for (unsigned guard = 0; game.battle() && guard < 2000; ++guard) {
        hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(take(fresh.session.step(pool)).state_hash.get() == hashes.back(), "saved mission child continuation equals uninterrupted state");
    }
    expect(!game.battle() && !fresh.session.battle() && game.completed_tick() > first, "mission child ended and returned to the galaxy");
    for (auto* session : {&game, &fresh.session}) {
        const auto* done = session->galaxy().ability_state();
        expect(done->missions[0].state == gc::CorruptionMissionState::succeeded && done->transitions[0].transition.active()
            && done->corruption_usage[1].uses[2] == 1,
            "child victory finishes the saved request, begins the transition and credits the retained recipient once");
        expect(session->return_state().heroes.size() == 1, "mission return leaves the consumed actor on its default return");
    }
}
// #7466 review F1: a stale preparation whose target planet now belongs to the actor's owner. The mission battle would have
// the same invader and defender, which the conflict queue refuses, and that refusal is not one of the original's
// (WGCH-127). So the probe sends the choice down the direct route before the debit (WGCH-199): charged once, no
// refusal, the timed transition. Before #7466's fix it was charged and then refused.
void stale_owned_target(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    auto config = setup(true); ability_fixture::add_observer(config, 2, {1000, 3000});
    auto game = take(campaign::GameSession::create(config));
    game.set_conflict_service([](const auto&, const auto&, auto) {
        return eawr::core::Result<std::optional<campaign::RecordedAdmission>>::success({});
    }, [](const campaign::ConflictRecord&) { return eawr::core::Result<tactical::TacticalContent>::success(mission_content()); }, {}, eawr::campaign::ScriptRequirement::none);
    // As the stock service: the defender is the target planet's current owner.
    game.set_corruption_mission_service([](const gc::GalacticSnapshot& snapshot, const auto& mission, const auto& choice, auto next) {
        auto record = mission_conflict(mission, choice, next);
        for (const auto& planet : snapshot.planets()) if (planet.id == mission.planet) record.defender = planet.owner;
        campaign::Arbitration arbitration; arbitration.route = campaign::ConflictRoute::tactical; arbitration.forced_tactical = true;
        return eawr::core::Result<std::optional<campaign::RecordedAdmission>>::success(
            campaign::RecordedAdmission{0, std::move(record), arbitration});
    });
    const auto prepared = prepare(game, config, pool, hashes);
    gc::PlayerId owner{};
    for (const auto& actor : game.galaxy().snapshot()->actor_records()) if (actor.id == prepared.actor) owner = actor.owner;
    expect(owner != 0, "the stale-preparation fixture finds the actor's owner");
    // The player takes the planet (here by the debug owner command) while the corruption menu is still open.
    order(game, 2, gc::DebugSetPlanetOwner{config.planets[0].id, owner}); hashes.push_back(take(game.step(pool)).state_hash.get());
    const auto before = game.galaxy().snapshot()->player_economy()->players[1].credits;
    order(game, 3, gc::ExecutePreparedAbility{prepared});
    const auto step = take(game.step(pool)); hashes.push_back(step.state_hash.get());
    for (unsigned wait = 0; wait < 3; ++wait) hashes.push_back(take(game.step(pool)).state_hash.get());
    const auto state = game.galaxy().ability_state()->transitions[0].transition;
    expect(before == f(1000) && game.galaxy().snapshot()->player_economy()->players[1].credits == f(900)
        && game.galaxy().ability_state()->missions.empty() && !game.battle() && !game.pending_conflict()
        && state.increasing && game.galaxy().ability_state()->corruption_usage[1].uses[2] == 1,
        "a stale preparation against the actor's own planet takes the direct route: charged once, no mission, the transition runs");
}
void removal(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    for (unsigned variant = 0; variant < 15; ++variant) {
        auto config = setup(); const auto target = config.planets[0].id, actor = buyer(config);
        config.planets[0].owner = 2;
        config.control->planets[0].ground_base.owner = config.control->planets[0].starbase.owner = 2;
        config.production->planets[0].target_owner = 2;
        config.economy_corruption->planets[0].corruption = gc::corruption_fixture::Type::black_market;
        auto& profile = config.ability_host->profiles[0]; profile.corruption.reset();
        profile.removal.emplace(); profile.removal->name = "MOD_REMOVE"; profile.removal->text_identity = "ModHero";
        profile.removal->hero = true; profile.removal->cost_multiplier = variant == 1 ? f(2) : Fixed::from_ratio(1, 2).value();
        profile.removal->seconds = variant == 2 ? f(2) : f(1);
        profile.removal->causes_despawn = variant == 3;
        profile.ground_activated = {gc::AbilityKind::remove_corruption};
        config.ability_host->activated_removal_abilities = {"MOD_REMOVE"};
        config.ability_host->corruption_planets[0].removal_base_cost = f(100);
        if (variant == 4) profile.removal->initially_enabled = false;
        if (variant == 5) profile.ground_activated.clear();
        if (variant == 6) config.ability_host->corruption_planets[0].removal_base_cost = f(3000);
        if (variant == 7) config.economy_corruption->planets[0].corruption = gc::corruption_fixture::Type::none;
        if (variant == 8) config.ability_host->corruption_planets[0].removal_base_cost = f(-100);
        if (variant == 10) {
            config.planets[0].owner = 1;
            config.control->planets[0].ground_base.owner = config.control->planets[0].starbase.owner = 1;
            config.production->planets[0].target_owner = 1;
        }
        if (variant == 11) profile.removal->cost_multiplier = {};
        if (variant == 12) config.ability_host->activated_removal_abilities.clear();
        if (variant == 13) {
            config.economy_corruption->planets[0].corruption = gc::corruption_fixture::Type::militia;
            profile.removal->causes_despawn = true;
        }
        if (variant == 14) {
            // WGCH-17: a militia planet with a starbase, a special political source, a political modifier, an enemy
            // fleet in orbit and an enemy force landed; the removal's completion runs the revolt.
            config.economy_corruption->planets[0].corruption = gc::corruption_fixture::Type::militia;
            ability_fixture::add_movement(config);
            config.fleets[0].orbit = target;
            for (auto& object : config.census->objects) if (object.container == config.fleets[0].id) object.planet = target;
            auto& presence = config.control->planets[0];
            presence.starbase = {2, 3}; presence.special_control = {f(1)};
            presence.transition_modifiers.push_back({7, 99, Fixed::from_ratio(1, 2).value(), 0});
        }
        // The viewer reads observer-local service rows, not the raw host state.
        ability_fixture::add_observer(config, 2, {1000, 3000});
        auto game = take(eawr::campaign::GameSession::create(config));
        unsigned land_notices{}, deploy_notices{};
        game.set_story_notice_handler([&](const eawr::campaign::GameStoryNotice& notice) {
            if (const auto* travel = std::get_if<gc::story::TravelNotice>(&notice.payload);
                travel && (travel->kind == gc::story::Travel::land || travel->kind == gc::story::Travel::deploy_hero)) {
                expect(notice.player == gc::story::Player{2} && notice.observer_planet == target && travel->fleet.owner == gc::story::Player{2}
                    && travel->fleet.types == std::vector<std::string>{"ModHero"}
                    && travel->fleet.heroes == std::vector<std::string>{"ModHero"} && travel->hero == "ModHero",
                    "removal emits exact actor-owner land and hero-deployment notices through GameSession");
                if (travel->kind == gc::story::Travel::land) ++land_notices;
                else ++deploy_notices;
            }
            return eawr::core::Result<void>::success();
        });
        if (variant == 9) {
            take(game.set_ability_lifecycle(actor, true, true, gc::AbilityKind::remove_corruption));
            hashes.push_back(take(game.step(pool)).state_hash.get()); // publish the lifecycle change before preview
        }
        const auto preview = eawr::viewer::galactic::ability_drop_candidate(*game.galaxy().snapshot(), 2,
            config.fleets[1].id, *config.ability_host);
        expect(bool(preview) == (variant < 4 || variant == 6 || variant == 8 || variant == 11 || variant >= 13),
            "shared viewer slot admits authored enabled removal on a corrupted owned target independently of price");
        order(game, 1, gc::DeployGroundAbility{actor, target});
        hashes.push_back(take(game.step(pool)).state_hash.get());
        const bool success = variant < 4 || variant == 8 || variant == 11 || variant == 12 || variant >= 13;
        const auto price = variant == 1 ? f(200) : variant == 8 || variant == 11 ? Fixed{} : f(50);
        expect(game.galaxy().snapshot()->player_economy()->players[1].credits == take(eawr::sim::math::subtract(f(1000), success ? price : Fixed{})),
            "removal uses authored base times multiplier, clamps negative cost and never charges a refusal");
        expect(land_notices == (success ? 1U : 0U) && deploy_notices == (success ? 1U : 0U)
            && game.return_state().heroes.size() == (variant == 3 || variant == 13 ? 1U : 0U),
            "removal notifies and consumes only on successful activation with the authored flag");
        const auto state = game.galaxy().ability_state()->transitions[0].transition;
        expect(state.active() == success && (!success || (!state.increasing && state.decreasing
            && state.end - state.start == (variant == 2 ? 60U : 30U))), "authored removal seconds control the transition deadline");
        expect(std::count_if(game.galaxy().snapshot()->events().begin(), game.galaxy().snapshot()->events().end(),
            [&](const auto& event) { return event.kind == gc::GalacticEventKind::corruption_removal_started
                && event.subject == target && event.value == profile.type; }) == (success ? 1 : 0),
            "successful removal publishes one typed source/target event even when the actor is consumed");
        if (variant >= 13) {
            // #6257: the corrupt-militia removal is no longer refused; it starts like any other removal.
            expect(game.galaxy().snapshot()->economy_corruption()->planets[0].corruption == gc::corruption_fixture::Type::militia
                && game.galaxy().ability_state()->transitions[0].transition.decreasing
                && game.galaxy().snapshot()->player_economy()->players[1].credits == take(eawr::sim::math::subtract(f(1000), f(50))),
                "militia removal starts its transition and charges the authored price");
        }
        const auto ids = cp::game_identities(config);
        const auto saved = take(cp::save_game_checkpoint(game, ids));
        auto fresh = take(cp::load_game_checkpoint(saved, ids, {}, pool));
        expect(take(cp::save_game_checkpoint(fresh.session, ids)) == saved, "removal profile and lifecycle state restore byte for byte");
        for (unsigned tick = 0; tick < 70; ++tick) {
            hashes.push_back(take(game.step(pool)).state_hash.get());
            expect(take(fresh.session.step(pool)).state_hash.get() == hashes.back(), "saved removal continuation equals uninterrupted state");
        }
        expect(!success || game.galaxy().snapshot()->economy_corruption()->planets[0].corruption == gc::corruption_fixture::Type::none,
            "successful removal finishes through the existing corruption completion owner");
        if (variant >= 13) {
            // WGCH-17: the completed militia removal cleared the starbase, special and political sources, the enemy
            // fleet left orbit, and allegiance went neutral at 1.
            const auto* control = game.galaxy().snapshot()->control();
            const auto& presence = control->planets[0];
            expect(presence.starbase.level == 0 && presence.special_control.empty() && presence.transition_modifiers.empty(),
                "militia revolt clears the starbase, special structures and political modifiers");
            const auto& allegiance = control->state[0];
            expect(allegiance.target_player == ability_fixture::neutral_player && allegiance.current_player == ability_fixture::neutral_player
                && allegiance.current > Fixed::from_ratio(9, 10).value(), "militia revolt processes neutral allegiance at 1");
            if (variant == 14) {
                const auto fleet = std::find_if(game.galaxy().snapshot()->fleets().begin(), game.galaxy().snapshot()->fleets().end(),
                    [&](const auto& row) { return row.id == config.fleets[0].id; });
                expect(fleet != game.galaxy().snapshot()->fleets().end() && fleet->orbit != target,
                    "militia revolt sends each orbiting owner's fleet away from the planet");
            }
        }
    }
}
} // namespace
// #7426: the corruption menu is published only for a planet the human player has discovered (WGUI-99), and a lone stealth hero
// does not discover the planet it orbits (WGUI-01). The host refuses such a preparation with a reason the seat is told
// instead of holding an open preparation nobody can see or cancel; a discovered planet still opens the menu, and an AI
// owner, which chooses without a published menu, is not gated. In multiplayer each human's own discovery decides, never the
// other human's. Variants: 0 solo undiscovered, 1 solo discovered, 2 AI undiscovered, 3 AI discovered, 4 multiplayer with
// only the other human discovering it, 5 multiplayer with only the acting human discovering it.
void undiscovered_target(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    std::optional<gc::corruption_fixture::Type> ai_target[2];
    for (unsigned variant = 0; variant < 6; ++variant) {
        const bool ai = variant == 2 || variant == 3, multiplayer = variant >= 4;
        auto config = setup();
        auto& observer = ability_fixture::add_observer(config, ai ? 1 : 2, {1000, 3000});
        if (multiplayer) { observer.human_observers = {1, 2}; observer.solo_reference_human = 0; }
        const auto planet = config.planets[0].id;
        for (auto& record : observer.records) if (record.planet == planet) {
            if (record.player == 2) record.discovered = variant == 1 || variant == 3 || variant == 5;
            if (record.player == 1 && multiplayer) record.discovered = variant == 4;
        }
        if (ai) config.economy_corruption->players[1].human = false;
        auto game = take(eawr::campaign::GameSession::create(config));
        order(game, 1, gc::PrepareGalacticAbility{buyer(config), config.planets[0].id, gc::AbilityKind::corruption});
        hashes.push_back(take(game.step(pool)).state_hash.get());
        const auto snapshot = game.galaxy().snapshot();
        const auto events = snapshot->events();
        const bool refused = std::any_of(events.begin(), events.end(), [](const auto& event) {
            return event.kind == gc::GalacticEventKind::command_rejected
                && event.reason == gc::GalacticRejectReason::ability_target_undiscovered; });
        const auto* state = game.galaxy().ability_state();
        // The acting human's own menu: in multiplayer its seat's view, in solo the local snapshot.
        const bool menu_shown = multiplayer ? game.galaxy().view_snapshot({2})->corruption_menu() != nullptr
                                            : snapshot->corruption_menu() != nullptr;
        if (variant == 0 || variant == 4) {
            expect(refused && state->preparations.empty() && !menu_shown
                && game.galaxy().snapshot()->player_economy()->players[1].credits == f(1000),
                variant == 0 ? "a human corruption preparation on an undiscovered planet is refused as undiscovered, with no hidden open menu"
                    : "in multiplayer the acting human's own discovery decides: another human's discovery does not admit it");
        } else if (variant == 1 || variant == 5) {
            expect(!refused && state->preparations.size() == 1 && state->preparations[0].open && menu_shown,
                variant == 1 ? "a discovered planet still opens the corruption menu"
                    : "in multiplayer the acting human's discovery opens its menu although the other human has not discovered the planet");
        } else {
            expect(!refused && state->transitions[0].transition.active() && state->preparations.size() == 1 && !state->preparations[0].open,
                "an AI owner chooses and settles without a published menu, discovered or not");
            ai_target[variant - 2] = state->transitions[0].transition.target;
        }
    }
    expect(ai_target[0] && ai_target[0] == ai_target[1], "an AI's corruption choice is the same with or without its discovery");
}
int case_galactic_ability_corruption_tests() {
    ability_fixture::arm_watchdog();
    try {
        using Phase = void (*)(const eawr::sim::PartitionExecutor&, std::vector<std::string>&);
        const Phase phases[] = {direct, direct_voice, ai_invocation, human_refusal, undiscovered_target, missions, land_timed, stale_units, representative, unsuccessful_missions, accepted_space_mission, stale_owned_target, startup, removal};
        std::vector<std::string> baseline;
        for (std::size_t count : {1U, 2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter workers(count); std::vector<std::string> hashes;
            for (const Phase phase : phases) phase(workers, hashes);
            if (count == 1) baseline = hashes;
            else expect(hashes == baseline, "corruption command, transition, reward and restore hashes equal on 1/2/4/8");
        }
        observer_idle();
    } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; }
    return galactic_test::failures ? 1 : 0;
}
