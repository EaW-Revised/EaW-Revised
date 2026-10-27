#pragma once
#include "eawr/mods/record.hpp"

namespace eawr::mods {
enum class ShaderTranslationPolicy { allowed, opted_out, malformed };
struct ShaderTranslationDecision {
    ShaderTranslationPolicy policy{ShaderTranslationPolicy::allowed};
    std::string_view contributor_key; // borrowed from the selected contributor
    std::string_view diagnostic_field{}; // nonempty for malformed policy
    [[nodiscard]] bool translation_allowed() const noexcept {
        return policy == ShaderTranslationPolicy::allowed;
    }
};
// STO-01..04: pass the contributor of the selected bytes, nullptr for retail.
// Pure metadata query; call before source/compiled admission AND cache access.
[[nodiscard]] ShaderTranslationDecision shader_translation_policy(const ModRecord* contributor);
} // namespace eawr::mods
