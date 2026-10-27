#pragma once

#include "eawr/script/foc/galactic_ai.hpp"
#include "ai_data.hpp"

namespace eawr::script::foc::galactic_ai {
// Reuse shared parser and equations; select the mode before exposing functions.
[[nodiscard]] std::vector<std::string> include_mounted_ai_files(LifecycleSetup&);
[[nodiscard]] core::Result<ai::AiData> load_data(const LifecycleSetup&);
// One line per galactic goal-function entry of the selected players whose equation is not loaded; the engine does not register those sets.
[[nodiscard]] std::vector<std::string> unresolved_goal_equations(const ai::AiData&, std::span<const PlayerSelection>);
[[nodiscard]] TemplatePolicy apply_template(const ai::AiData&, std::span<const PlanInput>, TemplatePolicy, const ai::Template&);
[[nodiscard]] TemplatePolicy reset_template(const ai::AiData&, std::span<const PlanInput>, TemplatePolicy, const ai::Template&);
} // namespace eawr::script::foc::galactic_ai
