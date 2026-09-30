// Script commands as next-tick replay input (#247, docs/lua-sandbox.md
// "Command routing"): a tactical session whose scripts order units alongside a
// local player's UI-07 input.
//
//   lua_bridge_tests routing | workers

#include "eawr/platform/sim_workers.hpp"
#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/script/authoritative/tactical_bridge.hpp"
#include "eawr/script/numeric/q24_boundary.hpp"

#include "harness.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace eawr::script::authoritative::test;
namespace tactical = eawr::sim::tactical;
namespace ui = eawr::presentation::ui;
using eawr::core::Result;

constexpr int total_ticks = 60;

// Combined state hash after the last tick, the same on every target and worker
// count: a change means the world rules, the script semantics, the numeric
// profile or an encoding changed.
constexpr std::string_view golden_final_hash = "5c75f0c1e68cef5ef7b93f41ee05262c2185000d8c4712f192f50145b4fad2e2";

// Each instance commands the player with its ID. Numbers go through parsing,
// formatting and division before they reach the Q24 boundary.
constexpr char commander[] = R"LUA(
Mine = {[1] = {1, 2}, [2] = {3, 4}}
Enemy = {[1] = 3, [2] = 1}

function Main()
    local me = Self()
    while true do
        local unit = Mine[me][Test_Random(2) + 1]
        local x = tonumber(string.format("%.17g", (Test_Unit() * 1000 - 500) / 7))
        Order("move", me, unit, x, x / 3)
        if Test_Random(5) == 0 then Order("attack", me, unit, Enemy[me]) end
        if Test_Random(7) == 0 then Order("bogus", me, unit) end
        if Test_Random(11) == 0 then Order("move", 9, unit, 0, 0) end
        if Test_Random(13) == 0 then Order("move", me, unit, 1e30, 0) end
        coroutine.yield(true)
    end
end

Create_Thread("Main")
)LUA";

eawr::sim::math::Fixed whole_fixed(std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); }

tactical::TacticalSetup battle_setup() {
    tactical::TacticalSetup setup;
    setup.seed = 0xB41D6E;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    for (std::uint64_t id = 1; id <= 4; ++id) {
        tactical::UnitState unit;
        unit.entity_id = id;
        unit.type_id = 100;
        unit.owner = id <= 2 ? 1 : 2;
        unit.position = {whole_fixed(static_cast<std::int64_t>(id) * 100), whole_fixed(0), whole_fixed(0)};
        setup.units.push_back(unit);
    }
    return setup;
}

eawr::core::Diagnostic translation_error(std::string message) {
    eawr::core::Diagnostic diagnostic;
    diagnostic.code = "EAWR-TEST-0001";
    diagnostic.message = std::move(message);
    return diagnostic;
}

Result<LuaNumber> number_argument(const auth::ValueList& arguments, std::size_t index) {
    const auto* number = index < arguments.size() ? std::get_if<LuaNumber>(&arguments[index].data) : nullptr;
    if (number == nullptr) return Result<LuaNumber>::failure(translation_error("number expected at " + std::to_string(index)));
    return Result<LuaNumber>::success(*number);
}

Result<eawr::sim::math::Fixed> fixed_argument(const auth::ValueList& arguments, std::size_t index) {
    auto number = number_argument(arguments, index);
    if (!number) return Result<eawr::sim::math::Fixed>::failure(number.error());
    // Rounds to the nearest Q24 value; infinite or out-of-range values fail (EAWR-SCRIPT-0101/0102).
    return eawr::script::numeric::to_fixed(number.value());
}

// A player or entity id: an exact integer in [1, max] (EAWR-SCRIPT-0101/0103 otherwise).
Result<std::uint64_t> id_argument(const auth::ValueList& arguments, std::size_t index, std::uint64_t max) {
    auto number = number_argument(arguments, index);
    if (!number) return Result<std::uint64_t>::failure(number.error());
    auto value = eawr::script::numeric::to_exact_integer(number.value());
    if (!value) return Result<std::uint64_t>::failure(value.error());
    if (value.value() < 1 || static_cast<std::uint64_t>(value.value()) > max) {
        return Result<std::uint64_t>::failure(translation_error("id out of range at " + std::to_string(index)));
    }
    return Result<std::uint64_t>::success(static_cast<std::uint64_t>(value.value()));
}

