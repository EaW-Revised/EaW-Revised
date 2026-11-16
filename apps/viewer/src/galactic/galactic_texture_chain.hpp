#pragma once

#include "compose_texture_cache.hpp"
#include "eawr/assets/assets.hpp"
#include "eawr/assets/texture_chain.hpp"
#include "eawr/scene/texture_prefetch.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::godot_backend {

// legacy #6726: the load-time mip chain of the galaxy scene's model textures (starfield tiles, planets, fleet models).
// A mod texture that ships as one level, or stops above 1x1, is sampled from its top level when the view is zoomed out, so
// stars become hard points and models shimmer; the legacy shaders sample with linear mip filtering and need the levels.
// The policy is the space compose's (#5989, texture_processing_for), chosen by the surface's effect and the binding, never
// by file or mod name:
//   - a legacy-effect surface's BaseTexture: texture_processing_for(selector) (colour chain for an opaque effect, the
//     brightness-compensated chain for an additive one, none for another transparent effect);
//     (the compensated chain is built from a single-level texture only, so an additive texture with a partial authored
//     chain stays as authored, exactly as in the space compose: a follow-up if it matters);
//   - a family texture the effect samples as a tangent-space normal map: the normal chain;
//   - the environment-effect surfaces (planets): the plain colour chain, as for every environment texture
//     (load_environment_texture: the plain chain is what matches the retail captures, additive or not);
//   - any other family texture: unchanged.
// A texture whose chain is complete, or of a format the chain does not handle, comes back as decoded, so a stock
// texture (all ship full chains) is byte-identical. Scene load only, never per frame.
[[nodiscard]] inline scene::TextureProcessing galactic_base_processing(const scene::LegacySelector* selector) noexcept {
    return selector == nullptr ? scene::TextureProcessing::colour_mip_chain : texture_processing_for(*selector);
}

// A family binding of a legacy-effect surface: `declared` is the texture name the binding was given.
[[nodiscard]] inline scene::TextureProcessing galactic_family_processing(const scene::LegacySelector& selector,
                                                                         const std::string_view declared,
                                                                         const std::vector<std::string>& normal_maps) noexcept {
    if (texture_processing_for(selector) == scene::TextureProcessing::none) return scene::TextureProcessing::none;
    return std::find(normal_maps.begin(), normal_maps.end(), declared) != normal_maps.end()
        ? scene::TextureProcessing::normal_mip_chain : scene::TextureProcessing::none;
}

[[nodiscard]] inline std::optional<assets::ChainKind> galactic_chain_kind(const scene::TextureProcessing processing) noexcept {
    switch (processing) {
    case scene::TextureProcessing::colour_mip_chain: return assets::ChainKind::colour;
    case scene::TextureProcessing::normal_mip_chain: return assets::ChainKind::normal;
    case scene::TextureProcessing::compensated_light_mip_chain: return assets::ChainKind::compensated_light;
    case scene::TextureProcessing::none: break;
    }
    return std::nullopt;
}

// Completes the chain the policy asks for. True when levels were added.
inline bool apply_galactic_chain(assets::Texture& texture, const scene::TextureProcessing processing) {
    const auto kind = galactic_chain_kind(processing);
    return kind && assets::extend_mip_chain(texture, *kind);
}

} // namespace eawr::presentation::godot_backend
