#include "galactic/galactic_texture_chain.hpp"

#include "eawr/assets/texture_chain.hpp"
#include "eawr/scene/scene.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

// legacy #6726: the galaxy scene's load-time chain policy: the same choice as the space compose for a legacy-effect
// surface's BaseTexture, the plain chain for an environment (planet) surface, the normal chain for a family normal map.
// A texture with a complete chain is untouched. Pure CPU; no game data.
namespace {
using namespace eawr::assets;
namespace gb = eawr::presentation::godot_backend;
using eawr::scene::TextureProcessing;

int failures = 0;
void expect(const bool condition, const std::string_view message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

// A starfield-like image: a dark field with scattered bright points.
std::vector<std::uint8_t> stars(const std::uint32_t width, const std::uint32_t height) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4U, 0);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint8_t* const texel = &rgba[(static_cast<std::size_t>(y) * width + x) * 4U];
            const bool star = ((x * 7U + y * 13U) % 61U) == 0U;
            texel[0] = star ? 255 : static_cast<std::uint8_t>((x + y) % 5U);
            texel[1] = star ? 240 : static_cast<std::uint8_t>(x % 4U);
            texel[2] = star ? 200 : static_cast<std::uint8_t>(y % 6U);
            texel[3] = 255;
        }
    return rgba;
}

Texture single_level(const std::uint32_t side) {
    Texture texture;
    texture.width = side;
    texture.height = side;
    texture.format = PixelFormat::bc3;
    texture.has_alpha = true;
    MipLevel level;
    level.width = side;
    level.height = side;
    level.row_pitch = ((side + 3U) / 4U) * 16U;
    level.bytes = encode_blocks(PixelFormat::bc3, stars(side, side), side, side);
    texture.mips.push_back(std::move(level));
    return texture;
}

bool same_levels(const Texture& a, const Texture& b) {
    if (a.mips.size() != b.mips.size()) return false;
    for (std::size_t i = 0; i < a.mips.size(); ++i)
        if (a.mips[i].bytes != b.mips[i].bytes) return false;
    return true;
}

void policy_per_surface() {
    const auto* opaque = eawr::scene::find_legacy_selector("MeshGloss.fx");
    const auto* additive = eawr::scene::find_legacy_selector("MeshAdditive.fx");
    expect(opaque != nullptr && additive != nullptr, "setup: both selectors exist");
    if (opaque == nullptr || additive == nullptr) return;
    expect(gb::galactic_base_processing(opaque) == TextureProcessing::colour_mip_chain, "an opaque effect: the colour chain");
    expect(gb::galactic_base_processing(additive) == TextureProcessing::compensated_light_mip_chain,
        "an additive effect: the compensated chain");
    expect(gb::galactic_base_processing(nullptr) == TextureProcessing::colour_mip_chain,
        "an environment (planet) surface: the plain colour chain");
    for (const auto& selector : eawr::scene::legacy_selectors())
        expect(gb::galactic_base_processing(&selector) == gb::texture_processing_for(selector),
            "a legacy effect uses the space compose's choice: " + std::string(selector.program));
}

void single_level_gets_a_chain() {
    const auto* opaque = eawr::scene::find_legacy_selector("MeshGloss.fx");
    const auto* additive = eawr::scene::find_legacy_selector("MeshAdditive.fx");
    if (opaque == nullptr || additive == nullptr) return;
    Texture colour = single_level(128);
    expect(mip_chain_incomplete(colour), "setup: one level stops above 1x1");
    expect(gb::apply_galactic_chain(colour, gb::galactic_base_processing(opaque)) && colour.mips.size() == 8
            && colour.mips.back().width == 1,
        "a single-level opaque texture gets its full chain, down to 1 x 1");
    Texture reference = single_level(128);
    expect(extend_mip_chain(reference, ChainKind::colour) && same_levels(colour, reference), "and it is the plain colour chain");

    Texture light = single_level(128);
    expect(gb::apply_galactic_chain(light, gb::galactic_base_processing(additive)) && light.mips.size() == 8,
        "a single-level additive texture gets a full chain");
    Texture compensated = single_level(128);
    expect(extend_mip_chain(compensated, ChainKind::compensated_light) && same_levels(light, compensated),
        "which is the brightness-compensated chain");
    expect(!same_levels(light, colour), "and differs from the plain one below the first filtered level");

    Texture planet = single_level(128);
    expect(gb::apply_galactic_chain(planet, gb::galactic_base_processing(nullptr)) && same_levels(planet, colour),
        "a planet texture gets the plain chain");
}

