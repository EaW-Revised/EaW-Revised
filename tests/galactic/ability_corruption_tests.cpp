#include "galactic_support.hpp"
#include "ability_fixture.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/ability_host.hpp"
#include "eawr/sim/galactic/income.hpp"
#include "eawr/sim/tactical/admission.hpp"

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
constexpr gc::TypeId unlock = (std::uint64_t{1} << 40) + 17;
gc::GalacticSetup setup(bool mission = false) {
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
        if (mission) { p.slots[0].mode = 2; p.slots[0].map = "AUTHORED"; p.slots[0].script = "PLOT"; }
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
void missions(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    const auto config = setup(true);
    auto refused = take(eawr::campaign::GameSession::create(config)); const auto prepared = prepare(refused, config, pool, hashes);
    order(refused, 2, gc::ExecutePreparedAbility{prepared}); hashes.push_back(take(refused.step(pool)).state_hash.get());
    expect(refused.galaxy().snapshot()->player_economy()->players[1].credits == f(900) && refused.return_state().heroes.empty(),
        "GameSession's missing authored map builder refuses after debit and before actor consumption");
    auto world = take(gc::GalacticSession::create(config)); std::vector<gc::HeroReturnRecord> returns;
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
    config.observer->local_player = 1;
    auto foreign = take(eawr::campaign::GameSession::create(config)); (void)prepare(foreign, config, pool, unused);
    expect(!foreign.galaxy().snapshot()->corruption_menu(), "foreign observer never receives an actor's corruption capability");
    config.observer->local_player = 2;
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
    other_view.observer->local_player = 1;
    const auto foreign_view = gc::GalacticSnapshot::native_view(publication, std::move(other_view));
    expect(foreign_view->corruption_transitions().empty(),
        "observer replacement never borrows another player's admitted transition facts");
    const auto stacked_foreign = gc::GalacticSnapshot::native_view(foreign_view, {});
    expect(stacked_foreign->observer() && stacked_foreign->observer()->local_player == 1
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
        }, [](const campaign::ConflictRecord&) { return eawr::core::Result<tactical::TacticalContent>::success(mission_content()); });
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
    cp::GameLoadOptions load; load.configure_session = [&](campaign::GameSession& session) { install(session); return eawr::core::Result<void>::success(); };
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
} // namespace
int main() {
    ability_fixture::arm_watchdog();
    try {
        using Phase = void (*)(const eawr::sim::PartitionExecutor&, std::vector<std::string>&);
        const Phase phases[] = {direct, missions, stale_units, representative, unsuccessful_missions, accepted_space_mission, startup};
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
