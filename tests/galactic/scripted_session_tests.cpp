#include "galactic_support.hpp"

#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/galactic_bridge.hpp"
#include "eawr/script/numeric/binary64.hpp"

#include <optional>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
using namespace galactic_test;
namespace auth = eawr::script::authoritative;
using eawr::script::numeric::LuaNumber;

auth::ScriptScheduler scheduler(std::uint64_t id, std::string script, bool throttled = false) {
    auth::SessionConfig config;
    config.seed = 913;
    config.galactic_clock = true;
    auth::ModuleManifest modules;
    if (throttled) script = "ServiceRate = 1/30; LastService = 0\n" + script;
    expect(static_cast<bool>(modules.add("Main.lua", std::move(script))), "script module admitted");
    auto created = auth::ScriptScheduler::create(config, std::move(modules));
    expect(static_cast<bool>(created), "GC-profile scheduler created");
    auto scripts = std::move(created).value();
    expect(static_cast<bool>(scripts.register_binding("Emit", [](auth::BindingContext& context, const auth::ValueList& arguments) {
        context.issue_command("mark", arguments);
        return eawr::core::Result<auth::ValueList>::success({});
    })), "test command binding");
    expect(static_cast<bool>(scripts.register_binding("Draw", [](auth::BindingContext& context, const auth::ValueList&) {
        return eawr::core::Result<auth::ValueList>::success({auth::Value::number(context.random().next_unit())});
    })), "shared counter-based random stream");
    expect(static_cast<bool>(scripts.create_instance(id, "Main.lua")), "script instance created");
    return scripts;
}

constexpr std::string_view loop = R"LUA(
AtLoad = GetCurrentTime.Frame()
Count = 0
function main()
    while true do
        Count = Count + 1
        Emit(GetCurrentTime.Frame(), Count, Draw())
        coroutine.yield(true)
    end
end
Create_Thread("main")
)LUA";

galactic::GalacticSetup setup() {
    auto out = galactic::synthetic_galaxy({});
    out.seats.push_back({{99}, out.players.front().player_id, galactic::SeatKind::script, 0, true, 0});
    return out;
}

void wire(auth::ScriptedGalacticSession& hosted) {
    const auto planet = hosted.world().setup().planets.front().id;
    expect(static_cast<bool>(hosted.register_verb("mark", [planet](const auth::ScriptCommand&) {
        return eawr::core::Result<auth::GalacticOrder>::success({{99}, galactic::DebugSetPlanetOwner{planet, 1}});
    })), "typed debug translator registered");
}

auth::ScriptedGalacticSession session(bool throttled = false) {
    auto world = galactic::GalacticSession::create(setup());
    expect(static_cast<bool>(world), "galaxy with a system script seat");
    auto created = auth::ScriptedGalacticSession::create(std::move(world).value(),
        scheduler(1, std::string(loop)), scheduler(2, std::string(loop), throttled));
    expect(static_cast<bool>(created), "scripted galaxy created");
    auto hosted = std::move(created).value();
    wire(hosted);
    return hosted;
}

void pause_clocks_and_throttle() {
    const eawr::sim::InlineExecutor executor;
    auto host = session();
    auto first = host.step(executor);
    expect(static_cast<bool>(first), "first service");
    if (!first) return;
    expect(first.value().ai && first.value().ai->commands.size() == 1 && first.value().story.commands.size() == 1,
        "AI and story produce independently");
    expect(host.pending_script_commands() == 2 && host.record().commands.empty(), "scripts stage next-tick input only");
    std::vector<galactic::GameCommand> pause{{{1, {1}, 1}, galactic::PauseGalaxy{}}};
    auto paused = host.step(executor, pause);
    expect(static_cast<bool>(paused), "pause is a recorded barrier");
    if (!paused) return;
    expect(!paused.value().ai && paused.value().story.commands.size() == 1, "paused AI stops while attached story services");
    expect(paused.value().script_input.size() == 2 && paused.value().script_input[0].key && paused.value().script_input[1].key,
        "prior AI and story commands route through their system seat");
    expect(paused.value().script_input[0].key->sequence + 1 == paused.value().script_input[1].key->sequence,
        "both hosts share the seat's routing cursor");
    const auto* frame = std::get_if<LuaNumber>(&paused.value().story.commands[0].arguments[0].data);
    expect(frame && frame->repr == LuaNumber(1).repr, "GetCurrentTime.Frame stays on the galactic frame while paused");
    auto again = host.step(executor);
    expect(again && !again.value().ai && again.value().story.commands.size() == 1, "second paused barrier services story only");
    if (again) {
        expect(std::get<LuaNumber>(again.value().story.commands[0].arguments[2].data).repr !=
            std::get<LuaNumber>(paused.value().story.commands[0].arguments[2].data).repr,
            "distinct paused services do not reuse a random draw");
    }
    auto saved = host.save_story();
    expect(static_cast<bool>(saved), "story saves at a paused barrier with a frozen mode clock");
    auto restored = scheduler(2, std::string(loop));
    expect(saved && static_cast<bool>(restored.load(saved.value())), "GC scheduler restores the paused clock profile");
    if (saved) expect(restored.save().value() == saved.value(), "GC scheduler save/load is byte-identical");
    expect(static_cast<bool>(restored.create_instance(3, "Main.lua")), "new plot chunk attaches at a paused restored barrier");
    const auto at_load = restored.read_global(3, "AtLoad");
    expect(at_load && at_load.value() && std::get<LuaNumber>(at_load.value()->data).repr == LuaNumber(1).repr,
        "newly attached chunks see the saved galactic frame, not the story service count");

    std::vector<galactic::GameCommand> resume{{{3, {1}, 2}, galactic::ResumeGalaxy{}}};
    auto resumed = host.step(executor, resume);
    expect(resumed && resumed.value().ai && host.world().clock().galactic_frame == 2, "AI resumes on the next galactic frame");

    auto throttled = session(true);
    auto tick = throttled.step(executor);
    expect(tick && tick.value().story.commands.empty(), "elapsed equal to ServiceRate does not pump");
    tick = throttled.step(executor);
    expect(tick && tick.value().story.commands.size() == 1, "elapsed strictly greater than ServiceRate pumps");
    std::vector<galactic::GameCommand> hold{{{2, {1}, 1}, galactic::PauseGalaxy{}}};
    tick = throttled.step(executor, hold);
    const auto frozen = throttled.world().clock().galactic_frame;
    // One step later is exactly the rate (equal to within rounding), two steps later is past it: over three paused steps the
    // pump comes due again, while the mode clock (GetCurrentTime) stays frozen.
    std::size_t pumped = tick ? tick.value().story.commands.size() : 0;
    for (int step = 0; step < 3; ++step) {
        tick = throttled.step(executor);
        pumped += tick ? tick.value().story.commands.size() : 0;
    }
    expect(pumped >= 1, "WST-54 the pump clock is the synchronizer's, which runs through a pause: a paused script is pumped again");
    expect(throttled.world().clock().galactic_frame == frozen, "while the mode clock (GetCurrentTime) stays frozen");
}

void named_fault_and_guardrails() {
    auth::SessionConfig config;
    config.galactic_clock = true;
    const eawr::sim::InlineExecutor executor;
    for (const std::string_view name : {"Find_Player", "Create_Thread.Is_Thread_Active", "ThreadValue.Get"}) {
        auth::ModuleManifest modules;
        expect(static_cast<bool>(modules.add("Main.lua", "function main() " + std::string(name) +
            "('Empire'); Emit(1) end Create_Thread('main')")), "fault module");
        auto scripts = auth::ScriptScheduler::create(config, std::move(modules)).value();
        auto installed = auth::register_missing_galactic_api(scripts);
        expect(static_cast<bool>(installed), "inventory names install deterministic fault bindings");
        if (!installed) std::cerr << installed.error().message << '\n';
        expect(static_cast<bool>(scripts.create_instance(7, "Main.lua")), "unbound API faults on call, not declaration");
        auth::ServiceOptions service;
        service.time_tick = 1;
        auto fault = scripts.service(executor, service);
        expect(fault && !fault.value().diagnostics.empty(), "unbound global produces a script diagnostic");
        if (fault && !fault.value().diagnostics.empty()) {
            expect(fault.value().diagnostics[0].code == auth::codes::missing_api &&
                fault.value().diagnostics[0].message.find(name) != std::string::npos,
                "missing API fault carries its inventory name and stable code");
            expect(fault.value().commands.empty(), "a missing API cannot silently publish later commands");
        }
    }
    const auto harmless = [](auth::BindingContext&, const auth::ValueList&) {
        return eawr::core::Result<auth::ValueList>::success({});
    };
    auto gc_protected = auth::ScriptScheduler::create(config, {}).value();
    expect(!gc_protected.register_binding("Create_Thread.Kill", harmless), "GC extensions cannot replace built-in members");
    expect(!gc_protected.register_binding("Create_Thread", harmless), "GC extensions cannot replace built-in calls");
    auto forbidden = gc_protected.register_binding("GameRandom.Free_Random", harmless);
    expect(!forbidden && forbidden.error().code == auth::codes::forbidden_capability,
        "unsynchronized random remains forbidden instead of a missing adapter");
    config.galactic_clock = false;
    auto tactical_protected = auth::ScriptScheduler::create(config, {}).value();
    expect(!tactical_protected.register_binding("ThreadValue.Get", harmless), "tactical infrastructure namespace remains protected");
    auto bad = auth::ScriptedGalacticSession::create(galactic::GalacticSession::create(setup()).value(),
        scheduler(1, std::string(loop)), scheduler(1, std::string(loop)));
    expect(!bad && bad.error().code == auth::codes::invalid_request, "AI/story RNG identities cannot overlap");
    auto host = session();
    expect(!host.register_verb("mark", [](const auth::ScriptCommand&) {
        return eawr::core::Result<auth::GalacticOrder>::success({});
    }), "duplicate translator refused");
    expect(static_cast<bool>(host.step(executor)), "first step");
    expect(!host.set_ai_engine({}), "engine cannot change after first step");
}

