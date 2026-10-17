#pragma once

#include "mesh_additive.hpp"

// CEM-03/CEM-06: the qualified doubled additive family keeps the shared
// vertex stage, bindings and ONE/ONE state; saturation follows sampling.
namespace eawr::presentation::godot_backend::legacy::mesh_additive_2x {

inline constexpr std::string_view sampled_product =
    "eawr_vertex_color * texture(BaseTexture, eawr_scrolled_uv).rgb";
inline constexpr std::size_t product_offset = mesh_additive::shader_source.find(sampled_product);
static_assert(product_offset != std::string_view::npos);
static_assert(mesh_additive::shader_source.find(sampled_product, product_offset + 1) == std::string_view::npos);

[[nodiscard]] inline std::string_view shader(const RenderPass pass) noexcept {
    if (pass != RenderPass::transparent) return {};
    static const std::string source = [] {
        std::string result(mesh_additive::shader_source);
        result.replace(product_offset, sampled_product.size(),
            "clamp(2.0 * eawr_vertex_color * texture(BaseTexture, eawr_scrolled_uv).rgb, 0.0, 1.0)");
        return result;
    }();
    return source;
}

inline constexpr Family family{
    .program = "MeshAdditive2x.fx",
    .technique = "t0",
    .pass_name = "t0_p0",
    .opaque = false,
    .transparent = true,
    .receives_shadows = false,
    .shader = &shader,
    .binding_problem = &mesh_additive::binding_problem,
    .uniforms = &mesh_additive::uniforms,
};

} // namespace eawr::presentation::godot_backend::legacy::mesh_additive_2x
