#pragma once

#include "bindings.hpp"
#include "family.hpp"

namespace eawr::presentation::godot_backend::legacy::galactic_closeup {

// CEM-14: independent body/cloud composition, using the existing scene light,
// authored tangent frame and scene-owned animation/effect clock.
inline constexpr std::string_view body_source = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, depth_draw_opaque, depth_test_default, cull_back;
uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform sampler2D NormalTexture : filter_linear_mipmap, repeat_enable;
uniform sampler2D CloudTexture : filter_linear_mipmap, repeat_enable;
uniform sampler2D CloudNormalTexture : filter_linear_mipmap, repeat_enable;
uniform vec3 eawr_emissive = vec3(0.0);
uniform vec3 eawr_diffuse = vec3(1.0);
uniform vec3 eawr_specular = vec3(1.0);
uniform vec4 eawr_atmosphere = vec4(0.0, 0.0, 0.0, 1.0);
uniform float eawr_atmosphere_power = 4.5;
uniform float eawr_night_visibility = 0.0;
uniform float eawr_cloud_visibility = 5.0;
uniform float eawr_cloud_scroll_rate = 0.0025;
uniform float eawr_effect_time = 0.0;
uniform mat4 eawr_sph_fill_r;
uniform mat4 eawr_sph_fill_g;
uniform mat4 eawr_sph_fill_b;
uniform vec4 eawr_light_scale = vec4(1.0);
instance uniform vec3 eawr_unit_light_scale = vec3(1.0);
uniform vec3 eawr_light_direction = vec3(0.0, 1.0, 0.0);
uniform vec3 eawr_light_diffuse = vec3(2.0, 1.88, 1.72);
uniform vec3 eawr_light_specular = vec3(2.0, 1.88, 1.72);
varying vec3 eawr_fill;
varying vec3 eawr_tangent_light;
varying vec3 eawr_tangent_half;
varying vec2 eawr_cloud_uv;
varying float eawr_night;
varying float eawr_rim;

vec3 eawr_stored_albedo(vec3 stored_rgb) {
    return stored_rgb;
}

void vertex() {
    mat3 basis = mat3(MODEL_MATRIX);
    vec3 n = normalize(NORMAL);
    vec3 t = normalize(TANGENT);
    vec3 b = CUSTOM0.x * t + CUSTOM0.y * n + CUSTOM0.z * cross(n, t);
    vec3 world_position = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    vec3 world_normal = normalize(basis * n);
    vec3 toward_eye = normalize(CAMERA_POSITION_WORLD - world_position);
    vec3 half_vector = normalize(toward_eye + eawr_light_direction);
    vec3 frame_t = basis * t;
    vec3 frame_b = basis * b;
    vec3 frame_n = basis * n;
    eawr_tangent_light = normalize(vec3(dot(eawr_light_direction, frame_t),
        dot(eawr_light_direction, frame_b), dot(eawr_light_direction, frame_n)));
    eawr_tangent_half = normalize(vec3(dot(half_vector, frame_t),
        dot(half_vector, frame_b), dot(half_vector, frame_n)));
    vec4 probe = vec4(world_normal, 1.0);
    vec3 sh = vec3(dot(probe, eawr_sph_fill_r * probe),
        dot(probe, eawr_sph_fill_g * probe), dot(probe, eawr_sph_fill_b * probe));
    eawr_fill = clamp(eawr_diffuse * sh * eawr_light_scale.rgb * eawr_unit_light_scale
        + eawr_emissive, 0.0, 1.0);
    eawr_night = clamp(dot(world_normal, -eawr_light_direction), 0.0, 1.0);
    eawr_rim = 2.0 * pow(abs(1.0 - dot(world_normal, toward_eye)), eawr_atmosphere_power)
        * eawr_atmosphere.a;
    eawr_cloud_uv = UV + vec2(eawr_effect_time * eawr_cloud_scroll_rate, 0.0);
}

void fragment() {
    float day = 1.0 - eawr_night;
    vec4 normal_texel = texture(NormalTexture, UV);
    vec3 surface_normal = normal_texel.rgb * 2.0 - 1.0;
    vec3 surface_light = clamp(dot(surface_normal, eawr_tangent_light), 0.0, 1.0)
        * eawr_diffuse * eawr_light_diffuse * eawr_light_scale.rgb * eawr_unit_light_scale + eawr_fill;
    vec3 surface_colour = texture(BaseTexture, UV).rgb + eawr_atmosphere.rgb * day * eawr_rim;
    vec3 body = 2.0 * surface_light * surface_colour * (day + eawr_night_visibility * eawr_night);
    body += pow(clamp(dot(surface_normal, eawr_tangent_half), 0.0, 1.0), 16.0)
        * eawr_specular * eawr_light_specular * normal_texel.a;
    vec4 cloud = texture(CloudTexture, eawr_cloud_uv);
    vec3 cloud_normal = texture(CloudNormalTexture, eawr_cloud_uv).rgb * 2.0 - 1.0;
    vec3 cloud_light = clamp(dot(cloud_normal, eawr_tangent_light), 0.0, 1.0)
        * eawr_light_diffuse * eawr_light_scale.rgb * eawr_unit_light_scale;
    vec3 cloud_colour = 2.0 * (cloud.rgb * cloud_light + eawr_fill) * day * eawr_cloud_visibility;
    // CEM-14: authored visibility scales the colour and the blend independently.
    ALBEDO = eawr_stored_albedo(mix(body, cloud_colour, day * cloud.a * eawr_cloud_visibility));
}
)GODOT";

