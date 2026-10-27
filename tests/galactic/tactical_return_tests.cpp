#include "../support/result.hpp"
#include "../support/env.hpp"
#include "../support/temp_dir.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/tactical_return_live.hpp"
#include "eawr/campaign/auto_resolve_outcome.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/admission.hpp"
#include "eawr/sim/galactic/victory.hpp"
#include "../../src/campaign/battle_return_internal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {
namespace gc = eawr::sim::galactic;
namespace tactical = eawr::sim::tactical;
namespace campaign = eawr::campaign;
using galactic_test::expect;
using eawr::sim::math::Fixed;
Fixed f(const std::int64_t n) { return Fixed::from_integer(n).value(); }
template<class T> T checked(eawr::core::Result<T> result) {
    return eawr::testing::take(std::move(result));
}
void okay(eawr::core::Result<void> result) {
    eawr::testing::must(std::move(result));
}

gc::GalacticSetup galaxy() {
    gc::GalacticSetup s; s.seed = 71;
    s.players = {{1, 1, 0}, {2, 2, 0}};
    s.seats = {{{1}, 1, gc::SeatKind::human, 0, true, 1}, {{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    s.planets = {{1, {}, 2}, {2, {f(12), {}, {}}, 1}};
    s.routes = {{4, 1, 2, f(1)}};
    s.fleets = {{10, 1, 1, {{20, 1}, {21, 3}, {22, 1}, {24, 2}, {25, 3}}},
        {11, 1, 2, {{30, 1}}}, {12, 2, 1, {{40, 1}}}};
    s.rules.maximum_fleet_movement_distance = f(12);
    s.fleet_control.emplace(); s.fleet_control->types = {{1, false, true, false}, {2, false, true, true}, {3, false, false, false}};
    s.movement.emplace(); auto& m = *s.movement; m.mode_modifier = f(1);
    m.types = {{1, f(2), f(100), 0, true, false, false, false}, {2, {}, f(50), 0, false, true, false, false},
        {3, {}, f(10), 0, false, false, false, false}};
    m.players = {{1, f(1), f(3), false}, {2, f(1), f(3), false}};
    m.planets = {{1, f(1), false}, {2, f(1), false}};
    m.contents = {{24, {{23, 3}}}};
    for (const auto& p : s.players) for (const auto& q : s.planets) m.access.push_back({p.player_id, q.id, true, false});
    return s;
}
campaign::ConflictRecord conflict(const gc::GalacticSetup& setup) {
    campaign::ConflictRecord c; auto world = checked(gc::GalacticSession::create(setup));
    c.id = world.next_entity_id(); c.planet = 1; c.invader = 1; c.defender = 2;
    c.forces = {{20, 1, 1, 10}, {21, 3, 1, 10}, {22, 1, 1, 10, campaign::ForceRole::reinforcement},
        {23, 3, 1, 10, campaign::ForceRole::rider}, {24, 2, 1, 10, campaign::ForceRole::retained},
        {25, 3, 1, 10}, {40, 1, 2, 12}};
    c.forces[1].hero = true; c.forces[3].carrier = 20;
    c.station = c.id + 1; c.station_level = 1;
    c.forces.push_back({c.station, 3, 1, 0, campaign::ForceRole::escort, true});
    c.seats = {{{1}, 1}, {{2}, 2}};
    c.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
    c.tactical.first_free_entity_id = c.id + 2;
    for (const auto& row : c.forces) {
        if (row.role == campaign::ForceRole::retained || row.role == campaign::ForceRole::rider || row.role == campaign::ForceRole::reinforcement) continue;
        tactical::UnitState unit; unit.entity_id = row.id; unit.type_id = row.type; unit.owner = row.owner;
        c.tactical.units.push_back(unit);
    }
    tactical::CarriedObject cargo; cargo.entity_id = 23; cargo.type_id = 3;
    expect(tactical::contain_object(c.tactical.units.front(), cargo, false), "cargo attaches to persistent flagship");
    return c;
}
tactical::TacticalContent content(const campaign::ConflictRecord&) {
    tactical::TacticalContent c;
    c.motion.rules.arc_degrees = f(30); c.motion.rules.expansion_distance = f(10);
    c.motion.profiles.push_back({1, f(1), f(1), f(1), f(1), f(1)});
    for (const auto type : {1ULL, 3ULL}) { tactical::DurabilityProfile p; p.type_id = type; p.max_hull = f(100); c.durability.profiles.push_back(p); }
    tactical::AbilityProfile ability; ability.kind = tactical::AbilityKind::turbo;
    c.abilities.profiles.push_back({1, {ability}});
    c.victory.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    c.victory.relevant_types = {1, 3};
    c.victory.starbase_types = {3}; c.victory.contenders = {1, 2}; c.victory.controlled_players = {1, 2};
    tactical::RetreatConfig retreat; retreat.rules.campaign = true; retreat.rules.players = {{1, 2, true}, {2, 2, true}};
    retreat.actors = {{20, 1, true, true, true, true}, {40, 2, true, true, true, true}};
    retreat.actors[0].persistent = true; retreat.actors[1].persistent = true;
    retreat.prevention = {{40, 0, {1}}}; c.retreat = std::move(retreat);
    return c;
}
eawr::core::Result<tactical::TacticalContent> resolve_content(const campaign::ConflictRecord& c) {
    return eawr::core::Result<tactical::TacticalContent>::success(content(c));
}
gc::TacticalLayerCommand envelope(const campaign::ConflictRecord& c, tactical::PlayerCommand command) {
    tactical::TacticalReplay replay; replay.setup.players = c.tactical.players;
    replay.final_tick_count = 1; replay.commands.push_back(std::move(command));
    const auto bytes = checked(tactical::write_replay(replay));
    const auto header = static_cast<std::size_t>(bytes[10]) | (static_cast<std::size_t>(bytes[11]) << 8U);
    const auto start = header + replay.setup.players.size() * 24 + 4;
    return {c.id, bytes[start + 20], {bytes.begin() + static_cast<std::ptrdiff_t>(start), bytes.end()}};
}
void hero(campaign::GameSession& game) {
    game.set_hero_defeat_resolver([](const auto& c, const auto& force, const auto& result, const auto&) {
        gc::HeroDefeat d; d.id = force.id; d.type = d.respawn_type = force.type; d.owner = force.owner;
        d.killer = result.final_blow_player; d.planet = c.planet; d.named = true; d.default_seconds = f(1);
        return eawr::core::Result<std::optional<gc::HeroDefeat>>::success(d);
    });
}
void retreat_destination_ties() {
    for (const bool surface : {false, true}) for (unsigned tier = 0; tier < 4; ++tier) {
        std::optional<std::string> reference;
        for (const auto workers : eawr::platform::determinism_worker_counts()) for (const bool reverse : {false, true}) {
            auto setup = galaxy(); setup.players.push_back({3, 3, 0});
            setup.planets = {{1, {}, 2}, {2, {f(-3), f(4), {}}, tier == 2 ? 3U : 2U},
                {3, {{}, {}, f(-5)}, tier == 2 ? 3U : 2U}};
            setup.movement->planets.push_back({3, f(1), false});
            for (const auto& player : setup.players) for (const auto& planet : setup.planets)
                if (player.player_id == 3 || planet.id == 3) setup.movement->access.push_back({player.player_id, planet.id, true, false});
            setup.movement->players.push_back({3, f(1), f(3), false});
            std::erase_if(setup.fleets, [](const auto& fleet) { return fleet.id == 11; });
            if (tier == (surface ? 1U : 0U)) {
                setup.fleets.push_back({11, 1, 2, {{30, 1}}});
                setup.fleets.push_back({13, 1, 3, {{31, 1}}});
            }
            std::sort(setup.fleets.begin(), setup.fleets.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
            std::sort(setup.movement->access.begin(), setup.movement->access.end(), [](const auto& a, const auto& b) {
                return std::pair{a.player, a.planet} < std::pair{b.player, b.planet};
            });
            setup.control.emplace(); auto& control = *setup.control;
            control.change_seconds = f(1); control.max_ground_forces = 10;
            control.routes = {{4, f(1)}};
            control.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
            for (const auto& a : control.players) for (const auto& b : control.players)
                control.relations.push_back({a.player, b.player, a.player == b.player});
            control.types = {{1, 0, false, false}, {2, 0, false, true}, {3, 0, false, false}};
            for (const auto& planet : setup.planets) {
                gc::ControlPlanet row; row.planet = planet.id; row.type = 100 + planet.id; row.surface_accessible = true;
                if (planet.id != 1 && tier == (surface ? 0U : 1U)) row.ground_base = {1, 1};
                control.planets.push_back(row);
            }
            // Source qualifies too, at zero distance, and must still be excluded.
            control.planets[0].ground_base = {1, 1};
            if (surface) {
                setup.movement->types[1].hyperspace_speed = f(2);
                setup.movement->types[1].hyperspace_eligible = true;
                setup.movement->types[1].squadron = false;
                setup.movement->types[1].transport = true;
                setup.movement->contents.push_back({50, {}});
                control.planets[0].landed = {{50, 2, 1}};
            }
            auto world = checked(gc::GalacticSession::create(setup));
            if (reverse) world.scramble_storage_for_testing();
            for (unsigned restore = 0; restore < 3; ++restore) {
                expect(checked(campaign::retreat_destination(*world.snapshot(), 1, 1, surface)) == 3,
                    "WGM-211/212 every tier keeps the last-created equal-distance planet, including after restore");
                okay(world.load_checkpoint_state(checked(world.save_checkpoint_state())));
            }
            eawr::platform::ThreadWorkerAdapter executor(workers);
            campaign::ConflictRecord conflict; conflict.planet = 1;
            conflict.layer = surface ? campaign::BattleLayer::land : campaign::BattleLayer::space;
            campaign::TacticalReturnPlan plan; plan.result.retreat_player = 1;
            if (!surface) plan.retreat_fleets = {10};
            campaign::TacticalReturnState state;
            okay(campaign::apply_tactical_return(world, conflict, plan, state, executor));
            const auto snapshot = world.snapshot(); const auto fleets = snapshot->fleets();
            const auto departed = std::find_if(fleets.begin(), fleets.end(), [&](const auto& fleet) {
                return surface ? std::any_of(fleet.members.begin(), fleet.members.end(), [](const auto& member) { return member.id == 50; }) : fleet.id == 10;
            });
            expect(departed != fleets.end() && departed->motion.transit && departed->motion.transit->path.back() == 3,
                "WGM-212 tactical return actually departs to the selected tie winner");
            if (reference) expect(*reference == world.state_sha256(), "retreat ties equal on 1/2/4/8 workers and scrambled storage");
            else reference = world.state_sha256();
        }
    }
    // Unequal squared distances that round to the same Fixed length must not tie.
    auto setup = galaxy(); setup.planets.push_back({3, {f(12), Fixed::from_raw(1), {}}, 1});
    setup.movement->planets.push_back({3, f(1), false});
    for (const auto& player : setup.players) setup.movement->access.push_back({player.player_id, 3, true, false});
    std::sort(setup.movement->access.begin(), setup.movement->access.end(), [](const auto& a, const auto& b) {
        return std::pair{a.player, a.planet} < std::pair{b.player, b.planet};
    });
    auto world = checked(gc::GalacticSession::create(setup));
    expect(checked(eawr::sim::math::length(setup.planets[1].position))
        == checked(eawr::sim::math::length(setup.planets[2].position)),
        "rounded tie fixture has identical Q24 lengths for (12,0,0) and (12,one-raw-quantum,0)");
    expect(eawr::sim::math::compare_squared_lengths(setup.planets[1].position, setup.planets[2].position) < 0,
        "squared raw distances are (12*scale)^2 and (12*scale)^2+1 respectively");
    expect(checked(campaign::retreat_destination(*world.snapshot(), 1, 2, false)) == 2,
        "WGM-211 a genuinely closer candidate replaces the newer rounded-length tie");
    expect(eawr::sim::math::compare_squared_lengths({f(3), f(4), {}}, {{}, {}, f(-5)}) == 0,
        "equal 3D magnitudes compare equal");
    const auto low = Fixed::from_raw(std::numeric_limits<std::int64_t>::min());
    const auto high = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    expect(eawr::sim::math::compare_squared_lengths({low, low, low}, {high, high, high}) > 0,
        "squared distance comparison retains full signed-raw range without overflow");
}
void pure_policy() {
    tactical::RetreatRules rules; rules.campaign = true; rules.players = {{1, 2, true}, {2, 2, true}};
    tactical::RetreatState state;
    std::vector<tactical::RetreatActor> actors = {{20, 1, true, true, true, true}, {21, 1, true, true, true, true}};
    for (auto& actor : actors) actor.persistent = true;
    expect(tactical::request_retreat(rules, state, 1, tactical::RetreatAction::start, 0, {true}, actors) == tactical::RetreatRefusal::prevention,
        "active prevention blocks a request");
    expect(tactical::request_retreat(rules, state, 1, tactical::RetreatAction::start, 0, {}, actors) == tactical::RetreatRefusal::none, "countdown starts");
    {
        // WGM-64/WGM-79 (GCMP-24): the refusal is the standalone tactical multiplayer test, not the galactic game's
        // multiplayer flag. The campaign never sets it, so a battle in a multiplayer GC retreats and respawns as in solo.
        auto standalone = rules; standalone.multiplayer_tactical = true;
        tactical::RetreatState fresh;
        expect(tactical::request_retreat(standalone, fresh, 1, tactical::RetreatAction::start, 0, {}, actors) == tactical::RetreatRefusal::wrong_mode,
            "standalone multiplayer tactical refuses retreat");
        expect(tactical::request_retreat(rules, fresh, 1, tactical::RetreatAction::start, 0, {}, actors) == tactical::RetreatRefusal::none,
            "campaign battle retreat is admitted whatever the galactic multiplayer flag");
        std::vector<gc::HeroReturnRecord> records; gc::HeroDefeat defeat; defeat.id = 21; defeat.type = defeat.respawn_type = 3;
        defeat.owner = 1; defeat.planet = 2; defeat.named = true; defeat.default_seconds = f(1);
        const auto board = galaxy().planets;
        defeat.multiplayer_tactical = true;
        expect(!checked(gc::schedule_hero_return(records, defeat, 5, board)).scheduled && records.empty(), "standalone multiplayer tactical refuses respawn");
        defeat.multiplayer_tactical = false;
        expect(checked(gc::schedule_hero_return(records, defeat, 5, board)).scheduled, "campaign battle respawn is admitted whatever the galactic multiplayer flag");
    }
    actors[1].engines_disabled = true;
    tactical::service_retreat(rules, state, 2, {}, actors);
    expect(state.actors == std::vector<eawr::sim::EntityId>{20}, "expiry prunes currently disabled engines");
    auto absent = state;
    expect(tactical::service_retreat_flight(absent, false, 0).completed, "WR-50 absent initialization completes at engine frame zero");
    auto stopped = state; stopped.playback_reported = true;
    expect(!tactical::service_retreat_flight(stopped, true, 0).completed,
        "frame-zero shortcut cannot apply after playback has ever existed");
    expect(!tactical::service_retreat_flight(stopped, true, 89).completed,
        "stopped playback cannot bypass the frame-90 flight guard");
    expect(!tactical::service_retreat_flight(stopped, true, 90).completed,
        "enabled stopped playback cannot shorten the engine duration");
    expect(tactical::service_retreat_flight(stopped, true, 150).completed, "enabled escape completes on engine duration");
    state.playback_reported = true;
    state.cinematic_duration = 15 * tactical::logical_frames_per_second;
    expect(!tactical::service_retreat_flight(state, true, 89).completed, "cinematic 89 is incomplete");
    expect(!tactical::service_retreat_flight(state, true, 90).completed && state.station_wait, "90 enters station wait");
    const auto attempt = tactical::service_retreat_flight(state, true, 181);
    expect(attempt.destroy_stations && !attempt.completed, "station attempt invocation remains incomplete");
    expect(!tactical::service_retreat_flight(state, true, 181).destroy_stations, "station attempt is once only");
    state.playback_playing = false;
    expect(!tactical::service_retreat_flight(state, true, 181).completed, "playback stop cannot shorten a station cinematic");
    expect(tactical::service_retreat_flight(state, true, 450).completed, "station cinematic ends at engine duration");
    expect(checked(tactical::decode_retreat_state(tactical::encode_retreat_state(state))) == state, "retreat state codec roundtrip");
    std::uint64_t draws{};
    const auto attrition = checked(tactical::retreat_attrition(actors, 1, {}, {}, [&]() { ++draws; return 0U; }));
    expect(draws == 2 && attrition.draws == 2 && attrition.destroyed.empty(), "zero attrition still consumes every eligible draw");
    const auto dead = checked(tactical::retreat_attrition(actors, 1, f(1), {}, []() { return 0U; }));
    expect(dead.destroyed == std::vector<eawr::sim::EntityId>({21, 20}), "attrition preserves reverse source order");
    std::vector<gc::HeroReturnRecord> heroes; gc::HeroDefeat d; d.id = 21; d.type = d.respawn_type = 3; d.owner = 1; d.planet = 2; d.named = true; d.default_seconds = f(1);
    const auto planets = galaxy().planets;
    expect(checked(gc::schedule_hero_return(heroes, d, 5, planets)).scheduled
        && heroes.front().start_frame == 5 && heroes.front().deadline == 35, "hero schedule retains actual start and rounded logical deadline");
    const auto encoded = gc::encode_hero_returns(heroes);
    expect(encoded.size() == 64 && checked(gc::decode_hero_returns(encoded)) == heroes,
        "version two hero schedule roundtrip retains nonzero start");
    auto malformed = encoded; malformed[0] = 1;
    expect(!gc::decode_hero_returns(malformed), "old hero schedule version is rejected");
    malformed = encoded; malformed[56] = 36;
    expect(!gc::decode_hero_returns(malformed), "schedule start after deadline is rejected");
    malformed = encoded; malformed.pop_back();
    expect(!gc::decode_hero_returns(malformed), "truncated schedule is rejected");
    auto foreign = heroes.front(); foreign.owner = 2; foreign.defeated = 22;
    const std::array records{heroes.front(), foreign};
    expect(gc::local_hero_returns(records, 1) == std::vector<gc::HeroReturnView>{{3, 1, 2, 5, 35}}
        && gc::local_hero_returns(records, 0).empty(), "local schedule view excludes foreign and absent observers");
    campaign::TacticalReturnState pending; pending.heroes = heroes;
    auto owner = checked(campaign::GameSession::create(galaxy()));
    okay(owner.restore_return_state(campaign::encode_tactical_return_state(pending)));
    expect(owner.return_state().heroes == heroes, "restoring an absent hero preserves its actual scheduling interval");
    expect(!checked(gc::schedule_hero_return(heroes, d, 6, planets)).scheduled, "duplicate named hero does not reschedule");
    okay(gc::service_hero_returns(heroes, 35, true, planets, {})); expect(heroes.size() == 1, "paused hero tail does not consume due records");
    okay(gc::service_hero_returns(heroes, 35, false, planets, [](const auto&, const auto) { return eawr::core::Result<bool>::success(true); }));
    expect(heroes.empty(), "qualified placement consumes the due record once");
}
void hero_fallback() {
    const gc::HeroReturnRecord record{21, 3, 0, 1, 2, 10, 1};
    // First owned is farthest; tie candidates are deliberately in reverse ID order.
    std::vector<gc::Planet> planets{{10, {}, 2}, {1, {f(20), {}, {}}, 1},
        {3, {f(2), {}, {}}, 1}, {2, {f(-2), {}, {}}, 1}};
    std::vector<gc::HeroReturnRecord> rows{record};
    std::vector<gc::PlanetId> attempts;
    const gc::HeroPlacement reject = [&](const auto& row, const auto planet) {
        expect(row.owner == 1 && row.killer == 2, "placement retains recorded owner and attribution");
        attempts.push_back(planet); return eawr::core::Result<bool>::success(false);
    };
    okay(gc::service_hero_returns(rows, 0, false, planets, reject));
    okay(gc::service_hero_returns(rows, 1, true, planets, reject));
    expect(attempts.empty() && rows.size() == 1, "deadline and pause gate the candidate scan");
    okay(gc::service_hero_returns(rows, 1, false, planets, reject));
    expect(attempts == std::vector<gc::PlanetId>({2, 3, 1}) && rows == std::vector{record},
        "all owned candidates use proximity, lowest-ID ties, and all-failure retains deadline");
    attempts.clear();
    const gc::HeroPlacement accept_second = [&](const auto&, const auto planet) {
        attempts.push_back(planet); return eawr::core::Result<bool>::success(planet == 3);
    };
    okay(gc::service_hero_returns(rows, 2, false, planets, accept_second));
    expect(attempts == std::vector<gc::PlanetId>({2, 3}) && rows.empty(), "nearest refusal tries the next candidate");
    rows = {record}; planets.front().owner = 1; attempts.clear();
    okay(gc::service_hero_returns(rows, 1, false, planets, [&](const auto&, const auto planet) {
        attempts.push_back(planet); return eawr::core::Result<bool>::success(true);
    }));
    expect(attempts == std::vector<gc::PlanetId>({10}) && rows.empty(), "valid recorded planet is nearest");
    for (auto& planet : planets) planet.owner = 2;
    rows = {record}; attempts.clear();
    okay(gc::service_hero_returns(rows, 1, false, planets, reject));
    expect(rows.empty() && attempts.empty(), "no owned candidate removes the record without placement");

    planets[1].owner = planets[2].owner = planets[3].owner = 1;
    auto structured = record; structured.structure = 7;
    gc::HeroStructureState census{2, 1, {1, 3}};
    const gc::HeroStructureResolver structures = [&](const auto& row) {
        expect(row.structure == 7 && row.type == 3 && row.owner == 1, "census resolves recorded owner and selected types");
        return eawr::core::Result<gc::HeroStructureState>::success(census);
    };
    rows = {structured};
    okay(gc::service_hero_returns(rows, 1, false, planets, reject));
    expect(rows.size() == 1 && attempts.empty(), "missing structure census retains due record");
    okay(gc::service_hero_returns(rows, 1, false, planets, reject, structures));
    expect(attempts == std::vector<gc::PlanetId>({3}) && rows.size() == 1,
        "structure-backed refusal retains and never tries the farther site");
    census.owned_instances = 2; attempts.clear();
    okay(gc::service_hero_returns(rows, 2, false, planets, reject, structures));
    expect(rows.empty() && attempts.empty(), "live hero count revalidation cancels satisfied backing capacity");
    rows = {structured}; census.owned_instances = 1; census.planets = {10};
    okay(gc::service_hero_returns(rows, 2, false, planets, reject, structures));
    expect(rows.empty() && attempts.empty(), "no currently owned backing site cancels record");
    rows = {structured}; census.planets = {1, 3};
    okay(gc::service_hero_returns(rows, 2, false, planets, accept_second, structures));
    expect(rows.empty() && attempts == std::vector<gc::PlanetId>({3}), "nearest backing site success consumes once");

    std::vector<std::string> reference;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto setup = galaxy(); setup.planets = planets; setup.routes.clear(); setup.fleets.clear();
        std::sort(setup.planets.begin(), setup.planets.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        setup.movement.reset(); setup.fleet_control.reset();
        auto game = checked(campaign::GameSession::create(setup));
        campaign::TacticalReturnState pending; pending.heroes = {record}; pending.heroes.front().deadline = 0;
        okay(game.restore_return_state(campaign::encode_tactical_return_state(pending)));
        auto restored = checked(campaign::GameSession::create(setup));
        okay(restored.restore_return_state(checked(game.save_return_state())));
        std::vector<gc::PlanetId> live_attempts, restored_attempts;
        bool accept = false;
        const auto bind = [&](auto& owner, auto& trace) {
            owner.set_hero_placement([&trace, &accept](const auto&, const auto planet) {
                trace.push_back(planet); return eawr::core::Result<bool>::success(accept && planet == 3);
            });
        };
        bind(game, live_attempts); bind(restored, restored_attempts);
        std::vector<std::string> hashes;
        for (unsigned i = 0; i < 3; ++i) {
            accept = i != 0;
            game.scramble_storage_for_testing(); restored.scramble_storage_for_testing();
            const auto hash = checked(game.step(executor)).state_hash.get();
            hashes.push_back(hash);
            expect(checked(restored.step(executor)).state_hash.get() == hash
                && restored.return_state() == game.return_state(), "restored pending hero tail matches uninterrupted parent");
            if (i == 0) {
                expect(game.return_state().heroes == pending.heroes, "failed parent placement retains due record");
                okay(restored.restore_return_state(checked(game.save_return_state())));
            }
        }
        expect(live_attempts == std::vector<gc::PlanetId>({2, 3, 1, 2, 3})
            && restored_attempts == live_attempts && game.return_state().heroes.empty(), "restored fallback retries once and never duplicates success");
        if (reference.empty()) reference = hashes;
        else expect(reference == hashes, "hero fallback parent hashes match on 1/2/4/8 workers");
    }
}
void cinematic_authority() {
    std::vector<std::string> reference;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        // Human absent/stopped/stuck, default disabled AI, explicit disabled human, explicit enabled AI.
        for (const bool station : {false, true}) for (unsigned mode = 0; mode < 6; ++mode) {
            const bool ai = mode == 3 || mode == 5;
            const bool enabled = mode != 3 && mode != 4;
            const auto c = conflict(galaxy()); auto setup = c.tactical; auto binding = content(c);
            std::erase_if(setup.units, [&](const auto& unit) {
                return unit.entity_id != 20 && unit.entity_id != 40 && unit.entity_id != c.station;
            });
            if (mode == 4 || mode == 5) setup.cinematic_seats = {{1, enabled}};
            binding.retreat->prevention.clear();
            if (!station) binding.victory.starbase_types.clear();
            binding.economy.players = {{1, {}, 10, ai}, {2, {}, 10, true}};
            auto session = checked(tactical::TacticalSession::create(setup, binding));
            const auto resolved_setup = session.record().setup;
            expect(resolved_setup.cinematic_seats == std::vector<tactical::CinematicSeat>({{1, enabled}, {2, false}}),
                "trusted creation roles resolve AI false/human true and preserve explicit settings");
            std::uint64_t sequence{};
            const auto report = [&](const bool playing) {
                tactical::PlayerCommand command{{session.completed_tick(), 1, ++sequence}, {}, tactical::RetreatCinematicPayload{playing, 0}};
                if (!ai) {
                    const tactical::SeatBinding seat{{1}, 1, true, false};
                    command = checked(tactical::admit_human_command(seat,
                        {{session.completed_tick(), {1}, sequence}, 1, {}, command.payload})).command;
                }
                okay(session.submit(command));
            };
            okay(session.submit({{0, 1, ++sequence}, {}, tactical::RetreatPayload{tactical::RetreatAction::start}}));
            do { hashes.push_back(checked(session.step(executor)).state_hash.get()); }
            while (session.retreat_state().phase == tactical::RetreatPhase::countdown);
            expect(session.retreat_state().cinematic_frame == 0
                && session.retreat_state().cinematic_duration == (station ? 450U : 150U),
                "FC-R02 initializes the engine clock and station-dependent duration");
            expect(session.retreat_state().phase == (enabled ? tactical::RetreatPhase::escaping : tactical::RetreatPhase::escaped),
                "only the trusted disabled seat completes at initialization k0, without any report");
            if (!enabled) {
                // A's AI wire layout remains valid even though no producer is required for k0 completion.
                report(false);
                hashes.push_back(checked(session.step(executor)).state_hash.get());
                expect(session.retreat_state().cinematic_frame == 0 && !session.retreat_state().station_destroyed,
                    "an AI no-playback report cannot start another clock or station destruction");
                const auto log = session.record();
                const auto parsed = checked(tactical::parse_replay(checked(tactical::write_replay(log))));
                expect(parsed.setup.cinematic_seats == resolved_setup.cinematic_seats, "disabled seat setup replay roundtrip");
                auto restored = checked(tactical::TacticalSession::from_replay(parsed, binding));
                okay(restored.load_checkpoint_state(checked(session.save_checkpoint_state())));
                expect(restored.record().setup.cinematic_seats == resolved_setup.cinematic_seats
                    && restored.state_sha256() == session.state_sha256(), "disabled seat checkpoint roundtrip");
                continue;
            }
            for (const auto frame : {1U, 90U, 181U}) {
                const tactical::RetreatCinematicPayload forged{true, frame};
                expect(!tactical::human_wire_payload(forged), "human wire refuses every nonzero reserved frame");
                expect(!session.submit({{session.completed_tick(), 1, sequence + 1}, {}, forged}),
                    "native intake refuses a client cinematic clock jump");
                const tactical::SeatBinding seat{{1}, 1, true, false};
                expect(!tactical::admit_human_command(seat,
                    {{session.completed_tick(), {1}, sequence + 1}, 1, {}, forged}), "LAN human seat refuses a forged clock");
            }
            tactical::TacticalReplay forged_log; forged_log.setup = resolved_setup; forged_log.final_tick_count = 1;
            forged_log.commands = {{{0, 1, 1}, {}, tactical::RetreatCinematicPayload{true, 181}}};
            expect(!tactical::write_replay(forged_log), "replay encoding refuses a nonzero reserved clock");
            forged_log.commands.front().payload = tactical::RetreatCinematicPayload{true, 0};
            auto forged_bytes = checked(tactical::write_replay(forged_log));
            forged_bytes[forged_bytes.size() - 12] = 181;
            const auto parsed_forgery = tactical::parse_replay(forged_bytes);
            expect(!parsed_forgery && parsed_forgery.error().message.find("frame is reserved") != std::string::npos,
                "replay parsing refuses a nonzero reserved clock");
            report(false);
            hashes.push_back(checked(session.step(executor)).state_hash.get());
            expect(session.retreat_state().phase == tactical::RetreatPhase::escaping
                && session.retreat_state().cinematic_frame == 1 && !session.retreat_state().playback_reported,
                "enabled human false/zero during initial escaping cannot complete in that tick");
            if (mode != 0) {
                report(true); report(true); report(true);
                hashes.push_back(checked(session.step(executor)).state_hash.get());
                expect(session.retreat_state().cinematic_frame == 2 && session.retreat_state().playback_reported
                    && !session.retreat_state().station_destroyed, "running reports cannot advance time or jump to 181");
            }
            while (session.retreat_state().cinematic_frame < 90)
                hashes.push_back(checked(session.step(executor)).state_hash.get());
            if (mode == 1) report(false); // stopped after k90 must still wait for start + duration.
            if (mode == 0) report(false); // playback was never reported: engine timer remains the fallback.
            hashes.push_back(checked(session.step(executor)).state_hash.get());
            expect(session.retreat_state().phase == tactical::RetreatPhase::escaping
                && session.retreat_state().cinematic_frame == 91, "enabled false/zero after k90 cannot complete early");
            const auto saved = checked(session.save_checkpoint_state());
            const auto saved_log = checked(tactical::parse_replay(checked(tactical::write_replay(session.record()))));
            expect(saved_log.setup.cinematic_seats == resolved_setup.cinematic_seats, "enabled seat setup replay roundtrip");
            const auto saved_tick = session.completed_tick();
            std::vector<std::string> continuation;
            std::size_t station_destructions{};
            while (session.retreat_state().phase == tactical::RetreatPhase::escaping && session.completed_tick() < 500) {
                session.scramble_storage_for_testing();
                const auto stepped = checked(session.step(executor));
                const auto hash = stepped.state_hash.get();
                hashes.push_back(hash); continuation.push_back(hash);
                const auto& state = session.retreat_state();
                expect(state.cinematic_duration == (station ? 450U : 150U),
                    "FSC-R03 duration remains fixed after countdown-to-escaping initialization");
                const auto station_events = std::count_if(stepped.snapshot->events().begin(), stepped.snapshot->events().end(),
                    [&](const auto& event) { return event.kind == tactical::EventKind::unit_destroyed && event.unit == c.station; });
                station_destructions += static_cast<std::size_t>(station_events);
                if (state.cinematic_frame < state.cinematic_duration)
                    expect(state.phase == tactical::RetreatPhase::escaping, "enabled absence/stop/stuck playback cannot shorten the timer");
                if (station && state.cinematic_frame == 180) expect(!state.station_destroyed, "station destruction waits for engine frame 181");
                if (station && state.cinematic_frame == 181) expect(state.station_destroyed && state.phase == tactical::RetreatPhase::escaping
                    && station_events == 1 && std::none_of(stepped.snapshot->instances().begin(), stepped.snapshot->instances().end(),
                        [&](const auto& instance) { return instance.entity_id == c.station; }),
                    "engine 181 station destruction stays incomplete regardless of playback reports");
                if (station && state.cinematic_frame == 182) expect(state.phase == tactical::RetreatPhase::escaping
                    && state.cinematic_duration == 450 && station_events == 0,
                    "FSC-R03 destroyed station at k181 cannot shorten the escape or be destroyed again at k182");
            }
            expect(session.retreat_state().phase == tactical::RetreatPhase::escaped
                && session.retreat_state().cinematic_frame == (station ? 450U : 150U),
                "enabled cinematic completes exactly at start plus duration");
            if (station) expect(station_destructions == 1, "FSC-R03 station destruction is emitted exactly once through k450");
            auto restored = checked(tactical::TacticalSession::from_replay(saved_log, binding));
            okay(restored.load_checkpoint_state(saved));
            expect(restored.record().setup.cinematic_seats == resolved_setup.cinematic_seats,
                "checkpoint retains enabled seat setting");
            const auto full_log = session.record();
            for (const auto& command : full_log.commands) if (command.key.tick >= saved_tick) okay(restored.submit(command));
            for (const auto& hash : continuation) {
                restored.scramble_storage_for_testing();
                expect(checked(restored.step(executor)).state_hash.get() == hash, "engine clock and seat setting resume deterministically");
            }
        }
        if (reference.empty()) reference = hashes;
        else expect(hashes == reference, "seat authority and no-playback completion hashes match at 1/2/4/8");
    }
}
// RP-09 / FS-R06: the station's authored retreat explosion is a projectile created at the station boundary
// (k181), once, by the station itself; a type without it creates none.
void station_explosion() {
    std::vector<std::string> reference;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (const bool authored : {false, true}) {
            const auto c = conflict(galaxy()); auto setup = c.tactical; auto binding = content(c);
            std::erase_if(setup.units, [&](const auto& unit) {
                return unit.entity_id != 20 && unit.entity_id != 40 && unit.entity_id != c.station;
            });
            setup.cinematic_seats = {{1, true}};
            if (authored) {
                tactical::CombatProfile profile; profile.type_id = 3;
                tactical::ShotProfile shot; shot.speed = f(1); shot.blast.damage = f(1200); shot.blast.radius = f(1200);
                shot.blast.dropoff = true; profile.retreat_explosion = shot;
                binding.combat.profiles.push_back(profile);
            }
            binding.economy.players = {{1, {}, 10, true}, {2, {}, 10, true}};
            auto session = checked(tactical::TacticalSession::create(setup, binding));
            std::uint64_t sequence{};
            okay(session.submit({{0, 1, ++sequence}, {}, tactical::RetreatPayload{tactical::RetreatAction::start}}));
            std::set<std::uint64_t> created;
            for (unsigned guard = 0; guard < 700 && session.retreat_state().phase != tactical::RetreatPhase::escaped; ++guard) {
                const auto stepped = checked(session.step(executor));
                hashes.push_back(stepped.state_hash.get());
                for (const auto& projectile : stepped.snapshot->projectiles()) {
                    if (projectile.weapon != tactical::retreat_explosion_slot_flag || !created.insert(projectile.id).second) continue;
                    expect(projectile.shooter == c.station && projectile.owner == 1 && session.retreat_state().station_destroyed
                        && session.retreat_state().cinematic_frame == 181, "RP-09 the station creates its explosion at k181");
                }
            }
            expect(created.size() == (authored ? 1U : 0U), "RP-09 the explosion is created once, and only for a type that authors one");
        }
        if (reference.empty()) reference = hashes;
        else expect(hashes == reference, "RP-09 explosion hashes match at 1/2/4/8 workers");
    }
}
void parent_return(const bool nonadjacent = false) {
    auto setup = galaxy();
    if (nonadjacent) {
        setup.routes.clear();
        setup.rules.maximum_fleet_movement_distance = f(1);
        auto ordinary = checked(gc::GalacticSession::create(setup));
        const auto move = checked(ordinary.admit_native_fleet(1, gc::MoveFleet{10, {1, 2}, {}}));
        expect(!move.admitted && move.reason == gc::GalacticRejectReason::invalid_path,
            "ordinary movement still rejects nonadjacent endpoints");
        const std::array<gc::FleetId, 1> retreating{10};
        okay(ordinary.apply_retreat_return(1, 1, 1, false, retreating));
        const auto fleets = ordinary.snapshot()->fleets();
        const auto fleet = std::find_if(fleets.begin(), fleets.end(), [](const auto& row) { return row.id == 10; });
        expect(fleet != fleets.end() && fleet->orbit == 1 && !fleet->motion.transit,
            "WGM-99 source/source retreat keeps the fleet in orbit");
    }
    gc::VictoryContext context;
    context.players = {{1, "First", false, true, true, false}, {2, "Second", false, true, false, true}};
    context.players[0].leader_company = context.players[0].leader_unit_type = 3;
    context.players[0].leader_unit = "Hero";
    context.planets = {{1, 101, 2, 2, 2}, {2, 102, 1, 1, 1}};
    for (const auto& p : context.players) for (const auto& q : context.players)
        context.relations.push_back({p.player, q.player, p.player == q.player});
    gc::VictoryAuthoring authored; authored.human = authored.ai = {gc::VictoryKind::kill_leader};
    setup.victory = gc::install_victory(authored, context, 1);
    const auto c = conflict(setup);
    std::vector<std::string> reference;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = checked(campaign::GameSession::create(setup)); hero(game); okay(game.stage_battle(c, content(c), eawr::campaign::ScriptRequirement::none));
        std::vector<std::string> hashes; hashes.push_back(checked(game.step(executor)).state_hash.get());
        std::uint64_t seq1{}, seq2{};
        const auto issue = [&](const std::uint32_t player, tactical::CommandPayload payload, std::vector<eawr::sim::EntityId> units = {}) {
            auto& sequence = player == 1 ? seq1 : seq2;
            okay(game.submit({{game.completed_tick(), {player}, ++sequence}, envelope(c, {{0, player, 1}, std::move(units), std::move(payload)})}));
        };
        std::optional<campaign::BattleResult> result;
        std::optional<std::vector<std::uint8_t>> child_save;
        std::optional<tactical::TacticalReplay> child_log;
        std::uint64_t saved_child_tick{};
        for (unsigned tick = 1; tick <= 461; ++tick) {
            if (tick == 1) { issue(2, tactical::AbilityPayload{tactical::AbilityKind::turbo, tactical::AbilityAction::activate}, {40}); issue(2, tactical::DamagePayload{f(1000)}, {21}); }
            if (tick == 2 || tick == 4 || tick == 6) issue(1, tactical::RetreatPayload{tactical::RetreatAction::start});
            if (tick == 3) { issue(1, tactical::RetreatPayload{tactical::RetreatAction::auto_resolve}); issue(2, tactical::AbilityPayload{tactical::AbilityKind::turbo, tactical::AbilityAction::deactivate}, {40}); }
            if (tick == 5) issue(1, tactical::RetreatPayload{tactical::RetreatAction::cancel});
            if (tick == 9) issue(1, tactical::RetreatCinematicPayload{true, 0});
            if (tick == 190) issue(1, tactical::RetreatCinematicPayload{false, 0});
            game.scramble_storage_for_testing(); const auto stepped = checked(game.step(executor)); hashes.push_back(stepped.state_hash.get());
            if (tick == 1) expect(game.battle()->retreat_views().front().refusal == tactical::RetreatRefusal::prevention, "live active source contributes prevention");
            if (tick == 2) expect(game.battle()->retreat_state().phase == tactical::RetreatPhase::idle, "prevented native request is refused");
            if (tick == 3) {
                expect(!game.battle()->retreat_views().front().auto_resolve_allowed && std::any_of(stepped.diagnostics.begin(), stepped.diagnostics.end(),
                    [](const auto& row) { return row.message.find("interleaved") != std::string::npos; }), "mid-battle solve refuses unavailable native death service");
                const auto live = game.battle()->units();
                const auto gathered = checked(campaign::collect_midbattle_objects(c, live, [&]() {
                    return eawr::core::Result<std::vector<campaign::ConflictForce>>::success({c.forces[2]});
                }));
                expect(std::none_of(gathered.begin(), gathered.end(), [](const auto& row) { return row.unit.entity_id == 21; })
                    && std::any_of(gathered.begin(), gathered.end(), [](const auto& row) { return row.unit.entity_id == 22 && row.reserve; }),
                    "mid-battle inputs omit the real casualty and retain the current unspent reserve");
            }
            if (tick == 5) expect(game.battle()->retreat_state().phase == tactical::RetreatPhase::idle, "native cancellation releases the roster");
            if (tick == 7) { child_save = checked(game.battle()->save_checkpoint_state()); child_log = game.battle_record(); saved_child_tick = game.battle()->completed_tick(); }
            if (stepped.result) {
                expect(!result, "return publishes once"); result = stepped.result;
                if (nonadjacent) std::cout << "nonadjacent retreat workers=" << workers
                    << " request_tick=6 completed_tick=" << game.completed_tick()
                    << " retreat_player=" << result->retreat_player.value_or(0) << '\n';
            }
        }
        expect(result && result->winner == 2 && result->retreat_player == 1U, "retreat produces enemy victory and actual retreat result");
        for (const auto id : {20ULL, 22ULL, 23ULL, 24ULL}) {
            const auto survivor = std::find_if(result->forces.begin(), result->forces.end(), [&](const auto& row) { return row.id == id; });
            expect(survivor != result->forces.end() && survivor->disposition == campaign::ForceDisposition::retreated,
                "deployed carrier, unspent reserve, rider and retained transport survive the return");
        }
        expect(game.return_state().heroes.size() == 1 && game.return_state().heroes.front().killer == 2, "real casualty schedules an attributed hero record");
        expect(game.galaxy().victory()->pending && game.galaxy().victory()->pending->player == 2
            && game.galaxy().victory()->pending->kind == gc::VictoryKind::kill_leader,
            "WGCH-76 live tactical hero return supplies the exact company/final-blow winner to the parent monitor");
        expect(game.mode() == campaign::SessionMode::galaxy && game.galaxy().clock().pause_depth == 0, "return releases the child and owned pause");
        const auto snapshot = game.galaxy().snapshot();
        const auto departing = std::find_if(snapshot->fleets().begin(), snapshot->fleets().end(), [](const auto& row) { return row.id == 10; });
        expect(departing != snapshot->fleets().end() && departing->motion.transit, "retreat uses ordinary movement rather than teleport");
        if (nonadjacent) {
            const bool in_transit = departing != snapshot->fleets().end() && departing->motion.transit.has_value();
            std::cout << "nonadjacent retreat destination=2 transit=" << in_transit << '\n';
            expect(in_transit && departing->orbit == 0
                && departing->motion.transit->path == std::vector<gc::PlanetId>({1, 2}),
                "WGM-198 retreat uses the selected direct hop beyond ordinary graph range");
            expect(std::none_of(snapshot->events().begin(), snapshot->events().end(), [](const auto& event) {
                return event.subject == 10 && event.kind == gc::GalacticEventKind::movement_canceled;
            }), "selected retreat departure does not emit movement-canceled");
        }
        expect(game.latest_tactical_result() && game.latest_tactical_context(), "latest summary retains its original type context");
        const auto returned = checked(game.save_return_state());
        expect(checked(campaign::decode_tactical_return_state(returned)) == game.return_state(), "authoritative return codec roundtrip");
        auto replayed = checked(campaign::GameSession::from_recording(game.recording(), resolve_content, {}, eawr::campaign::ScriptRequirement::none)); hero(replayed);
        std::vector<std::string> replay_hashes;
        while (replayed.completed_tick() < game.completed_tick()) { replayed.scramble_storage_for_testing(); replay_hashes.push_back(checked(replayed.step(executor)).state_hash.get()); }
        expect(replay_hashes == hashes, "full parent recording reproduces the return and hero scheduling");
        const auto log = *game.battle_record();
        auto restored = checked(tactical::TacticalSession::from_replay(*child_log, content(c))); okay(restored.load_checkpoint_state(*child_save));
        for (const auto& command : log.commands) if (command.key.tick >= saved_child_tick) okay(restored.submit(command));
        auto continuous = checked(tactical::TacticalSession::from_replay(log, content(c)));
        while (continuous.completed_tick() < saved_child_tick) checked(continuous.step(executor));
        while (restored.completed_tick() < log.final_tick_count) {
            restored.scramble_storage_for_testing(); expect(checked(restored.step(executor)).state_hash.get() == checked(continuous.step(executor)).state_hash.get(), "countdown checkpoint continuation matches");
        }
        expect(child_log && !game.battle(), "completed child recording remains available");
        if (reference.empty()) reference = hashes; else expect(hashes == reference, "1/2/4/8 scrambled parent hashes match");
    }
}
void selected_recording() {
    const auto setup = galaxy(); auto c = conflict(setup); auto unresolved = c; unresolved.tactical.units.clear();
    auto game = checked(campaign::GameSession::create(setup));
    game.set_conflict_service([](const auto&, const auto&, auto) { return eawr::core::Result<std::optional<campaign::RecordedAdmission>>::success({}); }, resolve_content, {}, eawr::campaign::ScriptRequirement::none);
    unsigned resolutions{};
    game.set_entry_resolver([&](const auto&, const auto&, auto) { ++resolutions; return eawr::core::Result<campaign::ResolvedConflictEntry>::success({c, content(c)}); });
    okay(game.queue_conflict(unresolved, {campaign::ConflictRoute::pending_tactical}));
    eawr::sim::InlineExecutor executor; std::vector<std::string> hashes;
    hashes.push_back(checked(game.step(executor)).state_hash.get());
    hashes.push_back(checked(game.step(executor)).state_hash.get());
    okay(game.submit({{game.completed_tick(), {1}, 1}, gc::BattleChoice{c.id, true}}));
    hashes.push_back(checked(game.step(executor)).state_hash.get());
    expect(game.battle() && resolutions == 1, "selected battle resolves placement once");
    const auto child_frame = game.battle()->completed_tick();
    okay(game.submit({{game.completed_tick(), {1}, 2}, gc::SaveRequest{0}}));
    hashes.push_back(checked(game.step(executor)).state_hash.get());
    expect(game.battle()->completed_tick() == child_frame, "recorded save-only barrier preserves child frame");
    auto replay = checked(campaign::GameSession::from_recording(game.recording(), [&](const auto& record) {
        expect(!record.tactical.units.empty(), "recording reuses the selected resolved record");
        return eawr::core::Result<tactical::TacticalContent>::success(content(record));
    }, {}, eawr::campaign::ScriptRequirement::none));
    std::vector<std::string> again;
    while (replay.completed_tick() < game.completed_tick()) again.push_back(checked(replay.step(executor)).state_hash.get());
    expect(again == hashes, "admission/choice/save barrier parent replay hashes agree");
}
void squadron_return(const campaign::StockTacticalReturnServices* stock = nullptr,
    const gc::TypeId squadron_type = 3, const gc::TypeId ship_type = 1) {
    auto setup = galaxy();
    setup.fleets = {{10, 1, 1, {{20, ship_type}, {21, squadron_type}}}, {12, 2, 1, {{40, ship_type}}}};
    setup.fleet_control->types = {{ship_type, false, true, false}, {squadron_type, false, true, false}, {4, false, true, false}};
    std::sort(setup.fleet_control->types.begin(), setup.fleet_control->types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    setup.movement->types = {{ship_type, f(2), f(100), 0, true, false, false, false},
        {squadron_type, f(2), f(10), 0, true, true, false, false}, {4, f(2), {}, 0, true, false, false, false}};
    std::sort(setup.movement->types.begin(), setup.movement->types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    setup.movement->contents = {{21, {{50, 4}, {51, 4}}}};
    campaign::ConflictRecord c; c.id = checked(gc::GalacticSession::create(setup)).next_entity_id();
    c.planet = 1; c.invader = 1; c.defender = 2; c.seats = {{{1}, 1}, {{2}, 2}};
    c.forces = {{20, ship_type, 1, 10}, {21, squadron_type, 1, 10}, {40, ship_type, 2, 12}};
    c.forces[0].tactical_type = c.forces[2].tactical_type = 1; c.forces[1].tactical_type = 3;
    c.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
    c.tactical.first_free_entity_id = 103;
    c.tactical.cinematic_seats = {{1, false}, {2, false}};
    for (const auto& [id, type, owner] : {std::tuple{20ULL, 1ULL, 1U}, {21ULL, 3ULL, 1U},
        {40ULL, 1ULL, 2U}, {50ULL, 4ULL, 1U}, {51ULL, 4ULL, 1U},
        {100ULL, 3ULL, 1U}, {101ULL, 4ULL, 1U}, {102ULL, 4ULL, 1U}}) {
        tactical::UnitState unit; unit.entity_id = id; unit.type_id = type; unit.owner = owner;
        c.tactical.units.push_back(unit);
    }
    c.tactical.squadrons = {{21, {50, 51}}, {100, {101, 102}}};
    auto binding = content(c); binding.retreat->prevention.clear();
    binding.retreat->actors.push_back({21, 1, true, true, true, true});
    binding.retreat->actors.back().persistent = true;
    tactical::CraftProfile craft; craft.type_id = 4; craft.max_speed = f(10); craft.min_speed = f(1);
    craft.rate_of_turn = craft.lift = craft.thrust = craft.roll_rate = f(1);
    binding.motion.squadrons.craft = {craft};
    tactical::SquadronProfile team; team.type_id = 3; team.members = {4, 4}; team.offsets = {{}, {}};
    binding.motion.squadrons.squadrons = {team};
    std::erase_if(binding.durability.profiles, [](const auto& p) { return p.type_id == 3; });
    tactical::DurabilityProfile health; health.type_id = 4; health.max_hull = f(100); binding.durability.profiles.push_back(health);
    if (stock) {
        okay(stock->bind(c, binding));
        const auto actor = std::find_if(binding.retreat->actors.begin(), binding.retreat->actors.end(), [](const auto& a) { return a.id == 21; });
        expect(actor != binding.retreat->actors.end() && actor->hyperspace,
            "WR-85 installed X-wing metadata reaches the live retreat binder");
    }
    binding.retreat->rules.lockout_frames = 0;
    for (auto& player : binding.retreat->rules.players) player.countdown_frames = 2;
    binding.retreat->attrition_factor = {};
    std::vector<std::string> reference;
    for (const auto count : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(count);
        auto trapped_binding = binding;
        for (auto& actor : trapped_binding.retreat->actors) if (actor.id == 21) actor.hyperspace = false;
        auto trapped = checked(tactical::TacticalSession::create(c.tactical, trapped_binding));
        okay(trapped.submit({{0, 1, 1}, {}, tactical::RetreatPayload{tactical::RetreatAction::start}}));
        while (!trapped.outcome() && trapped.completed_tick() < 10) checked(trapped.step(executor));
        const auto trapped_live = trapped.units();
        expect(std::none_of(trapped_live.begin(), trapped_live.end(), [](const auto& u) {
            return u.entity_id == 21 || u.entity_id == 50 || u.entity_id == 51;
        }) && std::any_of(trapped_live.begin(), trapped_live.end(), [](const auto& u) { return u.entity_id == 20; }),
            "a nonescaping squadron loses its craft while the same fleet's eligible frigate escapes");
        auto session = checked(tactical::TacticalSession::create(c.tactical, binding));
        okay(session.submit({{0, 1, 1}, {}, tactical::RetreatPayload{tactical::RetreatAction::start}}));
        while (!session.outcome() && session.completed_tick() < 10) checked(session.step(executor));
        const auto live = session.units();
        expect(session.retreat_state().phase == tactical::RetreatPhase::escaped
            && std::count_if(live.begin(), live.end(), [](const auto& u) { return u.entity_id == 50 || u.entity_id == 51; }) == 2,
            "WR-85 completion preserves every live craft of an escaped bought squadron");
        expect(std::none_of(live.begin(), live.end(), [](const auto& u) { return u.entity_id >= 100; }),
            "temporary carrier fighters do not become persistent retreat survivors");
        auto game = checked(campaign::GameSession::create(setup)); okay(game.stage_battle(c, binding, eawr::campaign::ScriptRequirement::none));
        checked(game.step(executor));
        okay(game.submit({{game.completed_tick(), {1}, 1}, envelope(c, {{0, 1, 1}, {}, tactical::RetreatPayload{tactical::RetreatAction::start}})}));
        std::optional<campaign::BattleResult> returned;
        std::vector<std::string> hashes;
        for (unsigned step = 0; game.mode() == campaign::SessionMode::battle && step < 10; ++step) {
            const auto tick = checked(game.step(executor)); hashes.push_back(tick.state_hash.get());
            if (tick.result) returned = tick.result;
        }
        expect(returned && returned->forces[1].id == 21 && returned->forces[1].disposition == campaign::ForceDisposition::retreated,
            "bought squadron retains its galactic identity through tactical return");
        for (unsigned step = 0; step < 400; ++step) {
            const auto snap = game.galaxy().snapshot();
            const auto fleet = std::find_if(snap->fleets().begin(), snap->fleets().end(), [](const auto& row) { return row.id == 10; });
            if (fleet == snap->fleets().end() || !fleet->motion.transit) break;
            checked(game.step(executor));
        }
        const auto snap = game.galaxy().snapshot();
        const auto fleet = std::find_if(snap->fleets().begin(), snap->fleets().end(), [](const auto& row) { return row.id == 10; });
        expect(fleet != snap->fleets().end() && fleet->orbit == 2 && !fleet->motion.transit
            && std::any_of(fleet->members.begin(), fleet->members.end(), [](const auto& m) { return m.id == 21; }),
            "frigate and bought squadron arrive together with the original squadron id");
        const auto contents = std::find_if(snap->movement()->contents.begin(), snap->movement()->contents.end(), [](const auto& row) { return row.container == 21; });
        expect(contents != snap->movement()->contents.end() && contents->children == setup.movement->contents.front().children,
            "retreat preserves the bought squadron craft roster");
        if (reference.empty()) reference = hashes; else expect(hashes == reference, "squadron retreat is equal on 1/2/4/8 workers");
    }
}
void deployed_reserve() {
    auto c = conflict(galaxy()); c.tactical.first_free_entity_id = 51;
    c.tactical.campaign.emplace();
    auto& campaign_setup = *c.tactical.campaign;
    campaign_setup.players = {{1, {}, 20}, {2, {}, 20}};
    for (auto& player : campaign_setup.players) player.credit_cap = f(100);
    tactical::CarriedObject passenger; passenger.entity_id = 50; passenger.type_id = 3;
    passenger.parent = 22; passenger.named_hero = true;
    campaign_setup.reserves = {{22, 1, 1, false, 0, {passenger}}};
    auto binding = content(c); binding.economy.campaign = true;
    binding.economy.players = campaign_setup.players;
    binding.economy.campaign_population = {{1, 1}, {3, 0}};
    binding.retreat->prevention.clear(); binding.retreat->attrition_factor = f(1);
    tactical::RetreatActor actor{22, 1, true, true, true, true}; actor.persistent = true;
    binding.retreat->actors.push_back(actor);
    eawr::sim::InlineExecutor executor;
    auto controlled = galaxy(); controlled.seat_control.emplace();
    controlled.seat_control->teams = {{1, {1}, false}};
    controlled.seat_control->controllers = {{1, gc::FactionControl::human, gc::FactionControl::ai, {}, {}}, {2, gc::FactionControl::ai, gc::FactionControl::ai, {}, {}}};
    auto game = checked(campaign::GameSession::create(controlled)); okay(game.stage_battle(c, binding, eawr::campaign::ScriptRequirement::none));
    std::vector<std::string> parent_hashes{checked(game.step(executor)).state_hash.get()};
    expect(!game.submit({{game.completed_tick(), {1}, 1}, envelope(c, {{0, 1, 1}, {},
        tactical::ReinforcePayload{1, {f(-500), {}, {}}, 21}})}), "human campaign envelope refuses an unknown reserve token");
    okay(game.submit({{game.completed_tick(), {1}, 1}, envelope(c, {{0, 1, 1}, {},
        tactical::ReinforcePayload{1, {f(-500), {}, {}}, 22}})}));
    // Queue while the manifest/pool still authorize this token; the earlier order consumes it first.
    okay(game.submit({{game.completed_tick() + 1, {1}, 2}, envelope(c, {{0, 1, 1}, {},
        tactical::ReinforcePayload{1, {f(-500), {}, {}}, 22}})}));
    parent_hashes.push_back(checked(game.step(executor)).state_hash.get());
    const auto parent_deployed = game.battle()->units();
    expect(std::any_of(parent_deployed.begin(), parent_deployed.end(), [](const auto& unit) { return unit.entity_id == 22; }),
        "human campaign envelope admits and executes a trusted available reserve");
    const auto credits_after_deployment = game.battle()->ledgers().front().credits;
    const auto reserve_refused = checked(game.step(executor)); parent_hashes.push_back(reserve_refused.state_hash.get());
    expect(std::any_of(reserve_refused.diagnostics.begin(), reserve_refused.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.message.find("human battle seat cannot issue engine-only orders") != std::string::npos;
    }), "queued human reserve order is rechecked against the consumed pool token at execution");
    const auto after_queued_order = game.battle()->units();
    expect(std::count_if(after_queued_order.begin(), after_queued_order.end(), [](const auto& unit) { return unit.entity_id == 22; }) == 1
        && game.battle()->ledgers().front().credits == credits_after_deployment,
        "refused queued reserve order neither duplicates the persistent actor nor spends credits");
    expect(!game.submit({{game.completed_tick(), {1}, 3}, envelope(c, {{0, 1, 1}, {},
        tactical::ReinforcePayload{1, {f(-500), {}, {}}, 22}})}), "human campaign envelope refuses a consumed reserve token");
    auto replayed = checked(campaign::GameSession::from_recording(game.recording(), [&](const auto&) {
        return eawr::core::Result<tactical::TacticalContent>::success(binding);
    }, {}, eawr::campaign::ScriptRequirement::none));
    for (const auto& hash : parent_hashes) expect(checked(replayed.step(executor)).state_hash.get() == hash,
        "trusted reserve admission reproduces from the parent recording");
    auto session = checked(tactical::TacticalSession::create(c.tactical, binding));
    okay(session.submit({{0, 1, 1}, {}, tactical::ReinforcePayload{1, {f(-500), {}, {}}, 22}}));
    checked(session.step(executor));
    const auto deployed = session.units();
    expect(std::any_of(deployed.begin(), deployed.end(), [](const auto& unit) { return unit.entity_id == 22; }),
        "campaign reserve deploys with its persistent root identity");
    okay(session.submit({{1, 1, 2}, {}, tactical::RetreatPayload{tactical::RetreatAction::start}}));
    checked(session.step(executor)); checked(session.step(executor)); checked(session.step(executor));
    expect(session.retreat_state().phase == tactical::RetreatPhase::escaping
        && std::find(session.retreat_state().actors.begin(), session.retreat_state().actors.end(), 22ULL) != session.retreat_state().actors.end(),
        "a reserve deployed after setup joins the retreat flight");
    okay(session.submit({{session.completed_tick(), 1, 3}, {}, tactical::RetreatCinematicPayload{false, 0}}));
    while (session.retreat_state().phase == tactical::RetreatPhase::escaping && session.completed_tick() < 500) checked(session.step(executor));
    expect(session.retreat_state().phase == tactical::RetreatPhase::escaped, "AI-compatible no-playback report completes on engine time");
    const auto survivors = session.units();
    expect(std::any_of(survivors.begin(), survivors.end(), [](const auto& unit) { return unit.entity_id == 22; }),
        "the deployed reserve escapes and its current named passenger protects it from attrition");
}
void land_return_keeps_orbit() {
    for (const bool automatic : {false, true}) for (const bool raid : {false, true})
        for (const bool no_survivors : {false, true}) {
        auto setup = galaxy();
        setup.fleets = {{10, 1, 1, {{20, 1}}}, {12, 2, 1, {{40, 1}}}};
        setup.fleet_control->types[1] = {2, true, true, false};
        setup.players.push_back({3, 3, 0});
        setup.movement->players.push_back({3, f(1), f(1), false});
        for (const auto& planet : setup.planets) setup.movement->access.push_back({3, planet.id, true, false});
        setup.movement->contents = {{30, {}}, {31, {}}, {32, {}}};
        setup.movement->types[1].hyperspace_speed = f(2);
        setup.movement->types[1].hyperspace_eligible = true;
        setup.movement->types[1].squadron = false;
        setup.movement->types[1].transport = true;
        setup.control.emplace(); auto& control = *setup.control;
        control.change_seconds = f(7); control.max_ground_forces = 10;
        control.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
        for (const auto& a : control.players) for (const auto& b : control.players)
            control.relations.push_back({a.player, b.player, a.player == b.player});
        control.types = {{1, 0, false, false}, {2, 0, false, true}};
        for (const auto& planet : setup.planets) {
            gc::ControlPlanet row; row.planet = planet.id; row.type = 100 + planet.id; row.surface_accessible = true;
            row.ground_base = {planet.owner, 1}; control.planets.push_back(row);
        }
        control.routes = {{4, f(1)}};
        control.planets[0].landed = {{30, 2, 1}, {31, 2, 1}, {32, 2, 2}};
        campaign::ConflictRecord c; c.id = 90; c.planet = 1; c.invader = 1; c.defender = 2;
        c.layer = campaign::BattleLayer::land; c.raid = raid;
        c.forces = {{30, 2, 1, 1}, {31, 2, 1, 1}};
        campaign::BattleResult result; result.conflict = c.id; result.winner = 2; result.retreat_player = 1;
        result.forces = {{30, campaign::ForceDisposition::retreated}, {31, campaign::ForceDisposition::destroyed, 2}};
        if (raid) result.retreat_player.reset(); // WAR-57 also applies without a tactical escape flag.
        if (no_survivors) result.forces.front() = {30, campaign::ForceDisposition::destroyed, 2};
        campaign::AutoResolveRoster roster; roster.planet.id = 1; roster.planet.landed = {30, 31, 32};
        roster.orbit_fleets = {10, 12};
        for (const auto& fleet : setup.fleets) roster.fleets.push_back({fleet, {}, true, false});
        for (const auto& force : c.forces) {
            campaign::AutoResolveObject row; row.id = force.id; row.owner = force.owner;
            row.original_type = row.current_type = "ModTransport"; row.kind = campaign::AutoResolveObjectKind::transport;
            roster.objects.push_back(row);
        }
        campaign::AutoResolveRoundOutcome outcome; outcome.winner = 2; outcome.retreating_player = 1;
        if (raid) outcome.retreating_player = 0;
        campaign::AutoResolveAllocation death; death.id = 31; death.owner = 1; death.killed = true; death.final_blow_player = 2;
        outcome.allocations.push_back(death);
        if (no_survivors) { death.id = 30; outcome.allocations.push_back(death); }
        campaign::AutoResolveOutcomeInputs inputs; inputs.planet_type = "ModWorld";
        inputs.lethal_closure = [](const auto& loss) { return eawr::core::Result<std::vector<eawr::sim::EntityId>>::success({loss.id}); };
        inputs.post_delivery_record = [](const auto&) { return eawr::core::Result<std::vector<campaign::AutoResolveRecordEntry>>::success({}); };
        std::vector<std::string> reference;
        for (const auto count : eawr::platform::determinism_worker_counts()) {
            eawr::platform::ThreadWorkerAdapter executor(count);
            auto world = checked(gc::GalacticSession::create(setup));
            if (automatic) {
                const auto plan = checked(campaign::plan_auto_resolve_outcome(c, roster, outcome, inputs));
                expect(plan.retreat_fleets == (raid ? std::vector<gc::FleetId>{10} : std::vector<gc::FleetId>{}),
                    "WGM-201 auto-resolve includes orbit fleets only for WAR-57 raid loss");
                campaign::AutoResolveOutcomeServices services;
                services.control = [](auto& galaxy, const auto& a) {
                    gc::ControlBattleReturn returned; returned.planet = a.planet; returned.invader = a.invader;
                    returned.defender = a.defender; returned.winner = a.result.winner; returned.land = a.land; returned.destroyed = a.destroyed;
                    return galaxy.apply_control_battle(returned);
                };
                services.retreat = [](auto& galaxy, const auto& a) {
                    return galaxy.apply_retreat_return(*a.result.retreat_player, a.planet, 2, a.land, a.retreat_fleets, a.destroyed);
                };
                okay(campaign::apply_auto_resolve_outcome(world, plan, services));
            } else {
                const auto plan = checked(campaign::plan_tactical_return(c, result, *world.snapshot()));
                expect(plan.retreat_fleets == (raid ? std::vector<gc::FleetId>{10} : std::vector<gc::FleetId>{}),
                    "WGM-201 tactical land return includes orbit fleets only for raid loss");
                // The return API requires its caller's control result first,
                // as GameSession does before committing ordinary losses/retreat.
                gc::ControlBattleReturn controlled; controlled.planet = c.planet;
                controlled.invader = c.invader; controlled.defender = c.defender;
                controlled.winner = plan.result.winner; controlled.land = true; controlled.destroyed = plan.destroyed;
                okay(world.apply_control_battle(controlled));
                campaign::TacticalReturnState state;
                okay(campaign::apply_tactical_return(world, c, plan, state, executor));
            }
            const auto snapshot = world.snapshot();
            const auto orbit = std::find_if(snapshot->fleets().begin(), snapshot->fleets().end(), [](const auto& f) { return f.id == 10; });
            expect(orbit != snapshot->fleets().end() && (raid ? orbit->motion.transit.has_value() : orbit->orbit == 1 && !orbit->motion.transit),
                "ordinary land loss keeps the space fleet in orbit; raid loss leaves orbit");
            const auto surface = std::find_if(snapshot->fleets().begin(), snapshot->fleets().end(), [](const auto& f) {
                return std::any_of(f.members.begin(), f.members.end(), [](const auto& m) { return m.id == 30; });
            });
            if (no_survivors) expect(surface == snapshot->fleets().end(), "zero landed survivors create no departing surface fleet");
            else expect(surface != snapshot->fleets().end() && surface->id != 10 && surface->motion.transit
                    && surface->members == std::vector<gc::FleetMember>{{30, 2}},
                    "only living landed transports form a new departing fleet");
            expect(snapshot->control()->planets.front().landed == std::vector<gc::SurfaceForce>{{32, 2, 2}},
                "surface return removes casualties and survivors while retaining the other owner");
            std::vector<std::string> hashes{world.state_sha256()};
            for (unsigned tick = 0; tick < 3; ++tick) hashes.push_back(checked(world.step(executor)).state_sha256);
            if (reference.empty()) reference = hashes; else expect(hashes == reference, "land returns agree on 1/2/4/8 workers");
        }
    }
}
void stock_faction_retreat() {
    namespace dg = eawr::data::galactic;
    struct XmlTree {
        std::filesystem::path root = std::filesystem::temp_directory_path() /
            ("eawr-retreat-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ~XmlTree() { eawr::testing::remove_temp_directory(root); }
    } tree;
    std::filesystem::create_directories(tree.root / "XML");
    const auto write = [&](const std::string& file, const std::string_view text) {
        std::ofstream out(tree.root / "XML" / file); out << text;
    };
    for (const auto* registry : {"GameObjectFiles", "HardpointDataFiles", "CampaignFiles", "SFXEventFiles"})
        write(std::string(registry) + ".xml", "<Files/>");
    write("FactionFiles.xml", "<Faction_Files><File>Factions.xml</File></Faction_Files>");
    // Two differently named factions exercise overrides, omission and request eligibility.
    const auto factions = [&](const std::string_view second_tags) {
        write("Factions.xml", std::string(R"xml(<Factions>
            <Faction Name="First"><Is_Playable>Yes</Is_Playable><Space_Retreat_Countdown_Seconds>1.125</Space_Retreat_Countdown_Seconds></Faction>
            <Faction Name="Second">)xml") + std::string(second_tags) + "</Faction></Factions>");
    };
    write("GameConstants.xml", R"xml(<GameConstants><Default_Hero_Respawn_Time>1</Default_Hero_Respawn_Time>
        <Space_Retreat_Attrition_Factor>0</Space_Retreat_Attrition_Factor>
        <Space_Retreat_Allowed_Countdown_Seconds>0</Space_Retreat_Allowed_Countdown_Seconds></GameConstants>)xml");
    const std::vector<eawr::vfs::MountSpec> mounts{{"synthetic", tree.root, "data", {}}};
    dg::GalacticData data;
    for (const auto* name : {"First", "Second", "Neutral"}) {
        dg::Faction faction; faction.name = name; faction.is_neutral = std::string_view(name) == "Neutral";
        data.factions.push_back(faction);
    }
    dg::Planet planet; planet.name = "Place"; planet.galactic_position = dg::Position{};
    planet.behaviors = {"PLANET"}; data.planets.push_back(planet);
    dg::Campaign authored; authored.name = "Synthetic"; authored.locations = {"Place"}; authored.planets = authored.locations;
    data.campaigns.push_back(authored);
    data.constants = {{"MaximumFleetMovementDistance", "10", f(10)}, {"Fiscal_Cycle_Time_In_Secs", "1", f(1)}};
    eawr::units::GalacticType ship; ship.name = "RenamedWell"; ship.object_class = "SpaceUnit"; ship.hyperspace_capable = true;
    auto other = ship; other.name = "RenamedOther";
    eawr::units::GalacticTypeTable types; types.types = {ship, other};
    campaign::CampaignSetupOptions options;
    options.players = {{{1, 1, 0}, "First", true}, {{2, 2, 0}, "Second", true}, {{3, 3, 0}, "Neutral", true}};
    options.types = {{1, "RenamedWell"}, {3, "RenamedOther"}};
    const auto plan = checked(campaign::plan_campaign_setup(data, types, authored.name, options));
    for (const auto second_tags : {"<Is_Playable>Yes</Is_Playable>", "<Is_Playable>No</Is_Playable>", ""}) {
        factions(second_tags);
        const auto files = checked(eawr::vfs::Vfs::mount(mounts));
        const bool playable = std::string_view(second_tags) == "<Is_Playable>Yes</Is_Playable>";
        const auto catalog = checked(eawr::data::load_catalog(files, eawr::data::Profile::foc));
        const auto services = checked(campaign::prepare_stock_tactical_return(files, catalog.catalog, plan));
        const auto c = conflict(galaxy());
        auto binding = content(c);
        okay(services.bind(c, binding));
        const auto& rules = binding.retreat->rules;
        expect(rules.players[0].countdown_frames == 33, "WR-86 XML override truncates fractional seconds times FPS");
        expect(rules.players[1].countdown_frames == 479, "WR-86 omitted countdown uses 15.99 seconds times 30 FPS, truncated");
        expect(rules.players[1].playable == playable, "WR-87 omitted or false playability remains retreat ineligible");
        std::vector<std::string> reference;
        for (const auto workers : eawr::platform::determinism_worker_counts()) {
            eawr::platform::ThreadWorkerAdapter executor(workers);
            auto session = checked(tactical::TacticalSession::create(c.tactical, binding));
            tactical::RetreatState request;
            expect(tactical::request_retreat(rules, request, 2, tactical::RetreatAction::start, 0, {}, binding.retreat->actors)
                == (playable ? tactical::RetreatRefusal::none : tactical::RetreatRefusal::faction),
                "WR-87 faction eligibility is checked at the retreat request");
            okay(session.submit({{0, 2, 1}, {}, tactical::RetreatPayload{tactical::RetreatAction::start}}));
            std::vector<std::string> hashes{checked(session.step(executor)).state_sha256};
            expect(session.completed_tick() == 1 && !session.outcome(),
                "WR-87 battle advances with a participant that omits retreat tags");
            expect(session.retreat_state().phase == (playable ? tactical::RetreatPhase::countdown : tactical::RetreatPhase::idle),
                "WR-86/87 defaulted playable countdown starts; non-playable request leaves battle idle");
            if (playable) expect(session.retreat_state().deadline == 479, "WR-86 native request uses the defaulted countdown");
            else {
                okay(session.submit({{1, 1, 1}, {}, tactical::RetreatPayload{tactical::RetreatAction::start}}));
                hashes.push_back(checked(session.step(executor)).state_sha256);
                expect(session.retreat_state().player == 1 && session.retreat_state().deadline == 34,
                    "WR-87 rejected faction does not block the playable opponent's authored countdown");
            }
            const auto log = checked(tactical::parse_replay(checked(tactical::write_replay(session.record()))));
            auto replayed = checked(tactical::TacticalSession::from_replay(log, binding));
            for (const auto& hash : hashes) expect(checked(replayed.step(executor)).state_sha256 == hash,
                "WR-86/87 parsed replay reproduces defaulted countdown and faction refusal");
            auto restored = checked(tactical::TacticalSession::from_replay(log, binding));
            okay(restored.load_checkpoint_state(checked(session.save_checkpoint_state())));
            expect(restored.state_sha256() == session.state_sha256(), "WR-86/87 checkpoint restores the current countdown");
            for (unsigned tick = 0; tick < 4; ++tick) {
                session.scramble_storage_for_testing();
                hashes.push_back(checked(session.step(executor)).state_sha256);
                expect(checked(restored.step(executor)).state_sha256 == hashes.back(), "WR-86/87 checkpoint continuation matches");
            }
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "WR-86/87 every tick agrees on 1/2/4/8 workers");
        }
    }
}
void stock_interdiction() {
    namespace dg = eawr::data::galactic;
    struct XmlTree {
        std::filesystem::path root = std::filesystem::temp_directory_path() /
            ("eawr-retreat-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ~XmlTree() { eawr::testing::remove_temp_directory(root); }
    } tree;
    std::filesystem::create_directories(tree.root / "XML");
    const auto write = [&](const std::string& file, const std::string_view text) {
        std::ofstream out(tree.root / "XML" / file); out << text;
    };
    for (const auto* registry : {"GameObjectFiles", "HardpointDataFiles", "CampaignFiles", "SFXEventFiles"})
        write(std::string(registry) + ".xml", "<Files/>");
    write("FactionFiles.xml", "<Faction_Files><File>Factions.xml</File></Faction_Files>");
    write("Factions.xml", R"xml(<Factions>
        <Faction Name="First"><Is_Playable>Yes</Is_Playable><Space_Retreat_Countdown_Seconds>1</Space_Retreat_Countdown_Seconds></Faction>
        <Faction Name="Second"><Is_Playable>Yes</Is_Playable><Space_Retreat_Countdown_Seconds>1</Space_Retreat_Countdown_Seconds></Faction>
        </Factions>)xml");
    write("GameConstants.xml", R"xml(<GameConstants><Default_Hero_Respawn_Time>1</Default_Hero_Respawn_Time>
        <Space_Retreat_Attrition_Factor>0</Space_Retreat_Attrition_Factor>
        <Space_Retreat_Allowed_Countdown_Seconds>0</Space_Retreat_Allowed_Countdown_Seconds></GameConstants>)xml");
    const std::vector<eawr::vfs::MountSpec> mounts{{"synthetic", tree.root, "data", {}}};
    const auto files = checked(eawr::vfs::Vfs::mount(mounts));
    const auto catalog = checked(eawr::data::load_catalog(files, eawr::data::Profile::foc));
    dg::GalacticData data;
    for (const auto* name : {"First", "Second", "Neutral"}) {
        dg::Faction faction; faction.name = name; faction.is_neutral = std::string_view(name) == "Neutral";
        data.factions.push_back(faction);
    }
    dg::Planet planet; planet.name = "Place"; planet.galactic_position = dg::Position{};
    planet.behaviors = {"PLANET"}; data.planets.push_back(planet);
    dg::Campaign authored; authored.name = "Synthetic"; authored.locations = {"Place"}; authored.planets = authored.locations;
    data.campaigns.push_back(authored);
    data.constants = {{"MaximumFleetMovementDistance", "10", f(10)}, {"Fiscal_Cycle_Time_In_Secs", "1", f(1)}};
    eawr::units::GalacticType ship; ship.name = "RenamedWell"; ship.object_class = "SpaceUnit"; ship.hyperspace_capable = true;
    auto other = ship; other.name = "RenamedOther";
    eawr::units::GalacticTypeTable types; types.types = {ship, other};
    campaign::CampaignSetupOptions options;
    options.players = {{{1, 1, 0}, "First", true}, {{2, 2, 0}, "Second", true}, {{3, 3, 0}, "Neutral", true}};
    options.types = {{1, "RenamedWell"}, {3, "RenamedOther"}};
    const auto plan = checked(campaign::plan_campaign_setup(data, types, authored.name, options));
    const auto services = checked(campaign::prepare_stock_tactical_return(files, catalog.catalog, plan));
    auto c = conflict(galaxy());
    c.tactical.first_free_entity_id = 100;
    c.tactical.players.push_back({3, 2, 3, 1}); // allied observer must not receive prevention
    c.tactical.campaign.emplace();
    c.tactical.campaign->players = {{1, {}, 20}, {2, {}, 20}, {3, {}, 20}};
    for (auto& player : c.tactical.campaign->players) player.credit_cap = f(100);
    c.tactical.campaign->reserves = {{22, 3, 1, false, 0, {}}};
    tactical::UnitState second; second.entity_id = 41; second.type_id = 1; second.owner = 2;
    second.position = {f(10000), {}, {}}; c.tactical.units.push_back(second);
    std::sort(c.tactical.units.begin(), c.tactical.units.end(), [](const auto& a, const auto& b) { return a.entity_id < b.entity_id; });
    auto binding = content(c);
    tactical::AbilityProfile well; well.kind = tactical::AbilityKind::interdict; well.disable_movement = true;
    binding.abilities.profiles.front().abilities.push_back(well); // slot 1, never assume primary
    binding.abilities.profiles.front().gravity_well_hardpoints = {0};
    binding.durability.profiles.front().hardpoints = {{tactical::HardpointRole::other, true, f(50)}};
    binding.economy.campaign = true; binding.economy.players = c.tactical.campaign->players;
    binding.economy.campaign_population = {{1, 1}, {3, 0}};
    binding.economy.heroes = {{3, 1, {}}}; // authored purchase-to-tactical deployment mapping
    okay(services.bind(c, binding));
    expect(binding.retreat->prevention.size() == 4, "WIN-03: GC binder covers renamed deployed and reserve types with authored slots");
    for (const auto& source : binding.retreat->prevention) {
        expect(source.ability_slot == 1, "WIN-03: only INTERDICT slot binds, unrelated ability does not");
        expect(source.enemies == (source.unit == 40 || source.unit == 41
            ? std::vector<tactical::PlayerId>{1} : std::vector<tactical::PlayerId>{2, 3}),
            "WIN-03: recipients follow enemy teams, including every enemy player and excluding allies");
    }
    auto invalid = binding; invalid.retreat->prevention.push_back({999, 1, {2}});
    expect(!tactical::TacticalSession::create(c.tactical, invalid), "unknown absent prevention actor remains invalid");
    invalid = binding; invalid.retreat->prevention.back().ability_slot = 99;
    expect(!tactical::TacticalSession::create(c.tactical, invalid), "unbound prevention ability slot remains invalid");
    std::vector<std::string> reference;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto session = checked(tactical::TacticalSession::create(c.tactical, binding));
        std::vector<std::string> hashes;
        std::uint64_t sequence{};
        const auto advance = [&] {
            session.scramble_storage_for_testing(); hashes.push_back(checked(session.step(executor)).state_sha256);
        };
        const auto step = [&](const tactical::PlayerId player, tactical::CommandPayload payload,
            std::vector<eawr::sim::EntityId> units = {}) {
            okay(session.submit({{session.completed_tick(), player, ++sequence}, std::move(units), std::move(payload)}));
            advance();
        };
        const auto refusal = [&](const tactical::PlayerId player) {
            const auto views = session.retreat_views();
            return std::find_if(views.begin(), views.end(), [&](const auto& row) { return row.player == player; })->refusal;
        };
        expect(refusal(1) == tactical::RetreatRefusal::none && refusal(2) == tactical::RetreatRefusal::none,
            "inactive wells and undeployed reserves permit retreat");
        step(2, tactical::AbilityPayload{tactical::AbilityKind::interdict, tactical::AbilityAction::activate}, {40, 41});
        expect(refusal(1) == tactical::RetreatRefusal::prevention && refusal(2) == tactical::RetreatRefusal::none,
            "active wells block the enemy regardless of distance, without blocking their owner");
        step(1, tactical::RetreatPayload{tactical::RetreatAction::start});
        expect(session.retreat_state().phase == tactical::RetreatPhase::idle, "prevented GC retreat request is refused");
        step(2, tactical::AbilityPayload{tactical::AbilityKind::interdict, tactical::AbilityAction::deactivate}, {40});
        expect(refusal(1) == tactical::RetreatRefusal::prevention, "second source survives first source switch-off");
        step(2, tactical::DamagePayload{f(1000), 0}, {41});
        advance(); // existing ability lifecycle consumes generator loss on its next service
        expect(refusal(1) == tactical::RetreatRefusal::none, "WHD-04: generator loss releases the last active source");
        step(2, tactical::AbilityPayload{tactical::AbilityKind::interdict, tactical::AbilityAction::activate}, {40});
        expect(refusal(1) == tactical::RetreatRefusal::prevention, "surviving source can resume prevention");
        step(2, tactical::DamagePayload{f(1000)}, {40});
        expect(refusal(1) == tactical::RetreatRefusal::none, "destroying the last active source releases retreat");
        step(1, tactical::ReinforcePayload{3, {f(-500), {}, {}}, 22});
        const auto deployed = session.units();
        expect(std::any_of(deployed.begin(), deployed.end(), [](const auto& unit) { return unit.entity_id == 22 && unit.type_id == 1; }),
            "reserve deploys as its authored alternate tactical type with persistent identity");
        expect(refusal(2) == tactical::RetreatRefusal::none, "deployed but inactive reserve does not prevent retreat");
        for (unsigned frame = 0; session.arrivals().contains(22) && frame < 200; ++frame) advance();
        expect(!session.arrivals().contains(22), "reserve completes arrival before manual ability activation");
        step(1, tactical::AbilityPayload{tactical::AbilityKind::interdict, tactical::AbilityAction::activate}, {22});
        const auto activated = session.ability_state(22);
        expect(activated && activated->slots.size() == 2 && activated->slots[1].active,
            "later-deployed reserve's authored INTERDICT activation succeeds");
        expect(refusal(2) == tactical::RetreatRefusal::prevention, "later-deployed reserve supplies active enemy prevention");
        step(1, tactical::AbilityPayload{tactical::AbilityKind::interdict, tactical::AbilityAction::deactivate}, {22});
        expect(refusal(2) == tactical::RetreatRefusal::none, "reserve switch-off removes only its source");
        if (reference.empty()) reference = hashes;
        else expect(hashes == reference, "GC active and reserve interdiction hashes match at 1/2/4/8 workers");
    }
}
std::optional<std::string> game_root() {
    return eawr::testing::game_root();
}
void installed() {
    const auto root = game_root();
    if (!root) { eawr::testing::skip_installed("SKIPPED: installed return bindings need game data\n", 0, std::cout); return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto* folder : {"corruption", "GameData"}) mounts.push_back(checked(eawr::vfs::resolve_manifest_mount(folder, std::filesystem::path(*root) / folder / "Data")).mount);
    const auto filesystem = checked(eawr::vfs::Vfs::mount(mounts));
    const auto catalog = checked(eawr::data::load_catalog(filesystem, eawr::data::Profile::foc));
    auto data = checked(eawr::data::galactic::load_galactic_data(filesystem, catalog.catalog));
    const auto types = eawr::units::galactic_type_table(catalog.catalog);
    eawr::data::galactic::Campaign authored; authored.name = "ReturnFixture"; authored.locations = {"Shola", "Kamino"}; authored.planets = authored.locations; authored.starting_active_player = "Rebel";
    authored.starting_tech_levels = {{"Rebel", 0}, {"Empire", 0}}; authored.max_tech_levels = {{"Rebel", 5}, {"Empire", 5}}; data.campaigns.push_back(authored);
    campaign::CampaignSetupOptions options; options.seed = 123; options.solo_reference_human = 1;
    options.players = {{{1, 1, 0}, "Rebel", true}, {{2, 2, 0}, "Empire", true}, {{3, 3, 0}, "Neutral", true}};
    options.seats = {{{1}, 1, gc::SeatKind::human, 0, true, 1}, {{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    for (const auto& type : types.types) options.types.push_back({eawr::assets::object_type_crc(type.name), type.name});
    std::sort(options.types.begin(), options.types.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    const auto plan = checked(campaign::plan_campaign_setup(data, types, authored.name, options));
    const auto services = checked(campaign::prepare_stock_tactical_return(filesystem, catalog.catalog, plan));
    campaign::ConflictRecord c; c.invader = 1; c.defender = 2;
    const auto reserve_type = std::find_if(types.types.begin(), types.types.end(), [](const auto& type) { return type.hyperspace_capable.value_or(false); });
    expect(reserve_type != types.types.end(), "stock fixture has a hyperspace-capable reserve type");
    if (reserve_type == types.types.end()) return;
    const auto reserve_id = eawr::assets::object_type_crc(reserve_type->name);
    c.forces = {{101, reserve_id, 1, 0, campaign::ForceRole::reinforcement},
        {102, reserve_id, 1, 0, campaign::ForceRole::reinforcement}};
    c.forces.back().carrier = 101;
    c.tactical.campaign.emplace();
    tactical::CarriedObject passenger; passenger.entity_id = 102; passenger.type_id = reserve_id; passenger.named_hero = true;
    c.tactical.campaign->reserves = {{101, reserve_id, 1, false, 0, {passenger}}};
    tactical::TacticalContent binding; okay(services.bind(c, binding));
    expect(binding.retreat && binding.retreat->rules.lockout_frames > 0 && binding.retreat->rules.players.size() == 2
        && binding.retreat->flight_qualified, "stock authored countdowns bind qualified base locomotion");
    expect(binding.retreat && binding.retreat->actors.size() == 1 && binding.retreat->actors.front().id == 101
        && binding.retreat->actors.front().carries_named_hero,
        "stock binder includes reserve roots, excludes passengers and protects their recorded named heroes");
    expect(binding.retreat && binding.retreat->prevention.empty(), "types without authored INTERDICT have no prevention sources");
    squadron_return(&services, eawr::assets::object_type_crc("Rebel_X-Wing_Squadron"), eawr::assets::object_type_crc("Nebulon_B_Frigate"));
}
}
int case_galactic_tactical_return_tests(int argc, char** argv) {
    try { if (argc > 1 && std::string_view(argv[1]) == "--nonadjacent-retreat") parent_return(true); else if (argc > 1) installed(); else { retreat_destination_ties(); pure_policy(); hero_fallback(); cinematic_authority(); station_explosion(); parent_return(); parent_return(true); selected_recording(); squadron_return(); deployed_reserve(); land_return_keeps_orbit(); stock_faction_retreat(); stock_interdiction(); } }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