struct ThrowingEngine final : auth::GalacticScriptEngine {
    eawr::core::Result<auth::ServiceOptions> before_service(const eawr::sim::PartitionExecutor&,
        const galactic::GalacticSession&, const galactic::GalacticTick&, auth::ScriptScheduler&) override {
        throw std::runtime_error("injected");
    }
};

struct CounterShadow final : auth::ImmediateEffectState {
    std::uint64_t value{};
    std::unique_ptr<auth::ImmediateEffectState> clone() const override { return std::make_unique<CounterShadow>(*this); }
    eawr::core::Result<auth::ValueList> apply(std::string_view, const auth::ValueList&) override {
        ++value;
        return eawr::core::Result<auth::ValueList>::success({});
    }
    eawr::core::Result<auth::ValueList> query(std::string_view, const auth::ValueList&) const override {
        return eawr::core::Result<auth::ValueList>::success({});
    }
};
struct CounterHost final : auth::ImmediateEffectHost {
    CounterShadow state;
    bool refuse{};
    eawr::core::Result<std::unique_ptr<auth::ImmediateEffectState>> capture() const override {
        return eawr::core::Result<std::unique_ptr<auth::ImmediateEffectState>>::success(state.clone());
    }
    eawr::core::Result<void> publish(std::unique_ptr<auth::ImmediateEffectState> next) override {
        if (refuse) {
            eawr::core::Diagnostic diagnostic; diagnostic.code = "test.publish"; diagnostic.message = "infrastructure refusal";
            return eawr::core::Result<void>::failure(std::move(diagnostic));
        }
        state = dynamic_cast<const CounterShadow&>(*next);
        return eawr::core::Result<void>::success();
    }
};

// A successful callback before the failing coroutine must not leak either its
// staged command or its private native write from this instance's service.
auth::ScriptScheduler retirement_scheduler(std::string_view failure, std::shared_ptr<CounterHost> effects = {}) {
    auth::SessionConfig config; config.galactic_clock = true;
    auth::ModuleManifest modules;
    const std::string source = R"LUA(
Count = 0
function Before() Add(); Emit(777) end
function main()
    while true do
        Count = Count + 1
        Add(); Emit(Count)
        if Self() == 1 and Count == 2 then )LUA" + std::string(failure) + R"LUA( end
        coroutine.yield(true)
    end
end
Create_Thread('main')
)LUA";
    expect(bool(modules.add("Main.lua", source)), "retirement module");
    auto scripts = auth::ScriptScheduler::create(config, std::move(modules)).value();
    expect(bool(scripts.register_binding("Self", [](auth::BindingContext& context, const auth::ValueList&) {
        return eawr::core::Result<auth::ValueList>::success({auth::Value::number(LuaNumber(context.instance()))});
    })), "retirement identity");
    expect(bool(scripts.register_binding("Emit", [](auth::BindingContext& context, const auth::ValueList& values) {
        context.issue_command("mark", values);
        return eawr::core::Result<auth::ValueList>::success({});
    })), "retirement output");
    expect(bool(scripts.register_binding("Fail", [](auth::BindingContext&, const auth::ValueList&) {
        eawr::core::Diagnostic diagnostic; diagnostic.code = "test.provider"; diagnostic.message = "deterministic provider Result failure";
        return eawr::core::Result<auth::ValueList>::failure(std::move(diagnostic));
    })), "retirement rejected binding");
    expect(bool(scripts.register_binding("Warn", [](auth::BindingContext& context, const auth::ValueList&) {
        context.report(auth::codes::script_warning, "nonfatal Lua warning");
        context.report(auth::codes::unsupported_api, "documented stand-in");
        context.report(auth::codes::invalid_request, "invalid argument uses a documented fallback");
        context.report(auth::codes::script_error, "a diagnostic code alone does not identify an uncaught Lua failure");
        return eawr::core::Result<auth::ValueList>::success({});
    })), "retirement warning seam");
    expect(bool(scripts.register_binding("Add", [native = bool(effects)](auth::BindingContext& context, const auth::ValueList& values) {
        return native ? context.apply_immediate("add", values) : eawr::core::Result<auth::ValueList>::success({});
    })), "retirement private effect");
    if (effects) expect(bool(scripts.set_immediate_effect_host(effects, 2)), "retirement effect host");
    expect(bool(scripts.create_instance(1, "Main.lua")) && bool(scripts.create_instance(3, "Main.lua")), "retirement siblings");
    return scripts;
}

// An uncaught Lua error in a story or object script ends the failing call (its output rolled back) and the script
// goes on; the AI providers keep the retirement rule. The script keeps working across a checkpoint round trip.
void script_thread_fault() {
    const eawr::sim::InlineExecutor executor;
    constexpr std::string_view source = R"LUA(
Seen = 0
function Handler(kind)
    if kind == 'bad' then Missing_Engine_Function() end
    Seen = Seen + 1
    Emit(Seen)
end
function main() while true do coroutine.yield(true) end end
Create_Thread('main')
)LUA";
    const auto build = [&](bool retire) {
        auth::SessionConfig config;
        config.seed = 77;
        config.galactic_clock = true;
        config.retire_on_lua_error = retire;
        auth::ModuleManifest modules;
        expect(bool(modules.add("Main.lua", std::string(source))), "thread fault module");
        auto scripts = auth::ScriptScheduler::create(config, std::move(modules)).value();
        expect(bool(scripts.register_binding("Emit", [](auth::BindingContext& context, const auth::ValueList& arguments) {
            context.issue_command("mark", arguments);
            return eawr::core::Result<auth::ValueList>::success({});
        })), "thread fault binding");
        return scripts;
    };
    const auto call = [&](auth::ScriptScheduler& scripts, std::uint64_t tick, std::string_view kind) {
        auth::ScriptEvent event;
        event.key = {tick, auth::first_simulation_producer, 1, 0};
        event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Handler";
        event.arguments.push_back(auth::Value::text(std::string(kind)));
        expect(bool(scripts.submit_event(std::move(event))), "post handler call");
    };
    const auto emitted = [](const auth::ServiceReport& report, int expected) {
        if (report.commands.size() != 1 || report.commands[0].arguments.size() != 1) return false;
        const auto* number = std::get_if<LuaNumber>(&report.commands[0].arguments[0].data);
        return number != nullptr && *number == LuaNumber(expected);
    };
    for (const bool retire : {true, false}) {
        auto scripts = build(retire);
        expect(bool(scripts.create_instance(1, "Main.lua")), "thread fault instance");
        call(scripts, 1, "bad");
        const auto first = scripts.service(executor);
        expect(bool(first) && first.value().diagnostics.size() == 1 && first.value().commands.empty(),
            "the failing handler is diagnosed and its output dropped");
        if (!first) continue;
        if (retire) {
            expect(scripts.retired_instances() == std::vector<std::uint64_t>{1} && scripts.instances().empty(),
                "provider rule: an uncaught Lua error retires the instance");
            continue;
        }
        expect(scripts.retired_instances().empty() && scripts.instances() == std::vector<std::uint64_t>{1}
            && first.value().load_notes.empty(), "story and object rule: the script is not retired by an error in one call");
        call(scripts, 2, "ok");
        const auto second = scripts.service(executor);
        expect(bool(second) && second.value().diagnostics.empty() && emitted(second.value(), 1),
            "a later call still runs, and the failed call left no state behind");
        const auto saved = scripts.save();
        expect(bool(saved), "save after a failed call");
        if (!saved) continue;
        auto restored = build(false);
        expect(bool(restored.load(saved.value())), "load after a failed call");
        call(restored, 3, "bad");
        const auto after_load = restored.service(executor);
        expect(bool(after_load) && after_load.value().diagnostics.size() == 1 && restored.retired_instances().empty(),
            "a failure after a checkpoint round trip still ends only that call");
        call(restored, 4, "ok");
        const auto last = restored.service(executor);
        expect(bool(last) && emitted(last.value(), 2), "the restored script keeps handling calls");
    }
}

