#include "lua_sandbox_support.hpp"
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

namespace lua_sandbox_test_support {


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

void expect_runs(const std::string& script, const std::string& label, std::vector<std::string> expected_commands) {
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

} // namespace

using namespace lua_sandbox_test_support;

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
