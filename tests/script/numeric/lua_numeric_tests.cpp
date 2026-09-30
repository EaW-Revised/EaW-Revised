// Contract tests for the authoritative Lua numeric profile (#246).
//
//   lua_numeric_tests vectors <vectors.txt>
//   lua_numeric_tests sequences --arithmetic <sha256> --compare <sha256> ...
//   lua_numeric_tests vm
//   lua_numeric_tests foc-probe <lua_number_probe.lua> <foc-retail-probe.txt>
//   lua_numeric_tests eval <lua chunk>   (prints soft-float and P0 results)
//   lua_numeric_tests script <file>...   (runs the files as one chunk; it must return "PASS")
//
// Expected values were produced offline from hardware binary64 by
// tools/generate_lua_numeric_vectors.py; this program computes only with the
// integer backend. The vm mode also compares against the P0 hardware-double
// VM as a differential oracle on the running target.

#include "eawr/core/sha256.hpp"
#include "eawr/script/numeric/backend.hpp"
#include "eawr/script/numeric/binary64.hpp"
#include "eawr/script/numeric/decimal.hpp"
#include "eawr/script/numeric/lua_number.hpp"
#include "eawr/script/numeric/q24_boundary.hpp"
#include "numeric_sequences.hpp"
#include "p0_oracle.hpp"
#include "sflua.hpp"

#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace b64 = eawr::script::numeric::binary64;
namespace decimal = eawr::script::numeric::decimal;
namespace numeric = eawr::script::numeric;
namespace test = eawr::script::numeric::test;
using numeric::LuaNumber;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string hex(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex;
    stream.width(16);
    stream.fill('0');
    stream << value;
    return stream.str();
}

// ------------------------------------------------------------ compile time

static_assert(LuaNumber(3).repr == 0x4008000000000000ULL);
static_assert(LuaNumber(-1).repr == 0xBFF0000000000000ULL);
static_assert(static_cast<int>(LuaNumber::from_repr(0x400C000000000000ULL)) == 3);   // 3.5
static_assert(static_cast<int>(LuaNumber::from_repr(0xC00C000000000000ULL)) == -3);  // -3.5
static_assert(static_cast<int>(LuaNumber::from_repr(0x41E65A0BC0000000ULL)) == -2147483647 - 1); // 3e9
static_assert(static_cast<int>(LuaNumber::from_repr(b64::canonical_nan)) == -2147483647 - 1);
static_assert(static_cast<long long>(LuaNumber::from_repr(0x41E65A0BC0000000ULL)) == -2147483647 - 1);
static_assert(static_cast<unsigned int>(LuaNumber(-1)) == 0xFFFFFFFFU);
static_assert(static_cast<unsigned int>(LuaNumber::from_repr(0x41F0000000000001ULL)) == 0U); // 2^32 + tiny -> 4294967296.000001 truncates to 2^32
static_assert(static_cast<unsigned int>(LuaNumber::from_repr(b64::canonical_nan)) == 0U);
static_assert(b64::less(0xBFF0000000000000ULL, 0x0000000000000000ULL));
static_assert(b64::equal(b64::positive_zero, b64::negative_zero));
static_assert(!b64::equal(b64::canonical_nan, b64::canonical_nan));
static_assert(b64::add(0x3FF0000000000000ULL, 0x3FF0000000000000ULL) == 0x4000000000000000ULL);
static_assert(numeric::numeric_abi() == numeric::NumericAbi{"eawr-lua-binary64-soft", 1});

// ------------------------------------------------------------------ vectors

std::uint64_t parse_hex(std::string_view text) {
    std::uint64_t value = 0;
    for (const char character : text) {
        value <<= 4U;
        value |= static_cast<std::uint64_t>(character <= '9' ? character - '0' : (character | 0x20) - 'a' + 10);
    }
    return value;
}

