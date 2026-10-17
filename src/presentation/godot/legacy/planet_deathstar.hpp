#pragma once

#include "bindings.hpp"
#include "family.hpp"

namespace eawr::presentation::godot_backend::legacy::planet_deathstar {

// The mod's highest-detail family: two normal maps, separate cloud UVs,
// spherical fill/reflection, atmosphere, night glow and a filmic output.
// Scene ownership and engine lighting are supplied by the callers.
inline constexpr std::string_view source = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, depth_draw_opaque, depth_test_default, cull_back;
uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform sampler2D NormalTexture : filter_linear_mipmap, repeat_enable;
uniform sampler2D CloudTexture : filter_linear_mipmap, repeat_enable;
uniform sampler2D CloudNormalTexture : filter_linear_mipmap, repeat_enable;
uniform vec4 eawr_atmosphere = vec4(0.0, 0.0, 0.0, 1.0);
uniform float eawr_atmosphere_power = 4.5;
uniform vec3 eawr_city_color = vec3(1.0);
uniform float eawr_cloud_scroll_rate = 0.0025;
uniform float eawr_effect_time = 0.0;
uniform float eawr_gamma = 2.2;
uniform float eawr_pi = 3.1415;
uniform float eawr_light_factor = 20.0;
uniform float eawr_ambient_factor = 3.0;
uniform float eawr_fill_factor = 5.0;
uniform vec3 eawr_luminance_weights = vec3(0.299, 0.587, 0.114);
uniform float eawr_saturation_factor = 0.3;
uniform mat4 eawr_sph_fill_r;
uniform mat4 eawr_sph_fill_g;
uniform mat4 eawr_sph_fill_b;
uniform vec3 eawr_light_ambient = vec3(0.2);
uniform vec3 eawr_light_direction = vec3(0.0, 1.0, 0.0);
uniform vec3 eawr_light_diffuse = vec3(2.0, 1.88, 1.72);
uniform vec4 eawr_light_scale = vec4(1.0);
instance uniform vec3 eawr_unit_light_scale = vec3(1.0);
varying vec3 eawr_position_world;
varying vec3 eawr_normal_world;
varying vec3 eawr_diffuse_fill;
varying vec3 eawr_reflection_fill;
varying vec3 eawr_tangent_light;
varying vec3 eawr_tangent_half;
varying vec2 eawr_cloud_uv;

// CEM-12E: the renderer replaces this marker for Compatibility transfer.
vec3 eawr_stored_albedo(vec3 stored_rgb) {
    return stored_rgb;
}

vec3 eawr_fill(vec3 direction) {
    vec4 probe = vec4(direction, 1.0);
    vec3 irradiance = vec3(dot(probe, eawr_sph_fill_r * probe),
        dot(probe, eawr_sph_fill_g * probe), dot(probe, eawr_sph_fill_b * probe));
    // The shader's logarithm ignores the sign of its input.
    return pow(abs(irradiance), vec3(eawr_gamma));
}

void vertex() {
    mat3 model_basis = mat3(MODEL_MATRIX);
    vec3 normal_model = normalize(NORMAL);
    vec3 tangent_model = normalize(TANGENT);
    vec3 binormal_model = CUSTOM0.x * tangent_model + CUSTOM0.y * normal_model
        + CUSTOM0.z * cross(normal_model, tangent_model);
    eawr_position_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    eawr_normal_world = normalize(model_basis * normal_model);
    vec3 frame_normal = model_basis * normal_model;
    vec3 frame_tangent = model_basis * tangent_model;
    vec3 frame_binormal = model_basis * binormal_model;
    vec3 toward_light = eawr_light_direction;
    vec3 half_vector = normalize(normalize(CAMERA_POSITION_WORLD - eawr_position_world) + toward_light);
    eawr_tangent_light = normalize(vec3(dot(toward_light, frame_tangent),
        dot(toward_light, frame_binormal), dot(toward_light, frame_normal)));
    eawr_tangent_half = normalize(vec3(dot(half_vector, frame_tangent),
        dot(half_vector, frame_binormal), dot(half_vector, frame_normal)));
    // CEM-12A: the precomputed inverse-transpose basis supplies the view ray.
    vec3 away_eye_model = normalize(transpose(MODEL_NORMAL_MATRIX)
        * (eawr_position_world - CAMERA_POSITION_WORLD));
    // The effect reflects its model-space view vector about the world normal.
    vec3 reflection_probe = reflect(away_eye_model, eawr_normal_world);
    vec3 reflection_fill = eawr_fill(reflection_probe);
    float luminance = dot(reflection_fill, eawr_luminance_weights);
    eawr_diffuse_fill = eawr_fill(eawr_normal_world) * eawr_fill_factor
        + eawr_ambient_factor * eawr_light_ambient;
    eawr_reflection_fill = mix(reflection_fill, vec3(luminance), eawr_saturation_factor)
        * eawr_fill_factor + eawr_light_ambient;
    eawr_cloud_uv = UV + vec2(eawr_effect_time * eawr_cloud_scroll_rate, 0.0);
}

void fragment() {
    vec3 toward_eye = normalize(CAMERA_POSITION_WORLD - eawr_position_world);
    float view_edge = 1.0 - clamp(dot(eawr_normal_world, toward_eye), 0.0, 1.0);
    float view_edge_fifth = view_edge * view_edge * view_edge * view_edge * view_edge;
    float atmosphere_rim = pow(view_edge, eawr_atmosphere_power);
    // CEM-12B: authored surface roughness and Fresnel response.
    vec4 normal_texel = texture(NormalTexture, UV);
    vec3 surface_normal = 2.0 * normal_texel.rgb - 1.0;
    float roughness = (1.0 - normal_texel.a) * 0.5 + 0.25;
    float surface_fresnel = clamp(mix(roughness * 0.56 + 0.04, 1.0, view_edge_fifth), 0.0, 1.0);
    float sun_diffuse = clamp(dot(surface_normal, eawr_tangent_light), 0.0, 1.0);
    sun_diffuse *= sun_diffuse;
    float sun_specular = pow(clamp(dot(surface_normal, eawr_tangent_half), 0.0, 1.0), 12.0);
    vec3 scaled_sun = eawr_light_factor * eawr_light_diffuse;
    vec3 diffuse = sun_diffuse * scaled_sun + eawr_diffuse_fill;
    vec3 specular = sun_specular * scaled_sun + eawr_reflection_fill;
    vec3 surface_light = (1.0 - surface_fresnel) * diffuse * (1.0 - roughness)
        + surface_fresnel * specular;
    vec3 atmosphere = pow(abs(eawr_atmosphere.rgb), vec3(eawr_gamma));
    vec3 surface = pow(abs(texture(BaseTexture, UV).rgb), vec3(eawr_gamma))
        + atmosphere * sun_diffuse * atmosphere_rim * eawr_atmosphere.a;
    vec3 surface_radiance = surface_light * surface;

    // CEM-12C: independent cloud normal, Fresnel and opacity.
    vec3 cloud_normal = 2.0 * texture(CloudNormalTexture, eawr_cloud_uv).rgb - 1.0;
    float cloud_diffuse = clamp(dot(cloud_normal, eawr_tangent_light), 0.0, 1.0);
    cloud_diffuse *= cloud_diffuse;
    float cloud_specular = pow(clamp(dot(cloud_normal, eawr_tangent_half), 0.0, 1.0), 12.0);
    float cloud_fresnel = clamp(view_edge_fifth * 0.792 + 0.208, 0.0, 1.0);
    vec3 cloud_light = (1.0 - cloud_fresnel) * (cloud_diffuse * scaled_sun + eawr_diffuse_fill) * 0.7
        + cloud_fresnel * (cloud_specular * scaled_sun + eawr_reflection_fill);
    vec4 cloud_texel = texture(CloudTexture, eawr_cloud_uv);
    vec3 cloud_radiance = cloud_light * pow(abs(cloud_texel.rgb), vec3(eawr_gamma));
    vec3 radiance = mix(surface_radiance, cloud_radiance, cloud_texel.a)
        * eawr_light_scale.rgb * eawr_unit_light_scale / eawr_pi;
    // CEM-12D: city and separate atmosphere contributions.
    float night = clamp(dot(eawr_normal_world, -eawr_light_direction), 0.0, 1.0);
    radiance += (1.0 - cloud_texel.a) * (1.0 - surface_fresnel) * night * night * eawr_city_color;
    radiance += 0.5 * atmosphere * max(sun_diffuse, 0.001) * atmosphere_rim
        * pow(abs(eawr_light_diffuse), vec3(eawr_gamma)) * eawr_light_factor;
    // CEM-12E: stored-value filmic output.
    vec3 film = max(radiance - vec3(0.004), vec3(0.0));
    vec3 stored_rgb = film * (6.2 * film + 0.5) / (film * (6.2 * film + 1.7) + 0.06);
    ALBEDO = eawr_stored_albedo(stored_rgb);
}
)GODOT";