// An uncaught error ends the failing thread only (the L-23 thread rule): the other threads of the script keep running,
// what the failing resume staged is rolled back, the diagnostic names the thread, and the service reports the failure so
// a host can release what the thread's plot locked. A script whose only thread fails stays (not retired); the host
// removes a script left with no live thread, as it does for one that returned. A failing call that repeats is reported
// a few times and then by count.
void script_thread_failures() {
    const eawr::sim::InlineExecutor executor;
    const auto build = [&](std::string_view source) {
        auth::SessionConfig config;
        config.seed = 78;
        config.galactic_clock = true;
        config.retire_on_lua_error = false;
        auth::ModuleManifest modules;
        expect(bool(modules.add("Main.lua", std::string(source))), "thread failure module");
        auto scripts = auth::ScriptScheduler::create(config, std::move(modules)).value();
        expect(bool(scripts.register_binding("Emit", [](auth::BindingContext& context, const auth::ValueList& arguments) {
            context.issue_command("mark", arguments);
            return eawr::core::Result<auth::ValueList>::success({});
        })), "thread failure binding");
        expect(bool(scripts.create_instance(1, "Main.lua")), "thread failure instance");
        return scripts;
    };
    const auto marks = [](const auth::ServiceReport& report) {
        std::vector<std::string> out;
        for (const auto& command : report.commands)
            if (!command.arguments.empty())
                if (const auto* text = std::get_if<std::string>(&command.arguments[0].data)) out.push_back(*text);
        return out;
    };
    {
        auto scripts = build(R"LUA(
function Good() while true do Emit('good') coroutine.yield(true) end end
function Bad()
    Emit('bad_before')
    coroutine.yield(true)
    Emit('bad_staged')
    Missing_Engine_Function()
end
Create_Thread('Good')
Create_Thread('Bad')
)LUA");
        const auto first = scripts.service(executor);
        expect(bool(first) && marks(first.value()) == std::vector<std::string>{"good", "bad_before"}
            && first.value().diagnostics.empty() && first.value().thread_failures.empty(), "both threads run before the error");
        const auto second = scripts.service(executor);
        expect(bool(second) && marks(second.value()) == std::vector<std::string>{"good"},
            "the failing resume's staged output is rolled back and the other thread's output survives");
        expect(bool(second) && second.value().diagnostics.size() == 1
            && second.value().diagnostics[0].instance == 1 && second.value().diagnostics[0].thread == 1
            && second.value().diagnostics[0].message.find("thread 1:") == 0
            && second.value().diagnostics[0].message.find("Missing_Engine_Function") != std::string::npos
            && second.value().thread_failures == std::vector<std::uint64_t>{1},
            "one diagnostic names the instance, the thread slot and the cause, and the failure is reported to the host");
        const auto third = scripts.service(executor);
        expect(bool(third) && marks(third.value()) == std::vector<std::string>{"good"} && third.value().diagnostics.empty()
            && third.value().thread_failures.empty() && scripts.retired_instances().empty(),
            "the other thread keeps running, and nothing repeats");
        const auto slots = scripts.thread_slots(1);
        expect(bool(slots) && slots.value() == std::vector<bool>{true, false}, "only the failing thread's slot ended");
    }
    {
        // The main thread is an ordinary thread: when it errors and no other thread lives, the script keeps its handlers
        // until the host removes it (a script with no live thread), and is not retired.
        auto scripts = build(R"LUA(
function Main() Missing_Engine_Function() end
function Handler() Emit('handled') end
Create_Thread('Main')
)LUA");
        const auto first = scripts.service(executor);
        expect(bool(first) && first.value().thread_failures == std::vector<std::uint64_t>{1} && scripts.retired_instances().empty()
            && scripts.instances() == std::vector<std::uint64_t>{1}, "a failing main thread does not retire its script");
        const auto slots = scripts.thread_slots(1);
        expect(bool(slots) && std::none_of(slots.value().begin(), slots.value().end(), [](bool live) { return live; }),
            "the failed main thread has ended and nothing restarts it");
        auth::ScriptEvent event;
        event.key = {2, auth::first_simulation_producer, 1, 0};
        event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Handler";
        expect(bool(scripts.submit_event(std::move(event))), "post handler call");
        const auto second = scripts.service(executor);
        expect(bool(second) && marks(second.value()) == std::vector<std::string>{"handled"}, "its handlers still run");
    }
    {
        auto repeated = build(R"LUA(
function Handler() Missing_Engine_Function() end
function main() while true do coroutine.yield(true) end end
Create_Thread('main')
)LUA");
        std::size_t reported = 0;
        std::string last;
        for (std::uint64_t tick = 1; tick <= 20; ++tick) {
            auth::ScriptEvent event;
            event.key = {tick, auth::first_simulation_producer, 1, 0};
            event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Handler";
            expect(bool(repeated.submit_event(std::move(event))), "post repeated call");
            const auto report = repeated.service(executor);
            expect(bool(report), "repeated failing call service");
            if (!report) break;
            reported += report.value().diagnostics.size();
            if (!report.value().diagnostics.empty()) last = report.value().diagnostics.back().message;
        }
        // Occurrences 1, 2, 3, 4, 8 and 16 of twenty are reported; the last carries its count.
        expect(reported == 6 && last.find("(occurrence 16)") != std::string::npos && repeated.retired_instances().empty(),
            "a repeating failing call is reported by count, not once per call: " + std::to_string(reported) + " " + last);
    }
}

