// Contract tests for the authoritative Lua sandbox and scheduler (#247,
// docs/lua-sandbox.md).
//
//   lua_sandbox_tests clock_rng | sandbox | iteration | scheduler | workers

#include "eawr/core/sha256.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/clock_rng.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/script/numeric/binary64.hpp"
#include "eawr/sim/world.hpp"
#include "harness.hpp"
#include "scenario.hpp"
#include "sflua_sandbox.hpp"

#include <algorithm>
#include <clocale>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace eawr::script::authoritative::test;

// ---- clock_rng ----

void run_clock_rng() {
    const auth::TickDuration thirty{1, 30};
    expect(auth::time_at_tick(0, thirty).repr == 0, "time at tick 0");
    expect(auth::time_at_tick(30, thirty).repr == 0x3FF0000000000000ULL, "time at tick 30 is 1.0");
    expect(auth::time_at_tick(3, thirty).repr == 0x3FB999999999999AULL, "time at tick 3 rounds to binary64 0.1");
    auto ticks = [&](std::uint64_t bits) { return auth::ticks_until(number(bits), thirty); };
    expect(ticks(0x3FF0000000000000ULL) == 30u, "1 s is 30 ticks");
    // Binary64 0.1 is slightly above 1/10, so three ticks would fire early.
    expect(ticks(0x3FB999999999999AULL) == 4u, "0.1 s is 4 ticks");
    expect(ticks(0x3FB9999999999999ULL) == 3u, "the binary64 below 0.1 is 3 ticks");
    expect(ticks(0) == 1u && ticks(0x8000000000000000ULL) == 1u, "zero is the next service");
    expect(ticks(1) == 1u, "subnormal is one tick");
    expect(!ticks(0xBFF0000000000000ULL), "negative has no deadline");
    expect(!ticks(0x7FF0000000000000ULL) && !ticks(0x7FF8000000000000ULL), "infinity and NaN have no deadline");
    expect(!ticks(0x4330000000000000ULL), "2^52 s is beyond the tick range");
    expect(auth::ticks_until(number(0x4000000000000000ULL), auth::TickDuration{3, 2}) == 2u, "2 s at 1.5 s per tick");

    // Frozen draw vectors (rng v1): changing them is a new rng_identity.
    const std::uint64_t words[] = {
        auth::random_word(0, 0, 0, 0), auth::random_word(1, 0, 0, 0), auth::random_word(1, 1, 0, 0),
        auth::random_word(1, 1, 1, 0), auth::random_word(1, 1, 1, 1), auth::random_word(0x5EED, 30, 7, 3),
    };
    std::string listing;
    for (const std::uint64_t word : words) listing += hex(word) + " ";
    std::cout << "rng words: " << listing << '\n';
    expect(listing == "6F9D5D1A3F38B1EA 97A1D7A7DB0CD52C 052450DE171B15BE 3645E9299A9BCBC7 B303FB4D639B3A10 4E92E7E5FC7FBB0A ", "frozen rng words");
    auth::RandomStream stream(0x5EED, 1, 2);
    std::string draws;
    for (int index = 0; index < 4; ++index) draws += std::to_string(stream.next_below(6)) + " ";
    for (int index = 0; index < 2; ++index) draws += std::to_string(stream.next_in_range(-5, 5)) + " ";
    draws += hex(stream.next_unit().repr);
    draws += " " + std::to_string(stream.draws());
    std::cout << "rng draws: " << draws << '\n';
    expect(draws == "0 0 4 0 -3 1 3FD5267C336BECE0 7", "frozen bounded and unit draws");
    // Rejection sampling consumes a draw per rejected word.
    auth::RandomStream wide(9, 9, 9);
    const std::uint64_t bound = (std::uint64_t{1} << 63) + 1;
    std::uint64_t rejected = 0;
    for (int index = 0; index < 64; ++index) static_cast<void>(wide.next_below(bound));
    rejected = wide.draws() - 64;
    expect(rejected > 10, "a bound just above 2^63 rejects about half the words");
    std::string sequence;
    auth::RandomStream digest_stream(0x5EED, 77, 3);
    for (int index = 0; index < 10'000; ++index) {
        const std::uint64_t word = digest_stream.next_word();
        sequence.append(reinterpret_cast<const char*>(&word), sizeof(word));
    }
    const std::string digest = eawr::core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(sequence.data()), sequence.size()));
    std::cout << "rng digest: " << digest << '\n';
    expect(digest == "56315b90407906a10f4936744e66f4d55d7e494b1798a1620cdc3a904e40b72c", "frozen 10,000-word digest");
}

