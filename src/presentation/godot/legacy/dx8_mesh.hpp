#pragma once

#include "bindings.hpp"
#include "family.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// BatchMeshGloss.fx, BatchMeshAlpha.fx and MeshAlphaGloss.fx sph_t0/sph_t0_p0
// (#200), the DX8 technique retail selects at the Highest shader detail
// (docs/graphics-features.md) beside the fixed-function sph_t1 rows. Facts
// from the FoC shader source:
//
// Vertex stage (sph_vs_main, the same in all three). With the world normal N
// (the world upper 3x3 times the normal, normalized) and the world position P:
// D = Diffuse.rgb x SPH_LIGHT_ALL irradiance(N) x LIGHT_SCALE.rgb + Emissive,
// alpha LIGHT_SCALE.a (BatchMeshGloss, whose Diffuse is a float3) or
// Diffuse.a x LIGHT_SCALE.a (the two alpha programs); S = Specular x
// max(N.H, 0)^16 x light-0 specular with H = normalize(normalize(eye - P) +
// L). Light-0 specular already contains environment specular x 2 x sun
// intensity; do not apply that scale again here. Shininess is authored but
// unread. Both are vs_1_1 colour outputs and so
// clamped to [0, 1].
//
// Pixel stages (ps_1_1):
//   BatchMeshGloss: RGB = 2 D.rgb x base.rgb + S x base.a, alpha D.a, then
//     RGB x the fog-of-war texel. ZWriteEnable TRUE, blend SRCALPHA /
//     INVSRCALPHA on D.a, which is LIGHT_SCALE.a = 1 in the opaque pass, so
//     the pass is drawn opaque.
//   BatchMeshAlpha: RGB = 2 D.rgb x base.rgb + S, alpha base.a x D.a, then
//     RGB x the fog-of-war texel. ZWriteEnable FALSE, SRCALPHA /
//     INVSRCALPHA, transparent phase.
//   MeshAlphaGloss: RGB = 2 D.rgb x base.rgb + S x gloss.r, alpha
//     base.a x D.a, where gloss is the GlossTexture sample at the same UV. No
//     fog-of-war stage. ZWriteEnable FALSE, SRCALPHA / INVSRCALPHA,
//     transparent phase. Colorization is declared but unread.
// The written colour saturates at the 8-bit target before it blends.
//
// Not reproduced: the BatchMesh programs' per-vertex distance fade
// (DISTANCE_FADE_VALS, an engine input the viewer does not have, so D.a keeps
// no fade term) and the engine distance fog, which every legacy adapter
// disables. The fog-of-war multiply is the renderer's derived fog-stub-v1
// stage, which it applies to the two BatchMesh passes in every fog mode.
//
// Colour policy (docs/rendering.md): the arithmetic runs on stored texel and
// constant values, as the retail ps_1_1 does, and ALBEDO is the stored result.
namespace eawr::presentation::godot_backend::legacy::dx8_mesh {

inline constexpr std::string_view opaque_modes = "depth_draw_opaque, depth_test_default, cull_back;\n";
inline constexpr std::string_view alpha_modes = "blend_mix, depth_draw_never, depth_test_default, cull_back;\n";

inline constexpr std::string_view gloss_sampler =
    "uniform sampler2D GlossTexture : filter_linear_mipmap, repeat_enable;\n";
inline constexpr std::string_view diffuse_rgb = "uniform vec3 eawr_diffuse = vec3(1.0);\n";
inline constexpr std::string_view diffuse_rgba = "uniform vec4 eawr_diffuse = vec4(1.0);\n";

inline constexpr std::string_view declarations = R"GODOT(uniform vec3 eawr_emissive = vec3(0.0);
uniform vec3 eawr_specular = vec3(1.0);
uniform mat4 eawr_sph_r;
uniform mat4 eawr_sph_g;
uniform mat4 eawr_sph_b;
uniform vec4 eawr_light_scale = vec4(1.0);
// WR-14/37: per-object preview tint and arrival fade share the light-scale input.
instance uniform vec3 eawr_unit_light_scale = vec3(1.0);
uniform vec3 eawr_light_direction = vec3(0.0, 1.0, 0.0);
uniform vec3 eawr_light_specular = vec3(2.0, 1.88, 1.72);
varying vec4 eawr_vertex_diffuse;
varying vec3 eawr_vertex_specular;

vec3 eawr_stored_albedo(vec3 stored_rgb) {
    return stored_rgb;
}

void vertex() {
    vec3 position_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    vec3 normal_world = normalize(mat3(MODEL_MATRIX) * NORMAL);
    vec4 normal_h = vec4(normal_world, 1.0);
    vec3 irradiance = vec3(
        dot(normal_h, eawr_sph_r * normal_h),
        dot(normal_h, eawr_sph_g * normal_h),
        dot(normal_h, eawr_sph_b * normal_h));
    vec3 half_direction = normalize(normalize(CAMERA_POSITION_WORLD - position_world) + eawr_light_direction);
    float highlight = pow(max(dot(normal_world, half_direction), 0.0), 16.0);
    eawr_vertex_diffuse = clamp(vec4(eawr_diffuse.rgb * irradiance * eawr_light_scale.rgb * eawr_unit_light_scale + eawr_emissive,
)GODOT";

// The vertex alpha line, then the pixel stage.
inline constexpr std::string_view light_scale_alpha_line = "        eawr_light_scale.a), 0.0, 1.0);\n";
inline constexpr std::string_view material_alpha_line = "        eawr_diffuse.a * eawr_light_scale.a), 0.0, 1.0);\n";

inline constexpr std::string_view vertex_tail = R"GODOT(    eawr_vertex_specular = clamp(eawr_specular * highlight * eawr_light_specular, 0.0, 1.0);
}

void fragment() {
    vec4 base = texture(BaseTexture, UV);
)GODOT";

inline constexpr std::string_view batch_mesh_gloss_pixel = R"GODOT(    vec3 stored_rgb = clamp(2.0 * eawr_vertex_diffuse.rgb * base.rgb + eawr_vertex_specular * base.a, 0.0, 1.0);
    ALBEDO = eawr_stored_albedo(stored_rgb);
}
)GODOT";

