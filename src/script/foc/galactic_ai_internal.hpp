#pragma once

#include "galactic_ai.hpp"

namespace eawr::script::foc::galactic_ai {
inline core::Diagnostic lifecycle_error(std::string message) {
    core::Diagnostic error;
    error.code = "EAWR-GCAI-0001";
    error.message = std::move(message);
    return error;
}
struct Manager::Impl {
    ai::AiData data;
    std::vector<PlanInput> plans;
    Capabilities capabilities;
    LifecycleState state;
    std::string configuration_digest;
    std::vector<ServiceTrace> traces;
    std::optional<core::Diagnostic> failure;
    [[nodiscard]] core::Result<bool> positive(const PlayerSelection&, std::string_view) const;
    [[nodiscard]] core::Result<void> initialize(PlayerLifecycle&);
    [[nodiscard]] core::Result<void> templates(PlayerLifecycle&);
};
} // namespace eawr::script::foc::galactic_ai
