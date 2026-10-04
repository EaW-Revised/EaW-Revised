#pragma once

#include "eawr/presentation/particles/render.hpp"
#include "eawr/presentation/renderer.hpp"

#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <map>

namespace eawr::presentation::godot_backend::particle_culling {

// PS-39: the scene's camera snapshot is independent of backend linkage, so
// standalone renderer probes can use the same renderer without a particle sink.
inline thread_local std::map<std::uint64_t, particles::CullingFrame> cameras;

inline void set_camera(const godot::RID& scenario, const FixedCamera& camera) {
    particles::CullingFrame result;
    if (camera.width == 0 || camera.height == 0) { cameras[scenario.get_id()] = result; return; }
    godot::Transform3D transform;
    transform.origin = godot::Vector3(camera.eye[0], camera.eye[1], camera.eye[2]);
    transform = transform.looking_at(godot::Vector3(camera.target[0], camera.target[1], camera.target[2]),
        godot::Vector3(camera.up[0], camera.up[1], camera.up[2]));
    const auto projection = godot::Projection::create_perspective(camera.vertical_fov_degrees,
        static_cast<float>(camera.width) / camera.height, camera.near_plane, camera.far_plane);
    const auto planes = projection.get_projection_planes(transform);
    if (planes.size() != 6) { cameras[scenario.get_id()] = result; return; }
    for (int64_t index = 0; index < 6; ++index) {
        const godot::Plane plane = planes[index];
        result.planes[static_cast<std::size_t>(index)] = {static_cast<float>(plane.normal.x),
            static_cast<float>(-plane.normal.z), static_cast<float>(plane.normal.y), static_cast<float>(plane.d)};
    }
    result.valid = true;
    result.enabled = true;
    cameras[scenario.get_id()] = result;
}

[[nodiscard]] inline particles::CullingFrame frame(const godot::RID& scenario) {
    const auto found = cameras.find(scenario.get_id());
    return found == cameras.end() ? particles::CullingFrame{} : found->second;
}
inline void clear_camera(const godot::RID& scenario) { cameras.erase(scenario.get_id()); }

} // namespace eawr::presentation::godot_backend::particle_culling