inline constexpr TextureBinding textures[]{
    {"NormalTexture", TexturePlaceholder::flat_normal}, {"CloudTexture", TexturePlaceholder::black},
    {"CloudNormalTexture", TexturePlaceholder::flat_normal}};

[[nodiscard]] inline std::string_view shader(const RenderPass pass) noexcept {
    return pass == RenderPass::opaque ? source : std::string_view{};
}

[[nodiscard]] inline std::optional<std::string> binding_problem(const MaterialDescription& material) {
    for (const std::string_view name : {std::string_view("Atmosphere"), std::string_view("CityColor"),
             std::string_view("luminance_weights")}) {
        if (auto problem = bindings::vector_problem(material, name)) return problem;
    }
    for (const std::string_view name : {std::string_view("AtmospherePower"), std::string_view("CloudScrollRate"),
             std::string_view("gamma"), std::string_view("pi"), std::string_view("light_factor"),
             std::string_view("ambient_factor"), std::string_view("fill_factor"),
             std::string_view("saturation_factor"), std::string_view("eawr_effect_time")}) {
        if (auto problem = bindings::scalar_problem(material, name)) return problem;
    }
    for (const TextureBinding& texture : textures) {
        if (bindings::count(material, texture.name) != 1) {
            return "requires exactly one " + std::string(texture.name) + " texture";
        }
        if (!std::holds_alternative<std::string>(bindings::find(material, texture.name)->value)) {
            return "binds " + std::string(texture.name) + " as a value that is not a texture";
        }
    }
    if (bindings::count(material, "BaseTexture") > 1) return "binds BaseTexture more than once";
    return std::nullopt;
}