inline constexpr std::string_view batch_mesh_alpha_pixel = R"GODOT(    vec3 stored_rgb = clamp(2.0 * eawr_vertex_diffuse.rgb * base.rgb + eawr_vertex_specular, 0.0, 1.0);
    ALBEDO = eawr_stored_albedo(stored_rgb);
    ALPHA = base.a * eawr_vertex_diffuse.a;
}
)GODOT";

inline constexpr std::string_view mesh_alpha_gloss_pixel = R"GODOT(    float gloss = texture(GlossTexture, UV).r;
    vec3 stored_rgb = clamp(2.0 * eawr_vertex_diffuse.rgb * base.rgb + eawr_vertex_specular * gloss, 0.0, 1.0);
    ALBEDO = eawr_stored_albedo(stored_rgb);
    ALPHA = base.a * eawr_vertex_diffuse.a;
}
)GODOT";

[[nodiscard]] inline std::string compose(const std::string_view modes, const bool gloss_texture,
    const std::string_view diffuse, const std::string_view alpha, const std::string_view pixel) {
    return "\nshader_type spatial;\nrender_mode unshaded, fog_disabled, " + std::string(modes)
        + "\nuniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;\n"
        + (gloss_texture ? std::string(gloss_sampler) : std::string()) + std::string(diffuse)
        + std::string(declarations) + std::string(alpha) + std::string(vertex_tail) + std::string(pixel);
}

[[nodiscard]] inline std::string_view batch_mesh_gloss_shader(const RenderPass pass) noexcept {
    static const std::string source
        = compose(opaque_modes, false, diffuse_rgb, light_scale_alpha_line, batch_mesh_gloss_pixel);
    return pass == RenderPass::opaque ? std::string_view(source) : std::string_view{};
}

[[nodiscard]] inline std::string_view batch_mesh_alpha_shader(const RenderPass pass) noexcept {
    static const std::string source
        = compose(alpha_modes, false, diffuse_rgba, material_alpha_line, batch_mesh_alpha_pixel);
    return pass == RenderPass::transparent ? std::string_view(source) : std::string_view{};
}

[[nodiscard]] inline std::string_view mesh_alpha_gloss_shader(const RenderPass pass) noexcept {
    static const std::string source
        = compose(alpha_modes, true, diffuse_rgba, material_alpha_line, mesh_alpha_gloss_pixel);
    return pass == RenderPass::transparent ? std::string_view(source) : std::string_view{};
}

inline constexpr std::array<float, 4> default_emissive{0.0F, 0.0F, 0.0F, 0.0F};
inline constexpr std::array<float, 4> default_diffuse{1.0F, 1.0F, 1.0F, 1.0F};
inline constexpr std::array<float, 4> default_specular{1.0F, 1.0F, 1.0F, 0.0F};

inline constexpr TextureBinding gloss_textures[]{{"GlossTexture", TexturePlaceholder::black}};

