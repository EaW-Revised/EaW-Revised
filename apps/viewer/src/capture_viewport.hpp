#pragma once

#include "eawr/presentation/renderer.hpp"

#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <cstdint>
#include <string>

namespace eawr::presentation::godot_backend {

// A probe reads the root viewport back, with or without --eawr-capture, and
// a window manager (PowerToys FancyZones, a tiling manager, a maximise) may
// resize the OS window once the first frame runs. Pinning the root window's
// content scale makes Godot draw the root viewport at exactly `width` x
// `height` and only scale that image into whatever window the OS gives it, so
// the read-back keeps the capture identity. Every probe pins when it
// activates; only an interactive run and an unlocked camera self-test, whose
// subject is following real resizes, leave the viewport following the window.
inline void pin_capture_viewport(godot::Window& window, const std::uint32_t width, const std::uint32_t height) {
    window.set_content_scale_factor(1.0F);
    window.set_content_scale_aspect(godot::Window::CONTENT_SCALE_ASPECT_KEEP);
    window.set_content_scale_size(godot::Vector2i(static_cast<int32_t>(width), static_cast<int32_t>(height)));
    window.set_content_scale_mode(godot::Window::CONTENT_SCALE_MODE_VIEWPORT);
}

// Empty when the read-back has the camera's size, otherwise why not. A capture
// run fails on a mismatch instead of writing an image its report misdescribes.
[[nodiscard]] inline std::string capture_size_problem(const CaptureResult& capture, const FixedCamera& camera) {
    if (capture.width == camera.width && capture.height == camera.height) return {};
    return "the capture read back " + std::to_string(capture.width) + "x" + std::to_string(capture.height)
        + ", not the camera's " + std::to_string(camera.width) + "x" + std::to_string(camera.height);
}

} // namespace eawr::presentation::godot_backend
