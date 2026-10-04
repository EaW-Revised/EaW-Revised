#pragma once

#include "bindings.hpp"
#include "family.hpp"

// AVC-01..06: authored vertex RGB, scrolling first UV and additive stored-value output.
namespace eawr::presentation::godot_backend::legacy::mesh_additive_vcolor {

inline constexpr std::string_view shader_source = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, blend_add, depth_draw_never, depth_test_default, cull_back;

uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform vec2 eawr_uv_scroll_rate = vec2(0.0);
uniform float eawr_effect_time = 0.0;
uniform vec4 eawr_light_scale = vec4(1.0);
instance uniform vec3 eawr_unit_light_scale = vec3(1.0);
varying vec3 eawr_tint;
varying vec2 eawr_sample_uv;

vec3 eawr_stored_albedo(vec3 stored_rgb) {
    return stored_rgb;
}

void vertex() {
    eawr_sample_uv = UV + eawr_effect_time * eawr_uv_scroll_rate;
    eawr_tint = clamp(COLOR.rgb * eawr_light_scale.rgb * eawr_light_scale.a * eawr_unit_light_scale, 0.0, 1.0);
}

void fragment() {
    vec3 stored_rgb = texture(BaseTexture, eawr_sample_uv).rgb * eawr_tint;
    ALBEDO = eawr_stored_albedo(stored_rgb);
}
)GODOT";

[[nodiscard]] inline std::string_view shader(const RenderPass pass) noexcept {
    return pass == RenderPass::transparent ? shader_source : std::string_view{};
}

[[nodiscard]] inline std::optional<std::string> binding_problem(const MaterialDescription& material) {
    if (auto problem = bindings::vector_problem(material, "UVScrollRate")) return problem;
    return bindings::scalar_problem(material, "eawr_effect_time");
}

[[nodiscard]] inline std::vector<Uniform> uniforms(const MaterialDescription& material) {
    const auto rate = bindings::vector_or(material, "UVScrollRate", {0.0F, 0.0F, 0.0F, 0.0F});
    return {{"eawr_uv_scroll_rate", std::array<float, 2>{rate[0], rate[1]}},
        {"eawr_effect_time", bindings::scalar_or(material, "eawr_effect_time", 0.0F)}};
}

// AVC-03..04: clamp before interpolation; texture/vertex alpha never changes RGB.
[[nodiscard]] constexpr std::array<float, 3> reference_vertex(
    const std::array<float, 4>& colour, const std::array<float, 4>& light_scale) noexcept {
    std::array<float, 3> result{};
    for (std::size_t channel = 0; channel < result.size(); ++channel) {
        result[channel] = bindings::saturate(colour[channel] * light_scale[channel] * light_scale[3]);
    }
    return result;
}

inline constexpr Family family{
    .program = "MeshAdditiveVColor.fx",
    .technique = "t0",
    .pass_name = "t0_p0",
    .opaque = false,
    .transparent = true,
    .receives_shadows = false,
    .shader = &shader,
    .binding_problem = &binding_problem,
    .uniforms = &uniforms,
};

} // namespace eawr::presentation::godot_backend::legacy::mesh_additive_vcolor
