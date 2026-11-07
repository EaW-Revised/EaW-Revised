#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/presentation/render_categories.hpp"
#include <algorithm>
#include <optional>
#include <span>

namespace eawr::presentation::render_categories {

// Diagnostic attribution follows XML-resolved turret roots (HPT-07), never
// mesh-name substrings. Mixed skinning and malformed ancestry stay unknown.
[[nodiscard]] inline HullPart hull_part(const assets::Mesh& mesh, const assets::Submesh& surface,
    std::span<const assets::Bone> bones, std::span<const std::size_t> turret_roots, bool complete = true) {
    if (!complete || turret_roots.empty()) return HullPart::unclassified;
    for (const auto root : turret_roots) if (root >= bones.size()) return HullPart::unclassified;
    const auto owned = [&](std::int32_t bone) -> std::optional<bool> {
        if (bone == -1) return false; // unbound, rigid model-space geometry
        if (bone < -1) return std::nullopt;
        for (std::size_t depth = 0; depth <= bones.size(); ++depth) {
            const auto index = static_cast<std::size_t>(bone);
            if (index >= bones.size()) return std::nullopt;
            if (std::find(turret_roots.begin(), turret_roots.end(), index) != turret_roots.end()) return true;
            bone = bones[index].parent;
            if (bone == -1) return false;
            if (bone < -1) return std::nullopt;
        }
        return std::nullopt; // cycle
    };
    bool turret{}, body{}, any{};
    const auto record = [&](std::int32_t bone) {
        const auto value = owned(bone);
        if (!value) return false;
        any = true;
        turret = turret || *value;
        body = body || !*value;
        return true;
    };
    if (surface.skin_bones.empty()) {
        if (!record(mesh.bone)) return HullPart::unclassified;
    } else for (const auto& vertex : surface.vertices) {
        for (std::size_t influence = 0; influence < vertex.bone_weights.size(); ++influence) {
            const auto weight = vertex.bone_weights[influence];
            if (weight == 0) continue;
            const auto local = vertex.bone_indices[influence];
            if (!std::isfinite(weight) || weight < 0 || local >= surface.skin_bones.size()) return HullPart::unclassified;
            const auto bone = surface.skin_bones[local];
            if (bone >= bones.size() || !record(static_cast<std::int32_t>(bone))) return HullPart::unclassified;
        }
    }
    if (!any || (turret && body)) return HullPart::unclassified;
    return turret ? HullPart::turret : HullPart::body;
}
} // namespace eawr::presentation::render_categories
