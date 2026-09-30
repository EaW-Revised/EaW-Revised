// UI-07 (#304): the command sink (UI-C1, UI-C2) and the input routing policy (UI-I1, UI-I3).
// The Godot dispatch half of UI-I1 to UI-I3 runs in the viewer
// (tests/presentation/renderer/test_input_routing.py).

#include "eawr/platform/sim_workers.hpp"
#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/presentation/ui/input_routing.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "ui_test_support.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
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
    std::vector<sim::EntityId> many(tactical::max_units_per_command + 1U);
    for (std::size_t i = 0; i < many.size(); ++i) many[i] = i + 1U;
    expect(!scheduler.issue(intent(ui::TacticalVerb::stop, many)), "the per-command unit limit holds");
    expect(scheduler.pending() == 0, "refused intents queue nothing");

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

int main() {
    command_sink_contracts();
    command_scheduler_cross_thread();
    input_routing_contracts();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " UI input/command contract(s) failed\n";
        return 1;
    }
    std::cout << "UI input and command contracts passed\n";
    return 0;
}
