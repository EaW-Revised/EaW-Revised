#pragma once

#include "bindings.hpp"
#include "family.hpp"

// CEM-10/11: compiled shield materials have explicit generic selectors.
// The existing ability-shell MeshShield.fx route remains independent.
namespace eawr::presentation::godot_backend::legacy::mesh_shield {

inline constexpr std::string_view source = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, blend_add, depth_draw_never, depth_test_default, cull_disabled;
uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform sampler2D WaveTexture : filter_linear_mipmap, repeat_enable;
uniform sampler2D DistortionTexture : filter_linear_mipmap, repeat_enable;
uniform vec4 eawr_color = vec4(1.0);
uniform float eawr_edge_brightness = 0.5;
uniform float eawr_base_uv_scale = 20.0;
uniform float eawr_wave_uv_scale = 1.0;
uniform float eawr_distort_uv_scale = 1.0;
uniform float eawr_base_scroll = 1.0;
uniform float eawr_wave_scroll = 1.0;
uniform float eawr_distort_scroll = 1.0;
uniform float eawr_effect_time = 0.0;
varying vec2 eawr_base_uv;
varying vec2 eawr_wave_uv;
varying vec2 eawr_distort_uv;
varying vec3 eawr_vertex_color;

vec3 eawr_stored_albedo(vec3 stored_rgb) {
    return stored_rgb;
}

void vertex() {
    eawr_base_uv = UV * eawr_base_uv_scale + vec2(0.0, eawr_effect_time * eawr_base_scroll);
    eawr_wave_uv = UV * eawr_wave_uv_scale + vec2(0.0, eawr_effect_time * eawr_wave_scroll);
    eawr_distort_uv = UV * eawr_distort_uv_scale + vec2(0.0, eawr_effect_time * eawr_distort_scroll);
    // Source model +Z is render +Y. The colour output saturates per vertex.
    eawr_vertex_color = clamp(COLOR.rgb * clamp(NORMAL.y + eawr_edge_brightness, 0.0, 1.0)
        * eawr_color.rgb, 0.0, 1.0);
}

void fragment() {
    // The authored program crosses the wave/distortion coordinates.
    vec2 distortion = texture(DistortionTexture, eawr_wave_uv).rg;
    vec3 energy = texture(BaseTexture, eawr_base_uv + 0.25 * distortion).rgb;
    vec3 wave = texture(WaveTexture, eawr_distort_uv).rgb;
    vec3 stored_rgb = energy * wave * eawr_vertex_color;
    ALBEDO = eawr_stored_albedo(stored_rgb);
}
)GODOT";

inline constexpr TextureBinding textures[]{
    {"WaveTexture", TexturePlaceholder::black}, {"DistortionTexture", TexturePlaceholder::black}};

[[nodiscard]] inline std::string_view shader(const RenderPass pass) noexcept {
    return pass == RenderPass::transparent ? source : std::string_view{};
}

