#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "game_session_loopback.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace galactic_test;
namespace campaign = eawr::campaign;
namespace tactical = eawr::sim::tactical;
namespace cp = eawr::checkpoint;
namespace lockstep = eawr::testing::lockstep;
namespace math = eawr::sim::math;
using eawr::testing::result_code_colon::take;
void check(eawr::core::Result<void> result) { eawr::testing::must(std::move(result)); }

galactic::GalacticSetup setup(bool coop = false) {
    galactic::SyntheticGalaxyOptions options; options.planets = 8; options.players = 4;
    options.fleets_per_player = 1; options.members_per_fleet = 1; options.debug_commands = true;
    auto result = galactic::synthetic_galaxy(options); result.seats.clear();
    for (std::uint32_t seat = 1; seat <= 8; ++seat) {
        const auto side = coop || seat <= 4 ? 1U : 2U;
        result.seats.push_back({{seat}, side, galactic::SeatKind::human, seat - 1, true, side});
    }
    for (std::uint32_t player = 1; player <= 4; ++player)
        result.seats.push_back({{8 + player}, player, galactic::SeatKind::ai, 0, player > (coop ? 1U : 2U), 0});
    result.seat_control.emplace();
    result.seat_control->teams.push_back({1, {1}, false});
    if (!coop) result.seat_control->teams.push_back({2, {5}, true});
    for (std::uint32_t player = 1; player <= 4; ++player)
        result.seat_control->controllers.push_back({player, player <= (coop ? 1U : 2U) ? galactic::FactionControl::human : galactic::FactionControl::ai,
            galactic::FactionControl::ai, {1, 2}, {3, 4, 5}});
    return result;
}
campaign::ConflictRecord conflict(const galactic::GalacticSetup& config, std::uint32_t defender = 3) {
    auto galaxy = take(galactic::GalacticSession::create(config));
    campaign::ConflictRecord record; record.id = galaxy.next_entity_id(); record.planet = config.planets.front().id;
    record.invader = 1; record.defender = defender; record.tactical.content_identity = config.content_identity;
    record.tactical.first_free_entity_id = record.id + 20;
    record.tactical.players = {{1, 1, 1, 1}, {defender, defender, defender, 1}};
    for (const auto& fleet : config.fleets) if (fleet.owner == 1 || fleet.owner == defender) {
        const auto& member = fleet.members.front();
        record.forces.push_back({member.id, member.type, fleet.owner, fleet.id});
        tactical::UnitState unit; unit.entity_id = member.id; unit.type_id = member.type; unit.owner = fleet.owner;
        record.tactical.units.push_back(unit);
    }
    for (const auto& seat : config.seats) if (seat.active && (seat.player == 1 || seat.player == defender))
        record.seats.push_back({seat.seat_id, seat.player});
    return record;
}
tactical::TacticalContent content(const campaign::ConflictRecord& record) {
    tactical::TacticalContent value;
    for (const auto& force : record.forces) {
        tactical::DurabilityProfile row; row.type_id = force.type; row.max_hull = math::Fixed::from_raw(100 * math::Fixed::scale);
        if (std::none_of(value.durability.profiles.begin(), value.durability.profiles.end(), [&](const auto& p) { return p.type_id == row.type_id; }))
            value.durability.profiles.push_back(row);
    }
    std::sort(value.durability.profiles.begin(), value.durability.profiles.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    return value;
}
galactic::TacticalLayerCommand input(const campaign::ConflictRecord& record, tactical::PlayerCommand command) {
    tactical::TacticalReplay replay; replay.setup.players = record.tactical.players;
    replay.final_tick_count = 1; replay.commands.push_back(std::move(command));
    const auto bytes = take(tactical::write_replay(replay));
    const auto header = static_cast<std::size_t>(bytes[10]) | (static_cast<std::size_t>(bytes[11]) << 8U);
    const auto start = header + replay.setup.players.size() * 24 + 4;
    return {record.id, bytes[start + 20], std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(start), bytes.end())};
}
cp::GameLoadOptions load_options() {
    cp::GameLoadOptions options; options.script_requirement = eawr::campaign::ScriptRequirement::none;
    options.content = [](const campaign::ConflictRecord& record) { return eawr::core::Result<tactical::TacticalContent>::success(content(record)); };
    return options;
}
void roundtrip(campaign::GameSession& game, const eawr::sim::PartitionExecutor& executor) {
    const auto identities = cp::game_identities(game.galaxy().setup());
    const auto bytes = take(cp::save_game_checkpoint(game, identities));
    auto restored = take(cp::load_game_checkpoint(bytes, identities, load_options(), executor));
    expect(restored.session.state_sha256() == game.state_sha256(), "restore retains role/controller/lead hash");
    expect(take(cp::save_game_checkpoint(restored.session, identities)) == bytes, "checkpoint bytes are canonical after pending/active role restoration");
    expect(restored.session.galaxy().seat_control() == game.galaxy().seat_control(), "pending choice, controller state and leads retained");
    if (game.battle()) expect(*restored.session.conflict() == *game.conflict(), "effective whole-team mapping restored");
    for (int i = 0; i < 3; ++i)
        expect(take(restored.session.step(executor)).state_hash.get() == take(game.step(executor)).state_hash.get(), "restored child continues identically through readiness waits");
}
void roster_and_dropout() {
    const eawr::sim::InlineExecutor executor;
    const galactic::CommandPayload role = galactic::AssignBattleTeamRole{11, galactic::BattleTeamRole::ally};
    auto wire = galactic::encode_payload(role);
    expect(galactic::decode_payload(galactic::CommandLayer::session, galactic::session_opcodes::battle_team_role, wire) == role,
        "whole-team choice command has canonical replay bytes");
    wire.push_back(1);
    expect(!galactic::decode_payload(galactic::CommandLayer::session, galactic::session_opcodes::battle_team_role, wire),
        "role wire refuses extra bytes that could encode an unauthorized seat subset");
    wire.pop_back(); wire.back() = 0;
    expect(!galactic::decode_payload(galactic::CommandLayer::session, galactic::session_opcodes::battle_team_role, wire),
        "pending is service-owned state and cannot be selected from the wire");
    for (const auto coop : {false, true}) {
        const auto config = setup(coop);
        expect(static_cast<bool>(galactic::validate_setup(config)), "8-seat coop and 4v4 admit two AI factions");
        galactic::GalacticReplay replay; replay.setup = config;
        const auto bytes = take(galactic::write_replay(replay));
        expect(take(galactic::parse_replay(bytes)) == replay, "seat policy extension round trips without graph extension");
        auto broken = config; broken.seats.insert(broken.seats.begin() + 8, {{13}, 1, galactic::SeatKind::human, 8, false, 1});
        expect(!galactic::validate_setup(broken), "inactive reconnect slots consume the eight-human roster limit");
        broken = config; broken.seats[7].human_side_id = 3;
        expect(!galactic::validate_setup(broken), "third human side refused");
        broken = config; broken.seat_control->teams.front().lead = {77};
        expect(!galactic::validate_setup(broken), "lead must be active member of its team");
        broken = config; broken.seat_control->controllers.back().kind = galactic::FactionControl::human;
        expect(!galactic::validate_setup(broken), "nonplayer mod faction remains AI");
        broken = config; broken.seat_control->controllers.back().state.resize(16385);
        expect(!galactic::write_replay({broken, 0, {}}), "untrusted persistent controller payload is bounded before allocation/encoding");
        broken = config;
        for (auto& seat : broken.seats) if (seat.player == 1) seat.active = seat.kind == galactic::SeatKind::ai;
        broken.seat_control->teams.front().lead = {};
        broken.seat_control->controllers.front().kind = galactic::FactionControl::ai;
        expect(static_cast<bool>(galactic::validate_setup(broken)), "tick-zero disconnected human team permits its active fallback issuer");
        auto game = take(campaign::GameSession::create(config));
        check(game.submit({{0, {1}, 1}, galactic::SeatLeave{}})); (void)take(game.step(executor));
        expect(game.galaxy().seat_control()->control.teams.front().lead.value == 2, "leaving lead reassigns deterministically");
        expect(game.galaxy().seat_control()->control.controllers.front().kind == galactic::FactionControl::human, "remaining seat retains faction control");
        const auto humans = coop ? 8U : 4U;
        for (std::uint32_t seat = 2; seat <= humans; ++seat) {
            check(game.submit({{game.completed_tick(), {seat}, 1}, galactic::SeatLeave{}})); (void)take(game.step(executor));
        }
        expect(game.galaxy().seat_control()->control.teams.front().lead.value == 0
            && game.galaxy().seat_control()->control.controllers.front().kind == galactic::FactionControl::ai,
            "last human dropout activates configured faction AI with no lead");
        expect(game.galaxy().seats()[8].active, "bound fallback AI issuer is activated");
        check(game.submit({{game.completed_tick(), {1}, 2}, galactic::SeatRejoin{}})); (void)take(game.step(executor));
        expect(game.galaxy().seat_control()->control.teams.front().lead.value == 1
            && game.galaxy().seat_control()->control.controllers.front().kind == galactic::FactionControl::human,
            "reconnect reactivates the human faction and lead");
        expect(!game.galaxy().seats()[8].active, "fallback AI issuer is disabled on reconnect");
        roundtrip(game, executor);
        broken = config; broken.seat_control->controllers.front().dropout = galactic::FactionControl::passive;
        auto passive = take(campaign::GameSession::create(broken));
        for (std::uint32_t seat = 1; seat <= humans; ++seat) check(passive.submit({{0, {seat}, 1}, galactic::SeatLeave{}}));
        (void)take(passive.step(executor));
        expect(passive.galaxy().seat_control()->control.controllers.front().kind == galactic::FactionControl::passive
            && !passive.galaxy().seats()[8].active, "explicit passive dropout policy does not activate AI");
    }
}
void roles_and_checkpoint() {
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (const auto role : {galactic::BattleTeamRole::ai_side, galactic::BattleTeamRole::spectate, galactic::BattleTeamRole::ally}) {
            const auto config = setup(); const auto record = conflict(config); const auto tables = content(record);
            auto game = take(campaign::GameSession::create(config)); check(game.stage_battle(record, tables, eawr::campaign::ScriptRequirement::none));
            (void)take(game.step(executor));
            expect(game.galaxy().seat_control()->battle->role == galactic::BattleTeamRole::pending, "manual human-AI entry awaits other team lead");
            expect(!game.battle_view({5}), "pending team sees no unapproved perspective");
            roundtrip(game, executor);
            expect(game.battle()->completed_tick() == 0 && game.galaxy().clock().galactic_frame == 1, "role wait steps neither galaxy nor child");
            check(game.submit({{game.completed_tick(), {6}, 1}, galactic::AssignBattleTeamRole{record.id, role}}));
            (void)take(game.step(executor));
            expect(game.galaxy().snapshot()->events().front().reason == galactic::GalacticRejectReason::not_authorized, "non-lead whole-team choice refused");
            check(game.submit({{game.completed_tick(), {5}, 1}, galactic::AssignBattleTeamRole{record.id + 1, role}}));
            (void)take(game.step(executor));
            expect(game.galaxy().seat_control()->battle->role == galactic::BattleTeamRole::pending, "stale conflict choice cannot unlock readiness");
            check(game.submit({{game.completed_tick(), {5}, 2}, galactic::AssignBattleTeamRole{record.id, role}}));
            const auto published = take(game.step(executor));
            expect(game.battle()->completed_tick() == 1, "lead choice opens child readiness");
            for (std::uint32_t seat = 5; seat <= 8; ++seat) {
                const auto mapping = std::find_if(game.conflict()->seats.begin(), game.conflict()->seats.end(), [&](const auto& row) { return row.seat.value == seat; });
                const auto player = role == galactic::BattleTeamRole::ai_side ? 3U : role == galactic::BattleTeamRole::ally ? 1U : 0U;
                expect(mapping != game.conflict()->seats.end() && mapping->player == player, "all four opposing seats atomically share one battle role");
                const auto view = take(game.battle_view({seat}));
                expect(view.commandable == (role != galactic::BattleTeamRole::spectate)
                    && view.perspective == (role == galactic::BattleTeamRole::ally ? 1U : 3U), "observer/controller view uses approved perspective");
                expect(view.instances.size() < game.battle()->snapshot()->instances().size()
                    && std::all_of(view.instances.begin(), view.instances.end(), [&](const auto& instance) { return instance.owner == view.perspective; }),
                    "seat view excludes instances hidden from its approved perspective before publication");
            }
            expect(game.battle_controller(3) == (role == galactic::BattleTeamRole::ai_side ? galactic::FactionControl::human : galactic::FactionControl::ai), "temporary AI takeover affects battle controller only");
            expect(game.galaxy().seat_control()->control.controllers[2].kind == galactic::FactionControl::ai, "galactic AI controller is never gifted to the other team");
            const auto unit = record.tactical.units.back().entity_id;
            const auto stop = input(record, {{0, 3, 1}, {unit}, tactical::StopPayload{}});
            const auto submitted = game.submit({{game.completed_tick(), {7}, 2}, stop});
            expect(static_cast<bool>(submitted) == (role != galactic::BattleTeamRole::spectate), "observers cannot submit even ordinary tactical orders");
            if (role == galactic::BattleTeamRole::ally) {
                (void)take(game.step(executor)); // owning-player validation refuses ordering AI unit
            } else if (submitted) (void)take(game.step(executor));
            const auto forbidden = input(record, {{0, 3, 2}, {unit}, tactical::DamagePayload{math::Fixed::from_raw(math::Fixed::scale)}});
            expect(!game.submit({{game.completed_tick(), {8}, 2}, forbidden}), "human controller cannot forge engine-only tactical damage");
            roundtrip(game, executor);
            check(game.submit({{game.completed_tick(), {5}, 3}, galactic::SeatLeave{}}));
            (void)take(game.step(executor));
            expect(take(campaign::seat_battle_view(published, {5})).commandable == (role != galactic::BattleTeamRole::spectate),
                "retained immutable publication can build observer views without accessing the live session");
            expect(game.galaxy().seat_control()->control.teams[1].lead.value == 6, "battle dropout reassigns team lead");
            expect(!game.submit({{game.completed_tick(), {5}, 4}, stop}), "departed battle seat cannot issue commands");
            roundtrip(game, executor);
            check(game.submit({{game.completed_tick(), {6}, 4}, galactic::AssignBattleTeamRole{record.id, galactic::BattleTeamRole::ally}}));
            (void)take(game.step(executor));
            expect(game.galaxy().seat_control()->battle->role == role, "an established role cannot be changed by the new lead");
            check(game.submit({{game.completed_tick(), {5}, 5}, galactic::SeatRejoin{}}));
            (void)take(game.step(executor));
            expect(take(game.battle_view({5})).commandable == (role != galactic::BattleTeamRole::spectate), "reconnected seat resumes same whole-team role");
            const auto hash = game.state_sha256();
            if (workers == 1) reference.push_back(hash);
            else expect(hash == reference[static_cast<std::size_t>(static_cast<unsigned>(role) - 1)], "role/dropout/checkpoint hashes equal on 1/2/4/8 workers");
        }
    }
}
void battle_start_events() {
    const eawr::sim::InlineExecutor executor;
    auto config = setup(); config.rules.fiscal_cycle_frames = 1;
    const auto record = conflict(config);
    auto ordinary = take(campaign::GameSession::create(config));
    auto staged = take(campaign::GameSession::create(config));
    check(staged.stage_battle(record, content(record), eawr::campaign::ScriptRequirement::none));
    for (auto* game : {&ordinary, &staged}) {
        check(game->submit({{0, {1}, 1}, galactic::SetSpeed{false, math::Fixed::from_raw(math::Fixed::scale)}}));
        check(game->submit({{0, {5}, 1}, galactic::AssignBattleTeamRole{record.id, galactic::BattleTeamRole::ally}}));
    }
    const auto expected = take(ordinary.step(executor));
    const auto started = take(staged.step(executor));
    expect(staged.galaxy().seat_control()->battle->role == galactic::BattleTeamRole::pending,
        "battle-start event regression enters the role readiness gate");
    const auto events = started.galaxy->events();
    for (const auto kind : {galactic::GalacticEventKind::command_executed,
        galactic::GalacticEventKind::command_rejected, galactic::GalacticEventKind::fiscal_cycle_completed})
        expect(std::any_of(events.begin(), events.end(), [kind](const auto& event) { return event.kind == kind; }),
            "battle start retains command results and fiscal events");
    expect(std::equal(events.begin(), events.end(), expected.galaxy->events().begin(), expected.galaxy->events().end()),
        "published battle-start tick retains the complete ordered galaxy event list");
    expect(started.galaxy == staged.galaxy().snapshot(), "live galaxy snapshot retains published battle-start events");
}
void unrelated_ai_activity() {
    const eawr::sim::InlineExecutor executor;
    auto config = setup();
    for (auto& seat : config.seats) if (seat.kind == galactic::SeatKind::ai && seat.player == 4) seat.active = false;
    auto game = take(campaign::GameSession::create(config));
    for (const auto leave : {true, false}) {
        const galactic::CommandPayload payload = leave ? galactic::CommandPayload{galactic::SeatLeave{}}
            : galactic::CommandPayload{galactic::SeatRejoin{}};
        check(game.submit({{game.completed_tick(), {1}, leave ? 1U : 2U}, payload}));
        (void)take(game.step(executor));
        const auto seats = game.galaxy().seats();
        const auto unrelated = std::find_if(seats.begin(), seats.end(), [](const auto& seat) { return seat.seat_id.value == 12; });
        expect(unrelated != seats.end() && !unrelated->active, "unrelated AI-only issuer retains its configured inactive flag");
    }
    roundtrip(game, executor);
}
void readiness_departure_and_return() {
    const eawr::sim::InlineExecutor executor;
    auto config = setup(); auto record = conflict(config); auto tables = content(record);
    auto pending = take(campaign::GameSession::create(config)); check(pending.stage_battle(record, tables, eawr::campaign::ScriptRequirement::none));
    (void)take(pending.step(executor));
    for (std::uint32_t seat = 5; seat <= 8; ++seat) check(pending.submit({{1, {seat}, 1}, galactic::SeatLeave{}}));
    (void)take(pending.step(executor));
    expect(pending.galaxy().seat_control()->battle->role == galactic::BattleTeamRole::spectate
        && pending.battle()->completed_tick() == 1, "empty choosing team closes its pending readiness choice");
    expect(pending.galaxy().snapshot()->events().size() == 4, "all departures publish command events in canonical order");
    roundtrip(pending, executor);

    auto versus = take(campaign::GameSession::create(config)); const auto human_battle = conflict(config, 2);
    check(versus.stage_battle(human_battle, content(human_battle), eawr::campaign::ScriptRequirement::none)); (void)take(versus.step(executor));
    expect(!versus.galaxy().seat_control()->battle, "human-versus-human battle has no other-team role choice");
    const auto stop = input(human_battle, {{0, 1, 1}, {human_battle.forces.front().id}, tactical::StopPayload{}});
    check(versus.submit({{1, {1}, 1}, galactic::SeatLeave{}}));
    check(versus.submit({{1, {1}, 2}, stop}));
    const auto tick = take(versus.step(executor));
    expect(!tick.diagnostics.empty() && versus.battle_record()->commands.empty(), "leave precedes queued tactical order in canonical sequence order");

    config.seats.push_back({{99}, 1, galactic::SeatKind::script, 0, true, 0});
    record = conflict(config); tables = content(record);
    tables.victory.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    tables.victory.contenders = {1, 3}; tables.victory.controlled_players = {1, 3}; tables.victory.humans = {1};
    tables.victory.countdown_frames = 2;
    for (const auto& type : tables.durability.profiles) tables.victory.relevant_types.push_back(type.type_id);
    auto game = take(campaign::GameSession::create(config)); check(game.stage_battle(record, tables, eawr::campaign::ScriptRequirement::none));
    (void)take(game.step(executor));
    check(game.submit({{1, {5}, 1}, galactic::AssignBattleTeamRole{record.id, galactic::BattleTeamRole::ally}}));
    (void)take(game.step(executor));
    const auto defender = record.tactical.units.back().entity_id;
    const auto damage = input(record, {{0, 1, 1}, {defender}, tactical::DamagePayload{math::Fixed::from_raw(1000 * math::Fixed::scale)}});
    check(game.submit({{2, {99}, 1}, damage}));
    bool returned{};
    for (int i = 0; i < 12 && game.battle(); ++i) {
        const auto result = take(game.step(executor));
        if (result.result) {
            returned = true;
            expect(result.result->winner == 1 && result.result->forces.back().id == defender
                && result.result->forces.back().disposition == campaign::ForceDisposition::destroyed,
                "temporary ally result attributes casualties to original persistent force");
        }
    }
    expect(returned && !game.galaxy().seat_control()->battle, "ordinary battle return removes temporary roles");
    expect(game.galaxy().seat_control()->control.controllers[1].kind == galactic::FactionControl::human
        && game.galaxy().seat_control()->control.controllers[2].kind == galactic::FactionControl::ai,
        "return retains original opposing human and AI galactic controllers");
    expect(std::equal(game.galaxy().snapshot()->planets().begin(), game.galaxy().snapshot()->planets().end(), config.planets.begin()),
        "battle delegation never mutates galactic ownership");
}
void all_peers_lockstep() {
    using Adapter = lockstep::GameSessionAdapter; using Peer = lockstep::Peer<Adapter>;
    for (const auto coop : {true, false}) for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers); const auto config = setup(coop);
        const auto record = conflict(config); const auto tables = content(record);
        std::vector<galactic::GameCommand> commands;
        if (!coop) commands.push_back({{2, {5}, 1}, galactic::AssignBattleTeamRole{record.id, galactic::BattleTeamRole::spectate}});
        commands.push_back({{5, {1}, 1}, galactic::SeatLeave{}});
        commands.push_back({{7, {1}, 2}, galactic::SeatRejoin{}});
        auto split = lockstep::split_by_issuer<Adapter>(commands, 8);
        std::vector<Peer> peers;
        for (std::size_t index = 0; index < 8; ++index) {
            auto game = take(campaign::GameSession::create(config)); check(game.stage_battle(record, tables, eawr::campaign::ScriptRequirement::none));
            peers.push_back({std::move(game), &executor, std::move(split[index]), {}, {}});
            if (index != 0) peers.back().before_step = [](campaign::GameSession& session, std::uint64_t) { session.scramble_storage_for_testing(); };
        }
        std::vector<Peer*> pointers; for (auto& peer : peers) pointers.push_back(&peer);
        lockstep::Options options; options.final_tick = 12; options.compare_every = 1;
        options.input_delay = 1; options.hold_every = 3; options.hold_rounds = 2;
        const auto report = lockstep::run<Adapter>(pointers, options);
        expect(report.agreed(), report.describe());
        expect(report.total_stalls() > 0 && report.stalls.size() == 8, "all eight peers, including empty spectator peers, wait for completeness");
    }
}
void generated_controller_orders() {
    namespace auth = eawr::script::authoritative;
    const eawr::sim::InlineExecutor executor;
    const auto config = setup(); const auto record = conflict(config); const auto tables = content(record);
    const auto scheduler = [](const campaign::ConflictRecord& entry) {
        auth::SessionConfig options; options.seed = campaign::child_seed(11, entry.id);
        auth::ModuleManifest modules;
        check(modules.add("Main.lua", R"LUA(
function main()
    while true do
        Hold()
        Effect()
        coroutine.yield(true)
    end
end
Create_Thread("main")
)LUA"));
        auto scripts = take(auth::ScriptScheduler::create(options, std::move(modules)));
        for (const auto verb : {"Hold", "Effect"}) check(scripts.register_binding(verb,
            [verb](auth::BindingContext& context, const auth::ValueList&) {
                context.issue_command(verb, {});
                return eawr::core::Result<auth::ValueList>::success({});
            }));
        check(scripts.create_instance(1, "Main.lua")); return scripts;
    };
    const auto unit = record.tactical.units.back().entity_id;
    const auto configure = [unit](auth::ScriptedTacticalSession& host) {
        auto hold = host.register_verb("Hold", [unit](const auth::ScriptCommand&) {
            return eawr::core::Result<auth::TacticalOrder>::success({3, {unit}, tactical::StopPayload{}, {}});
        });
        if (!hold) return hold;
        return host.register_verb("Effect", [unit](const auth::ScriptCommand&) {
            return eawr::core::Result<auth::TacticalOrder>::success({3, {unit}, tactical::DamagePayload{math::Fixed::from_raw(math::Fixed::scale)}, {}});
        });
    };
    for (const auto role : {galactic::BattleTeamRole::ai_side, galactic::BattleTeamRole::spectate}) {
        auto game = take(campaign::GameSession::create(config)); game.set_child_configurator(configure);
        check(game.stage_battle(record, tables, scheduler(record))); (void)take(game.step(executor));
        check(game.submit({{1, {5}, 1}, galactic::AssignBattleTeamRole{record.id, role}}));
        for (int i = 0; i < 5; ++i) (void)take(game.step(executor));
        const auto log = *game.battle_record();
        const auto stops = std::count_if(log.commands.begin(), log.commands.end(), [](const auto& command) { return std::holds_alternative<tactical::StopPayload>(command.payload); });
        const auto effects = std::count_if(log.commands.begin(), log.commands.end(), [](const auto& command) { return std::holds_alternative<tactical::DamagePayload>(command.payload); });
        expect((role == galactic::BattleTeamRole::ai_side ? stops == 0 : stops > 0) && effects > 0,
            "temporary human takeover suppresses generated AI orders and preserves privileged scripted effects");
        auto options = load_options(); options.configure_child = configure;
        options.scripts = [scheduler](const campaign::ConflictRecord& entry) {
            return eawr::core::Result<auth::ScriptScheduler>::success(scheduler(entry));
        };
        const auto identities = cp::game_identities(config);
        const auto bytes = take(cp::save_game_checkpoint(game, identities));
        auto restored = take(cp::load_game_checkpoint(bytes, identities, options, executor));
        for (int i = 0; i < 3; ++i)
            expect(take(restored.session.step(executor)).state_hash.get() == take(game.step(executor)).state_hash.get(),
                "restored script controller gate follows the retained team role");
    }
}
// TM-04: a rejoin that makes the session multi-human ends strategic fast forward, deterministically.
void rejoin_ends_fast_forward() {
    const eawr::sim::InlineExecutor executor;
    auto config = setup(true);
    for (auto& seat : config.seats) if (seat.kind == galactic::SeatKind::human && seat.seat_id.value > 1) seat.active = false;
    auto live = take(galactic::GalacticSession::create(config));
    const auto fast = math::Fixed::from_raw(galactic::fast_forward_factor_raw);
    check(live.submit({{0, {1}, 1}, galactic::SetSpeed{false, fast}}));
    (void)take(live.step(executor));
    expect(live.clock().strategic_speed == fast, "one active human keeps fast forward");
    check(live.submit({{1, {2}, 1}, galactic::SeatRejoin{}}));
    const auto tick = take(live.step(executor));
    const auto changed = std::count_if(tick.snapshot->events().begin(), tick.snapshot->events().end(),
        [](const galactic::GalacticEvent& event) { return event.kind == galactic::GalacticEventKind::speed_changed; });
    expect(live.clock().strategic_speed.raw() == math::Fixed::scale && changed == 1, "a second human rejoining resets strategic speed to 1x");
    std::string last;
    for (int index = 0; index < 5; ++index) last = take(live.step(executor)).state_sha256;
    auto replayed = take(galactic::GalacticSession::from_replay(live.record()));
    std::string again;
    for (int index = 0; index < 7; ++index) again = take(replayed.step(executor)).state_sha256;
    expect(again == last && replayed.clock().strategic_speed.raw() == math::Fixed::scale,
        "the reset replays to the same state hash");
}
} // namespace
int case_galactic_seat_roles_tests() {
    try { roster_and_dropout(); roles_and_checkpoint(); battle_start_events(); unrelated_ai_activity(); readiness_departure_and_return(); all_peers_lockstep(); generated_controller_orders(); rejoin_ends_fast_forward(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return failures == 0 ? 0 : 1;
}