// Tokens are separated by spaces; JSON-quoted tokens may contain anything.
std::vector<std::string> split_tokens(std::string_view line) {
    std::vector<std::string> tokens;
    std::size_t position = 0;
    while (position < line.size()) {
        if (line[position] == ' ') {
            ++position;
            continue;
        }
        std::string token;
        if (line[position] == '"') {
            ++position;
            while (position < line.size() && line[position] != '"') {
                char character = line[position++];
                if (character == '\\') {
                    const char escape = line[position++];
                    switch (escape) {
                    case 'n': character = '\n'; break;
                    case 't': character = '\t'; break;
                    case 'r': character = '\r'; break;
                    case 'b': character = '\b'; break;
                    case 'f': character = '\f'; break;
                    case 'u':
                        character = static_cast<char>(parse_hex(line.substr(position, 4)));
                        position += 4;
                        break;
                    default: character = escape; break;
                    }
                }
                token.push_back(character);
            }
            ++position;
        } else {
            while (position < line.size() && line[position] != ' ') {
                token.push_back(line[position++]);
            }
        }
        tokens.push_back(std::move(token));
    }
    return tokens;
}

int run_vectors(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "cannot open " << path << '\n';
        return 2;
    }
    std::map<std::string, int> counts;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const std::vector<std::string> t = split_tokens(line);
        const std::string& kind = t[0];
        ++counts[kind];
        if (kind == "add" || kind == "sub" || kind == "mul" || kind == "div") {
            const std::uint64_t a = parse_hex(t[1]);
            const std::uint64_t b = parse_hex(t[2]);
            const std::uint64_t got = kind == "add"   ? b64::add(a, b)
                                      : kind == "sub" ? b64::subtract(a, b)
                                      : kind == "mul" ? b64::multiply(a, b)
                                                      : b64::divide(a, b);
            expect(got == parse_hex(t[3]), line + " -> " + hex(got));
        } else if (kind == "sqrt") {
            const std::uint64_t got = b64::square_root(parse_hex(t[1]));
            expect(got == parse_hex(t[2]), line + " -> " + hex(got));
        } else if (kind == "cmp") {
            const std::uint64_t a = parse_hex(t[1]);
            const std::uint64_t b = parse_hex(t[2]);
            const int got = static_cast<int>(b64::equal(a, b)) | (static_cast<int>(b64::less(a, b)) << 1) |
                            (static_cast<int>(b64::less_equal(a, b)) << 2);
            expect(got == std::stoi(t[3]), line + " -> " + std::to_string(got));
        } else if (kind == "f2i") {
            const std::int64_t got = b64::to_int64_truncate(parse_hex(t[1]));
            expect(got == std::stoll(t[2]), line + " -> " + std::to_string(got));
        } else if (kind == "i2f") {
            const std::uint64_t got = b64::from_int64(std::stoll(t[1]));
            expect(got == parse_hex(t[2]), line + " -> " + hex(got));
        } else if (kind == "q24") {
            std::int64_t raw = 0;
            const int status = static_cast<int>(numeric::binary64_to_q24(parse_hex(t[1]), raw));
            expect(status == std::stoi(t[2]) && raw == std::stoll(t[3]),
                   line + " -> " + std::to_string(status) + " " + std::to_string(raw));
        } else if (kind == "fromq24") {
            const std::uint64_t got = numeric::q24_to_binary64(std::stoll(t[1]));
            expect(got == parse_hex(t[2]), line + " -> " + hex(got));
        } else if (kind == "parse") {
            const decimal::ParseResult got = decimal::parse_prefix(t[1]);
            expect(got.value == parse_hex(t[2]) && got.consumed == std::stoull(t[3]),
                   line + " -> " + hex(got.value) + " " + std::to_string(got.consumed));
        } else if (kind == "format") {
            const std::string got = decimal::format(parse_hex(t[2]), test::parse_spec(t[1]));
            expect(got == t[3], line + " -> \"" + got + "\"");
        } else {
            expect(false, "unknown vector kind: " + line);
        }
    }
    for (const auto& [kind, count] : counts) {
        std::cout << kind << ": " << count << '\n';
    }
    return 0;
}

