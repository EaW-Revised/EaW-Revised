#include "galactic_support.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/checkpoint/game.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

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
// #8171: a multiplayer battle whose entry fails (an AI player type, content or staging refusal) is reported and settles through
// the auto-resolver on every peer; the galactic game keeps stepping. A solo game keeps its refusal.
core::Diagnostic refusal(std::string code, std::string message) {
    core::Diagnostic out; out.code = std::move(code); out.message = std::move(message);
    return out;
}
void entry_failure_falls_back() {
    const sim::InlineExecutor executor;
    for (const bool multiplayer : {true, false}) for (const bool staging_failure : {false, true}) {
        auto setup = gc::synthetic_galaxy({7, 2, 2, 1, 1, false});
        // Multiplayer: two human sides vote, as in a lobby game; solo: the second side is the AI.
        setup.seats[1].kind = multiplayer ? gc::SeatKind::human : gc::SeatKind::ai;
        setup.seats[1].human_side_id = multiplayer ? 2 : 0;
        setup.networked_transport = multiplayer;
        std::array<std::string, 2> hashes;
        std::array<unsigned, 2> resolved{};
        std::array<bool, 2> kept_running{};
        std::array<std::string, 2> notes;
        std::array<unsigned, 2> note_lines{}, entry_calls{};
        for (unsigned peer = 0; peer < 2; ++peer) {
            auto game = take(Game::create(setup));
            // The canonical lobby policy: the humans' missing votes default to Begin after the 30-tick timeout.
            if (multiplayer) check(game.configure_multiplayer_votes(30));
            Calls calls;
            auto layer = provider(Layer::space, calls);
            if (staging_failure) layer.scripts = [](const auto&) {
                return core::Result<script::authoritative::ScriptScheduler>::failure(refusal("EAWR-TEST-0001", "scripts are unavailable"));
            };
            else layer.entry = [&](const auto&, const auto&, auto) {
                ++calls.entry;
                return core::Result<campaign::ResolvedConflictEntry>::failure(refusal("EAWR-TEST-0002", "missing MP tactical AI difficulty"));
            };
            check(game.install_conflict_layer(Layer::space, std::move(layer)));
            game.set_auto_resolver([&, peer](const auto&, const auto&, const auto&) {
                ++resolved[peer];
                return core::Result<std::optional<campaign::AutoResolveOutcomeApplication>>::success({});
            });
            check(game.queue_conflict(conflict(setup, Layer::space),
                {multiplayer ? campaign::ConflictRoute::pending_tactical : campaign::ConflictRoute::tactical, false, false}));
            bool failed = false;
            for (unsigned i = 0; i < 70; ++i) {
                auto stepped = game.step(executor);
                if (!stepped) { failed = true; break; }
                for (const auto& line : stepped.value().diagnostics)
                    if (line.code == "EAWR-ENTRY-0001") { notes[peer] = line.message; ++note_lines[peer]; }
            }
            kept_running[peer] = !failed;
            expect(!game.battle(), "a refused battle entry never opens a child");
            hashes[peer] = game.state_sha256();
            entry_calls[peer] = calls.entry;
        }
        if (multiplayer) {
            expect(kept_running[0] && kept_running[1] && resolved[0] >= 1 && resolved[1] >= 1,
                "a multiplayer entry failure settles through the auto-resolver and the game keeps stepping");
            expect(notes[0].find("could not be entered") != std::string::npos && notes[0] == notes[1],
                "the entry failure is reported once per peer with the same text");
            expect(hashes[0] == hashes[1], "every peer takes the same fallback and reaches the same state");
            // The vote route is recomputed every tick while the (stub) solver stays in progress: the entry is attempted once,
            // the failure reported once.
            expect(note_lines[0] == 1 && note_lines[1] == 1 && (staging_failure || (entry_calls[0] == 1 && entry_calls[1] == 1)),
                "the fallback is latched: one entry attempt and one report across every tick of the auto-resolve");
        } else {
            expect(!kept_running[0] && resolved[0] == 0, "a solo entry failure keeps its refusal and never reaches the resolver");
        }
    }
}
// #8171: a checkpoint taken while the refused battle is being auto-resolved keeps the fallback and the solver's state; the
// restored game resumes it identically and never re-enters the battle.
void entry_failure_checkpoint(const bool churn) {
    const sim::InlineExecutor executor;
    auto setup = gc::synthetic_galaxy({7, 2, 2, 1, 1, false});
    setup.seats[1].kind = gc::SeatKind::human; setup.seats[1].human_side_id = 2;
    setup.networked_transport = true;
    if (churn) {
        // Two human sides with seat control, so the seats can leave and rejoin.
        setup.seats[0].human_side_id = 1; setup.seats[1].human_side_id = 2;
        setup.seat_control.emplace();
        setup.seat_control->teams.push_back({1, {1}, false});
        setup.seat_control->teams.push_back({2, {2}, false});
        for (std::uint32_t player = 1; player <= 2; ++player)
            setup.seat_control->controllers.push_back({player, gc::FactionControl::human, gc::FactionControl::ai, {}, {}});
    }
    struct Peer { Calls calls; std::uint32_t solves{}; };
    const auto configure = [](Game& game, Peer& peer) {
        auto layer = provider(Layer::space, peer.calls);
        layer.entry = [&peer](const auto&, const auto&, auto) {
            ++peer.calls.entry;
            return core::Result<campaign::ResolvedConflictEntry>::failure(refusal("EAWR-TEST-0002", "missing MP tactical AI difficulty"));
        };
        check(game.install_conflict_layer(Layer::space, std::move(layer)));
        game.set_auto_resolver([&peer](const auto&, const auto&, const auto&) {
            ++peer.solves;
            return core::Result<std::optional<campaign::AutoResolveOutcomeApplication>>::success({});
        });
        // The solver's own state: how many ticks it has run.
        game.set_auto_resolve_checkpoint_services([&peer] {
            std::vector<std::uint8_t> bytes;
            for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(peer.solves >> (i * 8U)));
            return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
        }, [&peer](std::span<const std::uint8_t> bytes, const auto&, const auto&) {
            if (bytes.size() != 4) return core::Result<void>::failure(refusal("EAWR-TEST-0003", "bad solver state"));
            peer.solves = 0;
            for (unsigned i = 0; i < 4; ++i) peer.solves |= static_cast<std::uint32_t>(bytes[i]) << (i * 8U);
            return core::Result<void>::success();
        });
    };
    Peer straight, resumed;
    auto game = take(Game::create(setup));
    check(game.configure_multiplayer_votes(30)); // the canonical policy: missing votes default to Begin after 30 ticks
    configure(game, straight);
    std::uint64_t sequence[3]{};
    const auto seat_command = [&](Game& owner, const std::uint32_t seat, gc::CommandPayload payload) {
        check(owner.submit({{owner.completed_tick(), {seat}, ++sequence[seat]}, std::move(payload)}));
    };
    if (churn) { // the second human side is away before the entry is refused
        take(game.step(executor));
        seat_command(game, 2, gc::SeatLeave{});
        take(game.step(executor));
    }
    check(game.queue_conflict(conflict(setup, Layer::space), {campaign::ConflictRoute::pending_tactical, false, false}));
    for (unsigned i = 0; i < 80 && straight.solves < (churn ? 1U : 3U); ++i) take(game.step(executor));
    if (churn) { // the present side leaves, then the absent side comes back, while the solve runs
        seat_command(game, 1, gc::SeatLeave{});
        take(game.step(executor));
        seat_command(game, 2, gc::SeatRejoin{});
        for (unsigned i = 0; i < 3; ++i) take(game.step(executor));
    }
    expect(straight.solves >= 1 && straight.calls.entry == 1, "the refused entry went to the solver after one attempt");
    const auto view = game.pending_conflict_view(gc::SeatId{1});
    expect(view && view->arbitration.route == campaign::ConflictRoute::instant_auto,
        "the fallback is the conflict's saved route, not a recomputed battle route");
    const auto identities = checkpoint::game_identities(setup);
    const auto saved = take(checkpoint::save_game_checkpoint(game, identities));
    checkpoint::GameLoadOptions options;
    options.content = [](const auto& record) { return core::Result<tt::TacticalContent>::success(content(record)); };
    options.configure_session = [&](Game& restored) { configure(restored, resumed); return core::Result<void>::success(); };
    auto restored = take(checkpoint::load_game_checkpoint(saved, identities, options, executor)).session;
    expect(resumed.calls.entry == 0, "restoring never calls the refused entry");
    expect(resumed.solves == straight.solves, "the checkpoint saved the solver's state");
    const auto restored_view = restored.pending_conflict_view(gc::SeatId{1});
    expect(restored_view && restored_view->arbitration.route == campaign::ConflictRoute::instant_auto,
        "the restored conflict resumes on the auto-resolve route");
    expect(restored.state_sha256() == game.state_sha256(), "the restored game equals the saved one");
    for (unsigned i = 0; i < 4; ++i) {
        take(game.step(executor)); take(restored.step(executor));
        expect(game.state_sha256() == restored.state_sha256(), "the resumed solve stays equal to the uninterrupted one");
    }
    expect(resumed.solves == straight.solves && resumed.calls.entry == 0 && straight.calls.entry == 1,
        "the resumed solve runs on, and neither game re-enters the refused battle");
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
// #8300 (owner rule 2026-11-23): in a multiplayer galactic game only the humans whose factions are in the fight vote.
// One human against an AI/neutral side (pirates) decides alone: the second human, who is not in the fight, does nothing,
// is never asked, and can never block the vote. Auto-resolve resolves; the idle human's own choice is refused.
void uninvolved_human_never_blocks_vote() {
    const sim::InlineExecutor executor;
    for (const bool idle_human_clicks : {false, true}) {
        auto setup = gc::synthetic_galaxy({7, 3, 3, 1, 1, false}); // players 1 and 2 are human sides, player 3 is AI
        setup.networked_transport = true;
        auto record = conflict(setup, Layer::space);
        record.invader = 1; record.defender = 3; // human 1 fights the AI side; human 2 is not in this fight
        record.tactical.players = {{1, 1, 1, 1}, {3, 3, 3, 1}};
        std::erase_if(record.forces, [](const auto& force) { return force.owner == 2; });
        std::erase_if(record.tactical.units, [](const auto& unit) { return unit.owner == 2; });
        auto game = take(Game::create(setup));
        check(game.configure_multiplayer_votes(30));
        Calls calls;
        check(game.install_conflict_layer(Layer::space, provider(Layer::space, calls)));
        unsigned resolved{};
        game.set_auto_resolver([&](const auto&, const auto&, const auto&) {
            ++resolved; return core::Result<std::optional<campaign::AutoResolveOutcomeApplication>>::success({});
        });
        check(game.queue_conflict(record, {campaign::ConflictRoute::pending_tactical, false, false}));
        for (unsigned i = 0; i < 6; ++i) {
            const auto view = game.pending_conflict_view(gc::SeatId{1});
            if (view && view->choice_ready) break;
            take(game.step(executor));
        }
        const auto prompt = game.pending_conflict_view(gc::SeatId{1});
        expect(prompt && prompt->choice_ready && prompt->arbitration.route == campaign::ConflictRoute::vote,
            "a human against an AI side gets the vote");
        // The player who is not in the fight has no alert of its own and cannot vote.
        const auto idle_view = game.pending_conflict_view(gc::SeatId{2});
        // #8353: it gets a read-only alert (same timer and shell) so its client can show the passive prompt, and the alert
        // admits no answer.
        expect(idle_view && idle_view->alert && idle_view->alert->local == 2 && !idle_view->alert->begin_admitted
                && !idle_view->alert->resolve_admitted && !idle_view->alert->timeout_due,
            "a human who is not in the fight gets a passive battle alert that admits no answer");
        std::uint64_t idle_sequence{}, fighter_sequence{};
        if (idle_human_clicks) {
            check(game.submit({{game.completed_tick(), {2}, ++idle_sequence}, gc::BattleChoice{record.id, true}}));
            const auto refused = take(game.step(executor));
            expect(!refused.diagnostics.empty() && !game.battle() && resolved == 0,
                "a choice from a human who is not in the fight is refused and starts nothing");
        }
        check(game.submit({{game.completed_tick(), {1}, ++fighter_sequence}, gc::BattleChoice{record.id, false}}));
        for (unsigned i = 0; i < 3 && resolved == 0; ++i) take(game.step(executor));
        const auto after = game.pending_conflict_view(gc::SeatId{1});
        expect(resolved >= 1 && !game.battle() && calls.entry == 0 && calls.content == 0
                && after && after->arbitration.route == campaign::ConflictRoute::instant_auto,
            "the fighting human's Auto-resolve decides alone: the idle human never blocks and no battle starts");
    }
}
// #8353: three human sides, two of them fight. The third sees the same alert (so its client shows the passive prompt), cannot
// vote, and the fight resolves from the two fighting votes alone, whichever way they go. Peers stay equal.
void third_human_sees_passive_alert_and_cannot_vote() {
    const sim::InlineExecutor executor;
    for (const bool begin : {true, false}) {
        std::array<std::string, 2> hashes;
        for (unsigned peer = 0; peer < 2; ++peer) {
            auto setup = gc::synthetic_galaxy({.seed = 7, .planets = 3, .players = 3, .fleets_per_player = 1,
                .members_per_fleet = 1, .debug_commands = false, .human_sides = 3});
            setup.networked_transport = true;
            auto record = conflict(setup, Layer::space); // player 1 attacks player 2; player 3 is a human bystander
            record.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
            std::erase_if(record.forces, [](const auto& force) { return force.owner == 3; });
            std::erase_if(record.tactical.units, [](const auto& unit) { return unit.owner == 3; });
            auto game = take(Game::create(setup));
            check(game.configure_multiplayer_votes(30));
            Calls calls;
            check(game.install_conflict_layer(Layer::space, provider(Layer::space, calls)));
            unsigned resolved{};
            game.set_auto_resolver([&](const auto&, const auto&, const auto&) {
                ++resolved; return core::Result<std::optional<campaign::AutoResolveOutcomeApplication>>::success({});
            });
            check(game.queue_conflict(record, {campaign::ConflictRoute::pending_tactical, false, false}));
            for (unsigned i = 0; i < 6; ++i) {
                const auto view = game.pending_conflict_view(gc::SeatId{1});
                if (view && view->choice_ready) break;
                take(game.step(executor));
            }
            const auto fighter = game.pending_conflict_view(gc::SeatId{1});
            const auto bystander = game.pending_conflict_view(gc::SeatId{3});
            expect(fighter && fighter->alert && bystander && bystander->alert && bystander->choice_ready,
                "the fighting side and the third human both get the pending alert");
            if (!fighter || !fighter->alert || !bystander || !bystander->alert) return;
            expect(bystander->alert->local == 3 && bystander->conflict.id == fighter->conflict.id
                    && bystander->alert->visible == fighter->alert->visible
                    && bystander->alert->single_button == fighter->alert->single_button
                    && bystander->alert->timeout_ms == fighter->alert->timeout_ms
                    && bystander->alert->elapsed_wall_ms == fighter->alert->elapsed_wall_ms
                    && bystander->alert->untimed == fighter->alert->untimed,
                "the third human sees the same conflict, timer and shell as the fighting side");
            expect(fighter->alert->begin_admitted && !bystander->alert->begin_admitted && !bystander->alert->resolve_admitted
                    && !bystander->alert->timeout_due && !bystander->alert->waiting_for_opponent,
                "only the fighting side may answer; the third human's alert admits nothing and never times out");
            // The third human's own choice is refused, in either direction, and starts or resolves nothing.
            std::uint64_t sequence[4]{};
            for (const bool choice : {true, false}) {
                check(game.submit({{game.completed_tick(), {3}, ++sequence[3]}, gc::BattleChoice{record.id, choice}}));
                const auto refused = take(game.step(executor));
                expect(!refused.diagnostics.empty() && !game.battle() && resolved == 0 && calls.entry == 0,
                    "a choice from the third human is refused and counts as no vote");
            }
            // The two fighting humans decide: both Begin starts the battle, any Auto-resolve resolves it.
            check(game.submit({{game.completed_tick(), {1}, ++sequence[1]}, gc::BattleChoice{record.id, true}}));
            check(game.submit({{game.completed_tick(), {2}, ++sequence[2]}, gc::BattleChoice{record.id, begin}}));
            for (unsigned i = 0; i < 6 && !game.battle() && resolved == 0; ++i) take(game.step(executor));
            if (begin) expect(game.battle() != nullptr && resolved == 0, "two Begin votes start the battle with the third human idle");
            else expect(resolved >= 1 && !game.battle() && calls.entry == 0, "one Auto-resolve of the two fighting humans resolves it");
            hashes[peer] = game.state_sha256();
        }
        expect(hashes[0] == hashes[1], "both peers reach the same state");
    }
}
}
int case_galactic_conflict_dispatch_tests() {
    try { layer_hooks(); land_fallback(); land_admission_refusal(); land_recording(); land_vote_route(); coop_vote_route(); uninvolved_human_never_blocks_vote(); third_human_sees_passive_alert_and_cannot_vote(); entry_failure_falls_back(); entry_failure_checkpoint(false); entry_failure_checkpoint(true); automatic_persistence(); child_persistence(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
