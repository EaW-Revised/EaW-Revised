#pragma once

#include "bindings.hpp"
#include "family.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// MeshAdditive.fx t0/t0_p0 for placed objects (WP-43). The arithmetic is
// docs/behaviour/meshadditive-sun-billboard.md A-01..A-10: the first UV plus
// TIME x UVScrollRate.xy, a per-vertex colour saturate(Color.rgb x
// LIGHT_SCALE.rgb x LIGHT_SCALE.a), RGB = texel.rgb x colour, and ONE/ONE
// additive blending without depth writes. The effect declares Color as a
// float3 with initializer (1, 1, 1) and UVScrollRate as a float2 with (0, 0);
// the P1-02 descriptor omits Color only because its parameter extraction
// drops that declaration. The colour policy is the accepted sky MeshAdditive
// route's stored-value output (P1-06), not the linear policy of the lit
// adapters: the pass reads no light, so it never receives shadows.
namespace eawr::presentation::godot_backend::legacy::mesh_additive {

inline constexpr std::string_view shader_source = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, blend_add, depth_draw_never, depth_test_default, cull_back;

uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform vec3 eawr_color = vec3(1.0);
uniform vec2 eawr_uv_scroll_rate = vec2(0.0);
uniform float eawr_time = 0.0;
uniform vec4 eawr_light_scale = vec4(1.0);
// WR-14/37: object light scale also reaches attached additive model surfaces.
instance uniform vec3 eawr_unit_light_scale = vec3(1.0);
varying vec2 eawr_scrolled_uv;
varying vec3 eawr_vertex_color;

// ALBEDO under the stored-value output (docs/rendering.md, colour policy);
// the Compatibility fallback compiles compatibility_albedo_writer instead.
vec3 eawr_stored_albedo(vec3 stored_rgb) {
    return stored_rgb;
}

void vertex() {
    // Unwrapped; the sampler's repeat addressing wraps it per sample.
    eawr_scrolled_uv = UV + eawr_time * eawr_uv_scroll_rate;
    eawr_vertex_color = clamp(eawr_color * eawr_light_scale.rgb * eawr_unit_light_scale * eawr_light_scale.a, 0.0, 1.0);
}

void fragment() {
    // Texture alpha never scales the ONE/ONE RGB; fragment alpha stays 1.
    vec3 stored_rgb = eawr_vertex_color * texture(BaseTexture, eawr_scrolled_uv).rgb;
    ALBEDO = eawr_stored_albedo(stored_rgb);
}
)GODOT";

inline constexpr std::array<float, 4> default_color{1.0F, 1.0F, 1.0F, 0.0F};
inline constexpr std::array<float, 4> default_uv_scroll_rate{0.0F, 0.0F, 0.0F, 0.0F};
// legacy-meshadditive-inputs-v1: the renderer has no presentation clock, so
// TIME is fixed at 0 s and a fixed capture is deterministic. Not the retail
// clock (gate TIME-01).
inline constexpr float fixed_time = 0.0F;

[[nodiscard]] inline std::string_view shader(const RenderPass pass) noexcept {
    return pass == RenderPass::transparent ? shader_source : std::string_view{};
}

[[nodiscard]] inline std::optional<std::string> binding_problem(const MaterialDescription& material) {
    for (const std::string_view name : {std::string_view("Color"), std::string_view("UVScrollRate")}) {
        if (auto problem = bindings::vector_problem(material, name)) return problem;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::vector<Uniform> uniforms(const MaterialDescription& material) {
    const auto color = bindings::vector_or(material, "Color", default_color);
    const auto rate = bindings::vector_or(material, "UVScrollRate", default_uv_scroll_rate);
    return {
        {"eawr_color", std::array<float, 3>{color[0], color[1], color[2]}},
        {"eawr_uv_scroll_rate", std::array<float, 2>{rate[0], rate[1]}},
        {"eawr_time", fixed_time},
    };
}

// Engine-free reference: the scrolled, unwrapped texture coordinate (A-03).
[[nodiscard]] constexpr std::array<float, 2> reference_uv(
    const std::array<float, 2>& uv, const std::array<float, 2>& rate, const float time) noexcept {
    return {uv[0] + time * rate[0], uv[1] + time * rate[1]};
}

// Engine-free reference: stored framebuffer RGB after one ONE/ONE draw of a
// texel over `destination`, saturated by the 8-bit target (A-04, A-06..A-08).
[[nodiscard]] constexpr std::array<float, 3> reference_add(
    const std::array<float, 3>& destination, const std::array<float, 3>& texel,
    const std::array<float, 3>& color, const std::array<float, 4>& light_scale) noexcept {
    std::array<float, 3> result{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const float vertex = bindings::saturate(color[channel] * light_scale[channel] * light_scale[3]);
        result[channel] = bindings::saturate(destination[channel] + texel[channel] * vertex);
    }
    return result;
}

inline constexpr Family family{
    .program = "MeshAdditive.fx",
    .technique = "t0",
    .pass_name = "t0_p0",
    .opaque = false,
    .transparent = true,
    .receives_shadows = false,
    .shader = &shader,
    .binding_problem = &binding_problem,
    .uniforms = &uniforms,
};

} // namespace eawr::presentation::godot_backend::legacy::mesh_additive
