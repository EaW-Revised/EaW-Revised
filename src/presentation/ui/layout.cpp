#include "eawr/presentation/ui/layout.hpp"

#include <algorithm>
#include <cmath>

namespace eawr::presentation::ui {

namespace {

struct RcScale {
    double x{};
    double y{};
};

// UI-L3: pixels per `.rc` unit on each axis.
[[nodiscard]] RcScale rc_scale(const ReferenceSpace& space) noexcept {
    if (space.rules == LayoutRules::retail) {
        return {space.viewport.width / reference_width, space.viewport.height / reference_height};
    }
    return {space.scale, space.scale};
}

} // namespace

ReferenceSpace reference_space(const Viewport viewport, const LayoutRules rules) noexcept {
    ReferenceSpace space;
    space.viewport = viewport;
    space.rules = rules;
    if (viewport.width == 0U || viewport.height == 0U) return space;
    const double width = viewport.width;
    const double height = viewport.height;
    const auto wide = static_cast<std::uint64_t>(viewport.width);
    const auto high = static_cast<std::uint64_t>(viewport.height);
    // Exact 4:3 test in integers: W/H >= 4/3.
    space.height_based = wide * 3U >= high * 4U;
    if (space.height_based) {
        space.height = reference_height;
        space.width = reference_height * width / height;
        space.scale = height / reference_height;
    } else {
        space.width = reference_width;
        space.height = reference_width * height / width;
        space.scale = width / reference_width;
    }
    space.safe_width = space.width;
    // Exact 16:9 test in integers: W/H > 16/9.
    if (rules == LayoutRules::aspect_correct && wide * safe_area_aspect_height > high * safe_area_aspect_width) {
        space.safe_width = safe_area_width;
        space.safe_left = (space.width - safe_area_width) / 2.0;
    }
    return space;
}

PixelRect safe_area(const ReferenceSpace& space) noexcept {
    return PixelRect{space.safe_left * space.scale, 0.0, space.safe_width * space.scale,
                     static_cast<double>(space.viewport.height)};
}

ReferencePoint snap_shell_offset(const ReferencePoint offset) noexcept {
    return {std::round(offset.x), std::round(offset.y)};
}

ShellPlacement place_shell(const ReferenceSpace& space, const double shell_width) noexcept {
    if (space.rules == LayoutRules::retail) {
        return {0.0, static_cast<double>(space.viewport.height), space.scale};
    }
    if (shell_width > space.width) return overwide_shell(space, shell_width);
    const double left = std::min(space.safe_left, space.width - shell_width);
    return {left * space.scale, static_cast<double>(space.viewport.height), space.scale};
}

ShellPlacement overwide_shell(const ReferenceSpace& space, double /*shell_width*/) noexcept {
    return {0.0, static_cast<double>(space.viewport.height), space.scale};
}

PixelRect shell_to_screen(const data::ui::ReferenceRect& rect, const ShellPlacement& shell,
                          const ReferencePoint shell_offset) noexcept {
    const ReferencePoint origin = snap_shell_offset(shell_offset);
    const double left = origin.x + rect.x;
    const double top = origin.y + rect.y + rect.height; // y up in the shell
    return PixelRect{
        shell.left + left * shell.scale,
        shell.bottom - top * shell.scale,
        static_cast<double>(rect.width) * shell.scale,
        static_cast<double>(rect.height) * shell.scale,
    };
}

PixelRect scale_rc(const RcRect& rect, const ReferenceSpace& space) noexcept {
    const RcScale scale = rc_scale(space);
    return PixelRect{rect.x * scale.x, rect.y * scale.y, rect.width * scale.x, rect.height * scale.y};
}

DialogLayout place_dialog(const RcRect& dialog, const Placement placement, const ReferenceSpace& space,
                          const FrameBorder border) noexcept {
    DialogLayout layout;
    const RcScale scale = rc_scale(space);
    layout.scale_x = scale.x;
    layout.scale_y = scale.y;
    const PixelRect area = safe_area(space);
    const double screen_height = space.viewport.height;
    if (placement == Placement::full_screen) {
        layout.frame = PixelRect{0.0, 0.0, static_cast<double>(space.viewport.width), screen_height};
        // The 1024 x 768 gadget area, centred in the safe area.
        const double area_x = area.x + (area.width - reference_width * scale.x) / 2.0;
        const double area_y = (screen_height - reference_height * scale.y) / 2.0;
        layout.origin_x = area_x + (reference_width - dialog.width) / 2.0 * scale.x;
        layout.origin_y = area_y + (reference_height - dialog.height) / 2.0 * scale.y;
        return layout;
    }
    const double width = dialog.width * scale.x;
    const double height = dialog.height * scale.y;
    const double margin_x = margin_x_fraction * area.width;
    const double margin_y = margin_y_fraction * screen_height;
    const double left = area.x + margin_x + border.left;
    const double right = area.right() - width - margin_x;
    const double top = margin_y + border.top;
    const double bottom = screen_height - height - margin_y;
    const double centre_x = area.x + (area.width - width) / 2.0;
    const double centre_y = (screen_height - height) / 2.0;
    double x = centre_x;
    double y = centre_y;
    switch (placement) {
    case Placement::centre: break;
    case Placement::upper_left: x = left; y = top; break;
    case Placement::centre_left: x = left; break;
    case Placement::lower_left: x = left; y = bottom; break;
    case Placement::upper_right: x = area.right() - width; y = border.top; break;
    case Placement::centre_right: x = right; break;
    case Placement::lower_right: x = right; y = bottom; break;
    case Placement::upper_centre: y = top + upper_centre_extra_fraction * screen_height; break;
    case Placement::lower_centre: y = bottom; break;
    case Placement::full_screen: break;
    }
    layout.frame = PixelRect{x, y, width, height};
    layout.origin_x = x;
    layout.origin_y = y;
    return layout;
}

PixelRect place_gadget(const DialogLayout& dialog, const RcRect& gadget) noexcept {
    return PixelRect{
        dialog.origin_x + gadget.x * dialog.scale_x,
        dialog.origin_y + gadget.y * dialog.scale_y,
        gadget.width * dialog.scale_x,
        gadget.height * dialog.scale_y,
    };
}

std::int32_t font_pixel_height(const std::int32_t point_size, const std::uint32_t screen_height,
                               const bool static_size) noexcept {
    if (point_size <= 0) return 0;
    if (static_size) return point_size;
    // Integer division floors both steps for non-negative values.
    const std::int64_t dots_per_inch = static_cast<std::int64_t>(96) * screen_height / 600;
    return static_cast<std::int32_t>(dots_per_inch * point_size / 72);
}

std::uint32_t font_screen_height(const ReferenceSpace& space) noexcept {
    if (space.viewport.width == 0U || space.viewport.height == 0U) return 0U;
    if (space.rules == LayoutRules::retail || space.height_based) return space.viewport.height;
    // floor(768 * W/1024) in integers.
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(space.viewport.width) * 3U / 4U);
}

FontPixels font_pixels(const FontSpec& font, const std::uint32_t screen_height) noexcept {
    FontPixels result;
    result.em_height = font_pixel_height(font.point_size, screen_height, font.static_size);
    result.width_em = result.em_height;
    result.glyph_height = font.stretch_factor == 1.0
        ? result.em_height
        : static_cast<std::int32_t>(std::lround(result.em_height * font.stretch_factor));
    return result;
}

} // namespace eawr::presentation::ui
