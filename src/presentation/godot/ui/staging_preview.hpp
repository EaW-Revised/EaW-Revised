#pragma once

#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/rect2.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace godot { class Control; class TextureRect; }
namespace eawr::assets { struct MegaTextureAtlas; }
namespace eawr::skirmish { struct SetupMap; struct SetupFaction; }
namespace eawr::vfs { class Vfs; }

namespace eawr::presentation::godot_backend {
class UiTextures;

// The font role of the dialog's preview gadget, from its statement and window class as authored.
[[nodiscard]] std::string preview_font_role(const std::string& statement, const std::string& class_name);

struct StagingPreviewSeat final {
    std::uint32_t team{};
    std::string faction;
};

// WSS-10/63: view-owned start markers, shared by local and network staging.
// Rebuild only on map changes; roster edits reuse the resolved atlas textures.
class StagingPreview final {
public:
    void clear();
    // `font_role` and `variation` are the role and theme variation the dialog authors for the preview
    // gadget (see preview_font_role); a control-level font entry for the gadget lives in the variation.
    void rebuild(godot::TextureRect& preview, const skirmish::SetupMap& map,
                 const vfs::Vfs& filesystem, std::span<const skirmish::SetupFaction> factions,
                 const assets::MegaTextureAtlas* atlas, UiTextures* textures,
                 std::string_view font_role = "Global_Default",
                 std::string_view variation = "EawrUi");
    void update(std::span<const StagingPreviewSeat> occupied);
    [[nodiscard]] std::size_t size() const noexcept { return markers_.size(); }
    [[nodiscard]] std::size_t emblem_count() const noexcept;
    // Visible emblems' screen rectangles (canvas-transformed), for the pixel test.
    [[nodiscard]] std::vector<godot::Rect2> emblem_rects() const;
    // Each start number's measured text block (canvas-transformed), and the flags of the font role it uses.
    [[nodiscard]] std::vector<godot::Rect2> number_rects() const;
    [[nodiscard]] bool number_outline() const;
    [[nodiscard]] bool number_emboss() const;

private:
    struct Marker final {
        std::uint32_t team{};
        godot::Control* backing{};
        godot::TextureRect* emblem{};
        godot::Control* number{};
    };
    std::vector<Marker> markers_;
    std::map<std::string, godot::Ref<godot::Texture2D>, std::less<>> icons_;
};
} // namespace eawr::presentation::godot_backend
