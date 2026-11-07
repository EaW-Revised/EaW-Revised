#pragma once

namespace eawr::presentation::godot_backend {

// LAN button decoding must finish while the gesture owns its admission epoch.
// Standalone input retains the engine's normal buffered dispatch boundary.
template <typename Engine, typename Event, typename EpochScope>
void dispatch_scripted_input(Engine& engine, const Event& event, const EpochScope& input_epoch) {
    engine.parse_input_event(event);
    if (input_epoch && *input_epoch) engine.flush_buffered_events();
}

} // namespace eawr::presentation::godot_backend

#include <array>

namespace eawr::presentation::godot_backend {

// #5217: Input::parse_input_event takes WINDOW pixels and the viewport maps them back through the
// inverse of its final transform (content scale). Scripted points are viewport pixels (the visible
// rect), so they go through the final transform first; at scale 1.0 it is the identity.
// `m` is Godot's Transform2D as {x.x, x.y, y.x, y.y, origin.x, origin.y}.
using Affine2D = std::array<float, 6>;

[[nodiscard]] constexpr std::array<float, 2> viewport_to_window_point(const Affine2D& m, const std::array<float, 2>& p) {
    return {m[0] * p[0] + m[2] * p[1] + m[4], m[1] * p[0] + m[3] * p[1] + m[5]};
}
// A displacement (motion relative): the linear part only.
[[nodiscard]] constexpr std::array<float, 2> viewport_to_window_delta(const Affine2D& m, const std::array<float, 2>& d) {
    return {m[0] * d[0] + m[2] * d[1], m[1] * d[0] + m[3] * d[1]};
}

} // namespace eawr::presentation::godot_backend
