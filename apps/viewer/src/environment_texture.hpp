#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/assets/texture_chain.hpp"
#include "eawr/core/load_profile.hpp"
#include "eawr/vfs/vfs.hpp"

#include <string_view>
#include <utility>

namespace eawr::presentation::godot_backend {

// SKY-6358-07: the one loader for every texture an environment (sky, nebula, planet backdrop, glow) draws.
// A mod's star or nebula texture can stop above 1x1 (BFC ships single-level 1024 px ones); sampled from the top
// level every star becomes a hard point, so the chain is completed here, on a plain box filter of the stored
// values (assets::extend_mip_chain, ChainKind::colour). That is deliberately not the ship policy (an additive
// ship material gets the brightness-compensated chain, compose_texture_cache.hpp): the sky's stars are additive
// too, and the plain chain is what matches the retail captures. A texture with a complete chain, or in a format
// the chain does not handle (docs/rendering.md SKY-6358-07), comes back as decoded.
// Every environment route (the default view, the evidence harness, the translated-shader loaders) must load
// through this function: tests/presentation/renderer/test_space_meshgloss_sky.py fails on a raw load there.
[[nodiscard]] inline core::Result<assets::Texture> load_environment_texture(const vfs::Vfs& filesystem,
                                                                            std::string_view logical_path) {
    auto loaded = assets::load_texture(filesystem, logical_path);
    if (loaded) {
        core::load_profile::Scope scope(core::load_profile::Phase::texture);
        assets::extend_mip_chain(loaded.value(), assets::ChainKind::colour);
    }
    return loaded;
}

} // namespace eawr::presentation::godot_backend
