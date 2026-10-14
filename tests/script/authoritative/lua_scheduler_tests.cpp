#include "lua_sandbox_support.hpp"

namespace lua_sandbox_test_support {

namespace {
struct WalletShadow final : auth::ImmediateEffectState {
    std::int64_t balance{};
    std::uint64_t next_id{1};
    std::vector<std::uint64_t> objects;
    std::unique_ptr<auth::ImmediateEffectState> clone() const override { return std::make_unique<WalletShadow>(*this); }
    eawr::core::Result<auth::ValueList> apply(std::string_view action, const auth::ValueList& args) override {
        using Result = eawr::core::Result<auth::ValueList>;
        if (action == "spawn") {
            objects.push_back(next_id++);
            return Result::success({auth::Value{auth::Handle{9, objects.back()}}});
        }
        balance += b64::to_int64_truncate(std::get<LuaNumber>(args.at(0).data).repr);
        if (action == "fail") {
            eawr::core::Diagnostic error; error.code = "test.refused"; error.message = "action refused after a private write";
            return Result::failure(error);
        }
        return Result::success({});
    }
    eawr::core::Result<auth::ValueList> query(std::string_view name, const auth::ValueList&) const override {
        return eawr::core::Result<auth::ValueList>::success({auth::Value::number(LuaNumber(
            name == "count" ? static_cast<std::int64_t>(objects.size()) : balance))});
    }
};
struct WalletHost final : auth::ImmediateEffectHost {
    WalletShadow state;
    unsigned publications{};
    bool refuse{};
    eawr::core::Result<std::unique_ptr<auth::ImmediateEffectState>> capture() const override {
        return eawr::core::Result<std::unique_ptr<auth::ImmediateEffectState>>::success(state.clone());
    }
    eawr::core::Result<void> publish(std::unique_ptr<auth::ImmediateEffectState> shadow) override {
        if (refuse) {
            eawr::core::Diagnostic error; error.code = "test.publish"; error.message = "publication refused";
            return eawr::core::Result<void>::failure(error);
        }
        state = dynamic_cast<const WalletShadow&>(*shadow);
        ++publications;
        return eawr::core::Result<void>::success();
    }
};

void immediate_contracts() {
    const std::string source = R"LUA(
function Good()
    local before = Read()
    Add(Self())
    Test_Report(before, Read(), Spawn(), Count())
end
function Failed()
    Add(1000) Spawn() Test_Report('discard') error('rollback')
end
function Caught()
    local before = Read()
    local ok = pcall(function() Fail(1000) end)
    Test_Report(ok, before == Read())
end
function Quota() Add(1000) Spawn() while true do end end
function DispatchFailure() Add(1000) Spawn() error('dispatch rollback') end
Register_Event('Bad', DispatchFailure)
)LUA";
    std::vector<std::string> baseline;
    std::string digest;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auth::SessionConfig config = default_config(); config.quotas.instructions_per_service = 10000;
        Session session = make_session({{"Main.lua", source}, {"Root.lua", "Add(7) Seen = Read()"},
            {"BadRoot.lua", "Add(999) Spawn() error('bad root')"}}, config);
        auto host = std::make_shared<WalletHost>();
        expect(static_cast<bool>(session.scheduler->register_binding("Self", [](auth::BindingContext& context, const auth::ValueList&) {
            return eawr::core::Result<auth::ValueList>::success({auth::Value::number(LuaNumber(context.instance()))});
        })), "register instance identity");
        expect(static_cast<bool>(session.scheduler->set_immediate_effect_host(host, 4)), "register bounded immediate host");
        for (const auto& action : {"Add", "Fail", "Spawn"}) {
            const std::string verb = action == std::string_view("Add") ? "add" : action == std::string_view("Fail") ? "fail" : "spawn";
            expect(static_cast<bool>(session.scheduler->register_binding(action, [verb](auth::BindingContext& context, const auth::ValueList& args) {
                return context.apply_immediate(verb, args);
            })), "register immediate action");
        }
        for (const auto& name : {"Read", "Count"}) {
            const std::string query = name == std::string_view("Read") ? "credits" : "count";
            expect(static_cast<bool>(session.scheduler->register_binding(name, [query](auth::BindingContext& context, const auth::ValueList& args) {
                return context.query_immediate(query, args);
            })), "register immediate query");
        }
        expect(!session.scheduler->create_instance(8, "BadRoot.lua") && host->state.balance == 0 && host->state.next_id == 1,
            "failed root discards cash and identity allocation");
        expect(static_cast<bool>(session.scheduler->create_instance(9, "Root.lua")) && host->state.balance == 7, "successful root publishes immediately");
        expect(std::get<LuaNumber>(session.scheduler->read_global(9, "Seen").value()->data) == LuaNumber(7), "root reads its write");
        expect(static_cast<bool>(session.scheduler->remove_instance(9)), "remove root probe");
        for (const std::uint64_t id : {3U, 2U, 1U}) expect(static_cast<bool>(session.scheduler->create_instance(id, "Main.lua")), "reverse instance creation");
        const auto post = [&](std::uint64_t id, std::uint64_t sequence, std::string name) {
            auth::ScriptEvent event; event.key = {1, auth::first_simulation_producer, id, sequence};
            event.target = id; event.kind = auth::ScriptEvent::Kind::call; event.name = std::move(name);
            expect(static_cast<bool>(session.scheduler->submit_event(std::move(event))), "post native contract event");
        };
        post(1, 0, "Good"); post(1, 1, "Failed"); post(1, 2, "Caught");
        auth::ScriptEvent dispatched = dispatch_event({1, auth::first_simulation_producer, 1, 3}, 1, "Bad", {});
        expect(static_cast<bool>(session.scheduler->submit_event(std::move(dispatched))), "post failed registered handler");
        post(2, 0, "Good"); post(2, 1, "Quota"); post(3, 0, "Good");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto result = session.scheduler->service(executor);
        expect(static_cast<bool>(result), "immediate service completes despite Lua error and quota fault");
        if (!result) continue;
        std::vector<std::string> commands;
        for (const auto& command : result.value().commands) commands.push_back(text(command));
        expect(host->state.balance == 11 && host->state.objects == std::vector<std::uint64_t>{1, 2} && host->state.next_id == 3,
            "failed handler and whole faulted instance discard all cash, objects and identities");
        expect(commands == std::vector<std::string>{"1#0 report 7 8 handle(9,1) 1", "1#1 report false true", "3#0 report 8 11 handle(9,2) 2"},
            "same-call reads and later canonical instance see successful writes only: " + joined(commands));
        expect(host->publications == 2, "one root publication and one whole-service publication");
        expect(result.value().diagnostics.size() == 3 && result.value().removed_instances == std::vector<std::uint64_t>{2}, "rollback diagnostics and quota removal");
        const auto hash = session.scheduler->state_digest();
        if (baseline.empty()) { baseline = commands; digest = hash; }
        else expect(commands == baseline && hash == digest, "immediate equality on one/two/four/eight workers");
        expect(!session.scheduler->set_immediate_effect_host(host, 1), "native bound rejects existing oversized set");
        host->refuse = true;
        auth::ScriptEvent event; event.key = {2, auth::first_simulation_producer, 1, 0};
        event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Good";
        expect(static_cast<bool>(session.scheduler->submit_event(event)), "post refused publish");
        const auto refused = session.scheduler->service(executor);
        expect(!refused && refused.error().code == auth::codes::session_abort && host->state.balance == 11 && host->state.next_id == 3,
            "failed atomic publication aborts without leaking native effects");
        expect(!session.scheduler->service(executor), "native publication failure is terminal");
    }
    {
        Session session = script_session("function Main() Add(5) coroutine.yield(true) Add(999) error('resume rollback') end Create_Thread('Main')");
        auto host = std::make_shared<WalletHost>();
        expect(static_cast<bool>(session.scheduler->register_binding("Add", [](auth::BindingContext& context, const auth::ValueList& args) {
            return context.apply_immediate("add", args);
        })), "register coroutine action");
        expect(static_cast<bool>(session.scheduler->set_immediate_effect_host(host, 1)), "coroutine native host");
        const eawr::sim::InlineExecutor executor;
        expect(static_cast<bool>(session.scheduler->create_instance(1, "Main.lua")), "create native coroutine");
        expect(static_cast<bool>(session.scheduler->service(executor)) && host->state.balance == 5, "successful yield publishes writes");
        const auto saved = session.scheduler->save();
        expect(static_cast<bool>(saved), "save native coroutine at barrier");
        expect(static_cast<bool>(session.scheduler->service(executor)) && host->state.balance == 5, "failed resume discards only current resume");
        expect(host->publications == 1, "rolled-back service needs no native publication");
        expect(saved && static_cast<bool>(session.scheduler->load(saved.value())), "load retains runtime immediate host");
        expect(static_cast<bool>(session.scheduler->service(executor)) && host->state.balance == 5, "restored coroutine has the same native rollback");
    }
    {
        Session session = script_session("Immediate()");
        expect(static_cast<bool>(session.scheduler->register_binding("Immediate", [](auth::BindingContext& context, const auth::ValueList& args) {
            return context.apply_immediate("add", args);
        })), "register unconfigured native action");
        const auto rejected = session.scheduler->create_instance(1, "Main.lua");
        expect(!rejected && rejected.error().message.find(auth::codes::missing_api) != std::string::npos, "unconfigured profile fails explicitly");
    }
}
} // namespace

