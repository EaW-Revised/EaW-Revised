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
// PD-01: owner #5136 (2026-10-27); measured evidence is in the behaviour document.
inline constexpr std::array<PerformanceDeviation, 1> performance_deviations{{
    {"PD-01", "gameplay.reinforcement_areas_only_while_placing", "Reinforcement areas only while placing",
     PerformanceDeviationScope::presentation, "1", "Panel or data-authored drag rule", "Only while placing"},
}};

} // namespace eawr::presentation::ui
