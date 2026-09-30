#pragma once

// Engine-free UI theme model (ticket UI-06 #229, docs/ui/ui-layer.md sections
// 3.2 and 3.6). It turns the GUIDialogs.xml skin of a dialog catalogue into
// what the Godot theme builder needs: every texture slot resolved against the
// mega-texture atlas (the MTD directory) or a standalone texture, and every font
// role resolved through UI-F3 and sized by UI-F1 and UI-F2.
//
// The Default set is the base style. Each per-dialog and per-control set is a
// type variation of it holding only that set's slots and fonts. A control with
// its own set inside a dialog with its own set gets a chained variation (its
// base is the dialog's), so a per-item lookup walks control -> dialog ->
// Default exactly as data::ui::DialogCatalog resolves one control.

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/data/ui/dialog_catalog.hpp"
#include "eawr/presentation/ui/fonts.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::ui {

namespace diagnostic_codes {
inline constexpr std::string_view theme_slot = "EAWR-UI-0601";    // a texture slot no kit part draws
inline constexpr std::string_view theme_texture = "EAWR-UI-0602"; // a slot's texture is in neither the atlas nor a file
inline constexpr std::string_view theme_font = "EAWR-UI-0603";    // a font entry leaves out its face, size or colour
} // namespace diagnostic_codes

// The theme type every kit Control starts from; variations are named
// `EawrUi__<entry>` and, chained, `EawrUi__<dialog>__<control>` (Godot type
// names take identifier characters only).
inline constexpr std::string_view theme_type = "EawrUi";

// The kit part that draws a slot.
enum class KitPart : std::uint8_t {
    frame,       // the 16-piece dialog frame and its background
    small_frame, // the 8-piece frame of lists, edits and group boxes
    button,      // 3-piece push button, four states
    check,       // check box
    radio,       // radio button
    dial,        // slider (msctls_trackbar32): track, tab, minus and plus
    scroll,      // list scroll bar
    trackbar,    // the trackbar's scroll set
    combo,       // combo box
    progress,    // progress bar (msctls_progress32)
    scanlines,   // the dialog background's scan-line overlay
};

[[nodiscard]] std::string_view to_string(KitPart part) noexcept;

struct KitSlot final {
    std::string_view name;
    KitPart part;
};

// Every texture slot the kit draws, named as the XML elements of the retail
// Default set and in its order (87 slots).
extern const std::array<KitSlot, 87> kit_slots;

// Exact, case-sensitive match, as the catalogue matches slot names.
[[nodiscard]] const KitSlot* find_kit_slot(std::string_view name) noexcept;

enum class TextureOrigin : std::uint8_t {
    atlas,      // an entry of the mega-texture directory
    standalone, // a texture file Data/Art/Textures/<stem>.tga or .dds
    none,       // the set writes "none": the slot is deliberately empty
    missing,    // neither: drawn as nothing, with one EAWR-UI-0602
};

[[nodiscard]] std::string_view to_string(TextureOrigin origin) noexcept;

struct ThemeTexture final {
    std::string slot;
    std::string texture; // as written; empty for none
    TextureOrigin origin{TextureOrigin::missing};
    assets::AtlasRectangle rectangle; // atlas only, in page pixels
    bool has_alpha{};                 // atlas only
    std::string logical_path;         // standalone only
};

struct ThemeFont final {
    data::ui::FontRole role{data::ui::FontRole::global_default};
    // Where the description came from; a role the Default set lacks takes
    // Global_Default at the default level, as the catalogue does.
    data::ui::OverrideLevel level{data::ui::OverrideLevel::none};
    std::string face;       // requested
    std::int32_t point_size{};
    ResolvedFont resolved;  // UI-F3
    FontPixels pixels;      // UI-F1 and UI-F2 at the model's screen height
    // GDI's text cell for a cached face: its OS/2 usWinAscent and
    // usWinDescent at the glyph height, rounded. The original places text by
    // this cell, which for the EmpireAtWar faces reaches well above the hhea
    // ascender an engine uses. Zero for other faces, which keep the engine's
    // metrics.
    std::int32_t cell_ascent{};
    std::int32_t cell_descent{};
    std::int32_t character_padding{};
    double stretch_factor{1.0};
    data::ui::Rgba top_color;
    data::ui::Rgba bottom_color;
    bool emboss{};
    bool outline{};
    // A font entry is taken whole, so fields it leaves out are not inherited:
    // they get project-authored values (EmpireAtWar-Medium, 8 pt, white, no
    // padding, no stretch, no emboss or outline). Named here, as written in XML.
    std::vector<std::string> defaulted;
};

