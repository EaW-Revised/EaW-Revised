#pragma once

#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstdint>
#include <string_view>

namespace eawr::units {

// BF-2 (#3779): the one place that pairs a space unit-table tag with its land variant. The loader and
// the tables built from it ask for a stem plus the content domain and read whichever spelling comes back.
enum class DomainTag : std::uint8_t {
    reveal_range,      // per-type sensor range
    layer,             // movement layer (land authors none, so the land spelling is empty)
    speed_multiplier,  // game-constants scalar
    health_multiplier, // game-constants scalar
    model_name,        // per-type model (Space_Model_Name / Land_Model_Name)
};

// The XML tag a domain reads for `stem`; empty when the domain has no such variant.
[[nodiscard]] std::string_view domain_tag(sim::tactical::BattleDomain domain, DomainTag stem) noexcept;

// The domain's spelling of a space tag that has a variant row (unchanged when the row has none or the domain is
// space), so a list of space-named constants loads its land twin for a land table.
[[nodiscard]] std::string_view domain_variant(sim::tactical::BattleDomain domain, std::string_view space_spelling) noexcept;

// The Activation_Style spelling of the domain's always-on ability variant (space: Space_Automatic).
[[nodiscard]] std::string_view automatic_activation_name(sim::tactical::BattleDomain domain) noexcept;

} // namespace eawr::units
