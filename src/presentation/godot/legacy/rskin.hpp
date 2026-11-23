#pragma once

#include "dx8_mesh.hpp"
#include "mesh_additive.hpp"

// CEM-08/09: these skinned families share the qualified pixel arithmetic
// and bindings below. The renderer supplies native bone/weight streams and
// the pose palette before Godot runs either vertex stage.
namespace eawr::presentation::godot_backend::legacy::rskin {

inline constexpr Family additive_family{
    .program = "RSkinAdditive.fx",
    .technique = "t0",
    .pass_name = "t0_p0",
    .opaque = false,
    .transparent = true,
    .receives_shadows = false,
    .shader = &mesh_additive::shader,
    .binding_problem = &mesh_additive::binding_problem,
    .uniforms = &mesh_additive::uniforms,
};

inline constexpr Family alpha_family{
    .program = "RSkinAlpha.fx",
    .technique = "sph_t0",
    .pass_name = "sph_t0_p0",
    .opaque = false,
    .transparent = true,
    .receives_shadows = true,
    .shader = &dx8_mesh::batch_mesh_alpha_shader,
    .binding_problem = &dx8_mesh::colour_problem,
    .uniforms = &dx8_mesh::alpha_uniforms,
};

// RSkinAlphaGloss.fx sph_t0 (#8037): the vertex stage is the skinned form of MeshAlphaGloss's, the pixel stage is
// RGB = 2 D.rgb x base.rgb + S x gloss.r, alpha base.a x D.a. Its CPUSKIN technique is the fixed-function sph_t1.
inline constexpr Family alpha_gloss_family{
    .program = "RSkinAlphaGloss.fx",
    .technique = "sph_t0",
    .pass_name = "sph_t0_p0",
    .opaque = false,
    .transparent = true,
    .receives_shadows = true,
    .binding_textures = dx8_mesh::gloss_textures,
    .shader = &dx8_mesh::mesh_alpha_gloss_shader,
    .binding_problem = &dx8_mesh::gloss_problem,
    .uniforms = &dx8_mesh::alpha_uniforms,
};

} // namespace eawr::presentation::godot_backend::legacy::rskin
