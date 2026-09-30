#pragma once

// Engine-free UI layout, scaling and font-size model (design
// docs/ui/ui-layer.md section 2 rules UI-L1 to UI-L5 and UI-F1 to UI-F2,
// tickets UI-04 #171 and UI-04b #195). Pixel rects use the screen's top-left
// origin, y down, and continuous coordinates: a rect covers
// [x, x + width) x [y, y + height). Nothing here rounds to whole pixels; the
// Godot builder decides that.

#include "eawr/data/ui/shell_anchors.hpp"

#include <cstdint>

namespace eawr::presentation::ui {

// UI-L1: the command-bar reference space.
inline constexpr double reference_width = 1024.0;
inline constexpr double reference_height = 768.0;

// UI-L1: the widest area the UI lays out in. A wider screen centres an area of
// this aspect, 1365.33 x 768 reference units (decision in ui-layer.md 3.4).
inline constexpr std::uint32_t safe_area_aspect_width = 16U;
inline constexpr std::uint32_t safe_area_aspect_height = 9U;
inline constexpr double safe_area_width = reference_height * safe_area_aspect_width / safe_area_aspect_height;

// Which rules lay the UI out. Retail reproduces the original for comparison
// captures: the HUD in the screen's lower-left corner and dialogs stretched
// on both axes. Aspect-correct is decision D4 (#166): the same HUD and dialogs
// inside a centred safe area, and dialogs scaled uniformly.
enum class LayoutRules : std::uint8_t {
    aspect_correct,
    retail,
};

struct Viewport {
    std::uint32_t width{};
    std::uint32_t height{};
};

struct ReferenceSpace {
    Viewport viewport;
    LayoutRules rules{};
    double width{};          // reference units across the screen
    double height{};         // reference units down the screen
    double scale{};          // screen pixels per reference unit
    bool height_based{};     // aspect >= 4:3: height is 768 and scale is H/768
    double safe_left{};      // reference units from the screen's left edge to the safe area
    double safe_width{};     // reference units across the safe area; it is as high as the screen
};

// UI-L1. At aspect >= 4:3 the height is 768 and the width 768*W/H; below it
// the width is 1024 and the height 1024*H/W. With aspect-correct rules a
// screen wider than 16:9 has a 16:9 safe area centred on it; otherwise, and
// always with retail rules, the safe area is the whole screen. A zero-sized
// viewport yields a zero scale.
[[nodiscard]] ReferenceSpace reference_space(Viewport viewport,
                                             LayoutRules rules = LayoutRules::aspect_correct) noexcept;

struct PixelRect {
    double x{};
    double y{};
    double width{};
    double height{};

    [[nodiscard]] double right() const noexcept { return x + width; }
    [[nodiscard]] double bottom() const noexcept { return y + height; }
    [[nodiscard]] double centre_x() const noexcept { return x + width / 2.0; }
    [[nodiscard]] double centre_y() const noexcept { return y + height / 2.0; }

    friend bool operator==(const PixelRect&, const PixelRect&) = default;
};

// The UI-L1 safe area in screen pixels.
[[nodiscard]] PixelRect safe_area(const ReferenceSpace& space) noexcept;

struct ReferencePoint {
    double x{};
    double y{};

    friend bool operator==(const ReferencePoint&, const ReferencePoint&) = default;
};

// UI-L5: a shell offset snaps to whole reference units (nearest, halves away
// from zero). The retail half-unit D3D9 offset is not reproduced.
[[nodiscard]] ReferencePoint snap_shell_offset(ReferencePoint offset) noexcept;

// Where a shell's origin (its lower-left corner) lands on the screen.
struct ShellPlacement {
    double left{};   // screen pixels from the left edge
    double bottom{}; // screen pixels from the top edge: the screen's bottom edge
    double scale{};  // screen pixels per shell unit