// CEM-15: the city-light shell has its own cloud mask and night-side weight.
// The base coordinates do not scroll and no texture alpha attenuates the glow.
inline constexpr std::string_view lights_source = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, blend_add, depth_draw_never, depth_test_default, cull_back;
uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform sampler2D CloudTexture : filter_linear_mipmap, repeat_enable;
uniform vec3 eawr_light_colour = vec3(1.0);
uniform float eawr_cloud_visibility = 5.0;
uniform float eawr_cloud_scroll_rate = 0.0025;
uniform float eawr_effect_time = 0.0;
uniform vec3 eawr_light_direction = vec3(0.0, 1.0, 0.0);
varying vec2 eawr_cloud_uv;
varying float eawr_night;
vec3 eawr_stored_albedo(vec3 stored_rgb) {
    return stored_rgb;
}
void vertex() {
    vec3 world_normal = normalize(mat3(MODEL_MATRIX) * NORMAL);
    eawr_night = clamp(dot(world_normal, -eawr_light_direction), 0.0, 1.0);
    eawr_cloud_uv = UV + vec2(eawr_effect_time * eawr_cloud_scroll_rate, 0.0);
}
void fragment() {
    float cloud_mask = 1.0 - min(texture(CloudTexture, eawr_cloud_uv).a * eawr_cloud_visibility, 1.0);
    ALBEDO = eawr_stored_albedo(texture(BaseTexture, UV).rgb * eawr_light_colour * eawr_night * cloud_mask);
    ALPHA = 1.0;
}
)GODOT";

inline constexpr TextureBinding body_textures[]{
    {"NormalTexture", TexturePlaceholder::flat_normal}, {"CloudTexture", TexturePlaceholder::black},
    {"CloudNormalTexture", TexturePlaceholder::flat_normal}};
inline constexpr TextureBinding light_textures[]{{"CloudTexture", TexturePlaceholder::black}};

