#include "galactic_support.hpp"
#include "eawr/sim/galactic/victory.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <array>
#include <stdexcept>

namespace {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::checkpoint;
using galactic_test::expect;
using eawr::sim::math::Fixed;
using K = gc::VictoryKind;
template<class T> T take(eawr::core::Result<T> r) {
    if (!r) throw std::runtime_error(r.error().message);
    return std::move(r).value();
}
gc::VictoryContext context() {
    gc::VictoryContext c;
    c.players = {{1, "First", false, true, true, false}, {2, "Second", false, true, false, true}, {3, "Neutral", true, false, false, false}};
    c.planets = {{1, 101, 1, 2, 2, false}, {2, 102, 2, 1, 1, false}};
    c.types = {{"Home", 101}, {"Other", 102}, {"Absent", 103}};
    for (const auto& a : c.players) for (const auto& b : c.players) c.relations.push_back({a.player, b.player, a.player == b.player});
    return c;
}
void predicates() {
    auto c = context(); gc::GalacticVictoryCondition r; r.player = 1;
    r.kind = K::all_planets;
    expect(!take(gc::victory_predicate(r, c)), "WGS-19 hostile planet prevents all-planets");
    c.relations[1].allied = true;
    expect(take(gc::victory_predicate(r, c)), "WGS-19 allied ownership suffices");
    c.planets.front().current = 3;
    expect(!take(gc::victory_predicate(r, c)), "WGS-19 first neutral planet fails");
    c = context(); r.kind = K::control_fraction; r.parameters.control_fraction = Fixed::from_raw(Fixed::scale / 2);
    expect(take(gc::victory_predicate(r, c)), "WGS-22 target exact half passes");
    r.parameters.control_fraction = Fixed::from_raw(Fixed::scale / 2 + 1);
    expect(!take(gc::victory_predicate(r, c)), "WGS-22 greater threshold fails despite owned planet");
    r.kind = K::named_planets; r.parameters.planets = {"hOmE"};
    expect(!take(gc::victory_predicate(r, c)), "WGS-25 exact object owner differs from current allegiance");
    r.parameters.planets = {"Absent"};
    expect(take(gc::victory_predicate(r, c)), "WGS-25 known type without instances passes");
    r.parameters.planets.clear(); expect(take(gc::victory_predicate(r, c)), "WGS-25 empty list passes");
    r.parameters.planets = {"invalid"}; expect(!gc::victory_predicate(r, c), "WGS-25 invalid type reports diagnostic");
    r.kind = K::credits; r.parameters.credits = 10;
    c.players[0].credits = Fixed::from_raw(10 * Fixed::scale - 1);
    expect(!take(gc::victory_predicate(r, c)), "WGS-21 current credits truncate");
    c.players[0].credits = Fixed::from_raw(10 * Fixed::scale + 1);
    expect(take(gc::victory_predicate(r, c)), "WGS-21 target inclusive");
    c.players[0].credits = Fixed::from_raw(-Fixed::scale + 1); r.parameters.credits = 0;
    expect(take(gc::victory_predicate(r, c)), "WGS-21 negative fractional credits truncate towards zero");
    r.kind = K::cycles; r.parameters.cycles = 60; c.fiscal_cycle = 60;
    expect(!take(gc::victory_predicate(r, c)), "WGS-26 cycle limit equality fails");
    c.fiscal_cycle = 61; expect(take(gc::victory_predicate(r, c)), "WGS-26 next cycle passes");
    r.kind = K::no_enemy_planets; r.parameters.factions.clear();
    expect(!take(gc::victory_predicate(r, c)), "WGS-20 current enemy planet fails");
    r.parameters.factions = {"FIRST"}; expect(take(gc::victory_predicate(r, c)), "WGS-20 case-insensitive owner filter");
    r.parameters.factions.clear(); c.multiplayer = true;
    expect(take(gc::victory_predicate(r, c)), "WGS-20 multiplayer ignores nonhuman owner");
    c.multiplayer = false; c.players[0].ai = true;
    expect(take(gc::victory_predicate(r, c)), "WGS-20 AI contender ignores other AI owner");
    c = context(); c.players[0].primary_enemy = 2; c.players[1].home = 101;
    c.players[1].leader_company = 901; c.players[1].leader_unit = "Leader";
    c.leaders = {{2, 901, "Leader", true, false, false, false, 3}};
    c.planets[0].target = 1; r.kind = K::homeworld_and_leader;
    expect(take(gc::victory_predicate(r, c)), "WGS-23 home target plus scheduled leader ignores attribution");
    r.kind = K::kill_leader;
    expect(!take(gc::victory_predicate(r, c)), "WGS-24 another player's kill fails");
    c.leaders[0].killer = 1; expect(take(gc::victory_predicate(r, c)), "WGS-24 attributed scheduled return passes");
    c.leaders[0].scheduled_return = false; c.leaders[0].marked_for_death = true;
    r.parameters.leaders = {"lEaDeR"}; expect(take(gc::victory_predicate(r, c)), "WGS-24 attributed marked object passes");
    r.parameters.leaders = {"Other"}; expect(!take(gc::victory_predicate(r, c)), "WGS-24 name mismatch fails");
    r.kind = K::superweapon_last_planet; expect(!take(gc::victory_predicate(r, c)), "WGS-27 ordinary evaluator does not produce event result");
}
void installation() {
    auto c = context(); gc::VictoryAuthoring a;
    a.human = {K::credits, K::credits}; a.ai = {K::cycles};
    auto s = gc::install_victory(a, c, 1);
    expect(s.installed.size() == 9, "WGS-15..17 duplicate authored/default order preserved");
    expect(s.installed[0].kind == K::credits && s.installed[1].kind == K::credits && s.installed[2].kind == K::all_planets, "WGS-17 defaults follow authored list");
    expect(s.installed[4].kind == K::cycles && s.installed[5].kind == K::no_enemy_planets && s.installed[8].kind == K::no_enemy_planets, "WGS-16 nonempty AI list adds distinct extra default");
    a.ai.clear(); s = gc::install_victory(a, c, 1);
    expect(s.installed.size() == 7, "WGS-17 empty AI list still gets defaults");
    c.multiplayer = true; s = gc::install_victory(a, c, 1);
    expect(s.installed.size() == 10 && s.installed[5].kind == K::credits, "WGS-15 multiplayer AI player uses human authored list");
    for (const auto& row : s.installed) expect(row.delay == 30, "WGS-15 all installations use 30 frames");
}
gc::GalacticSetup leader_setup() {
    gc::SyntheticGalaxyOptions spec; spec.players = 3; spec.planets = 2; spec.fleets_per_player = 0;
    auto setup = gc::synthetic_galaxy(spec);
    setup.rules.fiscal_cycle_frames = 1000;
    auto c = context(); c.players[0].primary_enemy = 2;
    c.players[1].home = 101; c.players[1].leader_company = 901;
    c.players[1].leader_unit = c.players[1].planet_leader_unit = "Leader";
    c.players[1].leader_unit_type = c.players[1].planet_leader_type = 902;
    gc::VictoryAuthoring a; a.human = {K::kill_leader, K::homeworld_and_leader};
    setup.victory = gc::install_victory(a, c, 1);
    setup.player_economy.emplace(); setup.fiscal.emplace(); setup.fleet_control.emplace(); setup.production.emplace();
    setup.production->maximum_human_queue = 5;
    const auto one = Fixed::from_raw(Fixed::scale);
    setup.rules.maximum_fleet_movement_distance = one;
    for (const auto& player : setup.players) {
        setup.player_economy->players.push_back({player.player_id, {}, Fixed::from_raw(1000 * Fixed::scale), 0, 5});
        setup.fiscal->players.push_back({player.player_id, true, one});
        const bool human = std::any_of(setup.seats.begin(), setup.seats.end(), [&](const auto& seat) {
            return seat.player == player.player_id && seat.kind == gc::SeatKind::human;
        });
        setup.production->players.push_back({player.player_id, human, !human, human, {}, {}, {}, {}, {}});
    }
    for (const auto& planet : setup.planets) {
        setup.fiscal->planets.push_back({planet.id, {}});
        gc::PlanetProduction row; row.planet = planet.id; row.target_owner = planet.owner;
        setup.production->planets.push_back(row);
    }
    setup.census.emplace(); setup.census->types = {{901, {}, 0, {902, 903}, true, 903}, {902, {}}, {903, {}}};
    setup.census->players = {{1, 0, false, {}}, {2, 0, false, {}}, {3, 0, false, {}}};
    setup.census->objects = {{20, 902, 2, 1, 30, false, false, 902}, {21, 902, 2, 1, 31, false, false, 902},
        {30, 901, 2, 1, 0, false, false, 903}, {31, 901, 2, 1, 0, false, false, 903}};
    return setup;
}
void live_leader_lifecycle() {
    std::vector<std::string> baseline;
    for (const std::size_t n : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(n);
        const auto setup = leader_setup();
        auto world = take(gc::GalacticSession::create(setup));
        expect(world.victory()->context.leaders.size() == 2
            && world.victory()->context.leaders[0].object == 20 && world.victory()->context.leaders[1].object == 21,
            "WGS-24 sparse live leader projection initializes in active object order");
        gc::HeroDefeat defeat; defeat.id = 31; defeat.type = defeat.respawn_type = 901;
        defeat.owner = 2; defeat.killer = 1; defeat.planet = 1;
        auto applied = world.apply_hero_defeat_victory(defeat, {}, executor);
        if (!applied) throw std::runtime_error(applied.error().message);
        expect(!world.victory()->pending && world.victory()->context.leaders[1].killer == 1,
            "WGS-24 later active leader's exact kill does not bypass the first living matching unit");
        auto changed = world.change_actor_current_type(20, 903);
        if (!changed) throw std::runtime_error(changed.error().message);
        expect(world.victory()->context.leaders.size() == 1 && world.victory()->context.leaders[0].object == 21,
            "WGS-24 sparse current-type change removes the leader binding without original-type fallback");
        static_cast<void>(take(world.step(executor)));
        expect(world.victory()->pending && world.victory()->pending->kind == K::kill_leader && world.victory()->pending->player == 1,
            "WGS-24 admitted periodic test reads the current active projection and retained final blow");

        auto game = take(eawr::campaign::GameSession::create(setup));
        defeat.named = true; defeat.default_seconds = Fixed::from_raw(2 * Fixed::scale);
        expect(take(game.schedule_hero_defeat(defeat, executor)).scheduled && game.galaxy().victory()->pending,
            "WGCH-76 scheduled company return admits the attributed enemy leader kill before deletion");
        const auto pending = game.galaxy().victory()->pending;
        const auto event_count = game.galaxy().snapshot()->events().size();
        expect(!take(game.schedule_hero_defeat(defeat, executor)).scheduled
            && game.galaxy().victory()->pending == pending && game.galaxy().snapshot()->events().size() == event_count,
            "duplicate lifecycle report neither reschedules nor republishes an existing pending result");
        const auto ids = cp::game_identities(setup);
        auto restored = take(cp::load_game_checkpoint(take(cp::save_game_checkpoint(game, ids)), ids, {}, executor));
        expect(game.galaxy().victory() == restored.session.galaxy().victory()
            && game.return_state() == restored.session.return_state(), "WGS-57 leader attribution and scheduled return identities restore together");
        std::vector<std::string> hashes;
        for (int i = 0; i < 5; ++i) {
            static_cast<void>(take(game.step(executor))); static_cast<void>(take(restored.session.step(executor)));
            expect(game.state_sha256() == restored.session.state_sha256(), "leader checkpoint continuation equal");
            hashes.push_back(game.state_sha256());
        }
        game.set_hero_placement([](const auto&, const auto) { return eawr::core::Result<bool>::success(true); });
        restored.session.set_hero_placement([](const auto&, const auto) { return eawr::core::Result<bool>::success(true); });
        for (int i = 0; i < 60; ++i) {
            static_cast<void>(take(game.step(executor))); static_cast<void>(take(restored.session.step(executor)));
            expect(game.state_sha256() == restored.session.state_sha256(), "due return leader projection continuation equal");
            hashes.push_back(game.state_sha256());
        }
        expect(game.return_state().heroes.empty()
            && std::none_of(game.galaxy().victory()->context.leaders.begin(), game.galaxy().victory()->context.leaders.end(),
                [](const auto& row) { return row.scheduled_return; }), "due return retirement removes its exact scheduled leader projection");
        if (n == 1) baseline = hashes; else expect(hashes == baseline, "1/2/4/8 live leader lifecycle hashes equal");

        auto other = take(eawr::campaign::GameSession::create(setup));
        defeat.killer = 3;
        static_cast<void>(take(other.schedule_hero_defeat(defeat, executor)));
        expect(!other.galaxy().victory()->pending && other.galaxy().victory()->context.leaders.front().killer == 3,
            "WGS-24 another player's attributed scheduled kill does not credit the contender");
        auto refused = take(eawr::campaign::GameSession::create(setup));
        defeat.named = false; defeat.id = 30; defeat.killer = 1;
        expect(!take(refused.schedule_hero_defeat(defeat, executor)).scheduled && refused.galaxy().victory()->pending,
            "WGCH-76 tests the first attributed active leader even when scheduling refuses");
    }
}
void live_destruction_callback() {
    for (const std::size_t n : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(n);
        auto setup = leader_setup();
        auto c = setup.victory->context;
        c.players[0].planet_leader_type = 903; // actor's leader is not the controller's.
        gc::VictoryAuthoring a; a.human = {K::superweapon_kill_leader};
        a.human_parameters.leaders = {"Unrelated_Tactical_Filter"};
        setup.victory = gc::install_victory(a, c, 1);
        setup.victory->waits = 1;
        const std::array<gc::TypeId, 1> original_only{901}, controller_leader{902}, actor_leader{903};
        auto world = take(gc::GalacticSession::create(setup));
        const auto original = world.state_sha256();
        expect(!take(world.apply_planet_destruction_victory(1, 2, original_only))
            && !take(world.apply_planet_destruction_victory(1, 2, actor_leader))
            && !take(world.apply_planet_destruction_victory(1, 3, controller_leader))
            && world.state_sha256() == original, "WGS-79 only the current controller's exact landed current leader type qualifies");
        expect(take(world.apply_planet_destruction_victory(1, 2, controller_leader))
            && world.victory()->pending->player == 1 && world.victory()->remaining == 30
            && world.victory()->waits == 1 && world.victory()->queued.empty(),
            "WGS-79 strategic leader result credits actor, ignores tactical name list and registers directly under waits");
        const auto events = world.snapshot()->events().size();
        const auto registered = world.state_sha256();
        expect(!take(world.apply_planet_destruction_victory(1, 2, controller_leader))
            && world.state_sha256() == registered && world.snapshot()->events().size() == events,
            "WGS-30 repeated destruction callback preserves the first pending result and publication");
        auto game = take(eawr::campaign::GameSession::create(setup));
        expect(static_cast<bool>(game.restore_victory_state(*world.victory())), "game checkpoint owns the admitted live destruction state");
        const auto ids = cp::game_identities(setup);
        auto restored = take(cp::load_game_checkpoint(take(cp::save_game_checkpoint(game, ids)), ids, {}, executor));
        expect(game.galaxy().victory() == restored.session.galaxy().victory(), "WGS-57 destruction pending and retained wait survive fresh restore");
        setup.victory = gc::install_victory({}, c, 1);
        world = take(gc::GalacticSession::create(setup));
        expect(!take(world.apply_planet_destruction_victory(1, 2, controller_leader)) && !world.victory()->pending,
            "WGS-79 absent leader condition cannot admit a result; action execution has its separate owner");
        setup.victory.reset(); world = take(gc::GalacticSession::create(setup));
        expect(!take(world.apply_planet_destruction_victory(1, 2, controller_leader)), "campaign without monitor supplies no destruction result");
    }
}
void live_story_rewards() {
    namespace story = gc::story;
    std::vector<std::string> baseline;
    for (const bool suppress : {false, true}) for (const std::size_t n : {1U, 2U, 4U, 8U}) {
        gc::SyntheticGalaxyOptions spec; spec.players = 3; spec.planets = 2; spec.fleets_per_player = 0;
        auto setup = gc::synthetic_galaxy(spec);
        gc::VictoryAuthoring a; a.human = {K::credits};
        setup.victory = gc::install_victory(a, context(), 1);
        setup.victory->waits = 2;
        setup.victory->queued = {{K::credits, 1}, {K::cycles, 2}};
        auto game = take(eawr::campaign::GameSession::create(setup));
        eawr::platform::ThreadWorkerAdapter executor(n);
        std::vector<std::string> notices;
        game.set_story_notice_handler([&](const eawr::campaign::GameStoryNotice& notice) {
            if (const auto* battle = std::get_if<story::BattleNotice>(&notice.payload)) {
                expect(!notice.player && battle->kind == story::Battle::victory && battle->actor == 1,
                    "WGS-179 story registration broadcasts the exact winner faction");
                notices.push_back(battle->faction);
            }
            return eawr::core::Result<void>::success();
        });
        const auto before = game.state_sha256();
        expect(game.apply_story_reward(story::VictoryReward{"", suppress}, executor).status == story::RewardStatus::inert,
            "WGS-173 empty faction is inert");
        expect(game.apply_story_reward(story::VictoryReward{"Unknown", suppress}, executor).status == story::RewardStatus::inert
            && game.state_sha256() == before, "WGS-173 unresolved faction installs no monitor state");
        expect(game.apply_story_reward(story::VictoryWaitReward{false, false}, executor).status == story::RewardStatus::inert
            && game.state_sha256() == before, "WGS-174 missing enable parameter is inert");
        expect(game.apply_story_reward(story::VictoryReward{"fIrSt", suppress}, executor).status == story::RewardStatus::applied,
            "WGS-173 active-mode reward reaches the authoritative monitor");
        const auto& monitor = *game.galaxy().victory();
        expect(monitor.pending && monitor.pending->kind == K::story_forced && monitor.pending->player == 1
            && monitor.pending->suppress_text == suppress && monitor.remaining == (suppress ? 30 : 210)
            && monitor.waits == 0 && monitor.queued == setup.victory->queued,
            "WGS-172/173 clears all waits without replay and retains forced kind and 7/1-second delay");
        expect(notices == std::vector<std::string>{"First"}, "WGS-179 forced registration notifies before maturity once");
        const auto ids = cp::game_identities(setup);
        const auto bytes = take(cp::save_game_checkpoint(game, ids));
        std::size_t restore_notices{};
        cp::GameLoadOptions options;
        options.configure_session = [&](auto& session) {
            session.set_story_notice_handler([&](const auto&) { ++restore_notices; return eawr::core::Result<void>::success(); });
            return eawr::core::Result<void>::success();
        };
        auto restored = take(cp::load_game_checkpoint(bytes, ids, options, executor));
        expect(game.galaxy().victory() == restored.session.galaxy().victory() && !restore_notices,
            "WGS-57 fresh checkpoint restore hydrates story state without reward/notice replay");
        std::vector<std::string> hashes{game.state_sha256()};
        std::size_t matured{};
        for (int i = 0; i < (suppress ? 35 : 215); ++i) {
            const auto tick = take(game.step(executor)); static_cast<void>(take(restored.session.step(executor)));
            expect(game.state_sha256() == restored.session.state_sha256(), "story pending checkpoint continuation equal");
            hashes.push_back(game.state_sha256());
            for (const auto& event : tick.galaxy->events()) if (event.kind == gc::GalacticEventKind::victory_matured) ++matured;
        }
        expect(matured == 1 && notices.size() == 1 && !restore_notices, "forced result matures once without duplicate registration notices");
        auto final_restored = take(cp::load_game_checkpoint(take(cp::save_game_checkpoint(game, ids)), ids, options, executor));
        expect(final_restored.session.galaxy().victory() == game.galaxy().victory(), "matured story dispatch checkpoint retained");
        if (n == 1) baseline = hashes; else expect(hashes == baseline, "1/2/4/8 story checkpoint hashes equal");

        auto waiting = take(eawr::campaign::GameSession::create(setup));
        expect(waiting.apply_story_reward(story::VictoryWaitReward{true}, executor).status == story::RewardStatus::applied
            && waiting.galaxy().victory()->waits == 1 && !waiting.galaxy().victory()->pending,
            "WGS-174 enabling decrements one nested wait");
        expect(waiting.apply_story_reward(story::VictoryWaitReward{true}, executor).status == story::RewardStatus::applied
            && waiting.galaxy().victory()->pending->kind == K::credits && waiting.galaxy().victory()->queued.empty(),
            "WGS-174 last enable replays retained tests in order");
        const auto pending = waiting.galaxy().victory()->pending;
        static_cast<void>(waiting.apply_story_reward(story::VictoryWaitReward{false}, executor));
        static_cast<void>(waiting.apply_story_reward(story::VictoryReward{"Second", suppress}, executor));
        expect(waiting.galaxy().victory()->pending == pending && waiting.galaxy().victory()->waits == 0,
            "WGS-173 existing different pending result survives forced reward");
    }
}
void pending_and_parity() {
    std::vector<std::uint8_t> baseline;
    for (const std::size_t n : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter workers(n);
        auto spatial = context(); spatial.relations[1].allied = true;
        gc::VictoryAuthoring spatial_authoring;
        auto spatial_state = gc::install_victory(spatial_authoring, spatial, 1);
        expect(take(gc::test_victory(spatial_state, {K::all_planets, 1}, spatial, workers, 0)),
            "partitioned all-planets reduction passes on every worker count");
        spatial = context(); spatial.players[1].leader_unit = "Leader"; spatial.players[1].leader_company = 901;
        for (int i = 0; i < 128; ++i) spatial.leaders.push_back({2, 901, "Leader", false, i == 127, false, false, 1});
        spatial_authoring.human = {K::kill_leader};
        spatial_state = gc::install_victory(spatial_authoring, spatial, 1);
        expect(!take(gc::test_victory(spatial_state, {K::kill_leader, 1}, spatial, workers, 0)),
            "partitioned leader reduction respects first active object across partitions");
        spatial.leaders.front().deletion_pending = true;
        expect(take(gc::test_victory(spatial_state, {K::kill_leader, 1}, spatial, workers, 0)),
            "partitioned attributed first-object leader result passes on every worker count");
        auto c = context(); gc::VictoryAuthoring a; a.human = {K::credits};
        auto s = gc::install_victory(a, c, 1);
        gc::service_victory(s, 10, false);
        s.waits = 2;
        expect(!take(gc::test_victory(s, {K::credits, 1}, c, workers, 10)), "WGS-18 wait queues first result");
        expect(!take(gc::test_victory(s, {K::cycles, 2}, c, workers, 10)), "WGS-18 wait queues competing result");
        expect(!take(gc::release_victory_wait(s, true, c, workers, 10)) && s.queued.size() == 2, "WGS-32 nested wait retains queue");
        expect(take(gc::release_victory_wait(s, true, c, workers, 10)), "WGS-32 final release evaluates in order");
        expect(s.pending && s.pending->player == 1 && s.queued.empty(), "WGS-30 first pending wins and queue clears");
        expect(!gc::register_victory(s, K::all_planets, 2, 5, 10), "WGS-30 competing registration rejected");
        gc::service_victory(s, 17, false); expect(s.remaining == 23, "WGS-31 subtract elapsed seven frames");
        std::vector<std::uint8_t> bytes; gc::detail::encode_victory(bytes, s);
        expect(gc::detail::decode_victory(bytes) == std::optional(s), "WGS-57 pending copy and countdown round trip");
        if (baseline.empty()) baseline = bytes; else expect(bytes == baseline, "1/2/4/8 competing results equal");
        gc::GalacticVictoryCondition out; expect(!gc::pending_victory(s, out) && out.player == 0, "WGS-32 failed read resets output");
        gc::service_victory(s, 40, false); s.waits = 1;
        expect(!gc::pending_victory(s, out), "WGS-32 mature pending blocked by waits");
        static_cast<void>(take(gc::release_victory_wait(s, false, c, workers, 40)));
        expect(gc::pending_victory(s, out) && out.player == 1, "WGS-32 mature pending consumable");
        expect(!gc::campaign_result(s, 1, true, false), "WGS-82 solo dialog postpones presentation");
        expect(gc::campaign_result(s, 1, false, false)->local_won, "WGS-82 exact local winner heading");
        s.context.relations[3].allied = true;
        expect(!gc::campaign_result(s, 2, false, false)->local_won, "WGS-82 allied winner is still defeat heading");
        s.context.multiplayer = true;
        expect(gc::campaign_result(s, 2, true, true)->route == gc::CampaignResultRoute::multiplayer_quit,
            "WGS-82 multiplayer bypasses dialog/movie gates and projects quit intent");

        s.use_retry = true; s.remaining = 1; gc::service_victory(s, 41, false);
        expect(s.remaining == 1 && s.retry_waiting, "WGS-31 retry hold at one frame");
    }
}
void return_producers() {
    auto c = context(); gc::VictoryAuthoring a; a.human = {K::destroy_superweapon};
    auto s = gc::install_victory(a, c, 1);
    gc::victory_superweapon_return(s, {2, 1, 2, true, false, true}, c, 7);
    expect(s.pending && s.pending->player == 1 && s.remaining == 5, "WGS-28 winning killer requests five frames");
    s = gc::install_victory(a, c, 1); c.planets[1].destroyed = true;
    gc::victory_superweapon_return(s, {1, 1, 2, true, false, false}, c, 7);
    expect(s.pending && s.remaining == 30, "WGS-29 destroyed last current-enemy planet requests thirty frames");
    s = gc::install_victory(a, c, 1); c.planets[1].destroyed = false; c.planets[1].target = 1;
    gc::victory_superweapon_return(s, {1, 1, 2, true, false, false}, c, 7);
    expect(!s.pending, "WGS-29 current allegiance prevents last-planet result despite target");
    c.players[1].planet_leader_unit = "Leader"; a.human = {K::superweapon_kill_leader};
    a.human_parameters.leaders = {"Different"};
    s = gc::install_victory(a, c, 1);
    const std::vector<std::string> landed{"Leader"}, orbit_only{};
    expect(!gc::victory_leader_planet_destruction(s, 1, 2, orbit_only, false, 0), "WGS-79 orbit-only leader is insufficient");
    expect(!gc::victory_leader_planet_destruction(s, 1, 2, landed, true, 0), "WGS-79 tactical authored name mismatch fails");
    expect(gc::victory_leader_planet_destruction(s, 1, 2, landed, false, 0) && s.remaining == 30,
        "WGS-79 strategic scan skips the tactical name filter and credits actor");

}
void fiscal_admission() {
    std::vector<std::string> baseline;
    for (const std::size_t n : {1U, 2U, 4U, 8U}) {
        gc::SyntheticGalaxyOptions spec; spec.planets = 2; spec.players = 3; spec.fleets_per_player = 0;
        auto setup = gc::synthetic_galaxy(spec);
        setup.rules.fiscal_cycle_frames = 1;
        gc::VictoryAuthoring authored; authored.human = {K::cycles}; authored.human_parameters.cycles = 60;
        setup.victory = gc::install_victory(authored, context(), 1);
        auto world = take(gc::GalacticSession::create(setup));
        eawr::platform::ThreadWorkerAdapter executor(n);
        std::vector<std::string> hashes;
        for (int i = 0; i < 61; ++i) hashes.push_back(take(world.step(executor)).state_hash.get());
        expect(world.clock().fiscal_cycle == 60 && !world.victory()->pending,
            "WGS-26 fiscal callback does not accept cycle-limit equality");
        hashes.push_back(take(world.step(executor)).state_hash.get());
        expect(world.clock().fiscal_cycle == 61 && world.victory()->pending
            && world.victory()->pending->kind == K::cycles && world.victory()->pending->player == 1,
            "WGS-26 fiscal callback accepts the next cycle before timer advance");
        if (n == 1) baseline = hashes; else expect(hashes == baseline, "1/2/4/8 fiscal victory admission hashes equal");
    }
}
void checkpoint() {
    std::vector<std::string> baseline;
    for (const std::uint32_t waits : {0U, 1U}) for (const std::size_t n : {1U, 2U, 4U, 8U}) {
        gc::SyntheticGalaxyOptions spec; spec.planets = 2; spec.players = 3; spec.fleets_per_player = 0;
        auto setup = gc::synthetic_galaxy(spec);
        auto c = context(); gc::VictoryAuthoring a; a.human = {K::credits}; a.ai = {K::cycles};
        setup.rules.fiscal_cycle_frames = 1;
        setup.victory = gc::install_victory(a, c, 1);
        expect(gc::register_victory(*setup.victory, K::credits, 1, 30, 0), "initial pending accepted");
        setup.victory->waits = waits;
        if (waits) setup.victory->queued = {{K::cycles, 2}};
        const auto replay = take(gc::write_replay({setup, 0, {}}));
        expect(take(gc::parse_replay(replay)).setup == setup, "replay tag 14 round trip");
        eawr::platform::ThreadWorkerAdapter executor(n);
        auto game = take(eawr::campaign::GameSession::create(setup));
        for (int i = 0; i < 9; ++i) static_cast<void>(take(game.step(executor)));
        expect(!waits || game.galaxy().victory()->queued.size() > 1,
            "WGS-18 live fiscal admissions queue while a pending result is waiting");
        const auto build = cp::game_identities(setup);
        const auto bytes = take(cp::save_game_checkpoint(game, build));
        auto restored = take(cp::load_game_checkpoint(bytes, build, {}, executor));
        expect(restored.session.galaxy().victory() == game.galaxy().victory(), "WGS-57 checkpoint preserves waits/pending without fresh installation");
        std::vector<std::string> hashes;
        std::size_t matured = 0;
        for (int i = 0; i < 25; ++i) {
            const auto step = take(game.step(executor)); static_cast<void>(take(restored.session.step(executor)));
            expect(game.state_sha256() == restored.session.state_sha256(), "pending checkpoint continuation equal");
            expect(step.state_hash.get() == game.state_sha256(), "outer-tail changes enter published hash");
            hashes.push_back(game.state_sha256());
            for (const auto& event : step.galaxy->events()) if (event.kind == gc::GalacticEventKind::victory_matured) ++matured;
        }
        expect(matured == (waits ? 0U : 1U), "matured result dispatch occurs exactly once and honors wait gate");
        const auto after = take(cp::save_game_checkpoint(game, build));
        auto final_restored = take(cp::load_game_checkpoint(after, build, {}, executor));
        expect(final_restored.session.galaxy().victory() == game.galaxy().victory(), "post-expiry checkpoint preserves dispatch flag");
        if (n == 1) baseline = hashes; else expect(baseline == hashes, "1/2/4/8 pending checkpoint hashes equal");
    }

}
void account_admission() {
    std::vector<std::string> baseline;
    for (const std::size_t n : {1U, 2U, 4U, 8U}) {
        const auto f = [](std::int64_t v) { return take(Fixed::from_integer(v)); };
        gc::SyntheticGalaxyOptions spec; spec.players = 1; spec.planets = 2; spec.fleets_per_player = 0;
        auto setup = gc::synthetic_galaxy(spec);
        setup.rules.fiscal_cycle_frames = 1000;
        setup.rules.maximum_fleet_movement_distance = f(10000);
        setup.fleet_control = gc::FleetControlSetup{{}, {{1000, false, true, false}}, {}};
        setup.player_economy = gc::PlayerEconomySetup{{{1, f(250), f(10000), 2, 5}}};
        setup.fiscal = gc::FiscalSetup{f(2500), {{1, true, f(1)}}, {{1, f(10)}, {2, f(10)}}};
        setup.production = gc::ProductionSetup{5, {{1, true, false, true, f(50), f(0), {}, {}, {}}}, {}};
        gc::PlanetProduction planet; planet.planet = 1; planet.target_owner = 1;
        planet.ground_occupier = 1; planet.enabled = true; planet.surface_accessible = true;
        planet.ground_slots = 3; planet.maximum_ground_forces = 10;
        gc::ProductionOffer offer; offer.type = 1000; offer.price = f(100); offer.seconds = f(10);
        offer.tech = 2; offer.population = f(1); offer.affiliated = true; offer.direct_build_allowed = true;
        planet.offers.push_back(offer); setup.production->planets.push_back(planet);
        planet.planet = 2; setup.production->planets.push_back(planet);
        auto c = context(); c.players.resize(1);
        for (auto& row : c.planets) row.current = row.target = row.owner = 1;
        gc::VictoryAuthoring a; a.human = {K::credits}; a.human_parameters.credits = 200;
        setup.victory = gc::install_victory(a, c, 1);
        eawr::platform::ThreadWorkerAdapter executor(n);
        std::vector<std::string> hashes;
        auto game = take(eawr::campaign::GameSession::create(setup));
        const gc::GameCommand buy{{0, {1}, 1}, gc::ProductionCommand{1, 1000, gc::ProductionAction::buy, gc::ProductionDomain::ground, 0}};
        if (auto r = game.submit(buy); !r) throw std::runtime_error(r.error().message);
        static_cast<void>(take(game.step(executor)));
        expect(game.galaxy().snapshot()->player_economy()->players[0].credits == f(150)
            && !game.galaxy().victory()->pending, "WGEP-16 purchase crosses below threshold before admission");
        const auto ids = cp::game_identities(setup);
        auto restored = take(cp::load_game_checkpoint(take(cp::save_game_checkpoint(game, ids)), ids, {}, executor));
        const gc::GameCommand refund{{1, {1}, 2}, gc::ProductionCommand{1, 1000, gc::ProductionAction::cancel_index, gc::ProductionDomain::ground, 0}};
        for (auto* session : {&game, &restored.session}) {
            if (auto r = session->submit(refund); !r) throw std::runtime_error(r.error().message);
            static_cast<void>(take(session->step(executor)));
        }
        expect(game.galaxy().victory()->pending && game.galaxy().victory()->pending->kind == K::credits,
            "WGEP-16/46 refund crosses above threshold and admits without fiscal service");
        expect(game.state_sha256() == restored.session.state_sha256(), "checkpoint before refund preserves account admission");
        hashes.push_back(game.state_sha256());
        auto after = take(cp::load_game_checkpoint(take(cp::save_game_checkpoint(game, ids)), ids, {}, executor));
        expect(after.session.galaxy().victory() == game.galaxy().victory(), "checkpoint after atomic account callback preserves pending");

        a.human_parameters.credits = 100; setup.victory = gc::install_victory(a, c, 1);
        auto purchased = take(gc::GalacticSession::create(setup));
        if (auto r = purchased.submit(buy); !r) throw std::runtime_error(r.error().message);
        hashes.push_back(take(purchased.step(executor)).state_hash.get());
        expect(purchased.victory()->pending && purchased.victory()->pending->kind == K::credits,
            "WGEP-16 qualifying post-purchase balance admits immediately");

        setup.production.reset(); setup.player_economy->players[0].credits = f(0);
        setup.rules.fiscal_cycle_frames = 1; a.human = {K::cycles, K::credits};
        a.human_parameters.credits = 1; a.human_parameters.cycles = 0;
        setup.victory = gc::install_victory(a, c, 1);
        auto fiscal = take(gc::GalacticSession::create(setup));
        hashes.push_back(take(fiscal.step(executor)).state_hash.get());
        hashes.push_back(take(fiscal.step(executor)).state_hash.get());
        expect(fiscal.victory()->pending && fiscal.victory()->pending->kind == K::credits,
            "WGEP-16 fiscal credits admission precedes competing cycle despite authored order");
        if (n == 1) baseline = hashes; else expect(hashes == baseline, "1/2/4/8 account callback hashes equal");
    }
}
void capture_admission() {
    std::vector<std::string> baseline;
    for (const auto kind : {K::named_planets, K::all_planets, K::homeworld_and_leader}) for (const std::size_t n : {1U, 2U, 4U, 8U}) {
        const auto f = [](std::int64_t v) { return take(Fixed::from_integer(v)); };
        gc::SyntheticGalaxyOptions spec; spec.players = 3; spec.planets = 2; spec.fleets_per_player = 0;
        auto setup = gc::synthetic_galaxy(spec); setup.rules.fiscal_cycle_frames = 1000;
        setup.player_economy = gc::PlayerEconomySetup{{{1, f(100), f(1000), 0, 5}, {2, f(900), f(1000), 0, 5}, {3, f(0), f(0), 0, 5}}};
        setup.fiscal = gc::FiscalSetup{f(1000), {{1, true, f(1)}, {2, true, f(1)}, {3, false, f(1)}}, {{1, f(1)}, {2, f(1)}}};
        setup.control.emplace(); auto& control = *setup.control;
        control.change_seconds = f(7); control.max_ground_forces = 10;
        control.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
        auto c = context(); control.relations = c.relations;
        for (const auto& p : setup.planets) {
            gc::ControlPlanet row; row.planet = p.id; row.type = 100 + p.id;
            row.surface_accessible = true; row.ground_base = {1, 1}; row.capture_reward = f(250);
            control.planets.push_back(row);
            gc::PlanetControl state; state.planet = p.id; state.initialized = true;
            state.current_player = state.target_player = p.owner;
            state.current = state.target = f(1); control.state.push_back(state);
        }
        for (const auto& r : setup.routes) control.routes.push_back({r.id, f(1)});
        gc::VictoryAuthoring a; a.human = {K::named_planets, kind}; a.human_parameters.planets = {"Other"};
        a.ai = {K::named_planets}; // Player 2's empty list is also eligible.
        c.players[0].primary_enemy = 2; c.players[1].home = 101; c.players[1].leader_company = 901;
        c.leaders = {{2, 901, "Leader", true, false, false, false, 3}};
        setup.victory = gc::install_victory(a, c, 1);
        std::erase_if(setup.victory->installed, [&](const auto& row) {
            return row.kind != K::named_planets && row.kind != kind;
        });
        // Contrary authored/storage order must not choose player 2 or named
        // planets ahead of WGM-191's domination/homeworld request for player 1.
        std::stable_sort(setup.victory->installed.begin(), setup.victory->installed.end(),
            [](const auto& left, const auto& right) { return left.player > right.player; });
        eawr::platform::ThreadWorkerAdapter executor(n);
        auto game = take(eawr::campaign::GameSession::create(setup));
        bool conquest_saw_pending = false;
        game.set_control_effects_handler([&](const auto effects, const auto) {
            for (const auto& effect : effects) if (effect.planet == 2 && effect.first_capture) {
                conquest_saw_pending = game.galaxy().victory()->pending.has_value();
            }
            return eawr::core::Result<void>::success();
        });
        const auto tick = take(game.step(executor));
        expect(game.galaxy().victory()->pending && game.galaxy().victory()->pending->kind == kind
            && game.galaxy().victory()->pending->player == 1 && conquest_saw_pending,
            "GFS-08 capture admits result in the same tick before conquest delivery");
        const auto& ledger = tick.galaxy->player_economy()->players;
        expect(ledger[0].credit_cap == f(2000) && ledger[1].credit_cap == f(1000) && ledger[1].credits == f(900),
            "GFS-08 capture refreshes caps after admission with WGEP-17's one-planet minimum");
        const auto events = tick.galaxy->events();
        const auto pending = std::find_if(events.begin(), events.end(), [](const auto& e) { return e.kind == gc::GalacticEventKind::victory_pending; });
        const auto ownership = std::find_if(events.begin(), events.end(), [](const auto& e) { return e.kind == gc::GalacticEventKind::planet_owner_changed && e.subject == 2; });
        expect(pending != events.end() && ownership != events.end() && pending < ownership,
            "GFS-08 pending event precedes ownership/conquest notification");
        const auto ids = cp::game_identities(setup);
        auto restored = take(cp::load_game_checkpoint(take(cp::save_game_checkpoint(game, ids)), ids, {}, executor));
        expect(restored.session.galaxy().victory() == game.galaxy().victory(), "captured pending countdown survives checkpoint");
        std::vector<std::string> hashes{game.state_sha256()};
        for (int i = 0; i < 8; ++i) {
            static_cast<void>(take(game.step(executor))); static_cast<void>(take(restored.session.step(executor)));
            expect(restored.session.state_sha256() == game.state_sha256(), "captured pending checkpoint continuation equal");
            hashes.push_back(game.state_sha256());
        }
        if (n == 1) baseline = hashes; else expect(hashes == baseline, "1/2/4/8 capture callback hashes equal");
        if (kind == K::named_planets) {
            setup.victory->waits = 1;
            auto waiting = take(gc::GalacticSession::create(setup));
            static_cast<void>(take(waiting.step(executor)));
            std::vector<gc::VictoryTest> requests;
            for (const auto& contender : c.players) for (const auto requested :
                {K::all_planets, K::no_enemy_planets, K::control_fraction, K::homeworld_and_leader, K::named_planets})
                requests.push_back({requested, contender.player});
            // WGEP-16: the first-capture windfall produces an account callback
            // after the player-major capture wave and before periodic polling.
            requests.push_back({K::credits, 1});
            for (const auto& contender : c.players) requests.push_back({K::kill_leader, contender.player});
            expect(waiting.victory()->queued == requests && !waiting.victory()->pending,
                "WGM-191/WGS-171 wait queues player-major producer requests once including uninstalled kinds");
        }
    }
}
void periodic_request_order() {
    std::vector<std::string> baseline;
    for (const bool waiting : {false, true}) for (const std::size_t n : {1U, 2U, 4U, 8U}) {
        gc::SyntheticGalaxyOptions spec; spec.players = 3; spec.planets = 2; spec.fleets_per_player = 0;
        auto setup = gc::synthetic_galaxy(spec);
        auto c = context(); c.players[0].primary_enemy = 2; c.players[1].home = 101;
        c.players[1].leader_company = 901; c.players[1].leader_unit = "Leader";
        c.leaders = {{2, 901, "Leader", true, false, false, false, 3}};
        c.planets[0].target = 1;
        gc::VictoryAuthoring a; a.human = {K::homeworld_and_leader, K::kill_leader, K::kill_leader};
        setup.victory = gc::install_victory(a, c, 1);
        setup.victory->waits = waiting ? 1U : 0U;
        std::reverse(setup.victory->installed.begin(), setup.victory->installed.end());
        expect(take(gc::victory_predicate(setup.victory->installed.back(), c)),
            "homeworld fixture is eligible before the periodic callback");
        // Live owner/target facts replace authored facts on admission.
        setup.planets[0].owner = 1;
        eawr::platform::ThreadWorkerAdapter executor(n);
        auto world = take(gc::GalacticSession::create(setup));
        const auto tick = take(world.step(executor));
        expect(tick.work.periodic_checks == 1 && !world.victory()->pending,
            "WGS-171 periodic leader check does not admit an eligible homeworld condition");
        const std::vector<gc::VictoryTest> requests{{K::kill_leader, 1}, {K::kill_leader, 2}, {K::kill_leader, 3}};
        expect(world.victory()->queued == (waiting ? requests : std::vector<gc::VictoryTest>{}),
            "WGS-171 queues one kill-leader request per player in storage order despite duplicate installations");
        const std::vector<std::string> hashes{tick.state_hash.get()};
        if (n == 1) baseline = hashes; else expect(hashes == baseline, "1/2/4/8 periodic request hashes equal");
    }
}
}
int main() {
    try { predicates(); installation(); live_leader_lifecycle(); live_destruction_callback(); live_story_rewards(); pending_and_parity(); return_producers(); fiscal_admission(); checkpoint(); account_admission(); capture_admission(); periodic_request_order(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return galactic_test::failures == 0 ? 0 : 1;
}
