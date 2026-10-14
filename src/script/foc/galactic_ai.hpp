#pragma once

#include "eawr/script/foc/galactic_ai.hpp"
#include "ai_data.hpp"

namespace eawr::script::foc::galactic_ai {
// Reuse shared parser and equations; select the mode before exposing functions.
[[nodiscard]] core::Result<ai::AiData> load_data(const LifecycleSetup&);
[[nodiscard]] TemplatePolicy apply_template(const ai::AiData&, std::span<const PlanInput>, TemplatePolicy, const ai::Template&);
[[nodiscard]] TemplatePolicy reset_template(const ai::AiData&, std::span<const PlanInput>, TemplatePolicy, const ai::Template&);
} // namespace eawr::script::foc::galactic_ai
