#pragma once

#include "bindings.hpp"
#include "family.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// MeshGlow.fx sph_t0/sph_t0_p0 (#8037; CEM-19). A mod-authored effect, not a retail one: following the owner's decision
// (#3834) its compiled form was read locally on a mod host (vs_1_1 and ps_1_1, tools/shaders/d3d9_bytecode.py) to learn
// what it does, and the behaviour is described here in our own words; nothing is copied. Vertex stage: position times the world-view-projection,
// UV passed through, vertex colour (LIGHT_SCALE.rgb, 1) saturated, no normal and no light read, plus the engine
// distance fog the legacy adapters all disable. Pixel stage (ps_1_1): RGB = 2 x base.rgb x D.rgb x DistortionScale,
// alpha D.a = 1, saturated at the 8-bit target. DistortionScale is the effect's one float parameter, default 1.
// Declared states: ZWriteEnable TRUE, ZFunc LESSEQUAL, SrcBlend SRCALPHA / DestBlend INVSRCALPHA with
// AlphaBlendEnable FALSE, so the pass draws opaque although its render phase is Transparent. The pass reads no
// light, so it receives no shadows. Output is the stored value, as for the other unlit legacy/ families.
namespace eawr::presentation::godot_backend::legacy::mesh_glow {

inline constexpr std::string_view shader_source = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, depth_draw_opaque, depth_test_default, cull_back;

uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform float eawr_distortion_scale = 1.0;
uniform vec4 eawr_light_scale = vec4(1.0);
// WR-14/37: object light scale also reaches this unlit pass.
instance uniform vec3 eawr_unit_light_scale = vec3(1.0);
varying vec3 eawr_vertex_color;

vec3 eawr_stored_albedo(vec3 stored_rgb) {
    return stored_rgb;
}

void vertex() {
    eawr_vertex_color = clamp(eawr_light_scale.rgb * eawr_unit_light_scale, 0.0, 1.0);
}

void fragment() {
    vec3 stored_rgb = clamp(2.0 * texture(BaseTexture, UV).rgb * eawr_vertex_color * eawr_distortion_scale, 0.0, 1.0);
    ALBEDO = eawr_stored_albedo(stored_rgb);
}
)GODOT";

inline constexpr float default_distortion_scale = 1.0F;

[[nodiscard]] inline std::string_view shader(const RenderPass pass) noexcept {
    return pass == RenderPass::opaque ? shader_source : std::string_view{};
}

[[nodiscard]] inline std::optional<std::string> binding_problem(const MaterialDescription& material) {
    return bindings::scalar_problem(material, "DistortionScale");
}

[[nodiscard]] inline std::vector<Uniform> uniforms(const MaterialDescription& material) {
    return {{"eawr_distortion_scale", bindings::scalar_or(material, "DistortionScale", default_distortion_scale)}};
}

// Engine-free reference: the stored RGB for a texel and the vertex colour.
[[nodiscard]] constexpr std::array<float, 3> reference_pixel(const std::array<float, 3>& texel,
    const std::array<float, 3>& vertex_color, const float distortion_scale) noexcept {
    std::array<float, 3> result{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        result[channel] = bindings::saturate(
            2.0F * texel[channel] * bindings::saturate(vertex_color[channel]) * distortion_scale);
    }
    return result;
}

inline constexpr Family family{
    .program = "MeshGlow.fx",
    .technique = "sph_t0",
    .pass_name = "sph_t0_p0",
    .opaque = true,
    .transparent = false,
    .receives_shadows = false,
    .shader = &shader,
    .binding_problem = &binding_problem,
    .uniforms = &uniforms,
};

} // namespace eawr::presentation::godot_backend::legacy::mesh_glow
