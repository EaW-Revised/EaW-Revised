// Galactic build tray geometry (docs/ui/galactic-build-tray.md, GBT-02, GBT-06): the rectangles the
// packed rows turn into, and the count the handle shows. Synthetic data only.

#include "eawr/presentation/ui/build_tray_geometry.hpp"
#include "ui_test_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <optional>
#include <set>
#include <vector>

namespace {
namespace tray = eawr::presentation::ui::build_tray;
using eawr::test::ui::expect;

tray::Metrics metrics() {
    tray::Metrics value;
    value.entry_width = 40.0F;
    value.entry_height = 40.0F;
    value.header_height = 10.0F;
    value.entry_gap = 2.0F;
    value.group_gap = 12.0F;
    value.row_gap = 4.0F;
    value.padding = 6.0F;
    value.panel_width = 6.0F * 2.0F + 5.0F * 40.0F + 4.0F * 2.0F; // five entries per line
    return value;
}

tray::Group group(const tray::GroupKind kind, const std::string& name, const std::size_t first, const std::size_t count) {
    tray::Group value;
    value.kind = kind;
    value.name = name;
    for (std::size_t index = 0; index < count; ++index) value.choices.push_back(first + index);
    value.min_width = 20.0F;
    return value;
}

bool near(const float a, const float b) { return std::fabs(a - b) < 0.001F; }

void test_entries_sit_inside_the_panel_on_their_rows() {
    const auto size = metrics();
    // Two small groups share the first row; a third needs its own; buildings take the bottom row.
    std::vector<tray::Group> groups;
    groups.push_back(group(tray::GroupKind::category, "A", 0, 2));
    groups.push_back(group(tray::GroupKind::category, "B", 2, 1));
    groups.push_back(group(tray::GroupKind::category, "C", 3, 4));
    groups.push_back(group(tray::GroupKind::buildings, "Buildings", 7, 2));
    const auto packed = tray::pack(groups, size, {});
    const auto geometry = tray::place(packed, size, 100.0F, 500.0F);

    expect(near(geometry.panel.x, 100.0F) && near(geometry.panel.bottom(), 500.0F)
            && near(geometry.panel.width, size.panel_width) && near(geometry.panel.height, packed.height),
        "the panel's bottom edge sits on the given line and it grows upward");
    expect(geometry.entries.size() == 9, "every choice has exactly one entry box");
    std::set<std::size_t> seen;
    for (const auto& entry : geometry.entries) {
        seen.insert(entry.choice);
        expect(entry.box.x >= geometry.panel.x + size.padding - 0.001F
                && entry.box.right() <= geometry.panel.right() - size.padding + 0.001F
                && entry.box.y >= geometry.panel.y && entry.box.bottom() <= geometry.panel.bottom(),
            "an entry lies inside the panel padding");
    }
    expect(seen.size() == 9, "no choice is placed twice");
    expect(geometry.headers.size() == packed.groups.size(), "each group has one header when none wraps");
    // The entries of the first group start at the first row's entry line, left to right.
    const auto& first = geometry.entries[0];
    const auto& second = geometry.entries[1];
    expect(near(first.box.x, 100.0F + size.padding) && near(second.box.x, first.box.x + size.entry_width + size.entry_gap)
            && near(first.box.y, geometry.panel.y + size.padding + size.header_height),
        "entries step by one width and one gap, below the header");
    // Group B shares the row with A: one divider centred in the gap between them.
    expect(geometry.dividers.size() == 1, "one divider between the two groups of the first row");
    if (!geometry.dividers.empty()) {
        const auto& divider = geometry.dividers.front();
        expect(divider.width == 0.0F && near(divider.height, size.row_height())
                && divider.x > second.box.right() && divider.x < geometry.entries[2].box.x,
            "the divider sits in the gap, one row tall, with no width of its own");
    }
    // Rows step by the row height plus the row gap.
    const auto& third_group_entry = geometry.entries[3];
    expect(near(third_group_entry.box.y, first.box.y + size.row_height() + size.row_gap),
        "the next row starts one row height and one gap lower");
}

void test_a_wrapped_group_has_one_header() {
    auto size = metrics();
    std::vector<tray::Group> groups;
    groups.push_back(group(tray::GroupKind::category, "Wide", 0, 7)); // wider than one line of five
    const auto packed = tray::pack(groups, size, {});
    const auto geometry = tray::place(packed, size, 0.0F, 300.0F);
    expect(packed.rows.size() == 2 && geometry.entries.size() == 7, "a group wider than a row wraps onto two rows");
    expect(geometry.headers.size() == 1, "a wrapped group keeps a single header, on its first row");
    expect(geometry.dividers.empty(), "a lone group has no divider");
}

void test_the_handle_count() {
    std::vector<std::optional<std::size_t>> slots(4);
    slots[0] = 0; slots[1] = 1; slots[3] = 2;
    expect(tray::omitted_by_bar(3, slots) == 0, "every choice on the bar: nothing omitted");
    expect(tray::omitted_by_bar(7, slots) == 4, "seven choices, three on the bar: four omitted");
    expect(tray::omitted_by_bar(2, slots) == 0, "never negative");
    expect(tray::omitted_by_bar(5, {}) == 5, "no bar slots at all omits everything");
}

// GBT-14 (owner pick B): a queue strip the open tray would cover moves above the tray; no strip covers the handle.
// Boxes from the Remake capture at 1080p: space strip 466..833 x 813..903, handle 434..501 x 887..915, the open
// tray 434..1182 x 461..887; the land strip sits at the bar's right end, 1434..1804.
void test_the_queue_strip_lift() {
    const tray::Box space{466, 813, 367, 90};
    const tray::Box land{1434, 813, 370, 90};
    const tray::Box handle{434, 887, 67, 28};
    const tray::Box panel{434, 461, 748, 426};
    constexpr float gap = 4.0F;
    const float open = tray::queue_strip_lift(space, panel, handle, gap);
    expect(near(open, 813.0F - (461.0F - gap - 90.0F)), "the open tray covers the space strip: it moves to just above the tray");
    const tray::Box lifted{space.x, space.y - open, space.width, space.height};
    expect(lifted.bottom() <= panel.y - gap + 0.001F, "lifted, it clears the tray");
    const float closed = tray::queue_strip_lift(space, std::nullopt, handle, gap);
    expect(near(closed, 903.0F - (887.0F - gap)), "with the tray closed the strip still never covers the handle");
    expect(near(tray::queue_strip_lift(land, panel, handle, gap), 0.0F), "a strip the tray does not reach stays where it is");
    const tray::Box wide{434, 461, 1200, 426};
    expect(tray::queue_strip_lift(land, wide, handle, gap) > 0.0F, "a tray widened over the land strip lifts it too");
    expect(near(tray::queue_strip_lift(space, std::nullopt, std::nullopt, gap), 0.0F), "no tray and no handle: no lift");
    const tray::Box tall{434, 20, 748, 867};
    expect(near(tray::queue_strip_lift(space, tall, handle, gap, 0.0F), 813.0F), "the strip never leaves the top of the view");
    expect(near(tray::queue_strip_lift({}, panel, handle, gap), 0.0F), "an empty strip (nothing queued) does not move");
}

// GBT-07: the close icon's strip at the top of the panel is kept free: the panel grows by it and the rows move down.
void test_the_close_icon_strip() {
    auto size = metrics();
    std::vector<tray::Group> groups{group(tray::GroupKind::category, "A", 0, 3)};
    const auto plain = tray::place(tray::pack(groups, size, {}), size, 0.0F, 500.0F);
    size.top_reserve = 14.0F;
    const auto packed = tray::pack(groups, size, {});
    const auto strip = tray::place(packed, size, 0.0F, 500.0F);
    expect(near(strip.panel.height, plain.panel.height + 14.0F) && near(strip.panel.bottom(), 500.0F),
        "the panel is taller by the strip and still sits on the bar");
    expect(near(strip.headers.front().box.y, strip.panel.y + size.padding + 14.0F)
            && near(strip.entries.front().box.y - strip.panel.y, plain.entries.front().box.y - plain.panel.y + 14.0F),
        "the first header and entries start below the strip");
}

// Width_Mode Auto (GBT-06): the rows fill to the budget and the free height at the bar's width first; the panel widens,
// in whole entry pitches, only when they cannot hold every group without misc. The view's share caps it.
void test_the_automatic_width() {
    auto size = metrics();
    const float pitch = size.entry_width + size.entry_gap;
    const float bar = size.panel_width; // five entries per line
    constexpr float tall = 10000.0F;
    std::vector<tray::Group> many;
    for (std::size_t index = 0; index < 6; ++index)
        many.push_back(group(tray::GroupKind::category, std::string(1, static_cast<char>('A' + index)), index * 5, 5));
    tray::PackOptions seven;
    seven.max_rows = 7;
    expect(near(tray::automatic_width(many, size, seven, bar, 2000.0F, tall), bar),
        "six full groups fit a 7-row budget at the bar's width: the rows fill first and the panel keeps the bar's width");

    tray::PackOptions two;
    two.max_rows = 2;
    const float wide = tray::automatic_width(many, size, two, bar, 2000.0F, tall);
    expect(wide > bar && near(std::round((wide - bar) / pitch) * pitch, wide - bar),
        "more groups than the budget holds widen the panel in entry pitches");
    auto at = size;
    at.panel_width = wide;
    expect(!tray::pack(many, at, two).misc_mode, "at that width every group keeps its own place: no misc");
    at.panel_width = wide - pitch;
    expect(tray::pack(many, at, two).misc_mode, "one pitch narrower misc is needed: it is the narrowest such width");

    // The free height counts as much as the budget: a short view widens the panel instead of folding into misc.
    auto full = size;
    full.panel_width = bar;
    const float six_rows = tray::pack(many, full, seven).height;
    const float short_view = tray::automatic_width(many, size, seven, bar, 2000.0F, six_rows - 1.0F);
    at.panel_width = short_view;
    expect(short_view > bar && tray::pack(many, at, seven).height <= six_rows - 1.0F,
        "rows that would not fit the free height widen the panel until they do");

    expect(near(tray::automatic_width(many, size, two, bar, bar + pitch, tall), bar + pitch), "the cap stops the search");
    expect(near(tray::automatic_width(many, size, two, bar, bar * 0.5F, tall), bar), "a cap below the bar's width reads as the bar's width");
}

eawr::data::ui::ShellTriangle triangle(const float ax, const float ay, const float bx, const float by, const float cx, const float cy,
    const float scale) {
    // Texture coordinates follow the shell position: u = x * scale + 0.25, v = 1 - y * scale.
    eawr::data::ui::ShellTriangle value;
    const float points[3][2]{{ax, ay}, {bx, by}, {cx, cy}};
    for (std::size_t index = 0; index < 3; ++index) {
        value.vertices[index].position = {points[index][0], points[index][1], 0.0F};
        value.vertices[index].uv = {points[index][0] * scale + 0.25F, 1.0F - points[index][1] * scale};
    }
    return value;
}

// Back_Source Bar (GBT-10): the texture coordinates the bar's own shell mesh gives a point, and the map around it.
void test_the_bar_art_map() {
    // A quad from (0, 0) to (100, 50) as two triangles, mapped at 0.01 per shell unit.
    const std::vector<eawr::data::ui::ShellTriangle> quad{triangle(0, 0, 100, 0, 100, 50, 0.01F), triangle(0, 0, 100, 50, 0, 50, 0.01F)};
    const auto map = tray::uv_map_at(quad, 30.0, 40.0);
    expect(map.has_value(), "a point inside the quad has a map");
    if (map) {
        const auto [u, v] = map->at(30.0, 40.0);
        expect(std::fabs(u - 0.55) < 1e-6 && std::fabs(v - 0.6) < 1e-6, "the point's own coordinates");
        expect(std::fabs(map->du_dx - 0.01) < 1e-6 && std::fabs(map->du_dy) < 1e-6 && std::fabs(map->dv_dx) < 1e-6
                && std::fabs(map->dv_dy + 0.01) < 1e-6,
            "the map's slope is the mesh's: u grows to the right, v grows downward");
        const auto [far_u, far_v] = map->at(150.0, -20.0);
        expect(std::fabs(far_u - 1.75) < 1e-6 && std::fabs(far_v - 1.2) < 1e-6, "the map extends past the triangle (repeating coordinates)");
        expect(map->x_min == 0.0 && map->x_max == 100.0, "the map names its triangle's x span, where it holds (#7952)");
        expect(map->y_min == 0.0 && map->y_max == 50.0, "and its y span");
    }
    expect(tray::uv_map_at(quad, 100.0, 50.0).has_value(), "a corner counts as inside");
    expect(!tray::uv_map_at(quad, 120.0, 10.0) && !tray::uv_map_at(quad, 50.0, -1.0), "a point outside every triangle has none");
    const std::vector<eawr::data::ui::ShellTriangle> flat{triangle(0, 0, 10, 10, 20, 20, 0.01F)};
    expect(!tray::uv_map_at(flat, 10.0, 10.0), "a degenerate triangle gives no map");
    expect(!tray::uv_map_at({}, 0.0, 0.0), "no triangles, no map");
}

// #7952: the backdrop repeats the sampled span; it never stretches one sample over a wider band.
void test_the_band_pieces() {
    const auto pieces = tray::band_pieces(10.0F, 250.0F, 100.0F);
    expect(pieces.size() == 3, "a 250 px band of a 100 px sample is three pieces");
    if (pieces.size() == 3) {
        expect(pieces[0].x == 10.0F && pieces[0].width == 100.0F && pieces[1].x == 110.0F && pieces[1].width == 100.0F,
            "whole pieces side by side from the band's left");
        expect(pieces[2].x == 210.0F && pieces[2].width == 50.0F, "the last piece is the remainder");
    }
    expect(tray::band_pieces(0.0F, 80.0F, 100.0F).size() == 1, "a band narrower than the sample is one piece");
    expect(tray::band_pieces(0.0F, 200.0F, 100.0F).size() == 2, "an exact multiple leaves no sliver");
    // A sliver triangle in mod data: never a quad per fraction of a pixel, and never a loop that cannot advance.
    for (const float tiny : {0.5F, 1e-4F, 1e-9F}) {
        const auto one = tray::band_pieces(1000.0F, 1500.0F, tiny);
        expect(one.size() == 1 && one[0].x == 1000.0F && one[0].width == 1500.0F, "a sample under a pixel is stretched over the band");
    }
    const auto many = tray::band_pieces(1000.0F, 1500.0F, 1.0F);
    expect(many.size() == 1500 && std::fabs(many.back().x - 2499.0F) < 0.01F, "one-pixel pieces step without drift");
    expect(tray::band_pieces(0.0F, 0.0F, 100.0F).empty() && tray::band_pieces(0.0F, 50.0F, 0.0F).empty(), "nothing to draw");
}

// #7952 (owner, 2026-11-19): the tray ends at the planet info card, and the share and the view stay upper bounds.
void test_the_widest_panel() {
    // Remake at 2440 x 1372: the bar starts at 455, the card at about 1430.
    expect(std::fabs(tray::widest_panel(2440.0F, 455.0F, 0.6F, 1430.0F) - 975.0F) < 0.01F, "the card's left edge ends the tray");
    expect(std::fabs(tray::widest_panel(2440.0F, 455.0F, 0.3F, 1430.0F) - 732.0F) < 0.01F, "a smaller share still limits it");
    expect(std::fabs(tray::widest_panel(2440.0F, 455.0F, 0.6F, std::nullopt) - 1464.0F) < 0.01F, "no card: the share");
    expect(std::fabs(tray::widest_panel(1000.0F, 700.0F, 0.6F, std::nullopt) - 300.0F) < 0.01F, "never past the view's right edge");
    expect(std::fabs(tray::widest_panel(2440.0F, 455.0F, 0.6F, 300.0F) - 1464.0F) < 0.01F, "a card left of the tray does not limit it");
    expect(std::fabs(tray::widest_panel(1000.0F, 1200.0F, 0.6F, std::nullopt) - 0.0F) < 0.01F, "never negative");
}

bool overlaps(const tray::Box& a, const tray::Box& b) {
    return a.x < b.x + b.width - 0.001F && b.x < a.x + a.width - 0.001F && a.y < b.bottom() - 0.001F && b.y < a.bottom() - 0.001F;
}

// #8159, GBT-06: every header sits ABOVE the first icon of its own group (never beside it, never below it), off every
// icon frame, nearer to its own icons than to the row above (the gap over the label is at least twice the gap under it),
// and inside the panel. Checked on every group of a tray, so the last group (buildings) and a hero group are covered like
// the first.
void expect_every_header_above_its_icons(const tray::Packed& packed, const tray::TrayGeometry& geometry,
    const tray::Metrics& size, const float least_gap_under) {
    expect(!geometry.headers.empty(), "the tray has headers");
    for (const auto& header : geometry.headers) {
        const auto& group = packed.groups[header.group];
        const auto first = std::find_if(geometry.entries.begin(), geometry.entries.end(),
            [&](const tray::PlacedEntry& entry) { return entry.choice == group.choices.front(); });
        expect(first != geometry.entries.end(), "a headed group has its first entry placed");
        if (first == geometry.entries.end()) continue;
        expect(header.box.bottom() <= first->box.y + 0.001F, "the header ends above the first icon of its group");
        expect(header.box.y < first->box.y, "the header starts above the first icon of its group");
        expect(first->box.y - header.box.bottom() >= least_gap_under - 0.001F,
            "the label keeps its lift: the gap under it is at least the lift the metrics ask for (not touching its icons)");
        expect(header.box.y >= geometry.panel.y - 0.001F, "a lifted label stays inside the panel");
        for (const auto& entry : geometry.entries) expect(!overlaps(header.box, entry.box), "no header label intersects an icon frame");
        // The nearest icon bottom above the header, in its own columns: the previous row's.
        float above = geometry.panel.y + size.padding;
        bool has_row_above = false;
        for (const auto& entry : geometry.entries) {
            if (entry.box.bottom() > header.box.y + 0.001F || entry.box.right() <= header.box.x || entry.box.x >= header.box.right()) continue;
            above = std::max(above, entry.box.bottom());
            has_row_above = true;
        }
        if (has_row_above)
            expect(header.box.y - above >= 2.0F * (first->box.y - header.box.bottom()) - 0.001F,
                "the label is nearer to its own icons than to the row above");
    }
}

// #8031/#8159: a header label sits fully in the gap above its row on the space tray's and the ground tray's group
// shapes alike, and each row that starts a category sits category_gap further from the row above than a plain row.
void test_header_labels_sit_above_their_row_and_off_the_frames() {
    auto size = metrics();
    size.top_reserve = 14.0F;
    size.header_lift = 3.0F;
    size.category_gap = 9.0F;
    // The space tray: several categories over several rows, a wrapped group, buildings; the ground tray: groups with
    // heroes and a wrapped buildings group, then a one-row tray and a tray with a lone building row.
    std::vector<std::vector<tray::Group>> trays(4);
    trays[0].push_back(group(tray::GroupKind::category, "Fighters", 0, 2));
    trays[0].push_back(group(tray::GroupKind::category, "Bombers", 2, 1));
    trays[0].push_back(group(tray::GroupKind::category, "Corvettes", 3, 7));
    trays[0].push_back(group(tray::GroupKind::category, "Frigates", 10, 2));
    trays[0].push_back(group(tray::GroupKind::buildings, "Buildings", 12, 2));
    trays[1].push_back(group(tray::GroupKind::category, "Infantry", 0, 2));
    trays[1].push_back(group(tray::GroupKind::category, "Vehicle", 2, 5));
    trays[1].push_back(group(tray::GroupKind::category, "LandHero", 7, 2));
    trays[1].push_back(group(tray::GroupKind::buildings, "Buildings", 9, 7));
    trays[2].push_back(group(tray::GroupKind::category, "Infantry", 0, 2));
    trays[3].push_back(group(tray::GroupKind::buildings, "Buildings", 0, 3));
    for (const auto& groups : trays) {
        auto flat = size;
        flat.header_lift = flat.category_gap = 0.0F;
        const auto before = tray::pack(groups, flat, {});
        const auto packed = tray::pack(groups, size, {});
        const auto geometry = tray::place(packed, size, 100.0F, 900.0F);
        expect_every_header_above_its_icons(packed, geometry, size, size.header_lift);
        std::size_t headed_after_first{};
        for (std::size_t row = 1; row < packed.rows.size(); ++row) headed_after_first += tray::has_header(packed.rows[row]);
        expect(near(packed.height - before.height, static_cast<float>(headed_after_first) * size.category_gap),
            "the tray grows by the category gap once per category row after the first");
        float lowest = 0.0F;
        for (const auto& entry : geometry.entries) lowest = std::max(lowest, entry.box.bottom());
        expect(near(lowest + size.padding, geometry.panel.bottom()), "the panel still ends one padding under its last row");
    }
    // The first row's label never reaches the close icon's band: the reserve puts the first row one row gap under it, so a
    // lift larger than that (a large header font) is cut to the row gap and the label's top stays under the band.
    auto tall = size;
    tall.header_lift = 3.0F * size.row_gap;
    tall.category_gap = 3.0F * tall.header_lift;
    for (const auto& groups : trays) {
        const auto packed = tray::pack(groups, tall, {});
        const auto geometry = tray::place(packed, tall, 100.0F, 900.0F);
        expect_every_header_above_its_icons(packed, geometry, tall, tall.row_gap);
        const float band_bottom = geometry.panel.y + tall.padding + tall.top_reserve - tall.row_gap;
        for (const auto& header : geometry.headers)
            expect(header.box.y >= band_bottom - 0.001F, "no label reaches into the close icon's band above the first row");
    }
    // No lift and no gap asked for: nothing moves (a skin or test that sets neither is unchanged).
    auto none = metrics();
    const auto groups = trays[0];
    const auto plain = tray::place(tray::pack(groups, none, {}), none, 100.0F, 900.0F);
    expect(near(plain.headers.front().box.y, plain.entries.front().box.y - none.header_height),
        "without a lift the header box is the strip right above its icons, as before");
}

} // namespace

int main() {
    test_the_band_pieces();
    test_the_widest_panel();
    test_entries_sit_inside_the_panel_on_their_rows();
    test_a_wrapped_group_has_one_header();
    test_the_handle_count();
    test_the_close_icon_strip();
    test_header_labels_sit_above_their_row_and_off_the_frames();
    test_the_queue_strip_lift();
    test_the_automatic_width();
    test_the_bar_art_map();
    if (eawr::test::ui::failures() != 0) {
        std::cerr << eawr::test::ui::failures() << " build tray geometry check(s) failed\n";
        return 1;
    }
    std::cout << "build tray geometry: all checks passed\n";
    return 0;
}