// ---------------------------------------------------------------- sequences

void put64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
    }
}

std::string digest(const std::vector<std::uint8_t>& bytes) {
    return eawr::core::sha256_hex(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
}

std::string arithmetic_sequence(std::uint64_t seed, std::size_t count) {
    test::SplitMix64 rng(seed);
    std::vector<std::uint8_t> bytes;
    bytes.reserve(count * 9);
    for (std::size_t index = 0; index < count; ++index) {
        const auto operation = static_cast<std::uint8_t>(rng.next() % 5U);
        std::uint64_t result = 0;
        if (operation == 4) {
            result = b64::square_root(test::draw_operand(rng));
        } else {
            const auto [a, b] = test::draw_pair(rng);
            result = operation == 0   ? b64::add(a, b)
                     : operation == 1 ? b64::subtract(a, b)
                     : operation == 2 ? b64::multiply(a, b)
                                      : b64::divide(a, b);
        }
        bytes.push_back(operation);
        put64(bytes, result);
    }
    return digest(bytes);
}

std::string compare_sequence(std::uint64_t seed, std::size_t count) {
    test::SplitMix64 rng(seed);
    std::vector<std::uint8_t> bytes;
    for (std::size_t index = 0; index < count; ++index) {
        const auto [a, b] = test::draw_pair(rng);
        bytes.push_back(static_cast<std::uint8_t>(static_cast<int>(b64::equal(a, b)) |
                                                  (static_cast<int>(b64::less(a, b)) << 1) |
                                                  (static_cast<int>(b64::less_equal(a, b)) << 2)));
    }
    return digest(bytes);
}

std::string integer_sequence(std::uint64_t seed, std::size_t count) {
    test::SplitMix64 rng(seed);
    std::vector<std::uint8_t> bytes;
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint64_t shift = rng.next() & 63U;
        const std::int64_t integer = static_cast<std::int64_t>(rng.next()) >> shift;
        put64(bytes, b64::from_int64(integer));
        put64(bytes, static_cast<std::uint64_t>(b64::to_int64_truncate(test::draw_operand(rng))));
    }
    return digest(bytes);
}

std::string q24_sequence(std::uint64_t seed, std::size_t count) {
    test::SplitMix64 rng(seed);
    std::vector<std::uint8_t> bytes;
    for (std::size_t index = 0; index < count; ++index) {
        std::int64_t raw = 0;
        const auto status = static_cast<std::uint8_t>(numeric::binary64_to_q24(test::draw_operand(rng), raw));
        bytes.push_back(status);
        put64(bytes, static_cast<std::uint64_t>(raw));
        const std::uint64_t shift = rng.next() & 63U;
        put64(bytes, numeric::q24_to_binary64(static_cast<std::int64_t>(rng.next()) >> shift));
    }
    return digest(bytes);
}

std::string parse_sequence(std::uint64_t seed, std::size_t count) {
    test::SplitMix64 rng(seed);
    std::vector<std::uint8_t> bytes;
    for (std::size_t index = 0; index < count; ++index) {
        put64(bytes, decimal::parse_prefix(test::decimal_text(rng)).value);
    }
    return digest(bytes);
}

std::string format_sequence(std::uint64_t seed, std::size_t count) {
    test::SplitMix64 rng(seed);
    std::vector<std::uint8_t> bytes;
    std::vector<decimal::FormatSpec> specs;
    for (const std::string_view spec : test::sequence_formats) {
        specs.push_back(test::parse_spec(spec));
    }
    const auto append = [&bytes](const std::string& text) {
        bytes.insert(bytes.end(), text.begin(), text.end());
        bytes.push_back('\n');
    };
    for (std::size_t index = 0; index < count; ++index) {
        std::uint64_t bits = test::draw_operand(rng);
        if ((bits & b64::exponent_mask) == b64::exponent_mask) {
            bits &= ~0x4000000000000000ULL;
        }
        for (const decimal::FormatSpec& spec : specs) {
            append(decimal::format(bits, spec));
        }
        append(decimal::format_shortest(bits));
    }
    return digest(bytes);
}

