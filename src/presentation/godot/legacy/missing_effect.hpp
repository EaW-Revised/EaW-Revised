#pragma once

#include "family.hpp"

// ALDEFAULT-01: mounted-file lookup selects this renderer-owned material.
// The original's built-in placeholder is opaque green and samples no authored
// texture or colour. Its geometry participates in the ordinary shadow path.
namespace eawr::presentation::godot_backend::legacy::missing_effect {

inline constexpr std::string_view shader_source = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, depth_draw_opaque, depth_test_default, cull_back;

// Reflection probe only; the missing material has no texture input.
uniform sampler2D BaseTexture : filter_nearest, repeat_disable;
uniform vec3 eawr_fallback_colour = vec3(0.0, 1.0, 0.0);

vec3 eawr_stored_albedo(vec3 stored_rgb) {
    return stored_rgb;
}

void fragment() {
    vec3 stored_rgb = eawr_fallback_colour;
    ALBEDO = eawr_stored_albedo(stored_rgb);
}
)GODOT";

[[nodiscard]] inline std::string_view shader(const RenderPass pass) noexcept {
    return pass == RenderPass::opaque ? shader_source : std::string_view{};
}

[[nodiscard]] inline std::optional<std::string> binding_problem(const MaterialDescription&) {
    return std::nullopt;
}

[[nodiscard]] inline std::vector<Uniform> uniforms(const MaterialDescription&) {
    return {{"eawr_fallback_colour", std::array<float, 3>{0.0F, 1.0F, 0.0F}}};
}

inline constexpr Family family{
    .program = "eawr_missing_effect",
    .technique = "fallback",
    .pass_name = "opaque",
    .opaque = true,
    .transparent = false,
    .receives_shadows = true,
    .shader = &shader,
    .binding_problem = &binding_problem,
    .uniforms = &uniforms,
};

} // namespace eawr::presentation::godot_backend::legacy::missing_effect