void provider_retirement() {
    for (const std::string_view fault : {"error('injected Lua failure')", "Fail()", "while true do end"}) {
        for (const bool immediate : {false, true}) {
            std::vector<std::string> reference;
            for (const auto workers : {1U, 2U, 4U, 8U}) {
                const eawr::platform::ThreadWorkerAdapter executor(workers);
                auto effects = immediate ? std::make_shared<CounterHost>() : nullptr;
                auto host = auth::ScriptedGalacticSession::create(galactic::GalacticSession::create(setup()).value(),
                    retirement_scheduler(fault, effects), scheduler(2, std::string(loop))).value();
                wire(host);
                auto first = host.step(executor);
                expect(bool(first), "provider first step");
                if (!first) continue;
                auth::ScriptEvent before;
                before.key = {2, auth::first_simulation_producer, 1, 0};
                before.target = 1; before.kind = auth::ScriptEvent::Kind::call; before.name = "Before";
                expect(bool(host.ai_scheduler().submit_event(before)), "successful callback before fault");
                auto tick = host.step(executor);
                expect(bool(tick), "GC continues through a failing Lua provider");
                if (!tick) continue;
                expect(tick.value().ai && tick.value().ai->commands.size() == 1 && tick.value().ai->commands[0].issuer == 3,
                    "all failed-instance staging discarded while sibling commits");
                expect(host.ai_scheduler().retired_instances() == std::vector<std::uint64_t>{1}
                    && host.ai_scheduler().instances() == std::vector<std::uint64_t>{3}, "only failed instance retired");
                expect(!host.ai_scheduler().create_instance(1, "Main.lua"), "retired provider cannot restart");
                expect(host.world().load_notes().size() == 1 && host.world().load_notes()[0].code == "EAWR-LOAD-1400"
                    && host.world().load_notes()[0].object == "1" && host.world().load_notes()[0].phase == eawr::core::LoadPhase::runtime,
                    "runtime LoadNote names failed provider once");
                if (effects) expect(effects->state.value == 3, "failed provider discards prior callback and coroutine private writes");
                const auto saved = host.save_scripts().value();
                const auto world_saved = host.world().save_checkpoint_state().value();
                const auto sync = host.ai_scheduler().sync_hash(executor).value();
                auto restored_world = galactic::GalacticSession::create(setup()).value();
                expect(bool(restored_world.load_checkpoint_state(world_saved)), "retirement world checkpoint");
                auto restored = auth::ScriptedGalacticSession::restore(std::move(restored_world), retirement_scheduler(fault, effects),
                    scheduler(2, std::string(loop)), saved).value();
                wire(restored);
                expect(restored.ai_scheduler().save().value() == saved.ai && restored.ai_scheduler().sync_hash(executor).value() == sync,
                    "retirement full and optimized hashes survive checkpoint");
                expect(!restored.ai_scheduler().create_instance(1, "Main.lua"), "checkpoint keeps provider disabled");
                std::vector<std::string> hashes{first.value().state_hash.get(), tick.value().state_hash.get()};
                for (unsigned step = 0; step < 3; ++step) {
                    auto continued = host.step(executor);
                    auto resumed = restored.step(executor);
                    expect(continued && resumed && continued.value().state_hash.get() == resumed.value().state_hash.get(),
                        "retired provider checkpoint continuation equals uninterrupted session");
                    if (continued) hashes.push_back(continued.value().state_hash.get());
                }
                expect(host.world().load_notes().size() == 1, "retired provider produces no repeated note");
                auto replay = galactic::GalacticSession::from_replay(host.record()).value();
                while (replay.completed_tick() < host.completed_tick()) expect(bool(replay.step(executor)), "retirement world replay");
                expect(replay.state_sha256() == host.world().state_sha256(), "retired provider accepted orders replay equally");
                auto reproduced = auth::ScriptedGalacticSession::create(galactic::GalacticSession::create(setup()).value(),
                    retirement_scheduler(fault), scheduler(2, std::string(loop))).value();
                wire(reproduced);
                std::vector<std::string> reproduced_hashes;
                for (unsigned step = 0; step < 5; ++step) reproduced_hashes.push_back(reproduced.step(executor).value().state_hash.get());
                expect(reproduced_hashes == hashes, "fresh provider execution reproduces complete faulted script and world hashes");
                if (reference.empty()) reference = hashes;
                else expect(reference == hashes, "provider retirement equal on 1/2/4/8 workers");
            }
        }
    }
    const eawr::sim::InlineExecutor executor;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
      for (const auto code : {auth::codes::invalid_request, auth::codes::script_error}) {
        const eawr::platform::ThreadWorkerAdapter attached_executor(workers);
        auth::SessionConfig config; config.galactic_clock = true;
        auth::ModuleManifest modules;
        expect(bool(modules.add("Attached.lua", R"LUA(
function Base_Definitions() end
function main() Warn(); coroutine.yield(true) end
)LUA")), "attached diagnostic module");
        auto attached = auth::ScriptScheduler::create(config, std::move(modules)).value();
        expect(bool(attached.register_binding("Warn", [code](auth::BindingContext& context, const auth::ValueList&) {
            context.report(code, "fallback diagnostic");
            return eawr::core::Result<auth::ValueList>::success({});
        })), "attached diagnostic binding");
        const auto reconciled = attached.reconcile_objects({{{1, 7}, 1, 7, "Attached.lua"}});
        expect(reconciled && reconciled.value().empty(), "attach GC object script");
        // Object attachment starts a host-paced coroutine. Its pump follows the
        // attach's definition/start/Object events, otherwise Warn never runs.
        auth::ScriptEvent pump;
        pump.key = {1, auth::first_simulation_producer, 7, 3};
        pump.target = 7;
        pump.kind = auth::ScriptEvent::Kind::pump;
        expect(bool(attached.submit_event(std::move(pump))), "pump the attached GC coroutine");
        const auto serviced = attached.service(attached_executor);
        expect(serviced && serviced.value().diagnostics.size() == 1
            && serviced.value().diagnostics.front().code == code,
            "attached GC coroutine actually reports its fallback diagnostic");
        expect(serviced && serviced.value().load_notes.empty()
            && attached.retired_instances().empty() && attached.instances() == std::vector<std::uint64_t>{7},
            "attached GC fallback diagnostic does not retire provider");
      }
    }
    auto warned = retirement_scheduler("Warn()");
    expect(bool(warned.service(executor)), "warning fixture first step");
    const auto warnings = warned.service(executor);
    expect(warnings && warnings.value().diagnostics.size() == 4 && warnings.value().load_notes.empty()
        && warned.retired_instances().empty() && warned.instances().size() == 2, "warnings and stand-ins do not retire providers");
    auto scripts = retirement_scheduler("Fail()");
    expect(bool(scripts.service(executor)) && bool(scripts.service(executor)), "corruption fixture retires instance");
    expect(bool(scripts.remove_instance(3)), "corruption fixture has only a retired provider");
    const auto bytes = scripts.save().value();
    // Independent v4 corpus row: retirement count=1, ID=1, then empty object,
    // pending-event and live-instance registries. No old-format loader exists.
    const std::string suffix = std::string("\1\0\0\0\1", 5) + std::string(19, '\0');
    expect(bytes.ends_with(suffix), "current-format GC retirement corpus suffix");
    const auto offset = bytes.size() - suffix.size();
    const auto reject = [&](std::string malformed) {
        const auto before = scripts.state_hash().value();
        expect(!scripts.load(malformed) && scripts.state_hash().value() == before, "malformed retirement load is transactional");
    };
    auto zero = bytes; zero[offset + 4] = '\0'; reject(zero);
    auto duplicate = bytes; duplicate[offset] = '\2'; duplicate.insert(offset + 12, std::string("\1", 1) + std::string(7, '\0')); reject(duplicate);
    auto unsorted = duplicate; unsorted[offset + 4] = '\2'; reject(unsorted);
    auto oversized = bytes; oversized.replace(offset, 4, 4, '\xff'); reject(oversized);
    auto live = retirement_scheduler("Fail()");
    expect(bool(live.service(executor)), "live-provider corruption fixture");
    auto collision = live.save().value();
    collision[offset] = '\1'; collision.insert(offset + 4, std::string("\1", 1) + std::string(7, '\0')); reject(collision);
    reject(bytes.substr(0, bytes.size() - 1));
    auto old = bytes;
    const auto version = old.find("eawr-script-galactic-save-v5");
    expect(version != std::string::npos, "current GC save domain");
    if (version != std::string::npos) { old[version + std::string_view("eawr-script-galactic-save-v").size()] = '2'; reject(old); }
    auto notified = scripts.save().value();
    auth::ScriptEvent event; event.key = {3, auth::first_simulation_producer, 1, 0}; event.target = 1;
    expect(bool(scripts.submit_event(event)) && scripts.save().value() == notified, "events to retired provider are dropped");
    auto story = auth::ScriptedGalacticSession::create(galactic::GalacticSession::create(setup()).value(),
        scheduler(2, std::string(loop)), retirement_scheduler("Fail()")).value();
    wire(story);
    expect(bool(story.step(executor)), "story retirement first barrier");
    const auto story_fault = story.step(executor);
    expect(story_fault && story_fault.value().story.commands.size() == 1 && story_fault.value().story.commands[0].issuer == 3
        && story.world().load_notes().size() == 1 && bool(story.step(executor)), "failing story provider retires with AI and sibling continuation");
    auto effects = std::make_shared<CounterHost>(); effects->refuse = true;
    auto infrastructure = retirement_scheduler("Fail()", effects);
    const auto refused = infrastructure.service(executor);
    expect(bool(refused) && refused.value().commands.empty() && effects->state.value == 0
        && infrastructure.retired_instances() == std::vector<std::uint64_t>{1, 3} && refused.value().load_notes.size() == 2
        && bool(infrastructure.service(executor)),
        "a refused immediate publication retires the instances whose effects it dropped; the service and the session go on");
    struct FailedExecutor final : eawr::sim::PartitionExecutor {
        std::size_t worker_count() const noexcept override { return 1; }
        eawr::core::Result<void> execute(std::size_t, const std::function<void(std::size_t)>&) const override {
            eawr::core::Diagnostic diagnostic; diagnostic.code = "test.executor"; diagnostic.message = "executor failed";
            return eawr::core::Result<void>::failure(std::move(diagnostic));
        }
    } failed_executor;
    auto execution = retirement_scheduler("Fail()");
    const auto failed = execution.service(failed_executor);
    expect(!failed && failed.error().code == "test.executor" && execution.retired_instances().empty() && !execution.service(executor),
        "executor failure refuses without retiring a provider");
}

void terminal_failure() {
    const eawr::sim::InlineExecutor executor;
    auto host = session();
    expect(static_cast<bool>(host.set_story_engine(std::make_shared<ThrowingEngine>())), "engine test seam");
    auto tick = host.step(executor);
    expect(!tick && tick.error().code == auth::codes::session_abort, "engine exception aborts the scripted session");
    const auto completed = host.completed_tick();
    tick = host.step(executor);
    expect(!tick && host.completed_tick() == completed, "terminal failure prevents any further world advance");
}

void routing_failures_and_finished_plot() {
    const eawr::sim::InlineExecutor executor;
    auto make = [] {
        return auth::ScriptedGalacticSession::create(galactic::GalacticSession::create(setup()).value(),
            scheduler(1, std::string(loop)), scheduler(2, std::string(loop))).value();
    };
    auto absent = make();
    expect(static_cast<bool>(absent.step(executor)), "missing translator stages commands");
    auto dropped = absent.step(executor);
    expect(dropped && dropped.value().script_input.size() == 2 && dropped.value().script_input[0].dropped,
        "unregistered verbs are dropped with a named diagnostic");
    expect(absent.record().commands.empty(), "unregistered script verbs never enter the world replay");

    auto human = make();
    const auto planet = human.world().setup().planets.front().id;
    expect(static_cast<bool>(human.register_verb("mark", [planet](const auth::ScriptCommand&) {
        return eawr::core::Result<auth::GalacticOrder>::success({{1}, galactic::DebugSetPlanetOwner{planet, 1}});
    })), "human-seat translator test seam");
    expect(static_cast<bool>(human.step(executor)), "stage impersonation attempt");
    dropped = human.step(executor);
    expect(dropped && dropped.value().script_input[0].dropped && human.record().commands.empty(),
        "script routing cannot impersonate a human seat");

    auto throwing = make();
    expect(static_cast<bool>(throwing.register_verb("mark", [](const auth::ScriptCommand&) -> eawr::core::Result<auth::GalacticOrder> {
        throw std::runtime_error("translator");
    })), "throwing translator test seam");
    expect(static_cast<bool>(throwing.step(executor)), "stage translator exception");
    dropped = throwing.step(executor);
    expect(dropped && dropped.value().script_input[0].dropped && throwing.record().commands.empty(),
        "translator exceptions drop only their commands");
    expect(static_cast<bool>(throwing.step(executor)), "dropped commands do not abort the host");

    auto exhausted = session();
    expect(static_cast<bool>(exhausted.step(executor)), "stage sequence-exhaustion commands");
    std::vector<galactic::GameCommand> max_sequence{{{1, {99}, std::numeric_limits<std::uint64_t>::max()},
        galactic::DebugSetPlanetOwner{planet, 1}}};
    dropped = exhausted.step(executor, max_sequence);
    expect(dropped && dropped.value().refused_input.empty() && dropped.value().script_input[0].dropped,
        "an exhausted system-seat sequence is diagnosed without wrapping");

    auto finite = auth::ScriptedGalacticSession::create(galactic::GalacticSession::create(setup()).value(),
        scheduler(1, std::string(loop)), scheduler(2, "function main() Emit(1) end Create_Thread('main')")).value();
    auto ended = finite.step(executor);
    expect(ended && ended.value().story.removed_instances == std::vector<std::uint64_t>{2},
        "a finished attached story script detaches");
    ended = finite.step(executor);
    expect(ended && ended.value().story.loads.empty(), "detached plot Lua has no future service load");
}

std::vector<std::string> trace(const eawr::sim::PartitionExecutor& executor, bool async, bool restore = false) {
    auto host = session(true);
    if (async) host.set_state_hasher(std::make_shared<eawr::platform::ThreadStateHasher>());
    std::vector<std::string> hashes;
    for (std::uint64_t tick = 0; tick < 65; ++tick) {
        std::vector<galactic::GameCommand> input;
        if (tick == 5) input.push_back({{tick, {1}, 1}, galactic::PauseGalaxy{}});
        if (tick == 10) input.push_back({{tick, {1}, 2}, galactic::ResumeGalaxy{}});
        auto result = host.step(executor, input);
        expect(static_cast<bool>(result), "parity trace step");
        if (!result) return {};
        hashes.push_back(result.value().state_hash.get());
        if (restore && (tick == 7 || tick == 30)) {
            auto saved = host.save_scripts();
            expect(static_cast<bool>(saved), "AI/story barrier package saves with pending commands");
            if (!saved) return {};
            auto restored_world = galactic::GalacticSession::from_replay(host.record()).value();
            while (restored_world.completed_tick() < host.completed_tick()) {
                if (!restored_world.step(executor)) return {};
            }
            auto restored = auth::ScriptedGalacticSession::restore(std::move(restored_world),
                scheduler(1, std::string(loop)), scheduler(2, std::string(loop), true), saved.value());
            expect(static_cast<bool>(restored), "restores separate AI/story graphs, frozen clock, queue and cursors");
            if (!restored) return {};
            expect(restored.value().script_state_hash().value() == host.script_state_hash().value(),
                "restored script barrier hash is identical before continuation");
            host = std::move(restored).value();
            wire(host);
            if (async) host.set_state_hasher(std::make_shared<eawr::platform::ThreadStateHasher>());
        }
    }
    auto replayed = galactic::GalacticSession::from_replay(host.record()).value();
    while (replayed.completed_tick() < host.completed_tick()) expect(static_cast<bool>(replayed.step(executor)), "world replay step");
    expect(replayed.state_sha256() == host.world().state_sha256(), "recorded script orders replay the world without Lua");
    return hashes;
}

void staged_orbit_recheck() {
    namespace gc = eawr::sim::galactic;
    const auto f = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    gc::GalacticSetup s;
    s.players = {{1, 1, 0}, {2, 2, 0}};
    s.seats = {{{1}, 1, gc::SeatKind::human, 0, true, 1}, {{99}, 1, gc::SeatKind::script, 0, true, 0}};
    s.planets = {{1, {}, 1}, {2, {f(12), {}, {}}, 1}, {3, {f(24), {}, {}}, 2}};
    s.fleets = {{10, 1, 3, {{20, 1}}}, {11, 1, 2, {{21, 1}}}};
    s.rules.maximum_fleet_movement_distance = f(12);
    s.fleet_control.emplace(); s.fleet_control->types = {{1, false, true, false}};
    s.movement.emplace(); s.movement->mode_modifier = f(1);
    s.movement->types = {{1, f(2), f(100), 0, true, false, false, false}};
    s.movement->players = {{1, f(1), f(1), false}, {2, f(1), f(1), false}};
    s.movement->planets = {{1, f(1), false}, {2, f(1), false}, {3, f(1), false}};
    for (const auto& player : s.players) for (const auto& planet : s.planets)
        s.movement->access.push_back({player.player_id, planet.id, true, planet.id == 3});
    const auto scripts = [](std::uint64_t instance, eawr::sim::EntityId target) {
        auth::SessionConfig config; config.galactic_clock = true;
        auth::ModuleManifest modules;
        expect(bool(modules.add("Main.lua", "function main() Result = GetTarget():Force_Test_Space_Conflict() end Create_Thread('main')")), "orbit script module");
        auto scheduler = auth::ScriptScheduler::create(config, std::move(modules)).value();
        expect(bool(auth::register_galactic_orbit_recheck_method(scheduler, 1)), "Lua object orbit method binds before instances");
        expect(bool(scheduler.register_binding("GetTarget", [target](auth::BindingContext&, const auth::ValueList&) {
            return eawr::core::Result<auth::ValueList>::success({auth::Value{auth::Handle{1, target}}});
        })), "synthetic planet/fleet handle binding");
        expect(bool(scheduler.create_instance(instance, "Main.lua")), "orbit script instance");
        return scheduler;
    };
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto host = auth::ScriptedGalacticSession::create(gc::GalacticSession::create(s).value(), scripts(1, 10), scripts(2, 3)).value();
        expect(!host.bind_orbit_recheck({1}) && bool(host.bind_orbit_recheck({99})), "orbit provider explicitly binds only a script seat");
        const gc::GameCommand departure{{0, {1}, 1}, gc::MoveFleet{11, {2, 3}, {}}};
        auto tick = host.step(executor, std::span(&departure, 1)).value();
        expect(tick.ai && tick.ai->diagnostics.empty() && tick.story.diagnostics.empty() && host.pending_script_commands() == 2,
            "AI/story Lua stage planet and fleet rechecks without calling the world during partitioned service");
        expect(host.world().snapshot()->fleets()[1].motion.transit.has_value(), "Lua call leaves inbound fleet enroute until the next command barrier");
        std::vector<std::string> hashes{tick.state_hash.get()};
        const auto saved = host.save_scripts().value();
        auto restored_world = gc::GalacticSession::create(s).value();
        expect(bool(restored_world.load_checkpoint_state(host.world().save_checkpoint_state().value())), "pending Lua orbit world restores");
        host = auth::ScriptedGalacticSession::restore(std::move(restored_world), scripts(1, 10), scripts(2, 3), saved).value();
        expect(bool(host.bind_orbit_recheck({99})), "orbit provider rebinds after restore");
        bool tail_saw_conflict{};
        host.parent_world().set_conflict_tail_handler([&](gc::GalacticSession& world) {
            tail_saw_conflict = std::any_of(world.snapshot()->events().begin(), world.snapshot()->events().end(), [](const auto& event) {
                return event.kind == gc::GalacticEventKind::movement_conflict;
            });
            return eawr::core::Result<void>::success();
        }, false);
        tick = host.step(executor).value(); hashes.push_back(tick.state_hash.get());
        expect(tail_saw_conflict, "ordinary conflict tail receives the staged recheck in its executing tick");
        expect(tick.script_input.size() == 2 && tick.script_input[0].accepted && tick.script_input[1].accepted
            && std::holds_alternative<gc::RecheckOrbit>(tick.script_input[0].accepted->payload), "Lua rechecks route as recorded typed commands");
        const auto snapshot = host.world().snapshot();
        expect(snapshot->fleets()[1].orbit == 3 && !snapshot->fleets()[1].motion.transit
            && snapshot->fleets()[1].motion.position.x == f(42), "staged recheck gathers inbound fleet through the existing planet orbit");
        expect(std::any_of(snapshot->events().begin(), snapshot->events().end(), [](const auto& event) {
            return event.kind == gc::GalacticEventKind::movement_conflict;
        }), "recheck conflict event survives into the command tick's conflict tail");
        auto replay = gc::GalacticSession::from_replay(host.record()).value();
        while (replay.completed_tick() < host.completed_tick()) expect(bool(replay.step(executor)), "Lua orbit typed world replay");
        expect(replay.state_sha256() == host.world().state_sha256(), "Lua orbit commands replay without Lua or a native journal side channel");
        if (reference.empty()) reference = hashes; else expect(reference == hashes, "Lua orbit staging/restore/gather equal on 1/2/4/8 workers");
    }
}

