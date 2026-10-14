// UI-07 (#304): the command sink (UI-C1, UI-C2) and the input routing policy (UI-I1, UI-I3).
// The Godot dispatch half of UI-I1 to UI-I3 runs in the viewer
// (tests/presentation/renderer/test_input_routing.py).

#include "eawr/platform/sim_workers.hpp"
#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/presentation/ui/input_routing.hpp"
#include "eawr/presentation/ui/special_weapon.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "ui_test_support.hpp"
#include "../../apps/viewer/src/scripted_input_dispatch.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace eawr;
namespace ui = presentation::ui;
namespace tactical = sim::tactical;
using test::ui::expect;

constexpr tactical::PlayerId local_player = 1;

sim::math::Vec3 point(const std::int64_t x, const std::int64_t y) {
    return {sim::math::Fixed::from_integer(x).value(), sim::math::Fixed::from_integer(y).value(),
        sim::math::Fixed::from_integer(0).value()};
}

// Every aggregate field is given: the Linux presets build with -Wmissing-field-initializers.
ui::WorldPick at(const sim::math::Vec3 where, const sim::EntityId entity = sim::invalid_entity_id,
    const bool hostile = false) {
    return ui::WorldPick{where, entity, hostile, false, false};
}

ui::TacticalIntent intent(const ui::TacticalVerb verb, std::vector<sim::EntityId> units,
    const sim::math::Vec3 destination = {}, const sim::EntityId target = sim::invalid_entity_id,
    const std::uint32_t ability = 0, const ui::CommandOrigin origin = ui::CommandOrigin::world_click) {
    return ui::TacticalIntent{verb, std::move(units), destination, target, ability, origin};
}

tactical::TacticalSetup setup() {
    tactical::TacticalSetup out;
    out.seed = 304;
    out.players = {{1, 0, 1, tactical::player_flag_commandable}, {2, 1, 2, tactical::player_flag_commandable}};
    for (const auto& [id, owner] : std::vector<std::pair<sim::EntityId, tactical::PlayerId>>{
             {1, 1}, {2, 1}, {3, 1}, {10, 2}, {11, 2}}) {
        tactical::UnitState unit;
        unit.entity_id = id;
        unit.type_id = 7;
        unit.owner = owner;
        unit.position = point(static_cast<std::int64_t>(id) * 10, 0);
        out.units.push_back(unit);
    }
    return out;
}

// One frame script: what the player does before tick `tick` executes.
using Script = std::map<std::uint64_t, std::function<void(ui::OrderInput&)>>;

struct Run {
    std::vector<tactical::PlayerCommand> stream;
    std::vector<std::string> hashes;
    tactical::TacticalReplay replay;
};

// Plays `script` through OrderInput -> CommandScheduler -> TacticalSession, the path the live
// session bridge (#80) takes: intents queue during the frame and are stamped and submitted at
// the tick boundary.
Run play(const Script& script, const sim::PartitionExecutor& executor, const std::uint64_t ticks = 8) {
    Run run;
    auto created = tactical::TacticalSession::create(setup());
    expect(static_cast<bool>(created), "UI-C1 setup session is valid");
    if (!created) return run;
    auto session = std::move(created).value();
    ui::CommandScheduler scheduler(local_player);
    ui::OrderInput input(scheduler);
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        if (const auto step = script.find(tick); step != script.end()) {
            const std::string before = session.state_sha256();
            step->second(input);
            expect(session.state_sha256() == before && session.pending_command_count() == 0,
                "UI-C1: issuing an intent never touches the session");
        }
        for (auto& command : scheduler.take(session.completed_tick())) {
            expect(static_cast<bool>(session.submit(command)), "UI-C1: stamped commands are accepted");
            run.stream.push_back(std::move(command));
        }
        auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "UI-C1 tick executes");
        if (!stepped) return run;
        run.hashes.push_back(stepped.value().state_sha256);
    }
    run.replay = session.record();
    return run;
}

void check(ui::OrderInput&, const core::Result<void>& result, const char* message) {
    expect(static_cast<bool>(result), message);
}

// The same three orders given from the world (right clicks, a stop hotkey) and from the HUD
// (move and attack buttons then a left click, the stop button).
Script world_script() {
    return {
        {0, [](ui::OrderInput& in) {
             in.set_selection({2, 1});
             auto issued = in.world_command(at(point(50, 60)));
             expect(issued && issued.value(), "world: right click on the ground moves");
         }},
        {2, [](ui::OrderInput& in) {
             auto issued = in.world_command(at(point(100, 0), 10, true));
             expect(issued && issued.value(), "world: right click on an enemy attacks");
         }},
        {5, [](ui::OrderInput& in) { check(in, in.stop(ui::CommandOrigin::hotkey), "world: stop hotkey"); }},
    };
}

Script hud_script() {
    return {
        {0, [](ui::OrderInput& in) {
             in.set_selection({1, 2});
             in.arm(ui::OrderMode::move);
             auto issued = in.world_command(at(point(50, 60), 3), ui::CommandOrigin::hud_button);
             expect(issued && issued.value() && in.mode() == ui::OrderMode::none,
                 "HUD: move mode moves even onto a unit, then disarms");
         }},
        {2, [](ui::OrderInput& in) {
             in.arm(ui::OrderMode::attack);
             auto issued = in.world_command(at(point(100, 0), 10, true),
                 ui::CommandOrigin::hud_button);
             expect(issued && issued.value(), "HUD: attack mode attacks the enemy");
         }},
        {5, [](ui::OrderInput& in) { check(in, in.stop(ui::CommandOrigin::hud_button), "HUD: stop button"); }},
    };
}

