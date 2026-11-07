#pragma once

#include "internal.hpp"

namespace eawr::presentation::shader_runtime::detail {
// S-15: qualified only for the reviewed compiler's unremapped, non-system,
// whole-register signature leaves. Packed, ambiguous, missing or unknown
// signatures retain unavailable. Names identify public source semantics only;
// generated GLSL identifiers are never parsed for register identities.
inline void associate_interfaces(StageReflection& reflection, const Scan& scanned) {
    const auto usage = [](std::string_view name) -> std::optional<std::uint32_t> {
        constexpr std::pair<std::string_view, std::uint32_t> semantics[]{
            {"POSITION", 0U}, {"NORMAL", 3U}, {"PSIZE", 4U}, {"TEXCOORD", 5U},
            {"TANGENT", 6U}, {"BINORMAL", 7U}, {"COLOR", 10U}, {"FOG", 11U}};
        for (const auto& [label, value] : semantics) {
            if (name.size() != label.size()) continue;
            bool equal = true;
            for (std::size_t i = 0U; i < label.size(); ++i) {
                const auto c = name[i];
                if ((c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c) != label[i]) equal = false;
            }
            if (equal) return value;
        }
        return {};
    };
    for (auto& leaf : reflection.interfaces) {
        leaf.source = {};
        if (leaf.builtin || !leaf.location || leaf.component.value_or(0U) != 0U
            || leaf.index.value_or(0U) != 0U || !leaf.member_path.empty()
            || !leaf.declared_component_mask) continue;
        const auto& signatures = leaf.direction == InterfaceDirection::input ? scanned.input : scanned.output;
        const ScannedSignature* match{};
        bool ambiguous{};
        for (const auto& signature : signatures) {
            if (signature.index != *leaf.location) continue;
            if (match) ambiguous = true;
            match = &signature;
        }
        if (!match || ambiguous || match->sysval || match->mask != *leaf.declared_component_mask
            || !match->mask || match->mask > 15U || (match->used_mask & ~match->mask)) continue;
        const auto semantic = usage(match->semantic);
        if (!semantic) continue;
        leaf.source.availability = AssociationAvailability::public_backend_metadata;
        leaf.source.semantic = D3dSemanticIdentity{*semantic, match->semantic_index, match->semantic};
        leaf.source.declared_component_mask = match->mask;
        leaf.source.used_component_mask = match->used_mask;
    }
}
} // namespace eawr::presentation::shader_runtime::detail