// ---- scheduler ----

void run_scheduler() {
    immediate_contracts();
    const eawr::sim::InlineExecutor inline_executor;
    {
        const std::map<std::string, std::string> modules{{"Cache.lua", R"(
local hidden = 0
local shared = {value = 0}
function Change() hidden = hidden + 1; shared.value = hidden end
function Read() Test_Report(hidden, shared.value) end
)"}};
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter executor(workers);
            auto original = make_session(modules);
            auto restored = make_session(modules);
            expect(original.scheduler->create_instance(1, "Cache.lua").has_value(), "sync-cache instance with closed-over state");
            auth::ServiceOptions options; options.host_paced = [](std::uint64_t) { return true; };
            std::vector<std::string> hashes;
            for (std::uint64_t frame = 1; frame <= 16; ++frame) {
                if (frame % 3 == 1) {
                    auth::ScriptEvent event; event.key = {frame, 16, 1, 0}; event.target = 1;
                    event.kind = auth::ScriptEvent::Kind::call; event.name = "Change";
                    expect(original.scheduler->submit_event(event).has_value(), "sync mutation event accepted");
                    if (frame > 4) expect(restored.scheduler->submit_event(event).has_value(), "resumed sync mutation event accepted");
                }
                const auto encodes = original.scheduler->sync_graph_encodes();
                expect(original.scheduler->service(executor, options).has_value(), "sync original service");
                const auto digest = original.scheduler->sync_hash().value();
                if (frame == 2) expect(original.scheduler->sync_graph_encodes() == encodes, "paced quiet tick does no Lua graph serialization");
                expect(original.scheduler->sync_hash(true).value() == digest, "cached graph equals fresh full graph oracle including hidden upvalues");
                hashes.push_back(digest);
                if (frame == 4) {
                    expect(restored.scheduler->load(original.scheduler->save().value()).has_value(), "mid-window complete Lua restore");
                    expect(restored.scheduler->sync_hash().value() == digest, "Lua restored sync digest ignores local cache history");
                } else if (frame > 4) {
                    expect(restored.scheduler->service(executor, options).has_value(), "sync resumed service");
                    expect(restored.scheduler->sync_hash().value() == digest, "Lua continuation sync is identical after restore");
                    expect(restored.scheduler->state_hash().value() == original.scheduler->state_hash().value(), "full Lua archive oracle remains identical after restore");
                }
            }
            if (reference.empty()) reference = hashes; else expect(reference == hashes, "actual-state sync hashes agree on 1/2/4/8 workers");
        }
    }
    {
        auto config = default_config(); config.query_clock = true;
        auto session = make_session({{"Query.lua", "Created_At = GetCurrentTime()\nfunction Query() Test_Report(GetCurrentTime(), GetCurrentTime.Frame()) end\n"},
            {"Late.lua", "Created_At = GetCurrentTime()\n"}}, config);
        expect(session.scheduler->create_instance(1, "Query.lua").has_value(), "query clock instance");
        expect(std::get<LuaNumber>(session.scheduler->read_global(1, "Created_At").value()->data) == LuaNumber(0), "query root before any service sees zero");
        const auto before = session.scheduler->state_hash().value();
        expect(!session.scheduler->service(inline_executor) && session.scheduler->state_hash().value() == before,
            "query clock requires upstream time without advancing scheduler state");
        auth::ScriptEvent event; event.kind = auth::ScriptEvent::Kind::call;
        event.key = {1, 16, 1, 0}; event.target = 1; event.name = "Query";
        expect(session.scheduler->submit_event(event).has_value(), "sparse query event");
        auth::ServiceOptions options; options.time_tick = 9000;
        auto report = session.scheduler->service(inline_executor, options);
        expect(report && report.value().commands.size() == 1
            && text(report.value().commands.front()) == "1#0 report 300 9000"
            && session.scheduler->completed_tick() == 1, "query time advances independently of invocation count");
        expect(session.scheduler->create_instance(2, "Late.lua").has_value(), "late query root");
        expect(std::get<LuaNumber>(session.scheduler->read_global(2, "Created_At").value()->data) == LuaNumber(300), "root chunk reads last supplied query time");
        options.time_tick = 8999;
        const auto monotonic = session.scheduler->state_hash().value();
        expect(!session.scheduler->service(inline_executor, options) && session.scheduler->state_hash().value() == monotonic,
            "backwards query time refuses before changing state");
        options.time_tick = auth::max_script_tick;
        const auto valid = session.scheduler->state_hash().value();
        expect(!session.scheduler->service(inline_executor, options) && session.scheduler->state_hash().value() == valid,
            "query clock rejects out-of-format time before service");
        config.galactic_clock = true;
        expect(!auth::ScriptScheduler::create(config, {}), "clock profiles are mutually exclusive");
        config.galactic_clock = false; config.tick_duration = {1, 60};
        expect(!auth::ScriptScheduler::create(config, {}), "query clock requires logical 1/30 second frames");
    }

    // C-04 and FoC's same-pump rule for threads created during a pump.
    {
        Session session = script_session(
            "function A(...) Test_Report('A', arg.n) coroutine.yield(true) Test_Report('A again', arg.n) coroutine.yield(true) end\n"
            "function B() Test_Report('B') return true end\n"
            "function C() Test_Report('C') error('C fails') end\n"
            "function Spawner() Test_Report('spawn', Create_Thread('Late')) end\n"
            "function Late() Test_Report('late in first pump', GetThreadID()) end\n"
            "Test_Report(Create_Thread('A', 7, 8), Create_Thread('B'), Create_Thread('C'), Create_Thread('Spawner'), GetThreadID())\n");
        expect(run_script(session, 2).empty(), "C-04 create");
        expect(session.commands == std::vector<std::string>{
                   "1#0 report 0 1 2 3 -1", "1#1 report A 1", "1#2 report B", "1#3 report spawn 4",
                   "1#4 report late in first pump 4", "1#5 report A again 1", "1#6 report B"},
               "C-04 thread order\n" + joined(session.commands));
        expect(any_contains(session.diagnostics, "C fails"), "C-04 error diagnostic");
    }

    // L-23 retail rule (keep_slot_after_true_return): the topmost value decides,
    // and a slot that returned true starts again from the top without its parameter.
    {
        Session session = script_session(
            "runs = 0\n"
            "function Restart(...) runs = runs + 1 Test_Report('restart', runs, arg.n) return true end\n"
            "function LastTrue() Test_Report('last true') coroutine.yield(false, true) Test_Report('kept') coroutine.yield(false) end\n"
            "function LastFalse() Test_Report('last false') coroutine.yield(true, false) Test_Report('never') end\n"
            "function NoValue() Test_Report('no value') coroutine.yield() Test_Report('never') end\n"
            "Create_Thread('Restart', 1) Create_Thread('LastTrue') Create_Thread('LastFalse') Create_Thread('NoValue')\n");
        expect(run_script(session, 3).empty(), "L-23 create");
        expect(session.commands == std::vector<std::string>{
                   "1#0 report restart 1 1", "1#1 report last true", "1#2 report last false", "1#3 report no value",
                   "1#4 report restart 2 0", "1#5 report kept", "1#6 report restart 3 0"},
               "L-23 retail pump rule\n" + joined(session.commands));
    }

    // require sets _REQUIREDNAME while the module runs and restores it.
    {
        Session session = make_session({{"Main.lua", "_REQUIREDNAME = 'outer'\nTest_Report(require('Mod'), _REQUIREDNAME)\n"},
                                        {"Library/Mod.lua", "Test_Report(_REQUIREDNAME)\nreturn 5\n"}});
        expect(run_script(session, 1).empty(), "require create");
        expect(session.commands == std::vector<std::string>{"1#0 report Mod", "1#1 report 5 outer"},
               "_REQUIREDNAME\n" + joined(session.commands));
    }

    // C-05.
    {
        Session session = script_session(
            "function A() Test_Report('kill self', Create_Thread.Kill(GetThreadID())) Create_Thread.Kill_All() "
            "coroutine.yield(true) Test_Report('A still live') coroutine.yield(true) end\n"
            "function B() Test_Report('B ran') coroutine.yield(true) end\n"
            "Create_Thread('A') Create_Thread('B') Create_Thread('B')\n");
        expect(run_script(session, 2).empty(), "C-05 create");
        expect(session.commands == std::vector<std::string>{"1#0 report kill self", "1#1 report A still live"},
               "C-05\n" + joined(session.commands));
    }

    // C-06: independent callback and parameter FIFOs, skew and reset.
    {
        Session session = script_session(
            "function Red() end\n"
            "function Blue() end\n"
            "local function count(...) return arg.n end\n"
            "function Reader()\n"
            "  coroutine.yield(true)\n"
            "  local a, b = GetEvent(), GetEvent()\n"
            "  local p, q = GetEvent.Params(), GetEvent.Params()\n"
            "  Test_Report(a == Red, b == Blue, p[1], q[1], count(GetEvent()), count(GetEvent.Params()))\n"
            "  coroutine.yield(true)\n"
            "  local skew = GetEvent()\n"
            "  Test_Report('skew', skew == Red, count(GetEvent.Params()))\n"
            "  GetEvent.Reset()\n"
            "  Test_Report('after reset', count(GetEvent()), count(GetEvent.Params()))\n"
            "  coroutine.yield(true)\n"
            "end\n"
            "Create_Thread('Reader')\n"
            "Test_Signal(1, 0, 'Red', {10}) Test_Signal(1, 0, 'Blue', {20})\n"
            "function Later() Test_Signal(1, 0, 'Red') Test_Signal(1, 0, 'Blue', {30}) end\n"
            "Register_Event('later', Later)\n");
        expect(run_script(session, 1).empty(), "C-06 create");
        const eawr::sim::InlineExecutor executor;
        expect(session.scheduler->submit_event(dispatch_event({2, auth::first_simulation_producer, 0, 0}, 1, "later", {})).has_value(),
               "C-06 submit");
        step(session, executor, 3);
        expect(session.commands == std::vector<std::string>{
                   "1#0 report true true 10 20 0 0", "1#1 report skew true 0", "1#2 report after reset 0 0"},
               "C-06\n" + joined(session.commands));
    }

    // C-07: Register_Event mutation restart.
    {
        Session session = script_session(
            "calls = ''\n"
            "cancelled = false\n"
            "function A() calls = calls .. 'A' end\n"
            "function B() calls = calls .. 'B' if not cancelled then cancelled = true Cancel_Event('range', C) end end\n"
            "function C() calls = calls .. 'C' end\n"
            "function D() calls = calls .. 'D' end\n"
            "function Report() Test_Report(calls) end\n"
            "Register_Event('range', A) Register_Event('range', B) Register_Event('range', C) Register_Event('range', D)\n"
            "Register_Event('report', Report)\n");
        expect(run_script(session, 0).empty(), "C-07 create");
        expect(session.scheduler->submit_event(dispatch_event({1, 20, 5, 0}, 1, "range", {})).has_value(), "C-07 submit");
        expect(session.scheduler->submit_event(dispatch_event({1, 20, 6, 0}, 1, "report", {})).has_value(), "C-07 submit report");
        step(session, inline_executor);
        expect(session.commands == std::vector<std::string>{"1#0 report ABBD"}, "C-07\n" + joined(session.commands));
    }

    // Canonical event order and timer ties: (tick, producer, entity, sequence);
    // timers by (deadline, instance, registration).
    {
        Session session = make_session({
            {"One.lua", "function Log(tag) Test_Report(tag, GetCurrentTime.Frame()) end\nRegister_Event('log', Log)\n"
                        "Test_Timer(0.2, 'log', 'one-a') Test_Timer(0.1, 'log', 'one-b') Test_Timer(0.2, 'log', 'one-c')\n"},
            {"Two.lua", "function Log(tag) Test_Report(tag, GetCurrentTime.Frame()) end\nRegister_Event('log', Log)\n"
                        "Test_Timer(0.2, 'log', 'two-a')\n"},
        });
        expect(session.scheduler->create_instance(2, "Two.lua").has_value(), "timers create 2");
        expect(session.scheduler->create_instance(1, "One.lua").has_value(), "timers create 1");
        using auth::EventKey;
        for (const auto& [key, tag] : std::vector<std::pair<EventKey, std::string>>{
                 {{3, 40, 1, 0}, "p40"}, {{3, 17, 9, 1}, "p17-e9-s1"}, {{3, 17, 9, 0}, "p17-e9-s0"}, {{3, 17, 2, 5}, "p17-e2"},
                 {{2, 99, 0, 0}, "early"}}) {
            expect(session.scheduler->submit_event(dispatch_event(key, 1, "log", {auth::Value::text(tag)})).has_value(),
                   "submit " + tag);
        }
        const eawr::sim::InlineExecutor executor;
        step(session, executor, 8);
        const std::vector<std::string> expected{
            "1#0 report early 1",  "1#1 report p17-e2 2", "1#2 report p17-e9-s0 2", "1#3 report p17-e9-s1 2",
            "1#4 report p40 2",    "1#5 report one-b 4",  "1#6 report one-a 7",     "1#7 report one-c 7",
            "2#0 report two-a 7"};
        expect(session.commands == expected, "event and timer order\n" + joined(session.commands));
    }

    // GetCurrentTime and thread values.
    expect_runs(
        "Test_Report(GetCurrentTime(), GetCurrentTime.Frame(), ThreadValue('x'))\n"
        "ThreadValue.Set('x', 1)\n"
        "Test_Report(ThreadValue('x'))\n",
        "time and thread values outside threads", {"1#0 report 0 0", "1#1 report"});
    {
        Session session = script_session(
            "function T() ThreadValue.Set('x', 5) while true do Test_Report(GetCurrentTime.Frame(), ThreadValue('x')) "
            "coroutine.yield(true) end end\nCreate_Thread('T')\n");
        const eawr::sim::InlineExecutor executor;
        expect(run_script(session, 31).empty(), "time create");
        expect(session.commands.size() == 31 && session.commands[30] == "1#30 report 30 5", "frame 30");
    }
    expect_runs(
        "function T() while true do Test_Report(GetCurrentTime()) coroutine.yield(true) end end\n"
        "Create_Thread('T')\n",
        "time at tick 0");

    // Transactions: a failed resume publishes nothing and consumes no draws.
    {
        Session session = script_session(
            "function Bad() Test_Report('bad', Test_Random()) error('boom') end\n"
            "function Good() Test_Report('good', Test_Random()) end\n"
            "Create_Thread('Bad') Create_Thread('Good')\n");
        expect(run_script(session, 1).empty(), "rollback create");
        expect(session.commands.size() == 1 && contains(session.commands[0], "1#0 report good"), "rollback commands\n" + joined(session.commands));
        Session reference = script_session("function Good() Test_Report('good', Test_Random()) end\nCreate_Thread('Good')\n");
        expect(run_script(reference, 1).empty(), "rollback reference");
        expect(session.commands == reference.commands, "rolled-back draw is reused\n" + joined(reference.commands));
    }

    // Quota failure: the instance is removed and none of its tick output is
    // published; other instances are unaffected.
    {
        Session session = make_session({
            {"Busy.lua", "function Fine() Test_Report('before') end\nfunction Loop() while true do end end\n"
                         "Create_Thread('Fine') Create_Thread('Loop')\n"},
            {"Calm.lua", "function Tick() while true do Test_Report('calm') coroutine.yield(true) end end\nCreate_Thread('Tick')\n"},
        });
        expect(session.scheduler->create_instance(1, "Busy.lua").has_value(), "busy create");
        expect(session.scheduler->create_instance(2, "Calm.lua").has_value(), "calm create");
        const eawr::sim::InlineExecutor executor;
        step(session, executor, 2);
        expect(session.commands == std::vector<std::string>{"2#0 report calm", "2#1 report calm"}, "quota rollback\n" + joined(session.commands));
        expect(session.removed == std::vector<std::uint64_t>{1}, "faulted instance removed");
        expect(any_contains(session.diagnostics, std::string(auth::codes::instruction_budget)), "budget diagnostic");
        expect(session.scheduler->instances() == std::vector<std::uint64_t>{2}, "only the calm instance remains");
    }
    {
        auth::SessionConfig config = default_config();
        config.quotas.thread_slots = 4;
        Session session = script_session("function T() end\nfunction Spawn() for i = 1, 10 do Create_Thread('T') end end\n"
                                         "Create_Thread('Spawn')\n", config);
        expect(run_script(session, 1).empty(), "slot quota create");
        expect(session.removed == std::vector<std::uint64_t>{1} && any_contains(session.diagnostics, std::string(auth::codes::logical_quota)),
               "thread slot quota\n" + joined(session.diagnostics));
    }

    // _ScriptExit keeps the tick's output and ends the instance.
    {
        Session session = script_session(
            "function T() Test_Report('exiting') _ScriptExit() coroutine.yield(false) end\n"
            "function U() Test_Report('never') end\nCreate_Thread('T') Create_Thread('U')\n");
        expect(run_script(session, 2).empty(), "exit create");
        expect(session.commands == std::vector<std::string>{"1#0 report exiting"} && session.removed == std::vector<std::uint64_t>{1},
               "script exit\n" + joined(session.commands));
    }

    // Free_Random has no authoritative form.
    {
        Session session = script_session("");
        auto rejected = session.scheduler->register_binding(
            "GameRandom.Free_Random", [](auth::BindingContext&, const auth::ValueList&) {
                return eawr::core::Result<auth::ValueList>::success({});
            });
        expect(!rejected && rejected.error().code == auth::codes::forbidden_capability, "Free_Random is rejected");
    }
}

