#include "viewer_host_internal.hpp"

#include "ui/input_routing.hpp"

#include <godot_cpp/classes/canvas_layer.hpp>

namespace eawr::presentation::godot_backend {
namespace {

} // namespace

// P1-09 camera input: Godot event translation and synthetic injection. The
// binding loader and lifecycle adapter themselves are engine independent
// (camera_input.hpp); only this translation layer knows Godot types.
namespace {

} // namespace

namespace viewer_host_detail {

// Unit vector from a camera's eye toward its target; zero for a degenerate
// camera, which no self-test comparison accepts.
[[nodiscard]] std::array<float, 3> view_direction(const FixedCamera& camera) {
    const std::array<float, 3> delta{camera.target[0] - camera.eye[0],
        camera.target[1] - camera.eye[1], camera.target[2] - camera.eye[2]};
    const float length = std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
    if (!(length > 0.0F)) return {};
    return {delta[0] / length, delta[1] / length, delta[2] / length};
}

} // namespace viewer_host_detail

namespace {

} // namespace



} // namespace eawr::presentation::godot_backend
