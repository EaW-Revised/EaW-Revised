#pragma once

#include "eawr/presentation/particles/render.hpp"

#include <cmath>
#include <optional>

namespace eawr::presentation::particles {

// PS-02: retain the complete contact frame relative to the contacted bone.
// Bone axes may include the owner's scale; the effect keeps its own scale.
[[nodiscard]] inline Vec3 contact_vector(const Basis3& basis, const Vec3 value) noexcept {
    return {basis.x.x * value.x + basis.y.x * value.y + basis.z.x * value.z,
            basis.x.y * value.x + basis.y.y * value.y + basis.z.y * value.z,
            basis.x.z * value.x + basis.y.z * value.y + basis.z.z * value.z};
}

[[nodiscard]] inline EmitterFrame contact_world_frame(const EmitterFrame& bone,
                                                      const EmitterFrame& offset) noexcept {
    const Vec3 displacement = contact_vector(bone.basis, offset.origin);
    return {{bone.origin.x + displacement.x, bone.origin.y + displacement.y, bone.origin.z + displacement.z},
            {contact_vector(bone.basis, offset.basis.x), contact_vector(bone.basis, offset.basis.y),
             contact_vector(bone.basis, offset.basis.z)}};
}

[[nodiscard]] inline std::optional<EmitterFrame> contact_local_frame(const EmitterFrame& bone,
                                                                   const EmitterFrame& world) noexcept {
    const auto cross = [](const Vec3 a, const Vec3 b) {
        return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };
    const auto dot = [](const Vec3 a, const Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const Vec3 x = cross(bone.basis.y, bone.basis.z);
    const Vec3 y = cross(bone.basis.z, bone.basis.x);
    const Vec3 z = cross(bone.basis.x, bone.basis.y);
    const float determinant = dot(bone.basis.x, x);
    if (!std::isfinite(determinant) || determinant == 0.0F) return std::nullopt;
    const auto inverse = [&](const Vec3 value) {
        return Vec3{dot(x, value) / determinant, dot(y, value) / determinant, dot(z, value) / determinant};
    };
    EmitterFrame result{inverse({world.origin.x - bone.origin.x, world.origin.y - bone.origin.y,
                                world.origin.z - bone.origin.z}),
                        {inverse(world.basis.x), inverse(world.basis.y), inverse(world.basis.z)}};
    for (const Vec3 value : {result.origin, result.basis.x, result.basis.y, result.basis.z}) {
        if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) return std::nullopt;
    }
    return result;
}

} // namespace eawr::presentation::particles