void scheduler_phase_rollback() {
    std::vector<std::string> reference;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto scripts = scheduler(1, std::string(loop));
        expect(bool(scripts.service(executor)), "phase fixture runs its initial Lua tick");
        auth::ScriptEvent future;
        future.key = {3, auth::first_simulation_producer, 1, 0};
        future.target = 1;
        future.kind = auth::ScriptEvent::Kind::assign;
        future.name = "Pending";
        future.parameter = auth::Value{std::string("retained")};
        expect(bool(scripts.submit_event(std::move(future))), "phase fixture retains a future event");
        const auto before = scripts.save(executor).value();
        const auto encodes = scripts.sync_graph_encodes();
        auto captured = scripts.begin_phase(executor);
        expect(bool(captured), "GC scheduler captures its phase participant");
        if (!captured) return;
        auto phase = std::move(captured).value();
        expect(phase->work().graph_encodes == 0 && phase->work().shared_graphs == 1
            && scripts.sync_graph_encodes() == encodes, "warm phase capture shares its graph without a second Lua traversal");
        expect(!scripts.begin_phase(executor), "nested script phase refuses before mutation");
        auto speculative = scripts.service(executor);
        expect(speculative && speculative.value().commands.size() == 1, "speculative phase really resumes Lua and emits output");
        const auto after = scripts.save(executor).value();
        expect(after != before, "speculative Lua globals, counters and RNG really change");
        expect(bool(scripts.remove_instance(1)) && bool(scripts.create_instance(9, "Main.lua")),
            "phase also changes scheduler attachments and removes addressed events");
        expect(bool(phase->rollback()), "phase rollback restores the VM graph and host metadata");
        expect(scripts.instances() == std::vector<std::uint64_t>{1} && scripts.save(executor).value() == before,
            "rollback restores exact attachment, timer/event, coroutine, clock and RNG bytes");
        expect(!phase->rollback(), "settled script phase cannot restore twice");
        auto retry = scripts.begin_phase(executor).value();
        const auto repeated = scripts.service(executor);
        expect(repeated && repeated.value().commands.size() == 1, "retry emits successful provider output");
        expect(scripts.save(executor).value() == after, "phase retry exactly reproduces successful VM continuation");
        expect(bool(retry->commit()), "successful retry commits the script participant");
        const std::vector<std::string> trace{before, after};
        if (reference.empty()) reference = trace;
        else expect(trace == reference, "scheduler phase capture/restore/retry equal on 1/2/4/8 workers");

        auto abandoned = scripts.begin_phase(executor).value();
        abandoned.reset();
        expect(!scripts.service(executor), "an unaccounted script phase fails closed instead of leaking speculative state");
    }
}