// ---- sandbox ----

void expect_create_error(const std::string& script, std::string_view code, const std::string& label) {
    Session session = script_session(script);
    const std::string error = run_script(session);
    expect(!error.empty() && contains(error, code), label + ": expected " + std::string(code) + ", got '" + error + "'");
}

void expect_runs(const std::string& script, const std::string& label, std::vector<std::string> expected_commands = {}) {
    Session session = script_session(script);
    const std::string error = run_script(session);
    expect(error.empty(), label + ": unexpected error '" + error + "'");
    if (!expected_commands.empty()) {
        expect(session.commands == expected_commands, label + ": commands\n" + joined(session.commands));
    }
}

// getn charges each element as it counts a table without a stored size: with
// the budget spent, a count over a long table faults at its first charge
// instead of walking the table first. Qualified calls: see sflua.hpp.
void expect_getn_charges_as_it_counts() {
    namespace sf = eawr::script::sflua;
    const std::unique_ptr<sf::Sandbox> sandbox = sf::Sandbox::open(sf::SandboxLimits{});
    expect(sandbox != nullptr, "getn: sandbox opens");
    if (sandbox == nullptr) return;
    sf::lua_State* state = sandbox->state();
    constexpr int entries = 100'000;
    sf::lua_pushstring(state, "table");
    sf::lua_rawget(state, LUA_GLOBALSINDEX);
    sf::lua_pushstring(state, "getn");
    sf::lua_rawget(state, -2);
    sf::lua_newtable(state);
    for (int index = 1; index <= entries; ++index) {
        sf::lua_pushboolean(state, 1);
        sf::lua_rawseti(state, -2, index);
    }
    const sf::Sandbox::Scope scope(*sandbox);
    sandbox->set_budget(entries);
    expect(sf::luaL_getn(state, -1) == entries, "getn: upstream count");
    expect(sandbox->budget() == 0 && sandbox->fault() == sf::SandboxFault::none, "getn: charges the count once");
    sandbox->set_budget(10);
    sf::lua_pushvalue(state, -2);
    sf::lua_pushvalue(state, -2);
    expect(sf::lua_pcall(state, 1, 1, 0) != 0, "getn: a spent budget raises");
    expect(sandbox->fault() == sf::SandboxFault::instruction_budget, "getn: instruction budget fault");
    expect(sandbox->budget() == -1, "getn: faults at the first charge past the budget, got " + std::to_string(sandbox->budget()));
}

