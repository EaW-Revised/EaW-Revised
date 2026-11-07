#include "galactic_support.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/checkpoint/game.hpp"

#include <array>

namespace {
using namespace eawr;
using galactic_test::expect;
namespace gc = sim::galactic;
namespace tt = sim::tactical;
using Game = campaign::GameSession;
using Layer = campaign::BattleLayer;

template<class T> T take(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
void check(core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}
campaign::ConflictRecord conflict(const gc::GalacticSetup& setup, Layer layer) {
    campaign::ConflictRecord out;
    out.id = take(gc::GalacticSession::create(setup)).next_entity_id();
    out.planet = setup.planets.front().id; out.invader = 1; out.defender = 2; out.layer = layer;
    out.tactical.content_identity = setup.content_identity;
    out.tactical.first_free_entity_id = out.id + 1;
    out.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
    for (const auto& fleet : setup.fleets) {
        const auto& member = fleet.members.front();
        out.forces.push_back({member.id, member.type, fleet.owner, fleet.id});
        tt::UnitState unit; unit.entity_id = member.id; unit.type_id = member.type; unit.owner = fleet.owner;
        out.tactical.units.push_back(unit);
    }
    std::sort(out.forces.begin(), out.forces.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    std::sort(out.tactical.units.begin(), out.tactical.units.end(), [](const auto& a, const auto& b) { return a.entity_id < b.entity_id; });
    for (const auto& seat : setup.seats) out.seats.push_back({seat.seat_id, seat.player});
    return out;
}
tt::TacticalContent content(const campaign::ConflictRecord& record) {
    tt::TacticalContent out;
    for (const auto& force : record.forces) {
        if (std::any_of(out.durability.profiles.begin(), out.durability.profiles.end(),
            [&](const auto& row) { return row.type_id == force.type; })) continue;
        tt::DurabilityProfile row; row.type_id = force.type; row.max_hull = sim::math::Fixed::from_integer(100).value();
        out.durability.profiles.push_back(row);
    }
    std::sort(out.durability.profiles.begin(), out.durability.profiles.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    out.victory.condition = tt::VictoryCondition::all_enemy_units_destroyed;
    out.victory.contenders = out.victory.controlled_players = {1, 2};
    out.victory.humans = {1}; out.victory.countdown_frames = 2;
    for (const auto& row : out.durability.profiles) out.victory.relevant_types.push_back(row.type_id);
    return out;
}
struct Calls {
    unsigned entry{}, content{}, scripts{}, configure{}, decorate{}, apply{};
    friend bool operator==(const Calls&, const Calls&) = default;
};
Game::ConflictLayerServices provider(Layer layer, Calls& calls) {
    Game::ConflictLayerServices out;
    out.admission = [](const auto&, const auto&, auto) {
        return core::Result<std::optional<campaign::RecordedAdmission>>::success({});
    };
    out.entry = [&, layer](const auto& record, const auto&, auto) {
        expect(record.layer == layer, "entry uses its registered layer"); ++calls.entry;
        return core::Result<campaign::ResolvedConflictEntry>::success({record, content(record)});
    };
    out.content = [&, layer](const auto& record) {
        expect(record.layer == layer, "content uses its registered layer"); ++calls.content;
        return core::Result<tt::TacticalContent>::success(content(record));
    };
    out.scripts = [&, layer](const auto& record) {
        expect(record.layer == layer, "scripts use their registered layer"); ++calls.scripts;
        script::authoritative::SessionConfig config; config.seed = campaign::child_seed(7, record.id);
        return script::authoritative::ScriptScheduler::create(config, {});
    };
    out.configure = [&](auto&) { ++calls.configure; return core::Result<void>::success(); };
    out.decorate = [&, layer](const auto& record, const auto&, auto&) {
        expect(record.layer == layer, "decorator uses its registered layer"); ++calls.decorate;
        return core::Result<void>::success();
    };
    out.apply = [&, layer](auto&, const auto& record, const auto& result) {
        expect(record.layer == layer && record.planet && record.id == result.conflict,
            "applier receives its registered layer and matching conflict/planet context"); ++calls.apply;
        return core::Result<void>::success();
    };
    return out;
}
void install(Game& game, std::array<Calls, 2>& calls, bool reverse) {
    const std::array<Layer, 2> layers{Layer::space, Layer::land};
    for (unsigned i = 0; i < layers.size(); ++i) {
        const auto index = reverse ? 1 - i : i;
        check(game.install_conflict_layer(layers[index], provider(layers[index], calls[index])));
    }
}
gc::TacticalLayerCommand quit(const campaign::ConflictRecord& record) {
    tt::TacticalReplay replay; replay.setup.players = record.tactical.players; replay.final_tick_count = 1;
    replay.commands.push_back({{0, 2, 1}, {}, tt::QuitPayload{}});
    const auto bytes = take(tt::write_replay(replay));
    const auto header = static_cast<std::size_t>(bytes[10]) | (static_cast<std::size_t>(bytes[11]) << 8U);
    const auto start = header + replay.setup.players.size() * 24 + 4;
    return {record.id, bytes[start + 20], {bytes.begin() + static_cast<std::ptrdiff_t>(start), bytes.end()}};
}
gc::GalacticSetup persistence_setup() {
    auto setup = gc::synthetic_galaxy({7, 2, 2, 1, 1, false});
    setup.players.push_back({3, 3, 0});
    setup.control.emplace(); auto& control = *setup.control;
    control.change_seconds = sim::math::Fixed::from_integer(7).value(); control.max_ground_forces = 10;
    for (const auto& player : setup.players) {
        control.players.push_back({player.player_id, player.player_id == 3, player.player_id != 3, player.faction_id});
        for (const auto& other : setup.players)
            control.relations.push_back({player.player_id, other.player_id, player.player_id == other.player_id});
    }
    for (const auto& fleet : setup.fleets) for (const auto& member : fleet.members)
        if (std::none_of(control.types.begin(), control.types.end(), [&](const auto& row) { return row.type == member.type; }))
            control.types.push_back({member.type, 1, false, false});
    std::sort(control.types.begin(), control.types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    for (const auto& route : setup.routes) control.routes.push_back({route.id, sim::math::Fixed::from_integer(1).value()});
    for (const auto& planet : setup.planets) {
        gc::ControlPlanet row; row.planet = planet.id; row.type = planet.id + 100;
        row.ground_base = {planet.owner, 1};
        row.special_control = {sim::math::Fixed::from_integer(1).value()};
        // Deliberately unregistered mod types; persistence does not instantiate actors.
        for (const auto owner : {2U, 1U, 3U, 1U, 2U, 3U}) {
            gc::TacticalBuiltStructure built; built.type = 700 + row.built_structures.size();
            built.position = {sim::math::Fixed::from_raw(111 + static_cast<std::int64_t>(built.type)),
                sim::math::Fixed::from_raw(-333), sim::math::Fixed::from_raw(777)};
            built.enable_level = static_cast<std::uint32_t>(row.built_structures.size()); built.owner = owner;
            row.built_structures.push_back(built);
        }
        control.planets.push_back(std::move(row));
    }
    return setup;
}
void automatic_persistence() {
    const sim::InlineExecutor executor;
    for (const auto layer : {Layer::land, Layer::space}) for (const auto winner : {1U, 2U}) {
        auto setup = persistence_setup();
        setup.control->planets.front().landed.push_back({5000, setup.control->types.front().type, 3});
        auto game = take(Game::create(setup));
        unsigned space_calls{};
        const auto configure = [&](Game& owner) {
            // Stock layer installation supplies admission/refusal but no return override.
            owner.set_result_applier([&](auto&, const auto&) { ++space_calls; return core::Result<void>::success(); });
            check(owner.install_conflict_layer(Layer::land, {}));
            campaign::AutoResolveOutcomeServices services;
            services.control = [](auto&, const auto&) { return core::Result<void>::success(); };
            owner.set_auto_resolve_outcome_services(std::move(services));
            return core::Result<void>::success();
        };
        check(configure(game));
        campaign::AutoResolveOutcomeApplication application;
        application.planet = setup.planets.front().id; application.planet_type = "ModPlanet";
        application.invader = 1; application.defender = 2; application.land = layer == Layer::land;
        application.result.conflict = game.galaxy().next_entity_id(); application.result.winner = winner;
        // No legacy fix flag or control filter: this specifically exercises the layer writer.
        check(game.stage_auto_resolve_outcome(application));
        const auto identities = checkpoint::game_identities(setup);
        const auto saved = take(checkpoint::save_game_checkpoint(game, identities));
        checkpoint::GameLoadOptions options; options.configure_session = configure;
        auto restored = take(checkpoint::load_game_checkpoint(saved, identities, options, executor)).session;
        expect(restored.galaxy().snapshot()->control()->planets == game.galaxy().snapshot()->control()->planets,
            "pending automatic return checkpoint retains complete mixed-owner records");
        auto expected = setup.control->planets.front().built_structures;
        if (layer == Layer::land) std::erase_if(expected, [&](const auto& row) { return row.owner != winner; });
        for (auto* owner : {&game, &restored}) {
            const auto returned = take(owner->step(executor));
            expect(returned.result && returned.result->winner == winner, "staged automatic return completed");
            const auto& planets = returned.galaxy->control()->planets;
            expect(planets.front().built_structures == expected,
                "land dispatcher keeps only winner records intact in retained order; space keeps every record");
            expect(planets.back().built_structures == setup.control->planets.back().built_structures,
                "return filters only its conflict planet");
            expect(planets.front().special_control == setup.control->planets.front().special_control
                && planets.front().landed == setup.control->planets.front().landed
                && returned.galaxy->fleets().size() == setup.fleets.size(),
                "tactical persistence remains separate from specials and galactic forces");
        }
        expect(space_calls == 0, "automatic land/space results never borrow the legacy space child applier");
        expect(game.galaxy().state_sha256() == restored.galaxy().state_sha256(),
            "fresh restored automatic return equals uninterrupted world state");
    }
}
void child_persistence() {
    const sim::InlineExecutor executor;
    const auto setup = persistence_setup();
    for (const auto layer : {Layer::land, Layer::space}) {
        auto game = take(Game::create(setup));
        Calls calls, restored_calls;
        const auto configure = [&](Game& owner, Calls& count) {
            auto services = provider(layer, count);
            services.apply = {}; // Preserve the session's stateless default land writer.
            check(owner.install_conflict_layer(layer, std::move(services)));
        };
        configure(game, calls);
        const auto record = conflict(setup, layer);
        check(game.queue_conflict(record, {campaign::ConflictRoute::tactical, false, false}));
        for (unsigned i = 0; i < 4 && !game.battle(); ++i) take(game.step(executor));
        expect(game.battle() != nullptr, "synthetic future land provider enters with persistence records");
        const auto identities = checkpoint::game_identities(setup);
        const auto saved = take(checkpoint::save_game_checkpoint(game, identities));
        checkpoint::GameLoadOptions options;
        options.content = [](const auto& context) { return core::Result<tt::TacticalContent>::success(content(context)); };
        options.configure_session = [&](Game& owner) {
            configure(owner, restored_calls); return core::Result<void>::success();
        };
        auto restored = take(checkpoint::load_game_checkpoint(saved, identities, options, executor)).session;
        auto expected = setup.control->planets.front().built_structures;
        if (layer == Layer::land) std::erase_if(expected, [](const auto& row) { return row.owner != 1; });
        for (auto* owner : {&game, &restored}) {
            check(owner->submit({{owner->completed_tick(), {2}, 1}, quit(record)}));
            for (unsigned i = 0; i < 10 && owner->battle(); ++i) take(owner->step(executor));
            expect(!owner->battle(), "child with tactical-built records returns");
            expect(owner->galaxy().snapshot()->control()->planets.front().built_structures == expected,
                "child return and restore preserve winner values without converting losing or neutral records");
        }
        expect(game.galaxy().state_sha256() == restored.galaxy().state_sha256(),
            "fresh restored child return equals uninterrupted world state");
    }
}
void layer_hooks() {
    const sim::InlineExecutor executor;
    const auto setup = gc::synthetic_galaxy({7, 2, 2, 1, 1, false});
    for (const auto layer : {Layer::space, Layer::land}) for (const bool reverse : {false, true}) {
        auto game = take(Game::create(setup));
        std::array<Calls, 2> calls{}, restored_calls{};
        install(game, calls, reverse);
        const auto record = conflict(setup, layer);
        check(game.queue_conflict(record, {campaign::ConflictRoute::tactical, false, false}));
        for (unsigned i = 0; i < 4 && !game.battle(); ++i) take(game.step(executor));
        expect(game.battle() != nullptr, "registered synthetic provider enters a child");
        if (!game.battle()) return;
        const auto index = layer == Layer::land ? 1U : 0U, other = 1U - index;
        expect(calls[index] == Calls{1, 0, 1, 1, 0, 0} && calls[other] == Calls{},
            "entry, factory and configurator dispatch once without touching the other layer");
        const auto build = checkpoint::game_identities(setup);
        const auto saved = take(checkpoint::save_game_checkpoint(game, build));
        if (layer == Layer::land) {
            unsigned borrowed_space_content{};
            checkpoint::GameLoadOptions absent;
            absent.content = [&](const auto& context) {
                ++borrowed_space_content; return core::Result<tt::TacticalContent>::success(content(context));
            };
            auto refused = checkpoint::load_game_checkpoint(saved, build, absent, executor);
            expect(!refused && refused.error().message.find("land tactical provider is unavailable") != std::string::npos
                && borrowed_space_content == 0, "a land child restore cannot borrow the space content resolver");
            auto replay = take(Game::from_recording(game.recording(), absent.content));
            bool replay_refused{};
            for (unsigned i = 0; i < 4 && !replay_refused; ++i) {
                auto stepped = replay.step(executor);
                replay_refused = !stepped && stepped.error().code == campaign::unavailable_conflict_tactical;
            }
            expect(replay_refused && borrowed_space_content == 0, "recorded land child selection requires its provider on replay");
        }
        checkpoint::GameLoadOptions options;
        options.content = [](const auto& context) { return core::Result<tt::TacticalContent>::success(content(context)); };
        options.configure_session = [&](Game& restored) {
            install(restored, restored_calls, !reverse); return core::Result<void>::success();
        };
        auto restored = take(checkpoint::load_game_checkpoint(saved, build, options, executor)).session;
        expect(restored.state_sha256() == game.state_sha256(), "direct restore preserves child state for either provider layer");
        expect(restored_calls[index] == Calls{0, 1, 1, 1, 0, 0} && restored_calls[other] == Calls{},
            "restoration dispatches content, scripts and configuration only to the saved layer");
        const auto end = [&](Game& owner) {
            check(owner.submit({{owner.completed_tick(), {2}, 1}, quit(record)}));
            for (unsigned i = 0; i < 10 && owner.battle(); ++i) take(owner.step(executor));
            expect(!owner.battle(), "synthetic child returns through its layer");
        };
        end(game); end(restored);
        expect(calls[index].decorate == 1 && calls[index].apply == 1 && calls[other] == Calls{},
            "decorator and applier dispatch exactly once to the child layer");
        expect(restored_calls[index].decorate == 1 && restored_calls[index].apply == 1 && restored_calls[other] == Calls{},
            "restored child retains its own result consumers");
    }
}
void land_fallback() {
    const sim::InlineExecutor executor;
    auto setup = gc::synthetic_galaxy({7, 2, 2, 1, 1, false});
    setup.seats[1].kind = gc::SeatKind::ai;
    for (const bool two_humans : {false, true}) for (const bool vote : {false, true}) for (const bool forced : {false, true}) {
        setup.seats[1].kind = two_humans ? gc::SeatKind::human : gc::SeatKind::ai;
        setup.seats[1].human_side_id = two_humans ? 2 : 0;
        auto game = take(Game::create(setup));
        game.set_vote_progress([](const auto&, const auto&) { return core::Result<std::optional<bool>>::success(true); });
        Calls space_calls;
        check(game.install_conflict_layer(Layer::space, provider(Layer::space, space_calls)));
        Game::ConflictLayerServices land;
        land.tactical_refusal = "land tactical provider is unavailable";
        check(game.install_conflict_layer(Layer::land, std::move(land)));
        const auto record = conflict(setup, Layer::land);
        unsigned resolved{};
        game.set_auto_resolver([&](const auto& context, const auto&, const auto&) {
            expect(context.layer == Layer::land, "ordinary land request reaches the shared auto-resolver");
            ++resolved;
            return core::Result<std::optional<campaign::AutoResolveOutcomeApplication>>::success({});
        });
        check(game.queue_conflict(record, {vote ? campaign::ConflictRoute::pending_tactical : campaign::ConflictRoute::tactical, forced, false}));
        take(game.step(executor));
        // Guards must not abort matches: a refused land fight settles through the shared auto-resolver, forced or not.
        // The stub resolver returns no outcome, so the conflict stays admitted and is offered again: count 'reached it'.
        auto selected = game.step(executor);
        if (vote) selected = game.step(executor);
        expect(static_cast<bool>(selected), "a refused land fight never fails the match, forcing gate or not");
        expect(resolved >= 1, "a refused land fight reaches the shared auto-resolver, forcing gate or not");
        expect(!game.battle() && space_calls == Calls{}, "land selection never calls a space child hook");
    }
    auto direct = take(Game::create(setup));
    unsigned space_scripts{};
    check(direct.set_child_script_services(Layer::space, [&](const auto&) {
        ++space_scripts; return script::authoritative::ScriptScheduler::create({}, {});
    }, [](auto&) { return core::Result<void>::success(); }));
    const auto land = conflict(setup, Layer::land);
    const auto refused = direct.stage_battle(land, content(land));
    expect(!refused && refused.error().code == campaign::unavailable_conflict_tactical && space_scripts == 0,
        "space script assembly cannot supply a missing land provider");
    auto scripts = take(script::authoritative::ScriptScheduler::create({}, {}));
    const auto explicit_refused = direct.stage_battle(land, content(land), std::move(scripts));
    expect(!explicit_refused && explicit_refused.error().code == campaign::unavailable_conflict_tactical,
        "an explicit scheduler cannot bypass the layer capability");
}
// #4546 item 2: with no land provider and nothing to auto-resolve, a land request is refused by name at admission.
void land_admission_refusal() {
    const auto setup = gc::synthetic_galaxy({7, 2, 2, 1, 1, false});
    const auto record = conflict(setup, Layer::land);
    {
        auto game = take(Game::create(setup));
        game.set_conflict_service([](const auto&, const auto&, auto) {
            return core::Result<std::optional<campaign::RecordedAdmission>>::success({});
        }, [](const auto& value) { return core::Result<tt::TacticalContent>::success(content(value)); }, {},
            campaign::ScriptRequirement::none);
        auto queued = game.queue_conflict(record, {campaign::ConflictRoute::tactical, false, false});
        expect(!queued && queued.error().code == campaign::unavailable_conflict_tactical,
            "a land request without a land provider or auto-resolver is refused by name, not left pending");
        expect(!game.battle() && !game.has_pending_conflict_for_admission(), "the refused land request leaves nothing pending");
    }
    {
        auto game = take(Game::create(setup));
        Calls space_calls;
        check(game.install_conflict_layer(Layer::space, provider(Layer::space, space_calls)));
        game.set_auto_resolver([](const auto&, const auto&, const auto&) {
            return core::Result<std::optional<campaign::AutoResolveOutcomeApplication>>::success({});
        });
        check(game.queue_conflict(record, {campaign::ConflictRoute::tactical, false, false}));
    }
    {
        auto game = take(Game::create(setup));
        Calls space_calls, land_calls;
        check(game.install_conflict_layer(Layer::space, provider(Layer::space, space_calls)));
        check(game.install_conflict_layer(Layer::land, provider(Layer::land, land_calls)));
        check(game.queue_conflict(record, {campaign::ConflictRoute::tactical, false, false}));
    }
}
// #4546 item 1: a recorded land child is rebuilt through the land layer's own dispatcher services.
void land_recording() {
    const sim::InlineExecutor executor;
    const auto setup = gc::synthetic_galaxy({7, 2, 2, 1, 1, false});
    auto game = take(Game::create(setup));
    std::array<Calls, 2> calls{};
    install(game, calls, false);
    const auto record = conflict(setup, Layer::land);
    check(game.queue_conflict(record, {campaign::ConflictRoute::tactical, false, false}));
    for (unsigned i = 0; i < 4 && !game.battle(); ++i) take(game.step(executor));
    expect(game.battle() != nullptr, "land child enters through the dispatcher");
    if (!game.battle()) return;
    check(game.submit({{game.completed_tick(), {2}, 1}, quit(record)}));
    for (unsigned i = 0; i < 10 && game.battle(); ++i) take(game.step(executor));
    expect(!game.battle(), "recorded land child returns");
    const auto recording = game.recording();
    unsigned borrowed_space_content{};
    std::array<Calls, 2> replay_calls{};
    auto replay = take(Game::from_recording(recording, [&](const auto& value) {
        ++borrowed_space_content; return core::Result<tt::TacticalContent>::success(content(value));
    }, {}, campaign::ScriptRequirement::required, [&](Game& owner) {
        install(owner, replay_calls, false); return core::Result<void>::success();
    }));
    while (replay.completed_tick() < recording.commands.final_tick_count) take(replay.step(executor));
    expect(borrowed_space_content == 0 && replay_calls[1].content + replay_calls[1].entry >= 1 && replay_calls[1].scripts == 1
            && replay_calls[1].configure == 1 && replay_calls[0] == Calls{},
        "replayed land child is built by the land layer only");
    expect(replay_calls[1].decorate == calls[1].decorate && replay_calls[1].apply == calls[1].apply,
        "replayed land child returns through the same land result consumers");
    expect(replay.state_sha256() == game.state_sha256(), "land recording replay reproduces the live state");
}
// #4546 item 3: the multiplayer vote path asks the dispatcher whether a land conflict may be fought.
void land_vote_route() {
    const sim::InlineExecutor executor;
    auto setup = gc::synthetic_galaxy({7, 2, 2, 1, 1, false});
    setup.networked_transport = true;
    setup.seats[1].kind = gc::SeatKind::human; setup.seats[1].human_side_id = 2;
    const auto record = conflict(setup, Layer::land);
    for (const bool provider_installed : {false, true}) {
        auto game = take(Game::create(setup));
        check(game.configure_multiplayer_votes(30));
        Calls space_calls, land_calls;
        check(game.install_conflict_layer(Layer::space, provider(Layer::space, space_calls)));
        Game::ConflictLayerServices land;
        if (provider_installed) land = provider(Layer::land, land_calls);
        else land.tactical_refusal = "land tactical provider is unavailable";
        check(game.install_conflict_layer(Layer::land, std::move(land)));
        unsigned resolved{};
        game.set_auto_resolver([&](const auto&, const auto&, const auto&) {
            ++resolved; return core::Result<std::optional<campaign::AutoResolveOutcomeApplication>>::success({});
        });
        // WAR-01: multiplayer non-human combat resolves even when a tactical gate is set.
        // An unavailable land provider also cannot terminate a networked match.
        check(game.queue_conflict(record, {campaign::ConflictRoute::pending_tactical, !provider_installed, false}));
        for (unsigned i = 0; i < 4; ++i) take(game.step(executor));
        // The stub resolver returns no outcome, so the pending conflict stays admitted and every later step offers it again:
        // count 'reached the resolver', not exactly one call.
        expect(provider_installed ? resolved == 0 : resolved >= 1,
            "the vote path takes its land route from the dispatcher: auto-resolve without a land provider, a vote with one");
        expect(land_calls.entry == 0 && land_calls.content == 0 && !game.battle(), "no land child starts before the vote ends");
    }
}
// A networked SP-rules campaign must override solo instant-auto admission for a human side.
void coop_vote_route() {
    const sim::InlineExecutor executor;
    auto setup = gc::synthetic_galaxy({7, 2, 2, 1, 1, true});
    setup.networked_transport = true;
    const auto record = conflict(setup, Layer::land);
    auto game = take(Game::create(setup));
    check(game.configure_multiplayer_votes(30));
    Calls calls;
    check(game.install_conflict_layer(Layer::land, provider(Layer::land, calls)));
    unsigned resolved{};
    game.set_auto_resolver([&](const auto&, const auto&, const auto&) {
        ++resolved; return core::Result<std::optional<campaign::AutoResolveOutcomeApplication>>::success({});
    });
    check(game.queue_conflict(record, {campaign::ConflictRoute::instant_auto, false, false}));
    for (unsigned i = 0; i < 4; ++i) take(game.step(executor));
    expect(resolved == 0 && !game.battle(), "networked human versus AI waits for a vote even when solo admission says auto");
}
}
int case_galactic_conflict_dispatch_tests() {
    try { layer_hooks(); land_fallback(); land_admission_refusal(); land_recording(); land_vote_route(); coop_vote_route(); automatic_persistence(); child_persistence(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
