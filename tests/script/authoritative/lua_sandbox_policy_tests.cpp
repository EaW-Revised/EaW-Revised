#include "lua_sandbox_support.hpp"
// Sandbox policy v4 (docs/lua-sandbox.md, "Names mods call"): gcinfo, collectgarbage, print and loadstring behave
// the same on every peer and worker count; newproxy is absent; the other forbidden names still fault.
//
//   lua_sandbox_tests policy

namespace lua_sandbox_test_support {
namespace {

// What one run of a script produced: everything a peer could compare.
struct PolicyRun {
    std::vector<std::string> commands, log, diagnostics, digests;
    std::vector<std::vector<std::int64_t>> loads; // per tick, per serviced instance
    std::vector<std::uint64_t> removed;
    bool operator==(const PolicyRun&) const = default;
};

// Creates `instances` instances of Main.lua, then per tick posts the next of `calls` (global function names) to
// each and services once.
PolicyRun run_policy(const std::string& source, const std::vector<std::string>& calls, int ticks, int instances,
                     const eawr::sim::PartitionExecutor& executor, auth::SessionConfig config = default_config()) {
    Session session = make_session({{"Main.lua", source}}, std::move(config));
    for (int id = 1; id <= instances; ++id) {
        auto created = session.scheduler->create_instance(static_cast<std::uint64_t>(id), "Main.lua");
        expect(created.has_value(), "policy create: " + (created ? std::string() : created.error().message));
    }
    PolicyRun run;
    std::uint64_t sequence = 0;
    for (int tick = 1; tick <= ticks; ++tick) {
        for (int id = 1; id <= instances; ++id) {
            auth::ScriptEvent event;
            event.key = {static_cast<std::uint64_t>(tick), auth::first_simulation_producer, static_cast<std::uint64_t>(id), sequence++};
            event.target = static_cast<std::uint64_t>(id);
            event.kind = auth::ScriptEvent::Kind::call;
            event.name = calls[static_cast<std::size_t>(tick - 1) % calls.size()];
            expect(session.scheduler->submit_event(std::move(event)).has_value(), "policy post");
        }
        auto report = session.scheduler->service(executor);
        expect(report.has_value(), "policy service: " + (report ? std::string() : report.error().message));
        if (!report) return run;
        std::vector<std::int64_t> loads;
        for (const auto& load : report.value().loads) loads.push_back(load.instructions);
        run.loads.push_back(std::move(loads));
        for (const auto& command : report.value().commands) run.commands.push_back(text(command));
        for (const auto& line : report.value().log) {
            run.log.push_back(std::to_string(line.instance) + " " + line.code + " " + line.message);
        }
        for (const auto& diagnostic : report.value().diagnostics) {
            run.diagnostics.push_back(std::to_string(diagnostic.instance) + " " + diagnostic.code + " " + diagnostic.message);
        }
        for (const std::uint64_t id : report.value().removed_instances) run.removed.push_back(id);
        run.digests.push_back(session.scheduler->state_digest());
    }
    return run;
}

// The run on the inline executor, then on 1/2/4/8 workers: every one must equal the first.
PolicyRun run_everywhere(const std::string& label, const std::string& source, const std::vector<std::string>& calls,
                         int ticks, int instances, auth::SessionConfig config = default_config()) {
    const eawr::sim::InlineExecutor inline_executor;
    PolicyRun reference = run_policy(source, calls, ticks, instances, inline_executor, config);
    for (const std::size_t workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const PolicyRun other = run_policy(source, calls, ticks, instances, executor, config);
        expect(other.digests == reference.digests, label + ": digests on " + std::to_string(workers) + " workers");
        expect(other.commands == reference.commands, label + ": commands on " + std::to_string(workers) + " workers");
        expect(other.log == reference.log, label + ": log on " + std::to_string(workers) + " workers");
        expect(other.diagnostics == reference.diagnostics, label + ": diagnostics on " + std::to_string(workers) + " workers");
        expect(other.loads == reference.loads, label + ": instruction counts on " + std::to_string(workers) + " workers");
        expect(other.removed == reference.removed, label + ": removals on " + std::to_string(workers) + " workers");
    }
    return reference;
}

std::string log_line(std::uint64_t instance, const std::string& message) {
    return std::to_string(instance) + " " + std::string(auth::codes::script_log) + " " + message;
}

void gcinfo_and_collectgarbage() {
    // 1: two constants, so a loop that waits for the heap figure to drop ends at once.
    expect_runs(
        "local a, b = gcinfo()\n"
        "Test_Report(a, b, tostring(gcinfo()), table.getn({gcinfo()}))\n"
        "local waited = 0\n"
        "while gcinfo() > 0 do waited = waited + 1 end\n"
        "Test_Report(waited)\n",
        "gcinfo is the constant 0, 0", {"1#0 report 0 0 0 2", "1#1 report 0"});
    // 2: collectgarbage accepts nothing or a number, and still rejects an option string.
    expect_runs("collectgarbage(); collectgarbage(256); collectgarbage(1024); Test_Report(collectgarbage())",
                "collectgarbage with no argument, 256 and 1024", {"1#0 report"});
    expect_create_error("collectgarbage('collect')", "bad argument", "collectgarbage with an option string");
    // The call a mod makes on every service tick.
    const PolicyRun service = run_everywhere(
        "gcinfo service", "function Service() if gcinfo() > 10000 then collectgarbage(128) end Test_Report(gcinfo()) end\n",
        {"Service"}, 3, 4);
    expect(service.removed.empty() && service.diagnostics.empty() && service.commands.size() == 12,
           "a service that reads gcinfo keeps running: " + joined(service.diagnostics));
}

void print_policy() {
    // 3a: print assigns no identity, calls no __tostring, and reaches the log as text.
    {
        Session session = script_session(
            "local t, f = {}, function() end\n"
            "local meta = {__tostring = function() Test_Report('__tostring called') return 'x' end}\n"
            "print('x', 1, 2.5, true, nil, t, f, Test_Handle(1, 2), coroutine.create(f), setmetatable({}, meta))\n"
            "print()\n"
            "Test_Report(tostring({}))\n");
        expect(run_script(session).empty(), "print creates the instance");
        expect(session.commands == std::vector<std::string>{"1#0 report table: 0000000000000001"},
               "print assigned no identity and called no __tostring\n" + joined(session.commands));
        expect(session.log == std::vector<std::string>{
                   std::string(auth::codes::script_log) + " x\t1\t2.5\ttrue\tnil\ttable\tfunction\tuserdata\tthread\ttable",
                   std::string(auth::codes::script_log) + " "},
               "print text: references print as their type name\n" + joined(session.log));
        expect(session.diagnostics.empty() && session.removed.empty(), "print raises no diagnostic");
    }
    // 3b: the saved state is the same with and without the call.
    {
        const std::string source =
            "function Noisy() local t = {} print('noisy', 1, t, Noisy, 'a longer line of text') Total = (Total or 0) + 1 end\n"
            "function Quiet() local t = {} Total = (Total or 0) + 1 end\n";
        const PolicyRun with = run_everywhere("print on", source, {"Noisy"}, 6, 3);
        const PolicyRun without = run_everywhere("print off", source, {"Quiet"}, 6, 3);
        expect(with.digests == without.digests, "print leaves the state digest unchanged");
        expect(!with.log.empty() && without.log.empty(), "only the printing run has log lines");
    }
    // 3c: floods never fault. The same line is reported first 3 times, then at each power of two.
    {
        const PolicyRun same = run_everywhere("same line flood", "function Flood() for i = 1, 1000 do print('same') end end\n",
                                              {"Flood"}, 1, 1);
        expect(same.removed.empty() && same.diagnostics.empty(), "a flood of one line does not fault");
        std::vector<std::string> expected{log_line(1, "same"), log_line(1, "same (occurrence 2)"), log_line(1, "same (occurrence 3)")};
        for (const int count : {4, 8, 16, 32, 64, 128, 256, 512}) {
            expected.push_back(log_line(1, "same (occurrence " + std::to_string(count) + ")"));
        }
        expect(same.log == expected, "same line: first 3, then powers of two\n" + joined(same.log));
        const PolicyRun distinct = run_everywhere(
            "distinct flood", "function Flood() for i = 1, 5000 do print('line', i) end end\n", {"Flood"}, 1, 1);
        expect(distinct.removed.empty() && distinct.diagnostics.empty(), "a flood of distinct lines does not fault");
        expect(distinct.log.size() >= 256 && distinct.log.size() < 1024, "distinct lines stop at the flood guard, got " +
                                                                            std::to_string(distinct.log.size()));
        // The per-service cap drops lines instead of faulting.
        auth::SessionConfig small = default_config();
        small.quotas.staged_diagnostics = 8;
        const PolicyRun capped = run_everywhere("capped flood", "function Flood() for i = 1, 100 do print('line', i) end end\n",
                                                {"Flood"}, 1, 1, small);
        expect(capped.log.size() == 8 && capped.removed.empty() && capped.diagnostics.empty(),
               "the per-service cap drops lines without a fault, got " + std::to_string(capped.log.size()));
        expect(capped.log.front() == log_line(1, "line\t1") && capped.log.back() == log_line(1, "line\t8"), "the cap keeps the first lines");
        // A long line is cut, and the whole length is still charged.
        const PolicyRun longer = run_everywhere("long line", "function Long() print(string.rep('x', 2000)) end\n", {"Long"}, 1, 1);
        expect(longer.log.size() == 1 && longer.log[0] == log_line(1, std::string(512, 'x') + "..."), "a long line is cut at 512 bytes");
        // print spends the budget like any work, and running out is the ordinary budget fault, never a print fault.
        const PolicyRun spent = run_everywhere("print spends the budget", "function Spend() while true do print('x') end end\n",
                                               {"Spend"}, 1, 2);
        expect(spent.removed == std::vector<std::uint64_t>{1, 2} && spent.log.empty() &&
                   any_contains(spent.diagnostics, std::string(auth::codes::instruction_budget)),
               "print in an endless loop ends at the instruction budget and drops its lines\n" + joined(spent.diagnostics));
    }
    // 3d: output of a call that rolls back is dropped.
    {
        const PolicyRun rolled = run_everywhere(
            "rollback",
            "function Good() print('kept') end\n"
            "function Bad() print('dropped') error('boom') end\n"
            "function Caught() print('caught') pcall(function() print('inner') error('x') end) end\n",
            {"Good", "Bad", "Caught"}, 3, 1);
        expect(rolled.log == std::vector<std::string>{log_line(1, "kept"), log_line(1, "caught"), log_line(1, "inner")},
               "a failed call drops its print output, a caught error keeps it\n" + joined(rolled.log));
        expect(any_contains(rolled.diagnostics, "boom") && rolled.removed.empty(), "the failed call is diagnosed, not retired");
    }
}

// A valid chunk of 2 * 2^doublings bytes (one long comment), built by doubling so the script itself costs a few dozen steps.
std::string doubled_source(int doublings) {
    return "function Compile() local s = '--' for i = 1, " + std::to_string(doublings) +
           " do s = s .. s end local f, message = loadstring(s) Test_Report(type(f), type(message)) end\n";
}

void loadstring_policy() {
    // 4: text compiles; the chunk runs in the sandbox's globals.
    expect_runs(
        "Test_Report(loadstring('return 1 + 1')())\n"
        "local f, message = loadstring('return +')\n"
        "Test_Report(f, type(message))\n"
        "Test_Report(pcall(loadstring('error(\"boom\")', '=mine')))\n"
        "loadstring('Made = 7')()\n"
        "Test_Report(Made, loadstring('return type(dofile)')(), loadstring('return type(newproxy)')())\n",
        "loadstring compiles text",
        {"1#0 report 2", "1#1 report nil string", "1#2 report false mine:1: boom", "1#3 report 7 function nil"});
    // The chunk's environment holds stubs, never the forbidden functions themselves.
    expect_create_error("loadstring(\"dofile('Main.lua')\")()", auth::codes::forbidden_capability, "a chunk cannot call dofile");
    expect_create_error("loadstring(\"setfenv(1, {})\")()", auth::codes::forbidden_capability, "a chunk cannot call setfenv");
    expect_create_error("loadstring(\"loadfile('Main.lua')\")()", auth::codes::forbidden_capability, "a chunk cannot call loadfile");
    expect_create_error("loadstring(\"string.dump(function() end)\")()", auth::codes::forbidden_capability, "a chunk cannot call string.dump");
    expect_create_error("loadstring('pairs = nil')()", auth::codes::protected_binding, "a chunk cannot replace a protected global");
    expect_create_error("loadstring('local t = setmetatable({}, {[\"__\" .. \"mode\"] = \"k\"})')()", auth::codes::weak_or_finalizer,
                        "a chunk cannot make a weak table");
    // The compiler's own constant table is a sandboxed table write, so a chunk that spells the name as a literal is
    // refused at compile time with the same code: loadstring returns nil and the message.
    expect_runs("local f, message = loadstring('local t = {__gc = 1}')\n"
                "Test_Report(f, string.find(message, 'EAWR-SCRIPT-0202', 1, true) ~= nil)\n",
                "a literal __gc does not compile", {"1#0 report nil true"});
    // A precompiled chunk is refused before the loader sees it.
    expect_create_error("loadstring('\\27Lua')", auth::codes::module_rejected, "a binary chunk is refused");
    expect_create_error("loadstring('\\27Lua', 'chunk')", auth::codes::module_rejected, "a binary chunk is refused with a name");
    expect_create_error("string.dump(loadstring('return 1'))", auth::codes::forbidden_capability,
                        "string.dump stays forbidden even for a function loadstring made");

    // A function loadstring made has no module to be recompiled from, so a save that still reaches one is refused
    // (never written as a chunk that cannot load); the stock use, running the chunk and keeping its result, saves.
    {
        Session used = script_session("Result = loadstring('return {1, 2}')()\n");
        expect(run_script(used).empty() && used.scheduler->save().has_value(), "a value built by loadstring saves");
        Session kept = script_session("Kept = loadstring('return 1')\n");
        expect(run_script(kept).empty(), "keeping a loadstring function is allowed");
        const auto refused = kept.scheduler->save();
        expect(!refused && contains(refused.error().message, "loadstring"),
               "a save that reaches a loadstring function is refused: " + (refused ? std::string("saved") : refused.error().message));
    }

    // The state hash the lockstep sync compares saves every instance's graph: one instance that keeps a loadstring
    // function refuses it for the whole scheduler (every instance of the session), with the reason named.
    {
        Session session = make_session({{"Keeper.lua", "Kept = loadstring('return 1')\n"}, {"Plain.lua", "Value = 1\n"}});
        expect(session.scheduler->create_instance(1, "Plain.lua").has_value() && session.scheduler->create_instance(2, "Keeper.lua").has_value(),
               "sync refusal instances");
        const eawr::sim::InlineExecutor executor;
        step(session, executor);
        const auto refused = session.scheduler->sync_hash(executor);
        expect(!refused && refused.error().code == auth::codes::save_refused && contains(refused.error().message, "loadstring"),
               "the sync hash is refused for the whole scheduler: " +
                   (refused ? std::string("hashed") : refused.error().message));
        Session plain = make_session({{"Plain.lua", "Value = loadstring('return 1')()\n"}});
        expect(plain.scheduler->create_instance(1, "Plain.lua").has_value(), "sync control instance");
        step(plain, executor);
        expect(plain.scheduler->sync_hash(executor).has_value(), "running a loadstring chunk and keeping its result syncs");
    }

    // A compile is charged one unit per source byte before it runs: a 1 MB source fits the budget of a service (4M)...
    const PolicyRun fits = run_everywhere("1 MB source", doubled_source(19), {"Compile"}, 1, 4);
    expect(fits.removed.empty() && fits.commands.size() == 4 && fits.commands[0] == "1#0 report function nil",
           "a 1 MB source compiles inside the budget: " + joined(fits.diagnostics));
    expect(!fits.loads.empty() && fits.loads[0][0] >= (std::int64_t{1} << 20), "the compile is charged to the budget");
    // ...and a 4 MB one exhausts it, at the same instruction on every peer and every worker count.
    const PolicyRun over = run_everywhere("4 MB source", doubled_source(21), {"Compile"}, 1, 8);
    expect(over.removed == std::vector<std::uint64_t>{1, 2, 3, 4, 5, 6, 7, 8} &&
               over.commands.empty() && over.diagnostics.size() == 8 &&
               std::all_of(over.diagnostics.begin(), over.diagnostics.end(), [](const std::string& line) {
                   return line.find(std::string(auth::codes::instruction_budget)) != std::string::npos;
               }),
           "a 4 MB source exhausts the instruction budget in every instance\n" + joined(over.diagnostics));
    expect(!over.loads.empty() && std::all_of(over.loads[0].begin(), over.loads[0].end(),
                                              [&](std::int64_t load) { return load == over.loads[0][0]; }),
           "every instance exhausts the budget at the same instruction");
}

void removed_and_forbidden_names() {
    // 5: newproxy is absent, as in the retail base library; the file, environment and dump functions still fault.
    expect_runs("Test_Report(type(newproxy), type(print), type(gcinfo), type(loadstring), type(collectgarbage))",
                "newproxy is nil", {"1#0 report nil function function function function"});
    expect_create_error("newproxy(true)", "attempt to call", "calling newproxy is an ordinary Lua error");
    for (const char* script : {"dofile('Main.lua')", "loadfile('Main.lua')", "setfenv(0, {})", "setfenv(1, {})",
                               "string.dump(function() end)"}) {
        expect_create_error(script, auth::codes::forbidden_capability, script);
    }
    // The host cannot bind over what the sandbox installs now, but newproxy is free again.
    Session session = script_session("Test_Report(type(newproxy))");
    const auto noop = [](auth::BindingContext&, const auth::ValueList&) { return eawr::core::Result<auth::ValueList>::success({}); };
    for (const char* name : {"print", "gcinfo", "loadstring"}) {
        const auto result = session.scheduler->register_binding(name, noop);
        expect(!result && result.error().code == auth::codes::invalid_request, std::string("reserved binding name ") + name);
    }
}

// 7: a script that calls print, gcinfo and loadstring on every tick, over many instances: equal everywhere.
void policy_equality() {
    const std::string source =
        "Count = 0\n"
        "function Tick()\n"
        "  Count = Count + 1\n"
        "  local a, b = gcinfo()\n"
        "  print('tick', Count, GetCurrentTime.Frame(), a, b)\n"
        "  local f, message = loadstring('return ' .. Test_Random(100) .. ' + ' .. Count)\n"
        "  local g = loadstring('return nope(')\n"
        "  Test_Report('v', f(), g, message, tostring({}))\n"
        "  collectgarbage(256)\n"
        "end\n";
    const PolicyRun run = run_everywhere("print, gcinfo and loadstring", source, {"Tick"}, 40, 24);
    expect(run.commands.size() == 40u * 24u && run.log.size() == 40u * 24u && run.diagnostics.empty() && run.removed.empty(),
           "every call reported and logged, none faulted: " + joined(run.diagnostics));
    expect(run.digests.size() == 40u, "a digest per tick");
}

} // namespace

void run_policy() {
    gcinfo_and_collectgarbage();
    print_policy();
    loadstring_policy();
    removed_and_forbidden_names();
    policy_equality();
}

} // namespace lua_sandbox_test_support