// ---- workers ----

struct Outcome {
    std::vector<std::string> digests;
    std::vector<std::string> commands;
};

Outcome run_scenario(const eawr::sim::PartitionExecutor& executor, bool reverse_creation, int ticks, int instances) {
    Session session = make_session({{"Library/Library.lua", auth::test::scenario_library}, {"Ai.lua", auth::test::scenario_ai}});
    std::vector<std::uint64_t> ids;
    for (int index = 1; index <= instances; ++index) ids.push_back(static_cast<std::uint64_t>(index) * 3);
    if (reverse_creation) std::reverse(ids.begin(), ids.end());
    for (const std::uint64_t id : ids) expect(session.scheduler->create_instance(id, "Ai.lua").has_value(), "scenario create");
    Outcome outcome;
    std::uint64_t sequence = 0;
    for (int tick = 1; tick <= ticks; ++tick) {
        for (const std::uint64_t id : ids) {
            if (tick % 7 != static_cast<int>(id % 7)) continue;
            auth::ValueList arguments{auth::Value{auth::Handle{7, id % 24}}, auth::Value::number(integer(tick))};
            expect(session.scheduler
                       ->submit_event(dispatch_event({static_cast<std::uint64_t>(tick), 30, id, sequence++}, id, "attacked", arguments))
                       .has_value(),
                   "scenario submit");
        }
        step(session, executor);
        outcome.digests.push_back(session.scheduler->state_digest());
    }
    outcome.commands = session.commands;
    expect(session.diagnostics.empty(), "scenario diagnostics\n" + joined(session.diagnostics));
    return outcome;
}

