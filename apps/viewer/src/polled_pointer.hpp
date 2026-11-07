#pragma once

#include "edge_pointer.hpp"
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>

namespace eawr::presentation::godot_backend {

[[nodiscard]] inline PointerWindowMode pointer_window_mode(const godot::DisplayServer::WindowMode mode) noexcept {
    if (mode == godot::DisplayServer::WINDOW_MODE_FULLSCREEN) return PointerWindowMode::fullscreen;
    if (mode == godot::DisplayServer::WINDOW_MODE_EXCLUSIVE_FULLSCREEN) return PointerWindowMode::exclusive_fullscreen;
    return PointerWindowMode::windowed;
}

// DisplayServer reports desktop pixels; input and camera use viewport coordinates.
[[nodiscard]] inline godot::Vector2 polled_pointer(godot::Node3D& host) {
    const godot::Vector2i desktop = godot::DisplayServer::get_singleton()->mouse_get_position();
    const godot::Vector2i origin = host.get_window()->get_position();
    return host.get_viewport()->get_final_transform().affine_inverse().xform(godot::Vector2(desktop - origin));
}

} // namespace eawr::presentation::godot_backend