void complete_service_phase_seam() {
    struct PhaseEngine final : auth::GalacticScriptEngine {
        std::size_t attempts{}, preparations{}, tails{};
        bool speculative_notes_hidden{true};
        eawr::core::Result<auth::ServiceReport> service_phase(const eawr::sim::PartitionExecutor& executor,
            galactic::GalacticSession& world, const galactic::GalacticTick& tick, auth::ScriptScheduler& scripts,
            const std::function<eawr::core::Result<auth::ServiceReport>()>& run) override {
            for (unsigned attempt = 0; attempt < 2; ++attempt) {
                ++attempts;
                speculative_notes_hidden = speculative_notes_hidden && world.load_notes().empty() && world.completed_tick() == tick.completed_tick;
                auto begun = scripts.begin_phase(executor);
                if (!begun) return eawr::core::Result<auth::ServiceReport>::failure(begun.error());
                auto phase = std::move(begun).value(); auto result = run();
                auto settled = result ? phase->commit() : phase->rollback();
                if (!settled) return eawr::core::Result<auth::ServiceReport>::failure(settled.error());
                if (result || attempt == 1) return result;
            }
            throw std::logic_error("unreachable bounded seam fixture");
        }
        eawr::core::Result<auth::ServiceOptions> before_service(const eawr::sim::PartitionExecutor&,
            const galactic::GalacticSession&, const galactic::GalacticTick&, auth::ScriptScheduler&) override {
            ++preparations; return eawr::core::Result<auth::ServiceOptions>::success({});
        }
        eawr::core::Result<void> after_service(auth::ServiceReport& report) override {
            ++tails; eawr::core::LoadNote note; note.code = tails == 1 ? "SPECULATIVE" : "COMMITTED";
            note.phase = eawr::core::LoadPhase::runtime; note.object = "phase seam fixture";
            report.load_notes.push_back(std::move(note));
            if (tails == 1) {
                eawr::core::Diagnostic diagnostic; diagnostic.code = "INJECTED-TAIL";
                return eawr::core::Result<void>::failure(std::move(diagnostic));
            }
            return eawr::core::Result<void>::success();
        }
    };
    for (const unsigned workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers); auto host = session();
        auto engine = std::make_shared<PhaseEngine>(); expect(bool(host.set_ai_engine(engine)), "whole phase engine installed");
        const auto tick = host.step(executor);
        expect(tick && engine->attempts == 2 && engine->preparations == 2 && engine->tails == 2
            && engine->speculative_notes_hidden && host.world().completed_tick() == 1,
            "whole service retry encloses native preparation, Lua and late tail without repeating world.step");
        if (!tick || !tick.value().ai) continue;
        const auto count = host.ai_scheduler().read_global(1, "Count");
        expect(count && count.value() && std::get<LuaNumber>(count.value()->data) == LuaNumber(1)
            && tick.value().ai->commands.size() == 1, "late tail retry restores Lua globals and emits one successful command");
        const auto notes = host.world().load_notes();
        expect(notes.size() == 1 && notes.front().code == "COMMITTED", "only the successful complete service publishes runtime notes");
    }
}

// Async AI step 2c-3 (R3): the per-tick AI/story disjointness check reads a sim-side copy of the AI scheduler's instance ids and reads the
// scheduler's list again only when its instance revision moved (create, remove, load); a host that mutates the AI scheduler between steps
// is seen at the next step, and a colliding id is still refused.
void ai_instance_copy_seam() {
    const eawr::sim::InlineExecutor executor;
    auto host = session();
    expect(host.ai_instance_list_reads() == 0 && host.ai_instance_copy_matches(), "no list read before the first step");
    const auto revision = host.ai_scheduler().instance_revision();
    expect(bool(host.step(executor)), "first step");
    expect(host.ai_instance_list_reads() == 1, "the first step copies the AI instance ids once");
    for (unsigned tick = 0; tick < 5; ++tick) expect(bool(host.step(executor)), "steady steps");
    expect(host.ai_instance_list_reads() == 1 && host.ai_scheduler().instance_revision() == revision && host.ai_instance_copy_matches(),
        "steady ticks read no id list: the copy is used while the revision stands");
    expect(bool(host.ai_scheduler().create_instance(9, "Main.lua")), "the host creates an AI instance between steps");
    expect(host.ai_scheduler().instance_revision() != revision, "creating an instance moves the revision");
    expect(bool(host.step(executor)), "step after the creation");
    expect(host.ai_instance_list_reads() == 2 && host.ai_instance_copy_matches(), "the next step re-reads the list once and the copy matches");
    expect(bool(host.ai_scheduler().remove_instance(9)), "the host removes it again");
    expect(bool(host.step(executor)) && host.ai_instance_list_reads() == 3 && host.ai_instance_copy_matches(),
        "a removal is seen at the next step");
    // A colliding id (the story scheduler holds 2) is refused by the per-tick check, as before the copy.
    expect(bool(host.ai_scheduler().create_instance(2, "Main.lua")), "an AI instance whose id the story scheduler uses is created by the host");
    expect(!host.step(executor), "the per-tick check refuses ids shared with the story scheduler");
}

void worker_parity() {
    const eawr::sim::InlineExecutor inline_executor;
    const auto baseline = trace(inline_executor, false);
    expect(baseline.size() == 65, "complete baseline trace");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        PhaseProbe probe(executor);
        expect(trace(probe, false) == baseline, "scripted GC 1/2/4/8 worker hashes equal");
        expect(trace(executor, true, true) == baseline, "restored AI/story continue with identical hashes, including async hashing");
        expect(std::any_of(probe.phases.begin(), probe.phases.end(), [](const auto& phase) {
            return phase.first == auth::script_phase_name && phase.second == eawr::sim::tick_partition_count;
        }), "shared scheduler uses the partitioned script-instances phase");
    }
}

