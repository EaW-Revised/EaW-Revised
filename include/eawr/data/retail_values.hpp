#pragma once

// How FoC's database parse reads one authored value (docs/unit-data.md, "Value rules"; debug-build
// audit AU-80 to AU-83). Shared by the unit tables and the galactic loaders, so every table that
// loads types or records beyond the M2 fleet reads values the way the original game does.

#include "eawr/data/xml.hpp"
#include "eawr/sim/math/fixed.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace eawr::data::retail {

// AU-80, AU-81: an empty value, or exactly `TBD` (case-sensitive, surrounding whitespace ignored),
// changes nothing: the earlier or inherited value stays.
[[nodiscard]] bool is_placeholder(std::string_view text) noexcept;

// AU-82: true for `1` or for a value whose first letter is Y or T in either case, false for any
// other value. A placeholder has no value (nullopt).
[[nodiscard]] std::optional<bool> boolean(std::string_view text) noexcept;

// AU-82: the leading number of the value; trailing text is ignored, so `0.8f` reads 0.8.
struct LeadingNumber {
    sim::math::Fixed value;
    bool trailing_text{false}; // something other than whitespace followed the number
};

// nullopt when the value does not start with a number (after leading whitespace) or the number is
// outside Q24: the caller reports it, it never becomes a default.
[[nodiscard]] std::optional<LeadingNumber> leading_number(std::string_view text);

// How FoC reads an integer-typed game constant (#4992): the C `atoi` of the text, kept as 32 bits with
// no range or sign check. Leading whitespace and one sign are skipped, the leading digits are read and
// anything after them ignored (`2.5` reads 2, `-0.5` reads 0, text with no digits reads 0). A value
// beyond 32 bits saturates. An unsigned consumer reads the negative result as its 32-bit wrap (`-1`
// is 4294967295).
[[nodiscard]] std::int32_t integer_constant(std::string_view text) noexcept;
// LSL-P1: a float field as the retail parser reads it (the C runtime's decimal float parse). The
// leading number counts and trailing text is ignored; a value with no leading number (`abc`, `-`)
// reads 0; a magnitude beyond the fixed range (also `inf`) saturates at the largest fixed value of
// its sign, where retail stores an infinity. Never fails: nullopt only for a placeholder (AU-80/81).
struct FloatValue {
    sim::math::Fixed value;
    bool exact{true}; // false when trailing text was ignored, no number was found or the value saturated
};
[[nodiscard]] std::optional<FloatValue> float_value(std::string_view text);

// LSL-P2: a signed integer field as the retail parser reads it (the C runtime's integer parse):
// leading whitespace, one optional sign, the leading decimal digits; anything else ends the number
// (`2.9` reads 2, `-0.5` reads 0, `abc` reads 0); beyond the 32-bit range it saturates. Never fails:
// nullopt only for a placeholder.
struct IntegerValue {
    std::int32_t value{};
    bool exact{true}; // false when trailing text was ignored, no digits were found or the value saturated
};
[[nodiscard]] std::optional<IntegerValue> integer_value(std::string_view text);

// AU-82 / ML-26: list tags take every occurrence, but differ in how a variant's occurrences meet the list
// it inherits.
enum class ListRule : std::uint8_t {
    append,             // every occurrence appends, also onto an inherited list
    replace_in_variant, // appends in a base type; in a variant each occurrence replaces the list
    replace_by_variant_all, // a nonempty variant replaces inherited members with all its occurrences
};

// The verified rule of a list tag (case-insensitive), or nullopt for a tag AU-82 did not read.
[[nodiscard]] std::optional<ListRule> list_rule(std::string_view tag) noexcept;

// The layers of one object, most-derived first (as EffectiveObject::chain names them); the last
// layer is the base type, every earlier one a variant.
using Layers = std::span<const Definition* const>;

// The effective single value of `tag`: tags apply base first in document order (AU-80, AU-83),
// skipping placeholders, so this is the last non-placeholder occurrence of the most-derived layer
// that has one. nullptr when no layer authors a real value.
// Nested records with child nodes remain values even when their scalar text is empty.
[[nodiscard]] const XmlNode* single(Layers layers, std::string_view tag);

// Every non-placeholder occurrence of `tag` under `rule`, in the order the list ends up.
[[nodiscard]] std::vector<const XmlNode*> list(Layers layers, std::string_view tag, ListRule rule);

} // namespace eawr::data::retail