[[nodiscard]] inline std::optional<std::string> texture_problem(const MaterialDescription& material,
    const std::span<const TextureBinding> textures) {
    for (const TextureBinding& texture : textures) {
        if (bindings::count(material, texture.name) != 1)
            return "requires exactly one " + std::string(texture.name) + " texture";
        if (!std::holds_alternative<std::string>(bindings::find(material, texture.name)->value))
            return "binds " + std::string(texture.name) + " as a value that is not a texture";
    }
    if (bindings::count(material, "BaseTexture") > 1) return "binds BaseTexture more than once";
    return std::nullopt;
}
[[nodiscard]] inline std::optional<std::string> clock_problem(const MaterialDescription& material) {
    for (const std::string_view name : {"CloudVisibility", "CloudScrollRate", "eawr_effect_time"})
        if (auto problem = bindings::scalar_problem(material, name)) return problem;
    return std::nullopt;
}
[[nodiscard]] inline std::optional<std::string> body_problem(const MaterialDescription& material) {
    for (const std::string_view name : {"Emissive", "Diffuse", "Specular", "Atmosphere", "CityColor"})
        if (auto problem = bindings::vector_problem(material, name)) return problem;
    for (const std::string_view name : {"AtmospherePower", "BaseTexNightVisibility"})
        if (auto problem = bindings::scalar_problem(material, name)) return problem;
    if (auto problem = clock_problem(material)) return problem;
    return texture_problem(material, body_textures);
}
[[nodiscard]] inline std::optional<std::string> lights_problem(const MaterialDescription& material) {
    for (const std::string_view name : {"LightColor", "UVScrollRate"})
        if (auto problem = bindings::vector_problem(material, name)) return problem;
    if (auto problem = clock_problem(material)) return problem;
    return texture_problem(material, light_textures);
}
[[nodiscard]] inline std::array<float, 3> rgb(const MaterialDescription& material,
    const std::string_view name, const std::array<float, 4>& fallback) {
    const auto value = bindings::vector_or(material, name, fallback);
    return {value[0], value[1], value[2]};
}
[[nodiscard]] inline std::vector<Uniform> clock_uniforms(const MaterialDescription& material) {
    return {{"eawr_cloud_visibility", bindings::scalar_or(material, "CloudVisibility", 5.0F)},
        {"eawr_cloud_scroll_rate", bindings::scalar_or(material, "CloudScrollRate", 0.0025F)},
        {"eawr_effect_time", bindings::scalar_or(material, "eawr_effect_time", 0.0F)}};
}
[[nodiscard]] inline std::vector<Uniform> body_uniforms(const MaterialDescription& material) {
    auto result = clock_uniforms(material);
    result.push_back({"eawr_emissive", rgb(material, "Emissive", {0.0F, 0.0F, 0.0F, 0.0F})});
    result.push_back({"eawr_diffuse", rgb(material, "Diffuse", {1.0F, 1.0F, 1.0F, 0.0F})});
    result.push_back({"eawr_specular", rgb(material, "Specular", {1.0F, 1.0F, 1.0F, 0.0F})});
    result.push_back({"eawr_atmosphere", bindings::vector_or(material, "Atmosphere", {0.0F, 0.0F, 0.0F, 1.0F})});
    result.push_back({"eawr_atmosphere_power", bindings::scalar_or(material, "AtmospherePower", 4.5F)});
    result.push_back({"eawr_night_visibility", bindings::scalar_or(material, "BaseTexNightVisibility", 0.0F)});
    return result;
}
[[nodiscard]] inline std::vector<Uniform> lights_uniforms(const MaterialDescription& material) {
    auto result = clock_uniforms(material);
    result.push_back({"eawr_light_colour", rgb(material, "LightColor", {1.0F, 1.0F, 1.0F, 0.0F})});
    return result;
}
[[nodiscard]] inline std::string_view body_shader(const RenderPass pass) noexcept {
    return pass == RenderPass::opaque ? body_source : std::string_view{};
}
[[nodiscard]] inline std::string_view lights_shader(const RenderPass pass) noexcept {
    return pass == RenderPass::transparent ? lights_source : std::string_view{};
}
inline constexpr Family body_family{
    .program = "Planet_Shim_Closeup.fx", .technique = "sph_t3", .pass_name = "sph_t3_p0",
    .opaque = true, .transparent = false, .receives_shadows = true,
    .binding_textures = body_textures, .authored_binormals = true,
    .shader = &body_shader, .binding_problem = &body_problem, .uniforms = &body_uniforms,
};
inline constexpr Family lights_family{
    .program = "MeshAdditive_CityLights_Closeup.fx", .technique = "t0", .pass_name = "t0_p0",
    .opaque = false, .transparent = true, .receives_shadows = false,
    .binding_textures = light_textures,
    .shader = &lights_shader, .binding_problem = &lights_problem, .uniforms = &lights_uniforms,
};
} // namespace eawr::presentation::godot_backend::legacy::galactic_closeup