// The HUD script plus presentation-local churn: selection changes that are undone, hover-like
// clicks with nothing selected, armed and cancelled modes, attack-mode clicks on friendly units
// and ground, and refused face/ability orders. None of it may reach the stream (UI-C2).
Script noisy_hud_script() {
    Script script = hud_script();
    script[0] = [](ui::OrderInput& in) {
        auto empty = in.world_command(at(point(1, 1)));
        expect(empty && !empty.value(), "UI-C2: an order click with no selection issues nothing");
        in.set_selection({3});
        in.arm(ui::OrderMode::attack);
        in.cancel_mode();
        in.set_selection({1, 2, 2, 0});
        in.arm(ui::OrderMode::move);
        auto issued = in.world_command(at(point(50, 60), 3), ui::CommandOrigin::hud_button);
        expect(issued && issued.value(), "UI-C2: HUD move after selection churn");
    };
    script[1] = [](ui::OrderInput& in) {
        in.arm(ui::OrderMode::attack);
        auto friendly = in.world_command(at(point(30, 0), 3));
        auto ground = in.world_command(at(point(30, 30)));
        expect(friendly && !friendly.value() && ground && !ground.value() && in.mode() == ui::OrderMode::attack,
            "UI-C2: attack mode ignores friendly units and ground and stays armed");
        in.cancel_mode();
        const auto ability = in.ability(0x1234U, at(point(5, 5)), ui::CommandOrigin::hud_button);
        expect(!ability && ability.error().code == ui::diagnostic_codes::unsupported_intent,
            "UI-C2: an ability order is refused until the rules have one, and queues nothing");
    };
    script[3] = [](ui::OrderInput& in) {
        const auto keep = in.selection();
        in.set_selection({11});
        in.set_selection(keep);
    };
    return script;
}

void command_sink_contracts() {
    ui::CommandScheduler scheduler(local_player, 40);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> issued;
    scheduler.set_issue_observer([&](std::uint64_t sequence, std::uint64_t epoch) { issued.emplace_back(sequence, epoch); });
    expect(scheduler.take(0).empty(), "an empty scheduler yields no commands");

    const auto no_units = scheduler.issue(intent(ui::TacticalVerb::stop, {}));
    expect(!no_units && no_units.error().code == ui::diagnostic_codes::invalid_intent, "an order needs units");
    const auto zero = scheduler.issue(intent(ui::TacticalVerb::stop, {0, 4}));
    expect(!zero && zero.error().code == ui::diagnostic_codes::invalid_intent, "unit ID zero is refused");
    const auto untargeted = scheduler.issue(intent(ui::TacticalVerb::attack, {4}));
    expect(!untargeted && untargeted.error().code == ui::diagnostic_codes::invalid_intent,
        "an attack needs a target");
    for (const auto verb : {ui::TacticalVerb::face, ui::TacticalVerb::ability}) {
        const auto refused = scheduler.issue(intent(verb, {4}, point(1, 2), sim::invalid_entity_id, 9));
        expect(!refused && refused.error().code == ui::diagnostic_codes::unsupported_intent,
            "face and ability have no rules-v1 command");
    }
    // #76 (space-abilities AB-50): an ability intent naming a modelled unit ability is an ability command.
    auto switch_on = intent(ui::TacticalVerb::ability, {4});
    switch_on.unit_ability = tactical::AbilityKind::turbo;
    switch_on.ability_action = tactical::AbilityAction::deactivate;
    const auto ability_payload = ui::command_payload(switch_on);
    const auto* ability = ability_payload ? std::get_if<tactical::AbilityPayload>(&ability_payload.value()) : nullptr;
    expect(ability != nullptr && ability->ability == tactical::AbilityKind::turbo
               && ability->action == tactical::AbilityAction::deactivate,
        "a unit ability intent becomes an ability command");
    for (const auto kind : {tactical::AbilityKind::ion_cannon_shot, tactical::AbilityKind::concentrate_fire,
        tactical::AbilityKind::energy_weapon, tactical::AbilityKind::tractor_beam, tactical::AbilityKind::leech_shields}) {
        auto beam = intent(ui::TacticalVerb::ability, {4}, {}, 12, 0, ui::CommandOrigin::world_click);
        beam.unit_ability = kind;
        beam.hardpoint = 5;
        const auto transferred = ui::command_payload(beam);
        expect(transferred && transferred.value() == tactical::CommandPayload{
            tactical::AbilityPayload{kind, tactical::AbilityAction::activate, 12, 5}},
            "targeted ability world click preserves target and hardpoint through the command sink");
        beam.target = sim::invalid_entity_id;
        expect(!ui::command_payload(beam), "targeted ability activation refuses a missing world target");
    }
    // #531 (space-orders OR-20): an attack intent that names a hardpoint becomes an attack on it.
    auto on_hardpoint = intent(ui::TacticalVerb::attack, {4}, {}, 12, 0, ui::CommandOrigin::world_click);
    on_hardpoint.hardpoint = 5;
    const auto hardpoint_payload = ui::command_payload(on_hardpoint);
    expect(hardpoint_payload && hardpoint_payload.value() == tactical::CommandPayload{tactical::AttackPayload{12, 5}},
        "an attack intent with a hardpoint is an attack on that hardpoint");
    const auto hull_payload = ui::command_payload(intent(ui::TacticalVerb::attack, {4}, {}, 12, 0, ui::CommandOrigin::world_click));
    expect(hull_payload && hull_payload.value() == tactical::CommandPayload{tactical::AttackPayload{12, tactical::attack_hull}},
        "an attack intent without one is an attack on the unit");
    {
        ui::CommandScheduler reserve_scheduler(local_player);
        auto drop = intent(ui::TacticalVerb::reinforce, {}, point(3, 4));
        drop.type = 71;
        drop.reinforcement_token = 9001;
        drop.reinforcement_facing = sim::math::Fixed::from_raw(1234);
        expect(static_cast<bool>(reserve_scheduler.issue(drop)), "WGM-48: persistent reserve click schedules normally");
        const auto commands = reserve_scheduler.take(0);
        expect(commands.size() == 1 && std::get<tactical::ReinforcePayload>(commands[0].payload).pool_token == 9001
            && std::get<tactical::ReinforcePayload>(commands[0].payload).type == 71
            && std::get<tactical::ReinforcePayload>(commands[0].payload).facing_yaw == drop.reinforcement_facing,
            "WGM-48: command boundary retains reserve identity, type and deployment facing");
        drop.reinforcement_token = 0;
        const auto ordinary = ui::command_payload(drop);
        expect(ordinary && std::get<tactical::ReinforcePayload>(ordinary.value()).pool_token == 0,
            "ordinary tactical reinforcements retain type-based admission");
    }
    std::vector<sim::EntityId> many(tactical::max_units_per_command + 1U);
    for (std::size_t i = 0; i < many.size(); ++i) many[i] = i + 1U;
    expect(!scheduler.issue(intent(ui::TacticalVerb::stop, many)), "the per-command unit limit holds");
    expect(scheduler.pending() == 0, "refused intents queue nothing");
    expect(issued.empty(), "refused intents produce no issue-stage receipt");

    expect(static_cast<bool>(scheduler.issue(intent(ui::TacticalVerb::move, {9, 4, 9},
               point(3, 4), sim::invalid_entity_id, 0, ui::CommandOrigin::hud_button))),
        "a move is queued");
    expect(static_cast<bool>(scheduler.issue(intent(ui::TacticalVerb::attack, {4}, {}, 12, 0,
               ui::CommandOrigin::world_click))),
        "an attack is queued");
    const auto first = scheduler.take(17);
    expect(first.size() == 2 && scheduler.pending() == 0, "take drains the queue");
    if (first.size() == 2) {
        expect(first[0].key == sim::CommandKey{17, local_player, 40} && first[1].key == sim::CommandKey{17, local_player, 41},
            "take stamps (next tick, local player, sequence) in issue order");
        expect(first[0].units == std::vector<sim::EntityId>{4, 9}, "units are sorted and deduplicated");
        expect(first[0].payload == tactical::CommandPayload{tactical::MovePayload{point(3, 4)}}
                && first[1].payload == tactical::CommandPayload{tactical::AttackPayload{12}},
            "verbs map to rules-v1 payloads");
    }
    expect(scheduler.open_tick() == 18, "take closes its tick");
    static_cast<void>(scheduler.issue(intent(ui::TacticalVerb::stop, {4})));
    expect(scheduler.take(17).empty(), "an intent issued after take(17) never lands in tick 17");
    const auto second = scheduler.take(18);
    expect(second.size() == 1 && second[0].key == sim::CommandKey{18, local_player, 42},
        "it lands in the next tick, and sequences never repeat");
    static_cast<void>(scheduler.issue(intent(ui::TacticalVerb::stop, {4})));
    static_cast<void>(scheduler.issue(intent(ui::TacticalVerb::stop, {5})));
    const auto skipped = scheduler.take(25);
    expect(skipped.size() == 2 && skipped[0].key == sim::CommandKey{25, local_player, 43}
            && skipped[1].key == sim::CommandKey{25, local_player, 44},
        "commands for a tick the consumer skipped land in the tick it takes, in issue order");
    expect(scheduler.open_tick() == 26 && scheduler.next_sequence() == 45, "open tick and sequence advance");
    expect(issued == std::vector<std::pair<std::uint64_t, std::uint64_t>>{{40, 1}, {41, 1}, {42, 18}, {43, 19}, {44, 19}},
        "issue receipts retain original epochs while take restamps skipped commands");

    // The origin is presentation-only: identical orders from every origin give identical commands.
    std::vector<tactical::PlayerCommand> by_origin;
    for (const auto origin : {ui::CommandOrigin::hud_button, ui::CommandOrigin::hotkey, ui::CommandOrigin::world_click,
             ui::CommandOrigin::minimap}) {
        ui::CommandScheduler fresh(local_player);
        static_cast<void>(fresh.issue(intent(ui::TacticalVerb::stop, {5}, {}, sim::invalid_entity_id, 0, origin)));
        auto taken = fresh.take(3);
        if (taken.size() == 1) by_origin.push_back(std::move(taken[0]));
    }
    expect(by_origin.size() == 4 && std::all_of(by_origin.begin(), by_origin.end(),
                                        [&](const auto& c) { return c == by_origin.front(); }),
        "UI-C1: the origin never reaches the command");

    // UI-C1 and UI-C2 end to end, at 1, 2 and 4 workers.
    const sim::InlineExecutor inline_executor;
    const Run world = play(world_script(), inline_executor);
    const Run hud = play(hud_script(), inline_executor);
    const Run noisy = play(noisy_hud_script(), inline_executor);
    expect(world.stream.size() == 3, "the world script issues three commands");
    expect(hud.stream == world.stream, "UI-C1: HUD-issued and world-issued orders give equal command streams");
    expect(hud.hashes == world.hashes, "UI-C1: and equal state hashes every tick");
    expect(noisy.stream == hud.stream && noisy.hashes == hud.hashes,
        "UI-C2: selection, modes and refused orders never enter the stream or the hash");

    // Without the UI: the recorded replay alone reproduces every hash.
    auto replayed = tactical::TacticalSession::from_replay(world.replay);
    expect(static_cast<bool>(replayed) && world.replay.commands == world.stream,
        "UI-C1: the replay records exactly the sink's commands");
    if (replayed) {
        auto session = std::move(replayed).value();
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick < world.replay.final_tick_count; ++tick) {
            auto stepped = session.step(inline_executor);
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
        }
        expect(hashes == world.hashes, "UI-C1: replay hashes are unchanged with and without the UI");
    }
    for (const std::size_t workers : {1U, 2U, 4U}) {
        const platform::ThreadWorkerAdapter executor(workers);
        const Run threaded = play(noisy_hud_script(), executor);
        expect(threaded.stream == world.stream && threaded.hashes == world.hashes,
            "UI-C1/UI-C2 streams and hashes are identical at 1, 2 and 4 workers");
    }
}

