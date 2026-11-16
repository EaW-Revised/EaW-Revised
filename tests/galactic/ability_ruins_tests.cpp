#include "ability_ruins_support.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/campaign/tactical_return.hpp"
#include "eawr/sim/galactic/victory.hpp"
#include <optional>
#include <stdexcept>

// WGCH-67/140..143/176..179: the shared live ruins adapter (native effect 17).
namespace {
using namespace ability_fixture;
void expect_ruined(const gc::GalacticSession& world, const char* label) {
    const auto snapshot = world.snapshot(); const auto& surface = snapshot->control()->planets[0];
    expect(surface.destroyed && snapshot->economy_corruption()->planets[0].destroyed, label);
    expect(surface.landed.empty() && !present(world, transport) && !present(world, hero) && !present(world, companion),
        "every landed transport and its real contents leave through canonical loss");
    expect(surface.ground_base.level == 0 && !object(world, ground_structure) && object(world, orbital_structure),
        "ground special structures and base level clear while space structures remain");
    expect(snapshot->production()->planets[0].ground_slots == 0 && snapshot->production()->planets[0].space_slots == 3,
        "land capacity zeroes without changing space capacity");
    expect(surface.transition_modifiers.empty() && !surface.surface_accessible && !snapshot->production()->planets[0].surface_accessible,
        "political modifiers clear and surface readers return false");
    expect(!snapshot->economy_corruption()->planets[0].siphon.active
        && snapshot->economy_corruption()->planets[0].corruption == gc::corruption_fixture::Type::none,
        "siphon terminates and the old corruption is cleaned");
    expect(snapshot->observer()->planets[0].landed_transports.empty() && snapshot->observer()->planets[0].ground_level == 0,
        "observer landed and base facts follow the ordered commit");
    for (const auto& record : snapshot->observer()->records) expect(record.sources.empty(), "structure influence detaches from every observer record");
}
void notice_entry_latch() {
    for (const bool throws : {false, true}) {
        const auto config = ruins_setup(false); auto game = take(eawr::campaign::GameSession::create(config));
        std::unique_ptr<gc::GalacticSession::NativeOutputPhase> output; std::size_t callbacks{};
        eawr::testing::must(game.configure_native_replay_owners([&](auto& world) {
            output = take(world.begin_native_output_phase());
            world.set_ability_consumption_handler([&](const auto&) -> eawr::core::Result<void> {
                ++callbacks;
                expect(output->published(), "story_notice entry publishes before hero cleanup even without an opaque story handler");
                if (throws) throw std::runtime_error("injected fault after story notice entry");
                eawr::core::Diagnostic error; error.code = "INJECTED-AFTER-STORY-NOTICE";
                return eawr::core::Result<void>::failure(std::move(error));
            });
            return eawr::core::Result<void>::success();
        }));
        bool refused{};
        try { refused = !game.request_planet_ruins(config.planets[0].id); } catch (const std::runtime_error&) { refused = true; }
        output->record_fault();
        expect(refused && callbacks == 1 && output->published() && game.galaxy().native_post_publication_faults() == 1,
            "fault after actual notice entry is retained as a published-output refusal, with no callback retry");
        expect(!game.galaxy().snapshot()->control()->planets[0].destroyed && present(game.galaxy(), hero),
            "notice-before-cleanup ordering stays unchanged when later hero consumption fails");
    }
}
void ruins(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes, bool with_companion) {
    const auto config = ruins_setup(with_companion); const auto planet = config.planets[0].id;
    auto game = take(eawr::campaign::GameSession::create(config)); std::size_t notices = 0;
    game.set_story_notice_handler([&](const eawr::campaign::GameStoryNotice& notice) {
        const auto* travel = std::get_if<gc::story::TravelNotice>(&notice.payload);
        if (travel && travel->kind == gc::story::Travel::planet_destroyed) ++notices;
        return eawr::core::Result<void>::success();
    });
    const auto* before = game.galaxy().snapshot()->economy_forecast();
    expect(before && before->planets[0].basic == f(100), "an intact planet reads its ordinary base income");
    expect(take(game.request_planet_ruins(planet)), "the shared request applies to a live planet");
    expect(notices == 1, "the destroyed-planet story notification is delivered once");
    expect_ruined(game.galaxy(), "destroyed flag is written");
    const auto* after = game.galaxy().snapshot()->economy_forecast();
    expect(after && after->planets[0].basic == f(10), "ruined fiscal base switches to the authored destroyed value");
    const auto& returns = game.return_state().heroes;
    if (with_companion) expect(returns.empty(), "a surviving companion leaves WGM-79's parent check unsatisfied: no return is scheduled");
    else expect(returns.size() == 1 && returns[0].defeated == hero && returns[0].planet == planet && returns[0].owner == 2
        && returns[0].deadline == game.galaxy().clock().galactic_frame + 360 * 30,
        "the named hero requests its respawn through the shared owner with the default time and the current system");
    const auto effects = game.galaxy().native_effects().size();
    expect(!take(game.request_planet_ruins(planet)) && game.galaxy().native_effects().size() == effects,
        "an already ruined planet applies nothing and journals nothing");
    expect(!take(game.request_planet_ruins(planet + 100000)) && game.galaxy().native_effects().size() == effects,
        "an unknown planet refuses without a journal");
    hashes.push_back(take(game.step(pool)).state_hash.get());
    if (!with_companion) {
        // A hero return scheduled by a request applied BETWEEN steps changes the conflict-tail decision the next
        // step makes in the original, but a replay applies the journaled request inside that step. Live requests
        // come from commands and scripts inside a step, so the reconstruction check uses the return-free variant.
        for (int i = 0; i < 40; ++i) hashes.push_back(take(game.step(pool)).state_hash.get());
        return;
    }
    const auto ids = cp::game_identities(config); const auto saved = take(cp::save_game_checkpoint(game, ids));
    auto fresh = take(cp::load_game_checkpoint(saved, ids, {}, pool));
    expect(take(cp::save_game_checkpoint(fresh.session, ids)) == saved, "fresh reconstruction replays native effect 17 exactly once");
    expect_ruined(fresh.session.galaxy(), "reconstructed ruins match");
    for (int i = 0; i < 40; ++i) {
        hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(take(fresh.session.step(pool)).state_hash.get() == hashes.back(), "saved ruined continuation equals the uninterrupted state");
    }
}
void without_flag(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    const auto config = ruins_setup(false); const auto planet = config.planets[0].id;
    auto game = take(eawr::campaign::GameSession::create(config)); std::size_t notices = 0;
    game.set_story_notice_handler([&](const eawr::campaign::GameStoryNotice& notice) {
        if (std::holds_alternative<gc::story::TravelNotice>(notice.payload)) ++notices;
        return eawr::core::Result<void>::success();
    });
    expect(take(game.request_planet_ruins(planet, false)), "cleanup without the destroy flag applies");
    const auto snapshot = game.galaxy().snapshot(); const auto& surface = snapshot->control()->planets[0];
    expect(notices == 0 && !surface.destroyed && snapshot->production()->planets[0].ground_slots == 3 && surface.surface_accessible,
        "WGCH-140: omitting destruction skips story, capacity and flag writes");
    expect(surface.landed.empty() && surface.transition_modifiers.empty() && !object(game.galaxy(), ground_structure),
        "the remaining cleanup still applies");
    hashes.push_back(take(game.step(pool)).state_hash.get());
    expect(take(game.request_planet_ruins(planet, true)) && notices == 1 && game.galaxy().snapshot()->control()->planets[0].destroyed,
        "a later full request completes the destruction");
    hashes.push_back(take(game.step(pool)).state_hash.get());
}
void aborts_transition(const eawr::sim::PartitionExecutor& pool, std::vector<std::string>& hashes) {
    auto config = ruins_setup(false); config.economy_corruption->planets[0].corruption = gc::corruption_fixture::Type::none;
    config.economy_corruption->planets[0].siphon = {};
    auto world = take(gc::GalacticSession::create(config)); std::vector<gc::HeroReturnRecord> returns;
    world.set_ability_consumption_handler([&](const gc::HeroDefeat& defeat) {
        auto scheduled = gc::schedule_hero_return(returns, defeat, world.clock().galactic_frame, world.snapshot()->planets());
        if (!scheduled) return eawr::core::Result<void>::failure(scheduled.error());
        std::vector<eawr::sim::EntityId> removed = scheduled.value().destroyed_companions;
        removed.insert(removed.end(), defeat.removed_objects.begin(), defeat.removed_objects.end());
        removed.push_back(defeat.id); std::sort(removed.begin(), removed.end());
        removed.erase(std::unique(removed.begin(), removed.end()), removed.end());
        return world.apply_battle_losses(removed);
    });
    const auto planet = config.planets[0].id;
    expect(take(world.begin_corruption(planet, gc::corruption_fixture::Type::racketeering, f(5))) && world.ability_state()->transitions[0].transition.active(),
        "a corruption transition begins before ruins");
    expect(take(world.request_planet_ruins(planet)), "ruins applies over an active transition");
    expect(!world.ability_state()->transitions[0].transition.active(), "WGCH-67: the active transition aborts immediately");
    const auto cash = world.snapshot()->player_economy()->players[0].credits;
    for (int i = 0; i < 400; ++i) hashes.push_back(take(world.step(pool)).state_hash.get());
    expect(world.economy_corruption()->planets[0].corruption == gc::corruption_fixture::Type::none
        && world.snapshot()->player_economy()->players[0].credits >= cash, "an aborted transition never completes later or rewards");
}
// #7187 / TSW-10: a battle result that carries the planet fact commits the ruins once, through the shared request.
void battle_planet_fact() {
    const auto config = ruins_setup(false); const auto planet = config.planets[0].id;
    auto world = take(gc::GalacticSession::create(config)); std::vector<gc::HeroReturnRecord> returns;
    world.set_ability_consumption_handler([&](const gc::HeroDefeat& defeat) {
        auto scheduled = gc::schedule_hero_return(returns, defeat, world.clock().galactic_frame, world.snapshot()->planets());
        if (!scheduled) return eawr::core::Result<void>::failure(scheduled.error());
        std::vector<eawr::sim::EntityId> removed = scheduled.value().destroyed_companions;
        removed.insert(removed.end(), defeat.removed_objects.begin(), defeat.removed_objects.end());
        removed.push_back(defeat.id); std::sort(removed.begin(), removed.end());
        removed.erase(std::unique(removed.begin(), removed.end()), removed.end());
        return world.apply_battle_losses(removed);
    });
    eawr::platform::ThreadWorkerAdapter executor(1);
    eawr::campaign::ConflictRecord conflict; conflict.planet = planet; conflict.layer = eawr::campaign::BattleLayer::space;
    eawr::campaign::TacticalReturnPlan plan; eawr::campaign::TacticalReturnState state;
    eawr::testing::must(eawr::campaign::apply_tactical_return(world, conflict, plan, state, executor));
    expect(!world.snapshot()->control()->planets[0].destroyed, "a battle result without the planet fact leaves the planet intact");
    const auto before = world.native_effects().size();
    plan.result.planet_destroyed = true;
    eawr::testing::must(eawr::campaign::apply_tactical_return(world, conflict, plan, state, executor));
    expect(world.snapshot()->control()->planets[0].destroyed && world.snapshot()->economy_corruption()->planets[0].destroyed,
        "the battle's planet fact commits the planet's ruins");
    const auto effects = world.native_effects().size();
    eawr::testing::must(eawr::campaign::apply_tactical_return(world, conflict, plan, state, executor));
    expect(world.native_effects().size() == effects, "a repeated return of a destroyed planet commits nothing more");
    // The request journals the same native effect the ability route does, so save and load of a ruined planet is the
    // reconstruction `ruins` above already checks through the game checkpoint.
    expect(world.native_effects().size() == before + 1, "the battle's planet fact is one journaled ruins effect");
}
// WGSI-11 / WGEP-12: the income forecast publishes the credits the view player's own siphon takes from a planet (the
// planet's pre-siphon income times the fraction, the value the payment uses); another player's siphon, an ended one
// and an unsiphoned planet publish nothing.
void siphon_forecast() {
    const auto siphoned = [](const gc::GalacticSetup& config) {
        auto world = take(gc::GalacticSession::create(config));
        const auto* forecast = world.snapshot()->economy_forecast();
        expect(forecast && forecast->player == 2, "the forecast is the view player's");
        std::vector<std::optional<Fixed>> values;
        if (forecast) for (const auto& row : config.planets) {
            const auto found = std::find_if(forecast->planets.begin(), forecast->planets.end(),
                [&](const auto& value) { return value.planet == row.id; });
            values.push_back(found == forecast->planets.end() ? std::nullopt : found->siphoned);
        }
        return values;
    };
    auto config = ruins_setup(false);
    const auto whole = siphoned(config);
    expect(whole.size() == config.planets.size() && whole[0] && *whole[0] > Fixed{},
        "a whole-fraction siphon of the view player publishes the planet's income");
    for (std::size_t i = 1; i < whole.size(); ++i) expect(!whole[i], "an unsiphoned planet publishes no siphon");
    config.economy_corruption->planets[0].siphon.fraction = Fixed::from_raw(Fixed::scale / 4);
    const auto quarter = siphoned(config);
    expect(whole[0] && !quarter.empty() && quarter[0] == take(eawr::sim::math::multiply(*whole[0], Fixed::from_raw(Fixed::scale / 4))),
        "the published value is the pre-siphon income times the fraction");
    auto foreign = config; foreign.economy_corruption->planets[0].siphon.recipient = 1;
    const auto other = siphoned(foreign);
    expect(!other.empty() && !other[0], "another player's siphon is not published to the view player");
    auto inactive = config; inactive.economy_corruption->planets[0].siphon.active = false;
    const auto ended = siphoned(inactive);
    expect(!ended.empty() && !ended[0], "an ended siphon publishes nothing");
}
// WGT-102 / WGS-79: a tactical super weapon's obliteration tests the controller's leader among the landed transport
// contents before the ruins, with the condition's authored leader names, and credits the weapon's owner.
void tactical_obliteration_leader_kill() {
    const auto with_victory = [](std::vector<std::string> leaders, gc::PlayerId actor) {
        auto config = ruins_setup(false);
        gc::VictoryContext context;
        for (const auto& row : config.players) {
            gc::VictoryPlayer player; player.player = row.player_id; player.faction = "F" + std::to_string(row.player_id);
            player.neutral = row.player_id == neutral_player; player.playable = !player.neutral;
            player.human = row.player_id == actor; player.ai = !player.neutral && !player.human;
            player.planet_leader_unit = "Hero_Leader"; player.planet_leader_type = 1000; // the landed hero's type
            context.players.push_back(player);
        }
        for (const auto& a : context.players) for (const auto& b : context.players)
            context.relations.push_back({a.player, b.player, a.player == b.player});
        // Setup validation: one victory row per listed planet, in listing order, with its planet type.
        for (const auto& row : config.planets) context.planets.push_back({row.id, 3000, row.owner, row.owner, row.owner, false});
        gc::VictoryAuthoring authoring; authoring.human = {gc::VictoryKind::superweapon_kill_leader};
        authoring.human_parameters.leaders = std::move(leaders);
        config.victory = gc::install_victory(authoring, context, actor);
        return config;
    };
    const gc::PlayerId actor = 1;
    const auto planet = ruins_setup(false).planets[0].id;
    {
        auto world = take(gc::GalacticSession::create(with_victory({"Other_Leader"}, actor)));
        const auto before = world.state_sha256();
        expect(!take(world.apply_tactical_obliteration_victory(actor, planet)) && !world.victory()->pending
            && world.state_sha256() == before, "WGT-102: a leader-name list that does not name the controller's leader refuses");
    }
    for (const auto& names : {std::vector<std::string>{}, std::vector<std::string>{"hero_LEADER"}}) {
        auto world = take(gc::GalacticSession::create(with_victory(names, actor)));
        expect(!take(world.apply_tactical_obliteration_victory(actor, 999999)) && !world.victory()->pending,
            "an unknown planet tests nothing");
        expect(take(world.apply_tactical_obliteration_victory(actor, planet)) && world.victory()->pending
            && world.victory()->pending->player == actor && world.victory()->remaining == 30,
            "WGT-102: the landed leader credits the weapon owner with 30 frames (an empty list, or a case-insensitive match)");
    }
    {
        // The return path: the conflict's superweapon force names the weapon owner, and the test runs before the ruins
        // remove the landed transport that carries the leader.
        auto world = take(gc::GalacticSession::create(with_victory({}, actor)));
        world.set_ability_consumption_handler([&](const gc::HeroDefeat& defeat) {
            std::vector<eawr::sim::EntityId> removed = defeat.removed_objects; removed.push_back(defeat.id);
            std::sort(removed.begin(), removed.end()); removed.erase(std::unique(removed.begin(), removed.end()), removed.end());
            return world.apply_battle_losses(removed);
        });
        eawr::platform::ThreadWorkerAdapter executor(1);
        eawr::campaign::ConflictRecord conflict; conflict.planet = planet; conflict.layer = eawr::campaign::BattleLayer::space;
        conflict.superweapon = 70000;
        eawr::campaign::ConflictForce weapon; weapon.id = 70000; weapon.owner = actor; conflict.forces.push_back(weapon);
        eawr::campaign::TacticalReturnPlan plan; eawr::campaign::TacticalReturnState state;
        plan.result.planet_destroyed = true;
        eawr::testing::must(eawr::campaign::apply_tactical_return(world, conflict, plan, state, executor));
        expect(world.victory()->pending && world.victory()->pending->player == actor
            && world.snapshot()->control()->planets[0].destroyed, "the return credits the leader kill, then commits the ruins");
    }
}
} // namespace
int case_galactic_ability_ruins_tests() {
    try {
        notice_entry_latch();
        battle_planet_fact();
        siphon_forecast();
        tactical_obliteration_leader_kill();
        std::vector<std::string> baseline; eawr::platform::ThreadWorkerAdapter one(1);
        ruins(one, baseline, false); ruins(one, baseline, true); without_flag(one, baseline); aborts_transition(one, baseline);
        for (std::size_t count : {2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter workers(count); std::vector<std::string> hashes;
            ruins(workers, hashes, false); ruins(workers, hashes, true); without_flag(workers, hashes); aborts_transition(workers, hashes);
            expect(hashes == baseline, "ruins cleanup, journal replay and continuation hashes equal on 1/2/4/8");
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
