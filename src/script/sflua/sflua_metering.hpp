#pragma once

#include <cstdint>
#include <limits>

namespace eawr::script::sflua_metering {

using StepUnits = std::uint64_t;
static_assert(sizeof(StepUnits) == 8);

// -1 is the exhausted-budget sentinel. Compare before subtracting so a large
// charge cannot wrap the signed budget and become a credit.
constexpr std::int64_t charged_budget(std::int64_t budget, StepUnits units) noexcept {
    if (budget < 0 || units > static_cast<StepUnits>(budget)) return -1;
    return budget - static_cast<std::int64_t>(units);
}

constexpr StepUnits saturating_add(StepUnits left, StepUnits right) noexcept {
    constexpr StepUnits maximum = std::numeric_limits<StepUnits>::max();
    return right > maximum - left ? maximum : left + right;
}

constexpr StepUnits saturating_multiply(StepUnits left, StepUnits right) noexcept {
    constexpr StepUnits maximum = std::numeric_limits<StepUnits>::max();
    return right != 0 && left > maximum / right ? maximum : left * right;
}

} // namespace eawr::script::sflua_metering
