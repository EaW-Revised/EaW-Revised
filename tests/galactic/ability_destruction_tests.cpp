#include "ability_ruins_support.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/hero_action_views.hpp"

#include <algorithm>

// WGCH-58/113..115/137/138/155..158/176..179/183..185: live strategic destruction on the ability host.
namespace {
using namespace ability_fixture;
constexpr gc::TypeId weapon_type = 5000;
eawr::sim::EntityId bearer(const gc::GalacticSetup& s) { return s.fleets[1].members[0].id; }
void order(eawr::campaign::GameSession& game, std::uint64_t sequence, gc::CommandPayload payload) {
    take(game.submit({{game.completed_tick(), {2}, sequence}, std::move(payload)}));
}
// Player 2's fleet member in orbit at planet 0 authors the destruction ability; planet 0 belongs to player 1,
// whose own fleet (type 1000, nonstealth) also orbits it.
gc::GalacticSetup destroy_setup() {
    auto s = ruins_setup(false); auto& host = *s.ability_host; const auto planet = s.planets[0].id;
    s.fleets[0].orbit = planet; s.fleets[1].orbit = planet;
    s.fleets[1].members[0].type = weapon_type;
    for (auto& row : s.census->objects) if (row.id == bearer(s)) row.original_type = row.current_type = weapon_type;
    register_type(s, weapon_type);
    gc::GalacticAbilityProfile profile; profile.type = weapon_type;
    profile.destruction = gc::DestructionAbilityProfile{"DESTROY", true};
    host.profiles.push_back(profile);
    std::sort(host.profiles.begin(), host.profiles.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    host.activated_destruction_abilities = {"DESTROY"};
    return s;
}
// Player 1's fleet member becomes a skipping (stealthy) actor: the contested-orbit refusal reads the actor census
// (WGM-163), never a type table, so a nonblocking presence is declared on the live actor itself.
void stealth_fleet(gc::GalacticSetup& s) {
    for (auto& row : s.census->objects)
        if (row.id == s.fleets[0].members[0].id) { row.abilities.present = true; row.abilities.combat_skip = true; }
}
gc::PreparedAbility direct(const gc::GalacticSetup& config) {
    gc::PreparedAbility request; request.actor = bearer(config); request.planet = config.planets[0].id;
    request.kind = gc::AbilityKind::planet_destruction; return request;
}
void command_round_trip() {
    const auto request = direct(destroy_setup());
    expect(gc::decode_payload(gc::CommandLayer::galactic, gc::galactic_opcodes::special_execute,
        gc::encode_payload(gc::ExecutePreparedAbility{request})) == gc::CommandPayload{gc::ExecutePreparedAbility{request}},
        "the typed destruction payload carries only the supplied object and planet and round trips");
}
void destroyed(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    auto config = destroy_setup();
    // Stealthy nonallied presence and allied presence never refuse (WGCH-158).
    stealth_fleet(config); // player 1's orbital fleet skips combat: it cannot block
    auto game = take(eawr::campaign::GameSession::create(config));
    order(game, 1, gc::ExecutePreparedAbility{direct(config)}); hashes.push_back(take(game.step(pool)).state_hash.get());
    const auto& world = game.galaxy(); const auto snapshot = world.snapshot();
    expect(snapshot->control()->planets[0].destroyed && snapshot->economy_corruption()->planets[0].destroyed,
        "strategic destruction applies the shared ruins request");
    expect(present(world, bearer(config)) && snapshot->player_economy()->players[1].credits == f(1000),
        "WGCH-115: the actor survives and no credit moves");
    expect(snapshot->control()->planets[0].starbase.level == 1 && snapshot->production()->planets[0].space_slots == 3
        && snapshot->control()->state[0].current_player == 1 && !snapshot->fleets().empty(),
        "orbit fleets, starbase and ownership are preserved (hacking-only operations are absent)");
    expect(game.return_state().heroes.size() == 1 && game.return_state().heroes[0].defeated == hero,
        "the landed named hero requests its respawn through the shared owner");
    expect(world.ability_state()->results.at(0).kind == gc::AbilityKind::planet_destruction
        && world.ability_state()->results.at(0).outcome == gc::AbilityOutcome::purchased, "typed completed result");
    const auto first = world.ability_state()->results.at(0); const auto next_request = world.ability_state()->next_request;
    order(game, 2, gc::ExecutePreparedAbility{direct(config)});
    const auto second = take(game.step(pool)); hashes.push_back(second.state_hash.get());
    const auto after = game.galaxy().snapshot(); const auto events = after->events();
    expect(game.galaxy().ability_state()->results.size() == 1 && game.galaxy().ability_state()->results.at(0) == first
        && game.galaxy().ability_state()->next_request == next_request && game.return_state().heroes.size() == 1
        && second.story_notices.empty(),
        "a second request on the ruined planet settles nothing: no new receipt, request id, hero request or story notice");
    expect(std::any_of(events.begin(), events.end(), [](const auto& e) {
        return e.kind == gc::GalacticEventKind::command_rejected && e.producer == eawr::sim::SeatId{2} && e.sequence == 2
            && e.reason == gc::GalacticRejectReason::unknown_value;
    }), "the second request is rejected at admission (destroyed planet)");
    const auto ids = cp::game_identities(config); const auto saved = take(cp::save_game_checkpoint(game, ids));
    auto fresh = take(cp::load_game_checkpoint(saved, ids, {}, pool));
    expect(take(cp::save_game_checkpoint(fresh.session, ids)) == saved, "fresh reconstruction replays destruction exactly once");
    for (int i = 0; i < 40; ++i) {
        hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(take(fresh.session.step(pool)).state_hash.get() == hashes.back(), "saved destruction continuation equals the uninterrupted state");
    }
}
void contested(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    // Same observer contract alongside the live execution case on 1/2/4/8 workers.
    namespace h = gc::hero_actions;
    h::Fixture preview;
    h::Account account; account.base.player.id = 2; account.base.local = true;
    preview.accounts.push_back(account);
    h::Object actor; actor.id = 10; actor.owner = 2; actor.contested = true;
    h::Object blocker; blocker.id = 20; blocker.owner = 1; blocker.fleet = true;
    preview.objects = {actor, blocker};
    h::Planet planet; planet.id = 30; planet.orbit = {10, 20}; preview.planets.push_back(planet);
    const auto refusal = h::destroy_planet(preview, 10, 30, true).refusal;
    expect(refusal == h::Refusal::contested, "unscouted fleet still refuses execution");
    gc::DestructionPreparation preparation{10, 30, refusal, 10};
    gc::ObservedPlanet observation; observation.player = 2; observation.planet = 30; observation.discovered = true;
    auto menus = gc::observed_hero_action_menus(preview, 2, observation, 10, nullptr, nullptr, nullptr, &preparation);
    expect(menus && menus->destruction && menus->destruction->refusal == h::Refusal::none,
        "hidden enemy orbit leaves destruction preview enabled without contested refusal");
    observation.knowledge.fleet_count = true;
    menus = gc::observed_hero_action_menus(preview, 2, observation, 10, nullptr, nullptr, nullptr, &preparation);
    expect(menus && menus->destruction && menus->destruction->refusal == h::Refusal::contested,
        "known enemy orbit publishes contested refusal");
    expect(preparation.refusal == refusal && !preview.planets[0].destroyed,
        "observer filtering preserves execution inputs and planet state");
    const auto config = destroy_setup(); auto game = take(eawr::campaign::GameSession::create(config));
    order(game, 1, gc::ExecutePreparedAbility{direct(config)}); hashes.push_back(take(game.step(pool)).state_hash.get());
    expect(!game.galaxy().snapshot()->control()->planets[0].destroyed && game.return_state().heroes.empty()
        && game.galaxy().ability_state()->results.at(0).outcome == gc::AbilityOutcome::refused,
        "a nonstealth nonallied orbital fleet refuses before ruins with an identity-free public receipt");
    // The authored script bracket disables the check; the same command then succeeds (WGCH-160).
    auto world = take(gc::GalacticSession::create(config));
    world.set_ability_consumption_handler([&](const gc::HeroDefeat& defeat) {
        std::vector<eawr::sim::EntityId> removed = defeat.removed_objects; removed.push_back(defeat.id);
        std::sort(removed.begin(), removed.end()); removed.erase(std::unique(removed.begin(), removed.end()), removed.end());
        return world.apply_battle_losses(removed);
    });
    take(world.set_destruction_contested_check(bearer(config), false));
    take(world.submit({{0, {2}, 1}, gc::ExecutePreparedAbility{direct(config)}}));
    hashes.push_back(take(world.step(pool)).state_hash.get());
    expect(world.snapshot()->control()->planets[0].destroyed, "the disabled contested-space bracket admits the same command");
    take(world.set_destruction_contested_check(bearer(config), true));
}
void admission(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    {   // WGCH-113: an actor-owner ally of the TARGET allegiance player is refused at admission.
        auto config = destroy_setup(); stealth_fleet(config);
        for (auto& relation : config.control->relations) if (relation.player == 2 && relation.other == 1) relation.allied = true;
        auto game = take(eawr::campaign::GameSession::create(config));
        order(game, 1, gc::ExecutePreparedAbility{direct(config)}); hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(!game.galaxy().snapshot()->control()->planets[0].destroyed && game.galaxy().ability_state()->results.empty(),
            "allied target allegiance refuses with no result and no mutation");
    }
    {   // WGCH-137/157: the authored name must be in the activated list; a cancelled service refuses.
        auto config = destroy_setup(); stealth_fleet(config);
        config.ability_host->activated_destruction_abilities = {"OTHER"};
        auto game = take(eawr::campaign::GameSession::create(config));
        order(game, 1, gc::ExecutePreparedAbility{direct(config)}); hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(!game.galaxy().snapshot()->control()->planets[0].destroyed, "an unlisted ability name is never activated");
    }
    {
        auto config = destroy_setup(); stealth_fleet(config);
        auto game = take(eawr::campaign::GameSession::create(config));
        take(game.set_ability_lifecycle(bearer(config), true, true, gc::AbilityKind::planet_destruction));
        order(game, 1, gc::ExecutePreparedAbility{direct(config)}); hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(!game.galaxy().snapshot()->control()->planets[0].destroyed, "a cancelled generic service refuses this route");
    }
    {   // A forged priced payload is never a destruction capability.
        auto config = destroy_setup(); auto game = take(eawr::campaign::GameSession::create(config));
        auto forged = direct(config); forged.price = f(5);
        expect(!game.submit({{game.completed_tick(), {2}, 1}, gc::ExecutePreparedAbility{forged}}),
            "a priced destruction payload is malformed at submission");
        hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(!game.galaxy().snapshot()->control()->planets[0].destroyed, "no forged destruction is ever applied");
    }
}
void transport_container(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    // WGCH-155/156: the supplied object is a landed transport; the ability bearer is its contained object.
    auto config = destroy_setup(); stealth_fleet(config);
    // The fixture's landed transport carries the hero; give the hero's type the destruction ability instead.
    config.ability_host->profiles[0].destruction = gc::DestructionAbilityProfile{"DESTROY", true};
    auto game = take(eawr::campaign::GameSession::create(config));
    gc::PreparedAbility request; request.actor = transport; request.planet = config.planets[0].id;
    request.kind = gc::AbilityKind::planet_destruction;
    take(game.submit({{game.completed_tick(), {2}, 1}, gc::ExecutePreparedAbility{request}}));
    hashes.push_back(take(game.step(pool)).state_hash.get());
    expect(game.galaxy().snapshot()->control()->planets[0].destroyed,
        "a transport supplies its first contained ability bearer in stored order");
}
} // namespace
int case_galactic_ability_destruction_tests() {
    try {
        command_round_trip();
        std::vector<std::string> baseline; eawr::platform::ThreadWorkerAdapter one(1);
        destroyed(one, baseline); contested(one, baseline); admission(one, baseline); transport_container(one, baseline);
        for (std::size_t count : {2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter workers(count); std::vector<std::string> hashes;
            destroyed(workers, hashes); contested(workers, hashes); admission(workers, hashes); transport_container(workers, hashes);
            expect(hashes == baseline, "destruction command, contested refusal, ruins and restore hashes equal on 1/2/4/8");
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