int run_sequences(int argc, char** argv) {
    using Generator = std::function<std::string(std::uint64_t, std::size_t)>;
    const std::map<std::string, std::pair<Generator, std::uint64_t>> sequences{
        {"arithmetic", {arithmetic_sequence, 0x5EED0246A0000001ULL}},
        {"compare", {compare_sequence, 0x5EED0246A0000002ULL}},
        {"integer", {integer_sequence, 0x5EED0246A0000003ULL}},
        {"q24", {q24_sequence, 0x5EED0246A0000004ULL}},
        {"parse", {parse_sequence, 0x5EED0246A0000005ULL}},
        {"format", {format_sequence, 0x5EED0246A0000006ULL}},
    };
    const std::map<std::string, std::size_t> counts{{"arithmetic", 1000000}, {"compare", 250000},
                                                    {"integer", 250000},     {"q24", 250000},
                                                    {"parse", 200000},       {"format", 50000}};
    int checked = 0;
    for (int index = 2; index + 1 < argc; index += 2) {
        const std::string name = std::string(argv[index]).substr(2);
        const auto found = sequences.find(name);
        if (found == sequences.end()) {
            std::cerr << "unknown sequence " << name << '\n';
            return 2;
        }
        const std::string got = found->second.first(found->second.second, counts.at(name));
        expect(got == argv[index + 1], name + " digest " + got + " != " + argv[index + 1]);
        std::cout << name << ": " << got << '\n';
        ++checked;
    }
    expect(checked == static_cast<int>(sequences.size()), "every sequence needs an expected digest");
    return 0;
}

// ----------------------------------------------------------------------- vm

// Qualified calls, not a using-directive: see the ::std note in sflua.hpp.
std::string sf_run(std::string_view code) {
    namespace sf = eawr::script::sflua;
    sf::lua_State* state = sf::open_profile_state();
    int status = sf::luaL_loadbuffer(state, code.data(), code.size(), "=test");
    if (status == 0) {
        status = sf::lua_pcall(state, 0, 1, 0);
    }
    const char* text = sf::lua_type(state, -1) == LUA_TSTRING ? sf::lua_tostring(state, -1) : nullptr;
    const std::size_t length = text != nullptr ? sf::lua_strlen(state, -1) : 0;
    std::string result = status != 0 ? "error: " + std::string(text != nullptr ? text : "?")
                                     : (text != nullptr ? std::string(text, length) : std::string("<not a string>"));
    sf::lua_close(state);
    return result;
}

void expect_sf(std::string_view code, std::string_view expected) {
    const std::string got = sf_run(code);
    expect(got == expected, std::string(code) + "\n  got      \"" + got + "\"\n  expected \"" + std::string(expected) + "\"");
}

void expect_same_as_p0(std::string_view code) {
    const std::string got = sf_run(code);
    const std::string oracle = test::p0_run(code);
    expect(got == oracle, std::string(code) + "\n  soft-float \"" + got + "\"\n  P0 hardware \"" + oracle + "\"");
}

// Numeric literals of source chunks, including FoC-shaped and boundary values.
constexpr std::string_view literal_chunk =
    "local a = {0.1, 0.2, 0.3, 0.25, 0.33, 0.8, 1.5, 7.0e-3, 3.14159265358979323846, 1e22, 1e23, "
    "123456789012345678, 9007199254740993, 999999999999999.0, 1e308, 1.7976931348623157e308, "
    "2.2250738585072011e-308, 4.9406564584124654e-324, 8.98846567431158e307, -0, 0, .5, 5., 1E2} "
    "local out = {} for i = 1, table.getn(a) do out[i] = string.format('%.17g', a[i]) end "
    "return table.concat(out, ' ')";