void run_workers() {
    constexpr int ticks = 90;
    constexpr int instances = 24;
    const eawr::sim::InlineExecutor inline_executor;
    const Outcome reference = run_scenario(inline_executor, false, ticks, instances);
    expect(reference.commands.size() > 100, "scenario issues commands");
    std::cout << "scenario: " << reference.commands.size() << " commands, final digest " << reference.digests.back() << '\n';
    for (const std::size_t workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const Outcome outcome = run_scenario(executor, false, ticks, instances);
        expect(outcome.digests == reference.digests, std::to_string(workers) + " workers: per-tick digests");
        expect(outcome.commands == reference.commands, std::to_string(workers) + " workers: commands");
    }
    // Storage perturbation: instance creation order does not change results.
    {
        const eawr::platform::ThreadWorkerAdapter executor(4);
        const Outcome outcome = run_scenario(executor, true, ticks, instances);
        expect(outcome.digests == reference.digests && outcome.commands == reference.commands, "reversed creation order");
    }
    // Allocation perturbation: shifted heap addresses and a different
    // collection schedule are unobservable.
    {
        std::vector<std::unique_ptr<char[]>> ballast;
        for (int index = 0; index < 4096; ++index) ballast.push_back(std::make_unique<char[]>(static_cast<std::size_t>(17 + (index * 37) % 509)));
        for (std::size_t index = 0; index < ballast.size(); index += 3) ballast[index].reset();
        const eawr::platform::ThreadWorkerAdapter executor(2);
        const Outcome outcome = run_scenario(executor, false, ticks, instances);
        expect(outcome.digests == reference.digests && outcome.commands == reference.commands, "allocation perturbation");
    }
    // Locale perturbation.
    for (const char* locale : {"", "de_DE.UTF-8", "German_Germany.1252", "tr_TR.UTF-8"}) {
        if (std::setlocale(LC_ALL, locale) == nullptr) continue;
        const Outcome outcome = run_scenario(inline_executor, false, ticks, instances);
        expect(outcome.digests == reference.digests && outcome.commands == reference.commands,
               std::string("locale '") + locale + "'");
        std::setlocale(LC_ALL, "C");
    }
}


} // namespace lua_sandbox_test_support
