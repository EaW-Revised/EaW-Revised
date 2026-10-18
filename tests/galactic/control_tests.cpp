#include "galactic_support.hpp"
#include "eawr/campaign/control.hpp"
#include "eawr/campaign/auto_resolve_outcome.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/sim/galactic/observer.hpp"
#include "eawr/vfs/vfs.hpp"
#include "galactic/planet_hud.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <limits>

namespace {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::checkpoint;
using galactic_test::expect;
using eawr::sim::math::Fixed;
Fixed f(std::int64_t value) { return Fixed::from_integer(value).value(); }
gc::GalacticSetup setup() {
    gc::GalacticSetup s;
    s.players = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
    s.seats = {{gc::SeatId{1}, 1, gc::SeatKind::human, 0, true, 1}, {gc::SeatId{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    s.planets = {{1, {}, 1}, {2, {f(12), {}, {}}, 3}, {3, {f(24), {}, {}}, 3}};
    s.routes = {{4, 1, 2, f(2)}}; s.rules.maximum_fleet_movement_distance = f(12); s.rules.fiscal_cycle_frames = 7;
    s.fleets = {{10, 1, 1, {{20, 1}, {21, 2}}}, {11, 2, 3, {{22, 1}}}};
    s.fleet_control.emplace(); s.fleet_control->types = {{1, false, true, false}, {2, true, true, false}, {3, false, true, false}};
    s.movement.emplace(); auto& m = *s.movement; m.mode_modifier = f(1);
    m.types = {{1, f(2), f(100), 0, true, false, false, false}, {2, f(2), f(50), 0, true, false, false, false}, {3, f(2), f(50), 0, true, false, false, true}};
    m.players = {{1, f(1), f(1), false}, {2, f(1), f(1), false}, {3, f(1), f(1), false}};
    m.planets = {{1, f(1), false}, {2, f(1), false}, {3, f(1), false}};
    for (const auto& player : s.players) for (const auto& p : s.planets) m.access.push_back({player.player_id, p.id, true, false});
    s.control.emplace(); auto& c = *s.control;
    c.change_seconds = f(7); c.max_ground_forces = 10;
    c.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
    for (const auto& a : c.players) for (const auto& b : c.players) c.relations.push_back({a.player, b.player, a.player == b.player});
    c.types = {{1, 1, false, false}, {2, 0, false, true}, {3, 1, true, false}};
    for (const auto& p : s.planets) {
        gc::ControlPlanet row; row.planet = p.id; row.type = 100 + p.id;
        row.surface_accessible = true; row.capture_reward = f(250); c.planets.push_back(row);
    }
    c.planets[0].ground_base = {1, 2}; c.routes = {{4, f(1)}};
    return s;
}
void enable_production(gc::GalacticSetup& s);
void payload_ground_capability() {
    auto s = setup();
    // Two companies share the exact same current carrier type. Their direct
    // payloads, rather than a shared type flag, supply different answers.
    s.control->ground_capability_types = {{1, f(7), false, false}, {2, {}, true, false}, {3, f(9), false, true}};
    s.movement->contents = {{21, {{30, 1}}}, {31, {{32, 3}}}};
    expect(gc::has_ground_capability(*s.control, {21, 2}, &*s.movement).value()
        && !gc::has_ground_capability(*s.control, {31, 2}, &*s.movement).value(),
        "WGM-106 shared carrier differs by direct company payload");
    s.movement->contents[0].children.push_back({35, 3});
    expect(gc::has_ground_capability(*s.control, {21, 2}, &*s.movement).value(),
        "one capable direct payload suffices alongside a stay-in-transport payload");
    s.movement->contents[0].children[0].type = 3;
    expect(!gc::has_ground_capability(*s.control, {21, 2}, &*s.movement).value(),
        "two stay-in-transport direct payloads are incapable");
    s.movement->contents[0].children = {{30, 1}};
    s.movement->contents.push_back({30, {{33, 3}}});
    expect(gc::has_ground_capability(*s.control, {21, 2}, &*s.movement).value(), "payload capability does not recursively test its own contents");
    s.control->absent_forces = {30};
    expect(gc::has_ground_capability(*s.control, {21, 2}, &*s.movement).value(), "payload query does not independently filter death state");
    s.control->ground_capability_types[1].stays_in_transport = true;
    expect(!gc::has_ground_capability(*s.control, {21, 2}, &*s.movement).value(), "transport's own stay flag defeats capability");
    s.control->ground_capability_types[1].stays_in_transport = false;
    expect(!gc::has_ground_capability(*s.control, {34, 2}, &*s.movement).value()
        && !gc::has_ground_capability(*s.control, {30, 1}, &*s.movement).value(), "empty carrier and ordinary nontransport are incapable");
    s.movement->contents[0].children[0].type = 999;
    expect(!gc::has_ground_capability(*s.control, {21, 2}, &*s.movement), "missing direct payload facts refuse");
    s.control->ground_capability_types.clear();
    expect(gc::has_ground_capability(*s.control, {21, 2}, &*s.movement).value(), "absent sourced tail preserves legacy supplied predicate");
    auto replay = gc::GalacticReplay{setup(), 1, {}};
    replay.setup.control->ground_capability_types = {{1, f(7), false, false}, {2, {}, true, false}, {3, f(9), false, true}};
    auto encoded = gc::write_replay(replay);
    expect(encoded && gc::parse_replay(encoded.value()).value() == replay, "ground capability facts replay round trip");
    eawr::units::GalacticTypeTable types;
    eawr::units::GalacticType transport; transport.name = "Carrier"; transport.behavior = {"TRANSPORT"};
    eawr::units::GalacticType troop; troop.name = "Troop"; troop.ai_combat_power = f(7); troop.stays_in_transport = true;
    types.types = {transport, troop};
    const std::vector<eawr::campaign::FleetTypeBinding> bindings{{1, "Carrier"}, {2, "Troop"}};
    auto prepared = eawr::campaign::prepare_ground_capability_types(types, bindings);
    expect(prepared && prepared.value()[0].transport && prepared.value()[0].combat_power == Fixed{}
        && !prepared.value()[0].stays_in_transport && prepared.value()[1].stays_in_transport,
        "current-type authoring precedes fresh power/stay omission defaults");
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = setup(); initial.fleets[0].orbit = 2; initial.fleets[0].members = {{21, 2}, {31, 2}};
        initial.movement->types[1].transport = true;
        initial.control->ground_capability_types = {{1, f(7), false, false}, {2, {}, true, false}, {3, f(9), false, true}};
        initial.movement->contents = {{21, {{30, 1}}}, {31, {{32, 3}}}};
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        auto created = eawr::campaign::GameSession::create(initial);
        if (!created) throw std::runtime_error(created.error().message);
        auto game = std::move(created).value();
        const auto identities = cp::game_identities(initial);
        const auto saved = cp::save_game_checkpoint(game, identities).value();
        auto loaded = cp::load_game_checkpoint(saved, identities, {}, pool).value();
        expect(cp::save_game_checkpoint(loaded.session, identities).value() == saved, "ground payload facts retain canonical tick-zero checkpoint");
        const gc::GameCommand command{{0, gc::SeatId{1}, 1}, gc::LandFleet{10, 2}};
        game.submit(command).value(); loaded.session.submit(command).value();
        std::vector<std::string> actual;
        for (int tick = 0; tick < 3; ++tick) {
            const auto stepped = game.step(pool).value(); actual.push_back(stepped.state_hash.get());
            expect(loaded.session.step(pool).value().state_hash.get() == actual.back(), "restored payload capability continuation agrees");
        }
        const auto snapshot = game.galaxy().snapshot();
        const auto& landed = snapshot->control()->planets[1].landed;
        expect(landed.size() == 1 && landed[0].id == 21 && snapshot->fleets()[0].members == std::vector<gc::FleetMember>{{31, 2}},
            "hostile uncontested landing admits capable company and keeps same-carrier incapable company in orbit");
        if (workers == 1) reference = actual; else expect(actual == reference, "direct payload landing has 1/2/4/8 canonical equality");
    }
}
void authored_mixed_surface_owners() {
    auto initial = setup();
    initial.control->planets[0].ground_base = {};
    const std::vector<gc::SurfaceForce> expected{{30, 3, 1}, {31, 3, 2}, {32, 3, 3}};
    initial.control->planets[0].landed = expected;
    initial.player_economy.emplace(); initial.fiscal.emplace(); initial.fiscal->credit_cap_per_planet = f(1000);
    for (const auto& player : initial.players) {
        initial.player_economy->players.push_back({player.player_id, f(100), f(1000), 0, 5});
        initial.fiscal->players.push_back({player.player_id, true, f(1)});
    }
    for (const auto& planet : initial.planets) initial.fiscal->planets.push_back({planet.id, f(1)});
    enable_production(initial);
    expect(bool(gc::validate_setup(initial)), "WGS-181 admits authored mixed surface owners");
    auto bad = initial; bad.control->planets[0].landed.back().owner = 99;
    expect(!gc::validate_setup(bad), "mixed-owner admission still rejects an unknown owner");
    bad = initial; bad.control->planets[0].landed.back().id = 30;
    expect(!gc::validate_setup(bad), "mixed-owner admission still rejects duplicate identities");
    bad = initial; bad.control->max_ground_forces = 1;
    expect(!gc::validate_setup(bad), "mixed-owner admission still enforces shared surface capacity");
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto created = eawr::campaign::GameSession::create(initial);
        if (!created) throw std::runtime_error(created.error().message);
        auto game = std::move(created).value();
        const auto identities = cp::game_identities(initial);
        const auto saved = cp::save_game_checkpoint(game, identities).value();
        auto restored = cp::load_game_checkpoint(saved, identities, {}, executor).value();
        expect(restored.session.galaxy().snapshot()->control()->planets[0].landed == expected,
            "mixed surface owners and authored order survive checkpoint restore");
        std::vector<std::string> hashes;
        for (unsigned tick = 0; tick < 3; ++tick) {
            const auto step = game.step(executor).value();
            hashes.push_back(step.state_sha256);
            expect(restored.session.step(executor).value().state_sha256 == step.state_sha256,
                "mixed-owner restored continuation matches live control");
        }
        expect(game.galaxy().snapshot()->control()->planets[0].landed == expected,
            "ordinary control preserves each authored surface owner");
        expect(game.galaxy().snapshot()->control()->state[0].controller == 1
            && game.galaxy().snapshot()->production()->planets[0].ground_occupier == 1,
            "WGS-186 mixed list controller and ground occupation select the first owner");
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "mixed-owner surface continuation equals on 1/2/4/8 workers");
    }
    auto control = *initial.control;
    std::vector<gc::CensusObject> actors(3);
    actors[0].id = 30; actors[0].delete_pending = true; actors[1].id = 31; actors[2].id = 32;
    const auto calculate = [&] {
        return gc::calculate_planet_control(control, control.planets[0], {}, {},
            &*initial.movement, {}, 0, actors).value().state;
    };
    expect(calculate().controller == 2, "WGS-186 political controller skips the deleting first owner in a mixed list");
    actors[0].delete_pending = false;
    expect(calculate().controller == 1, "WGS-186 first live entry wins rather than the last or political owner");
    for (auto& actor : actors) actor.delete_pending = true;
    expect(calculate().controller == 0 && calculate().target == Fixed{},
        "WGS-186 all-deleting mixed transports contribute neither controller nor military presence");
    for (auto& force : control.planets[0].landed) force.owner = 1;
    expect(calculate().controller == 0 && calculate().target == Fixed{},
        "WGS-186 all-deleting single-owner transports also contribute no military presence");
    control.planets[0].landed = expected;
    for (auto& actor : actors) actor.delete_pending = false;
    std::swap(control.planets[0].landed[0], control.planets[0].landed[1]);
    expect(calculate().controller == 2, "WGS-186 mixed political controller follows authored list order");
    control.planets[0].ground_base = {1, 2};
    expect(calculate().controller == 1, "WGS-186 nonzero ground base precedes the mixed surface list");
    auto admission = initial;
    admission.control = eawr::campaign::prepare_initial_control(admission, *admission.control).value();
    for (const auto target : {0U, 1U, 2U, 3U}) {
        admission.control->state[0].target_player = target;
        auto world = gc::GalacticSession::create(admission).value();
        world.enable_native_invasions(true);
        const auto receipt = world.admit_native_invasion(1, 10).value();
        expect(receipt.admitted && world.native_invasion(receipt.request)->defender == (target == 0 || target == 3 ? 1U : target),
            "WGS-186 native defender uses target over current owner, with first-owner fallback for a neutral target");
    }
    admission.control->state[0].target_player = 99;
    expect(!gc::GalacticSession::create(admission), "unknown target identities refuse before native defender selection");
    for (const auto target : {0U, 1U, 2U, 3U}) for (const bool base : {false, true}) {
        auto input = admission; input.control->state[0].target_player = target;
        input.control->planets[0].ground_base = base ? gc::ControlBase{3, 1} : gc::ControlBase{};
        input.production->planets[0].ground_occupier = base ? 3U : 0U;
        input.production->planets[0].ground_level = base ? 1U : 0U;
        auto staged = gc::GalacticSession::create(input).value(); auto direct = gc::GalacticSession::create(input).value();
        staged.enable_native_invasions(true); direct.enable_native_invasions(true);
        const auto initial_bytes = staged.canonical_state_bytes();
        const std::array<gc::PlayerId, 1> players{1};
        auto barrier = std::move(staged.begin_native_barrier(players, 16)).value(); barrier->select_player(1).value();
        const auto actual = staged.admit_native_invasion(1, 10).value(); const auto oracle = direct.admit_native_invasion(1, 10).value();
        const auto expected_defender = base ? 3U : (target == 0 || target == 3 ? 1U : target);
        expect(actual == oracle && actual.admitted && staged.native_invasion(actual.request)->defender == expected_defender,
            "WGS-186 staged defender follows target allegiance, with mixed-prefix fallback only without a base");
        expect(staged.canonical_state_bytes() == initial_bytes, "mixed-owner invasion reader leaves the live owner unchanged before commit");
        barrier->commit().value(); barrier.reset();
        expect(staged.canonical_state_bytes() == direct.canonical_state_bytes() && staged.record() == direct.record(),
            "mixed-owner invasion shadow commits the live owner's exact state and journal");
    }
}
void initial_control_provider() {
    auto s = setup();
    auto& inputs = *s.control;
    inputs.planets[1].ground_base = {1, 1};
    // Already-controlled neighbours make trade eligible even at startup if
    // accidentally passed to the ordinary calculator.
    auto prepared = eawr::campaign::prepare_initial_control(s, inputs);
    expect(prepared && prepared.value().state[0].current == f(2)
        && prepared.value().state[1].current == f(1), "CGS-02 excludes trade at root start");
    if (!prepared) return;
    for (const auto& state : prepared.value().state)
        expect(state.initialized && state.current == state.target && state.current_player == state.target_player,
            "CGS-02 sets current and target together");
    expect(prepared.value().captured_types.empty(), "startup does not award conquest rewards");
    expect(!eawr::campaign::prepare_initial_control(s, prepared.value()), "root provider refuses prior initialized control");
    auto malformed = inputs; malformed.types.clear();
    expect(!eawr::campaign::prepare_initial_control(s, malformed), "missing live political quantities refuse");
    s.control = prepared.value();
    for (std::size_t i = 0; i < s.planets.size(); ++i) s.planets[i].owner = s.control->state[i].current_player;
    std::string first_hash;
    for (const auto count : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter workers(count);
        auto game = eawr::campaign::GameSession::create(s).value();
        const auto identities = cp::game_identities(s);
        const auto saved = cp::save_game_checkpoint(game, identities).value();
        auto restored = cp::load_game_checkpoint(saved, identities, {}, workers).value();
        expect(restored.session.state_sha256() == game.state_sha256(), "initial control round trips at tick zero");
        const auto step = game.step(workers).value();
        expect(restored.session.step(workers).value().state_sha256 == step.state_sha256, "initial checkpoint continues identically");
        expect(step.galaxy->control()->state[0].target == f(3), "ordinary service subsequently applies eligible trade influence");
        if (count == 1) first_hash = step.state_sha256;
        else expect(first_hash == step.state_sha256, "initial control continues equally on 1/2/4/8 workers");
    }
}
void control_and_outcome_extensions() {
    auto s = setup();
    s.player_economy.emplace(); s.fiscal.emplace();
    s.fiscal->credit_cap_per_planet = f(1000);
    for (const auto& player : s.players) {
        s.player_economy->players.push_back({player.player_id, f(100), f(1000), 0, 5});
        s.fiscal->players.push_back({player.player_id, true, f(1)});
    }
    for (const auto& planet : s.planets) s.fiscal->planets.push_back({planet.id, f(1)});
    enable_production(s);
    s.census = gc::PopulationCensus{{{1, f(1), 0}, {2, f(0), 0}, {3, f(1), 0}},
        {{1, 100, false, {}}, {2, 100, false, {}}, {3, 100, false, {}}}, {}};
    eawr::campaign::AutoResolveOutcomeApplication application;
    application.planet = s.planets.front().id; application.planet_type = "SyntheticPlanet";
    application.invader = 2; application.defender = 1;
    application.result.conflict = 900; application.result.winner = 1;
    application.destroyed = {20};
    std::vector<std::string> hashes;
    for (const std::size_t count : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter workers(count);
        auto game = eawr::campaign::GameSession::create(s).value();
        expect(game.galaxy().snapshot()->population()[0].used == f(1), "census initially counts the original member");
        expect(bool(game.stage_auto_resolve_outcome(application)), "control setup stages trusted outcome");
        const auto recording = game.record();
        const auto bytes = gc::write_replay(recording).value();
        const auto parsed = gc::parse_replay(bytes);
        expect(parsed && parsed.value().setup == recording.setup,
            "control tag 8, census tag 10 and outcome tag 11 coexist in ascending replay order");
        const auto tick = game.step(workers).value();
        expect(tick.state_hash.get() == game.state_sha256(), "combined return publishes committed hash");
        expect(tick.galaxy->population()[0].used == f(0), "outcome return publishes post-casualty census");
        const auto identities = cp::game_identities(s);
        const auto saved = cp::save_game_checkpoint(game, identities).value();
        auto restored = cp::load_game_checkpoint(saved, identities, {}, workers).value();
        expect(restored.session.state_sha256() == game.state_sha256(),
            "checkpoint retains control section 9, census section 11 and applied outcome section 12");
        const auto continued = game.step(workers).value();
        expect(restored.session.step(workers).value().state_sha256 == continued.state_sha256,
            "combined control/outcome checkpoint continues identically");
        if (count == 1) hashes = {tick.state_sha256, continued.state_sha256};
        else expect(hashes == std::vector<std::string>{tick.state_sha256, continued.state_sha256},
            "combined control/outcome state equals on 1/2/4/8 workers");
    }
}
void enable_production(gc::GalacticSetup& s) {
    s.production.emplace(); s.production->maximum_human_queue = 5;
    for (const auto& player : s.players) {
        gc::ProductionPlayer row; row.player = player.player_id;
        row.human = player.player_id == 1; row.pay_as_you_go = !row.human;
        row.uses_population_cap = row.human; row.population_capacity = f(100);
        s.production->players.push_back(row);
    }
    for (std::size_t i = 0; i < s.planets.size(); ++i) {
        gc::PlanetProduction row; row.planet = s.planets[i].id;
        row.target_owner = s.planets[i].owner; row.enabled = true; row.surface_accessible = true;
        row.maximum_space_level = 5; row.ground_slots = 3; row.space_slots = 3;
        row.maximum_ground_forces = s.control->max_ground_forces;
        row.ground_occupier = s.control->planets[i].ground_base.owner;
        row.ground_level = s.control->planets[i].ground_base.level;
        row.starbase_owner = s.control->planets[i].starbase.owner;
        row.space_level = s.control->planets[i].starbase.level;
        s.production->planets.push_back(row);
    }
}
// WGM-163: combat-skip evaluation refuses actors without a qualified combat role.
void qualify_census_roles(gc::PopulationCensus& census, const std::vector<eawr::sim::EntityId>& transports = {}) {
    for (auto& object : census.objects)
        object.combat_role = std::find(transports.begin(), transports.end(), object.id) != transports.end()
            ? gc::ActorCombatRole::transport : gc::ActorCombatRole::ordinary;
}
void landed_census_checkpoint() {
    auto s = setup();
    s.fleets[0].members = {{21, 2}}; // transport containing two live original children
    s.movement->types[1].fleet_container = true;
    s.movement->contents = {{21, {{30, 1}, {31, 1}}}};
    s.player_economy.emplace(); s.fiscal.emplace(); s.fiscal->credit_cap_per_planet = f(1000);
    for (const auto& player : s.players) {
        s.player_economy->players.push_back({player.player_id, f(100), f(1000), 0, 5});
        s.fiscal->players.push_back({player.player_id, true, f(1)});
    }
    for (const auto& planet : s.planets) s.fiscal->planets.push_back({planet.id, f(1)});
    enable_production(s);
    s.census = gc::PopulationCensus{{{1, f(1), 0}, {2, f(0), 0}, {3, f(1), 0}},
        {{1, 2, false, {}}, {2, 100, false, {}}, {3, 100, false, {}}},
        // Ascending identities; every fleet member needs a census row for combat-skip evaluation.
        {{21, 2, 1, 1, 10, false, false}, {22, 1, 2, 3, 11, false, false}, {30, 1, 1, 1, 21, false, false},
         {31, 1, 1, 1, 21, false, false}}};
    qualify_census_roles(*s.census, {21});
    gc::ProductionOffer offer; offer.type = 1; offer.price = f(1); offer.seconds = f(1); offer.population = f(1);
    offer.affiliated = true; offer.direct_build_allowed = true;
    s.production->planets[0].offers = {offer};
    gc::GalacticReplay replay{s, 3, {{{0, gc::SeatId{1}, 1}, gc::LandFleet{10, 1}},
        {{0, gc::SeatId{1}, 2}, gc::ProductionCommand{1, 1, gc::ProductionAction::buy, gc::ProductionDomain::ground, 0}}}};
    expect(gc::parse_replay(gc::write_replay(replay).value()).value() == replay, "landing census replay round trip");
    std::vector<std::string> hashes;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = eawr::campaign::GameSession::from_replay(replay).value();
        expect(game.galaxy().snapshot()->population()[0].used == f(2), "orbit children contribute before landing");
        const auto tick = game.step(pool).value();
        expect(tick.galaxy->control()->planets[0].landed.size() == 1 && tick.galaxy->fleets().size() == 1, "landing transfers transport and destroys its empty fleet");
        expect(tick.galaxy->population()[0].used == f(2), "landed transport children preserve usage");
        expect(tick.galaxy->production()->players[0].counts == std::vector<gc::ProductionCount>{{1, 2, 0}, {2, 1, 0}}, "landed original and child owned counts preserved");
        expect(std::any_of(tick.galaxy->events().begin(), tick.galaxy->events().end(), [](const auto& event) {
            return event.kind == gc::GalacticEventKind::command_rejected && event.value == static_cast<std::uint64_t>(gc::ProductionAdmission::population);
        }), "same-tick buy after landing still respects cap");
        const auto identities = cp::game_identities(s);
        const auto saved = cp::save_game_checkpoint(game, identities).value();
        auto restored = cp::load_game_checkpoint(saved, identities, {}, pool).value();
        expect(cp::save_game_checkpoint(restored.session, identities).value() == saved, "landed children checkpoint canonical round trip");
        std::vector<std::string> actual{tick.state_hash.get()};
        for (int i = 1; i < 3; ++i) {
            actual.push_back(game.step(pool).value().state_hash.get());
            expect(restored.session.step(pool).value().state_hash.get() == actual.back(), "landed checkpoint continuation equality");
        }
        if (workers == 1) hashes = actual; else expect(hashes == actual, "landed census 1/2/4/8 equality");
    }
    auto initial = s; initial.fleets.erase(initial.fleets.begin()); initial.census->objects.clear();
    initial.control->planets[0].landed = {{21, 2, 1}};
    auto world = gc::GalacticSession::create(initial).value();
    expect(world.snapshot()->population()[0].used == f(2), "tick-zero landed provider includes its original children");
    initial.census->types.erase(initial.census->types.begin() + 1);
    expect(!gc::validate_setup(initial), "landed forces require original census traits");
}
gc::PlanetControl state(gc::PlanetId id, gc::PlayerId player, std::int64_t amount) {
    gc::PlanetControl s; s.planet = id; s.initialized = true; s.current_player = player; s.target_player = player; s.current = f(amount); s.target = f(amount); return s;
}
gc::ControlStep calculate(const gc::PoliticalControlSetup& s, const gc::ControlPlanet& p, const gc::PlanetControl& prior,
    const std::vector<gc::Fleet>& orbit = {}, const std::vector<gc::Route>& routes = {}, std::uint64_t frame = 0) {
    return gc::calculate_planet_control(s, p, prior, orbit, nullptr, routes, frame).value();
}
void categories() {
    auto s = setup(); auto c = *s.control; auto p = c.planets[0]; const auto prior = state(1, 1, 2);
    expect(calculate(c, p, prior, {s.fleets[0]}).state.target == f(2), "WGM-68 military sources take category maximum, not their sum");
    auto enemy = s.fleets[1]; enemy.orbit = 1; enemy.members.push_back({30, 1});
    auto result = calculate(c, p, prior, {enemy});
    expect(result.state.target == f(1) && result.state.controller == 1 && result.state.blockaded && result.state.contested,
        "enemy fleet subtotal clamps to minus one; a defended surface retains its controller");
    result = calculate(c, p, prior, {s.fleets[0], enemy});
    expect(!result.state.blockaded && result.state.contested, "WGM-C02 first ordinary orbit fleet decides blockade even with a later hostile fleet");
    auto stealth = enemy; stealth.members = {{30, 3}};
    result = calculate(c, p, prior, {stealth, s.fleets[0]});
    expect(!result.state.blockaded && result.state.orbital_player == 1, "WGM-C01 stealth fallback yields to first ordinary occupant");
    result = calculate(c, p, prior, {stealth});
    expect(result.state.orbital_player == 2 && !result.state.blockaded, "only-stealth orbit has an occupant but no blockade");
    p.ground_base = {}; p.landed = {{50, 2, 2}}; p.starbase = {1, 5};
    expect(calculate(c, p, prior).state.controller == 2, "landed troop precedes starbase in controller selection");
    p.landed.clear(); expect(calculate(c, p, prior).state.controller == 1, "starbase controls without surface forces");
    p.starbase = {}; p.production_owner = {{2, 1}};
    expect(calculate(c, p, prior).state.controller == 2, "ground production is examined before space production");
    p.production_owner = {}; p.surface_accessible = false;
    expect(calculate(c, p, prior, {stealth, enemy}).state.controller == 2, "inaccessible surface admits ordinary orbital control");
    p.surface_accessible = true;
    result = calculate(c, p, prior, {enemy});
    expect(result.state.current_player == 3 && result.state.target == Fixed{}, "accessible neutral surface is not conquered by orbital arrival alone");
    p.retains_residual = true;
    expect(calculate(c, p, prior).state.target == f(1), "live residual influence retains prior player");
    p.destroyed = true;
    expect(calculate(c, p, prior).state.target == Fixed{}, "destroyed planet cannot retain residual influence");
    p = c.planets[0]; p.leaning_player = 2; p.leaning = f(9);
    expect(calculate(c, p, prior).state.target == f(1), "opposing leaning clamps to five and military minimum remains one");
    c.players[1].faction = 1;
    expect(calculate(c, p, prior).state.target == f(5), "another player in the controlling faction has positive leaning");
    c.players[1].faction = 2;
    p.leaning_player = 1; p.special_control = {f(1), f(2)}; p.caps = {{1, 3}};
    expect(calculate(c, p, prior).state.target == f(3), "special categories add while faction cap applies");
    p.caps[0].maximum = 0;
    expect(calculate(c, p, prior).state.target == f(5), "invalid faction cap below minimum falls back to five");
    p = c.planets[0]; c.state = {prior, state(2, 1, 1), state(3, 3, 0)};
    expect(calculate(c, p, prior, {}, s.routes).state.target == f(3), "allied trade endpoint at allegiance one contributes gain");
    c.state[1].current = Fixed::from_ratio(1, 2).value();
    expect(calculate(c, p, prior, {}, s.routes).state.target == f(2), "trade needs BOTH endpoints at allegiance at least one");
}
void transitions() {
    const auto s = setup(); const auto c = *s.control; auto p = c.planets[0]; auto prior = state(1, 1, 2);
    p.ground_base = {2, 1}; p.protected_control = true;
    auto result = calculate(c, p, prior);
    expect(result.owner_changed && result.stop_production && result.state.current_player == 2 && result.state.current == Fixed{},
        "WGM-72/74 target-player reset happens before degrading protection; threshold drop stops production");
    p.protected_control = false; result = calculate(c, p, prior);
    const auto delta = Fixed::from_ratio(1, 210).value();
    expect(result.state.current == delta && result.state.target == f(1), "WGM-73 rate is one point per authored duration, not whole-planet ETA");
    prior = result.state;
    for (int i = 1; i < 210; ++i) prior = calculate(c, p, prior).state;
    expect(prior.current == f(1), "transition stops exactly at target without overshoot");
    p.ground_base = {1, 2}; prior = state(1, 1, 1);
    p.transition_modifiers = {{1, 100, f(2), 0}, {1, 101, f(3), 0}, {2, 102, Fixed::from_ratio(1, 2).value(), 0}};
    expect(calculate(c, p, prior).state.current == eawr::sim::math::add(f(1), delta).value(), "transition modifiers take category minimum then multiply categories");
    p.transition_modifiers[0].expires_frame = 5;
    expect(calculate(c, p, prior, {}, {}, 6).state.current < calculate(c, p, prior, {}, {}, 5).state.current,
        "expired fastest modifier leaves slower same-category source");
    p.protected_control = true; p.ground_base.level = 1; prior = state(1, 1, 3);
    expect(calculate(c, p, prior).state.current == f(3), "political protection suppresses same-player degradation");
    p.ground_base.level = 5;
    expect(calculate(c, p, prior).state.current > f(3), "political protection does not suppress gains");
}
gc::GalacticReplay replay() {
    gc::GalacticReplay r; r.setup = setup(); r.final_tick_count = 240;
    r.commands = {{{0, gc::SeatId{1}, 1}, gc::MoveFleet{10, {1, 2}, {}}},
        {{8, gc::SeatId{1}, 2}, gc::LandFleet{10, 2}}, {{9, gc::SeatId{1}, 3}, gc::PauseGalaxy{}},
        {{12, gc::SeatId{1}, 4}, gc::ResumeGalaxy{}}, {{20, gc::SeatId{1}, 5}, gc::SetSpeed{false, f(8)}}};
    return r;
}
void replay_checkpoint_and_hooks() {
    auto r = replay();
    r.setup.observer.emplace(); auto& observer = *r.setup.observer; observer.local_player = 1;
    for (const auto& player : r.setup.players) observer.players.push_back({player.player_id, false, false, 0});
    for (const auto& planet : r.setup.planets) {
        gc::ObserverPlanet row; row.planet = planet.id;
        row.allegiance = row.target_owner = row.current_owner = row.current_controller = planet.owner;
        observer.planets.push_back(row);
    }
    observer.types = {{1, false, false, false}, {2, false, false, false}, {3, false, false, false}};
    observer.planet_spies = {{2, 1, "capture network", gc::bit(gc::Intelligence::income), f(-1), f(30), gc::SpyStyle::galactic_automatic, true, false}};
    expect(bool(gc::initialize_campaign_observer(r.setup, {})), "control fixture initializes registered planet spy");
    const auto bytes = gc::write_replay(r).value();
    expect(gc::parse_replay(bytes).value() == r, "control inputs, current/target, history and landing command roundtrip");
    auto combined = r; combined.setup.seat_control.emplace();
    combined.setup.seat_control->teams = {{1, gc::SeatId{1}, false}};
    for (const auto& player : combined.setup.players)
        combined.setup.seat_control->controllers.push_back({player.player_id,
            player.player_id == 1 ? gc::FactionControl::human : gc::FactionControl::ai,
            gc::FactionControl::ai, {1, 2}, {3, 4}});
    combined.setup.player_economy.emplace();
    for (const auto& player : combined.setup.players)
        combined.setup.player_economy->players.push_back({player.player_id, f(100), f(1000), 0, 5});
    combined.setup.fiscal.emplace(); combined.setup.fiscal->credit_cap_per_planet = f(1000);
    for (const auto& player : combined.setup.players)
        combined.setup.fiscal->players.push_back({player.player_id, true, f(1)});
    for (const auto& planet : combined.setup.planets)
        combined.setup.fiscal->planets.push_back({planet.id, f(20)});
    enable_production(combined.setup);
    expect(gc::parse_replay(gc::write_replay(combined).value()).value() == combined,
        "all eight graph/fleet/economy/movement/seat/fiscal/production/political replay extensions coexist");
    const eawr::sim::InlineExecutor serial;
    auto composed = eawr::campaign::GameSession::create(combined.setup).value();
    (void)composed.step(serial).value();
    expect(composed.galaxy().snapshot()->seat_control().has_value() && composed.galaxy().snapshot()->control()
        && composed.galaxy().snapshot()->player_economy()->players[0].credits == f(100),
        "seat policy and political state both survive snapshot publication with economy enabled");
    const auto identities = cp::game_identities(combined.setup);
    const auto checkpoint = cp::save_game_checkpoint(composed, identities).value();
    auto restored_composed = cp::load_game_checkpoint(checkpoint, identities, {}, serial).value();
    expect(restored_composed.session.step(serial).value().state_hash.get() == composed.step(serial).value().state_hash.get(),
        "combined seat and political checkpoint continues with identical state");
    for (std::size_t size = 0; size < bytes.size(); size += 19) expect(!gc::parse_replay(std::span(bytes).first(size)), "truncated control replay refuses");
    auto bad = r.setup; bad.control->relations.pop_back(); expect(!gc::validate_setup(bad), "relation table must cover every player pair");
    bad = r.setup; bad.control->players[0].faction = 2; expect(!gc::validate_setup(bad), "control faction identities must match owning players");
    bad = r.setup; bad.control->planets[0].landed = {{20, 2, 1}}; expect(!gc::validate_setup(bad), "surface identity cannot alias an orbit member");
    bad = r.setup; bad.control->planets[0].landed = {{40, 2, 1}}; bad.control->absent_forces = {40};
    expect(!gc::validate_setup(bad), "dead roster identity cannot supply live surface control");
    bad = r.setup; bad.control->planets[0].special_control = {f(1), f(1), f(1), f(1)}; expect(!gc::validate_setup(bad), "three special-control categories maximum");
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = eawr::campaign::GameSession::from_replay(r).value();
        std::size_t captures = 0, ownership = 0;
        game.set_control_effects_handler([&](const auto effects, const auto planets) {
            for (const auto& effect : effects) {
                if (effect.owner_changed) {
                    ++ownership; const auto planet = std::find_if(planets.begin(), planets.end(), [&](const auto& row) { return row.id == effect.planet; });
                    expect(planet != planets.end() && planet->owner == effect.new_owner, "credit-cap/visibility hook receives COMMITTED planet owners before fiscal");
                }
                if (effect.first_capture) { ++captures; expect(effect.capture_reward == f(250), "first capture carries authored reward"); }
            }
            return eawr::core::Result<void>::success();
        });
        std::vector<std::string> hashes;
        std::optional<eawr::campaign::GameSession> restored;
        for (std::uint64_t tick = 0; tick < r.final_tick_count; ++tick) {
            const auto step = game.step(pool).value(); hashes.push_back(step.state_hash.get() + ":" + step.galaxy->sha256());
            if (tick == 0) expect(step.galaxy_work.control_planets == r.setup.planets.size()
                && step.galaxy_work.control_route_visits == 2 * r.setup.routes.size()
                && step.galaxy_work.control_member_visits == 1,
                "control work visits each planet and only its incident routes and present flattened members");
            expect(game.galaxy().state_sha256() == game.galaxy().cached_state_hash().get(), "dirty section cache equals full control state hash");
            if (tick == 8) {
                expect(step.galaxy->control()->planets[1].landed == std::vector<gc::SurfaceForce>{{21, 2, 1}}, "uncontested landing moves the persistent transport to surface");
                expect(step.galaxy->planets()[1].owner == 1 && step.galaxy->control()->state[1].current < f(1), "conquest changes controller through allegiance and retains gradual control amount");
                std::size_t old_sources{}, new_sources{};
                for (const auto& record : step.galaxy->observer()->records) for (const auto& source : record.sources)
                    if (source.source == 2 && source.ability == 1) {
                        old_sources += record.player == 3; new_sources += record.player == 1;
                    }
                expect(old_sources == 0 && new_sources == r.setup.planets.size(),
                    "ordered military capture transfers nearby spy intelligence before snapshot/fiscal readers");
            }
            if (tick == 60) {
                const auto ids = cp::game_identities(r.setup);
                const auto saved = cp::save_game_checkpoint(game, ids).value();
                auto loaded = cp::load_game_checkpoint(saved, ids, {}, pool).value();
                expect(loaded.session.state_sha256() == game.state_sha256(), "checkpoint retains half-complete political transition and capture history");
                restored.emplace(std::move(loaded.session));
            } else if (tick > 60 && restored) expect(restored->step(pool).value().state_hash.get() == step.state_hash.get(), "restored transition proceeds without a second capture reward");
        }
        expect(captures == 1 && ownership == 1, "one owner transition and one first-capture effect");
        if (reference.empty()) reference = hashes; else expect(hashes == reference, "control hash/events/snapshots equal on 1/2/4/8 workers");
    }
}
void political_fiscal_deadline() {
    // GFSC-02 / GFD-02/05: political commit, cap clamp, listener, fresh income.
    auto s = setup(); s.rules.fiscal_cycle_frames = 2;
    s.control->state = {state(1, 1, 1), state(2, 1, 1), state(3, 2, 1)};
    s.control->planets[1].ground_base = {1, 1};
    s.control->planets[2].ground_base = {2, 1};
    s.planets[1].owner = 1; s.planets[2].owner = 2;
    s.control->planets[0].protected_control = true;
    // Delay the target-owner change until a landing exactly at the deadline.
    s.control->planets[0].ground_base = {};
    s.control->planets[0].retains_residual = true;
    s.fleets[1].orbit = 1; s.fleets[1].members = {{22, 2}};
    s.player_economy.emplace();
    s.player_economy->players = {{1, f(80), f(100), 0, 5}, {2, {}, f(100), 0, 5}, {3, {}, f(100), 0, 5}};
    s.fiscal.emplace(); s.fiscal->credit_cap_per_planet = f(40);
    s.fiscal->players = {{1, true, f(1)}, {2, true, f(1)}, {3, false, f(1)}};
    s.fiscal->planets = {{1, f(20)}, {2, f(5)}, {3, f(7)}};
    s.seat_control.emplace(); s.seat_control->teams = {{1, gc::SeatId{1}, false}};
    for (const auto& player : s.players)
        s.seat_control->controllers.push_back({player.player_id,
            player.player_id == 1 ? gc::FactionControl::human : gc::FactionControl::ai,
            gc::FactionControl::ai, {}, {}});
    s.control->planets[0].starbase = {1, 1};
    enable_production(s);
    gc::ProductionOffer space; space.type = 1; space.domain = gc::ProductionDomain::space;
    space.price = f(4); space.seconds = Fixed::from_ratio(1, 10).value();
    space.affiliated = true; space.direct_build_allowed = true;
    auto ground = space; ground.type = 2; ground.domain = gc::ProductionDomain::ground; ground.price = f(3);
    s.production->planets[0].offers = {space, ground};
    const gc::GalacticReplay input{s, 5, {
        {{1, gc::SeatId{1}, 1}, gc::ProductionCommand{1, 2, gc::ProductionAction::buy, gc::ProductionDomain::ground, 0}},
        {{1, gc::SeatId{1}, 2}, gc::ProductionCommand{1, 1, gc::ProductionAction::buy, gc::ProductionDomain::space, 0}},
        {{2, gc::SeatId{2}, 1}, gc::LandFleet{11, 1}}}};
    const eawr::sim::InlineExecutor serial;
    auto reference = eawr::campaign::GameSession::from_replay(input).value();
    std::vector<std::string> hashes;
    for (int i = 0; i < 2; ++i) hashes.push_back(reference.step(serial).value().state_hash.get());
    hashes.push_back(reference.step(serial).value().state_hash.get());
    for (int i = 3; i < 5; ++i) hashes.push_back(reference.step(serial).value().state_hash.get());
    const auto identities = cp::game_identities(s);
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = eawr::campaign::GameSession::from_replay(input).value();
        for (int i = 0; i < 2; ++i) expect(game.step(pool).value().state_hash.get() == hashes[i], "pre-deadline worker parity");
        expect(game.galaxy().snapshot()->production()->planets[0].queues[0].size() == 1
            && game.galaxy().snapshot()->production()->planets[0].queues[1].size() == 1,
            "both live prepaid queues exist before deadline conquest");
        const auto saved = cp::save_game_checkpoint(game, identities).value();
        auto restored = cp::load_game_checkpoint(saved, identities, {}, pool).value();
        expect(cp::save_game_checkpoint(restored.session, identities).value() == saved, "all eight extension checkpoint is canonical");
        auto& resumed = restored.session;
        bool clamped_before_listener = false;
        resumed.set_control_effects_handler([&](const auto effects, const auto planets) {
            for (const auto& effect : effects) if (effect.owner_changed && effect.planet == 1) {
                // Read the live canonical ledger; published snapshots represent the last tick.
                const auto bytes = resumed.galaxy().canonical_state_bytes();
                const auto read = [&](std::size_t offset, std::size_t width) {
                    std::uint64_t value = 0;
                    for (std::size_t i = 0; i < width; ++i) value |= std::uint64_t{bytes.at(offset + i)} << (i * 8);
                    return value;
                };
                for (std::size_t offset = 16; offset < bytes.size();) {
                    const auto id = read(offset, 2), size = read(offset + 4, 8);
                    if (id == static_cast<std::uint16_t>(gc::StateSection::players)) {
                        const auto balance_offset = offset + 12 + 4 + s.players.size() * 12 + 4 + 4;
                        clamped_before_listener = planets[0].owner == 2
                            && read(balance_offset, 8) == static_cast<std::uint64_t>(f(40).raw())
                            && read(balance_offset + 8, 8) == static_cast<std::uint64_t>(f(40).raw());
                    }
                    offset += 12 + static_cast<std::size_t>(size);
                }
            }
            return eawr::core::Result<void>::success();
        });
        const auto tick = resumed.step(pool).value();
        expect(tick.state_hash.get() == hashes[2], "political/fiscal same-frame checkpoint parity");
        const auto& ledger = resumed.galaxy().snapshot()->player_economy()->players;
        expect(ledger[0].credit_cap == f(40) && ledger[0].credits == f(40), "losing account clamps at ownership commit");
        expect(ledger[1].credit_cap == f(80) && ledger[1].credits == f(80), "capture reward and fresh fiscal income obey the new ownership cap at deadline");
        expect(tick.galaxy_work.fiscal_cycles == 1 && clamped_before_listener, "one due payment and cap-clamped conquest listener");
        const auto& producer = resumed.galaxy().snapshot()->production()->planets[0];
        expect(producer.queues[0].empty() && producer.queues[1].empty() && !producer.enabled && producer.target_owner == 2,
            "WGM-74 / WGEP-56 stops both queues through live control inputs before production");
        expect(producer.starbase_owner == 0 && producer.space_level == 0,
            "uncontested landing clears production's live station prerequisite input");
        expect(std::count_if(tick.galaxy->events().begin(), tick.galaxy->events().end(), [](const auto& event) {
            return event.kind == gc::GalacticEventKind::production_cancelled;
        }) == 2, "bulk control stop notifies each entry once without direct refund");
        const auto paid_checkpoint = cp::save_game_checkpoint(resumed, identities).value();
        auto after_paid = cp::load_game_checkpoint(paid_checkpoint, identities, {}, pool).value();
        expect(cp::save_game_checkpoint(after_paid.session, identities).value() == paid_checkpoint,
            "paid ledger, cancelled production and control history share canonical checkpoint");
        while (after_paid.session.completed_tick() < 5) {
            const auto index = after_paid.session.completed_tick();
            after_paid.session.scramble_storage_for_testing();
            expect(after_paid.session.step(pool).value().state_hash.get() == hashes[index],
                "eight-extension continuation neither repeats payment nor completes cancelled work");
        }
    }
}
void station_production_presence() {
    auto s = setup(); s.player_economy.emplace(); s.fiscal.emplace();
    s.fiscal->credit_cap_per_planet = f(1000);
    for (const auto& player : s.players) {
        s.player_economy->players.push_back({player.player_id, f(100), f(1000), 0, 5});
        s.fiscal->players.push_back({player.player_id, true, f(1)});
    }
    for (const auto& planet : s.planets) s.fiscal->planets.push_back({planet.id, f(1)});
    enable_production(s);
    gc::ProductionOffer station; station.type = 1; station.domain = gc::ProductionDomain::space;
    station.kind = gc::ProductionKind::starbase; station.price = f(3); station.base_level = 1;
    station.affiliated = true; station.direct_build_allowed = true;
    s.production->planets[0].offers = {station};
    const gc::GalacticReplay input{s, 3, {{{0, gc::SeatId{1}, 1},
        gc::ProductionCommand{1, 1, gc::ProductionAction::buy, gc::ProductionDomain::space, 0}}}};
    const eawr::sim::InlineExecutor serial;
    auto reference = eawr::campaign::GameSession::from_replay(input).value();
    std::vector<std::string> hashes;
    for (int i = 0; i < 3; ++i) hashes.push_back(reference.step(serial).value().state_hash.get());
    const auto identities = cp::game_identities(s);
    auto direct = gc::GalacticSession::from_replay(input).value();
    (void)direct.step(serial).value();
    gc::ControlBattleReturn space_return;
    space_return.planet = 1; space_return.invader = 2; space_return.defender = 1; space_return.winner = 2;
    expect(bool(direct.apply_control_battle(space_return)), "space return reconciles produced station presence");
    expect(direct.snapshot()->control()->planets[0].starbase.level == 0
        && direct.snapshot()->production()->planets[0].space_level == 0
        && direct.snapshot()->production()->planets[0].starbase_owner == 0,
        "WGM-57 space return refreshes production prerequisites before next command admission");
    {
        // #3958: the planet owner counterattacks (invader) into an orbit held by the defender; the station that fought
        // stands for the planet owner's own starbase, so its loss reduces that starbase rather than being skipped.
        auto counter = gc::GalacticSession::from_replay(input).value();
        (void)counter.step(serial).value();
        gc::ControlBattleReturn counter_return;
        counter_return.planet = 1; counter_return.invader = 1; counter_return.defender = 2; counter_return.winner = 2;
        counter_return.station_owner = 1;
        expect(bool(counter.apply_control_battle(counter_return)), "counterattack space return reconciles");
        expect(counter.snapshot()->control()->planets[0].starbase.level == 0
            && counter.snapshot()->production()->planets[0].starbase_owner == 0,
            "#3958 a destroyed station reduces the starbase of the owner it stood for, not the orbit holder's");
        auto kept = gc::GalacticSession::from_replay(input).value();
        (void)kept.step(serial).value();
        counter_return.station_owner = 0;
        expect(bool(kept.apply_control_battle(counter_return)) && kept.snapshot()->control()->planets[0].starbase.level == 1,
            "a return without a station owner falls back to the defender, which does not own this starbase");
    }
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = eawr::campaign::GameSession::from_replay(input).value();
        expect(game.step(pool).value().state_hash.get() == hashes[0], "station placement worker parity");
        expect(game.galaxy().snapshot()->control()->planets[0].starbase == gc::ControlBase{1, 1}
            && game.galaxy().snapshot()->production()->planets[0].space_level == 1,
            "completed starbase supplies the political military presence record");
        const auto saved = cp::save_game_checkpoint(game, identities).value();
        auto restored = cp::load_game_checkpoint(saved, identities, {}, pool).value();
        expect(cp::save_game_checkpoint(restored.session, identities).value() == saved, "station/control checkpoint canonical");
        for (int i = 1; i < 3; ++i) expect(restored.session.step(pool).value().state_hash.get() == hashes[i],
            "later control service retains produced station prerequisites after restore");
    }
}
void tactical_built_persistence() {
    const gc::TacticalBuiltStructure winner_a{701, {f(7), f(-2), f(9)}, 0, 1};
    const gc::TacticalBuiltStructure winner_b{702, {f(-8), f(4), f(1)}, 5, 1};
    const gc::TacticalBuiltStructure loser{703, {f(3), {}, f(2)}, 3, 2};
    const gc::TacticalBuiltStructure neutral{704, {{}, f(1), {}}, 2, 3};
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto s = setup(); s.fleets[0].orbit = 2; s.planets[1].owner = 2;
        s.control->planets[1].ground_base = {};
        auto world = gc::GalacticSession::create(s).value();
        expect(!world.add_tactical_built_structure(2, winner_a, 2), "WGT-84 rejects unequal child/holder owners");
        auto bad = winner_a; bad.enable_level = 6;
        expect(!world.add_tactical_built_structure(2, bad, 1), "WGT-84 checks the sourced enable-level limit");
        for (const auto& record : {loser, winner_a, neutral, winner_b})
            expect(bool(world.add_tactical_built_structure(2, record, record.owner)), "typed persistence writer accepts authored record");
        expect(bool(world.remove_tactical_built_structure(2, loser.type, loser.position)), "typed removal matches type and position");
        expect(bool(world.add_tactical_built_structure(2, loser, loser.owner)), "removed tactical build can be recorded again");
        expect(world.native_effects().size() == 6 && world.native_effects().back().kind == gc::NativeEffectKind::tactical_built,
            "accepted add/remove writers enter the ordered native journal; rejected writes do not");
        auto journaled_game = eawr::campaign::GameSession::from_replay(world.record()).value();
        expect(bool(journaled_game.finish_native_reconstruction(executor)), "built-in typed owner reconstructs barrier persistence writes");
        expect(journaled_game.galaxy().snapshot()->control()->planets[1].built_structures == world.snapshot()->control()->planets[1].built_structures,
            "native replay reconstructs the same ordered records without tactical callbacks");
        const auto writer_identities = cp::game_identities(s);
        auto writer_restored = cp::load_game_checkpoint(cp::save_game_checkpoint(journaled_game, writer_identities).value(),
            writer_identities, {}, executor).value();
        expect(writer_restored.session.galaxy().snapshot()->control()->planets[1].built_structures == world.snapshot()->control()->planets[1].built_structures,
            "fresh checkpoint reconstructs journaled add/remove at frontier zero");
        const auto records = world.snapshot()->control()->planets[1].built_structures;
        expect(records == std::vector<gc::TacticalBuiltStructure>{winner_a, neutral, winner_b, loser},
            "WGT-84 writer preserves type, pose, enable level, owner and insertion order");
        s.control->planets[1].built_structures = records;
        const auto encoded = gc::write_replay({s, 0, {}}).value();
        expect(gc::parse_replay(encoded).value().setup.control->planets[1].built_structures == records,
            "tactical build records round trip in the galaxy encoding");
        auto game = eawr::campaign::GameSession::create(s).value();
        const auto identities = cp::game_identities(s);
        auto restored = cp::load_game_checkpoint(cp::save_game_checkpoint(game, identities).value(), identities, {}, executor).value();
        expect(restored.session.galaxy().snapshot()->control()->planets[1].built_structures == records,
            "fresh checkpoint keeps mixed-owner tactical persistence before capture");
        for (auto* session : {&game, &restored.session})
            expect(bool(session->submit({{0, gc::SeatId{1}, 1}, gc::LandFleet{10, 2}})), "uncontested military capture submits");
        const auto captured = game.step(executor).value();
        expect(restored.session.step(executor).value().state_hash.get() == captured.state_hash.get(), "restored capture equals uninterrupted capture");
        expect(captured.galaxy->control()->planets[1].built_structures == std::vector<gc::TacticalBuiltStructure>{winner_a, winner_b},
            "WGM-187 reverse fixup removes all other owners without conversion and preserves winner values/order");
        auto defender = s; auto defense = gc::GalacticSession::create(defender).value();
        gc::ControlBattleReturn returned; returned.planet = 2; returned.invader = 1; returned.defender = 2;
        returned.winner = 2; returned.land = true;
        expect(bool(defense.apply_control_battle(returned)), "defender win applies ordinary land return");
        expect(defense.snapshot()->control()->planets[1].built_structures == std::vector<gc::TacticalBuiltStructure>{loser},
            "winner-only persistence also applies when the defender wins");
        auto automatic = gc::GalacticSession::create(s).value();
        eawr::campaign::AutoResolveOutcomeApplication application; application.planet = 2; application.planet_type = "ModPlanet";
        application.invader = 1; application.defender = 2; application.land = true; application.fix_land_structures = true;
        application.result.conflict = 90; application.result.winner = 1;
        eawr::campaign::AutoResolveOutcomeServices services;
        services.control = [](auto&, const auto&) { return eawr::core::Result<void>::success(); };
        expect(bool(eawr::campaign::apply_auto_resolve_outcome(automatic, application, services)), "auto-resolve consumes the explicit persistence fix flag");
        expect(automatic.snapshot()->control()->planets[1].built_structures == std::vector<gc::TacticalBuiltStructure>{winner_a, winner_b},
            "automatic return filters tactical records even with a supplied control seam");
        std::vector<std::string> hashes{captured.state_hash.get(), world.step(executor).value().state_hash.get(), defense.step(executor).value().state_hash.get()};
        if (reference.empty()) reference = hashes; else expect(reference == hashes, "tactical persistence is equal on 1/2/4/8 workers");
    }
}
void capture_structure_cleanup() {
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto s = setup(); s.fleets[0].orbit = 2; s.planets[1].owner = 2;
        s.control->planets[1].ground_base = {2, 1}; s.control->planets[1].starbase = {2, 1};
        s.player_economy.emplace(); s.fiscal.emplace(); s.fiscal->credit_cap_per_planet = f(1000);
        for (const auto& player : s.players) {
            s.player_economy->players.push_back({player.player_id, f(100), f(1000), 0, 5});
            s.fiscal->players.push_back({player.player_id, true, f(1)});
        }
        for (const auto& planet : s.planets) s.fiscal->planets.push_back({planet.id, f(1)});
        enable_production(s);
        auto& producer = s.production->planets[1];
        producer.objects = {{40, 3, 2, f(20), gc::ProductionDomain::ground, gc::ProductionKind::structure},
            {41, 3, 1, f(30), gc::ProductionDomain::space, gc::ProductionKind::structure},
            {42, 1, 1, f(10), gc::ProductionDomain::ground, gc::ProductionKind::unit}};
        s.census = gc::PopulationCensus{{{1, f(1), 0}, {2, {}, 0}, {3, f(1), 0}},
            {{1, 100, false, {}}, {2, 100, false, {}}, {3, 100, false, {}}},
            {{20, 1, 1, 2, 10, false, false}, {21, 2, 1, 2, 10, false, false}, {22, 1, 2, 3, 11, false, false},
             {40, 3, 2, 2, 2, false, false}, {41, 3, 1, 2, 2, false, false}, {42, 1, 1, 2, 2, false, false}}};
        qualify_census_roles(*s.census);
        auto world = gc::GalacticSession::create(s).value();
        std::vector<eawr::sim::EntityId> lost;
        world.set_native_actor_changes_handler([&](const auto& snapshot, const auto changes) {
            for (const auto& change : changes) if (change.kind == gc::GalacticSession::NativeActorChange::Kind::lost) {
                lost.push_back(change.actor);
                expect(snapshot.production()->planets[1].objects.size() == 1,
                    "WGM-188 loss listeners receive the committed special actor removal");
            }
            return eawr::core::Result<void>::success();
        });
        gc::ControlBattleReturn returned; returned.planet = 2; returned.invader = 1; returned.defender = 2;
        returned.winner = 1; returned.land = true; returned.surviving_invaders = {{21, 2, 1}};
        expect(bool(world.apply_control_battle(returned)), "land winner applies military cleanup");
        expect(lost == std::vector<eawr::sim::EntityId>{40, 41}
            && world.snapshot()->production()->planets[1].objects == std::vector<gc::ProductionObject>{producer.objects[2]},
            "WGM-188 clears ground and orbital specials of either owner without converting or deleting ordinary units");
        const auto tick = world.step(executor).value();
        expect(tick.snapshot->population()[0].used == f(2), "removed special releases owned population while other actors remain");
        std::vector<std::string> hashes{tick.state_hash.get()};
        // The ordinary uncontested invasion must use the same special removal.
        s.control->planets[1].ground_base = {};
        auto game = eawr::campaign::GameSession::create(s).value();
        expect(bool(game.submit({{0, gc::SeatId{1}, 1}, gc::LandFleet{10, 2}})), "undefended capture submits");
        const auto captured = game.step(executor).value(); hashes.push_back(captured.state_hash.get());
        expect(captured.galaxy->production()->planets[1].objects.size() == 1, "uncontested landing removes real specials too");
        const auto identities = cp::game_identities(s);
        auto restored = cp::load_game_checkpoint(cp::save_game_checkpoint(game, identities).value(), identities, {}, executor).value();
        expect(restored.session.galaxy().snapshot()->production()->planets[1].objects.size() == 1,
            "checkpoint restore does not resurrect removed structures");
        const auto continued = game.step(executor).value(); hashes.push_back(continued.state_hash.get());
        expect(restored.session.step(executor).value().state_hash.get() == continued.state_hash.get(), "special cleanup restore continues equally");
        if (reference.empty()) reference = hashes; else expect(hashes == reference, "special removal is equal on 1/2/4/8 workers");
    }
}
void first_capture_rewards() {
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto s = setup(); s.rules.fiscal_cycle_frames = 1000;
        s.fleets[0].orbit = 2; s.fleets[1].owner = 1; s.fleets[1].members = {{22, 2}};
        s.control->planets[2].type = s.control->planets[1].type;
        s.player_economy = gc::PlayerEconomySetup{{{1, f(10), f(400), 0, 5}, {2, {}, f(400), 0, 5}, {3, {}, f(400), 0, 5}}};
        s.fiscal = gc::FiscalSetup{f(400), {{1, true, f(2)}, {2, true, f(1)}, {3, false, f(1)}}, {{1, {}}, {2, {}}, {3, {}}}};
        auto world = gc::GalacticSession::create(s).value();
        std::size_t conquests = 0, windfalls = 0;
        world.set_control_effects_handler([&](const auto effects, const auto) {
            for (const auto& effect : effects) if (effect.owner_changed) ++conquests;
            expect(windfalls == 0, "WGM-75 conquest listeners precede first-capture award");
            return eawr::core::Result<void>::success();
        });
        world.set_credit_notification_handler([&](const auto& notice) {
            ++windfalls;
            expect(conquests == 2 && notice.kind == gc::GalacticSession::CreditNotification::Kind::windfall
                && notice.player == 1 && notice.credits == f(510) && notice.amount == f(500),
                "WGM-76 authored reward uses difficulty adjustment and delivers the actual AI windfall");
            return eawr::core::Result<void>::success();
        });
        expect(bool(world.submit({{0, gc::SeatId{1}, 1}, gc::LandFleet{10, 2}}))
            && bool(world.submit({{0, gc::SeatId{1}, 2}, gc::LandFleet{11, 3}})), "same-type captures submit");
        const auto captured = world.step(executor).value();
        expect(windfalls == 1 && captured.snapshot->player_economy()->players[0].credits == f(510),
            "one reward is paid for two simultaneously captured planets of the same type");
        std::vector<std::string> hashes{captured.state_hash.get()};
        const auto identities = cp::game_identities(s);
        auto game = eawr::campaign::GameSession::from_replay(world.record()).value();
        (void)game.step(executor).value();
        auto restored = cp::load_game_checkpoint(cp::save_game_checkpoint(game, identities).value(), identities, {}, executor).value();
        world.set_control_effects_handler({});
        for (int i = 0; i < 3; ++i) {
            const auto tick = world.step(executor).value(); hashes.push_back(tick.state_hash.get());
            (void)restored.session.step(executor).value();
            expect(restored.session.galaxy().state_sha256() == tick.state_hash.get(),
                "saved first-capture history prevents reward replay after load");
        }
        expect(windfalls == 1, "control continuation does not pay again");
        if (reference.empty()) reference = hashes; else expect(hashes == reference, "reward balances/history equal on 1/2/4/8 workers");
    }
}
void native_capture_notices() {
    namespace st = gc::story;
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto s = setup(); s.rules.fiscal_cycle_frames = 1000; s.fleets[0].orbit = 2;
        s.fleets[1].owner = 1; s.fleets[1].members = {{22, 2}};
        s.control->planets[2].type = s.control->planets[1].type;
        s.planets[0].owner = 3; s.control->planets[0].ground_base = {};
        s.fleets.push_back({12, 1, 1, {{23, 2}}});
        s.player_economy = gc::PlayerEconomySetup{{{1, f(10), f(1000), 0, 5}, {2, {}, f(1000), 0, 5}, {3, {}, f(1000), 0, 5}}};
        auto game = eawr::campaign::GameSession::create(s).value();
        game.set_story_identities({{2, "ModMoon"}, {3, "ModTwin"}}, {{1, "ModFaction"}});
        std::vector<std::string> delivered;
        game.set_story_notice_handler([&](const auto& notice) {
            expect(!notice.player, "WGM-192 conquest broadcasts without a winner filter");
            delivered.emplace_back(st::event_name(notice.payload));
            return eawr::core::Result<void>::success();
        });
        bool observer_called = false;
        game.set_control_effects_handler([&](const auto, const auto) {
            observer_called = true;
            expect(delivered == std::vector<std::string>{"STORY_CONQUER", "STORY_CONQUER_COUNT", "STORY_CONQUER", "STORY_CONQUER_COUNT"},
                "native conquer/count delivery composes with the external pre-reward observer");
            return eawr::core::Result<void>::success();
        });
        game.submit({{0, gc::SeatId{1}, 1}, gc::LandFleet{10, 2}}).value();
        game.submit({{0, gc::SeatId{1}, 2}, gc::LandFleet{11, 3}}).value();
        const auto tick = game.step(executor).value();
        expect(observer_called && tick.story_notices.size() == 4 && tick.galaxy->player_economy()->players[0].credits == f(260),
            "each ownership callback publishes two notices, independently of one per-type reward");
        const auto& conquest = std::get<st::ConquestNotice>(tick.story_notices[0].payload);
        const auto& count = std::get<st::ConquestCountNotice>(tick.story_notices[1].payload);
        expect(conquest.planet == "ModMoon" && conquest.object == 2 && count.object == 2 && count.planet == conquest.planet,
            "authored planet name and original object identity survive both notice families");
        expect(tick.observer_story_notices(1).size() == 4 && tick.observer_story_notices(2).empty()
            && tick.observer_story_notices(0).empty(), "observer publication reveals ownership notices only to the involved owner");
        const auto identities = cp::game_identities(s);
        std::size_t restored_callbacks = 0;
        cp::GameLoadOptions options; options.script_requirement = eawr::campaign::ScriptRequirement::none;
        options.configure_session = [&](auto& restored_game) {
            restored_game.set_story_notice_handler([&](const auto&) {
                ++restored_callbacks; return eawr::core::Result<void>::success();
            });
            return eawr::core::Result<void>::success();
        };
        auto restored = cp::load_game_checkpoint(cp::save_game_checkpoint(game, identities).value(), identities, options, executor).value();
        expect(restored_callbacks == 0, "restore installer never receives historical conquest/count callbacks");
        game.set_control_effects_handler({});
        const auto next = game.step(executor).value();
        const auto resumed = restored.session.step(executor).value();
        expect(next.story_notices.empty() && resumed.story_notices.empty() && resumed.state_hash.get() == next.state_hash.get(),
            "fresh checkpoint continuation does not republish past conquest notices");
        for (auto* session : {&game, &restored.session})
            session->submit({{2, gc::SeatId{1}, 3}, gc::LandFleet{12, 1}}).value();
        const auto future = game.step(executor).value();
        const auto restored_future = restored.session.step(executor).value();
        expect(restored_callbacks == 2 && restored_future.story_notices.size() == 2
            && restored_future.state_hash.get() == future.state_hash.get(),
            "restored shared sink receives the next new conquest and count exactly once");
        const std::vector<std::string> hashes{tick.state_hash.get(), next.state_hash.get(), future.state_hash.get()};
        if (reference.empty()) reference = hashes; else expect(hashes == reference, "notice publication preserves 1/2/4/8 worker hashes");
    }
}
// #2918: LandFleet arrivals reach the observer's live landed_transports exactly like placement commits.
void landfleet_observer_roster() {
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto s = setup();
        s.fleets[0].orbit = 2; s.fleets[0].members = {{21, 2}, {31, 2}};
        s.planets[1].owner = 1;
        s.control->planets[1].ground_base = {};
        gc::ObserverSetup o; o.local_player = 1; o.reveal_delay_seconds = f(1); o.automatic_reveal_range = f(1);
        o.players = {{1, false, false, 0, 0, {}}, {2, false, false, 0, 0, {}}, {3, false, false, 0, 0, {}}};
        for (const auto& p : s.planets) {
            o.planets.push_back({p.id, p.owner, p.owner, 0, 0, 0, 0, false, false, false, false, {}});
            o.planets.back().current_owner = o.planets.back().current_controller = p.owner;
        }
        o.types = {{1, false, false, false}, {2, false, false, false}, {3, false, false, false}};
        for (const auto& player : s.players) for (const auto& p : s.planets) o.records.push_back({player.player_id, p.id, false, 0, 0, 0, {}});
        s.observer = std::move(o);
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto created = gc::GalacticSession::create(s);
        if (!created) throw std::runtime_error(created.error().message);
        auto world = std::move(created).value();
        const auto roster = [&](const auto& snapshot) {
            std::vector<std::pair<eawr::sim::EntityId, gc::PlayerId>> ids;
            for (const auto& row : snapshot->observer()->planets) if (row.planet == 2)
                for (const auto& t : row.landed_transports) ids.emplace_back(t.id, t.owner);
            return ids;
        };
        expect(roster(world.snapshot()).empty(), "no surface roster before landing");
        expect(bool(world.submit({{0, gc::SeatId{1}, 1}, gc::LandFleet{10, 2}})), "company landing submits");
        const auto tick = world.step(executor).value();
        const auto landed = roster(tick.snapshot);
        expect(landed == std::vector<std::pair<eawr::sim::EntityId, gc::PlayerId>>{{21, 1}, {31, 1}},
            "LandFleet arrivals append to the observer roster in member order");
        const auto& control = tick.snapshot->control()->planets[1].landed;
        expect(control.size() == 2 && control[0].id == 21 && control[1].id == 31, "observer roster matches the live surface list");
        for (const auto& row : tick.snapshot->observations()) if (row.planet == 2) {
            const bool owner = row.player == 1;
            expect(row.knowledge.ground_contents == owner && row.knowledge.ground_count == owner,
                "observer content and count gates still filter non-owner players");
        }
        if (workers == 1) reference = {tick.state_hash.get()};
        else expect(reference == std::vector<std::string>{tick.state_hash.get()}, "landing roster state hash is equal on 1/2/4/8 workers");
    }
}
// #3189: a planet that changes hands reaches the observer's planet facts and the local player's observation.
template<class T> T capture_checked(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(eawr::core::format_diagnostic(result.error()));
    return std::move(result).value();
}
void capture_build_and_ground_views() {
    namespace campaign = eawr::campaign;
    namespace tactical = eawr::sim::tactical;
    namespace hud = eawr::viewer::galactic;
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (const bool automatic : {false, true}) {
            std::cout << "capture-view workers=" << workers << " automatic=" << automatic << std::endl;
            auto s = setup(); s.rules.fiscal_cycle_frames = 10000;
            s.fleets[0].orbit = 2; s.fleets[0].members = {{21, 2}};
            s.fleets[1].orbit = 2; s.fleets[1].members = {{22, 2}};
            s.planets[1].owner = 2; s.control->planets[1].ground_base = {2, 1};
            s.control->planets[1].starbase = {2, 2}; s.control->change_seconds = f(1);
            s.player_economy = gc::PlayerEconomySetup{{{1, f(1000), f(10000), 0, 5},
                {2, f(1000), f(10000), 0, 5}, {3, {}, f(10000), 0, 5}}};
            s.fiscal.emplace(); s.fiscal->credit_cap_per_planet = f(10000);
            for (const auto& player : s.players) s.fiscal->players.push_back({player.player_id, false, f(1)});
            for (const auto& planet : s.planets) s.fiscal->planets.push_back({planet.id, {}});
            enable_production(s);
            gc::ProductionOffer offer; offer.type = 3; offer.kind = gc::ProductionKind::structure;
            offer.domain = gc::ProductionDomain::ground; offer.price = offer.authored_price = f(5);
            offer.seconds = f(10); offer.direct_build_allowed = true;
            // Renamed mod originals: initial-owner flags deliberately disagree with the capturer.
            offer.affiliated = false;
            auto enemy = offer; enemy.type = 4; enemy.affiliated = true;
            auto shared = offer; shared.type = 5;
            s.production->technology_types = {{3, 0, false, false, {1}}, {4, 0, false, false, {2}},
                {5, 0, false, false, {1, 2}}};
            s.production->planets[1].offers = {offer, enemy, shared};
            s.production->planets[1].objects = {{40, 3, 2, f(5), gc::ProductionDomain::ground, gc::ProductionKind::structure}};
            gc::ObserverSetup o; o.local_player = 1;
            o.players = {{1, false, false, 0}, {2, true, false, 0}, {3, true, false, 0}};
            o.types = {{1, false, false, false}, {2, false, false, false}, {3, false, false, false}};
            for (std::size_t index = 0; index < s.planets.size(); ++index) {
                const auto& p = s.planets[index]; gc::ObserverPlanet row;
                row.planet = p.id; row.allegiance = row.target_owner = row.current_owner = row.current_controller = p.owner;
                row.ground_owner = s.control->planets[index].ground_base.owner;
                row.ground_level = s.control->planets[index].ground_base.level;
                row.space_owner = s.control->planets[index].starbase.owner;
                row.space_level = s.control->planets[index].starbase.level;
                o.planets.push_back(row);
            }
            // Already discovered before capture, with no automatic planet spy to mask the missing refresh.
            for (const auto& player : s.players) for (const auto& p : s.planets) {
                gc::ObserverRecord row; row.player = player.player_id; row.planet = p.id; row.discovered = true;
                if (row.player == p.owner) row.mask = row.player == 1 ? gc::owner_intelligence : gc::all_intelligence;
                if (row.player == 2 && p.id == 2) row.sources.push_back({2, 17, "independent-mod-source",
                    gc::bit(gc::Intelligence::major_stealth), std::numeric_limits<std::uint64_t>::max()});
                o.records.push_back(row);
            }
            s.observer = o;
            auto game = capture_checked(campaign::GameSession::create(s));
            campaign::ConflictRecord conflict; conflict.id = game.galaxy().next_entity_id();
            conflict.planet = 2; conflict.invader = 1; conflict.defender = 2; conflict.layer = campaign::BattleLayer::land;
            conflict.forces = {{21, 2, 1, 10}, {22, 2, 2, 11}};
            conflict.seats = {{{1}, 1}, {{2}, 2}};
            conflict.tactical.content_identity = s.content_identity;
            conflict.tactical.first_free_entity_id = conflict.id + 20;
            conflict.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
            for (const auto& force : conflict.forces) {
                tactical::UnitState unit; unit.entity_id = force.id; unit.type_id = force.type; unit.owner = force.owner;
                conflict.tactical.units.push_back(unit);
            }
            tactical::TacticalContent content; tactical::DurabilityProfile durability;
            durability.type_id = 2; durability.max_hull = f(100); content.durability.profiles = {durability};
            content.victory.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
            content.victory.contenders = content.victory.controlled_players = {1, 2};
            content.victory.humans = {1}; content.victory.relevant_types = {2}; content.victory.countdown_frames = 2;
            cp::GameLoadOptions load; load.script_requirement = eawr::campaign::ScriptRequirement::none;
            load.content = [content](const auto&) { return eawr::core::Result<tactical::TacticalContent>::success(content); };
            load.auto_resolve_outcomes.control = [](auto& galaxy, const auto& application) {
                gc::ControlBattleReturn returned; returned.planet = application.planet;
                returned.invader = application.invader; returned.defender = application.defender;
                returned.winner = application.result.winner; returned.land = application.land;
                returned.destroyed = application.destroyed; returned.surviving_invaders = {{21, 2, 1}};
                return galaxy.apply_control_battle(returned);
            };
            if (automatic) {
                game.set_auto_resolve_outcome_services(load.auto_resolve_outcomes);
                // Exercise the journaled solver return, as auto_resolve_outcome_tests does.
                // Restore replays that trusted input rather than generating a second outcome.
                campaign::AutoResolveOutcomeApplication a; a.planet = conflict.planet;
                a.planet_type = "RenamedModPlanet"; a.invader = 1; a.defender = 2;
                a.land = true; a.fix_land_structures = true; a.land_invaders = {21};
                a.destroyed = {22}; a.result.conflict = conflict.id; a.result.winner = 1;
                a.result.forces = {{21, campaign::ForceDisposition::survived}, {22, campaign::ForceDisposition::destroyed}};
                capture_checked(game.stage_auto_resolve_outcome(a));
            } else {
                // M3 menu admission routes land to the solver; direct staging exercises the child return itself.
                game.stage_battle(conflict, content, eawr::campaign::ScriptRequirement::none).value(); (void)capture_checked(game.step(executor));
                tactical::TacticalReplay damage; damage.setup.players = conflict.tactical.players; damage.final_tick_count = 1;
                damage.commands.push_back({{0, 1, 1}, {22}, tactical::DamagePayload{f(1000)}});
                const auto bytes = capture_checked(tactical::write_replay(damage));
                const auto header = static_cast<std::size_t>(bytes[10]) | (static_cast<std::size_t>(bytes[11]) << 8U);
                const auto start = header + damage.setup.players.size() * 24 + 4;
                game.submit({{2, {1}, 1}, gc::TacticalLayerCommand{conflict.id, bytes[start + 20],
                    {bytes.begin() + static_cast<std::ptrdiff_t>(start), bytes.end()}}}).value();
            }
            bool returned{};
            while (game.completed_tick() < 60) {
                const auto tick = capture_checked(game.step(executor)); hashes.push_back(tick.state_hash.get());
                returned |= tick.result && tick.result->winner == 1;
                if (returned && tick.galaxy->production()->planets[1].enabled && tick.galaxy->planets()[1].owner == 1) break;
            }
            expect(returned, automatic ? "automatic land victory returns" : "tactical land child victory returns");
            const auto verify = [&](const auto& snapshot) {
                const auto observations = snapshot.observations();
                const auto& observation = *std::find_if(observations.begin(), observations.end(),
                    [](const auto& row) { return row.player == 1 && row.planet == 2; });
                expect(snapshot.planets()[1].owner == 1 && observation.known_owner == 1U, "capture changes actual and observed owner");
                expect(observation.knowledge.production && observation.knowledge.ground_contents && observation.knowledge.ground_count,
                    "pre-fix symptom: new owner mask lacks production/ground contents despite owned colour after capture");
                const auto& planet = snapshot.production()->planets[1];
                expect(planet.target_owner == 1 && planet.ground_slots == 3 && planet.objects.empty(),
                    "capture clears defender structures and retains authored build slots for the invader");
                std::vector<gc::TypeId> current;
                for (const auto& row : snapshot.production_offers()) if (row.player == 1 && row.planet == 2) {
                    if (row.current) current.push_back(row.type);
                    if (row.type == 4) expect(row.admission == gc::ProductionAdmission::unavailable,
                        "previous owner's structure is unavailable despite its cached affiliated flag");
                }
                expect(current == std::vector<gc::TypeId>{3, 5},
                    "captured planet offers the capturer's exclusive and shared mod types");
                expect(snapshot.control()->planets[1].landed == std::vector<gc::SurfaceForce>{{21, 2, 1}}
                    && snapshot.observer()->planets[1].landed_transports.front().owner == 1,
                    "surviving invading transport keeps identity and owner on the captured surface");
                expect(std::none_of(snapshot.fleets().begin(), snapshot.fleets().end(), [](const auto& fleet) {
                    return std::any_of(fleet.members.begin(), fleet.members.end(), [](const auto& member) { return member.id == 21; });
                }), "landed invading transport leaves its orbital fleet exactly once");
                auto facts = hud::observed_hud_facts(observation, &snapshot.observer()->planets[1], &planet, 1, {});
                if (observation.knowledge.production) {
                    facts.ground_queue.emplace(); facts.maximum_queue = snapshot.production()->maximum_human_queue;
                    for (const auto& row : snapshot.production_offers()) if (row.player == 1 && row.planet == 2 && row.current) {
                        hud::ProductionChoice choice; choice.type = "RenamedModStructure"; choice.type_id = row.type;
                        choice.current = row.admission == gc::ProductionAdmission::allowed; facts.choices.push_back(choice);
                    }
                }
                const auto view = hud::planet_hud_view(&facts, false, 0, 5);
                expect(facts.ground_capacity == 3 && view.accepts_queue && view.choices.size() == 2
                    && view.choices[0].enabled && view.choices[1].enabled,
                    "pre-fix symptom: captured planet HUD has no enabled build offer even though simulation retains slots");
                const auto& old = snapshot.observer()->records[4];
                expect(!(old.mask & gc::bit(gc::Intelligence::production)) && old.sources.size() == 1
                    && (old.mask & gc::bit(gc::Intelligence::major_stealth)), "losing owner drops default permissions and keeps independent source");
            };
            verify(*game.galaxy().snapshot());
            const auto identities = cp::game_identities(s);
            const auto saved = capture_checked(cp::save_game_checkpoint(game, identities));
            auto restored = capture_checked(cp::load_game_checkpoint(saved, identities, load, executor));
            expect(restored.session.state_sha256() == game.state_sha256(), "captured views save/load with canonical equality");
            verify(*restored.session.galaxy().snapshot());
            const gc::GameCommand foreign{{game.completed_tick(), {1}, 10},
                gc::ProductionCommand{2, 4, gc::ProductionAction::buy, gc::ProductionDomain::ground, 0}};
            game.submit(foreign).value(); restored.session.submit(foreign).value();
            const auto continued = capture_checked(game.step(executor)); hashes.push_back(continued.state_hash.get());
            expect(game.galaxy().snapshot()->production()->planets[1].queues[0].empty(),
                "injected purchase cannot queue a previous-owner structure after capture");
            expect(capture_checked(restored.session.step(executor)).state_hash.get() == continued.state_hash.get(), "captured save continues equally");
            const gc::GameCommand own{{game.completed_tick(), {1}, 11},
                gc::ProductionCommand{2, 3, gc::ProductionAction::buy, gc::ProductionDomain::ground, 0}};
            game.submit(own).value(); restored.session.submit(own).value();
            const auto purchased = capture_checked(game.step(executor)); hashes.push_back(purchased.state_hash.get());
            expect(capture_checked(restored.session.step(executor)).state_hash.get() == purchased.state_hash.get(),
                "restored capturer purchase agrees");
            const auto& queue = game.galaxy().snapshot()->production()->planets[1].queues[0];
            expect(queue.size() == 1 && queue.front().type == 3 && queue.front().builder == 1,
                "capturer can queue its structure despite the cached initial-owner refusal");
        }
        if (reference.empty()) reference = hashes; else expect(reference == hashes, "both capture routes and restore agree on 1/2/4/8 workers");
    }
}
void captured_planet_observer_facts() {
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto s = setup();
        s.fleets[0].orbit = 2; s.planets[1].owner = 2; s.control->planets[1].ground_base = {2, 1};
        s.control->planets[1].starbase = {2, 2};
        gc::ObserverSetup o; o.local_player = 1; o.reveal_delay_seconds = f(1); o.automatic_reveal_range = f(1);
        o.players = {{1, false, false, 0, 0, {}}, {2, false, false, 0, 0, {}}, {3, false, false, 0, 0, {}}};
        for (const auto& p : s.planets) {
            o.planets.push_back({p.id, p.owner, p.owner, 0, 0, 0, 0, false, false, false, false, {}});
            o.planets.back().current_owner = o.planets.back().current_controller = p.owner;
        }
        o.planets[1].ground_owner = 2; o.planets[1].ground_level = 1; o.planets[1].space_owner = 2; o.planets[1].space_level = 2;
        o.types = {{1, false, false, false}, {2, false, false, false}, {3, false, false, false}};
        for (const auto& player : s.players) for (const auto& p : s.planets) o.records.push_back({player.player_id, p.id, true, 0, 0, 0, {}});
        s.observer = std::move(o);
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto created = gc::GalacticSession::create(s);
        if (!created) throw std::runtime_error(created.error().message);
        auto world = std::move(created).value();
        const auto facts = [&]() { return world.snapshot()->observer()->planets[1]; };
        expect(facts().current_owner == 2 && facts().allegiance == 2 && facts().ground_level == 1 && facts().space_level == 2,
            "the defender holds the planet before the invasion");
        gc::ControlBattleReturn result; result.planet = 2; result.invader = 1; result.defender = 2; result.winner = 1;
        result.land = true; result.surviving_invaders = {{21, 2, 1}};
        expect(bool(world.apply_control_battle(result)), "the land return supplies its surviving transport");
        expect(facts().ground_level == 0 && facts().ground_owner == 0 && facts().space_level == 0 && facts().space_owner == 0
            && facts().current_owner == 2, "the return clears the observed bases at once and leaves ownership to the control service");
        const auto tick = world.step(executor).value();
        expect(tick.snapshot->planets()[1].owner == 1, "the control service hands the planet to the invader");
        expect(facts().current_owner == 1 && facts().allegiance == 1 && facts().target_owner == 1 && facts().current_controller == 1,
            "the observer's planet facts follow the ownership flip");
        const auto observations = tick.snapshot->observations();
        const auto observed = std::find_if(observations.begin(), observations.end(),
            [](const auto& row) { return row.player == 1 && row.planet == 2; });
        expect(observed != observations.end(), "the captured planet has a local-player observation");
        if (observed != observations.end())
            expect(observed->knowledge.ownership && observed->known_owner == 1U,
                "the local player's observation knows the captured planet is theirs");
        if (workers == 1) reference = {tick.state_hash.get()};
        else expect(reference == std::vector<std::string>{tick.state_hash.get()}, "captured-planet observer state is equal on 1/2/4/8 workers");
    }
}
// #2970: every surface add publishes to the observer roster and every surface removal un-publishes from it.
void native_action_barrier_control() {
    const auto require = [](const auto& result) { if (!result) throw std::runtime_error(result.error().message); };
    const auto same = [](const auto& a, const auto& b) { return a.admitted == b.admitted && a.object == b.object && a.reason == b.reason; };
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto config = setup(); config.planets[1].owner = 1;
        config.movement->types[1].transport = true;
        config.fleets[0].orbit = 2; config.fleets[0].members = {{20, 1}, {21, 2}, {31, 2}};
        config.fleets[1].members = {{22, 2}}; config.control->planets[1].ground_base = {1, 1};
        config.player_economy.emplace(); config.fiscal.emplace(); config.fiscal->credit_cap_per_planet = f(1000);
        config.census.emplace(); config.census->types = {{1, f(1), 0}, {2, {}, 0}, {3, f(1), 0}};
        for (const auto& player : config.players) {
            config.player_economy->players.push_back({player.player_id, f(100), f(1000), 0, 5});
            config.fiscal->players.push_back({player.player_id, true, f(1)});
            config.census->players.push_back({player.player_id, 100, true, {}});
        }
        for (const auto& planet : config.planets) config.fiscal->planets.push_back({planet.id, f(1)});
        enable_production(config);
        config.control->planets[1].landed = {{50, 2, 2}, {51, 2, 1}, {52, 2, 3}};
        for (const auto& force : config.control->planets[1].landed) {
            gc::CensusObject actor; actor.id = force.id; actor.original_type = actor.current_type = force.type;
            actor.owner = force.owner; actor.planet = actor.container = 2; actor.combat_role = gc::ActorCombatRole::transport;
            config.census->objects.push_back(actor); config.movement->contents.push_back({actor.id, {}});
        }
        for (const auto& fleet : config.fleets) for (const auto& member : fleet.members) {
            gc::CensusObject actor; actor.id = member.id; actor.original_type = actor.current_type = member.type;
            actor.owner = fleet.owner; actor.planet = fleet.orbit; actor.container = fleet.id;
            actor.combat_role = member.type == 2 ? gc::ActorCombatRole::transport : gc::ActorCombatRole::ordinary;
            config.census->objects.push_back(actor);
            if (member.type == 2) config.movement->contents.push_back({member.id, {}});
        }
        std::sort(config.census->objects.begin(), config.census->objects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        std::sort(config.movement->contents.begin(), config.movement->contents.end(), [](const auto& a, const auto& b) { return a.container < b.container; });
        gc::ObserverSetup observer; observer.local_player = 1; observer.reveal_delay_seconds = f(1); observer.automatic_reveal_range = f(1);
        observer.players = {{1, false, false, 0, 0, {}}, {2, false, false, 0, 0, {}}, {3, false, false, 0, 0, {}}};
        for (const auto& planet : config.planets) {
            observer.planets.push_back({planet.id, planet.owner, planet.owner, 0, 0, 0, 0, false, false, false, false, {}});
            observer.planets.back().current_owner = observer.planets.back().current_controller = planet.owner;
            for (const auto& surface : config.control->planets) if (surface.planet == planet.id)
                for (const auto& force : surface.landed) observer.planets.back().landed_transports.push_back({force.id, force.owner, false, false});
        }
        observer.types = {{1, false, false, false}, {2, false, false, false}, {3, false, false, false}};
        for (const auto& player : config.players) for (const auto& planet : config.planets)
            observer.records.push_back({player.player_id, planet.id, false, 0, 0, 0, {}});
        config.observer = std::move(observer);
        auto staged_result = gc::GalacticSession::create(config); auto direct_result = gc::GalacticSession::create(config);
        require(staged_result); require(direct_result);
        auto staged = std::move(staged_result).value(); auto direct = std::move(direct_result).value();
        staged.enable_native_invasions(true); direct.enable_native_invasions(true);
        const auto initial = staged.canonical_state_bytes(); const auto record = staged.record();
        const std::array<gc::PlayerId, 2> players{1, 2};
        auto opened = staged.begin_native_barrier(players, 256); require(opened); auto barrier = std::move(opened).value();
        require(barrier->select_player(1));
        const auto check = [&] {
            expect(staged.snapshot()->canonical_bytes() == direct.snapshot()->canonical_bytes(), "native control read publication matches the live oracle");
            expect(staged.canonical_state_bytes() == initial && staged.record() == record, "native control decisions leave the galaxy and journal unchanged before commit");
        };
        auto left = staged.land_native_transports(1, 10); auto right = direct.land_native_transports(1, 10);
        require(left); require(right); expect(same(left.value(), right.value()) && left.value().admitted, "staged fleet landing has the live receipt"); check();
        left = staged.launch_native_transport(1, 31, 10); right = direct.launch_native_transport(1, 31, 10);
        require(left); require(right); expect(same(left.value(), right.value()) && left.value().admitted, "staged launch has the live receipt"); check();
        left = staged.admit_native_fleet(1, gc::RemoveFleetMembers{10, {31}}, false);
        right = direct.admit_native_fleet(1, gc::RemoveFleetMembers{10, {31}}, false);
        require(left); require(right); expect(same(left.value(), right.value()), "staged detachment has the live receipt"); check();
        const auto gate = staged.native_transport_landing_available(1, 2); const auto oracle_gate = direct.native_transport_landing_available(1, 2);
        require(gate); require(oracle_gate); expect(gate.value() == oracle_gate.value(), "landing availability reads the current staged surface");
        auto landed = staged.land_native_transport(1, 31, 2); auto oracle_landed = direct.land_native_transport(1, 31, 2);
        require(landed); require(oracle_landed);
        expect(same(landed.value().event, oracle_landed.value().event) && landed.value().linked == oracle_landed.value().linked && landed.value().linked,
            "direct actor landing keeps its event and actual-link result"); check();
        left = staged.launch_native_transport(1, 31, 10); right = direct.launch_native_transport(1, 31, 10);
        require(left); require(right); expect(same(left.value(), right.value()), "relaunch reads the staged actor link"); check();
        auto invasion = staged.admit_native_invasion(1, 10); auto oracle_invasion = direct.admit_native_invasion(1, 10);
        require(invasion); require(oracle_invasion); expect(invasion.value() == oracle_invasion.value() && invasion.value().admitted,
            "same-owner invasion completion reserves the same retained identity"); check();
        const auto completed = invasion.value().request; const auto frozen = staged.snapshot();
        require(barrier->select_player(2));
        left = staged.admit_native_fleet(2, gc::SetFleetOrbitSlot{11, 1}, false);
        right = direct.admit_native_fleet(2, gc::SetFleetOrbitSlot{11, 1}, false);
        require(left); require(right); expect(same(left.value(), right.value()), "higher player inherits the lower player frontier"); check();
        invasion = staged.admit_native_invasion(2, 11); oracle_invasion = direct.admit_native_invasion(2, 11);
        require(invasion); require(oracle_invasion); expect(invasion.value() == oracle_invasion.value() && invasion.value().queued,
            "queued invasion records stay private with the exact ordered identity");
        const auto pending = invasion.value().request;
        expect(staged.pending_native_invasion() && direct.pending_native_invasion()
            && *staged.pending_native_invasion() == *direct.pending_native_invasion(), "pending invasion reader sees staged records");
        require(staged.retire_native_invasion(pending)); require(direct.retire_native_invasion(pending));
        expect(staged.native_invasion(pending) && *staged.native_invasion(pending) == *direct.native_invasion(pending),
            "retiring a pending reader keeps its queued transport inventory"); check();
        for (unsigned transaction = 0; transaction < 40; ++transaction) {
            auto empty = staged.begin_native_actions(1); auto oracle_empty = direct.begin_native_actions(1);
            require(empty); require(oracle_empty);
            require(staged.commit_native_actions(empty.value())); require(direct.commit_native_actions(oracle_empty.value()));
        }
        expect(barrier->work().read_checkpoints > 0 && staged.pending_native_invasion()
            && *staged.pending_native_invasion() == *direct.pending_native_invasion(),
            "sparse checkpoints retain the earliest queue entry and released reader state");
        require(barrier->begin_tail());
        left = staged.admit_native_fleet(1, gc::SetFleetOrbitSlot{10, 2}, false);
        right = direct.admit_native_fleet(1, gc::SetFleetOrbitSlot{10, 2}, false);
        require(left); require(right); expect(same(left.value(), right.value()), "serial tail accepts an earlier player's deferred command"); check();
        require(staged.retire_native_invasion(completed)); require(direct.retire_native_invasion(completed));
        expect(!staged.native_invasion(completed) && !direct.native_invasion(completed), "completed invasion reader disappears at release");
        for (unsigned transaction = 0; transaction < 40; ++transaction) {
            auto empty = staged.begin_native_actions(1); auto oracle_empty = direct.begin_native_actions(1);
            require(empty); require(oracle_empty);
            require(staged.commit_native_actions(empty.value())); require(direct.commit_native_actions(oracle_empty.value()));
        }
        expect(!staged.native_invasion(completed) && staged.pending_native_invasion()
            && *staged.pending_native_invasion() == *direct.pending_native_invasion(),
            "later checkpoints preserve invasion tombstones and the pending transport inventory");
        expect(frozen->canonical_bytes() != staged.snapshot()->canonical_bytes(), "tail updates do not mutate the frozen player publication");
        require(barrier->commit()); barrier.reset();
        expect(staged.canonical_state_bytes() == direct.canonical_state_bytes() && staged.record() == direct.record()
            && staged.snapshot()->canonical_bytes() == direct.snapshot()->canonical_bytes(), "ordered control commit matches live state, publication and replay bytes");
        std::vector<std::string> hashes;
        for (unsigned tick = 0; tick < 12; ++tick) {
            auto actual = staged.step(executor); auto oracle = direct.step(executor); require(actual); require(oracle);
            expect(actual.value().state_sha256 == oracle.value().state_sha256, "native control continuation matches the live oracle");
            hashes.push_back(actual.value().state_sha256);
        }
        if (workers == 1) reference = hashes; else expect(reference == hashes, "native control barrier continuation is equal at 1/2/4/8 workers");
    }
}
void surface_roster_paths() {
    using Roster = std::vector<std::pair<eawr::sim::EntityId, gc::PlayerId>>;
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        const auto build = [&](const auto& edit) {
            auto s = setup();
            s.fleets[0].orbit = 2; s.fleets[0].members = {{20, 1}, {21, 2}, {31, 2}};
            s.planets[1].owner = 1;
            s.control->planets[1].ground_base = {};
            edit(s);
            gc::ObserverSetup o; o.local_player = 1; o.reveal_delay_seconds = f(1); o.automatic_reveal_range = f(1);
            o.players = {{1, false, false, 0, 0, {}}, {2, false, false, 0, 0, {}}, {3, false, false, 0, 0, {}}};
            for (std::size_t i = 0; i < s.planets.size(); ++i) {
                const auto& p = s.planets[i];
                o.planets.push_back({p.id, p.owner, p.owner, 0, 0, 0, 0, false, false, false, false, {}});
                o.planets.back().current_owner = o.planets.back().current_controller = p.owner;
                for (const auto& force : s.control->planets[i].landed)
                    o.planets.back().landed_transports.push_back({force.id, force.owner, false, false});
            }
            o.types = {{1, false, false, false}, {2, false, false, false}, {3, false, false, false}};
            for (const auto& player : s.players) for (const auto& p : s.planets) o.records.push_back({player.player_id, p.id, false, 0, 0, 0, {}});
            s.observer = std::move(o);
            auto created = gc::GalacticSession::create(s);
            if (!created) throw std::runtime_error(created.error().message);
            return std::move(created).value();
        };
        const auto roster = [](const auto& snapshot) {
            Roster ids;
            for (const auto& row : snapshot->observer()->planets) if (row.planet == 2)
                for (const auto& t : row.landed_transports) ids.emplace_back(t.id, t.owner);
            return ids;
        };
        const auto surface = [](const auto& snapshot) {
            Roster ids;
            for (const auto& force : snapshot->control()->planets[1].landed) ids.emplace_back(force.id, force.owner);
            return ids;
        };
        // The observer roster must equal the control surface list, in order, after every path.
        const auto check = [&](gc::GalacticSession& world, const Roster& want, const char* what) {
            const auto snapshot = world.snapshot();
            const bool same = roster(snapshot) == want && surface(snapshot) == want;
            if (!same) {
                std::cerr << "roster/surface for: " << what << " roster=";
                for (const auto& [id, owner] : roster(snapshot)) std::cerr << id << ':' << owner << ' ';
                std::cerr << "surface=";
                for (const auto& [id, owner] : surface(snapshot)) std::cerr << id << ':' << owner << ' ';
                std::cerr << '\n';
            }
            expect(same, what);
        };
        const auto fleet_of = [](gc::GalacticSession& world, eawr::sim::EntityId member) {
            for (const auto& fleet : world.snapshot()->fleets())
                for (const auto& row : fleet.members) if (row.id == member) return fleet.id;
            throw std::runtime_error("launched transport has no fleet");
        };
        const auto launched = [](const auto& receipt, const char* what) {
            if (!receipt.admitted) std::cerr << what << " reason=" << static_cast<int>(receipt.reason) << '\n';
            return receipt.admitted;
        };
        {   // WGS-187: the command retains a mixed allied prefix before appending.
            auto world = build([](auto& s) {
                s.control->planets[1].landed = {{50, 2, 2}, {51, 2, 1}, {52, 2, 3}};
                for (auto& relation : s.control->relations)
                    if (relation.player == 1 && relation.other == 2) relation.allied = true;
            });
            world.submit({{0, gc::SeatId{1}, 1}, gc::LandFleet{10, 2}}).value();
            (void)world.step(executor).value();
            check(world, {{50, 2}, {51, 1}, {52, 3}, {21, 1}, {31, 1}},
                "WGS-187 seat landing preserves mixed allied owners and appends in command order");
            hashes.push_back(world.state_sha256());
        }
        {   // WGS-187: TaskForce-style fleet landing visits members backwards.
            auto world = build([](auto& s) {
                s.control->planets[1].landed = {{50, 2, 2}, {51, 2, 1}, {52, 2, 3}};
                for (auto& relation : s.control->relations)
                    if (relation.player == 1 && relation.other == 2) relation.allied = true;
            });
            expect(world.land_native_transports(1, 10).value().admitted, "mixed-owner native fleet landing is admitted");
            check(world, {{50, 2}, {51, 1}, {52, 3}, {31, 1}, {21, 1}},
                "WGS-187 native fleet landing appends without replacing a different first owner");
            hashes.push_back(world.state_sha256());
        }
        {   // land_native_transports -> launch -> land_native_transport
            auto world = build([](auto& s) { s.control->planets[1].ground_base = {1, 1}; });
            check(world, {}, "no surface roster before native landing");
            expect(world.land_native_transports(1, 10).value().admitted, "native fleet landing is admitted");
            check(world, {{31, 1}, {21, 1}}, "native fleet landing publishes in landing order");
            expect(launched(world.launch_native_transport(1, 31, 10).value(), "launch back"), "launch back into the fleet is admitted");
            check(world, {{21, 1}}, "launch un-publishes the launched transport");
            hashes.push_back(world.state_sha256());
        }
        {   // direct fleetless landing needs the actor census; then launch back out
            auto world = build([](auto& s) {
                s.control->planets[1].ground_base = {1, 1};
                s.player_economy.emplace(); s.fiscal.emplace(); s.fiscal->credit_cap_per_planet = f(1000);
                for (const auto& player : s.players) {
                    s.player_economy->players.push_back({player.player_id, f(100), f(1000), 0, 5});
                    s.fiscal->players.push_back({player.player_id, true, f(1)});
                }
                for (const auto& planet : s.planets) s.fiscal->planets.push_back({planet.id, f(1)});
                enable_production(s);
                s.census = gc::PopulationCensus{{{1, f(1), 0}, {2, {}, 0}, {3, f(1), 0}},
                    {{1, 100, false, {}}, {2, 100, false, {}}, {3, 100, false, {}}},
                    {{10, 1, 1, 2, 2, false, false, 1, 0, gc::ActorCombatRole::fleet},
                     {11, 1, 2, 3, 3, false, false, 1, 0, gc::ActorCombatRole::fleet},
                     {20, 1, 1, 2, 10, false, false, 1, 0, gc::ActorCombatRole::ordinary},
                     {21, 2, 1, 2, 10, false, false, 2, 0, gc::ActorCombatRole::ordinary},
                     {22, 1, 2, 3, 11, false, false, 1, 0, gc::ActorCombatRole::ordinary},
                     {31, 2, 1, 2, 10, false, false, 2, 0, gc::ActorCombatRole::ordinary},
                     {50, 2, 2, 2, 2, false, false, 2, 0, gc::ActorCombatRole::transport},
                     {51, 2, 1, 2, 2, false, false, 2, 0, gc::ActorCombatRole::transport},
                     {52, 2, 3, 2, 2, false, false, 2, 0, gc::ActorCombatRole::transport}}};
                s.control->planets[1].landed = {{50, 2, 2}, {51, 2, 1}, {52, 2, 3}};
            });
            expect(world.admit_native_fleet(1, gc::RemoveFleetMembers{10, {31}}).value().admitted, "direct removal detaches the transport");
            const auto direct = world.land_native_transport(1, 31, 2).value();
            if (!direct.linked) std::cerr << "direct landing admitted=" << direct.event.admitted
                << " reason=" << static_cast<int>(direct.event.reason) << '\n';
            expect(direct.linked, "direct landing links the detached transport");
            check(world, {{50, 2}, {51, 1}, {52, 3}, {31, 1}}, "WGS-187 direct landing retains mixed owners and appends the detached transport");
            expect(launched(world.launch_native_transport(1, 31, 10).value(), "direct relaunch"), "relaunch is admitted");
            check(world, {{50, 2}, {51, 1}, {52, 3}}, "relaunch un-publishes only the directly landed transport");
            hashes.push_back(world.state_sha256());
        }
        {   // launch -> native fleet landing again: the relanded transport joins at the tail
            auto world = build([](auto& s) { s.control->planets[1].ground_base = {1, 1}; });
            expect(world.land_native_transports(1, 10).value().admitted, "native fleet landing is admitted");
            expect(launched(world.launch_native_transport(1, 31, 10).value(), "launch for reland"), "launch into the fleet is admitted");
            check(world, {{21, 1}}, "launch un-publishes before the reland");
            expect(world.land_native_transports(1, 10).value().admitted, "native fleet reland is admitted");
            check(world, {{21, 1}, {31, 1}}, "native fleet reland publishes at the tail");
            hashes.push_back(world.state_sha256());
        }
        {   // WGS-188: undefended invasion keeps the defender, then appends invaders.
            auto world = build([](auto& s) {
                s.planets[1].owner = 2; s.fleets[0].members = {{21, 2}, {31, 2}};
                s.control->planets[1].landed = {{50, 2, 2}, {51, 2, 1}, {52, 2, 3}};
            });
            check(world, {{50, 2}, {51, 1}, {52, 3}}, "three-owner surface roster starts published");
            world.enable_native_invasions(true);
            const auto receipt = world.admit_native_invasion(1, 10).value();
            expect(receipt.admitted && receipt.queued, "invasion of a defender-held surface queues");
            expect(bool(world.dispatch_native_invasion(receipt.request, 0)), "undefended completion dispatches");
            check(world, {{50, 2}, {21, 1}, {31, 1}}, "WGS-188 undefended invasion keeps defender ownership, removes other owners and appends invaders");
            hashes.push_back(world.state_sha256());
        }
        {   // land victory: non-invaders leave, survivors arrive
            auto world = build([](auto& s) { s.planets[1].owner = 2; s.control->planets[1].landed = {{50, 2, 2}, {51, 2, 2}}; });
            check(world, {{50, 2}, {51, 2}}, "defender surface roster starts published");
            gc::ControlBattleReturn won; won.planet = 2; won.invader = 1; won.defender = 2; won.winner = 1;
            won.land = true; won.surviving_invaders = {{21, 2, 1}, {31, 2, 1}};
            expect(bool(world.apply_control_battle(won)), "land victory return applies");
            check(world, {{21, 1}, {31, 1}}, "land victory un-publishes non-invaders and publishes the survivors");
            hashes.push_back(world.state_sha256());
        }
        {   // battle return: destroyed surface forces leave the roster
            auto world = build([](auto& s) { s.planets[1].owner = 2; s.control->planets[1].landed = {{50, 2, 2}, {51, 2, 2}}; });
            gc::ControlBattleReturn lost; lost.planet = 2; lost.invader = 1; lost.defender = 2; lost.winner = 2;
            lost.land = true; lost.destroyed = {50};
            expect(bool(world.apply_control_battle(lost)), "defender-win land return applies");
            check(world, {{51, 2}}, "battle return un-publishes destroyed surface forces");
            hashes.push_back(world.state_sha256());
        }
        {   // LandFleet -> launch -> LandFleet (the #2949 interaction, executed)
            auto world = build([](auto&) {});
            expect(bool(world.submit({{0, gc::SeatId{1}, 1}, gc::LandFleet{10, 2}})), "first landing submits");
            (void)world.step(executor).value();
            check(world, {{21, 1}, {31, 1}}, "first LandFleet publishes in member order");
            expect(world.launch_native_transport(1, 21).value().admitted, "launch from the surface is admitted");
            check(world, {{31, 1}}, "launch erases the roster row");
            const auto fleet = fleet_of(world, 21);
            expect(bool(world.submit({{world.completed_tick(), gc::SeatId{1}, 2}, gc::LandFleet{fleet, 2}})), "second landing submits");
            (void)world.step(executor).value();
            check(world, {{31, 1}, {21, 1}}, "land-launch-land leaves one row per transport, in surface order");
            hashes.push_back(world.state_sha256());
        }
        if (reference.empty()) reference = hashes; else expect(hashes == reference, "surface roster paths are equal on 1/2/4/8 workers");
    }
}
void landing_admission_and_result() {
    const eawr::sim::InlineExecutor executor;
    auto s = setup(); s.fleets[0].orbit = 2;
    auto world = gc::GalacticSession::create(s).value();
    expect(bool(world.submit({{0, gc::SeatId{2}, 1}, gc::LandFleet{10, 2}})), "unauthorized landing has valid shape");
    auto tick = world.step(executor).value();
    expect(tick.snapshot->events()[0].reason == gc::GalacticRejectReason::not_authorized && tick.snapshot->control()->planets[1].landed.empty(), "foreign seat cannot land another player's transport");
    auto repeated_type = s; repeated_type.fleets[1].owner = 1; repeated_type.fleets[1].members = {{22, 2}};
    repeated_type.control->planets[2].type = repeated_type.control->planets[1].type;
    auto captures = gc::GalacticSession::create(repeated_type).value(); std::size_t rewards = 0;
    captures.set_control_effects_handler([&](const auto effects, const auto) {
        for (const auto& effect : effects) if (effect.first_capture) ++rewards;
        return eawr::core::Result<void>::success();
    });
    expect(bool(captures.submit({{0, gc::SeatId{1}, 1}, gc::LandFleet{10, 2}}))
        && bool(captures.submit({{0, gc::SeatId{1}, 2}, gc::LandFleet{11, 3}})), "same-type conquests enter canonical input order");
    const auto captured = captures.step(executor).value();
    expect(rewards == 1 && captured.snapshot->control()->captured_types == std::vector<gc::CapturedPlanetType>{{1, 102}}
        && captured.snapshot->planets()[1].owner == 1 && captured.snapshot->planets()[2].owner == 1,
        "WGM-76 reward history is per owning player and original planet type, even for two simultaneous conquests");
    s.control->planets[1].ground_base = {2, 1}; s.planets[1].owner = 2;
    world = gc::GalacticSession::create(s).value();
    expect(bool(world.submit({{0, gc::SeatId{1}, 1}, gc::LandFleet{10, 2}})), "defended landing has valid shape");
    tick = world.step(executor).value();
    expect(tick.snapshot->events()[0].reason == gc::GalacticRejectReason::movement_blocked, "defended invasion stays at conflict-admission seam");
    gc::ControlBattleReturn result; result.planet = 2; result.invader = 1; result.defender = 2; result.winner = 1;
    result.land = true; result.surviving_invaders = {{21, 2, 1}};
    expect(bool(world.apply_control_battle(result)), "FT-08 land return supplies surviving transport identity");
    expect(world.snapshot()->planets()[1].owner == 2 && !world.snapshot()->control()->planets[1].ground_base.level,
        "WGM-58 battle return clears military base without direct political owner assignment");
    tick = world.step(executor).value();
    expect(tick.snapshot->planets()[1].owner == 1, "following live control service commits conquest from surviving surface presence");
    auto invalid = result; invalid.surviving_invaders = {{999, 2, 1}}; const auto before = world.canonical_state_bytes();
    expect(!world.apply_control_battle(invalid) && world.canonical_state_bytes() == before, "unknown survivor rejects without modifying control");
    invalid.surviving_invaders = {{20, 1, 1}};
    expect(!world.apply_control_battle(invalid) && world.canonical_state_bytes() == before, "space craft cannot become a landed transport on return");
    invalid = result; invalid.destroyed = {world.next_entity_id()};
    expect(!world.apply_control_battle(invalid) && world.canonical_state_bytes() == before,
        "unallocated future identity cannot enter the absent roster and suppress a later force");
}
void child_battle_control() {
    namespace campaign = eawr::campaign;
    namespace tactical = eawr::sim::tactical;
    auto s = setup(); s.fleets[0].orbit = 2; s.fleets[1].orbit = 2; s.fleets[1].members = {{22, 2}};
    s.planets[1].owner = 2; s.control->planets[1].starbase = {2, 1};
    campaign::ConflictRecord conflict;
    conflict.id = gc::GalacticSession::create(s).value().next_entity_id();
    conflict.layer = campaign::BattleLayer::space; conflict.planet = 2; conflict.invader = 1; conflict.defender = 2;
    conflict.forces = {{21, 2, 1, 10}, {22, 2, 2, 11}};
    conflict.seats = {{gc::SeatId{1}, 1}, {gc::SeatId{2}, 2}};
    conflict.tactical.content_identity = s.content_identity; conflict.tactical.first_free_entity_id = conflict.id + 20;
    conflict.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
    for (const auto& force : conflict.forces) {
        tactical::UnitState unit; unit.entity_id = force.id; unit.type_id = force.type; unit.owner = force.owner;
        conflict.tactical.units.push_back(unit);
    }
    tactical::TacticalContent content;
    tactical::DurabilityProfile profile; profile.type_id = 2; profile.max_hull = f(100); content.durability.profiles = {profile};
    content.victory.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    content.victory.contenders = {1, 2}; content.victory.controlled_players = {1, 2};
    content.victory.humans = {1}; content.victory.countdown_frames = 2; content.victory.relevant_types = {2};
    tactical::TacticalReplay damage; damage.setup.players = conflict.tactical.players; damage.final_tick_count = 1;
    damage.commands.push_back({{0, 1, 1}, {22}, tactical::DamagePayload{f(1000)}});
    const auto bytes = tactical::write_replay(damage).value();
    const auto header = static_cast<std::size_t>(bytes[10]) | (static_cast<std::size_t>(bytes[11]) << 8U);
    const auto start = header + damage.setup.players.size() * 24 + 4;
    gc::TacticalLayerCommand input{conflict.id, bytes[start + 20],
        {bytes.begin() + static_cast<std::ptrdiff_t>(start), bytes.end()}};
    cp::GameLoadOptions options; options.script_requirement = eawr::campaign::ScriptRequirement::none;
    options.content = [content](const auto&) { return eawr::core::Result<tactical::TacticalContent>::success(content); };
    const auto identities = cp::game_identities(s); const eawr::sim::InlineExecutor serial;
    auto game = campaign::GameSession::create(s).value();
    // FT-08 currently admits space children; land admission belongs to #1115.
    // The direct return contract above covers WGM-58's land-result adapter.
    game.stage_battle(conflict, content, eawr::campaign::ScriptRequirement::none).value();
    (void)game.step(serial).value();
    expect(bool(game.submit({{2, gc::SeatId{1}, 1}, input})), "space child damage enters through authenticated command envelope");
    const auto saved = cp::save_game_checkpoint(game, identities).value();
    std::vector<std::string> reference;
    bool returned = false, conquered = false; std::uint64_t landing_tick{};
    while (game.completed_tick() < 15) {
        const auto tick = game.step(serial).value(); reference.push_back(tick.state_hash.get());
        if (tick.result) {
            returned = true;
            expect(tick.result->winner == 1 && game.galaxy().snapshot()->planets()[1].owner == 2,
                "FT-08 child return preserves political ownership until the next galactic service");
            expect(!game.galaxy().snapshot()->control()->planets[1].starbase.level,
                "WGM-57 space child without a tactical station clears stored starbase");
            landing_tick = game.completed_tick();
            game.submit({{landing_tick, gc::SeatId{1}, 2}, gc::LandFleet{10, 2}}).value();
        } else if (returned && game.galaxy().snapshot()->planets()[1].owner == 1) conquered = true;
    }
    expect(returned && conquered, "won space child followed by uncontested landing leads to live-service conquest");
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        auto restored = cp::load_game_checkpoint(saved, identities, options, pool).value();
        for (const auto& hash : reference) {
            expect(restored.session.step(pool).value().state_hash.get() == hash,
                "checkpoint inside space child continues through political conquest equally on 1/2/4/8 workers");
            if (restored.session.completed_tick() == landing_tick)
                restored.session.submit({{landing_tick, gc::SeatId{1}, 2}, gc::LandFleet{10, 2}}).value();
        }
        const auto after = cp::save_game_checkpoint(restored.session, identities).value();
        auto again = cp::load_game_checkpoint(after, identities, options, pool).value();
        expect(cp::save_game_checkpoint(again.session, identities).value() == after,
            "post-return checkpoint retains surface identities, absent defenders and capture history");
    }
    std::vector<gc::GalacticEvent> return_feed;
    std::string return_hash;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        auto controlled = campaign::GameSession::create(s).value();
        controlled.stage_battle(conflict, content, eawr::campaign::ScriptRequirement::none).value();
        (void)controlled.step(pool).value();
        controlled.submit({{2, gc::SeatId{1}, 1}, input}).value();
        controlled.submit({{landing_tick - 1, gc::SeatId{1}, 2}, gc::PauseGalaxy{}}).value();
        bool observed_return = false;
        while (controlled.mode() == campaign::SessionMode::battle && controlled.completed_tick() < 15) {
            const auto tick = controlled.step(pool).value();
            if (!tick.result) continue;
            observed_return = true;
            expect(tick.completed_tick == landing_tick, "session pause does not move the tactical return boundary");
            const auto events = tick.galaxy->events();
            const gc::GalacticEvent executed{landing_tick - 1, gc::GalacticEventKind::command_executed,
                gc::SeatId{1}, 0, 2, 0, gc::GalacticRejectReason::none};
            expect(std::find(events.begin(), events.end(), executed) != events.end(),
                "control-enabled child return retains the return-tick session command event");
            expect(!tick.galaxy->control()->planets[1].starbase.level,
                "event preservation still applies the military control return");
            const std::vector<gc::GalacticEvent> feed(events.begin(), events.end());
            if (workers == 1) { return_feed = feed; return_hash = tick.state_hash.get(); }
            else expect(feed == return_feed && tick.state_hash.get() == return_hash,
                "control-enabled return feed and state agree on 1/2/4/8 workers");
        }
        expect(observed_return, "control-enabled command fixture reaches child return");
    }
}
void adapter() {
    auto presence = *setup().control;
    eawr::data::galactic::GalacticData data;
    data.constants = {{"Political_Control_Change_Time_Seconds", "", f(9)}, {"Max_Ground_Forces_On_Planet", "", f(12)}};
    for (const auto name : {"A", "B", "C"}) { eawr::data::galactic::Planet p; p.name = name; p.political_control = f(2); p.planet_capture_bonus_reward = 333; data.planets.push_back(p); }
    data.planets[1].planet_retains_residual_influence = false;
    data.planets[2].planet_retains_residual_influence = true;
    eawr::data::galactic::TradeRoute route; route.name = "Route"; route.political_control_gain = f(2); data.trade_routes = {route};
    eawr::units::GalacticTypeTable types; eawr::units::GalacticType type; type.name = "Ship"; type.political_control = f(1); types.types = {type};
    const std::vector<eawr::campaign::ControlTypeBinding> objects{{1, "Ship", false, false}};
    const std::vector<eawr::campaign::ControlObjectBinding> planets{{1, "A"}, {2, "B"}, {3, "C"}}, routes{{4, "Route"}};
    auto bound = eawr::campaign::bind_control_data(data, types, presence, objects, planets, routes);
    expect(bound && bound.value().change_seconds == f(9) && bound.value().max_ground_forces == 12 && bound.value().planets[1].capture_reward == f(333)
        && bound.value().routes[0].political_gain == f(2), "control rules and rewards follow authored values, never stock literals");
    expect(bound && bound.value().planets[0].retains_residual && !bound.value().planets[1].retains_residual
        && bound.value().planets[2].retains_residual, "residual influence uses sourced true default and explicit authored overrides");
    data.constants.clear(); expect(!eawr::campaign::bind_control_data(data, types, presence, objects, planets, routes), "omitted transition constants do not acquire guessed defaults");
}
void generate(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory); const auto r = replay();
    auto game = eawr::campaign::GameSession::from_replay(r).value();
    std::ofstream hashes(directory / "political-conquest.hashes.csv", std::ios::binary); hashes << "tick,sha256\n";
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t i = 0; i < r.final_tick_count; ++i) { const auto step = game.step(executor).value(); hashes << step.completed_tick << ',' << step.state_hash.get() << '\n'; }
    const auto bytes = gc::write_replay(game.record()).value();
    std::ofstream out(directory / "political-conquest.eawr-gcreplay", std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out || !hashes) throw std::runtime_error("control corpus write failed");
}
void installed() {
    std::optional<std::string> root;
#ifdef _WIN32
    char* value = nullptr; std::size_t size = 0;
    if (_dupenv_s(&value, &size, "EAWR_EAW_GAME_ROOT") == 0 && value) root = value;
    std::free(value);
#else
    if (const auto* value = std::getenv("EAWR_EAW_GAME_ROOT")) root = value;
#endif
    if (!root) { std::cout << "SKIPPED: stock control needs EAWR_EAW_GAME_ROOT\n"; return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}}) {
        auto mount = eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data");
        expect(bool(mount), "stock control manifest mount"); if (!mount) return;
        mounts.push_back(std::move(mount).value().mount);
    }
    const auto filesystem = eawr::vfs::Vfs::mount(mounts).value();
    const auto catalog = eawr::data::load_catalog(filesystem, eawr::data::Profile::foc).value();
    const auto data = eawr::data::galactic::load_galactic_data(filesystem, catalog.catalog).value();
    const auto types = eawr::units::galactic_type_table(catalog.catalog);
    gc::PoliticalControlSetup presence; std::vector<eawr::campaign::ControlTypeBinding> bindings;
    for (const auto& t : types.types) if (t.object_class == "SpaceUnit" && t.political_control)
        bindings.push_back({bindings.size() + 1, t.name, false, false});
    std::vector<eawr::campaign::ControlObjectBinding> planets, routes;
    for (const auto& p : data.planets) if (p.political_control && p.planet_capture_bonus_reward) {
        gc::ControlPlanet row; row.planet = planets.size() + 1; row.type = row.planet;
        presence.planets.push_back(row); planets.push_back({row.planet, p.name});
    }
    for (const auto& route : data.trade_routes) if (route.political_control_gain) {
        const auto id = planets.size() + routes.size() + 1;
        presence.routes.push_back({id, {}}); routes.push_back({id, route.name});
    }
    const auto bound = eawr::campaign::bind_control_data(data, types, presence, bindings, planets, routes);
    expect(bound && !bindings.empty() && !planets.empty() && !routes.empty(), "stock original-type political quantities bind without copied defaults");
    if (!bound) { std::cerr << eawr::core::format_diagnostic(bound.error()) << '\n'; return; }
    expect(bound.value().change_seconds == *data.constant("Political_Control_Change_Time_Seconds")->number, "stock control rate equals loader quantity");
    for (std::size_t i = 0; i < planets.size(); ++i)
        expect(bound.value().planets[i].leaning == *data.planet(planets[i].original_type)->political_control, "stock leaning follows original planet data");
    std::cout << "stock control types=" << bindings.size() << " planets=" << planets.size() << " routes=" << routes.size() << '\n';
}
} // namespace
int main(int argc, const char* const argv[]) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--generate") { generate(argv[2]); return 0; }
        if (argc == 2 && std::string_view(argv[1]) == "--installed") { installed(); return galactic_test::failures == 0 ? 0 : 1; }
        if (argc == 2 && std::string_view(argv[1]) == "--capture-build-list") {
            capture_build_and_ground_views(); return galactic_test::failures == 0 ? 0 : 1;
        }
        authored_mixed_surface_owners();
        native_action_barrier_control(); capture_build_and_ground_views(); surface_roster_paths(); landfleet_observer_roster(); captured_planet_observer_facts(); payload_ground_capability(); initial_control_provider(); control_and_outcome_extensions(); landed_census_checkpoint(); categories(); transitions(); replay_checkpoint_and_hooks(); political_fiscal_deadline(); station_production_presence(); first_capture_rewards(); capture_structure_cleanup(); tactical_built_persistence(); native_capture_notices(); landing_admission_and_result(); child_battle_control(); adapter();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures == 0 ? 0 : 1;
}
