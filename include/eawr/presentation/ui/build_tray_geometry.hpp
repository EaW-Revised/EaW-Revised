#pragma once

#include "eawr/data/ui/shell_anchors.hpp"
#include "eawr/presentation/ui/build_tray_model.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// Galactic build tray geometry (docs/ui/galactic-build-tray.md, GBT-02, GBT-03, GBT-06). Engine-free and
// presentation only: it turns the packed rows of the grouping model into rectangles for the panel, the
// group headers, the entries and the dividers, and counts what the bar omits for the handle. It never
// changes the choice list or the bar's own mapping.
namespace eawr::presentation::ui::build_tray {

struct Box final {
    float x{}, y{}, width{}, height{};
    [[nodiscard]] float right() const noexcept { return x + width; }
    [[nodiscard]] float bottom() const noexcept { return y + height; }
    friend bool operator==(const Box&, const Box&) = default;
};

struct PlacedEntry final {
    std::size_t choice{};  // index into the choice list
    Box box;
};

struct PlacedHeader final {
    std::size_t group{};   // index into Packed::groups
    Box box;
};

struct TrayGeometry final {
    Box panel;
    std::vector<PlacedEntry> entries;
    std::vector<PlacedHeader> headers;
    // Zero width: centred on the gap between two groups of one row, the full height of the row. The
    // caller sets the width from its divider texture.
    std::vector<Box> dividers;
};

// GBT-06: the panel's left edge is `left`, its bottom edge sits on `bottom` and it grows upward. Rows are
// placed top to bottom in the order of `packed.rows`; `metrics` must be the ones the rows were packed with.
[[nodiscard]] TrayGeometry place(const Packed& packed, const Metrics& metrics, float left, float bottom);

// GBT-02: the number of choices the bar omits, which the handle shows. `slots` is the bar's slot mapping
// (one entry per slot, holding the choice it shows or nothing); `choice_count` the length of the choice list.
// The bar never shows a choice twice, so this is the list length minus the slots in use.
[[nodiscard]] std::size_t omitted_by_bar(std::size_t choice_count,
    std::span<const std::optional<std::size_t>> slots) noexcept;

// GBT-14 (owner pick B, 2026-11-19): how far a production queue strip (the space or the land one) moves up, in
// pixels (0 or more), so that neither the open tray nor the handle covers it. `strip` is the strip's box where the bar
// puts it; `panel` the open tray (none while it is closed); `handle` the handle (none when it is not shown); `gap` the
// space kept between the strip and what it clears. A strip that meets neither stays where it is. A strip the open tray
// would cover moves above the tray's top; a strip that would cover the handle moves above the handle's top. The strip
// never moves past the top of the view (`view_top`).
[[nodiscard]] float queue_strip_lift(const Box& strip, const std::optional<Box>& panel, const std::optional<Box>& handle,
    float gap, float view_top = 0.0F) noexcept;

// GBT-10, Back_Source Bar: the texture coordinates a shell mesh gives a point, as the affine map of the
// triangle that holds the point. The panel's back and frame are drawn with these coordinates, so they show the
// bar's own art and colour from the same shell data the bar is drawn from.
struct UvMap final {
    double x0{}, y0{};          // the sampled point, shell units (y up)
    double u0{}, v0{};          // its texture coordinates
    double du_dx{}, du_dy{}, dv_dx{}, dv_dy{};
    [[nodiscard]] std::pair<double, double> at(double x, double y) const noexcept {
        return {u0 + du_dx * (x - x0) + du_dy * (y - y0), v0 + dv_dx * (x - x0) + dv_dy * (y - y0)};
    }
};

// The map of the first triangle of `triangles` that holds (`x`, `y`) (shell units, y up; edges count); none when
// no triangle holds it or each that does is degenerate.
[[nodiscard]] std::optional<UvMap> uv_map_at(std::span<const data::ui::ShellTriangle> triangles, double x, double y) noexcept;

} // namespace eawr::presentation::ui::build_tray