constexpr std::uint64_t max_player = std::numeric_limits<tactical::PlayerId>::max();
constexpr std::uint64_t max_entity = std::numeric_limits<std::int64_t>::max();

// The shape #79's translators follow: arity, types and ranges are checked and
// every failure is returned, never thrown.
Result<auth::TacticalOrder> translate_order(const auth::ScriptCommand& command, bool attack) {
    using Order = Result<auth::TacticalOrder>;
    const auth::ValueList& arguments = command.arguments;
    if (arguments.size() != (attack ? 3U : 4U)) return Order::failure(translation_error("wrong argument count"));
    auto issuer = id_argument(arguments, 0, max_player);
    if (!issuer) return Order::failure(issuer.error());
    auto unit = id_argument(arguments, 1, max_entity);
    if (!unit) return Order::failure(unit.error());
    auth::TacticalOrder order;
    order.issuer = static_cast<tactical::PlayerId>(issuer.value());
    order.units = {unit.value()};
    if (attack) {
        auto target = id_argument(arguments, 2, max_entity);
        if (!target) return Order::failure(target.error());
        order.payload = tactical::AttackPayload{target.value()};
    } else {
        auto x = fixed_argument(arguments, 2);
        if (!x) return Order::failure(x.error());
        auto z = fixed_argument(arguments, 3);
        if (!z) return Order::failure(z.error());
        order.payload = tactical::MovePayload{{x.value(), whole_fixed(0), z.value()}};
    }
    return Order::success(std::move(order));
}

Result<auth::TacticalOrder> translate_move(const auth::ScriptCommand& command) { return translate_order(command, false); }
Result<auth::TacticalOrder> translate_attack(const auth::ScriptCommand& command) { return translate_order(command, true); }

// Translators that break the contract; the bridge still drops their commands.
Result<auth::TacticalOrder> translate_throwing(const auth::ScriptCommand& command) {
    return translate_move({command.tick, command.issuer, command.sequence, command.verb, {command.arguments.at(9)}});
}
Result<auth::TacticalOrder> translate_throwing_other(const auth::ScriptCommand&) { throw 7; }

auth::ScriptedTacticalSession make_battle(const char* script = commander) {
    Session scripts = make_session({{"Commander.lua", script}});
    expect(scripts.scheduler
               ->register_binding("Self",
                   [](auth::BindingContext& context, const auth::ValueList&) {
                       return Result<auth::ValueList>::success(
                           {auth::Value::number(integer(static_cast<std::int64_t>(context.instance())))});
                   })
               .has_value(),
           "register Self");
    expect(scripts.scheduler
               ->register_binding("Order",
                   [](auth::BindingContext& context, const auth::ValueList& arguments) {
                       const auto* verb = std::get_if<std::string>(&arguments.at(0).data);
                       context.issue_command(verb == nullptr ? std::string() : *verb,
                           auth::ValueList(arguments.begin() + 1, arguments.end()));
                       return Result<auth::ValueList>::success({});
                   })
               .has_value(),
           "register Order");
    for (const std::uint64_t id : {1U, 2U}) {
        expect(scripts.scheduler->create_instance(id, "Commander.lua").has_value(), "create commander");
    }
    auto world = tactical::TacticalSession::create(battle_setup());
    expect(world.has_value(), "world create: " + (world ? std::string() : world.error().message));
    auto battle = auth::ScriptedTacticalSession::create(std::move(world).value(), std::move(*scripts.scheduler));
    expect(battle.has_value(), "scripted session create");
    expect(battle.value().register_verb("move", translate_move).has_value(), "register move");
    expect(battle.value().register_verb("attack", translate_attack).has_value(), "register attack");
    expect(battle.value().register_verb("throw", translate_throwing).has_value(), "register throw");
    expect(battle.value().register_verb("throw_other", translate_throwing_other).has_value(), "register throw_other");
    expect(!battle.value().register_verb("move", translate_move).has_value(), "a verb registers once");
    return std::move(battle).value();
}

