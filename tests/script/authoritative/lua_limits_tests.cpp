// Metered pattern matching and the logical memory quota of the authoritative
// Lua sandbox (#375, docs/lua-sandbox.md).
//
//   lua_limits_tests patterns | foc_patterns | memory | memory_persistence | memory_workers
//
// patterns and foc_patterns compare the sandbox's metered string.find, gsub
// and gfind with the P0 runtime (upstream Lua 5.0.2) on the same Lua code;
// foc_patterns takes its patterns from the retail FoC scripts and skips
// unless EAWR_EAW_GAME_ROOT is set.

#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/sim/world.hpp"
#include "eawr/vfs/vfs.hpp"
#include "harness.hpp"
#include "p0_oracle.hpp"
#include "sflua_metering.hpp"
#include "sflua_sandbox.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace eawr::script::authoritative::test;

void run_metering_width() {
    namespace metering = eawr::script::sflua_metering;
    static_assert(sizeof(metering::StepUnits) == 8);
    constexpr metering::StepUnits capture_bytes = 80ULL * 1024 * 1024;
    constexpr metering::StepUnits copied_bytes = [=] {
        metering::StepUnits total = 0;
        for (int capture = 0; capture < 32; ++capture) total = metering::saturating_add(total, capture_bytes);
        return total;
    }();
    static_assert(copied_bytes > (1ULL << 31));
    expect(metering::charged_budget(2'600'000'000LL, copied_bytes) == -1,
           "32 large captures exhaust the budget instead of crediting it");
    expect(metering::charged_budget(3'000'000'000LL, copied_bytes) == 315'645'440LL,
           "large capture charge subtracts the same units on every platform");
    expect(metering::charged_budget(3'000'000'000LL, std::numeric_limits<metering::StepUnits>::max()) == -1,
           "unrepresentable charge exhausts without signed overflow");
    expect(metering::saturating_add(std::numeric_limits<metering::StepUnits>::max(), capture_bytes) ==
               std::numeric_limits<metering::StepUnits>::max(),
           "capture byte sum saturates");
}

// ---- Differential pattern runs ----

// Lua 5.0.2 code shared by both runtimes: `patterns` and `subjects` are
// defined before it; it leaves one line per call in `out`.
constexpr std::string_view differential_body = R"lua(
local lines, count = {}, 0
local function put(text) count = count + 1 lines[count] = text end
local function pack(...) return arg end
local function show(results)
  local parts = {}
  for index = 1, results.n do parts[index] = tostring(results[index]) end
  return table.concat(parts, ",")
end
local function call(f, a, b, c, d) return show(pack(pcall(f, a, b, c, d))) end
local function all_found(s, p)
  local parts, found = {}, 0
  for a, b, c in string.gfind(s, p) do
    found = found + 1
    parts[found] = tostring(a) .. "/" .. tostring(b) .. "/" .. tostring(c)
    if found > 64 then break end
  end
  return table.concat(parts, ";")
end
local function wrap(a, b) return "[" .. tostring(a) .. "|" .. tostring(b) .. "]" end
for pi = 1, table.getn(patterns) do
  local p = patterns[pi]
  for si = 1, table.getn(subjects) do
    local s = subjects[si]
    put(call(string.find, s, p))
    put(call(string.find, s, p, 3))
    put(call(string.find, s, p, -4))
    put(call(string.find, s, p, 1, 1))
    put(call(string.gsub, s, p, "<%1>"))
    put(call(string.gsub, s, p, "#", 2))
    put(call(string.gsub, s, p, wrap))
    put(call(all_found, s, p))
  end
end
out = table.concat(lines, "\n")
)lua";

// Quotes a byte string as a Lua 5.0 literal.
std::string lua_literal(std::string_view bytes) {
    std::string out = "\"";
    for (const char character : bytes) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '"' || character == '\\') {
            out += '\\';
            out += character;
        } else if (byte < 0x20 || byte >= 0x7F) {
            out += '\\' + std::to_string(byte);
        } else {
            out += character;
        }
    }
    return out + "\"";
}

std::string lua_list(const std::vector<std::string>& literals) {
    std::string out = "{";
    for (const std::string& literal : literals) out += literal + ",\n";
    return out + "}";
}

