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