void run_vm() {
    // Arithmetic, comparison and coercion through the VM.
    expect_sf("return tostring(0.1 + 0.2)", "0.3");
    expect_sf("return string.format('%.17g', 0.1 + 0.2)", "0.30000000000000004");
    expect_sf("return tostring(1/0)..' '..tostring(-1/0)..' '..tostring(0/0)..' '..tostring(-(0/0))",
              "inf -inf -nan(ind) -nan(ind)");
    expect_sf("return tostring(0/0 == 0/0)..tostring(0/0 ~= 0/0)..tostring(-0 == 0)..tostring(1 < 0/0)",
              "falsetruetruefalse");
    expect_sf("return tostring('10' + 5)..' '..tostring(' 12 ' * 2)", "15 24");
    expect_sf("return tostring('0x10' + 0)..' '..tostring(tonumber('inf'))..' '..tostring(tonumber('1d2'))",
              "16 inf nil");
    expect_sf("return tostring(tonumber('ff', 16))..' '..tostring(tonumber('ffffffffff', 16))..' '.."
              "tostring(tonumber('-1', 16))..' '..tostring(tonumber('z', 36))..' '..tostring(tonumber('1e'))",
              "255 4294967295 4294967295 35 nil");
    expect_sf("return string.format('%d|%d|%.3f|%5.1f|%-9.2e|%g|%x|%5.2s|%c', 3.99, -3.99, 2/3, 1.25, 1234.5, "
              "1e20, 255.9, 'abc', 65)",
              "3|-3|0.667|  1.2|1.23e+03 |1e+20|ff|   ab|A");
    expect_sf("return string.format('%d %d', 3e9, 0/0)", "-2147483648 -2147483648");
    // PGBase.lua's Dirty_Floor/Simple_Mod pattern.
    expect_sf("local function Dirty_Floor(v) return string.format('%d', v) end "
              "return tostring(7 - 2 * Dirty_Floor(7 / 2))",
              "1");
    expect_sf("local n, last = 0 for i = 0, 1, 0.1 do n = n + 1 last = i end "
              "return n .. ' ' .. string.format('%.17g', last)",
              "11 0.99999999999999989");
    expect_sf("local s = 0 for i = 1, 100 do s = s + 1 / i end return string.format('%.17g', s)",
              "5.1873775176396206");
    expect_sf("local t = {} t[1] = 'a' t[1.0] = 'b' t[-0] = 'z' t[0] = 'y' return t[1] .. t[-0]", "by");
    expect_sf("local t = {} t[0/0] = 1 return 'x'", "error: test:1: table index is NaN");
    expect_sf("return tostring(2^3)", "error: test:1: `__pow' (`^' operator) is not a function");
    expect_sf("return tostring(math)", "nil");
    expect_sf("return string.rep('a', 2.9) .. string.sub('hello', 2.7, 4.2)", "aaell");
    expect_sf("return tostring(123456789012345)", "1.2345678901234e+14");
    expect_sf("return string.format('%.0f %.0f %.0f %.2f', 0.5, 1.5, 2.5, 1.125)", "0 2 2 1.12");

    // Differential against the P0 hardware-double VM on this target.
    expect_same_as_p0("return string.format('%.17g %.17g %.17g', 0.1 + 0.2, 1/3, 2/3*3)");
    expect_same_as_p0("local s = 0 for i = 1, 1000 do s = s + 1 / i end return string.format('%.17g', s)");
    expect_same_as_p0("local n, last = 0 for i = 0, 1, 0.1 do n = n + 1 last = i end "
                      "return n .. ' ' .. string.format('%.17g', last)");
    expect_same_as_p0("return tostring(1e300 * 1e10 > 1e308) .. string.format(' %.17g', 1e-310 / 3)");
    expect_same_as_p0("return 1 .. '|' .. 0.5 .. '|' .. -0 .. '|' .. 1e15 .. '|' .. 1e16 .. '|' .. 123.456");
    expect_same_as_p0("return string.format('%.17g %.17g %.17g', 0.1 * 3, 1e23, 9007199254740993)");
    expect_same_as_p0("return string.format('%.17g %.17g', 5e-324, 2.2250738585072011e-308)");
    expect_same_as_p0("local x = 1 for i = 1, 60 do x = x * 1.1 end return string.format('%.17g', x)");
    expect_same_as_p0("local x = 7 for i = 1, 40 do x = (x * 31 + 17) / 3 - x / 7 end return string.format('%.17g', x)");
    expect_same_as_p0("return tostring(0.1 + 0.2 == 0.3) .. tostring(0.1 + 0.2 > 0.3) .. tostring(1/3*3 == 1)");
    expect_same_as_p0("return string.format('%d %d %x %5.2f|%e|%g|%G', 2147483647.9, -2147483648.5, 4294967295, "
                      "3.14159, 12345.6789, 0.0001234, 1e-10)");
    expect_same_as_p0("return tostring(tonumber('  0.1e1  ')) .. tostring(tonumber('1e')) .. tostring('3' * '4')");
    expect_same_as_p0(literal_chunk);

    // Source and compiled constants: identical chunk bytes to the P0 compiler,
    // and a reloaded chunk returns the same text as its source.
    const std::string dump_source = "return string.dump(loadstring(\"" + std::string(literal_chunk) + "\"))";
    const std::string dumped = sf_run(dump_source);
    expect(dumped.size() > 64 && dumped.rfind("error", 0) != 0, "string.dump of the literal chunk: " + dumped);
    expect(dumped == test::p0_run(dump_source), "soft-float and P0 compile the literal chunk to different bytes");
    expect_sf("return loadstring(string.dump(loadstring(\"" + std::string(literal_chunk) + "\")))()",
              sf_run(literal_chunk));

    // Q24 boundary.
    const auto fixed = numeric::to_fixed(LuaNumber::from_repr(0x3FB999999999999AULL)); // 0.1
    expect(fixed.has_value() && fixed.value().raw() == 1677722, "0.1 rounds to Q24 raw 1677722");
    const auto half_quantum = numeric::to_fixed(LuaNumber::from_repr(0x3E60000000000000ULL)); // 2^-25
    expect(half_quantum.has_value() && half_quantum.value().raw() == 0, "2^-25 ties to even raw 0");
    const auto large = numeric::to_fixed(LuaNumber(std::int64_t{1} << 39));
    expect(!large.has_value() && large.error().code == numeric::diagnostic_codes::out_of_range, "2^39 is out of range");
    const auto nan = numeric::to_fixed(LuaNumber::from_repr(b64::canonical_nan));
    expect(!nan.has_value() && nan.error().code == numeric::diagnostic_codes::not_finite, "NaN has no Q24 value");
    expect(numeric::from_fixed(eawr::sim::math::Fixed::from_raw(1)).repr == 0x3E70000000000000ULL, "raw 1 is 2^-24");
    const auto three = numeric::to_exact_integer(LuaNumber(3));
    expect(three.has_value() && three.value() == 3, "3 is an exact integer");
    const auto fraction = numeric::to_exact_integer(LuaNumber::from_repr(0x400C000000000000ULL));
    expect(!fraction.has_value() && fraction.error().code == numeric::diagnostic_codes::not_integral, "3.5 is not integral");
    const auto minimum = numeric::to_exact_integer(LuaNumber::from_repr(0xC3E0000000000000ULL));
    expect(minimum.has_value() && minimum.value() == std::numeric_limits<std::int64_t>::min(), "-2^63 is an int64");
    const auto beyond = numeric::to_exact_integer(LuaNumber::from_repr(0x43E0000000000000ULL));
    expect(!beyond.has_value() && beyond.error().code == numeric::diagnostic_codes::out_of_range, "2^63 is not");
}

