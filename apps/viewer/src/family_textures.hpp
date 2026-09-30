#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "legacy/registry.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The per-binding textures a legacy/ family samples besides BaseTexture (the
// bump colorize NormalTexture, #199), for every viewer upload site.
namespace eawr::presentation::godot_backend {

// The normal binding fallback: a flat
// tangent-space normal (128, 128, 255) with gloss alpha 0, so the sun lights
// the vertex normal and adds no highlight. A viewer policy, like the grey
// BaseTexture placeholder; the scene records the texture_unresolved cause.
[[nodiscard]] inline assets::Texture family_placeholder_texture(const legacy::TexturePlaceholder placeholder) {
    assets::MipLevel mip;
    mip.width = 1;
    mip.height = 1;
    mip.row_pitch = 4;
    mip.bytes = placeholder == legacy::TexturePlaceholder::flat_normal
        ? std::vector<std::byte>{std::byte{128}, std::byte{128}, std::byte{255}, std::byte{0}}
        : std::vector<std::byte>(4, std::byte{0});
    assets::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.format = assets::PixelFormat::rgba8;
    texture.mips.push_back(std::move(mip));
    return texture;
}

// One entry per texture the material's family samples besides BaseTexture,
// loaded by `load(declared name) -> std::optional<assets::Texture>`. A binding
// that is missing or not a texture gets no entry: the renderer then refuses
// the material with its own diagnostic.
template <typename Load>
[[nodiscard]] std::vector<GodotRenderer::BindingTexture> family_binding_textures(
    const MaterialDescription& material, const std::span<const legacy::TextureBinding> sampled, Load&& load) {
    std::vector<GodotRenderer::BindingTexture> result;
    for (const legacy::TextureBinding& texture : sampled) {
        const std::string_view name = texture.name;
        const MaterialBinding* binding = legacy::bindings::find(material, name);
        const auto* declared = binding == nullptr ? nullptr : std::get_if<std::string>(&binding->value);
        if (declared == nullptr) continue;
        std::optional<assets::Texture> loaded = load(*declared);
        result.push_back({std::string(name), loaded ? std::move(*loaded) : family_placeholder_texture(texture.placeholder)});
    }
    return result;
}

template <typename Load>
[[nodiscard]] std::vector<GodotRenderer::BindingTexture> family_binding_textures(
    const MaterialDescription& material, Load&& load) {
    return family_binding_textures(material, legacy::binding_textures(material), std::forward<Load>(load));
}

} // namespace eawr::presentation::godot_backend
