#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/presentation/ui/theme.hpp"

#include <string_view>
#include <utility>

namespace eawr::viewer::galactic {
// Resolve only the authored radar-view slot through a lower atlas. Other HUD
// lookups retain the effective atlas/standalone policy and their own cache.
template<class Original, class Materialize>
auto radar_texture_lookup(const vfs::Vfs* filesystem, const assets::MegaTextureAtlas* effective,
                          std::string_view atlas_path, std::string_view name,
                          Original&& original, Materialize&& materialize) {
    if (!filesystem || !effective || name.empty()) return original();
    const auto slot = presentation::ui::resolve_ui_texture(name, &effective->directory,
        presentation::ui::vfs_standalone_textures(*filesystem));
    if (slot.origin != presentation::ui::TextureOrigin::missing) return original();
    auto fallback = assets::load_shadowed_atlas_entry(*filesystem, atlas_path, name);
    if (!fallback || !fallback.value()) return original();
    auto atlas = std::move(*fallback.value());
    const auto lower = presentation::ui::resolve_ui_texture(name, &atlas.directory, {});
    if (auto texture = materialize(atlas, lower)) return *texture;
    return original();
}
} // namespace eawr::viewer::galactic
