#include "ability_ruins_support.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/observer.hpp"

// WGCH-57/66/109..112/139/148/176..181: live superweapon hacking on the ability host.
namespace {
using namespace ability_fixture;
constexpr gc::TypeId weapon_type = 4000;
eawr::sim::EntityId actor(const gc::GalacticSetup& s) { return s.fleets[1].members[0].id; }
eawr::sim::EntityId weapon(const gc::GalacticSetup& s) { return s.fleets[0].members[0].id; }
void order(eawr::campaign::GameSession& game, std::uint64_t sequence, gc::CommandPayload payload) {
    take(game.submit({{game.completed_tick(), {2}, sequence}, std::move(payload)}));
}
// The hacker is player 2's fleet member in orbit at planet 0; the matching weapon belongs to player 1's fleet.
gc::GalacticSetup hack_setup(bool human = true) {
    auto s = ruins_setup(false); auto& host = *s.ability_host; const auto planet = s.planets[0].id;
    s.fleets[0].orbit = planet; s.fleets[0].members[0].type = weapon_type;
    for (auto& row : s.census->objects) if (row.id == weapon(s)) row.original_type = row.current_type = weapon_type;
    register_type(s, weapon_type);
    host.profiles[0].hack = gc::HackAbilityProfile{"HACK", true};
    host.activated_hack_abilities = {"HACK"}; host.hack_required_type = weapon_type; host.hack_cost = f(300);
    s.economy_corruption->players[1].human = human;
    return s;
}
gc::PreparedAbility prepare(eawr::campaign::GameSession& game, const gc::GalacticSetup& config,
    const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    order(game, 1, gc::PrepareGalacticAbility{actor(config), config.planets[0].id, gc::AbilityKind::superweapon_hack});
    hashes.push_back(take(game.step(pool)).state_hash.get());
    const auto& menus = game.galaxy().ability_state()->preparations;
    expect(menus.size() == 1 && menus[0].kind == gc::AbilityKind::superweapon_hack && menus[0].offers.size() == 1
        && menus[0].offers[0].price == f(300), "hack preparation retains the authored price as one prepared choice");
    return gc::prepared_ability(menus[0], 0);
}
bool locked(const gc::GalacticSession& world, gc::PlayerId player, gc::TypeId type) {
    for (const auto& row : world.snapshot()->production()->players) if (row.player == player)
        return std::binary_search(row.locked.begin(), row.locked.end(), type);
    return false;
}
void paid(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    const auto config = hack_setup(); auto game = take(eawr::campaign::GameSession::create(config));
    const auto prepared = prepare(game, config, pool, hashes);
    const auto* menu = game.galaxy().snapshot()->hero_action_menus();
    expect(menu && menu->hack && menu->hack->live_prepared == prepared && menu->hack->prepared.prepared_price == f(300)
        && menu->hack->weapon == weapon(config) && menu->hack->affordable, "observer menu carries the exact live capability and the display-only weapon");
    expect(gc::decode_payload(gc::CommandLayer::galactic, gc::galactic_opcodes::special_execute,
        gc::encode_payload(gc::ExecutePreparedAbility{prepared})) == gc::CommandPayload{gc::ExecutePreparedAbility{prepared}},
        "hack prepared payload round trips");
    auto forged = prepared; forged.price = f(1);
    order(game, 2, gc::ExecutePreparedAbility{forged}); hashes.push_back(take(game.step(pool)).state_hash.get());
    expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(1000) && !locked(game.galaxy(), 1, weapon_type),
        "a forged price refuses before any debit or lock");
    order(game, 3, gc::ExecutePreparedAbility{prepared}); hashes.push_back(take(game.step(pool)).state_hash.get());
    const auto& world = game.galaxy(); const auto snapshot = world.snapshot();
    expect(snapshot->player_economy()->players[1].credits == f(700), "the human route pays the prepared price from the current actor owner");
    expect(locked(world, 1, weapon_type) && !locked(world, 2, weapon_type), "the matched weapon's current type is locked for its current owner");
    expect(!present(world, actor(config)) && !present(world, weapon(config)), "hacker and every orbital fleet member leave through canonical loss");
    expect(snapshot->fleets().empty() || std::none_of(snapshot->fleets().begin(), snapshot->fleets().end(),
        [&](const auto& fleet) { return fleet.orbit == config.planets[0].id; }), "no orbital fleet remains at the planet");
    expect(snapshot->control()->planets[0].destroyed && snapshot->control()->planets[0].starbase.level == 0
        && snapshot->control()->planets[0].landed.empty(), "shared ruins, starbase clear and surface cleanup apply");
    expect(snapshot->control()->state[0].current_player == neutral_player
        && snapshot->control()->state[0].target_player == neutral_player
        && snapshot->planets()[0].owner == neutral_player, "the named Neutral player is forced as the planet owner");
    const auto& heroes = game.return_state().heroes;
    expect(heroes.size() == 1 && heroes[0].defeated == hero, "the hacker schedules no return; only the ruined landed named hero requests one");
    expect(world.ability_state()->results.at(0).outcome == gc::AbilityOutcome::purchased && world.ability_state()->results.at(0).kind == gc::AbilityKind::superweapon_hack,
        "typed purchased result");
    const auto ids = cp::game_identities(config); const auto saved = take(cp::save_game_checkpoint(game, ids));
    auto fresh = take(cp::load_game_checkpoint(saved, ids, {}, pool));
    expect(take(cp::save_game_checkpoint(fresh.session, ids)) == saved, "fresh reconstruction retains the lock, ruins and ownership once");
    for (int i = 0; i < 40; ++i) {
        hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(take(fresh.session.step(pool)).state_hash.get() == hashes.back(), "saved hack continuation equals the uninterrupted state");
    }
}
void refusals(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    {   // WGCH-112: unaffordable current funds fail before any debit or mutation.
        auto config = hack_setup(); config.player_economy->players[1].credits = f(299);
        auto game = take(eawr::campaign::GameSession::create(config)); const auto prepared = prepare(game, config, pool, hashes);
        expect(!game.galaxy().snapshot()->hero_action_menus()->hack->affordable, "menu affordability compares current local credits");
        order(game, 2, gc::ExecutePreparedAbility{prepared}); hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(299) && present(game.galaxy(), actor(config))
            && !locked(game.galaxy(), 1, weapon_type) && !game.galaxy().snapshot()->control()->planets[0].destroyed,
            "insufficient funds refuse with no debit, lock, removal or ruins");
    }
    {   // The weapon is re-resolved: removing it between prepare and execute refuses before payment.
        const auto config = hack_setup(); auto world = take(gc::GalacticSession::create(config)); std::vector<gc::HeroReturnRecord> returns;
        world.set_ability_consumption_handler([&](const gc::HeroDefeat& defeat) {
            auto scheduled = gc::schedule_hero_return(returns, defeat, world.clock().galactic_frame, world.snapshot()->planets());
            if (!scheduled) return eawr::core::Result<void>::failure(scheduled.error());
            std::vector<eawr::sim::EntityId> removed = scheduled.value().destroyed_companions;
            removed.insert(removed.end(), defeat.removed_objects.begin(), defeat.removed_objects.end());
            removed.push_back(defeat.id); std::sort(removed.begin(), removed.end());
            removed.erase(std::unique(removed.begin(), removed.end()), removed.end());
            return world.apply_battle_losses(removed);
        });
        take(world.submit({{0, {2}, 1}, gc::PrepareGalacticAbility{actor(config), config.planets[0].id, gc::AbilityKind::superweapon_hack}}));
        hashes.push_back(take(world.step(pool)).state_hash.get());
        const auto prepared = gc::prepared_ability(world.ability_state()->preparations.at(0), 0);
        const eawr::sim::EntityId gone = weapon(config);
        take(world.apply_battle_losses(std::span<const eawr::sim::EntityId>(&gone, 1)));
        take(world.submit({{world.completed_tick(), {2}, 2}, gc::ExecutePreparedAbility{prepared}}));
        hashes.push_back(take(world.step(pool)).state_hash.get());
        expect(world.snapshot()->player_economy()->players[1].credits == f(1000) && present(world, actor(config)) && returns.empty(),
            "a missing weapon refuses before payment, removal or ruins");
    }
    {   // No exact current-type match at preparation opens no hack menu (WGCH-139).
        auto config = hack_setup(); config.ability_host->hack_required_type = 4999;
        auto game = take(eawr::campaign::GameSession::create(config));
        order(game, 1, gc::PrepareGalacticAbility{actor(config), config.planets[0].id, gc::AbilityKind::superweapon_hack});
        hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(game.galaxy().ability_state()->preparations.empty() && !game.galaxy().snapshot()->hero_action_menus(),
            "an unmatched required type opens no menu and records no preparation");
    }
}
void ai_route(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    const auto config = hack_setup(false); auto game = take(eawr::campaign::GameSession::create(config));
    const auto prepared = prepare(game, config, pool, hashes);
    order(game, 2, gc::ExecutePreparedAbility{prepared}); hashes.push_back(take(game.step(pool)).state_hash.get());
    expect(game.galaxy().snapshot()->player_economy()->players[1].credits == f(1000) && locked(game.galaxy(), 1, weapon_type)
        && game.galaxy().snapshot()->control()->planets[0].destroyed, "the direct nonhuman route pays nothing and still applies every effect");
}
void spy_transfer(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    for (const auto range : {0, 10000}) {
        auto config = hack_setup(); const auto planet = config.planets[0].id;
        const auto previous_owner = config.planets[0].owner;
        const auto ability = source_ability + 1;
        config.observer->planet_spies = {{planet, ability, "hacked planet network",
            gc::bit(gc::Intelligence::income_breakdown), f(-1), f(range), gc::SpyStyle::galactic_automatic, true, false}};
        config.observer->records.clear();
        take(gc::initialize_campaign_observer(config, {}));
        for (auto& record : config.observer->records) record.discovered = true;
        auto game = take(eawr::campaign::GameSession::create(config));
        const auto prepared = prepare(game, config, pool, hashes);
        order(game, 2, gc::ExecutePreparedAbility{prepared});
        hashes.push_back(take(game.step(pool)).state_hash.get());
        std::size_t old_sources{}, neutral_sources{};
        for (const auto& record : game.galaxy().snapshot()->observer()->records) for (const auto& source : record.sources)
            if (source.source == planet && source.ability == ability) {
                old_sources += record.player == previous_owner;
                neutral_sources += record.player == neutral_player;
            }
        expect(old_sources == 0 && neutral_sources == (range == 0 ? 1 : config.planets.size()),
            "hack-forced Neutral ownership transfers the planet spy across its authored local/nearby targets");
        const auto ids = cp::game_identities(config);
        const auto saved = take(cp::save_game_checkpoint(game, ids));
        auto restored = take(cp::load_game_checkpoint(saved, ids, {}, pool));
        expect(restored.session.state_sha256() == game.state_sha256(), "checkpoint retains hack-transferred planet intelligence exactly once");
    }
}
} // namespace
int main() {
    try {
        std::vector<std::string> baseline; eawr::platform::ThreadWorkerAdapter one(1);
        paid(one, baseline); refusals(one, baseline); ai_route(one, baseline); spy_transfer(one, baseline);
        for (std::size_t count : {2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter workers(count); std::vector<std::string> hashes;
            paid(workers, hashes); refusals(workers, hashes); ai_route(workers, hashes); spy_transfer(workers, hashes);
            expect(hashes == baseline, "hack command, lock, ruins and restore hashes equal on 1/2/4/8");
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