// Big enough for a whole corpus in one chunk; the corpus is ordinary work.
auth::SessionConfig roomy_config() {
    auth::SessionConfig config = default_config();
    config.quotas.instructions_per_service = 2'000'000'000;
    config.quotas.memory_bytes = std::uint64_t{1} << 30;
    return config;
}

// Runs the corpus on both runtimes; returns the number of result lines.
std::size_t compare_with_upstream(const std::vector<std::string>& patterns, const std::vector<std::string>& subjects,
                                  const std::string& label) {
    const std::string prelude = "patterns = " + lua_list(patterns) + "\nsubjects = " + lua_list(subjects) + "\n";
    const std::string code = prelude + std::string(differential_body);
    const std::string upstream = eawr::script::numeric::test::p0_run(code + "\nreturn out\n");
    Session session = script_session(code + "\nTest_Report(out)\n", roomy_config());
    const std::string error = run_script(session, 1);
    expect(error.empty(), label + ": sandbox run: " + error);
    expect(session.commands.size() == 1, label + ": one report");
    if (session.commands.size() != 1) return 0;
    // Error positions name the chunk: "test" in the P0 run, the module here.
    std::string sandbox = session.commands[0].substr(std::string_view("1#0 report ").size());
    for (std::size_t at = 0; (at = sandbox.find("MAIN.LUA:", at)) != std::string::npos;) sandbox.replace(at, 9, "test:");
    expect(upstream.rfind("error: ", 0) != 0, label + ": upstream run: " + upstream.substr(0, 200));
    if (sandbox != upstream) {
        std::size_t line = 1;
        std::size_t at = 0;
        while (at < sandbox.size() && at < upstream.size() && sandbox[at] == upstream[at]) {
            if (sandbox[at] == '\n') ++line;
            ++at;
        }
        expect(false, label + ": the sandbox differs from upstream at result line " + std::to_string(line) + "\n  upstream: " +
                          upstream.substr(at > 60 ? at - 60 : 0, 160) + "\n  sandbox:  " + sandbox.substr(at > 60 ? at - 60 : 0, 160));
    }
    std::size_t lines = 1;
    for (const char character : upstream) lines += character == '\n' ? 1 : 0;
    return lines;
}

const std::vector<std::string>& synthetic_subjects() {
    static const std::vector<std::string> subjects = {
        lua_literal(""),
        lua_literal("a"),
        lua_literal("TIE_FIGHTER_SQUADRON"),
        lua_literal("Rebel_X-Wing_Squadron 12"),
        lua_literal("  spaced  out text  "),
        lua_literal("key = value; other=42"),
        lua_literal("f(a(b)c)(d) [x] {y}"),
        lua_literal("aaaaab abab ba"),
        lua_literal("THE (quick) brown fox\n2nd line\t3.25"),
        lua_literal("%d%%[]^$.*+-?"),
        lua_literal("Data\\Scripts\\AI\\SpaceMode\\Plan.lua"),
        lua_literal(std::string("nul\0byte", 8)),
    };
    return subjects;
}

// ---- patterns ----

// A script that must fault with `code`, with none of its output published.
void expect_fault(const std::string& script, std::string_view code, const std::string& label,
                  auth::SessionConfig config = default_config(), int ticks = 1) {
    Session session = script_session(script, std::move(config));
    const std::string error = run_script(session, ticks);
    if (!error.empty()) {
        expect(contains(error, code), label + ": create error '" + error + "'");
        return;
    }
    expect(session.removed == std::vector<std::uint64_t>{1}, label + ": the instance is removed");
    expect(any_contains(session.diagnostics, code), label + ": " + std::string(code) + "\n" + joined(session.diagnostics));
    expect(session.commands.empty(), label + ": no output\n" + joined(session.commands));
}

