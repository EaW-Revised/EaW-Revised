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

namespace {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::checkpoint;
using galactic_test::expect;
using eawr::sim::math::Fixed;

Fixed f(std::int64_t n) { return take(Fixed::from_integer(n)); }
gc::GalacticSetup setup(bool consumes = true) { return ability_fixture::base_setup(consumes); }
eawr::sim::EntityId buyer(const gc::GalacticSetup& s) { return s.fleets[1].members[0].id; }
void order(eawr::campaign::GameSession& game, std::uint64_t sequence, gc::CommandPayload payload) {
    take(game.submit({{game.completed_tick(), {2}, sequence}, std::move(payload)}));
}
void contracts(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>* hashes) {
    const auto config = setup(); const auto actor = buyer(config);
    auto game = take(eawr::campaign::GameSession::create(config));
    const auto step = [&] {
        auto tick = take(game.step(pool));
        if (hashes) hashes->push_back(tick.state_hash.get());
        return tick;
    };
    expect(game.galaxy().economy_corruption()->buyers.size() == 2, "live buyers come from canonical actors, not setup-only stubs");
    order(game, 1, gc::PrepareGalacticAbility{actor, config.planets[0].id}); (void)step();
    expect(game.galaxy().ability_state()->preparations.size() == 1, "typed preparation installs retained facts");
    const auto prepared = gc::prepared_ability(game.galaxy().ability_state()->preparations[0], 0);
    expect(prepared.item == 91 && prepared.price == f(100) && prepared.absence == f(2), "independent item and buyer-tech absence operands");
    expect(gc::decode_payload(gc::CommandLayer::galactic, gc::galactic_opcodes::special_execute,
        gc::encode_payload(gc::ExecutePreparedAbility{prepared})) == gc::CommandPayload{gc::ExecutePreparedAbility{prepared}}, "typed prepared bytes round trip");
    auto forged = prepared; forged.price = {};
    order(game, 2, gc::ExecutePreparedAbility{forged}); (void)step();
    expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(1000)
        && game.return_state().heroes.empty(), "forged discount cannot debit, grant or schedule");
    const auto identities = cp::game_identities(config);
    auto saved = take(cp::save_game_checkpoint(game, identities));
    auto restored = take(cp::load_game_checkpoint(saved, identities, {}, pool));
    expect(take(cp::save_game_checkpoint(restored.session, identities)) == saved, "fresh host retains preparation without repricing/redraw");
    game = std::move(restored.session);
    order(game, 3, gc::ExecutePreparedAbility{prepared}); (void)step();
    expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(900)
        && game.galaxy().economy_corruption()->players[1].unlocks.contains(91), "purchase changes actual shared account and acquired ledger");
    expect(game.return_state().heroes.size() == 1 && game.return_state().heroes[0].deadline == 62,
        "WGCH-70 schedules in the existing saved return owner before removal");
    expect(std::none_of(game.galaxy().snapshot()->actor_records().begin(), game.galaxy().snapshot()->actor_records().end(),
        [actor](const auto& row) { return row.id == actor; }), "successful purchase removes the actual canonical actor");
    order(game, 4, gc::ExecutePreparedAbility{prepared}); (void)step();
    expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(900)
        && game.return_state().heroes.size() == 1, "stale absent actor cannot purchase/schedule twice");
    saved = take(cp::save_game_checkpoint(game, identities)); restored = take(cp::load_game_checkpoint(saved, identities, {}, pool));
    expect(take(cp::save_game_checkpoint(restored.session, identities)) == saved, "post-purchase restore does not repeat reward or absence");
    game = std::move(restored.session);
    order(game, 5, gc::PauseGalaxy{}); (void)step(); const auto frame = game.galaxy().clock().galactic_frame;
    for (unsigned i = 0; i < 3; ++i) (void)step();
    expect(game.galaxy().clock().galactic_frame == frame && game.return_state().heroes[0].deadline == 62,
        "nested galaxy pause does not charge wall time to ability absence");
}
void cancellation_and_stale() {
    eawr::sim::InlineExecutor pool; const auto config = setup(false); const auto actor = buyer(config);
    auto game = take(eawr::campaign::GameSession::create(config));
    order(game, 1, gc::PrepareGalacticAbility{actor, config.planets[0].id}); (void)take(game.step(pool));
    const auto first = gc::prepared_ability(game.galaxy().ability_state()->preparations[0], 0);
    order(game, 2, gc::PrepareGalacticAbility{actor, config.planets[0].id}); (void)take(game.step(pool));
    const auto current = gc::prepared_ability(game.galaxy().ability_state()->preparations[0], 0);
    order(game, 3, gc::ExecutePreparedAbility{first}); (void)take(game.step(pool));
    expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(1000), "superseded identity never reinterprets a queued index");
    order(game, 4, gc::CancelGalacticAbility{actor, current.preparation}); (void)take(game.step(pool));
    expect(!game.galaxy().ability_state()->preparations[0].open && game.return_state().heroes.empty(), "menu cancellation has no invented consumption/refund");
    // WGEP-80: explicit helper execution does not repeat the local menu-open gate.
    order(game, 5, gc::ExecutePreparedAbility{current}); (void)take(game.step(pool));
    order(game, 6, gc::ExecutePreparedAbility{current}); (void)take(game.step(pool));
    expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(800), "live unconsumed helper allows explicit repeated owned purchase");
    auto bytes = gc::encode_payload(gc::ExecutePreparedAbility{current}); bytes.push_back(0);
    expect(!gc::decode_payload(gc::CommandLayer::galactic, gc::galactic_opcodes::special_execute, bytes), "trailing prepared payload bytes refuse");
}
void lifecycle() {
    eawr::sim::InlineExecutor pool; const auto config = setup(false); const auto actor = buyer(config);
    auto game = take(eawr::campaign::GameSession::create(config));
    order(game, 1, gc::PrepareGalacticAbility{actor, config.planets[0].id}); (void)take(game.step(pool));
    const auto prepared = gc::prepared_ability(game.galaxy().ability_state()->preparations[0], 0);
    take(game.set_ability_lifecycle(actor, false, false));
    order(game, 2, gc::ExecutePreparedAbility{prepared}); (void)take(game.step(pool));
    expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(1000), "disabled service refuses retained activation");
    take(game.set_ability_lifecycle(actor, true, true));
    order(game, 3, gc::ExecutePreparedAbility{prepared}); (void)take(game.step(pool));
    expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(1000), "cancelled service refuses without an invented refund/debit");
    const auto identities = cp::game_identities(config);
    const auto saved = take(cp::save_game_checkpoint(game, identities));
    auto restored = take(cp::load_game_checkpoint(saved, identities, {}, pool));
    expect(take(cp::save_game_checkpoint(restored.session, identities)) == saved, "native lifecycle flags reconstruct in the saved host");
    game = std::move(restored.session);
    take(game.set_ability_lifecycle(actor, true, false));
    order(game, 4, gc::ExecutePreparedAbility{prepared}); (void)take(game.step(pool));
    expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(900), "rebound service executes the original retained price");
    expect(game.galaxy().snapshot()->ability_results().empty(), "no observer binding publishes no ability receipts");
}