// ---------------------------------------------------------------- foc-probe

// The #376 probe (tools/validation/p1_capture/lua_number_probe.lua) against the line
// the retail game showed on the rig: the first line of the record file that is not
// empty and does not start with '#'.
int run_foc_probe(const char* probe_path, const char* record_path) {
    std::ifstream probe_file(probe_path, std::ios::binary);
    std::ifstream record_file(record_path, std::ios::binary);
    if (!probe_file || !record_file) {
        std::cerr << "cannot read " << (!probe_file ? probe_path : record_path) << '\n';
        return 2;
    }
    std::ostringstream probe;
    probe << probe_file.rdbuf();
    std::string recorded;
    for (std::string line; std::getline(record_file, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty() && line.front() != '#') {
            recorded = line;
            break;
        }
    }
    const std::string source = probe.str();
    const std::string got = sf_run(source + "\nreturn EaWR_Lua_Number_Probe()");
    expect(got.rfind("LNP1 OK ", 0) == 0, "the soft-float VM misses the probe's expected texts:\n  " + got);
    expect(got == recorded, "soft-float probe line differs from the retail FoC record\n  soft-float \"" + got +
                                "\"\n  retail FoC \"" + recorded + "\"");
    // A failure evaluating case P must not masquerade as the intended ^ error.
    std::string broken = source;
    const std::string target = "local ok = pcall(function() local a = 2 return a ^ 3 end)";
    const auto at = broken.find(target);
    expect(at != std::string::npos, "cannot locate case P for the negative check");
    if (at != std::string::npos) {
        broken.insert(at, "error('broken case P') ");
        const std::string failed = sf_run(broken + "\nreturn EaWR_Lua_Number_Probe()");
        expect(failed.rfind("LNP1 BAD=P n=26 ", 0) == 0 && failed.find("P=FAIL:") != std::string::npos,
               "a broken case P must give BAD=P and FAIL: " + failed);
    }
    return 0;
}

