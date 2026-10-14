#include "ability_ruins_support.hpp"
#include "eawr/platform/sim_workers.hpp"

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
} // namespace
int main() {
    try {
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