struct Run {
    std::vector<std::string> hashes;       // combined, per tick
    std::vector<std::string> world_hashes; // world only, per tick
    tactical::TacticalReplay replay;
    std::size_t script_commands{};
    std::size_t submitted{};
    std::size_t dropped{};
    std::size_t ui_commands{};
    std::vector<std::string> drop_codes;
};

// The local player (1) gives UI-07 orders on some ticks; they are taken at the
// tick boundary, as the live session's command source does.
Run run_battle(const eawr::sim::PartitionExecutor& executor, std::shared_ptr<eawr::sim::StateHasher> hasher = nullptr) {
    auth::ScriptedTacticalSession battle = make_battle();
    const bool hashed_off_thread = hasher != nullptr;
    battle.set_state_hasher(std::move(hasher));
    ui::CommandScheduler local(1);
    Run run;
    std::vector<auth::ScriptCommand> previous_service;
    for (int index = 0; index < total_ticks; ++index) {
        const std::uint64_t tick = battle.completed_tick();
        if (tick % 4 == 1) {
            ui::TacticalIntent intent;
            intent.verb = tick % 8 == 1 ? ui::TacticalVerb::stop : ui::TacticalVerb::move;
            intent.units = {2, 1};
            intent.destination = {whole_fixed(static_cast<std::int64_t>(tick)), whole_fixed(0), whole_fixed(5)};
            expect(local.issue(intent).has_value(), "UI intent");
        }
        const std::vector<tactical::PlayerCommand> input = local.take(tick);
        run.ui_commands += input.size();
        auto stepped = battle.step(executor, input);
        expect(stepped.has_value(), "step " + std::to_string(tick) + (stepped ? "" : ": " + stepped.error().message));
        if (!stepped) break;
        const auth::ScriptedTick& result = stepped.value();
        expect(result.refused_input.empty(), "UI input is accepted next to script commands of the same player");
        // The previous service's commands are this tick's input, in commit order, keyed for this tick.
        expect(result.script_input.size() == previous_service.size(), "every script command is routed once");
        for (std::size_t position = 0; position < result.script_input.size(); ++position) {
            const auth::RoutedCommand& routed = result.script_input[position];
            expect(routed.command.issuer == previous_service[position].issuer &&
                       routed.command.sequence == previous_service[position].sequence,
                   "script input keeps commit order");
            if (routed.submitted) {
                ++run.submitted;
                expect(routed.key.tick == tick, "a script command executes in the tick after its service");
            } else {
                ++run.dropped;
                run.drop_codes.push_back(routed.dropped.code);
            }
        }
        previous_service = result.scripts.commands;
        run.script_commands += result.scripts.commands.size();
        expect(result.scripts.diagnostics.empty(), "script diagnostics");
        // #637: off the stepping thread the hashes are only in state_hash; on it, in both.
        expect(hashed_off_thread ? result.state_sha256.empty() && result.world.state_sha256.empty()
                                 : result.state_hash.get() == result.state_sha256
                                     && result.world.state_hash.get() == result.world.state_sha256,
            "the tick's hashes are where the hashing mode puts them");
        run.hashes.push_back(result.state_hash.get());
        run.world_hashes.push_back(result.world.state_hash.get());
    }
    expect(battle.pending_script_commands() == previous_service.size(), "the last service waits for the next tick");
    run.replay = battle.record();
    return run;
}

