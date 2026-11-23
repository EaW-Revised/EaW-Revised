#pragma once

#include "dx8_mesh.hpp"
#include "mesh_additive.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// MeshAlphaScroll.fx sph_t0/sph_t0_p0 (#8037). The effect is the DX8 alpha
// program (see dx8_mesh.hpp, MeshAlphaGloss without the gloss texture and
// BatchMeshAlpha without the fog-of-war stage) with one difference: the first
// texture coordinate is UV + TIME x UVScrollRate.xy in the vertex stage
// (UVScrollRate is a float2 with initializer (0, 0)). Lighting, vertex colours,
// the pixel stage RGB = 2 D.rgb x base.rgb + S, alpha base.a x D.a, and the
// SRCALPHA/INVSRCALPHA, no-depth-write state are the shared alpha ones (CEM-17,
// which also records where the sph_t0 technique is declared). The time term
// reads the scene's effect clock exactly like the scrolling additive families.
namespace eawr::presentation::godot_backend::legacy::mesh_alpha_scroll {

inline constexpr std::string_view scroll_declarations =
    "uniform vec2 eawr_uv_scroll_rate = vec2(0.0);\nuniform float eawr_effect_time = 0.0;\n";
inline constexpr std::string_view scroll_vertex_line = "    UV += eawr_effect_time * eawr_uv_scroll_rate;\n";

[[nodiscard]] inline std::string with_scroll(std::string source) {
    const auto insert_before = [&source](const std::string_view anchor, const std::string_view text) {
        const auto at = source.find(anchor);
        if (at != std::string::npos) source.insert(at, text);
    };
    insert_before("uniform vec3 eawr_emissive", scroll_declarations);
    insert_before("    eawr_vertex_specular = clamp(", scroll_vertex_line);
    return source;
}

[[nodiscard]] inline std::string_view shader(const RenderPass pass) noexcept {
    static const std::string source = with_scroll(dx8_mesh::compose(dx8_mesh::alpha_modes, false,
        dx8_mesh::diffuse_rgba, dx8_mesh::material_alpha_line, dx8_mesh::batch_mesh_alpha_pixel));
    return pass == RenderPass::transparent ? std::string_view(source) : std::string_view{};
}

[[nodiscard]] inline std::optional<std::string> binding_problem(const MaterialDescription& material) {
    if (auto problem = dx8_mesh::colour_problem(material)) return problem;
    if (auto problem = bindings::vector_problem(material, "UVScrollRate")) return problem;
    return bindings::scalar_problem(material, "eawr_effect_time");
}

[[nodiscard]] inline std::vector<Uniform> uniforms(const MaterialDescription& material) {
    std::vector<Uniform> result = dx8_mesh::alpha_uniforms(material);
    const auto rate = bindings::vector_or(material, "UVScrollRate", mesh_additive::default_uv_scroll_rate);
    result.push_back({"eawr_uv_scroll_rate", std::array<float, 2>{rate[0], rate[1]}});
    result.push_back({"eawr_effect_time", bindings::scalar_or(material, "eawr_effect_time", 0.0F)});
    return result;
}

inline constexpr Family family{
    .program = "MeshAlphaScroll.fx",
    .technique = "sph_t0",
    .pass_name = "sph_t0_p0",
    .opaque = false,
    .transparent = true,
    .receives_shadows = true,
    .shader = &shader,
    .binding_problem = &binding_problem,
    .uniforms = &uniforms,
};

} // namespace eawr::presentation::godot_backend::legacy::mesh_alpha_scroll