// Exercise each public invalidation path against a populated cache. Some
// setup operations affect only future instances or host callbacks, so their
// present serialized digest stays equal even though reuse must be refused.
void hash_reuse_mutator_misses() {
    const eawr::sim::InlineExecutor executor;
    const auto miss = [](auth::ScriptedGalacticSession& host, auth::ScriptScheduler& scripts,
        std::string_view name, auto&& mutate, bool changes_digest) {
        const auto before = host.script_state_hash();
        expect(bool(before), std::string(name) + " seeds the hash cache");
        if (!before) return;
        const auto generation = scripts.state_revision();
        const auto hits = host.verified_script_hash_reuse_count();
        expect(mutate(), std::string(name) + " operation succeeds");
        expect(scripts.state_revision() != generation, std::string(name) + " invalidates the generation");
        const auto after = host.script_hash_for_final_barrier(false);
        expect(host.verified_script_hash_reuse_count() == hits, std::string(name) + " does not reuse the old hash");
        expect(after && ((after.value() != before.value()) == changes_digest),
            std::string(name) + " has the expected serialized effect");
    };
    const auto empty = [] {
        auth::SessionConfig config; config.galactic_clock = true;
        auth::ModuleManifest modules;
        expect(bool(modules.add("Main.lua", "Value = 1")), "empty scheduler root module");
        expect(bool(modules.add("Prelude.lua", "PreludeValue = 7")), "empty scheduler prelude module");
        return auth::ScriptScheduler::create(config, std::move(modules)).value();
    };
    const auto binding = [](auth::BindingContext&, const auth::ValueList&) {
        return eawr::core::Result<auth::ValueList>::success({});
    };
    auto configured = auth::ScriptedGalacticSession::create(galactic::GalacticSession::create(setup()).value(),
        scheduler(1, std::string(loop)), empty()).value();
    auto& setup_scripts = configured.story_scheduler();
    miss(configured, setup_scripts, "register_binding", [&] { return bool(setup_scripts.register_binding("Extra", binding)); }, true);
    miss(configured, setup_scripts, "register_free_random", [&] { return bool(setup_scripts.register_free_random(binding)); }, true);
    miss(configured, setup_scripts, "register_method", [&] { return bool(setup_scripts.register_method(1, "Extra", binding)); }, true);
    miss(configured, setup_scripts, "register_prelude", [&] { return bool(setup_scripts.register_prelude("Prelude.lua")); }, false);
    miss(configured, setup_scripts, "register_managed_handles", [&] {
        return bool(setup_scripts.register_managed_handles({1, "inventory", "release", 1}));
    }, true);
    miss(configured, setup_scripts, "set_managed_handle_roots", [&] {
        setup_scripts.set_managed_handle_roots([] { return auth::ValueList{}; }); return true;
    }, false);
    miss(configured, setup_scripts, "set_immediate_effect_host", [&] {
        return bool(setup_scripts.set_immediate_effect_host(std::make_shared<CounterHost>(), 8));
    }, false);

    auto host = session(true);
    auto& story = host.story_scheduler();
    miss(host, story, "create_instance", [&] { return bool(story.create_instance(3, "Main.lua")); }, true);
    miss(host, story, "remove_instance", [&] { return bool(story.remove_instance(3)); }, true);
    miss(host, story, "reconcile_objects", [&] {
        const auto result = story.reconcile_objects({{{1, 4}, 1, 4, "Main.lua"}});
        return result && result.value().empty();
    }, true);
    miss(host, story, "set_galactic_frame", [&] { return bool(story.set_galactic_frame(1)); }, true);
    miss(host, story, "stamp_galactic_object_service", [&] {
        return bool(story.stamp_galactic_object_service(4, false, LuaNumber(1), LuaNumber(7)));
    }, true);
    // Object turns are invalid for the galactic clock; even a rejected call
    // conservatively bumps the generation without changing serialized state.
    miss(host, story, "service_turn rejection", [&] { return !story.service_turn(executor, {}); }, false);

    const auto saved = story.save();
    expect(bool(saved), "save before load invalidation");
    if (saved) miss(host, story, "load", [&] { return bool(story.load(saved.value())); }, false);
    auto phase = story.begin_phase(executor);
    expect(bool(phase), "open rollback savepoint");
    if (phase) {
        auth::ScriptEvent event;
        event.key = {story.completed_tick() + 1, auth::first_simulation_producer, 2, 0};
        event.target = 2; event.kind = auth::ScriptEvent::Kind::assign; event.name = "RollbackProbe";
        event.parameter = auth::Value::number(LuaNumber(9));
        expect(bool(story.submit_event(std::move(event))), "stage rollback mutation");
        miss(host, story, "phase rollback", [&] { return bool(phase.value()->rollback()); }, true);
    }
    miss(host, story, "scheduler move assignment", [&] {
        story = scheduler(2, "Different = 123\n" + std::string(loop)); return true;
    }, true);
    expect(story.state_revision() == std::numeric_limits<std::uint64_t>::max(),
        "replacement conservatively disables reuse for this scheduler");

    auto serviced = session();
    auto& service_scripts = serviced.story_scheduler();
    miss(serviced, service_scripts, "service", [&] { return bool(service_scripts.service(executor)); }, true);

    auto object_host = session();
    const auto before_object = object_host.script_state_hash();
    const auto object_hits = object_host.verified_script_hash_reuse_count();
    expect(bool(object_host.set_object_scheduler(scheduler(3, std::string(loop)))), "attach an object scheduler");
    const auto after_object = object_host.script_hash_for_final_barrier(false);
    expect(object_host.verified_script_hash_reuse_count() == object_hits, "object scheduler presence prevents reuse");
    expect(before_object && after_object && before_object.value() != after_object.value(),
        "object scheduler presence changes the script digest");
}

void hash_reuse_mutations() {
    struct ObjectEngine final : auth::GalacticObjectScriptEngine {
        eawr::core::Result<auth::ServiceReport> service(const eawr::sim::PartitionExecutor& executor,
            galactic::GalacticSession& world, auth::ScriptScheduler& scripts) override {
            if (auto set = scripts.set_galactic_frame(world.clock().galactic_frame); !set)
                return eawr::core::Result<auth::ServiceReport>::failure(set.error());
            auth::ServiceOptions options; options.time_tick = world.clock().galactic_frame;
            return scripts.service(executor, options);
        }
    };
    struct PublishedAI final : auth::GalacticScriptEngine {
        std::string digest = "first-publication";
        bool uses_sync_hash() const noexcept override { return true; }
        std::optional<std::string> published_ai_sync_hash() const override { return digest; }
        eawr::core::Result<auth::ServiceOptions> before_service(const eawr::sim::PartitionExecutor&,
            const galactic::GalacticSession&, const galactic::GalacticTick&, auth::ScriptScheduler&) override {
            return eawr::core::Result<auth::ServiceOptions>::success({});
        }
    };
    const eawr::sim::InlineExecutor executor;
    auto host = auth::ScriptedGalacticSession::create(galactic::GalacticSession::create(setup()).value(),
        scheduler(1, std::string(loop)), scheduler(2, std::string(loop)), scheduler(3, std::string(loop))).value();
    expect(host.script_hash_reuse_verification_enabled(), "CTest enables the hash-reuse verification mode in the test process");
    wire(host);
    expect(bool(host.set_object_engine(std::make_shared<ObjectEngine>())), "object engine installed for service");
    auto checkpoint = host.save_scripts();
    expect(bool(checkpoint), "initial script checkpoint saved");
    if (checkpoint) {
        const auto restore = [](const auth::GalacticScriptSave& saved) {
            return auth::ScriptedGalacticSession::restore(galactic::GalacticSession::create(setup()).value(),
                scheduler(1, std::string(loop)), scheduler(2, std::string(loop)), saved, scheduler(3, std::string(loop)));
        };
        expect(bool(restore(checkpoint.value())), "unchanged script checkpoint restores");
        auto changed = checkpoint.value();
        changed.next_sequence[{99}] += 17;
        const auto cursor = restore(changed);
        expect(!cursor && cursor.error().message.find("script barrier digest differs") != std::string::npos,
            "routing sequence alone changes the script digest");
        changed = checkpoint.value();
        changed.pending.push_back({1, 1, 0, "mark", {}});
        const auto pending = restore(changed);
        expect(!pending && pending.error().message.find("script barrier digest differs") != std::string::npos,
            "pending command alone changes the script digest");
    }
    auto publication = std::make_shared<PublishedAI>();
    expect(bool(host.set_ai_engine(publication)), "published AI engine installed");
    auto sync_before = host.script_sync_hash();
    publication->digest = "second-publication";
    auto sync_after = host.script_hash_for_final_barrier(true);
    expect(sync_before && sync_after && sync_before.value() != sync_after.value(),
        "a new asynchronous AI publication changes the sync hash without scheduler mutation");
    auto first = host.script_state_hash();
    const auto hits_before = host.verified_script_hash_reuse_count();
    auto repeated = host.script_hash_for_final_barrier(false);
    expect(first && repeated && first.value() == repeated.value(), "unchanged scheduler components reuse identical hashes");
    expect(host.verified_script_hash_reuse_count() > hits_before, "unchanged barrier actually reuses and verifies the digest");
    auto prior = repeated;
    for (const auto& [scripts, id] : {std::pair{&host.ai_scheduler(), 1U}, std::pair{&host.story_scheduler(), 2U},
                                     std::pair{host.object_scheduler(), 3U}}) {
        const auto revision = scripts->state_revision();
        auth::ScriptEvent event;
        event.key = {1, auth::first_simulation_producer, id, 0}; event.target = id;
        event.kind = auth::ScriptEvent::Kind::assign; event.name = "HashMutation";
        event.parameter = auth::Value::number(LuaNumber(7));
        expect(bool(scripts->submit_event(std::move(event))), "pending event admitted");
        expect(scripts->state_revision() != revision, "pending event invalidates scheduler hash");
        const auto hits_before_mutation = host.verified_script_hash_reuse_count();
        auto next = host.script_hash_for_final_barrier(false);
        expect(host.verified_script_hash_reuse_count() == hits_before_mutation, "mutation prevents a reuse hit");
        expect(prior && next && prior.value() != next.value(), "AI, story and object mutations change script hash");
        auto checked = host.script_state_hash();
        expect(next && checked && next.value() == checked.value(), "verification rechecks the cached component");
        prior = std::move(next);
    }
    const auto story_revision = host.story_scheduler().state_revision();
    const auto ai_revision = host.ai_scheduler().state_revision();
    const auto before_pending = host.script_routing_revision();
    expect(bool(host.step(executor)), "service and command routing step");
    expect(host.story_scheduler().state_revision() != story_revision, "story service invalidates scheduler hash");
    expect(host.ai_scheduler().state_revision() != ai_revision, "AI service advances its mutation generation");
    expect(host.pending_script_commands() > 0, "service stages outbound commands");
    expect(host.script_routing_revision() != before_pending, "staging pending commands advances routing generation");
    auto after = host.script_state_hash();
    const auto hits_after_tick = host.verified_script_hash_reuse_count();
    expect(prior && after && prior.value() != after.value() && bool(host.script_hash_for_final_barrier(false)),
        "post-service hash and its cached recomputation agree");
    expect(host.verified_script_hash_reuse_count() > hits_after_tick, "a steady tick actually reuses its script digest");
    const auto before_routing = host.script_routing_revision();
    expect(bool(host.step(executor)), "route commands and advance sequence");
    expect(host.script_routing_revision() != before_routing, "routing commands and advancing cursors move the generation");
    auto routed = host.script_state_hash();
    expect(after && routed && after.value() != routed.value() && bool(host.script_hash_for_final_barrier(false)),
        "routing and sequence changes are reflected in the recomposed hash");
    auto saved = host.story_scheduler().save();
    const auto before_load = host.story_scheduler().state_revision();
    expect(saved && bool(host.story_scheduler().load(saved.value())), "story scheduler reloads at barrier");
    expect(host.story_scheduler().state_revision() != before_load, "loading invalidates scheduler hash");
    auto reloaded = host.script_hash_for_final_barrier(false);
    expect(routed && reloaded && routed.value() == reloaded.value() && bool(host.script_hash_for_final_barrier(false)),
        "byte-identical reload preserves the hash");
    host.story_scheduler() = scheduler(2, "Different = 123\n" + std::string(loop));
    auto replaced = host.script_hash_for_final_barrier(false);
    expect(reloaded && replaced && reloaded.value() != replaced.value() && bool(host.script_hash_for_final_barrier(false)),
        "replacing a borrowed scheduler cannot reuse its previous owner's hash");
}