// A rig probe driven by a Lua test script with fake engine calls (the #351/#81 staging
// probe): the files run as one chunk in the FoC profile state and must return "PASS".
int run_script(int count, char** paths) {
    std::string source;
    for (int i = 0; i < count; ++i) {
        std::ifstream file(paths[i], std::ios::binary);
        if (!file) {
            std::cerr << "cannot read " << paths[i] << '\n';
            return 2;
        }
        std::ostringstream text;
        text << file.rdbuf();
        source += text.str();
        source += '\n';
    }
    const std::string got = sf_run(source);
    expect(got == "PASS", "the script did not pass:\n  " + got);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "";
    int status = 0;
    if (mode == "vectors" && argc > 2) {
        status = run_vectors(argv[2]);
    } else if (mode == "sequences") {
        status = run_sequences(argc, argv);
    } else if (mode == "vm") {
        run_vm();
    } else if (mode == "foc-probe" && argc > 3) {
        status = run_foc_probe(argv[2], argv[3]);
    } else if (mode == "script" && argc > 2) {
        status = run_script(argc - 2, argv + 2);
    } else if (mode == "eval" && argc > 2) {
        std::cout << "soft-float: " << sf_run(argv[2]) << "\nP0:         " << test::p0_run(argv[2]) << '\n';
        return 0;
    } else {
        std::cerr << "usage: lua_numeric_tests vectors <file> | sequences --<name> <sha256>... | vm | "
                     "foc-probe <probe.lua> <record.txt> | script <file>... | eval <chunk>\n";
        return 2;
    }
    if (status != 0) {
        return status;
    }
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "passed\n";
    return 0;
}