void run_routing() {
    const eawr::sim::InlineExecutor inline_executor;
    const Run run = run_battle(inline_executor);
    std::cout << "battle: " << run.script_commands << " script commands, " << run.submitted << " submitted, "
              << run.dropped << " dropped, " << run.ui_commands << " UI commands, final hash " << run.hashes.back() << '\n';
    expect(run.submitted > 50 && run.dropped > 5 && run.ui_commands > 5, "the battle exercises every path");
    for (const std::string& code : run.drop_codes) expect(code == auth::codes::command_unroutable, "drops are EAWR-SCRIPT-0214");

    // The replay holds exactly the accepted UI and script commands.
    expect(run.replay.commands.size() == run.ui_commands + run.submitted, "the record holds every accepted command once");
    std::size_t scripted_keys = 0;
    for (const auto& command : run.replay.commands) {
        if (command.key.player_id == 2) ++scripted_keys;
    }
    expect(scripted_keys > 0, "player 2 is commanded only by its script");

    // Headless: the recorded replay reproduces every world tick without a script.
    auto headless = tactical::TacticalSession::from_replay(run.replay);
    expect(headless.has_value(), "headless replay loads");
    if (headless) {
        std::vector<std::string> world_hashes;
        while (headless.value().completed_tick() < run.replay.final_tick_count) {
            auto tick = headless.value().step(inline_executor);
            expect(tick.has_value(), "headless step");
            if (!tick) break;
            world_hashes.push_back(tick.value().state_sha256);
        }
        expect(world_hashes == run.world_hashes, "headless replay equals the scripted run tick by tick");
    }
    // A rerun reproduces it; the combined hash differs from the world hash.
    const Run again = run_battle(inline_executor);
    expect(again.hashes == run.hashes && again.replay == run.replay, "rerun reproduces the battle");
    expect(run.hashes.back() != run.world_hashes.back(), "the combined hash covers the scripts");
    if (!golden_final_hash.empty()) expect(run.hashes.back() == golden_final_hash, "golden final combined hash");

    // Construction rules.
    {
        Session scripts = make_session({{"Commander.lua", commander}});
        const eawr::sim::InlineExecutor executor;
        expect(scripts.scheduler->service(executor).has_value(), "advance the scripts one tick");
        auto world = tactical::TacticalSession::create(battle_setup());
        auto mismatched = auth::ScriptedTacticalSession::create(std::move(world).value(), std::move(*scripts.scheduler));
        expect(!mismatched && mismatched.error().code == auth::codes::invalid_request, "world and script ticks must agree");
    }
    {
        auth::ScriptedTacticalSession battle = make_battle();
        const eawr::sim::InlineExecutor executor;
        expect(battle.step(executor).has_value(), "first step");
        expect(!battle.register_verb("stop", translate_move).has_value(), "no verb after the first step");
    }
}

// Malformed script commands: every translation failure, returned or thrown, is
// a clean EAWR-SCRIPT-0214 drop that leaves the world and the issuer's
// sequence as if the command had never been issued.
constexpr char clean_orders[] = R"LUA(
function Main()
    local me = Self()
    coroutine.yield(true)
    Order("move", me, me * 2, 50, 25)
    coroutine.yield(true)
    Order("attack", me, me * 2 - 1, 5 - me * 2)
    Order("move", me, me * 2, -30, 0)
    while true do coroutine.yield(true) end
end
Create_Thread("Main")
)LUA";

constexpr char malformed_orders[] = R"LUA(
function Main()
    local me = Self()
    coroutine.yield(true)
    Order("move")
    Order("move", me)
    Order("move", me, me * 2, 50)
    Order("move", me, me * 2, 50, 25, 1)
    Order("move", "one", me * 2, 50, 25)
    Order("move", me, {me * 2}, 50, 25)
    Order("move", me, me * 2, "50", 25)
    Order("move", me, me * 2, 50, 25)
    Order("move", me, me * 2, 0 / 0, 25)
    Order("move", me, me * 2, 50, 1 / 0)
    Order("move", me, me * 2, -1e30, 25)
    Order("move", me, 2.5, 50, 25)
    Order("move", me, 0, 50, 25)
    Order("move", me, -1, 50, 25)
    Order("move", me, 1e300, 50, 25)
    Order("move", 0 / 0, me * 2, 50, 25)
    Order("move", 1099511627776, me * 2, 50, 25)
    Order("move", 9, me * 2, 50, 25)
    coroutine.yield(true)
    Order("attack", me, me * 2 - 1)
    Order("attack", me, me * 2 - 1, nil)
    Order("attack", me, me * 2 - 1, 0)
    Order("attack", me, me * 2 - 1, 5 - me * 2)
    Order("attack", me, me * 2 - 1, 1 / 0)
    Order("throw", me)
    Order("throw_other", me)
    Order("move", me, me * 2, -30, 0)
    Order("bogus", me, me * 2)
    while true do coroutine.yield(true) end