[[nodiscard]] inline std::optional<std::string> colour_problem(const MaterialDescription& material) {
    for (const std::string_view name :
        {std::string_view("Emissive"), std::string_view("Diffuse"), std::string_view("Specular")}) {
        if (auto problem = bindings::vector_problem(material, name)) return problem;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string> gloss_problem(const MaterialDescription& material) {
    if (auto problem = colour_problem(material)) return problem;
    const std::size_t gloss_count = bindings::count(material, "GlossTexture");
    if (gloss_count == 0) {
        return std::string("binds no GlossTexture; an unbound gloss sampler's result is not established");
    }
    if (gloss_count > 1 || bindings::count(material, "BaseTexture") > 1) {
        return std::string("binds GlossTexture or BaseTexture more than once");
    }
    if (!std::holds_alternative<std::string>(bindings::find(material, "GlossTexture")->value)) {
        return std::string("binds GlossTexture as a value that is not a texture");
    }
    return std::nullopt;
}

[[nodiscard]] inline std::array<float, 3> rgb(
    const MaterialDescription& material, const std::string_view name, const std::array<float, 4>& fallback) {
    const auto value = bindings::vector_or(material, name, fallback);
    return {value[0], value[1], value[2]};
}

// BatchMeshGloss declares Diffuse as a float3: an authored fourth component
// never reaches the effect.
[[nodiscard]] inline std::vector<Uniform> gloss_uniforms(const MaterialDescription& material) {
    return {
        {"eawr_emissive", rgb(material, "Emissive", default_emissive)},
        {"eawr_diffuse", rgb(material, "Diffuse", default_diffuse)},
        {"eawr_specular", rgb(material, "Specular", default_specular)},
    };
}

[[nodiscard]] inline std::vector<Uniform> alpha_uniforms(const MaterialDescription& material) {
    return {
        {"eawr_emissive", rgb(material, "Emissive", default_emissive)},
        {"eawr_diffuse", bindings::vector_or(material, "Diffuse", default_diffuse)},
        {"eawr_specular", rgb(material, "Specular", default_specular)},
    };
}

// Engine-free reference on stored values. The vertex colours: D from the
// irradiance at the vertex, alpha as the program declares it; S from the
// highlight max(N.H, 0)^16. Both saturate like a vs_1_1 colour output.
struct VertexColours final {
    std::array<float, 4> diffuse{};
    std::array<float, 3> specular{};
};

[[nodiscard]] inline VertexColours reference_vertex(const std::array<float, 4>& diffuse,
    const std::array<float, 3>& emissive, const std::array<float, 3>& specular,
    const std::array<float, 3>& irradiance, const std::array<float, 4>& light_scale,
    const std::array<float, 3>& light_specular, const float n_dot_h, const bool material_alpha) noexcept {
    VertexColours result;
    const float highlight = std::pow(n_dot_h > 0.0F ? n_dot_h : 0.0F, 16.0F);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        result.diffuse[channel] = bindings::saturate(
            diffuse[channel] * irradiance[channel] * light_scale[channel] + emissive[channel]);
        result.specular[channel] = bindings::saturate(specular[channel] * highlight * light_specular[channel]);
    }
    result.diffuse[3] = bindings::saturate(material_alpha ? diffuse[3] * light_scale[3] : light_scale[3]);
    return result;
}

enum class Program : std::uint8_t { batch_mesh_gloss, batch_mesh_alpha, mesh_alpha_gloss };

struct Pixel final {
    std::array<float, 3> rgb{};
    float alpha{};
};

// The pixel stage before the fog-of-war multiply, given the base texel, the
// gloss texel's red channel (MeshAlphaGloss) and the vertex colours.
[[nodiscard]] inline Pixel reference_pixel(const Program program, const std::array<float, 4>& base,
    const float gloss_red, const VertexColours& vertex) noexcept {
    const float mask = program == Program::batch_mesh_gloss ? base[3]
        : program == Program::mesh_alpha_gloss ? gloss_red : 1.0F;
    Pixel result;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        result.rgb[channel] = bindings::saturate(
            2.0F * vertex.diffuse[channel] * base[channel] + vertex.specular[channel] * mask);
    }
    result.alpha = program == Program::batch_mesh_gloss ? vertex.diffuse[3] : base[3] * vertex.diffuse[3];
    return result;
}

inline constexpr Family batch_mesh_gloss_family{
    .program = "BatchMeshGloss.fx",
    .technique = "sph_t0",
    .pass_name = "sph_t0_p0",
    .opaque = true,
    .transparent = false,
    .receives_shadows = true,
    .shader = &batch_mesh_gloss_shader,
    .binding_problem = &colour_problem,
    .uniforms = &gloss_uniforms,
};

inline constexpr Family batch_mesh_alpha_family{
    .program = "BatchMeshAlpha.fx",
    .technique = "sph_t0",
    .pass_name = "sph_t0_p0",
    .opaque = false,
    .transparent = true,
    .receives_shadows = true,
    .shader = &batch_mesh_alpha_shader,
    .binding_problem = &colour_problem,
    .uniforms = &alpha_uniforms,
};

inline constexpr Family mesh_alpha_gloss_family{
    .program = "MeshAlphaGloss.fx",
    .technique = "sph_t0",
    .pass_name = "sph_t0_p0",
    .opaque = false,
    .transparent = true,
    .receives_shadows = true,
    .binding_textures = gloss_textures,
    .shader = &mesh_alpha_gloss_shader,
    .binding_problem = &gloss_problem,
    .uniforms = &alpha_uniforms,
};

} // namespace eawr::presentation::godot_backend::legacy::dx8_mesh
