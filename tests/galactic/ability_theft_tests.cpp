#include "galactic_support.hpp"
#include "ability_fixture.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/ability_host.hpp"
#include "eawr/sim/galactic/income.hpp"

#include <stdexcept>
#include <utility>

namespace {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::checkpoint;
using galactic_test::expect;
using eawr::sim::math::Fixed;
template<class T> T take(eawr::core::Result<T> r) {
    if (!r) throw std::runtime_error(r.error().message);
    return std::move(r).value();
}
void take(eawr::core::Result<void> r) { if (!r) throw std::runtime_error(r.error().message); }
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
    auto game = take(eawr::campaign::GameSession::create(config));
    order(game, 1, gc::PrepareGalacticAbility{hero, target, gc::AbilityKind::technology_theft}); hashes.push_back(take(game.step(pool)).state_hash.get());
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
} // namespace
int main() {
    ability_fixture::arm_watchdog();
    try {
        std::vector<std::string> baseline; eawr::platform::ThreadWorkerAdapter one(1); cases(one, baseline); lifecycle(one, baseline); parent_loss(one, baseline);
        for (std::size_t workers : {2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter pool(workers); std::vector<std::string> hashes; cases(pool, hashes); lifecycle(pool, hashes); parent_loss(pool, hashes);
            expect(hashes == baseline, "theft effects and restore hashes equal on1/2/4/8");
        }
        observer_and_idle();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
