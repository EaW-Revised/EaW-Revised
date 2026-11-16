#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/assets/texture_chain.hpp"
#include "eawr/core/load_profile.hpp"
#include "eawr/vfs/vfs_fwd.hpp"
#include "particle_chain_counters.hpp"

#include <chrono>
#include <cstdint>
#include <string_view>
#include <utility>

namespace eawr::presentation::godot_backend {

// MT-03 (docs/behaviour/texture-mip-upload.md): the one loader for the colour texture and the bump normal texture of
// every particle system. A mod's sprite texture can stop above 1x1 (BFC and Remake ship hundreds of single-level ones).
// The particle shaders sample with linear mip filtering, so a small or distant sprite sampled from a lone top level
// sparkles and aliases. Retail asks its loader for a full box-filtered chain, so the chain is completed here with the
// plain box filter on the stored values (assets::extend_mip_chain, ChainKind::colour), once, when the texture is first
// resolved. Every effect a battle can draw is resolved at load (the effect prewarm), so no chain is built on a battle
// frame (particle_chain_counters() shows it). Same kind as the environment loader (environment_texture.hpp); the ship
// policy's brightness-compensated chain is not used. A complete chain, or a format the generator does not handle (BC4,
// BC5, BC7, L8, A8, BC1 with 1-bit alpha), comes back as decoded; stock FoC particle textures all ship full chains, so
// stock is byte-identical.
// The bump normal texture takes the colour kind too. That is the same output only while renormalize_normal_chains is off
// (a plain box average is what retail's request builds); the assertion makes anyone who turns it on decide here.
static_assert(!assets::renormalize_normal_chains,
              "particle normal textures are chained with ChainKind::colour; give them ChainKind::normal first");
// Every particle texture resolver (battle effects, unit emitters, debris, land map particles, galactic corruption, the
// effect probe) must load through this function: tests/presentation/renderer/test_particle_texture_origin.py fails on
// a raw load in those files.
[[nodiscard]] inline core::Result<assets::Texture> load_particle_texture(const vfs::Vfs& filesystem,
                                                                         std::string_view logical_path) {
    auto loaded = assets::load_texture(filesystem, logical_path);
    if (loaded) {
        core::load_profile::Scope scope(core::load_profile::Phase::texture);
        const auto start = std::chrono::steady_clock::now();
        if (assets::extend_mip_chain(loaded.value(), assets::ChainKind::colour)) {
            auto& counters = particle_chain_counters();
            counters.builds.fetch_add(1, std::memory_order_relaxed);
            counters.microseconds.fetch_add(
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count()),
                std::memory_order_relaxed);
        }
    }
    return loaded;
}

} // namespace eawr::presentation::godot_backend