// The live session (#80) steps on its own thread: the Godot main thread issues while the
// simulation thread takes at each tick boundary. One producer thread issues moves whose points
// encode their issue index while a consumer thread takes tick after tick. Every command must
// arrive exactly once, stamped for the tick that took it, in issue order, the stream must equal
// the one a single thread gives, and an intent issued after take(t) returned must land after t.
void command_scheduler_cross_thread() {
    constexpr std::uint64_t count = 20000;
    constexpr std::uint64_t first_sequence = 7;
    constexpr std::uint64_t first_tick = 3;
    const auto order = [](const std::uint64_t index) {
        return intent(ui::TacticalVerb::move, {static_cast<sim::EntityId>(1U + index % 3U)},
            point(static_cast<std::int64_t>(index), 0));
    };
    std::vector<tactical::PlayerCommand> expected;
    {
        ui::CommandScheduler serial(local_player, first_sequence, first_tick);
        for (std::uint64_t index = 0; index < count; ++index) static_cast<void>(serial.issue(order(index)));
        expected = serial.take(first_tick);
    }
    expect(expected.size() == count, "the serial reference stream is complete");
    for (int round = 0; round < 4; ++round) {
        ui::CommandScheduler scheduler(local_player, first_sequence, first_tick);
        // Every tick below `closed` has been taken.
        std::atomic<std::uint64_t> closed{first_tick};
        std::vector<std::uint64_t> earliest(count);
        std::vector<std::pair<std::uint64_t, std::vector<tactical::PlayerCommand>>> batches;
        std::thread consumer([&] {
            std::uint64_t received = 0;
            for (std::uint64_t tick = first_tick; received < count; ++tick) {
                auto batch = scheduler.take(tick);
                closed.store(tick + 1U, std::memory_order_release);
                received += batch.size();
                if (!batch.empty()) batches.emplace_back(tick, std::move(batch));
                if (tick % 64U == 0U) std::this_thread::yield();
            }
        });
        std::uint64_t refused = 0;
        for (std::uint64_t index = 0; index < count; ++index) {
            earliest[index] = closed.load(std::memory_order_acquire);
            if (!scheduler.issue(order(index))) ++refused;
            if (index % 97U == 0U) std::this_thread::yield();
        }
        consumer.join();

        std::vector<tactical::PlayerCommand> stream;
        bool stamped = true;
        bool after_close = true;
        std::uint64_t previous_tick = 0;
        for (auto& [tick, batch] : batches) {
            stamped = stamped && (stream.empty() || tick > previous_tick);
            previous_tick = tick;
            for (auto& command : batch) {
                stamped = stamped && command.key.tick == tick;
                const std::size_t index = stream.size();
                after_close = after_close && index < count && tick >= earliest[index];
                stream.push_back(std::move(command));
            }
        }
        expect(refused == 0 && stream.size() == count, "P2: every issued command is taken exactly once");
        expect(stamped, "P2: each batch is stamped for the tick that took it, ticks increasing");
        expect(after_close, "P2: an intent issued after take(t) returned never lands in tick t or earlier");
        bool same_order = stream.size() == expected.size();
        for (std::size_t index = 0; same_order && index < stream.size(); ++index) {
            same_order = stream[index].key.player_id == local_player
                && stream[index].key.sequence == expected[index].key.sequence
                && stream[index].units == expected[index].units && stream[index].payload == expected[index].payload;
        }
        expect(same_order, "P2: sequences and order equal the single-thread stream whatever the thread timing");
        expect(scheduler.pending() == 0 && scheduler.next_sequence() == first_sequence + count,
            "P2: nothing is left behind and no sequence is skipped");
    }
}

