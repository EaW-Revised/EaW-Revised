#pragma once

#include "bump_colorize.hpp"

// CEM-07: a separate specular/glow map family. Shared tangent-frame and
// lighting inputs keep the existing bump family unchanged.
namespace eawr::presentation::godot_backend::legacy::mesh_bump_spec_glow_colorize {

inline constexpr std::string_view base_sampler =
    "uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;";
inline constexpr std::string_view vertex_emissive = " + eawr_emissive";
inline constexpr std::size_t sampler_offset = bump_colorize::shader_head.find(base_sampler);
inline constexpr std::size_t emissive_offset = bump_colorize::shader_tail.find(vertex_emissive);
inline constexpr std::size_t fragment_offset = bump_colorize::shader_tail.find("void fragment()");
static_assert(sampler_offset != std::string_view::npos && fragment_offset != std::string_view::npos
    && emissive_offset < fragment_offset);
static_assert(bump_colorize::shader_head.find(base_sampler, sampler_offset + 1) == std::string_view::npos);
static_assert(bump_colorize::shader_tail.find(vertex_emissive, emissive_offset + 1) == std::string_view::npos);

inline constexpr std::string_view fragment = R"GODOT(void fragment() {
    vec4 base = texture(BaseTexture, UV);
    vec4 normal_texel = texture(NormalTexture, UV);
    vec4 specular_texel = texture(SpecularTexture, UV);
    vec3 colorization = eawr_unit_colorization.a < 0.0 ? eawr_colorization : eawr_unit_colorization.rgb;
    vec3 surface = mix(base.rgb, colorization * base.rgb, base.a);
    vec3 normal_vector = 2.0 * (normal_texel.rgb - 0.5);
    float n_dot_l = clamp(dot(normal_vector, eawr_tangent_light), 0.0, 1.0);
    float n_dot_h = clamp(dot(normal_vector, eawr_tangent_half), 0.0, 1.0);
    vec3 diffuse = 2.0 * surface
        * (n_dot_l * eawr_diffuse * eawr_light_diffuse * eawr_light_scale.rgb * eawr_unit_light_scale
            + eawr_vertex_diffuse);
    vec3 specular = eawr_light_specular * specular_texel.rgb * pow(n_dot_h, 32.0 * specular_texel.a);
    vec3 stored_rgb = diffuse + specular + eawr_emissive * normal_texel.a;
    ALBEDO = eawr_stored_albedo(stored_rgb);
}
)GODOT";

[[nodiscard]] inline std::string_view shader(const RenderPass pass) noexcept {
    if (pass != RenderPass::opaque) return {};
    static const std::string source = [] {
        std::string head(bump_colorize::shader_head);
        head.insert(sampler_offset + base_sampler.size(),
            "\nuniform sampler2D SpecularTexture : filter_linear_mipmap, repeat_enable;");
        std::string vertex(bump_colorize::shader_tail.substr(0, fragment_offset));
        vertex.erase(emissive_offset, vertex_emissive.size());
        return head + std::string(bump_colorize::mesh_frame_normal) + vertex + std::string(fragment);
    }();
    return source;
}

inline constexpr TextureBinding sampled_textures[]{
    {"NormalTexture", TexturePlaceholder::flat_normal}, {"SpecularTexture", TexturePlaceholder::black}};

[[nodiscard]] inline std::optional<std::string> binding_problem(const MaterialDescription& material) {
    for (const std::string_view name : {std::string_view("Emissive"), std::string_view("Diffuse"),
             std::string_view("Colorization"), std::string_view("UVOffset")}) {
        if (auto problem = bindings::vector_problem(material, name)) return problem;
    }
    for (const TextureBinding& binding : sampled_textures) {
        const auto count = bindings::count(material, binding.name);
        if (count != 1) return "requires exactly one " + std::string(binding.name) + " texture";
        if (!std::holds_alternative<std::string>(bindings::find(material, binding.name)->value)) {
            return "binds " + std::string(binding.name) + " as a value that is not a texture";
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
    const auto offset = bindings::vector_or(material, "UVOffset", bump_colorize::default_uv_offset);
    return {
        {"eawr_emissive", rgb("Emissive", {1.0F, 1.0F, 1.0F, 0.0F})},
        {"eawr_diffuse", rgb("Diffuse", bump_colorize::default_diffuse)},
        {"eawr_colorization", rgb("Colorization", bump_colorize::default_colorization)},
        {"eawr_uv_offset", std::array<float, 2>{offset[0], offset[1]}},
    };
}

inline constexpr Family family{
    .program = "MeshBumpSpecGlowColorize.fx",
    .technique = "sph_t2",
    .pass_name = "sph_t2_p0",
    .opaque = true,
    .transparent = false,
    .receives_shadows = true,
    .binding_textures = sampled_textures,
    .authored_binormals = true,
    .shader = &shader,
    .binding_problem = &binding_problem,
    .uniforms = &uniforms,
};

} // namespace eawr::presentation::godot_backend::legacy::mesh_bump_spec_glow_colorize