void partial_chain_is_completed() {
    const auto* opaque = eawr::scene::find_legacy_selector("MeshGloss.fx");
    if (opaque == nullptr) return;
    Texture full = single_level(128);
    expect(extend_mip_chain(full, ChainKind::colour), "setup: a full chain");
    Texture partial = full;
    partial.mips.resize(3);
    const Texture authored = partial;
    expect(mip_chain_incomplete(partial), "setup: an authored chain stopping at 32 px");
    expect(gb::apply_galactic_chain(partial, gb::galactic_base_processing(opaque)) && partial.mips.size() == full.mips.size(),
        "a partial chain gets its tail");
    bool kept = true;
    for (std::size_t i = 0; i < 3; ++i) kept = kept && partial.mips[i].bytes == authored.mips[i].bytes;
    expect(kept, "the authored levels keep every byte");
}

// Parity with the space compose, pinned: the brightness-compensated chain is built from a single-level texture only
// (extend_mip_chain refuses it for an authored partial chain), so an additive texture that stops above 1x1 with several
// authored levels stays as authored. Follow-up if it matters.
void additive_partial_chain_stays_as_authored() {
    const auto* additive = eawr::scene::find_legacy_selector("MeshAdditive.fx");
    if (additive == nullptr) return;
    Texture full = single_level(128);
    expect(extend_mip_chain(full, ChainKind::colour), "setup: a full chain");
    Texture partial = full;
    partial.mips.resize(3);
    const Texture authored = partial;
    expect(!gb::apply_galactic_chain(partial, gb::galactic_base_processing(additive)) && same_levels(partial, authored),
        "an additive texture with a partial authored chain is left as authored (as in the space compose)");
}

void full_chain_is_unchanged() {
    const auto* opaque = eawr::scene::find_legacy_selector("MeshGloss.fx");
    const auto* additive = eawr::scene::find_legacy_selector("MeshAdditive.fx");
    if (opaque == nullptr || additive == nullptr) return;
    Texture full = single_level(64);
    expect(extend_mip_chain(full, ChainKind::colour), "setup: a full chain");
    for (const auto processing : {gb::galactic_base_processing(opaque), gb::galactic_base_processing(additive),
             gb::galactic_base_processing(nullptr), TextureProcessing::normal_mip_chain}) {
        Texture copy = full;
        expect(!gb::apply_galactic_chain(copy, processing) && same_levels(copy, full) && copy.mips.size() == full.mips.size(),
            "a texture that already ships a full chain is untouched (stock stays byte-identical)");
    }
    Texture single = single_level(64);
    const Texture before = single;
    expect(!gb::apply_galactic_chain(single, TextureProcessing::none) && same_levels(single, before),
        "a texture whose effect asks for no chain is untouched");
}

void family_textures() {
    const auto* opaque = eawr::scene::find_legacy_selector("MeshGloss.fx");
    const auto* additive = eawr::scene::find_legacy_selector("MeshAdditive.fx");
    if (opaque == nullptr || additive == nullptr) return;
    const std::vector<std::string> normal_maps{"w_ship_bump.tga"};
    expect(gb::galactic_family_processing(*opaque, "w_ship_bump.tga", normal_maps) == TextureProcessing::normal_mip_chain,
        "a family normal map gets the normal chain");
    expect(gb::galactic_family_processing(*opaque, "w_ship_mask.tga", normal_maps) == TextureProcessing::none,
        "another family texture is unchanged");
    expect(gb::galactic_family_processing(*opaque, "w_ship_bump.tga", {}) == TextureProcessing::none, "no declared normal map, no chain");
    for (const auto& selector : eawr::scene::legacy_selectors())
        if (gb::texture_processing_for(selector) == TextureProcessing::none)
            expect(gb::galactic_family_processing(selector, "w_ship_bump.tga", normal_maps) == TextureProcessing::none,
                "a transparent effect's family textures are unchanged");
    expect(gb::galactic_family_processing(*additive, "w_ship_bump.tga", normal_maps) == TextureProcessing::normal_mip_chain,
        "an additive effect's declared normal map takes the normal chain, as in the space compose");
}
} // namespace

int main() {
    policy_per_surface();
    single_level_gets_a_chain();
    partial_chain_is_completed();
    additive_partial_chain_stays_as_authored();
    full_chain_is_unchanged();
    family_textures();
    return failures == 0 ? 0 : 1;
}