void run_sandbox() {
    for (const char* script : {
             "pairs = nil",
             "rawset(_G, 'next', function() end)",
             "local g = getfenv(0); g.tostring = 1",
             "_G.Create_Thread = nil",
             "string.format = nil",
             "local s = string; rawset(s, 'len', nil)",
             "table.insert = nil",
             "coroutine.yield = nil",
             "rawset(getfenv(1), 'require', nil)",
             "_G['_G'] = {}",
         }) {
        expect_create_error(script, auth::codes::protected_binding, script);
    }
    expect_create_error("local t = setmetatable({}, {__mode = 'k'})", auth::codes::weak_or_finalizer, "weak table");
    expect_create_error("local mt = {}; mt['__' .. 'gc'] = print", auth::codes::weak_or_finalizer, "finalizer key");
    expect_create_error("local mt = {}; rawset(mt, '__mode', 'v')", auth::codes::weak_or_finalizer, "raw weak mode");
    for (const char* script : {"print('x')", "loadstring('return 1')", "dofile('Main.lua')", "loadfile('Main.lua')",
                               "gcinfo()", "newproxy(true)", "setfenv(0, {})", "setfenv(1, {})",
                               "string.dump(function() end)"}) {
        expect_create_error(script, auth::codes::forbidden_capability, script);
    }
    expect_create_error("local x = io.open", "", "io is absent");
    expect_create_error("require('Missing')", auth::codes::module_rejected, "unlisted module");
    expect_create_error("require('../Main')", auth::codes::module_rejected, "path escape");
    expect_create_error("local t = getmetatable(GetEvent); t.__index = nil", "", "binding metatable is guarded");
    expect_create_error("collectgarbage('count')", "bad argument", "collectgarbage options other than a number");
    expect_create_error("local x = {}; x[x] = 1; table.sort({3, 1, 2}, function(a, b) x[1] = 2; return a < b end)",
                        auth::codes::impure_comparator, "comparator writes a table");
    expect_create_error("local t = {3, 1, 2}; table.sort(t, function(a, b) t[1] = 9; return a < b end)",
                        auth::codes::impure_comparator, "comparator mutates the sorted table");
    expect_create_error("Hits = 0; table.sort({3, 1, 2}, function(a, b) Hits = Hits + 1; return a < b end)",
                        auth::codes::impure_comparator, "comparator writes a global");
    expect_create_error("while true do end", auth::codes::instruction_budget, "budget stops the chunk");
    expect_create_error("while true do pcall(function() while true do end end) end", auth::codes::instruction_budget,
                        "pcall cannot absorb a budget fault");
    expect_create_error("local co = coroutine.create(function() while true do end end); coroutine.resume(co)",
                        auth::codes::instruction_budget, "coroutines inherit the budget hook");
    expect_create_error("local f = coroutine.wrap(function() while true do end end); pcall(f)",
                        auth::codes::instruction_budget, "wrapped coroutines inherit the budget hook");
    expect_create_error("local t = {}; for i = 1, 100000 do t[i] = i end; while true do for k in pairs(t) do end end",
                        auth::codes::instruction_budget, "pairs is charged to the budget");
    // table.setn sizes, `n' fields and far positions drive the table library's C
    // loops; each is charged before it runs (the sort copy, insert and remove
    // moves) or as it runs (foreachi calls, getn's count).
    for (const char* script : {
             "local t = {}; table.setn(t, 2000000000); table.insert(t, 1, 'x')",
             "local t = {}; table.setn(t, 2000000000); table.remove(t, 1)",
             "local t = {n = 2000000000}; table.remove(t, 1)",
             "table.insert({}, -2000000000, 'x')",
             "local t = {}; table.setn(t, 2000000000); table.sort(t)",
             "local t = {}; table.setn(t, 2000000000); table.foreachi(t, collectgarbage)",
             "local t = {}; for i = 1, 50000 do t[i] = i end; while true do table.getn(t) end",
             "local t = {}; for i = 1, 50000 do t[i] = 'x' end; while true do table.concat(t) end",
             "local s = string.rep('', 2000000000)",
         }) {
        expect_create_error(script, auth::codes::instruction_budget, script);
    }
    expect_getn_charges_as_it_counts();
    expect_runs(
        "local t = {'a', 'b', 'c'}; table.insert(t, 2, 'x'); Test_Report(table.concat(t, ','))\n"
        "Test_Report(table.remove(t, 1), table.concat(t, '-', 2, 3), table.getn(t))\n"
        "table.foreachi(t, function(i, v) Test_Report(i, v) end)\n"
        "Test_Report(table.foreachi(t, function(i, v) if i == 2 then return v end end))\n"
        "local u = {}; table.setn(u, 3); table.insert(u, 'z'); Test_Report(table.getn(u), u[4])\n"
        "Test_Report(pcall(function() return table.concat({1, {}}) end))\n"
        "Test_Report(pcall(function() return table.insert({}, 'k', 1) end))\n"
        "Test_Report(string.rep('ab', 3), string.rep('x', -2) == '', pcall(function() return string.rep('x') end))\n",
        "metered table library keeps upstream results and messages",
        {"1#0 report a,x,b,c", "1#1 report a b-c 3", "1#2 report 1 x", "1#3 report 2 b", "1#4 report 3 c",
         "1#5 report b", "1#6 report 4 z",
         "1#7 report false MAIN.LUA:6: bad argument #1 to `concat' (table contains non-strings)",
         "1#8 report false MAIN.LUA:7: bad argument #2 to `insert' (number expected, got string)",
         "1#9 report ababab true false MAIN.LUA:8: bad argument #2 to `rep' (number expected, got no value)"});

    expect_runs(
        "collectgarbage(256); collectgarbage()\n"
        "Test_Report(type(io), type(os), type(debug), type(math), type(package))\n"
        "Test_Report(pcall(rawset, _G, 'pairs', nil), type(pairs))\n"
        "Test_Report(getmetatable(GetEvent), getmetatable(Test_Handle(1, 2)))\n"
        "local t = {3, 1, 2, 'x'}; table.remove(t); table.sort(t); Test_Report(t[1], t[2], t[3], table.getn(t))\n"
        "local u = {}; table.insert(u, 'a'); table.insert(u, 'b'); Test_Report(table.getn(u))\n"
        "Test_Report(string.upper('abc\\228'), string.format('%5.2f|%d|%g', 2.5, 3e9, 0.1))\n",
        "allowed library surface",
        {"1#0 report nil nil nil nil nil", "1#1 report false function",
         "1#2 report EAWR binding EAWR host handle", "1#3 report 1 2 3 3", "1#4 report 2",
         "1#5 report ABC\xE4  2.50|-2147483648|0.1"});

    // tostring prints identities in the retail shape, never addresses.
    expect_runs(
        "local f, g = function() end, function() end\n"
        "local a, b = tostring(f), tostring(g)\n"
        "Test_Report(a, b, tostring(f) == a, tostring({}), tostring(coroutine.create(f)))\n",
        "tostring identities",
        {"1#0 report function: 0000000000000001 function: 0000000000000002 true table: 0000000000000003 "
         "thread: 0000000000000004"});

    // Identity quota.
    auth::SessionConfig small = default_config();
    small.quotas.identities = 8;
    Session limited = script_session("local t = {}; for i = 1, 20 do t[function() end] = i end", small);
    expect(contains(run_script(limited), auth::codes::identity_quota), "identity quota");

    // Host handles print (kind, id) and use no identities, as keys or through tostring.
    small.quotas.identities = 4;
    Session handles = script_session(
        "local t, n = {}, 0\n"
        "for kind = 1, 3 do for i = 1, 70 do local h = Test_Handle(kind, i); tostring(h); t[h] = i end end\n"
        "for k in pairs(t) do n = n + 1 end\n"
        "Test_Report(n, tostring(Test_Handle(1, 20)), tostring(Test_Handle(2, 5)) < tostring(Test_Handle(10, 1)))\n"
        "Test_Report(tostring(Test_Handle(1, 20)) == tostring(Test_Handle(1, 20)), tostring({}))\n",
        small);
    const std::string handle_error = run_script(handles);
    expect(handle_error.empty(), "handles use no identities: '" + handle_error + "'");
    expect(handles.commands == std::vector<std::string>{
               "1#0 report 210 userdata: 00000001:0000000000000014 true", "1#1 report true table: 0000000000000001"},
           "handle tostring\n" + joined(handles.commands));

    // Bindings cannot take a name the sandbox or the infrastructure installs.
    Session reserved = script_session("Test_Report(type(pairs), type(string.format), type(_G), Custom.Probe ~= nil)");
    const auto noop = [](auth::BindingContext&, const auth::ValueList&) {
        return eawr::core::Result<auth::ValueList>::success({});
    };
    for (const char* name : {
             "pairs", "next", "tostring", "collectgarbage",                  // replaced base functions
             "print", "loadstring", "dofile", "newproxy", "setfenv",           // forbidden stubs
             "type", "assert", "unpack", "rawset", "getfenv", "_G", "_VERSION", // base library
             "string", "string.Foo", "table.Foo", "coroutine.Foo",             // library tables
             "require", "Create_Thread.Foo", "GetEvent",                       // infrastructure
         }) {
        const auto result = reserved.scheduler->register_binding(name, noop);
        expect(!result && result.error().code == auth::codes::invalid_request && contains(result.error().message, name),
               std::string("reserved binding name ") + name);
    }
    expect(reserved.scheduler->register_binding("Custom.Probe", noop).has_value(), "unreserved binding name");
    expect(run_script(reserved).empty(), "reserved-name session create");
    expect(reserved.commands == std::vector<std::string>{"1#0 report function function table true"},
           "rejected bindings leave the globals intact\n" + joined(reserved.commands));
}

