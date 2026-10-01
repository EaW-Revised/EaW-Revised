#pragma once

#include "bindings.hpp"
#include "family.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// MeshAdditiveOffset.fx t0/t0_p0 for placed objects (WP-43). It shares
// MeshAdditive's pixel stage, vertex colour and ONE/ONE no-depth-write state
// (see mesh_additive.hpp); the only difference in the selected pass is the
// texture coordinate, which is the first UV plus UVOffset.xy with no TIME
// term. The effect declares UVOffset as a float4 with initializer (0,0,0,0)
// and Color as a float3 with (1, 1, 1); the P1-02 descriptor omits Color for
// the same extraction reason as MeshAdditive. The FIXEDFUNCTION t1 texture
// transform applies the same offset, but t1 is not selected here.
namespace eawr::presentation::godot_backend::legacy::mesh_additive_offset {

inline constexpr std::string_view shader_source = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, blend_add, depth_draw_never, depth_test_default, cull_back;

uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform vec3 eawr_color = vec3(1.0);
uniform vec2 eawr_uv_offset = vec2(0.0);
uniform vec4 eawr_light_scale = vec4(1.0);
// WR-14/37: object light scale also reaches attached additive model surfaces.
instance uniform vec3 eawr_unit_light_scale = vec3(1.0);
varying vec2 eawr_offset_uv;
varying vec3 eawr_vertex_color;

// ALBEDO under the stored-value output (docs/rendering.md, colour policy);
// the Compatibility fallback compiles compatibility_albedo_writer instead.
vec3 eawr_stored_albedo(vec3 stored_rgb) {
    return stored_rgb;
}

void vertex() {
    eawr_offset_uv = UV + eawr_uv_offset;
    eawr_vertex_color = clamp(eawr_color * eawr_light_scale.rgb * eawr_unit_light_scale * eawr_light_scale.a, 0.0, 1.0);
}

void fragment() {
    vec3 stored_rgb = eawr_vertex_color * texture(BaseTexture, eawr_offset_uv).rgb;
    ALBEDO = eawr_stored_albedo(stored_rgb);
}
)GODOT";

inline constexpr std::array<float, 4> default_color{1.0F, 1.0F, 1.0F, 0.0F};
inline constexpr std::array<float, 4> default_uv_offset{0.0F, 0.0F, 0.0F, 0.0F};

[[nodiscard]] inline std::string_view shader(const RenderPass pass) noexcept {
    return pass == RenderPass::transparent ? shader_source : std::string_view{};
}

[[nodiscard]] inline std::optional<std::string> binding_problem(const MaterialDescription& material) {
    for (const std::string_view name : {std::string_view("Color"), std::string_view("UVOffset")}) {
        if (auto problem = bindings::vector_problem(material, name)) return problem;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::vector<Uniform> uniforms(const MaterialDescription& material) {
    const auto color = bindings::vector_or(material, "Color", default_color);
    const auto offset = bindings::vector_or(material, "UVOffset", default_uv_offset);
    return {
        {"eawr_color", std::array<float, 3>{color[0], color[1], color[2]}},
        {"eawr_uv_offset", std::array<float, 2>{offset[0], offset[1]}},
    };
}

// Engine-free reference: the offset, unwrapped texture coordinate.
[[nodiscard]] constexpr std::array<float, 2> reference_uv(
    const std::array<float, 2>& uv, const std::array<float, 2>& offset) noexcept {
    return {uv[0] + offset[0], uv[1] + offset[1]};
}

inline constexpr Family family{
    .program = "MeshAdditiveOffset.fx",
    .technique = "t0",
    .pass_name = "t0_p0",
    .opaque = false,
    .transparent = true,
    .receives_shadows = false,
    .shader = &shader,
    .binding_problem = &binding_problem,
    .uniforms = &uniforms,
};

} // namespace eawr::presentation::godot_backend::legacy::mesh_additive_offset
