#pragma once

#include "eawr/presentation/renderer.hpp"
#include "eawr/presentation/render_categories.hpp"
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <cmath>
#include <map>
#include <optional>

namespace eawr::presentation::godot_backend::distance_band {
// Only populated by opt-in diagnostics; keyed by scenario, never a global camera.
inline thread_local std::map<std::uint64_t, FixedCamera> cameras;
[[nodiscard]] inline double coverage(const godot::AABB& bounds,
    const godot::Transform3D& transform, const FixedCamera& camera) {
    if (!bounds.position.is_finite() || !bounds.size.is_finite()
        || bounds.size.length_squared() <= 0 || camera.height == 0 || camera.width == 0) return -1;
    const auto world = transform.xform(bounds);
    const auto center = world.get_center();
    const double radius = world.size.length() * 0.5;
    const godot::Vector3 eye(camera.eye[0], camera.eye[1], camera.eye[2]);
    const godot::Vector3 target(camera.target[0], camera.target[1], camera.target[2]);
    const auto forward = (target - eye).normalized();
    const auto offset = center - eye;
    const double depth = forward.dot(offset);
    const double lateral = (offset - forward * static_cast<decltype(forward.x)>(depth)).length();
    const double tangent = std::tan(camera.vertical_fov_degrees * 0.008726646259971648);
    // Conservative bounding sphere projected at its nearest depth. No decision
    // for a near-plane crossing, invalid transform, or missing bounds/camera.
    return render_categories::projected_diameter(radius, depth, camera.near_plane, tangent, lateral);
}
[[nodiscard]] inline double coverage(const godot::AABB& bounds,
    const godot::Transform3D& transform, std::uint64_t scenario) {
    const auto found = cameras.find(scenario);
    return found == cameras.end() ? -1 : coverage(bounds, transform, found->second);
}
} // namespace eawr::presentation::godot_backend::distance_band