    friend bool operator==(const ShellPlacement&, const ShellPlacement&) = default;
};

// UI-L2. `shell_width` is the shell's visible extent in reference units from
// its origin to its right edge (FoC tactical: 1077, as the faceplate spans -1
// to 1077). Retail rules put the origin in the screen's lower-left corner.
// Aspect-correct rules put it in the safe area's lower-left corner, moved left
// only as far as keeps the shell's right edge on screen; a shell wider than
// the screen is placed by overwide_shell.
[[nodiscard]] ShellPlacement place_shell(const ReferenceSpace& space, double shell_width) noexcept;

// Owner question OD-1 (#205), option A: a shell wider than the screen keeps
// the retail lower-left anchor at the UI-L1 scale and clips on the right.
// Option B (scale down uniformly to fit) would replace this function alone.
[[nodiscard]] ShellPlacement overwide_shell(const ReferenceSpace& space, double shell_width) noexcept;

// UI-L2: `rect` is a shell anchor (origin bottom-left, y up) in shell units,
// moved by `shell_offset` (snapped per UI-L5); the result is in screen pixels.
[[nodiscard]] PixelRect shell_to_screen(const data::ui::ReferenceRect& rect, const ShellPlacement& shell,
                                        ReferencePoint shell_offset = {}) noexcept;

// A rect in `.rc` dialog units: top-left origin, y down.
struct RcRect {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t width{};
    std::int32_t height{};
};

// UI-L3: `.rc` units are pixels of the 1024 x 768 reference. Aspect-correct
// rules scale both axes by the UI-L1 scale, min(W/1024, H/768); retail rules
// stretch x by W/1024 and y by H/768.
[[nodiscard]] PixelRect scale_rc(const RcRect& rect, const ReferenceSpace& space) noexcept;

// UI-L4 placement presets.
enum class Placement : std::uint8_t {
    centre,
    upper_left,
    centre_left,
    lower_left,
    upper_right,
    centre_right,
    lower_right,
    upper_centre,
    lower_centre,
    full_screen,
};

// Thickness of the dialog frame on its left and top edges, in pixels.
struct FrameBorder {
    double left{};
    double top{};
};

struct DialogLayout {
    PixelRect frame;
    // Where the dialog's `.rc` origin lands and the pixels per `.rc` unit;
    // gadgets are placed from these. A full-screen dialog recentres its
    // gadgets in 1024 x 768, so its origin is not the frame's corner.
    double origin_x{};
    double origin_y{};
    double scale_x{};
    double scale_y{};
};

// Edge margins of UI-L4 as fractions of the safe area's width and height.
inline constexpr double margin_x_fraction = 0.008;
inline constexpr double margin_y_fraction = 0.006;
inline constexpr double upper_centre_extra_fraction = 0.02625;

// UI-L4. The presets apply to the safe area, which is the whole screen under
// retail rules. The dialog's size is its `.rc` size scaled per UI-L3; its
// `.rc` position is ignored. Left and top edges keep a margin of 0.008 of the
// safe width and 0.006 of the height plus the frame border, right and bottom
// edges the margin alone. Upper-right keeps no margin and no left border;
// upper-centre moves down a further 0.02625 of the height. A full-screen frame
// covers the viewport, and its gadgets keep a 1024 x 768 area centred in the
// safe area.
[[nodiscard]] DialogLayout place_dialog(const RcRect& dialog, Placement placement, const ReferenceSpace& space,
                                        FrameBorder border = {}) noexcept;

// A gadget's `.rc` rect (relative to its dialog) on screen.
[[nodiscard]] PixelRect place_gadget(const DialogLayout& dialog, const RcRect& gadget) noexcept;

// UI-F1: em height in pixels = floor(floor(96*H/600)*pt/72) for screen
// height H; a `static size` font uses pt as pixels. Non-positive sizes give 0.
[[nodiscard]] std::int32_t font_pixel_height(std::int32_t point_size, std::uint32_t screen_height,
                                             bool static_size = false) noexcept;

// The H that UI-F1 takes. Retail rules use the screen height. Aspect-correct
// rules use floor(768 * scale), the height of the uniformly scaled reference:
// the screen height at aspect >= 4:3 and less below it, so text keeps its size
// relative to the dialogs of UI-L3.
[[nodiscard]] std::uint32_t font_screen_height(const ReferenceSpace& space) noexcept;

struct FontSpec {
    std::int32_t point_size{};
    bool static_size{};
    double stretch_factor{1.0};
};

struct FontPixels {
    std::int32_t em_height{};     // UI-F1
    std::int32_t glyph_height{};  // UI-F2: em height x Stretch_Factor, rounded
    std::int32_t width_em{};      // em height whose average glyph width is kept
};

// UI-F1 and UI-F2. With Stretch_Factor != 1 the glyphs are em x factor high
// (rounded, halves away from zero) while the average width stays that of the
// unstretched face at `em_height`, which `width_em` names.
[[nodiscard]] FontPixels font_pixels(const FontSpec& font, std::uint32_t screen_height) noexcept;

} // namespace eawr::presentation::ui