void run_patterns() {
    std::vector<std::string> patterns;
    for (const char* pattern : {
             "", "a", "^a", "a$", "^$", ".", "..-", "%a+", "%A+", "%d+", "%D*", "%l", "%u+", "%s", "%S+", "%w+", "%W",
             "%x+", "%p+", "%c", "%z", "[%a_]+", "[^%s]+", "[a-f]+", "[^a-z]", "[%]]", "[]]", "[a-]", "[-a]", "[%-%%]",
             "^%s*(.-)%s*$", "(%w+)%s*=%s*(%w+)", "(%w+)_(%w+)", "()", "()a()", "(a*(.)%w(%s*))", "%b()", "%b[]",
             "%f[%w]%w+", "%f[%a]%a", "(a)%1", "(%a)%1*", "a?b", "a*b", "a-b", "a+b", "ab?", "x*", "x-", "$", "^",
             "%.", "%%", "%$", "%(", "a.-b", "[%w_]+$", "^(%u)(%l*)", "%s+", "Squadron$", "_", "\\",
             // Malformed or refused: the messages must be upstream's.
             "%", "[a", "[", "(", ")", "%1", "(()", "%b", "%ba", "%f", "%fx", "(a)%2", "%g",
         }) {
        patterns.push_back(lua_literal(pattern));
    }
    std::string captures;
    for (int index = 0; index < 33; ++index) captures += "(a*)";
    patterns.push_back(lua_literal(captures));
    const std::size_t lines = compare_with_upstream(patterns, synthetic_subjects(), "synthetic corpus");
    std::cout << "synthetic corpus: " << patterns.size() << " patterns, " << lines << " results equal upstream\n";

    // Pathological patterns: each faults with the budget error, within the
    // budget, and pcall cannot absorb it.
    const std::vector<std::pair<std::string, std::string>> pathological = {
        {"nested max_expand", "string.find(string.rep('a', 5000), string.rep('a*', 20) .. 'b')"},
        {"nested min_expand in gsub", "string.gsub(string.rep('a', 4000), 'a-a-a-a-b', 'x')"},
        {"gfind backtracking", "for w in string.gfind(string.rep('a', 3000), 'a*a*a*c') do end"},
        {"balance scan", "string.find(string.rep('(', 20000), '%b()')"},
        {"back reference", "string.find(string.rep('a', 20000), '(a*)%1b')"},
        {"long set", "string.find(string.rep('b', 20000), '[' .. string.rep('a', 20000) .. ']')"},
        {"plain find", "string.find(string.rep('a', 100000), string.rep('a', 50000) .. 'b', 1, 1)"},
        {"plain find without specials", "string.find(string.rep('a', 100000), string.rep('a', 50000) .. 'b')"},
        {"frontier", "string.find(string.rep('a', 20000), '%f[' .. string.rep('b', 20000) .. ']')"},
        {"replacement length", "string.gsub(string.rep('x', 100000), '', string.rep('y', 1000))"},
        // A short replacement copies each capture it names; a replacement
        // function's result is copied too (PR #380 review).
        {"capture replacement", "string.gsub(string.rep('x', 20000), '^(.*)$', string.rep('%1', 300))"},
        {"function replacement", "local r = string.rep('y', 60000) string.gsub(string.rep('x', 100), '.', function() return r end)"},
    };
    for (const auto& [label, call] : pathological) {
        expect_fault("local ok = pcall(function() " + call + " end)\nTest_Report('absorbed', ok)\n", auth::codes::instruction_budget,
                     label);
    }
    // The budget counts matcher steps: an ordinary find is a handful.
    {
        auth::SessionConfig config = default_config();
        config.quotas.instructions_per_service = 1'000;
        expect_fault("while true do string.find('TIE_FIGHTER', '_(%u+)$') end", auth::codes::instruction_budget, "loop of finds", config);
        Session session = script_session("for i = 1, 20 do string.find('TIE_FIGHTER', '_(%u+)$') end Test_Report('done')", config);
        expect(run_script(session).empty() && session.commands == std::vector<std::string>{"1#0 report done"},
               "twenty small finds fit a 1,000-instruction budget\n" + joined(session.diagnostics));
    }
}

// ---- foc_patterns ----