end
Create_Thread("Main")
)LUA";

void run_malformed() {
    const eawr::sim::InlineExecutor executor;
    auth::ScriptedTacticalSession clean = make_battle(clean_orders);
    auth::ScriptedTacticalSession malformed = make_battle(malformed_orders);
    std::size_t submitted = 0;
    std::size_t dropped = 0;
    for (int index = 0; index < 6; ++index) {
        auto expected = clean.step(executor);
        auto got = malformed.step(executor);
        expect(expected.has_value() && got.has_value(),
            "malformed step " + std::to_string(index) + (got ? "" : ": " + got.error().code + " " + got.error().message));
        if (!expected || !got) return;
        expect(got.value().scripts.diagnostics.empty(), "malformed commands raise no script diagnostics: " +
            (got.value().scripts.diagnostics.empty() ? std::string() : got.value().scripts.diagnostics.front().message));
        expect(got.value().world.state_sha256 == expected.value().world.state_sha256,
            "tick " + std::to_string(index) + ": drops leave the world as if never issued");
        std::vector<eawr::sim::CommandKey> kept;
        for (const auth::RoutedCommand& routed : got.value().script_input) {
            if (routed.submitted) {
                ++submitted;
                kept.push_back(routed.key);
            } else {
                ++dropped;
                expect(routed.dropped.code == auth::codes::command_unroutable,
                    "drop code of " + routed.command.verb + ": " + routed.dropped.code + " " + routed.dropped.message);
            }
        }
        std::vector<eawr::sim::CommandKey> keys;
        for (const auth::RoutedCommand& routed : expected.value().script_input) keys.push_back(routed.key);
        expect(kept == keys, "tick " + std::to_string(index) + ": drops take no sequence");
    }
    expect(submitted == 2 * 3 && dropped == 2 * 24,
        "malformed: " + std::to_string(submitted) + " submitted, " + std::to_string(dropped) + " dropped");
    expect(malformed.record() == clean.record(), "malformed: the replay holds only the valid commands");
}

void run_workers() {
    const eawr::sim::InlineExecutor inline_executor;
    const Run reference = run_battle(inline_executor);
    for (const std::size_t workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const Run run = run_battle(executor);
        expect(run.hashes == reference.hashes, std::to_string(workers) + " workers: combined hashes");
        expect(run.replay == reference.replay, std::to_string(workers) + " workers: recorded replay");
        // #637: the live game's dispatch and hashing thread give the same hashes.
        const eawr::platform::ThreadWorkerAdapter by_cost(workers, eawr::platform::ThreadWorkerAdapter::Dispatch::by_cost);
        const Run live = run_battle(by_cost, std::make_shared<eawr::platform::ThreadStateHasher>());
        expect(live.hashes == reference.hashes && live.world_hashes == reference.world_hashes,
            std::to_string(workers) + " workers, by cost, hashed off thread: combined and world hashes");
        expect(live.replay == reference.replay, std::to_string(workers) + " workers, by cost: recorded replay");
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "";
    if (mode == "routing") {
        run_routing();
        run_malformed();
    } else if (mode == "workers") {
        run_workers();
    } else {
        std::cerr << "usage: lua_bridge_tests routing | workers\n";
        return 2;
    }
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "passed\n";
    return 0;
}