void input_routing_contracts() {
    using ui::InputClass;
    const ui::InputFocus unfocused{};
    const ui::InputFocus typing{false, true};
    const ui::InputFocus modal{true, false};
    const ui::InputFocus both{true, true};
    for (const auto input : {InputClass::key, InputClass::button_press, InputClass::button_release, InputClass::wheel,
             InputClass::motion}) {
        expect(ui::world_accepts(input, unfocused), "UI-I1: with no modal or text focus the world takes every unhandled event");
        expect(ui::world_accepts(input, modal) == (input == InputClass::button_release),
            "UI-I1: a modal dialog blocks the world except releases");
        expect(ui::world_accepts(input, both) == (input == InputClass::button_release),
            "UI-I1: modal and text focus together block the world except releases");
    }
    expect(!ui::world_accepts(InputClass::key, typing), "UI-I3: a focused edit box suppresses hotkeys and camera keys");
    for (const auto input : {InputClass::button_press, InputClass::button_release, InputClass::wheel, InputClass::motion}) {
        expect(ui::world_accepts(input, typing), "UI-I3: text focus leaves the pointer to the world");
    }

    ui::WorldPointerCapture capture;
    expect(!capture.active() && !ui::world_takes_before_gui(InputClass::motion, 0, capture),
        "without a world hold the GUI sees motion first");
    capture.pressed(2);
    capture.pressed(3);
    expect(capture.active() && capture.holds(2) && capture.holds(3) && !capture.holds(1), "holds track buttons");
    expect(ui::world_takes_before_gui(InputClass::motion, 0, capture), "a world hold takes motion before the GUI");
    expect(ui::world_takes_before_gui(InputClass::button_release, 2, capture), "and the release of a held button");
    expect(!ui::world_takes_before_gui(InputClass::button_release, 1, capture), "but not another button's release");
    expect(!ui::world_takes_before_gui(InputClass::button_press, 1, capture)
            && !ui::world_takes_before_gui(InputClass::wheel, 4, capture)
            && !ui::world_takes_before_gui(InputClass::key, 0, capture),
        "presses, wheel and keys always go to the GUI first");
    capture.released(2);
    expect(capture.active() && !capture.holds(2), "a release clears only its button");
    capture.released(3);
    expect(!capture.active(), "the hold ends with the last release");
    capture.pressed(0);
    capture.pressed(40);
    expect(!capture.active(), "out-of-range buttons are ignored");
    capture.pressed(1);
    capture.clear();
    expect(!capture.active(), "clear releases everything");

    ui::ModalHoldGuard guard;
    expect(!guard.changed(false) && !guard.open(), "no modal: nothing to cancel");
    expect(guard.changed(true) && guard.open(), "UI-I1: a modal opening cancels world holds");
    expect(!guard.changed(true), "only once while it stays open");
    expect(guard.changed(false) && !guard.open(), "UI-I1: and closing it cancels again");
    expect(!guard.changed(false), "then nothing until the next edge");
}

} // namespace

