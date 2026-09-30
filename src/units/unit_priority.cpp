#include "unit_internal.hpp"

#include <algorithm>

namespace eawr::units {

// Rules checked against the FoC debug build (#270, docs/unit-data.md "Priority sets").
std::optional<Fixed> attack_priority(const TargetingPrioritySet& set, const UnitType& candidate) {
    Fixed score = unlisted_priority;
    bool property_listed = false;
    for (const auto& entry : set.attack_priorities) {
        switch (entry.match) {
        case PriorityMatch::type:
            // An exact type entry decides at once, before any exclusion.
            if (detail::iequals(entry.name, candidate.id)) return entry.weight;
            break;
        case PriorityMatch::category:
            if ((entry.bits & candidate.category_bits) != 0) score = std::min(score, entry.weight);
            break;
        case PriorityMatch::property:
            // The first matching property entry replaces the score so far; later ones take the minimum.
            if ((entry.bits & candidate.property_bits) != 0) {
                score = property_listed ? std::min(score, entry.weight) : entry.weight;
                property_listed = true;
            }
            break;
        }
    }
    for (const auto& excluded : set.unit_exclusions) {
        if (detail::iequals(excluded, candidate.id)) return std::nullopt;
    }
    // A matching property entry overrides a category exclusion, never a property exclusion.
    if (!property_listed && (set.category_exclusion_bits & candidate.category_bits) != 0) return std::nullopt;
    if ((set.property_exclusion_bits & candidate.property_bits) != 0) return std::nullopt;
    return score;
}

std::optional<Fixed> hard_point_priority(const TargetingPrioritySet& set, const HardpointType type) {
    // Priority-set names drop the HARD_POINT_ prefix of the hardpoint type names.
    const auto name = to_string(type);
    constexpr std::string_view prefix = "HARD_POINT_";
    const auto matches = [&](const std::string& listed) {
        return name.size() == prefix.size() + listed.size() && name.starts_with(prefix) &&
               detail::iequals(name.substr(prefix.size()), listed);
    };
    for (std::size_t index = 0; index < set.hard_point_priorities.size(); ++index) {
        if (matches(set.hard_point_priorities[index])) {
            return Fixed::from_raw((static_cast<std::int64_t>(index) + 1) * Fixed::scale);
        }
    }
    if (std::any_of(set.hard_point_exclusions.begin(), set.hard_point_exclusions.end(), matches)) return std::nullopt;
    return unlisted_priority;
}

} // namespace eawr::units
