#pragma once
#include "galactic_ai.hpp"

namespace eawr::script::foc::galactic_ai {
// The caller's complete game asset identity also validates provider-owned data.
[[nodiscard]] std::string lifecycle_configuration_digest(const LifecycleSetup&, const ai::AiData&, std::span<const PlanInput>);
} // namespace eawr::script::foc::galactic_ai
