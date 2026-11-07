#pragma once
#include "eawr/presentation/shader_activation_snapshot.hpp"
#include "eawr/mods/shader_translation.hpp"

// Caller adapter: presentation itself has no dependency on the mod library.
namespace eawr::presentation::shader_activation {
[[nodiscard]] inline LayerPolicy contributor_policy(std::string layer, const mods::ModRecord& record) {
    const auto decision = mods::shader_translation_policy(&record);
    const auto policy = decision.policy == mods::ShaderTranslationPolicy::allowed ? Policy::allowed :
        decision.policy == mods::ShaderTranslationPolicy::opted_out ? Policy::opted_out : Policy::malformed;
    return {std::move(layer), true, policy};
}
} // namespace eawr::presentation::shader_activation