// ---- iteration ----

void run_iteration() {
    expect_runs(
        "local t = {}\n"
        "local f2, f1 = function() end, function() end\n"
        "t[f2] = 'f2'; t[10] = 10; t['b'] = 'b'; t[true] = 't'; t[-1] = -1; t['ab'] = 'ab'; t[2.5] = 2.5\n"
        "t[f1] = 'f1'; t[''] = 'empty'; t[false] = 'f'; t[3] = 3; t['a'] = 'a'; t[Test_Handle(2, 5)] = 'h25'\n"
        "t[Test_Handle(1, 9)] = 'h19'; t[Test_Handle(2, 1)] = 'h21'\n"
        "local order = ''\n"
        "for k, v in pairs(t) do order = order .. tostring(v) .. ',' end\n"
        "Test_Report(order)\n"
        "local by_next = ''\n"
        "local k, v = next(t)\n"
        "while k ~= nil do by_next = by_next .. tostring(v) .. ','; k, v = next(t, k) end\n"
        "Test_Report(by_next == order)\n"
        "local generic = ''\n"
        "for k, v in t do generic = generic .. tostring(v) .. ',' end\n"
        "Test_Report(generic == order)\n"
        "local each = ''\n"
        "table.foreach(t, function(k, v) each = each .. tostring(v) .. ',' end)\n"
        "Test_Report(each == order)\n",
        "canonical order",
        {"1#0 report f,t,-1,2.5,3,10,empty,a,ab,b,h19,h21,h25,f2,f1,", "1#1 report true", "1#2 report true",
         "1#3 report true"});

    expect_runs(
        "local t = {a = 1, b = 2, c = 3, d = 4}\n"
        "local seen = ''\n"
        "for k, v in pairs(t) do\n"
        "  seen = seen .. k .. v .. ','\n"
        "  if k == 'a' then t.c = nil; t.b = 20; t.aa = 5; t.e = 6 end\n"
        "end\n"
        "Test_Report(seen)\n"
        "Test_Report(next(t, 'c'), next(t, 'bb'), next(t, 'e'))\n"
        "local nested = ''\n"
        "for k in pairs({x = 1, y = 2}) do for j in pairs({1, 2}) do nested = nested .. k .. j end end\n"
        "Test_Report(nested)\n",
        "mutation during traversal",
        {"1#0 report a1,b20,d4,", "1#1 report d d nil", "1#2 report x1x2y1y2"});

    // Removing a reference key and inserting it again keeps its identity and
    // place; a key first used later sorts after it.
    expect_runs(
        "local first, second, third = function() end, function() end, function() end\n"
        "local t = {}\n"
        "t[second] = 'second'; t[first] = 'first'\n"
        "t[second] = nil; t[third] = 'third'; t[second] = 'second again'\n"
        "local order = ''\n"
        "for k, v in pairs(t) do order = order .. v .. ',' end\n"
        "Test_Report(order)\n",
        "removal and reinsertion order",
        {"1#0 report second again,first,third,"});

    // Upstream table.sort result for a comparator with ties.
    expect_runs(
        "local t = {}\n"
        "for i = 1, 12 do t[i] = {k = Test_Random(4), i = i} end\n"
        "table.sort(t, function(a, b) return a.k < b.k end)\n"
        "local out = ''\n"
        "for i = 1, 12 do out = out .. t[i].k .. ':' .. t[i].i .. ' ' end\n"
        "Test_Report(out)\n",
        "sort ties follow the upstream algorithm");
}

// ---- scheduler ----

void run_scheduler() {
    const eawr::sim::InlineExecutor inline_executor;

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

} // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "";
    if (mode == "clock_rng") {
        run_clock_rng();
    } else if (mode == "sandbox") {
        run_sandbox();
    } else if (mode == "iteration") {
        run_iteration();
    } else if (mode == "scheduler") {
        run_scheduler();
    } else if (mode == "workers") {
        run_workers();
    } else {
        std::cerr << "usage: lua_sandbox_tests clock_rng | sandbox | iteration | scheduler | workers\n";
        return 2;
    }
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "passed\n";
    return 0;
}