void object_scheduler_barrier() {
    struct ObjectEngine final : auth::GalacticObjectScriptEngine {
        eawr::core::Result<auth::ServiceReport> service(const eawr::sim::PartitionExecutor& executor,
            galactic::GalacticSession& world, auth::ScriptScheduler& scripts) override {
            if (auto set = scripts.set_galactic_frame(world.clock().galactic_frame); !set)
                return eawr::core::Result<auth::ServiceReport>::failure(set.error());
            auth::ServiceOptions options; options.time_tick = world.clock().galactic_frame;
            return scripts.service(executor, options);
        }
    };
    const eawr::sim::InlineExecutor executor;
    auto host = auth::ScriptedGalacticSession::create(galactic::GalacticSession::create(setup()).value(),
        scheduler(1, std::string(loop)), scheduler(2, std::string(loop)), scheduler(3, std::string(loop))).value();
    wire(host);
    expect(bool(host.set_object_engine(std::make_shared<ObjectEngine>())), "object engine installed");
    const auto object_revision = host.object_scheduler()->state_revision();
    const auto first = host.step(executor);
    expect(host.object_scheduler()->state_revision() != object_revision, "object service invalidates its scheduler hash");
    expect(first && first.value().objects && first.value().objects->commands.size() == 1,
        "the object scheduler contributes to the galactic barrier");
    auto changed_objects = auth::ScriptedGalacticSession::create(galactic::GalacticSession::create(setup()).value(),
        scheduler(1, std::string(loop)), scheduler(2, std::string(loop)), scheduler(3, std::string(loop))).value();
    wire(changed_objects);
    expect(bool(changed_objects.set_object_engine(std::make_shared<ObjectEngine>())), "different object state engine installed");
    auth::ScriptEvent object_state;
    object_state.key = {1, auth::first_simulation_producer, 3, 0}; object_state.target = 3;
    object_state.kind = auth::ScriptEvent::Kind::assign; object_state.name = "ObjectOnlyState";
    object_state.parameter = auth::Value::number(LuaNumber(7));
    expect(bool(changed_objects.object_scheduler()->submit_event(std::move(object_state))), "object state change admitted");
    const auto changed_tick = changed_objects.step(executor);
    expect(first && changed_tick && host.world().state_sha256() == changed_objects.world().state_sha256()
        && first.value().state_hash.get() != changed_tick.value().state_hash.get(),
        "object script state changes the sim-side hash without changing the world or AI state");
    auto saved = host.save_scripts();
    expect(saved && saved.value().objects.has_value(), "object scheduler graph is saved");
    auto no_host_world = galactic::GalacticSession::from_replay(host.record()).value();
    while (no_host_world.completed_tick() < host.completed_tick()) expect(bool(no_host_world.step(executor)), "missing host world replay");
    expect(!auth::ScriptedGalacticSession::restore(std::move(no_host_world), scheduler(1, std::string(loop)),
        scheduler(2, std::string(loop)), saved.value()), "a saved object graph cannot be restored without its scheduler");
    auto restored_world = galactic::GalacticSession::from_replay(host.record()).value();
    while (restored_world.completed_tick() < host.completed_tick()) expect(bool(restored_world.step(executor)), "object host world replay");
    auto restored = auth::ScriptedGalacticSession::restore(std::move(restored_world), scheduler(1, std::string(loop)),
        scheduler(2, std::string(loop)), saved.value(), scheduler(3, std::string(loop)));
    expect(bool(restored), "the separate object scheduler restores at the same barrier");
    if (!restored) return;
    expect(restored.value().script_state_hash().value() == host.script_state_hash().value(), "restored object state enters the hash");
    wire(restored.value());
    expect(bool(restored.value().set_object_engine(std::make_shared<ObjectEngine>())), "restored object engine installed");
    const auto continued = host.step(executor);
    const auto resumed = restored.value().step(executor);
    expect(continued && resumed && continued.value().state_hash.get() == resumed.value().state_hash.get(),
        "restored object service continues identically");
    auto without_engine = auth::ScriptedGalacticSession::create(galactic::GalacticSession::create(setup()).value(),
        scheduler(1, std::string(loop)), scheduler(2, std::string(loop)), scheduler(3, std::string(loop))).value();
    expect(!without_engine.step(executor), "an installed object scheduler cannot silently run without its engine");
    auth::SessionConfig empty_config; empty_config.galactic_clock = true;
    auto without_ai = auth::ScriptedGalacticSession::create(galactic::GalacticSession::create(setup()).value(),
        auth::ScriptScheduler::create(empty_config, {}).value(), auth::ScriptScheduler::create(empty_config, {}).value()).value();
    wire(without_ai);
    expect(bool(without_ai.set_object_scheduler(scheduler(3, std::string(loop))))
        && bool(without_ai.set_object_engine(std::make_shared<ObjectEngine>())),
        "world setup installs an object host when native AI is absent");
    expect(!without_ai.set_object_scheduler(scheduler(4, std::string(loop))), "object host cannot be installed twice");
    const auto ai_off = without_ai.step(executor);
    expect(ai_off && ai_off.value().objects && ai_off.value().objects->commands.size() == 1,
        "the object scheduler services scripts with no AI instance");
    expect(!without_ai.set_object_scheduler(scheduler(4, std::string(loop))), "a stepped world cannot replace the object host");
    const auto ai_off_save = without_ai.save_scripts();
    expect(bool(ai_off_save), "AI-off object scripts save at the barrier");
    if (!ai_off_save) return;
    auto ai_off_world = galactic::GalacticSession::from_replay(without_ai.record()).value();
    while (ai_off_world.completed_tick() < without_ai.completed_tick())
        expect(bool(ai_off_world.step(executor)), "AI-off world replay reaches the saved barrier");
    auto ai_off_restored = auth::ScriptedGalacticSession::restore(std::move(ai_off_world),
        auth::ScriptScheduler::create(empty_config, {}).value(), auth::ScriptScheduler::create(empty_config, {}).value(),
        ai_off_save.value(), scheduler(3, std::string(loop)));
    expect(bool(ai_off_restored), "AI-off object scheduler restores without a native AI owner");
    if (ai_off_restored) {
        wire(ai_off_restored.value());
        expect(bool(ai_off_restored.value().set_object_engine(std::make_shared<ObjectEngine>())), "AI-off restored engine installed");
        const auto live = without_ai.step(executor), resumed = ai_off_restored.value().step(executor);
        expect(live && resumed && live.value().state_hash.get() == resumed.value().state_hash.get(),
            "AI-off object scripts continue identically after restore");
    }
    auto live_commands = auth::ScriptedGalacticSession::create(galactic::GalacticSession::create(setup()).value(),
        scheduler(1, std::string(loop)), scheduler(2, std::string(loop)), scheduler(3, std::string(loop))).value();
    wire(live_commands);
    expect(bool(live_commands.set_object_engine(std::make_shared<ObjectEngine>())), "object command engine installed");
    unsigned committed{};
    live_commands.set_story_command_committer([&](auth::ServiceReport& report) {
        std::erase_if(report.commands, [&](const auto& command) {
            if (command.issuer != 3) return false;
            ++committed; return true;
        });
        return eawr::core::Result<void>::success();
    });
    const auto live = live_commands.step(executor);
    expect(live && live.value().objects && live.value().objects->commands.empty() && committed == 1
        && live_commands.pending_script_commands() == 2,
        "object story commands commit in the current barrier rather than route as next-tick orders");
}
} // namespace

int main() {
    hash_reuse_mutations();
    hash_reuse_mutator_misses();
    pause_clocks_and_throttle();
    named_fault_and_guardrails();
    terminal_failure();
    provider_retirement();
    script_thread_fault();
    script_thread_failures();
    routing_failures_and_finished_plot();
    staged_orbit_recheck();
    scheduler_phase_rollback();
    complete_service_phase_seam();
    ai_instance_copy_seam();
    worker_parity();
    object_scheduler_barrier();
    if (failures) return 1;
    std::cout << "galactic Lua host contracts passed\n";
    return 0;
}