// Short string literals of a Lua 5.0 source, verbatim with their quotes
// (comments and long strings skipped), and the number of string.find, gsub
// and gfind calls.
void scan_literals(std::string_view source, std::set<std::string>& literals, std::size_t& pattern_calls) {
    const auto skip_to = [&](std::size_t from, std::string_view end) {
        const std::size_t found = source.find(end, from);
        return found == std::string_view::npos ? source.size() : found + end.size();
    };
    std::size_t at = 0;
    while (at < source.size()) {
        const char character = source[at];
        if (source.substr(at, 4) == "--[[") {
            at = skip_to(at + 4, "]]");
        } else if (source.substr(at, 2) == "--") {
            at = skip_to(at + 2, "\n");
        } else if (source.substr(at, 2) == "[[") {
            at = skip_to(at + 2, "]]");
        } else if (character == '"' || character == '\'') {
            std::size_t end = at + 1;
            while (end < source.size() && source[end] != character && source[end] != '\n') end += source[end] == '\\' ? 2 : 1;
            if (end < source.size() && source[end] == character) literals.insert(std::string(source.substr(at, end - at + 1)));
            at = end + 1;
        } else {
            for (const std::string_view call : {"string.find", "string.gsub", "string.gfind"}) {
                if (source.substr(at, call.size()) == call &&
                    (at + call.size() >= source.size() || std::isalnum(static_cast<unsigned char>(source[at + call.size()])) == 0)) {
                    ++pattern_calls;
                }
            }
            ++at;
        }
    }
}

std::optional<std::string> environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
#else
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    std::string result(value);
#endif
    if (result.empty()) return std::nullopt;
    return result;
}

int run_foc_patterns() {
    const std::optional<std::string> root = environment("EAWR_EAW_GAME_ROOT");
    if (!root) {
        std::cout << "SKIPPED: EAWR_EAW_GAME_ROOT is not set\n";
        return 0;
    }
    const std::filesystem::path game_root = *root;
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, data] : std::vector<std::pair<std::string, std::filesystem::path>>{
             {"expansion", game_root / "corruption" / "Data"}, {"base", game_root / "GameData" / "Data"}}) {
        auto resolved = eawr::vfs::resolve_manifest_mount(id, data);
        expect(resolved.has_value(), "mount " + id);
        if (!resolved) return 1;
        specs.push_back(std::move(resolved.value().mount));
    }
    auto mounted = eawr::vfs::Vfs::mount(specs);
    expect(mounted.has_value(), "FoC profile mounts");
    if (!mounted) return 1;
    auto scripts = mounted.value().enumerate("data/scripts", ".lua");
    expect(scripts.has_value() && !scripts.value().empty(), "FoC scripts are listed");
    if (!scripts) return 1;
    std::set<std::string> literals;
    std::size_t calls = 0;
    for (const eawr::vfs::AssetRecord& record : scripts.value()) {
        auto bytes = mounted.value().open(record.canonical_path);
        expect(bytes.has_value(), "read " + record.canonical_path);
        if (!bytes) continue;
        const std::string source(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
        scan_literals(source, literals, calls);
    }
    // The retail scripts call none of the pattern functions (370 scripts, only
    // string.format), so the corpus is every string literal they hold: each is
    // tried as a pattern against the synthetic subjects and a spread of the
    // literals themselves.
    std::cout << "FoC scripts: " << scripts.value().size() << ", string.find/gsub/gfind calls: " << calls
              << ", distinct string literals: " << literals.size() << std::endl;
    expect(literals.size() > 100, "the FoC scripts hold string literals");
    // Literals with many quantifiers (separator lines such as "-----") are
    // backtracking patterns: the P0 oracle has no budget and would run for
    // hours; the pathological cases in `patterns` cover them.
    std::vector<std::string> all;
    for (const std::string& literal : literals) {
        if (std::count_if(literal.begin(), literal.end(), [](char c) { return c == '*' || c == '+' || c == '-' || c == '?'; }) <= 3) {
            all.push_back(literal);
        }
    }
    std::cout << "FoC literals with at most three quantifiers: " << all.size() << std::endl;
    std::vector<std::string> subjects = synthetic_subjects();
    for (std::size_t index = 0; index < 4 && !all.empty(); ++index) subjects.push_back(all[index * all.size() / 4]);
    constexpr std::size_t batch = 400;
    std::size_t lines = 0;
    for (std::size_t first = 0; first < all.size() && failures == 0; first += batch) {
        const std::vector<std::string> patterns(all.begin() + static_cast<std::ptrdiff_t>(first),
                                                all.begin() + static_cast<std::ptrdiff_t>(std::min(all.size(), first + batch)));
        lines += compare_with_upstream(patterns, subjects, "FoC corpus from literal " + std::to_string(first));
    }
    std::cout << "FoC corpus: " << lines << " results equal upstream\n";
    return 0;
}

