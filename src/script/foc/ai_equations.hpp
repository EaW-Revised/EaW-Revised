#pragma once

// The perceptual equations of the FoC tactical AI (#449, docs/behaviour/foc-tactical-ai.md
// "Perceptual equations", rules PE-xx): parsing the equation XML and evaluating an equation
// against the host's perception. All arithmetic uses the authoritative soft binary64 numbers;
// retail single-precision steps round through to_single().

#include "eawr/core/result.hpp"
#include "eawr/script/numeric/lua_number.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace eawr::script::foc::ai {

using Real = numeric::LuaNumber;

// Round a binary64 value to the nearest binary32 value (ties to even), kept as binary64.
// Single-precision + - * / of single operands equal to_single() of the binary64 result.
[[nodiscard]] Real to_single(Real value) noexcept;
[[nodiscard]] Real real(std::int64_t value) noexcept;
// A decimal literal of the data (the C runtime's atof); nullopt when it is not one.
[[nodiscard]] std::optional<Real> parse_real(std::string_view text) noexcept;
[[nodiscard]] Real square_root(Real value) noexcept;
// C's (int)x truncation toward zero (values in range).
[[nodiscard]] std::int64_t truncate(Real value) noexcept;
// Retail CRC-32 of the raw bytes (PE-12).
[[nodiscard]] std::uint32_t crc32(std::string_view text) noexcept;

// The engine's 15-bit linear congruential random generator (PE-11, GR-xx).
class AiRandom {
public:
    void set_seed(std::uint32_t seed) noexcept { seed_ = seed; }
    [[nodiscard]] std::uint32_t seed() const noexcept { return seed_; }
    // Next 15-bit value, 0..32767.
    [[nodiscard]] std::uint32_t next() noexcept;
    // Whole number in [low, high] (either order), by masked rejection.
    [[nodiscard]] std::int32_t range(std::int32_t low, std::int32_t high) noexcept;
    // Single-precision uniform value in [low, high] (either order): low + (next / 32767) * (high - low).
    [[nodiscard]] Real uniform(Real low, Real high) noexcept;

private:
    std::uint32_t seed_{};
};

// A parameter value of a token lookup: a number (numbers, converter constants) or an
// upper-case string (quoted names).
using ParameterValue = std::variant<Real, std::string>;

struct Binding {
    std::string parameter; // upper case, e.g. PARAMETER_CATEGORY
    ParameterValue value;
};

// One token chain of an equation, e.g. Variable_Target.Location.EnemyForce, in upper case.
struct Lookup {
    std::vector<std::string> tokens;
    std::vector<std::string> parameters; // bound in order; values come from the program
};

struct Node {
    enum class Kind : std::uint8_t {
        constant, text, lookup, negate, add, subtract, multiply, divide, random, equal, not_equal,
        less, less_equal, greater, greater_equal, clamp,
    };
    Kind kind{Kind::constant};
    Real value{};
    std::string text;
    std::size_t lookup{};
};

struct Equation {
    std::string name;               // as written in the XML (its key)
    std::vector<Node> program;      // postfix
    std::vector<Lookup> lookups;
};

// Converter constants `Converter[Name | Name]`: the number the named converter gives.
using ConverterFunction = std::function<std::optional<Real>(std::string_view converter, std::string_view value)>;

// Parses one equation body (PE-01 grammar).
[[nodiscard]] core::Result<Equation> parse_equation(
    std::string name, std::string_view body, const ConverterFunction& converters);

// The equations of the equation XML files: the first definition of a name wins (PE-02).
class EquationSet {
public:
    // `files` in load order: (logical path, XML text).
    [[nodiscard]] static core::Result<EquationSet> parse(
        const std::vector<std::pair<std::string, std::string>>& files, const ConverterFunction& converters);
    [[nodiscard]] const Equation* find(std::string_view name) const; // case-insensitive
    [[nodiscard]] std::size_t size() const noexcept { return equations_.size(); }

private:
    std::map<std::string, Equation, std::less<>> equations_; // key upper case
};

// What the evaluator asks its host: the value of one lookup with its bound parameters, or
// nullopt when the lookup fails (PE-05).
class LookupResolver {
public:
    virtual ~LookupResolver() = default;
    [[nodiscard]] virtual std::optional<Real> resolve(const Lookup& lookup, const std::vector<Binding>& bindings) = 0;
};

// Runs the program on a value stack (PE-03, PE-04). nullopt when a step fails or the stack
// does not end with exactly one value. `random` draws the # operator.
[[nodiscard]] std::optional<Real> run(const Equation& equation, LookupResolver& resolver, AiRandom& random);

} // namespace eawr::script::foc::ai