struct ThemeStyle final {
    std::string name;  // theme_type, or a variation name
    std::string base;  // empty for the default style
    data::ui::OverrideLevel level{data::ui::OverrideLevel::default_set};
    std::string entry; // the GUIDialogs entry (the control's, for a chained style)
    std::vector<ThemeTexture> textures;
    std::vector<ThemeFont> fonts;

    [[nodiscard]] const ThemeTexture* texture(std::string_view slot) const noexcept;
    [[nodiscard]] const ThemeFont* font(data::ui::FontRole role) const noexcept;
};

struct ThemeModel final {
    ReferenceSpace space;
    // Texture pieces draw at their texel size times these: the UI-L3 dialog
    // scale (uniform, or stretched per axis under retail rules).
    double scale_x{};
    double scale_y{};
    std::uint32_t font_screen_height{}; // the H of UI-F1
    std::string language;
    std::string atlas; // the MTD's logical path
    ThemeStyle defaults;
    // Dialog and control variations in skin file order, then the chained ones.
    std::vector<ThemeStyle> variations;
    // Override entries that name no dialog and no control; they get no style.
    std::vector<std::string> unmatched;
    std::vector<core::Diagnostic> diagnostics;

    [[nodiscard]] const ThemeStyle* find(std::string_view name) const noexcept;
    // The style a control of `dialog` takes; `control` null is the dialog
    // itself (its frame). theme_type when neither has an entry.
    [[nodiscard]] std::string variation(const data::ui::Dialog& dialog,
                                        const data::ui::DialogControl* control) const;
    // Walks from `style` through its bases to the default style, as Godot's
    // theme lookup does per item.
    [[nodiscard]] const ThemeTexture* texture(std::string_view style, std::string_view slot) const noexcept;
    [[nodiscard]] const ThemeFont* font(std::string_view style, data::ui::FontRole role) const noexcept;
};

// The logical path of a standalone texture, or nothing.
using StandaloneTextures = std::function<std::optional<std::string>(std::string_view texture)>;

// Data/Art/Textures/<stem>.tga, then .dds, as data::ui::vfs_texture_probe
// probes (the name may carry either extension), then the name as written, for
// the other image files mods name. `filesystem` must outlive it.
[[nodiscard]] StandaloneTextures vfs_standalone_textures(const vfs::Vfs& filesystem);

// A UI texture name resolved as a kit slot's is: the mega-texture entry, then a
// standalone file; `missing` when neither has it (no diagnostic is recorded).
[[nodiscard]] ThemeTexture resolve_ui_texture(std::string_view texture, const assets::MegaTexture* atlas,
                                              const StandaloneTextures& standalone);

// Limits for a standalone mod JPEG, checked before any decoder allocates:
// the file size and the frame's pixel count (the same 8192^2 page budget as
// the texture loader's BMP/TGA pages).
inline constexpr std::size_t max_jpeg_bytes = 64U * 1024U * 1024U;
inline constexpr std::uint64_t max_jpeg_pixels = 8192ULL * 8192ULL;

struct JpegFrame final {
    std::uint32_t width{};
    std::uint32_t height{};
};

// The frame size from a JPEG's first SOFn marker, scanning markers without
// decoding; nothing when the data is not a JPEG, is truncated, or has no frame.
[[nodiscard]] std::optional<JpegFrame> jpeg_frame(std::span<const std::byte> bytes) noexcept;

// Empty when the JPEG may be decoded, else the reason it is refused.
[[nodiscard]] std::string jpeg_refusal(std::span<const std::byte> bytes);

struct ThemeSources final {
    const data::ui::DialogCatalog* catalog{};
    const assets::MegaTexture* atlas{};
    StandaloneTextures standalone;
    const FontCache* fonts{};
    SystemFaceProbe system;
    std::string language{"ENGLISH"};
};

// Builds the model for one screen. Never fails: an unresolved texture or an
// incomplete font entry is drawn as nothing or with project-authored values,
// with a diagnostic.
[[nodiscard]] ThemeModel build_theme_model(const ThemeSources& sources, const ReferenceSpace& space);

} // namespace eawr::presentation::ui