// ---- memory ----

auth::SessionConfig small_memory(std::uint64_t bytes) {
    auth::SessionConfig config = default_config();
    config.quotas.memory_bytes = bytes;
    config.quotas.instructions_per_service = 200'000'000;
    return config;
}

std::string many_locals(int count) {
    std::string names;
    for (int index = 0; index < count; ++index) names += (index == 0 ? "l" : ", l") + std::to_string(index);
    return "local " + names + "\n";
}

// `count` nested functions of about 300 logical bytes of prototype each.
std::string nested_functions(int count) {
    std::string out;
    for (int index = 0; index < count; ++index) out += "do local function f() local a, b, c = 1, 2, 3 return a + b + c end end\n";
    return out;
}

// The compiler charges each nested prototype and its arrays as it builds
// them (PR #380 review). Compiling each function below charges 368 logical
// bytes of prototype items (a child of 128 + 10 instructions + 8 constants,
// and the parent's closure instruction, local and child entry) on top of
// about 570 bytes of token strings, so a quota of 750 bytes per function
// holds the chunk only when its prototypes go uncharged. Qualified calls:
// see sflua.hpp.
void expect_prototypes_charged_while_compiling() {
    namespace sf = eawr::script::sflua;
    constexpr int functions = 2000;
    std::string source = "if false then\n";
    for (int index = 0; index < functions; ++index) source += "do local function f() return 1, 2, 3, 4, 5, 6, 7, 8 end end\n";
    source += "end\n";
    const auto compile = [&](std::uint64_t memory_bytes, std::uint64_t& baseline, std::uint64_t& charged) {
        sf::SandboxLimits limits;
        limits.memory_bytes = memory_bytes;
        const std::unique_ptr<sf::Sandbox> sandbox = sf::Sandbox::open(limits);
        expect(sandbox != nullptr, "prototypes: sandbox opens");
        if (sandbox == nullptr) return false;
        const sf::Sandbox::Scope scope(*sandbox);
        baseline = sandbox->memory_measured() + sandbox->memory_charged();
        const bool compiled = sandbox->load_source(source, "=prototypes") == 0;
        charged = sandbox->memory_measured() + sandbox->memory_charged() - baseline;
        if (!compiled) {
            const char* message = sf::lua_tostring(sandbox->state(), -1);
            expect(sandbox->fault() == sf::SandboxFault::memory_quota && message != nullptr &&
                       contains(message, "memory quota exhausted"),
                   "prototypes: memory quota fault");
        }
        return compiled;
    };
    std::uint64_t baseline = 0;
    std::uint64_t charged = 0;
    expect(compile(std::uint64_t{64} << 20, baseline, charged), "prototypes: compiles under the default quota");
    expect(charged >= std::uint64_t{functions} * 900,
           "prototypes: compiling charges the prototypes, but charged " + std::to_string(charged) + " bytes");
    std::uint64_t small_baseline = 0;
    expect(!compile(baseline + std::uint64_t{functions} * 750, small_baseline, charged),
           "prototypes: a chunk whose prototypes are over the quota does not compile");
}

