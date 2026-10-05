#pragma once

#include "eawr/sim/tactical/economy.hpp"

namespace eawr::sim::tactical {

// WHE-55: add each nonzero source contribution before reducing categories.
// Zero means absent. A category with only negative contributions retains its largest value.
inline void accumulate_combat_bonus(CombatBonuses& category, const CombatBonuses& contribution) noexcept {
    for (std::size_t stat = 0; stat < category.size(); ++stat) {
        const auto value = contribution[stat];
        if (value.raw() != 0) category[stat] = category[stat].raw() == 0 ? value : std::max(category[stat], value);
    }
}

inline CombatBonuses sum_combat_bonus_categories(const std::span<const CombatBonuses> categories,
    const bool capped = true) noexcept {
    CombatBonuses result{};
    for (const auto& values : categories) for (std::size_t stat = 0; stat < result.size(); ++stat)
        result[stat] = math::Fixed::from_raw(result[stat].raw() + values[stat].raw());
    if (!capped) return result;
    // Nearest fixed representation of -0.99, matching authored decimal conversion.
    const auto floor = math::Fixed::from_raw(-(99 * math::Fixed::scale + 50) / 100);
    result[0] = std::max(result[0], floor);
    for (const auto stat : {1U, 2U, 3U}) result[stat] = std::max(result[stat], math::Fixed::from_raw(-math::Fixed::scale));
    result[4] = std::min(result[4], math::Fixed::from_raw(3 * math::Fixed::scale / 4));
    result[5] = std::max(result[5], floor);
    return result;
}

} // namespace eawr::sim::tactical