[[nodiscard]] inline std::optional<std::string> binding_problem(const MaterialDescription& material) {
    if (auto problem = bindings::vector_problem(material, "Color")) return problem;
    for (const std::string_view name : {std::string_view("EdgeBrightness"), std::string_view("BaseUVScale"),
             std::string_view("WaveUVScale"), std::string_view("DistortUVScale"),
             std::string_view("BaseUVScrollRate"), std::string_view("WaveUVScrollRate"),
             std::string_view("DistortUVScrollRate"), std::string_view("eawr_effect_time")}) {
        if (auto problem = bindings::scalar_problem(material, name)) return problem;
    }
    for (const std::string_view name : {std::string_view("BaseTexture"), std::string_view("WaveTexture"),
             std::string_view("DistortionTexture")}) {
        if (bindings::count(material, name) != 1) return "requires exactly one " + std::string(name) + " texture";
        if (!std::holds_alternative<std::string>(bindings::find(material, name)->value)) {
            return "binds " + std::string(name) + " as a value that is not a texture";
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline std::vector<Uniform> uniforms(const MaterialDescription& material) {
    return {
        {"eawr_color", bindings::vector_or(material, "Color", {1.0F, 1.0F, 1.0F, 1.0F})},
        {"eawr_edge_brightness", bindings::scalar_or(material, "EdgeBrightness", 0.5F)},
        {"eawr_base_uv_scale", bindings::scalar_or(material, "BaseUVScale", 20.0F)},
        {"eawr_wave_uv_scale", bindings::scalar_or(material, "WaveUVScale", 1.0F)},
        {"eawr_distort_uv_scale", bindings::scalar_or(material, "DistortUVScale", 1.0F)},
        {"eawr_base_scroll", bindings::scalar_or(material, "BaseUVScrollRate", 1.0F)},
        {"eawr_wave_scroll", bindings::scalar_or(material, "WaveUVScrollRate", 1.0F)},
        {"eawr_distort_scroll", bindings::scalar_or(material, "DistortUVScrollRate", 1.0F)},
        {"eawr_effect_time", bindings::scalar_or(material, "eawr_effect_time", 0.0F)},
    };
}

[[nodiscard]] inline bool reads_effect_time(const MaterialDescription& material) noexcept {
    return bindings::scalar_or(material, "BaseUVScrollRate", 1.0F) != 0.0F
        || bindings::scalar_or(material, "WaveUVScrollRate", 1.0F) != 0.0F
        || bindings::scalar_or(material, "DistortUVScrollRate", 1.0F) != 0.0F;
}

inline constexpr Family family{
    .program = "MeshShield.fxo",
    .technique = "t0",
    .pass_name = "t0_p0",
    .opaque = false,
    .transparent = true,
    .receives_shadows = false,
    .binding_textures = textures,
    .shader = &shader,
    .binding_problem = &binding_problem,
    .uniforms = &uniforms,
};

inline constexpr std::string_view farseer_source = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, blend_add, depth_draw_never, depth_test_default, cull_back;
// Declared for the renderer's compile probe; this family samples no texture.
uniform sampler2D BaseTexture;
uniform vec4 eawr_atmosphere = vec4(0.0, 0.0, 0.0, 1.0);
uniform float eawr_atmosphere_power = 4.5;
// Engine-owned cloak tint; ordinary/DEFEND rim arithmetic keeps its default.
uniform float eawr_cloak_tint_enabled = 0.0;
instance uniform vec3 eawr_unit_light_scale = vec3(1.0);
varying float eawr_vertex_rim;

vec3 eawr_stored_albedo(vec3 stored_rgb) {
    return stored_rgb;
}

void vertex() {
    vec3 position_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    vec3 normal_world = normalize(mat3(MODEL_MATRIX) * NORMAL);
    vec3 toward_eye = normalize(CAMERA_POSITION_WORLD - position_world);
    eawr_vertex_rim = 2.0 * pow(abs(1.0 - dot(normal_world, toward_eye)), eawr_atmosphere_power)
        * eawr_atmosphere.a;
}

void fragment() {
    // Atmosphere alpha is used once in each stage, not just once overall.
    vec3 stored_rgb = clamp(eawr_vertex_rim * eawr_atmosphere.rgb * eawr_atmosphere.a, 0.0, 1.0);
    ALBEDO = eawr_stored_albedo(stored_rgb * mix(vec3(1.0), eawr_unit_light_scale, eawr_cloak_tint_enabled));
}
)GODOT";

[[nodiscard]] inline std::string_view farseer_shader(const RenderPass pass) noexcept {
    return pass == RenderPass::transparent ? farseer_source : std::string_view{};
}

[[nodiscard]] inline std::optional<std::string> farseer_problem(const MaterialDescription& material) {
    if (auto problem = bindings::vector_problem(material, "Atmosphere")) return problem;
    if (auto problem = bindings::scalar_problem(material, "AtmospherePower")) return problem;
    return bindings::scalar_problem(material, "eawr_cloak_tint_enabled");
}

[[nodiscard]] inline std::vector<Uniform> farseer_uniforms(const MaterialDescription& material) {
    return {
        {"eawr_atmosphere", bindings::vector_or(material, "Atmosphere", {0.0F, 0.0F, 0.0F, 1.0F})},
        {"eawr_atmosphere_power", bindings::scalar_or(material, "AtmospherePower", 4.5F)},
        {"eawr_cloak_tint_enabled", bindings::scalar_or(material, "eawr_cloak_tint_enabled", 0.0F)},
    };
}

inline constexpr Family farseer_family{
    .program = "MeshShieldFarseer.fxo",
    .technique = "sph_t3",
    .pass_name = "sph_t3_p0",
    .opaque = false,
    .transparent = true,
    .receives_shadows = false,
    .shader = &farseer_shader,
    .binding_problem = &farseer_problem,
    .uniforms = &farseer_uniforms,
};

} // namespace eawr::presentation::godot_backend::legacy::mesh_shield
