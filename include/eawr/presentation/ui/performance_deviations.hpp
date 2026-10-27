#pragma once

// Approved deviations and their published descriptions share this registry.
// docs/behaviour/performance-deviations.md is checked against it by ui_settings_contracts.

#include <array>
#include <cstdint>
#include <string_view>

namespace eawr::presentation::ui {

enum class PerformanceDeviationScope : std::uint8_t { presentation, simulation };

struct PerformanceDeviation final {
    std::string_view rule_id;
    std::string_view setting_key;
    std::string_view name;
    PerformanceDeviationScope scope;
    std::string_view default_value;
    std::string_view retail_behaviour;
    std::string_view our_behaviour;
};

// Add a row only with an approved behaviour rule and its measured evidence.
// No deployment-grid or simulation deviation has been registered here yet.
inline constexpr std::array<PerformanceDeviation, 0> performance_deviations{};

} // namespace eawr::presentation::ui