void paused_reservation_contracts() {
    ui::CommandScheduler scheduler(local_player);
    scheduler.present_snapshot(4);
    ui::TacticalIntent request; request.units = {1}; request.verb = ui::TacticalVerb::ability;
    request.unit_ability = tactical::AbilityKind::ion_cannon_shot; request.target = 2;
    expect(static_cast<bool>(scheduler.issue(request)), "paused activation reserves its ability");
    request.target = 3;
    expect(!scheduler.issue(request), "changing target cannot consume the same pending ability twice");
    scheduler.present_snapshot(6);
    expect(scheduler.consumed(request), "an untaken command cannot be acknowledged by an unrelated newer snapshot");
    const auto taken = scheduler.take(7);
    expect(taken.size() == 1 && scheduler.consumed(request), "handoff keeps presentation reservation");
    expect(scheduler.reservations()[0].execution_tick == 7,
        "offline handoff records its actual execution tick for live-ledger projection");
    scheduler.present_snapshot(7);
    expect(scheduler.consumed(request), "execution tick snapshot still predates its effect");
    scheduler.present_snapshot(8);
    expect(!scheduler.consumed(request), "next completed snapshot retires the restamped command");
    for (const auto verb : {ui::TacticalVerb::pad_build, ui::TacticalVerb::pad_sell,
                           ui::TacticalVerb::repair_hardpoint, ui::TacticalVerb::special_fire}) {
        ui::CommandScheduler sink(local_player); sink.present_snapshot(0);
        request.verb = verb; request.hardpoint = 0;
        expect(static_cast<bool>(sink.issue(request)) && !sink.issue(request), "one consumed entity action per snapshot");
    }
    ui::CommandScheduler selling(local_player); selling.present_snapshot(0);
    request.verb = ui::TacticalVerb::pad_sell;
    expect(static_cast<bool>(selling.issue(request)), "sale consumes child entity");
    request.verb = ui::TacticalVerb::guard;
    expect(!selling.issue(request), "repeated sale gesture cannot fall through into an order for sold child");
    ui::CommandScheduler lan(local_player, 0, 0, [] { return 2U; });
    lan.present_snapshot(2); request.verb = ui::TacticalVerb::ability;
    expect(static_cast<bool>(lan.issue(request)), "LAN ability admitted");
    static_cast<void>(lan.take(2)); lan.present_snapshot(3);
    expect(lan.consumed(request), "LAN producer epoch is not an execution receipt");
    lan.present_result(0);
    expect(!lan.consumed(request), "LAN command result releases reservation");

    expect(static_cast<bool>(lan.issue(request)), "LAN reservation for a lost result admitted");
    static_cast<void>(lan.take(3));
    const auto gap_revision = lan.reservation_revision();
    lan.present_event_gap(20);
    expect(lan.consumed(request), "a producer epoch is never a lost execution receipt");
    lan.note_execution(1, 25);
    lan.present_event_gap(25);
    expect(lan.consumed(request), "gap ends before the command completes");
    lan.present_event_gap(26);
    expect(!lan.consumed(request) && lan.reservation_revision() == gap_revision + 1,
        "lost completed result clears reservation and invalidates its projection");
    expect(static_cast<bool>(lan.issue(request)), "cleared LAN action can be retried");
    lan.present_event_gap(100);
    expect(lan.consumed(request), "lost gap preserves a newly queued command");

    tactical::EconomyView ledger; ledger.credits = sim::math::Fixed::from_raw(1000 * sim::math::Fixed::scale);
    ledger.pool = {11, 11}; ledger.pool_tokens = {40, 41}; ledger.population_cap = 3;
    tactical::BuildOption option; option.type = 11; option.price = sim::math::Fixed::from_raw(500 * sim::math::Fixed::scale);
    option.build_frames = 30; option.population = 2;
    auto option_of = [&](sim::EntityId, tactical::TypeId) { return &option; };
    ui::TacticalIntent buy; buy.verb = ui::TacticalVerb::buy; buy.units = {1}; buy.type = 11;
    ui::TacticalIntent drop; drop.verb = ui::TacticalVerb::reinforce; drop.type = 11;
    const std::vector<ui::PendingCommand> rows{{5, 0, buy}, {5, 1, drop}};
    const auto projected = ui::pending_economy(ledger, rows, option_of);
    expect(projected.credits.raw() == 500 * sim::math::Fixed::scale && projected.queues[0].size() == 1,
        "pending buy consumes money and queue capacity");
    expect(projected.queues[0][0].entry_id == 0, "pending buy cannot forge an authoritative cancel identity");
    auto campaign_ledger = ledger;
    campaign_ledger.pool = {11, 11, 11};
    campaign_ledger.pool_tokens = {40, 41, 42};
    campaign_ledger.reserves = {{40, 11, 2, false}, {41, 11, 5, false}, {42, 11, 3, true}};
    auto campaign_drop = drop;
    campaign_drop.reinforcement_token = 41;
    auto restricted_drop = drop;
    restricted_drop.reinforcement_token = 42;
    const std::vector<ui::PendingCommand> campaign_rows{
        {5, 0, campaign_drop}, {5, 1, campaign_drop}, {5, 2, restricted_drop}};
    const auto campaign_projection = ui::pending_economy(campaign_ledger, campaign_rows, option_of);
    expect(campaign_projection.population == 5 && campaign_projection.pool_tokens == std::vector<std::uint64_t>{40, 42}
        && campaign_projection.reserves == std::vector<tactical::ReserveView>{{40, 11, 2, false}, {42, 11, 3, true}},
        "campaign reservation consumes its persistent token and original population once, preserving restrictions");
    ui::TacticalIntent synthetic_cancel; synthetic_cancel.verb = ui::TacticalVerb::cancel;
    const auto next_sequence = scheduler.next_sequence();
    expect(!scheduler.issue(synthetic_cancel) && scheduler.next_sequence() == next_sequence,
        "zero cancel identity queues nothing and consumes no sequence");
    const std::vector<ui::PendingCommand> synthetic_cancels{{5, 0, synthetic_cancel}};
    const auto unchanged = ui::pending_economy(projected, synthetic_cancels, option_of);
    expect(unchanged.credits == projected.credits && unchanged.queues == projected.queues,
        "zero cancel identity cannot preview a refund for a pending purchase");

    tactical::ProductionCounts base;
    base.owned_player = 1; base.owned_allies = 2;
    base.current_player = 3; base.current_allies = 6;
    base.queued_allies = 1; base.lifetime_player = 4; base.lifetime_allies = 8;
    const auto pending_counts = ui::pending_production_counts(base, ledger, 5, rows, option_of, 11);
    expect(pending_counts.queued_player == 1 && pending_counts.queued_allies == 2
        && pending_counts.current_player == 4 && pending_counts.current_allies == 7
        && pending_counts.owned_player == 1 && pending_counts.owned_allies == 2
        && pending_counts.lifetime_player == 4 && pending_counts.lifetime_allies == 8,
        "pending purchase counts once and retains owned, pool, ally and lifetime counts");
    // Purchase-ahead: the atomic live read already contains the pending command.
    auto ahead = base; ++ahead.current_player; ++ahead.current_allies;
    ++ahead.queued_player; ++ahead.queued_allies;
    auto executed = rows;
    for (auto& row : executed) { row.handed_off = true; row.execution_tick = 5; }
    const auto ahead_counts = ui::pending_production_counts(ahead, projected, 6, executed, option_of, 11);
    expect(ahead_counts.current_player == pending_counts.current_player
        && ahead_counts.queued_player == pending_counts.queued_player
        && ahead_counts.current_allies == pending_counts.current_allies
        && ahead_counts.queued_allies == pending_counts.queued_allies,
        "world one tick ahead cannot double-count an executed pending purchase");
    // Completion-ahead: queue -> pool leaves current unchanged. The presented
    // ledger still has the queue entry, but must not be used with these live counts.
    auto finished = ahead; --finished.queued_player; --finished.queued_allies;
    auto finished_ledger = projected;
    finished_ledger.queues[0].clear(); finished_ledger.pool.push_back(11);
    const auto finished_counts = ui::pending_production_counts(finished, finished_ledger, 7, executed, option_of, 11);
    expect(finished_counts.current_player == ahead.current_player
        && finished_counts.current_allies == ahead.current_allies
        && finished_counts.queued_player == 0 && finished_counts.queued_allies == base.queued_allies,
        "completion ahead of presentation cannot overcount current player or allies");
    // The exact review arithmetic: presented current=5 and queue=2, world
    // current=5 and queue=1. No reservations; current must remain 5, not 6.
    tactical::ProductionCounts review_base;
    review_base.current_player = 5; review_base.current_allies = 5;
    review_base.queued_player = 1; review_base.queued_allies = 1;
    auto review_ledger = ledger;
    review_ledger.queues[0].push_back({11, 1, option.price, 30, 35, 90});
    const auto review_counts = ui::pending_production_counts(review_base, review_ledger, 7, {}, option_of, 11);
    expect(review_counts.current_player == 5 && review_counts.current_allies == 5,
        "F1 completion-ahead arithmetic remains at five");
    const auto future_counts = ui::pending_production_counts(base, ledger, 5, executed, option_of, 11);
    expect(future_counts.current_player == pending_counts.current_player,
        "execution at completed tick is still pending until the next completed step");
    auto cancel_ledger = projected;
    cancel_ledger.queues[0][0].entry_id = 90;
    ui::TacticalIntent count_cancel; count_cancel.verb = ui::TacticalVerb::cancel; count_cancel.queue_entry_id = 90;
    const std::vector<ui::PendingCommand> count_cancels{{6, 2, count_cancel, false, {}}};
    const auto cancelled_counts = ui::pending_production_counts(ahead, cancel_ledger, 6, count_cancels, option_of, 11);
    expect(cancelled_counts.current_player == base.current_player
        && cancelled_counts.queued_allies == base.queued_allies,
        "unexecuted cancellation replaces only the live local queue contribution");
    expect(projected.pool == std::vector<tactical::TypeId>{11} && projected.pool_tokens == std::vector<std::uint64_t>{41}
        && projected.population == 2, "pending drop consumes one pool entry and reserves its population");
    ledger.queues[0].push_back({11, 1, option.price, 30, 35, 90});
    ui::TacticalIntent cancel; cancel.verb = ui::TacticalVerb::cancel; cancel.queue_entry_id = 90;
    const std::vector<ui::PendingCommand> cancels{{5, 0, cancel}};
    const auto refunded = ui::pending_economy(ledger, cancels, option_of);
    expect(refunded.queues[0].empty() && refunded.credits.raw() == 1500 * sim::math::Fixed::scale,
        "pending cancel hides exact identity and previews one refund");
    ui::CommandScheduler cancelling(local_player); cancelling.present_snapshot(0);
    expect(static_cast<bool>(cancelling.issue(cancel)) && !cancelling.issue(cancel), "duplicate cancellation reserves no second refund");
    request.verb = ui::TacticalVerb::move;
    expect(static_cast<bool>(cancelling.issue(request)) && static_cast<bool>(cancelling.issue(request)), "paused replacement orders stay legal");
}

