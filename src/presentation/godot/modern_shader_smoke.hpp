#pragma once

#include <string_view>

namespace eawr::presentation::godot_backend {

// Independent newly authored material smoke. It deliberately does not pass
// through the legacy FX descriptor/adapter.
inline constexpr std::string_view modern_shader_smoke = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, depth_draw_opaque, depth_test_default, cull_back;

uniform sampler2D authored_texture : filter_linear_mipmap, repeat_enable;
uniform vec4 authored_accent = vec4(0.1, 0.8, 1.0, 1.0);
uniform float authored_displacement = 0.0;

void vertex() {
    VERTEX += NORMAL * authored_displacement;
}

void fragment() {
    vec4 sampled = texture(authored_texture, UV);
    float bands = 0.5 + 0.5 * sin((UV.x + UV.y) * 48.0);
    ALBEDO = sampled.rgb * mix(vec3(1.0), authored_accent.rgb, bands * 0.35);
}
)GODOT";

} // namespace eawr::presentation::godot_backend
