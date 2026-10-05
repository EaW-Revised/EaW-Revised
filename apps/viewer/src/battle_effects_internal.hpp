#pragma once

#include "eawr/core/load_profile.hpp"
#include "battle_effects.hpp"

#include "eawr/presentation/space/space.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/start.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

namespace eawr::presentation::godot_backend::battle_effects_detail {
using namespace godot;
namespace tactical = sim::tactical;


// The report's spawn log keeps this many rows.
constexpr std::size_t spawn_log_limit = 8192;

using V = particles::Vec3;

[[nodiscard]] inline float to_float(const sim::math::Fixed value) {
    return static_cast<float>(static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale));
}
[[nodiscard]] inline V add(const V a, const V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
[[nodiscard]] inline V sub(const V a, const V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] inline V scale(const V a, const float s) { return {a.x * s, a.y * s, a.z * s}; }
[[nodiscard]] inline V vec(const sim::math::Vec3& value) { return {to_float(value.x), to_float(value.y), to_float(value.z)}; }
[[nodiscard]] inline V vec(const assets::Vec3f& value) { return {value.x, value.y, value.z}; }

[[nodiscard]] inline V vec(const space::Vec3d& value) {
    return {static_cast<float>(value[0]), static_cast<float>(value[1]), static_cast<float>(value[2])};
}

// Every triangle of `mesh`'s submeshes, its corners placed by `place`.
template <typename Place>
void append_triangles(const assets::Mesh& mesh, const Place& place, std::vector<space::ShieldTriangle>& result) {
    for (const assets::Submesh& submesh : mesh.submeshes) {
        for (std::size_t index = 0; index + 2 < submesh.indices.size(); index += 3) {
            const std::array<std::uint16_t, 3> corner{submesh.indices[index], submesh.indices[index + 1],
                                                      submesh.indices[index + 2]};
            if (corner[0] >= submesh.vertices.size() || corner[1] >= submesh.vertices.size()
                || corner[2] >= submesh.vertices.size()) {
                continue;
            }
            result.push_back({place(submesh.vertices[corner[0]].position), place(submesh.vertices[corner[1]].position),
                              place(submesh.vertices[corner[2]].position)});
        }
    }
}
} // namespace eawr::presentation::godot_backend::battle_effects_detail