// Concatenation checks the result's charge before luaV_concat sizes its
// buffer for the result (PR #380 review): a result over the quota faults
// without that allocation. lua_getgccount counts the buffer. Qualified calls:
// see sflua.hpp.
void expect_concat_checks_before_buffer(const std::string& expression) {
    namespace sf = eawr::script::sflua;
    const std::string label = "concatenation " + expression;
    sf::SandboxLimits limits;
    limits.memory_bytes = std::uint64_t{1} << 20;
    const std::unique_ptr<sf::Sandbox> sandbox = sf::Sandbox::open(limits);
    expect(sandbox != nullptr, label + ": sandbox opens");
    if (sandbox == nullptr) return;
    sf::lua_State* state = sandbox->state();
    const sf::Sandbox::Scope scope(*sandbox);
    sandbox->set_budget(1'000'000'000);
    const std::string source = "return function(s) return " + expression + " end";
    expect(sandbox->load_source(source, "=concat") == 0 && sf::lua_pcall(state, 0, 1, 0) == 0, label + ": compiles");
    const std::string half(600'000, 'x');
    sf::lua_pushlstring(state, half.data(), half.size());
    const int before = sf::lua_getgccount(state);
    expect(sf::lua_pcall(state, 1, 1, 0) != 0, label + ": a result over the quota raises");
    const int grown = sf::lua_getgccount(state) - before;
    const char* message = sf::lua_tostring(state, -1);
    expect(sandbox->fault() == sf::SandboxFault::memory_quota && message != nullptr && contains(message, "memory quota exhausted"),
           label + ": memory quota fault");
    expect(grown < 64, label + ": no buffer for the result, but the heap grew by " + std::to_string(grown) + " KiB");
}

void run_memory() {
    constexpr std::uint64_t mebibyte = std::uint64_t{1} << 20;
    const std::string growth_cases[][2] = {
        {"table entries", "local t = {} for i = 1, 10000000 do t[i] = i end"},
        {"hash entries", "local t = {} for i = 1, 10000000 do t['k' .. i] = true end"},
        {"string doubling", "local s = 'x' while true do s = s .. s end"},
        {"string.rep", "local s = string.rep('x', 2000000)"},
        {"gsub result", "local s = string.gsub(string.rep('x', 4000), '', string.rep('y', 400))"},
        {"tables", "local t = {} for i = 1, 10000000 do t[i] = {} end"},
        {"closures", "local t = {} for i = 1, 10000000 do local u = i t[i] = function() return u end end"},
        {"coroutines", "local t = {} for i = 1, 10000000 do t[i] = coroutine.create(function() end) end"},
    };
    for (const auto& [label, body] : growth_cases) {
        expect_fault("local ok = pcall(function() " + body + " end)\nTest_Report('absorbed', ok)\n", "memory quota exhausted", label,
                     small_memory(mebibyte));
    }
    // Deep stacks: charged past the allowance at every call, so recursion
    // faults on the quota well before upstream's call limit.
    {
        const std::string deep = "local function deep(n)\n" + many_locals(150) + "return deep(n + 1) + 1\nend\n";
        expect_fault(deep + "pcall(deep, 1) Test_Report('absorbed')\n", "memory quota exhausted", "deep stack", small_memory(256 << 10));
        expect_fault(deep + "local co = coroutine.create(deep) coroutine.resume(co, 1) Test_Report('absorbed')\n",
                     "memory quota exhausted", "deep coroutine stack", small_memory(256 << 10));
        // Without a small quota the same recursion ends in upstream's error.
        Session session = script_session(deep + "local ok, message = pcall(deep, 1) Test_Report(ok, message)\n", small_memory(64 * mebibyte));
        expect(run_script(session).empty() && session.commands.size() == 1 && contains(session.commands[0], "stack overflow"),
               "upstream stack overflow under the default quota\n" + joined(session.commands) + joined(session.diagnostics));
    }
    // A binding body only records the fault; the binding raises it after.
    {
        Session session = script_session("function T() local t = {} for i = 1, 100 do t[i] = Test_Text(20000) end Test_Report('absorbed') end Create_Thread('T')",
                                         small_memory(mebibyte));
        expect(session.scheduler
                   ->register_binding("Test_Text", [](auth::BindingContext&, const auth::ValueList& arguments) {
                       return eawr::core::Result<auth::ValueList>::success(
                           {auth::Value::text(std::string(static_cast<std::size_t>(whole(arguments.at(0))), 'z'))});
                   })
                   .has_value(),
               "register Test_Text");
        expect(run_script(session).empty(), "binding fault create");
        expect(session.removed == std::vector<std::uint64_t>{1} && any_contains(session.diagnostics, "memory quota exhausted") &&
                   session.commands.empty(),
               "binding result over the quota\n" + joined(session.diagnostics));
    }
    // Host pushes outside a protected call: an event argument over the quota
    // faults the instance at its handler, never the session.
    {
        Session session = script_session(
            "keep = {} for i = 1, 9000 do keep[i] = i end\n"
            "Register_Event('big', function(s) Test_Report('handled', string.len(s)) end)\n",
            small_memory(mebibyte));
        expect(run_script(session, 0).empty(), "event fault create");
        const eawr::sim::InlineExecutor executor;
        expect(session.scheduler
                   ->submit_event(dispatch_event({1, 30, 1, 0}, 1, "big", {auth::Value::text(std::string(900'000, 'e'))}))
                   .has_value(),
               "submit big event");
        step(session, executor);
        expect(session.removed == std::vector<std::uint64_t>{1} && any_contains(session.diagnostics, "memory quota exhausted") &&
                   session.commands.empty(),
               "event argument over the quota\n" + joined(session.diagnostics) + joined(session.commands));
        step(session, executor);
    }
    // Garbage does not accumulate: each measurement drops what is unreachable.
    {
        Session session = script_session(
            "function Churn() while true do local t = {} for i = 1, 3000 do t[i] = 'v' .. i end coroutine.yield(true) end end\n"
            "Create_Thread('Churn')\n",
            small_memory(mebibyte));
        expect(run_script(session, 40).empty(), "churn create");
        expect(session.removed.empty() && session.diagnostics.empty(), "churning ten times the quota's worth\n" + joined(session.diagnostics));
    }
    // A chunk over the quota leaves no instance.
    expect_fault("t = {} for i = 1, 100000 do t[i] = i end", "memory quota", "chunk over the quota", small_memory(mebibyte));
    // Compiled prototypes (PR #380 review): nested ones are charged as the
    // compiler builds them, so a chunk whose code alone is over the quota
    // does not compile...
    expect_prototypes_charged_while_compiling();
    // ...and the measurement counts the nested prototypes a closure keeps:
    // after a measurement (tick 1's churn makes one due), 800,000 bytes of
    // table entries no longer fit next to them.
    expect_fault("Keep = function()\n" + nested_functions(1200) +
                     "end\n"
                     "function Run()\n"
                     "  local scratch = {} for i = 1, 3200 do scratch[i] = {} end scratch = nil\n"
                     "  coroutine.yield(true)\n"
                     "  live = {} for i = 1, 25000 do live[i] = i end\n"
                     "  Test_Report('grown')\n"
                     "  coroutine.yield(true)\n"
                     "end\n"
                     "Create_Thread('Run')\n",
                 "memory quota exhausted", "measured nested prototypes", small_memory(mebibyte), 2);
    expect_concat_checks_before_buffer("s .. s");
    expect_concat_checks_before_buffer("table.concat({s, s})");
    // Quotas are validated.
    {
        auth::SessionConfig config = default_config();
        config.quotas.memory_bytes = 0;
        auto created = auth::ScriptScheduler::create(std::move(config), auth::ModuleManifest{});
        expect(!created, "a zero memory quota is rejected");
    }
}

// ---- memory_persistence ----

// Keeps about 35 KiB more each tick and churns about 300 KiB (so measurements
// run), so it passes a 1 MiB quota after a couple of dozen ticks.
constexpr std::string_view growing_script = R"lua(
keep = {}
function Grow()
  local round = 0
  while true do
    round = round + 1
    local block = {}
    for i = 1, 1000 do block[i] = i end
    keep[round] = block
    local scratch = {}
    for i = 1, 2000 do scratch[i] = 'r' .. round .. '.' .. i end
    Test_Report('round', round)
    coroutine.yield(true)
  end
end
Create_Thread('Grow')
)lua";

constexpr int persistence_ticks = 30;

auth::SessionConfig persistence_config() { return small_memory(std::uint64_t{1} << 20); }

Session growing_session() { return make_session({{"Grow.lua", std::string(growing_script)}}, persistence_config()); }

std::string tick_output(Session& session, const eawr::sim::PartitionExecutor& executor) {
    const std::size_t commands = session.commands.size();
    const std::size_t diagnostics = session.diagnostics.size();
    const std::size_t removed = session.removed.size();
    step(session, executor);
    std::string out;
    for (std::size_t index = commands; index < session.commands.size(); ++index) out += "C " + session.commands[index] + "\n";
    for (std::size_t index = diagnostics; index < session.diagnostics.size(); ++index) out += "D " + session.diagnostics[index] + "\n";
    for (std::size_t index = removed; index < session.removed.size(); ++index) out += "R " + std::to_string(session.removed[index]) + "\n";
    auto hash = session.scheduler->state_hash();
    return out + "H " + (hash ? hash.value() : "?") + "\nS " + session.scheduler->state_digest() + "\n";
}

void run_memory_persistence() {
    const eawr::sim::InlineExecutor executor;
    std::vector<std::string> reference;
    int fault_tick = 0;
    {
        Session session = growing_session();
        expect(session.scheduler->create_instance(1, "Grow.lua").has_value(), "grow create");
        for (int tick = 1; tick <= persistence_ticks; ++tick) {
            reference.push_back(tick_output(session, executor));
            if (fault_tick == 0 && contains(reference.back(), "memory quota exhausted")) fault_tick = tick;
        }
    }
    std::cout << "memory quota fault at tick " << fault_tick << '\n';
    expect(fault_tick > 5 && fault_tick < persistence_ticks, "the growing script faults mid-run");
    for (const int saved : {1, 5, 9, fault_tick - 1}) {
        std::string bytes;
        {
            Session session = growing_session();
            expect(session.scheduler->create_instance(1, "Grow.lua").has_value(), "grow create");
            for (int tick = 1; tick <= saved; ++tick) static_cast<void>(tick_output(session, executor));
            auto saved_bytes = session.scheduler->save();
            expect(saved_bytes.has_value(), "save at " + std::to_string(saved));
            if (!saved_bytes) continue;
            bytes = saved_bytes.value();
        }
        Session session = growing_session();
        auto loaded = session.scheduler->load(bytes);
        expect(loaded.has_value(), "load at " + std::to_string(saved) + ": " + (loaded ? std::string() : loaded.error().message));
        auto resaved = session.scheduler->save();
        expect(resaved.has_value() && resaved.value() == bytes, "re-saving the loaded state gives the same bytes");
        for (int tick = saved + 1; tick <= persistence_ticks; ++tick) {
            const std::string output = tick_output(session, executor);
            if (output != reference[static_cast<std::size_t>(tick - 1)]) {
                expect(false, "saved at " + std::to_string(saved) + ": tick " + std::to_string(tick) + " differs\n--- reference\n" +
                                  reference[static_cast<std::size_t>(tick - 1)] + "--- loaded\n" + output);
                break;
            }
        }
    }
    // A save whose counters exceed the session's quota is rejected.
    {
        Session session = growing_session();
        expect(session.scheduler->create_instance(1, "Grow.lua").has_value(), "grow create");
        for (int tick = 1; tick <= fault_tick - 1; ++tick) static_cast<void>(tick_output(session, executor));
        auto bytes = session.scheduler->save();
        auth::SessionConfig tighter = persistence_config();
        tighter.quotas.memory_bytes = 1024;
        Session other = make_session({{"Grow.lua", std::string(growing_script)}}, tighter);
        expect(bytes.has_value() && !other.scheduler->load(bytes.value()), "a save from another quota is rejected");
    }
}

// ---- memory_workers ----

std::vector<std::string> worker_run(const eawr::sim::PartitionExecutor& executor) {
    Session session = make_session({{"Grow.lua", std::string(growing_script)}}, persistence_config());
    for (std::uint64_t id = 1; id <= 12; ++id) expect(session.scheduler->create_instance(id * 5, "Grow.lua").has_value(), "create");
    std::vector<std::string> outputs;
    for (int tick = 1; tick <= persistence_ticks; ++tick) outputs.push_back(tick_output(session, executor));
    return outputs;
}

void run_memory_workers() {
    const eawr::sim::InlineExecutor inline_executor;
    const std::vector<std::string> reference = worker_run(inline_executor);
    expect(contains(reference.back(), "R ") || contains(joined(reference), "memory quota exhausted"), "instances fault on the quota");
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(worker_run(executor) == reference, std::to_string(workers) + " workers: per-tick output, hashes and digests");
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "";
    if (mode == "metering_width") {
        run_metering_width();
    } else if (mode == "patterns") {
        run_patterns();
    } else if (mode == "foc_patterns") {
        if (const int status = run_foc_patterns(); status != 0) return status;
    } else if (mode == "memory") {
        run_memory();
    } else if (mode == "memory_persistence") {
        run_memory_persistence();
    } else if (mode == "memory_workers") {
        run_memory_workers();
    } else {
        std::cerr << "usage: lua_limits_tests metering_width | patterns | foc_patterns | memory | memory_persistence | memory_workers\n";
        return 2;
    }
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "passed\n";
    return 0;
}
