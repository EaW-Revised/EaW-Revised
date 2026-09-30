#pragma once

#include "bindings.hpp"
#include "family.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// MeshSolidColor.fx t0/t0_p0 for placed objects (WP-43). The selected pass
// (vertex and pixel model 1.1, render phase Opaque, vertex type alD3dVertN)
// outputs the authored Color unchanged, saturated to [0, 1] as a ps_1_1
// colour input, and reads no texture, normal or light. Its declared states are
// AlphaBlendEnable FALSE (source ONE, destination ZERO), ZWriteEnable FALSE
// and ZFunc LESSEQUAL; cull is the renderer baseline. The effect declares
// Color as a float4 with initializer (0, 0, 1, 0.5); with blending off its
// alpha reaches no visible output. Output is the stored value, as for the
// other unlit legacy/ families.
namespace eawr::presentation::godot_backend::legacy::mesh_solid_color {

inline constexpr std::string_view shader_source = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, depth_draw_never, depth_test_default, cull_back;

// Declared only as the renderer's legacy compile probe: the pass samples no texture.
uniform sampler2D BaseTexture : filter_nearest, repeat_disable;
uniform vec3 eawr_color = vec3(0.0, 0.0, 1.0);

// ALBEDO under the stored-value output (docs/rendering.md, colour policy);
// the Compatibility fallback compiles compatibility_albedo_writer instead.
vec3 eawr_stored_albedo(vec3 stored_rgb) {
    return stored_rgb;
}

void fragment() {
    vec3 stored_rgb = clamp(eawr_color, 0.0, 1.0);
    ALBEDO = eawr_stored_albedo(stored_rgb);
}
)GODOT";

inline constexpr std::array<float, 4> default_color{0.0F, 0.0F, 1.0F, 0.5F};

[[nodiscard]] inline std::string_view shader(const RenderPass pass) noexcept {
    return pass == RenderPass::opaque ? shader_source : std::string_view{};
}

[[nodiscard]] inline std::optional<std::string> binding_problem(const MaterialDescription& material) {
    return bindings::vector_problem(material, "Color");
}

[[nodiscard]] inline std::vector<Uniform> uniforms(const MaterialDescription& material) {
    const auto color = bindings::vector_or(material, "Color", default_color);
    return {{"eawr_color", std::array<float, 3>{color[0], color[1], color[2]}}};
}

// Engine-free reference: the stored framebuffer RGB the pass writes.
[[nodiscard]] constexpr std::array<float, 3> reference_color(const std::array<float, 3>& color) noexcept {
    return {bindings::saturate(color[0]), bindings::saturate(color[1]), bindings::saturate(color[2])};
}

inline constexpr Family family{
    .program = "MeshSolidColor.fx",
    .technique = "t0",
    .pass_name = "t0_p0",
    .opaque = true,
    .transparent = false,
    .receives_shadows = false,
    .shader = &shader,
    .binding_problem = &binding_problem,
    .uniforms = &uniforms,
};

} // namespace eawr::presentation::godot_backend::legacy::mesh_solid_color
