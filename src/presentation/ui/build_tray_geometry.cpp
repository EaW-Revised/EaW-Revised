#include "eawr/presentation/ui/build_tray_geometry.hpp"

#include <algorithm>
#include <cmath>

namespace eawr::presentation::ui::build_tray {

namespace {

float span_width(const Metrics& metrics, const std::size_t count) noexcept {
    return count == 0 ? 0.0F
        : static_cast<float>(count) * metrics.entry_width + static_cast<float>(count - 1) * metrics.entry_gap;
}

} // namespace

TrayGeometry place(const Packed& packed, const Metrics& metrics, const float left, const float bottom) {
    TrayGeometry geometry;
    geometry.panel = {left, bottom - packed.height, metrics.panel_width, packed.height};
    float row_top = geometry.panel.y + metrics.padding + metrics.top_reserve;
    for (const auto& row : packed.rows) {
        for (std::size_t position = 0; position < row.runs.size(); ++position) {
            const auto& run = row.runs[position];
            const auto& group = packed.groups[run.group];
            const float x = left + metrics.padding + run.x;
            const float width = span_width(metrics, run.count);
            if (run.header) {
                geometry.headers.push_back({run.group,
                    {x, row_top, std::max(width, group.min_width), metrics.header_height}});
            }
            for (std::size_t index = 0; index < run.count; ++index) {
                geometry.entries.push_back({group.choices[run.first + index],
                    {x + static_cast<float>(index) * (metrics.entry_width + metrics.entry_gap),
                        row_top + metrics.header_height, metrics.entry_width, metrics.entry_height}});
            }
            if (position > 0) {
                const auto& before = row.runs[position - 1];
                const float before_end = left + metrics.padding + before.x
                    + std::max(span_width(metrics, before.count),
                        before.header ? packed.groups[before.group].min_width : 0.0F);
                geometry.dividers.push_back({(before_end + x) * 0.5F, row_top, 0.0F, metrics.row_height()});
            }
        }
        row_top += metrics.row_height() + metrics.row_gap;
    }
    return geometry;
}

std::size_t omitted_by_bar(const std::size_t choice_count,
    const std::span<const std::optional<std::size_t>> slots) noexcept {
    const auto shown = static_cast<std::size_t>(std::count_if(slots.begin(), slots.end(),
        [](const auto& slot) { return slot.has_value(); }));
    return choice_count > shown ? choice_count - shown : 0;
}

float queue_strip_lift(const Box& strip, const std::optional<Box>& panel, const std::optional<Box>& handle,
    const float gap, const float view_top) noexcept {
    const auto meets = [](const Box& a, const Box& b) {
        return a.width > 0.0F && a.height > 0.0F && b.width > 0.0F && b.height > 0.0F
            && a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
    };
    if (strip.width <= 0.0F || strip.height <= 0.0F) return 0.0F;
    // The highest top the strip has to clear: the tray (which sits above the handle) when they meet, else the handle.
    std::optional<float> clear_top;
    for (const auto& other : {panel, handle}) {
        if (!other || !meets(strip, *other)) continue;
        clear_top = clear_top ? std::min(*clear_top, other->y) : other->y;
    }
    // Lifted above the tray, the strip may meet the handle no more (the handle is below the tray), and lifted above the
    // handle it may now meet the open tray: settle on the top of everything it meets after the move.
    for (int pass = 0; pass < 2 && clear_top; ++pass) {
        const Box moved{strip.x, *clear_top - gap - strip.height, strip.width, strip.height};
        for (const auto& other : {panel, handle})
            if (other && meets(moved, *other)) clear_top = std::min(*clear_top, other->y);
    }
    if (!clear_top) return 0.0F;
    const float target = std::max(view_top, *clear_top - gap - strip.height);
    return std::max(0.0F, strip.y - target);
}

std::optional<UvMap> uv_map_at(const std::span<const data::ui::ShellTriangle> triangles, const double x, const double y) noexcept {
    for (const auto& triangle : triangles) {
        const auto& [a, b, c] = triangle.vertices;
        const double ax = a.position.x, ay = a.position.y;
        const double bx = b.position.x - ax, by = b.position.y - ay;
        const double cx = c.position.x - ax, cy = c.position.y - ay;
        const double det = bx * cy - by * cx;
        if (std::abs(det) < 1e-9) continue;
        // Barycentric weights of b and c; the point is inside (edges included) when both and their sum are in [0, 1].
        const double px = x - ax, py = y - ay;
        const double s = (px * cy - py * cx) / det;
        const double t = (bx * py - by * px) / det;
        constexpr double edge = 1e-6;
        if (s < -edge || t < -edge || s + t > 1.0 + edge) continue;
        const double bu = b.uv.x - a.uv.x, bv = b.uv.y - a.uv.y;
        const double cu = c.uv.x - a.uv.x, cv = c.uv.y - a.uv.y;
        UvMap map;
        map.x0 = x;
        map.y0 = y;
        map.u0 = a.uv.x + s * bu + t * cu;
        map.v0 = a.uv.y + s * bv + t * cv;
        // d(s)/dx = cy/det, d(s)/dy = -cx/det, d(t)/dx = -by/det, d(t)/dy = bx/det.
        map.du_dx = (bu * cy - cu * by) / det;
        map.du_dy = (cu * bx - bu * cx) / det;
        map.dv_dx = (bv * cy - cv * by) / det;
        map.dv_dy = (cv * bx - bv * cx) / det;
        return map;
    }
    return std::nullopt;
}

} // namespace eawr::presentation::ui::build_tray