[[nodiscard]] inline std::vector<Uniform> uniforms(const MaterialDescription& material) {
    const auto rgb = [&material](const std::string_view name, const std::array<float, 4>& fallback) {
        const auto value = bindings::vector_or(material, name, fallback);
        return std::array<float, 3>{value[0], value[1], value[2]};
    };
    return {
        {"eawr_atmosphere", bindings::vector_or(material, "Atmosphere", {0.0F, 0.0F, 0.0F, 1.0F})},
        {"eawr_atmosphere_power", bindings::scalar_or(material, "AtmospherePower", 4.5F)},
        {"eawr_city_color", rgb("CityColor", {1.0F, 1.0F, 1.0F, 0.0F})},
        {"eawr_cloud_scroll_rate", bindings::scalar_or(material, "CloudScrollRate", 0.0025F)},
        {"eawr_effect_time", bindings::scalar_or(material, "eawr_effect_time", 0.0F)},
        {"eawr_gamma", bindings::scalar_or(material, "gamma", 2.2F)},
        {"eawr_pi", bindings::scalar_or(material, "pi", 3.1415F)},
        {"eawr_light_factor", bindings::scalar_or(material, "light_factor", 20.0F)},
        {"eawr_ambient_factor", bindings::scalar_or(material, "ambient_factor", 3.0F)},
        {"eawr_fill_factor", bindings::scalar_or(material, "fill_factor", 5.0F)},
        {"eawr_luminance_weights", rgb("luminance_weights", {0.299F, 0.587F, 0.114F, 0.0F})},
        {"eawr_saturation_factor", bindings::scalar_or(material, "saturation_factor", 0.3F)},
    };
}

inline constexpr Family family{
    .program = "Planet_Deathstar.fx",
    .technique = "sph_t3",
    .pass_name = "sph_t3_p0",
    .opaque = true,
    .transparent = false,
    .receives_shadows = true,
    .binding_textures = textures,
    .authored_binormals = true,
    .shader = &shader,
    .binding_problem = &binding_problem,
    .uniforms = &uniforms,
};

} // namespace eawr::presentation::godot_backend::legacy::planet_deathstar
