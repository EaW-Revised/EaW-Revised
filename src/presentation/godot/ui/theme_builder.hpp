#pragma once

// Godot theme builder for the UI kit (ticket UI-06 #229, docs/ui/ui-layer.md
// section 3.6). It turns a presentation::ui::ThemeModel into one Theme: the
// default style is the `EawrUi` type and every per-dialog, per-control and
// chained style a type variation, holding
//
// - one icon per skin slot, cut from the mega-texture page (or read from a
//   standalone texture file) at texel resolution and sized to the model's
//   UI-L3 scale, so a kit piece draws at its texture size;
// - per font role a font (the UI-F3 face as a FontVariation carrying the
//   Character_Padding and the UI-F2 width), its UI-F1/UI-F2 pixel size, the
//   `<role>_top` and `<role>_bottom` colours and the `<role>_emboss`,
//   `<role>_outline`, `<role>_cell_ascent` and `<role>_cell_descent`
//   constants (kit.hpp).
//
// A slot written "none", or one whose texture resolves nowhere, gets an empty
// marker texture so it still overrides the base style.

#include "ui/font_provider.hpp"
#include "ui/kit.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/presentation/ui/theme.hpp"
#include "eawr/vfs/vfs.hpp"

#include <godot_cpp/classes/font_variation.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/theme.hpp>

#include <cstddef>
#include <map>
#include <string_view>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

// The (empty) base type the default `EawrUi` style is declared a variation of.
inline constexpr std::string_view kit_base_type = "EawrUiBase";

// The base mip of a decoded texture as an RGBA8, top-left-first image, or null
// with `failure` set.
[[nodiscard]] godot::Ref<godot::Image> texture_image(const assets::Texture& texture, std::string& failure);

// Slot textures for one atlas page and the VFS its standalone textures live in.
class UiTextures final {
public:
    UiTextures(const assets::MegaTextureAtlas& atlas, const vfs::Vfs& filesystem);

    // Why the page could not be read; empty when it was.
    [[nodiscard]] const std::string& page_failure() const noexcept { return page_failure_; }
    [[nodiscard]] godot::Ref<godot::Image> page() const { return page_; }
    // The slot's texture at texel resolution, sized to its texel size times
    // the scale; null for none, missing or unreadable slots (see problems()).
    [[nodiscard]] godot::Ref<godot::Texture2D> texture(const presentation::ui::ThemeTexture& slot, double scale_x,
                                                       double scale_y);
    // The empty marker texture.
    [[nodiscard]] godot::Ref<godot::Texture2D> empty();
    // Standalone textures that could not be read, one line each.
    [[nodiscard]] const std::vector<std::string>& problems() const noexcept { return problems_; }

private:
    const vfs::Vfs& filesystem_;
    godot::Ref<godot::Image> page_;
    std::string page_failure_;
    std::map<std::string, godot::Ref<godot::Image>> standalone_;
    std::map<std::string, godot::Ref<godot::ImageTexture>> cache_;
    godot::Ref<godot::ImageTexture> empty_;
    std::vector<std::string> problems_;
};

struct ThemeBuildSummary final {
    std::size_t styles{};
    std::size_t icons{};
    std::size_t empty_icons{}; // none, missing or unreadable slots
    std::size_t fonts{};
    std::size_t font_variations{}; // distinct FontVariation resources
};

[[nodiscard]] godot::Ref<godot::Theme> build_theme(const presentation::ui::ThemeModel& model, UiTextures& textures,
                                                   FontProvider& fonts, ThemeBuildSummary* summary = nullptr);

} // namespace eawr::presentation::godot_backend
