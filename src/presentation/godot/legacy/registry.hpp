#pragma once

#include "bump_colorize.hpp"
#include "dx8_mesh.hpp"
#include "family.hpp"
#include "grass.hpp"
#include "mesh_additive.hpp"
#include "mesh_additive_offset.hpp"
#include "mesh_additive_vcolor.hpp"
#include "mesh_gloss_colorize.hpp"
#include "mesh_solid_color.hpp"
#include "tree.hpp"

#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The legacy/ family table (WP-43). Adding a family is one include and one
// row here; renderer_contract.cpp and renderer.cpp reach every row through
// the functions below and never name a family. A family is found only by an
// exact, case-sensitive program and technique, like the renderer's original
// selector table; a program may have an original row and families for other
// techniques (#199).
namespace eawr::presentation::godot_backend::legacy {

inline constexpr Family families[]{
    mesh_additive::family,
    mesh_additive_offset::family,
    mesh_additive_vcolor::family,
    mesh_solid_color::family,
    mesh_gloss_colorize::family,
    tree::family,
    grass::family,
    bump_colorize::mesh_family,
    bump_colorize::rskin_family,
    dx8_mesh::batch_mesh_gloss_family,
    dx8_mesh::batch_mesh_alpha_family,
    dx8_mesh::mesh_alpha_gloss_family,
};

[[nodiscard]] inline std::span<const Family> registry() noexcept {
    return {std::begin(families), std::end(families)};
}

[[nodiscard]] inline const Family* find_family(
    const std::string_view program, const std::string_view technique) noexcept {
    for (const Family& family : families) {
        if (family.program == program && family.technique == technique) return &family;
    }
    return nullptr;
}

[[nodiscard]] inline const Family* find_family(const MaterialDescription& material) noexcept {
    return material.route == MaterialRoute::legacy_effect
        ? find_family(material.program, material.technique) : nullptr;
}

// Whether the family admits the material's render pass.
[[nodiscard]] inline bool admits_pass(const Family& family, const RenderPass pass) noexcept {
    return (pass == RenderPass::opaque && family.opaque)
        || (pass == RenderPass::transparent && family.transparent);
}

// The adapter source for an exact selector of a legacy/ family, independently
// of validate_material, or nullopt.
[[nodiscard]] inline std::optional<std::string_view> shader_source(const MaterialDescription& material) {
    const Family* family = find_family(material);
    if (family == nullptr || material.pass_name != family->pass_name
        || !admits_pass(*family, material.pass) || family->binding_problem(material)) {
        return std::nullopt;
    }
    const std::string_view source = family->shader(material.pass);
    return source.empty() ? std::nullopt : std::optional<std::string_view>{source};
}

// False only for a legacy/ family whose pass reads no engine light; every
// other legacy selector keeps the renderer's shadow-receiving variant rule.
[[nodiscard]] inline bool receives_shadows(const MaterialDescription& material) noexcept {
    const Family* family = find_family(material);
    return family == nullptr || family->receives_shadows;
}

// True only for a legacy/ family whose vertex stage reads the scene wind.
[[nodiscard]] inline bool reads_wind(const MaterialDescription& material) noexcept {
    const Family* family = find_family(material);
    return family != nullptr && family->reads_wind;
}
[[nodiscard]] inline std::vector<Uniform> uniforms(const MaterialDescription& material) {
    const Family* family = find_family(material);
    return family == nullptr ? std::vector<Uniform>{} : family->uniforms(material);
}

// The texture bindings besides BaseTexture that the material's legacy/ family
// samples; each must reach the upload as a per-binding texture.
[[nodiscard]] inline std::span<const TextureBinding> binding_textures(const MaterialDescription& material) noexcept {
    const Family* family = find_family(material);
    return family == nullptr ? std::span<const TextureBinding>{} : family->binding_textures;
}

// Whether the material's legacy/ family reads authored binormals (CUSTOM0).
[[nodiscard]] inline bool authored_binormals(const MaterialDescription& material) noexcept {
    const Family* family = find_family(material);
    return family != nullptr && family->authored_binormals;
}

} // namespace eawr::presentation::godot_backend::legacy
