#pragma once

#include "eawr/assets/assets.hpp"

#include <array>
#include <cmath>

namespace eawr::presentation::godot_backend::detail {

// Hidden skin palettes keep their bone origins. Different influence streams
// can therefore leave a triangle between those origins even with zero bases.
// Two identical streams guarantee coincident vertices and a degenerate face.
template <typename Mesh>
[[nodiscard]] inline bool hidden_surface_collapses(const Mesh& mesh,
    const assets::Submesh& surface, const std::size_t bone_count) noexcept {
    const bool palette = !surface.skin_bones.empty();
    const bool rigid = !palette && mesh.bone >= 0;
    if ((!palette && !rigid) || bone_count == 0
        || (rigid && static_cast<std::size_t>(mesh.bone) >= bone_count)) return false;
    const auto valid_vertex = [&](const assets::Vertex& vertex) {
        const auto& p = vertex.position;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
        if (rigid) return true; // Upload replaces this route's weights with one rigid bone.
        bool weighted = false;
        for (std::size_t influence = 0; influence < 4; ++influence) {
            const float weight = vertex.bone_weights[influence];
            if (!std::isfinite(weight) || weight < 0.0F) return false;
            if (weight == 0.0F) continue;
            weighted = true;
            const auto local = vertex.bone_indices[influence];
            if (local >= surface.skin_bones.size() || surface.skin_bones[local] >= bone_count) return false;
        }
        return weighted;
    };
    const auto same_influences = [&](const assets::Vertex& left, const assets::Vertex& right) {
        for (std::size_t influence = 0; influence < 4; ++influence) {
            const float weight = left.bone_weights[influence];
            if (weight != right.bone_weights[influence]) return false;
            if (weight != 0.0F && surface.skin_bones[left.bone_indices[influence]]
                != surface.skin_bones[right.bone_indices[influence]]) return false;
        }
        return true;
    };
    const std::size_t count = surface.indices.empty() ? surface.vertices.size() : surface.indices.size();
    if (count % 3 != 0) return false;
    for (std::size_t index = 0; index < count; index += 3) {
        const std::array<std::size_t, 3> corners = surface.indices.empty()
            ? std::array<std::size_t, 3>{index, index + 1, index + 2}
            : std::array<std::size_t, 3>{surface.indices[index], surface.indices[index + 1], surface.indices[index + 2]};
        for (const auto corner : corners)
            if (corner >= surface.vertices.size() || !valid_vertex(surface.vertices[corner])) return false;
        if (!rigid && !same_influences(surface.vertices[corners[0]], surface.vertices[corners[1]])
            && !same_influences(surface.vertices[corners[0]], surface.vertices[corners[2]])
            && !same_influences(surface.vertices[corners[1]], surface.vertices[corners[2]])) return false;
    }
    return true;
}

template <typename Model>
[[nodiscard]] inline bool hidden_skin_geometry_collapses(const Model& model) noexcept {
    // Billboard posing can replace a hidden palette's basis independently of
    // the clip. Keep unknown/unsupported modes conservative too.
    for (const auto& bone : model.bones)
        if ((bone.billboard & 15U) != 0) return false;
    bool geometry = false;
    for (const auto& mesh : model.meshes) {
        if (!mesh.visible) continue;
        for (const auto& surface : mesh.submeshes) {
            if (surface.vertices.empty() && surface.indices.empty()) continue;
            geometry = true;
            if (!hidden_surface_collapses(mesh, surface, model.bones.size())) return false;
        }
    }
    return geometry;
}

} // namespace eawr::presentation::godot_backend::detail