void unrelated_actor_write() {
    eawr::sim::InlineExecutor pool; const auto config = setup(false); const auto actor = buyer(config);
    auto world = take(gc::GalacticSession::create(config));
    take(world.submit({{0, {2}, 1}, gc::PrepareGalacticAbility{actor, config.planets[0].id}})); (void)take(world.step(pool));
    const auto prepared = gc::prepared_ability(world.ability_state()->preparations[0], 0);
    take(world.submit({{1, {2}, 2}, gc::ExecutePreparedAbility{prepared}})); (void)take(world.step(pool));
    const auto lookup = [&]() -> const gc::BlackMarketBuyer& {
        return *std::find_if(world.economy_corruption()->buyers.begin(), world.economy_corruption()->buyers.end(),
            [actor](const auto& row) { return row.buyer == actor; });
    };
    const auto before = lookup(); expect(before.prepared_absence == prepared.absence, "successful helper retains its prepared absence marker");
    take(world.set_actor_garrison(actor, true));
    expect(lookup() == before, "unrelated sparse actor facts do not reset retained buyer pricing/lifetime facts");
}

void representations(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    for (const auto& [initial, target] : {std::pair<gc::TypeId, gc::TypeId>{1000, 1001}, {1001, 1000}, {1000, 1002}}) {
        auto config = setup(false); const auto actor = buyer(config);
        config.census->types.push_back({1001, f(1), 0}); config.census->types.push_back({1002, f(1), 0});
        for (auto& row : config.census->objects) if (row.id == actor) row.current_type = initial;
        auto variant = config.ability_host->profiles[0]; variant.type = 1002;
        variant.market.price_modifier = f(2); variant.initially_enabled = false;
        variant.market.absence_min = variant.market.absence_max = {f(3), f(3), f(3), f(3), f(3)};
        config.ability_host->profiles.push_back(variant);
        const auto identities = cp::game_identities(config);
        // Existing qualified representation writes may be the only activity at this barrier.
        auto zero = take(eawr::campaign::GameSession::create(config));
        take(zero.change_actor_current_type(actor, target));
        if (target != 1001) take(zero.set_ability_lifecycle(actor, false, true));
        auto saved = take(cp::save_game_checkpoint(zero, identities));
        auto fresh = take(cp::load_game_checkpoint(saved, identities, {}, pool));
        expect(take(cp::save_game_checkpoint(fresh.session, identities)) == saved,
            "zero-tick market/nonmarket/different-profile representation restores its exact host and buyer");
        hashes.push_back(fresh.session.state_sha256());
        auto game = take(eawr::campaign::GameSession::create(config));
        if (initial != 1000) take(game.change_actor_current_type(actor, 1000));
        order(game, 1, gc::PrepareGalacticAbility{actor, config.planets[0].id}); (void)take(game.step(pool));
        const auto prepared = gc::prepared_ability(game.galaxy().ability_state()->preparations[0], 0);
        expect(prepared.price == f(100) && prepared.absence == f(2), "original prepared facts precede the representation change");
        take(game.set_ability_lifecycle(actor, false, true));
        take(game.change_actor_current_type(actor, target));
        if (target != 1001) take(game.set_ability_lifecycle(actor, false, true));
        saved = take(cp::save_game_checkpoint(game, identities));
        fresh = take(cp::load_game_checkpoint(saved, identities, {}, pool));
        expect(take(cp::save_game_checkpoint(fresh.session, identities)) == saved,
            "representation restoration retains canonical preparation, service flags and exact old pricing");
        expect(gc::prepared_ability(fresh.session.galaxy().ability_state()->preparations[0], 0) == prepared,
            "hydration does not redraw or reprice an existing preparation");
        hashes.push_back(fresh.session.state_sha256());
        game = std::move(fresh.session);
        if (target != 1001) {
            const auto& actors = game.galaxy().ability_state()->actors;
            const auto state = std::find_if(actors.begin(), actors.end(), [actor](const auto& a) { return a.actor == actor; });
            expect(state != actors.end() && state->type == target && !state->enabled && state->cancelled,
                "saved current-profile service flags survive reconstruction before activation");
            take(game.set_ability_lifecycle(actor, true, false));
        }
        order(game, 2, gc::ExecutePreparedAbility{prepared}); const auto tick = take(game.step(pool));
        expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(target == 1001 ? 1000 : 900),
            "restored nonmarket refuses; restored market uses its retained price, not its new profile price");
        hashes.push_back(tick.state_hash.get());
    }
}

}
int case_galactic_ability_host_tests() {
    try {
        std::vector<std::string> baseline;
        eawr::platform::ThreadWorkerAdapter one(1); contracts(one, &baseline); representations(one, baseline);
        for (std::size_t workers : {2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter pool(workers); std::vector<std::string> hashes;
            contracts(pool, &hashes); representations(pool, hashes); expect(hashes == baseline, "market/restore hashes equal on 1/2/4/8 workers");
        }
        cancellation_and_stale(); lifecycle(); unrelated_actor_write();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