int main() {
    {
        ui::CommandScheduler scheduler(local_player, 7, 10);
        auto fire = intent(ui::TacticalVerb::special_fire, {91});
        fire.target = 92;
        expect(static_cast<bool>(scheduler.issue(fire)), "WSW-31: special click schedules a source without selecting it");
        const auto commands = scheduler.take(10);
        expect(commands.size() == 1 && commands[0].key.player_id == local_player
            && commands[0].key.sequence == 7 && commands[0].units == std::vector<sim::EntityId>{91}
            && std::get<tactical::SpecialFirePayload>(commands[0].payload)
                == tactical::SpecialFirePayload{92, tactical::SpecialFireAdmission::player},
            "WSW-31: player admission and object identity reach the replay scheduler");
        fire.target = 0;
        expect(!scheduler.issue(fire), "WSW-31: empty target queues no shot");
        fire.target = 92; fire.units = {91, 93};
        expect(!scheduler.issue(fire) && scheduler.next_sequence() == 8, "WSW-31: multiple sources use no sequence");
        const auto one = sim::math::Fixed::from_raw(sim::math::Fixed::scale);
        const auto half = sim::math::Fixed::from_raw(sim::math::Fixed::scale / 2);
        using State = ui::SpecialWeaponAvailability;
        expect(ui::special_weapon_pick_target(8, 74) == 74,
            "MC-10: special targeting keeps the bomber craft instead of its squadron container");
        expect(ui::special_weapon_pick_target(7, sim::invalid_entity_id) == 7,
            "MC-10: an ordinary valid target keeps its identity");
        expect(ui::special_weapon_pick_target(8, sim::invalid_entity_id, true) == sim::invalid_entity_id
            && ui::special_weapon_pick_target(8, 74, true) == sim::invalid_entity_id,
            "MC-11: an icon refuses both its container and a world craft underneath it");
        expect(ui::special_weapon_availability(false, true, true, one) == State::unavailable,
            "WSW-28: setup disables a ready source");
        expect(ui::special_weapon_availability(true, false, true, one) == State::unavailable,
            "WSW-28: missing station/source disables the button");
        expect(ui::special_weapon_availability(true, true, false, one) == State::unavailable,
            "WSW-12: ready does not replace purchase permission");
        expect(ui::special_weapon_availability(true, true, true, {}) == State::purchased,
            "WSW-28: enabled but unavailable manual gun cannot activate");
        expect(ui::special_weapon_availability(true, true, true, half) == State::charging,
            "WSW-27: partial recharge disables activation");
        expect(ui::special_weapon_availability(true, true, true, one) == State::ready,
            "WSW-16: full readiness permits activation");
        ui::SpecialWeaponButton button;
        expect(button.dial() == 1.0 && !button.enabled(), "WSW-29: zero dial and disabled are independent");
        button.readiness = half;
        expect(button.dial() == 0.5, "WSW-29: positive readiness reaches the dial unchanged");
        const std::vector<tactical::SnapshotPlayer> players{{1, 1, false}, {2, 2, true}, {3, 1, false}, {4, 2, false}};
        tactical::TacticalInstance source, target;
        source.entity_id = 91; source.owner = 1;
        source.planetary_weapon = tactical::TacticalInstance::PlanetaryWeaponView{};
        source.planetary_weapon->valid_targets = 6;
        target.entity_id = 92; target.owner = 2; target.category_bits = 2;
        const auto valid = [&] {
            const auto admitted = tactical::manual_special_fire_admission(players, source, target, false);
            return admitted && admitted.value().has_value();
        };
        expect(valid(), "WSW-13: planetary HUD admission includes non-allied neutral targets");
        target.owner = 4;
        expect(valid(), "WSW-13: planetary HUD admission includes ordinary enemies");
        target.owner = 3;
        expect(!valid(), "WSW-13: planetary HUD admission rejects allies");
        target.owner = 2; target.category_bits = 1;
        expect(!valid(), "WSW-13: planetary HUD admission rejects disjoint categories");
        target.category_bits = 2; target.durability.emplace();
        expect(!valid(), "WSW-13: planetary HUD admission rejects dead targets");
        target.durability.reset(); target.entity_id = source.entity_id;
        expect(!valid(), "WSW-13: planetary HUD admission rejects its source");
    }
    {
        ui::CommandScheduler scheduler(local_player);
        ui::OrderInput input(scheduler);
        input.set_selection({1});
        auto issued = input.world_command(at(point(100, 0)), ui::CommandOrigin::world_click,
            ui::OrderModifiers{false, false, true});
        expect(issued && issued.value(), "WHZ-08a: right double click issues a movement order");
        auto commands = scheduler.take(0);
        const auto* move = commands.size() == 1 ? std::get_if<tactical::MovePayload>(&commands[0].payload) : nullptr;
        expect(move && move->through_hazards, "WHZ-08a: double click survives input and command scheduling");
        tactical::TacticalReplay replay{setup(), 1, commands};
        auto bytes = tactical::write_replay(replay);
        expect(static_cast<bool>(bytes), "WHZ-08a: explicit hazard movement has a valid replay encoding");
        if (bytes) {
            auto parsed = tactical::parse_replay(bytes.value());
            expect(parsed && parsed.value() == replay, "WHZ-08a: hazard movement replay round trips");
        }
        const auto script = [](const bool through) {
            return Script{
                {0, [through](ui::OrderInput& in) {
                    in.set_selection({1});
                    const auto result = in.world_command(at(point(100, 0)), ui::CommandOrigin::world_click,
                        ui::OrderModifiers{false, false, through});
                    expect(result && result.value(), "WHZ-08a: movement submits for canonical flag contract");
                }},
                {2, [](ui::OrderInput& in) { check(in, in.stop(ui::CommandOrigin::hotkey), "WHZ-08a: stop replaces the prior movement flag"); }},
            };
        };
        const sim::InlineExecutor executor;
        const auto ordinary = play(script(false), executor, 3);
        const auto forced = play(script(true), executor, 3);
        expect(ordinary.hashes.size() == 3 && forced.hashes.size() == 3
            && ordinary.hashes[0] != forced.hashes[0] && ordinary.hashes[2] == forced.hashes[2],
            "WHZ-08a: MVHZ binds a retained flag even without motion and disappears on replacement");
        issued = input.world_command(at(point(100, 0), 10, true), ui::CommandOrigin::world_click,
            ui::OrderModifiers{false, false, true});
        commands = scheduler.take(1);
        expect(issued && commands.size() == 1 && std::holds_alternative<tactical::AttackPayload>(commands[0].payload),
            "WHZ-08a: double click on an enemy retains the ordinary attack command");
    }
    {
        auto sell = intent(ui::TacticalVerb::pad_sell, {91});
        ui::CommandScheduler scheduler(local_player);
        expect(static_cast<bool>(scheduler.issue(sell)), "WBP-30 sale schedules one selected child");
        const auto commands = scheduler.take(0);
        expect(commands.size() == 1 && commands.front().units == std::vector<sim::EntityId>{91}
            && std::holds_alternative<tactical::PadSellPayload>(commands.front().payload),
            "WBP-30 sale reaches the recorded command stream");
        sell.units.clear();
        expect(!scheduler.issue(sell), "sale requires a child");
        sell.units = {91, 92};
        expect(!scheduler.issue(sell), "sale refuses multiple children");
    }
    {
        auto build = intent(ui::TacticalVerb::pad_build, {91});
        build.type = 123;
        const auto payload = ui::command_payload(build);
        expect(payload && std::holds_alternative<tactical::PadBuildPayload>(payload.value()),
            "WBP-36 palette dispatches the pad build payload");
        if (payload) {
            const auto* pad = std::get_if<tactical::PadBuildPayload>(&payload.value());
            expect(pad && pad->type == 123, "UC type survives dispatch");
        }
        ui::CommandScheduler scheduler(local_player);
        expect(static_cast<bool>(scheduler.issue(build)), "the palette request queues");
        const auto commands = scheduler.take(0);
        expect(commands.size() == 1 && commands.front().units == std::vector<sim::EntityId>{91},
            "the parent pad survives dispatch to the replay command");
        build.units.push_back(92);
        expect(!scheduler.issue(build), "a pad request cannot become a multi-station purchase");
    }

    {
        ui::CommandScheduler scheduler(local_player);
        ui::OrderInput input(scheduler);
        input.set_selection({1});
        input.arm(ui::OrderMode::waypoint);
        expect(input.world_command(at(point(200, 300), 10, true)).value()
            && input.mode() == ui::OrderMode::waypoint, "HCB-57: hostile point placement retains waypoint mode");
        expect(input.world_command(at(point(400, 300)), ui::CommandOrigin::minimap).value(),
            "HCB-57: minimap point placement shares the waypoint command path");
        const auto commands = scheduler.take(0);
        expect(commands.size() == 2 && std::get<tactical::MovePayload>(commands[0].payload).append
            && std::get<tactical::MovePayload>(commands[1].payload).append,
            "HCB-60: persistent waypoint input records append moves");
        expect(input.world_command(at(point(500, 300)), ui::CommandOrigin::world_click, {true, true, false}).value(),
            "HCB-57: Ctrl+Alt suppresses queuing even while waypoint is armed");
        expect(std::holds_alternative<tactical::GuardPayload>(scheduler.take(1)[0].payload)
            && input.mode() == ui::OrderMode::waypoint, "HCB-57: guard modifier preserves the armed waypoint toggle");
        input.cancel_mode();
        expect(input.world_command(at(point(500, 300)), ui::CommandOrigin::world_click, {false, true, false}).value(),
            "HCB-57: Alt alone appends point movement");
        expect(std::get<tactical::MovePayload>(scheduler.take(2)[0].payload).append,
            "HCB-57: Alt queue modifier survives the scheduler");
        expect(input.world_command(at(point(500, 300)), ui::CommandOrigin::world_click, {true, true, false}).value(),
            "HCB-57: Ctrl+Alt remains guard");
        expect(std::holds_alternative<tactical::GuardPayload>(scheduler.take(3)[0].payload),
            "HCB-57: guard suppresses append movement");
    }
    command_sink_contracts();
    {
        auto barrage = intent(ui::TacticalVerb::ability, {91});
        barrage.unit_ability = tactical::AbilityKind::barrage;
        barrage.ability_action = tactical::AbilityAction::activate;
        barrage.destination = {eawr::sim::math::Fixed::from_raw(17), eawr::sim::math::Fixed::from_raw(-31), {}};
        const auto payload = ui::command_payload(barrage);
        const auto* area = payload ? std::get_if<tactical::AreaAbilityPayload>(&payload.value()) : nullptr;
        expect(area && area->ability == tactical::AbilityKind::barrage && area->point == barrage.destination,
            "WAD-38: BARRAGE world point becomes the admitted area command without an object target");
        barrage.ability_action = tactical::AbilityAction::deactivate;
        const auto ended = ui::command_payload(barrage);
        const auto* ability = ended ? std::get_if<tactical::AbilityPayload>(&ended.value()) : nullptr;
        expect(ability && ability->ability == tactical::AbilityKind::barrage
            && ability->action == tactical::AbilityAction::deactivate && !ability->position,
            "WAD-38: BARRAGE cancellation retains its ordinary ability command");
    }
    for (const auto point_kind : {tactical::AbilityKind::weaken_enemy, tactical::AbilityKind::fow_reveal_ping}) {
        auto weaken = intent(ui::TacticalVerb::ability, {91});
        weaken.unit_ability = point_kind;
        weaken.ability_action = tactical::AbilityAction::activate;
        weaken.destination = {eawr::sim::math::Fixed::from_raw(17), eawr::sim::math::Fixed::from_raw(-31), {}};
        const auto payload = ui::command_payload(weaken);
        const auto* ability = payload ? std::get_if<tactical::AbilityPayload>(&payload.value()) : nullptr;
        expect(ability && ability->position == weaken.destination && ability->target == 0,
            "weaken/ping world point survives the command sink without an object target");
        weaken.ability_action = tactical::AbilityAction::deactivate;
        const auto ended = ui::command_payload(weaken);
        expect(ended && !std::get<tactical::AbilityPayload>(ended.value()).position,
            "a non-activation carries no world point");
    }
    {
        ui::CommandScheduler scheduler(local_player);
        auto repair = intent(ui::TacticalVerb::repair_hardpoint, {91});
        repair.hardpoint = 3;
        expect(static_cast<bool>(scheduler.issue(repair)), "WSL-40: repair click schedules one station");
        const auto commands = scheduler.take(0);
        expect(commands.size() == 1 && commands[0].key.player_id == local_player
            && commands[0].units == std::vector<sim::EntityId>{91}
            && std::get<tactical::RepairHardpointPayload>(commands[0].payload).hardpoint == 3,
            "WSL-40: scheduler stamps the station, index and paying player for replay");
        repair.units = {91, 92};
        expect(!scheduler.issue(repair), "WSL-40: repair rejects multiple stations");
        repair.units = {91}; repair.hardpoint = tactical::attack_hull;
        expect(!scheduler.issue(repair), "WSL-40: repair requires a hardpoint rather than hull");
    }
    // #2328: exercise the viewer's dispatch helper with a buffered engine. An
    // engaged optional containing an empty standalone scope must NOT flush.
    {
        struct BufferedInput {
            ui::CommandScheduler& scheduler;
            std::vector<ui::TacticalIntent> events;
            unsigned flushes{};
            void parse_input_event(const ui::TacticalIntent& event) { events.push_back(event); }
            void flush_buffered_events() {
                ++flushes;
                for (const auto& event : events) expect(static_cast<bool>(scheduler.issue(event)), "scripted order accepted");
                events.clear();
            }
        };
        ui::CommandScheduler standalone(local_player);
        BufferedInput engine{standalone, {}, 0};
        std::optional<ui::CommandScheduler::IntentEpochScope> epoch;
        epoch.emplace(standalone.capture_intent_epoch());
        presentation::godot_backend::dispatch_scripted_input(engine, intent(ui::TacticalVerb::move, {1}, point(50, 60)), epoch);
        expect(engine.flushes == 0 && standalone.pending() == 0,
            "standalone scripted input remains buffered during replay");
        expect(standalone.take(4).empty(), "standalone closes the current tick before normal input dispatch");
        epoch.reset();
        engine.flush_buffered_events(); // The engine's next normal dispatch boundary.
        const auto commands = standalone.take(5);
        expect(commands.size() == 1 && commands[0].key.tick == 5 && commands[0].key.sequence == 0,
            "standalone scripted input keeps its previous next-boundary command stamp");

        ui::CommandScheduler lan(local_player, 0, 0, [] { return 4U; });
        BufferedInput lan_engine{lan, {}, 0};
        epoch.emplace(lan.capture_intent_epoch());
        presentation::godot_backend::dispatch_scripted_input(lan_engine, intent(ui::TacticalVerb::move, {1}, point(50, 60)), epoch);
        expect(lan_engine.flushes == 1 && lan.pending() == 1,
            "LAN scripted input dispatches while its epoch is held");
        const auto lan_commands = lan.take(5);
        expect(lan_commands.size() == 1 && lan_commands[0].key.tick == 4,
            "LAN scripted input retains its captured producer epoch");
        epoch.reset();
        expect(!lan.pending_intent_epoch(), "scripted gesture releases the LAN epoch");
    }
    paused_reservation_contracts();
    command_scheduler_cross_thread();
    input_routing_contracts();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " UI input/command contract(s) failed\n";
        return 1;
    }
    std::cout << "UI input and command contracts passed\n";
    return 0;
}
