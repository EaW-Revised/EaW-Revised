#include "ability_travel_fixture.hpp"
#include "galactic_support.hpp"
#include "../support/result.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/ability_host.hpp"
#include <array>
#include <iostream>

namespace {
using namespace ability_travel_fixture;
using galactic_test::expect;
using eawr::testing::take;
namespace cp = eawr::checkpoint;
using Game = eawr::campaign::GameSession;
const gc::GalacticSession& world(const Game& game) { return game.galaxy(); }
const gc::GalacticSession& world(const gc::GalacticSession& game) { return game; }
template <class Session> void submit(Session& game, gc::CommandPayload payload, std::uint64_t sequence = 1) {
    take(game.submit({{game.completed_tick(), {2}, sequence}, std::move(payload)}));
}
template <class Session> const gc::Fleet* moving(const Session& game, gc::FleetId id) {
    const auto rows = world(game).snapshot()->fleets();
    const auto at = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.id == id; });
    return at == rows.end() ? nullptr : &*at;
}
template <class Session> void finish(Session& game, const eawr::sim::PartitionExecutor& pool) {
    for (unsigned tick = 0; tick < 100; ++tick) {
        const auto rows = world(game).snapshot()->fleets();
        if (std::none_of(rows.begin(), rows.end(), [](const auto& row) { return row.motion.transit.has_value(); })) return;
        (void)take(game.step(pool));
    }
    expect(false, "travel terminates within fixture bound");
}
void final_and_save(const eawr::sim::PartitionExecutor& pool) {
    const auto config = setup(); auto game = take(Game::create(config));
    const auto command = move(config); submit(game, command); (void)take(game.step(pool));
    const auto* fleet = moving(game, command.fleet);
    expect(fleet && fleet->motion.transit && fleet->motion.transit->destination_actor == transport,
        "accepted movement retains supplied container, never the inner bearer");
    expect(game.galaxy().ability_state()->preparations.empty(), "no priced preparation during travel");
    const auto ids = cp::game_identities(config); const auto saved = take(cp::save_game_checkpoint(game, ids));
    auto loaded = take(cp::load_game_checkpoint(saved, ids, {}, pool));
    expect(take(cp::save_game_checkpoint(loaded.session, ids)) == saved, "save retains exact transit identity and inbox");
    galactic_test::mark_saved(game);
    for (unsigned i = 0; i < 30; ++i) {
        const auto a = take(game.step(pool)); const auto b = take(loaded.session.step(pool));
        expect(a.state_hash.get() == b.state_hash.get(), "saved continuation matches uninterrupted arrival");
    }
    const auto& menus = game.galaxy().ability_state()->preparations;
    expect(menus.size() == 1 && menus[0].actor == first && menus[0].planet == 1 && menus[0].open,
        "arrival opens live first contained actor corruption menu once");
    expect(game.galaxy().ability_state()->next_request == 2, "finished transit cannot activate twice");
}
void clear_and_stale(const eawr::sim::PartitionExecutor& pool) {
    for (unsigned scenario = 0; scenario < 9; ++scenario) {
        auto config = setup();
        if (scenario == 0) for (auto& row : config.movement->access) if (row.player == 2 && row.planet == 1) row.potential_conflict = true;
        if (scenario == 1) for (auto& row : config.movement->access) if (row.player == 2 && row.planet == 1) row.accessible = false;
        if (scenario == 6) config.ability_host->profiles[0].ground_activated.clear();
        auto game = take(gc::GalacticSession::create(config)); auto command = move(config);
        if (scenario == 7) command.destination_actor = 999999;
        submit(game, command); (void)take(game.step(pool));
        if (scenario == 2) take(game.apply_battle_losses(std::array{transport}));
        if (scenario == 3) take(game.apply_battle_losses(std::array{first}));
        if (scenario == 4) take(game.set_ability_lifecycle(first, false, true, gc::AbilityKind::corruption));
        if (scenario == 5) {
            take(game.set_ability_lifecycle(first, false, true, gc::AbilityKind::corruption));
            take(game.set_ability_lifecycle(second, false, true, gc::AbilityKind::corruption));
        }
        if (scenario == 8) take(game.apply_battle_losses(std::array{first, second}));
        if (scenario == 2 || scenario == 3 || scenario == 8) {
            const auto saved = take(game.save_checkpoint_state());
            auto loaded = take(gc::GalacticSession::create(config));
            take(loaded.load_checkpoint_state(saved));
            expect(take(loaded.save_checkpoint_state()) == saved, "stale-identity owner checkpoint restores exactly");
            game = std::move(loaded);
        }
        finish(game, pool);
        const auto& menus = game.ability_state()->preparations;
        if (scenario == 3 || scenario == 4) expect(menus.size() == 1 && menus[0].actor == second,
            "changed contents or cancelled first service resolves next live bearer on arrival");
        else expect(menus.empty(), "conflict, bounce, root loss, disabled group, unauthored group or foreign identity never activate");
        for (const auto& fleet : game.snapshot()->fleets()) expect(!fleet.motion.transit, "settled continuation clears");
    }
}
void paths_launch_merge(const eawr::sim::PartitionExecutor& pool) {
    auto config = setup(); auto game = take(Game::create(config));
    auto invalid = move(config); invalid.path = {2, 99}; submit(game, invalid); (void)take(game.step(pool));
    expect(moving(game, invalid.fleet)->orbit == 2 && !moving(game, invalid.fleet)->motion.transit,
        "rejected path retains no destination actor");
    submit(game, move(config, {2, 3, 4, 1}), 2); (void)take(game.step(pool));
    bool intermediate = false;
    for (unsigned i = 0; i < 90 && moving(game, invalid.fleet)->motion.transit; ++i) {
        const auto& transit = *moving(game, invalid.fleet)->motion.transit;
        if (transit.hop > 0) { intermediate = true; expect(game.galaxy().ability_state()->preparations.empty(), "intermediate hop never activates"); }
        (void)take(game.step(pool));
    }
    expect(intermediate && game.galaxy().ability_state()->preparations.size() == 1, "multi-hop action fires only at final destination");
    auto surface = setup(true); auto launched = take(Game::create(surface));
    submit(launched, gc::LaunchFleet{{transport}, {2, 1}, {}, transport}); (void)take(launched.step(pool));
    expect(launched.galaxy().ability_state()->preparations.empty(), "combined launch travels before preparing");
    finish(launched, pool);
    expect(launched.galaxy().ability_state()->preparations.size() == 1, "combined launch retains original transport for arrival");
    for (const bool conflict : {false, true}) {
        auto refused = setup(true);
        for (auto& access : refused.movement->access) if (access.player == 2 && access.planet == (conflict ? 2 : 1)) {
            if (conflict) access.potential_conflict = true;
            else access.accessible = false;
        }
        auto stopped = take(Game::create(refused));
        submit(stopped, gc::LaunchFleet{{transport}, {2, 1}, {}, transport}); (void)take(stopped.step(pool));
        finish(stopped, pool);
        expect(stopped.galaxy().ability_state()->preparations.empty(), "source conflict or rejected launch destination never prepares an arrival action");
    }
    auto merge_config = setup();
    // Use an existing nonempty fleet as the same-owner target, avoiding allocation prediction.
    merge_config.fleets[0].owner = 2; merge_config.fleets[0].orbit_slot = std::uint8_t{1};
    merge_config.census->objects[0].owner = 2;
    auto merged = take(Game::create(merge_config)); auto merging = move(merge_config); merging.destination_slot = std::uint8_t{1};
    submit(merged, merging); (void)take(merged.step(pool)); finish(merged, pool);
    const auto arrived = merged.galaxy().snapshot();
    expect(!moving(merged, merging.fleet) && std::any_of(arrived->actor_records().begin(), arrived->actor_records().end(),
        [&](const auto& row) { return row.id == transport && row.container == merge_config.fleets[0].id && row.planet == 1; }),
        "merge destroys old fleet shell but preserves the supplied container at its live destination");
    // WGCH-02 still refuses corruption from the now mixed fleet. Arrival must reach that shared host admission.
    expect(merged.galaxy().ability_state()->preparations.empty()
        && std::any_of(arrived->events().begin(), arrived->events().end(), [](const auto& event) {
            return event.kind == gc::GalacticEventKind::command_rejected && event.subject == first;
        }), "merged arrival dispatches the live bearer through the shared host instead of retaining a priced preparation");
}
void destruction_uses_qualified_bearer(const eawr::sim::PartitionExecutor& pool) {
    constexpr gc::TypeId second_type = 4000;
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        auto config = setup();
        auto& host = *config.ability_host;
        auto& first_profile = host.profiles[0];
        first_profile.corruption.reset();
        first_profile.destruction = gc::DestructionAbilityProfile{"MOD_DESTRUCTION", scenario != 1};
        first_profile.ground_activated = scenario == 0 ? std::vector<gc::AbilityKind>{}
            : std::vector{gc::AbilityKind::planet_destruction};
        auto second_profile = first_profile; second_profile.type = second_type;
        second_profile.destruction->initially_enabled = true;
        second_profile.ground_activated = {gc::AbilityKind::planet_destruction};
        host.profiles.push_back(second_profile); host.activated_destruction_abilities = {"MOD_DESTRUCTION"};
        config.census->types.push_back({second_type, {}, 0});
        config.fleet_control->types.push_back({second_type, false, true, false});
        config.control->types.push_back({second_type, 0, false, false});
        config.movement->types.push_back({second_type, whole(10), whole(10), 0, true, false, false, false, false});
        gc::NestedCreationType created; created.type = second_type;
        created.combat_role = gc::ActorCombatRole::ordinary; created.abilities.present = true;
        config.production->nested_creation->types.push_back(created);
        for (auto& actor : config.census->objects) {
            if (actor.id == second) actor.current_type = actor.original_type = second_type;
            if (actor.owner == 1) { actor.abilities.present = true; actor.abilities.combat_skip = true; }
        }
        auto& contents = config.movement->contents[0].children;
        contents[1].type = second_type;
        if (scenario == 2) std::reverse(contents.begin(), contents.end());
        const auto expected = scenario == 3 ? first : second;
        auto game = take(Game::create(config)); submit(game, move(config)); (void)take(game.step(pool));
        expect(game.galaxy().ability_state()->results.empty(), "destruction waits for final arrival");
        finish(game, pool);
        const auto& results = game.galaxy().ability_state()->results;
        expect(results.size() == 1 && results[0].actor == expected
            && results[0].kind == gc::AbilityKind::planet_destruction && results[0].outcome == gc::AbilityOutcome::purchased,
            "arrival binds the first enabled ground-qualified destruction child in stored order");
        expect(game.galaxy().snapshot()->control()->planets[0].destroyed,
            "an earlier unqualified or disabled destruction child cannot block the eligible arrival bearer");
        expect(game.galaxy().ability_state()->preparations.empty(), "arrival destruction retains no priced preparation");
    }
}
void consumed_spy_arrival() {
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        auto config = setup();
        auto& profile = config.ability_host->profiles[0];
        profile.corruption.reset(); profile.named = false;
        // Short synthetic duration exercises the stock finite-source contract
        // without making the equality/save/worker regression run for 1800 frames.
        profile.spy = gc::SpyAbilityProfile{"MOD_SPY", "mod scout", 7, whole(2),
            gc::bit(gc::Intelligence::fleet_contents) | gc::bit(gc::Intelligence::structures)
                | gc::bit(gc::Intelligence::ownership), true, true, true};
        profile.ground_activated = {gc::AbilityKind::system_spy};
        config.ability_host->activated_spy_abilities = {"MOD_SPY"};
        config.movement->contents[0].children.resize(1);
        std::erase_if(config.census->objects, [](const auto& actor) { return actor.id == second; });
        ability_fixture::add_observer(config, 2, {1000, 2000});
        auto game = take(Game::create(config));
        submit(game, move(config));
        std::vector<std::string> actual{take(game.step(pool)).state_hash.get()};
        expect(game.galaxy().ability_state()->results.empty(), "ground-activated spy waits for final arrival");
        const auto ids = cp::game_identities(config);
        const auto saved = take(cp::save_game_checkpoint(game, ids));
        auto loaded = take(cp::load_game_checkpoint(saved, ids, {}, pool));
        galactic_test::mark_saved(game);
        for (unsigned tick = 0; tick < 30; ++tick) {
            actual.push_back(take(game.step(pool)).state_hash.get());
            expect(take(loaded.session.step(pool)).state_hash.get() == actual.back(),
                "contained spy arrival continues identically after an in-flight save");
        }
        const auto& results = game.galaxy().ability_state()->results;
        expect(results.size() == 1 && results[0].actor == first && results[0].kind == gc::AbilityKind::system_spy
            && results[0].outcome == gc::AbilityOutcome::purchased,
            "final arrival invokes the live contained spy exactly once");
        const auto snapshot = game.galaxy().snapshot();
        expect(std::none_of(snapshot->actor_records().begin(), snapshot->actor_records().end(),
            [](const auto& actor) { return actor.id == first; }), "arrival consumes the generic spy bearer");
        expect(std::none_of(snapshot->actor_records().begin(), snapshot->actor_records().end(),
            [](const auto& actor) { return actor.id == transport; }) && !moving(game, config.fleets[1].id),
            "last-passenger spy consumption removes its empty transport and fleet");
        expect(!game.pending_conflict() && game.galaxy().clock().pause_depth == 0,
            "consumed lone spy leaves no phantom battle or campaign pause");
        const auto& observer = snapshot->observer();
        expect(observer && std::any_of(observer->records.begin(), observer->records.end(), [](const auto& row) {
            return row.player == 2 && row.planet == 1 && std::any_of(row.sources.begin(), row.sources.end(),
                [](const auto& source) { return source.source == first; });
        }), "consumed arrival spy leaves its finite intelligence source at the destination");
        if (!observer) { expect(false, "spy arrival retains its observer owner"); continue; }
        const auto record = std::find_if(observer->records.begin(), observer->records.end(), [](const auto& row) {
            return row.player == 2 && row.planet == 1;
        });
        if (record == observer->records.end()) { expect(false, "spy recipient record exists"); continue; }
        const auto source = std::find_if(record->sources.begin(), record->sources.end(), [](const auto& row) {
            return row.source == first;
        });
        if (source == record->sources.end()) { expect(false, "finite spy source exists before expiry"); continue; }
        const auto end = source->final_frame;
        const auto advance = [&] {
            actual.push_back(take(game.step(pool)).state_hash.get());
            expect(take(loaded.session.step(pool)).state_hash.get() == actual.back(),
                "saved consumed spy expiry matches uninterrupted planet service");
        };
        while (game.galaxy().clock().galactic_frame < end) advance();
        // Service reads this frame before the tail increments the published
        // clock. Observe the service at end, then at end+1 independently.
        advance();
        const auto at_end = game.galaxy().snapshot();
        const auto knows = [](const auto& state) {
            const auto rows = state->observations();
            const auto found = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
                return row.player == 2 && row.planet == 1;
            });
            return found != rows.end() && found->knowledge.fleet_contents && found->knowledge.structures;
        };
        expect(knows(at_end), "finite consumed spy knowledge remains at the exact expiry frame");
        advance();
        const auto after = game.galaxy().snapshot();
        expect(after->clock().galactic_frame == end + 2, "expiry witness services exactly end+1 before the timer tail");
        const auto& after_records = after->observer()->records;
        const auto expired = std::find_if(after_records.begin(), after_records.end(), [](const auto& row) {
            return row.player == 2 && row.planet == 1;
        });
        expect(expired != after_records.end() && expired->refresh_frame == end + 1 && expired->sources.empty(),
            "planet service removes consumed finite source strictly after expiry without an explicit refresh command");
        expect(expired != after_records.end() && expired->apparent_owner == config.planets[0].owner,
            "direct expiry recomputation caches actual controller on ownership-bit loss (WGUI-99)");
        expect(!knows(after), "expiry republishes observer knowledge instead of retaining the old HUD mask");
        if (workers == 1) reference = actual;
        else expect(actual == reference, "spy arrival has canonical equality on 1/2/4/8 workers");
    }
}
void codecs() {
    const auto config = setup();
    for (const gc::CommandPayload& payload : {gc::CommandPayload{move(config)}, gc::CommandPayload{gc::LaunchFleet{{transport}, {2, 1}, {}, transport}}}) {
        const auto bytes = gc::encode_payload(payload);
        expect(gc::decode_payload(gc::layer_of(payload), gc::opcode_of(payload), bytes) == payload, "destination command fields round trip");
        auto truncated = bytes; truncated.pop_back();
        expect(!gc::decode_payload(gc::layer_of(payload), gc::opcode_of(payload), truncated), "truncated destination identity refuses");
    }
}
}
int case_galactic_ability_travel_tests() {
    try {
        ability_fixture::arm_watchdog(); codecs();
        eawr::platform::ThreadWorkerAdapter pool(1);
        final_and_save(pool); clear_and_stale(pool); paths_launch_merge(pool); destruction_uses_qualified_bearer(pool);
        consumed_spy_arrival();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
