// #455: the tactical minimap's model (docs/behaviour/foc-minimap.md). Settings, the world square,
// point mapping, blips (visibility, colour, size, facing, order), the camera guide and the fog
// layer's row-by-row update. The Godot half (drawing, the pointer) runs in the viewer
// (tests/presentation/renderer/test_tactical_hud.py and test_battle_input.py).

#include "eawr/presentation/ui/minimap.hpp"
#include "ui_test_support.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace eawr;
namespace ui = presentation::ui;
using test::ui::expect;

[[nodiscard]] bool near(const double a, const double b, const double tolerance = 1e-9) { return std::abs(a - b) <= tolerance; }

data::XmlAttribute attribute(std::string name, std::string value) {
    data::XmlAttribute result;
    result.name = std::move(name);
    result.value = std::move(value);
    return result;
}

data::XmlNode tag(std::string name, std::string text) {
    data::XmlNode node;
    node.name = std::move(name);
    node.raw_text = std::move(text);
    return node;
}

// MM-01, MM-05.
void test_settings() {
    data::XmlNode settings = tag("RadarMapSettings", "");
    settings.children = {tag("Space_Backdrop_Texture_Name", "i_radar_map_grid.tga"),
                         tag("Space_FOW_Color", "25, 66, 120, 100 "), tag("Space_Is_Guide_Rectangle", "No")};
    data::XmlNode space_colour = tag("Color", "12, 30, 51, 255");
    space_colour.attributes.push_back(attribute("name", "space"));
    data::XmlNode dirt_colour = tag("Color", "0, 0, 0, 0");
    dirt_colour.attributes.push_back(attribute("name", "dirt"));
    settings.children.push_back(space_colour);
    settings.children.push_back(dirt_colour);
    data::XmlNode radar = tag("RadarMap", "");
    radar.children = {settings};
    data::XmlNode constants = tag("GameConstants", "");
    constants.children = {tag("Radar_Colorize_Selected_Units", "Yes"), tag("Radar_Selected_Units_Color", "209, 255, 209, 255")};
    const ui::MinimapSettings read = ui::minimap_settings(&radar, &constants);
    expect(read.diagnostics.empty(), "the FoC settings parse without warnings");
    expect(read.backdrop == "i_radar_map_grid.tga" && read.fog == data::ui::Rgba8{25, 66, 120, 100} && !read.guide_rectangle,
           "MM-01 space backdrop, fog colour and trapezoid guide");
    expect(read.colorize_selected && read.selected == data::ui::Rgba8{209, 255, 209, 255}, "MM-05 selected colour");
    expect(read.background == data::ui::Rgba8{12, 30, 51, 255}, "MM-14: the space colour fills the background");
    data::XmlNode broken_settings = tag("RadarMapSettings", "");
    broken_settings.children = {tag("Space_FOW_Color", "blue"), tag("Space_Is_Guide_Rectangle", "Yes")};
    data::XmlNode broken = tag("RadarMap", "");
    broken.children = {broken_settings};
    const ui::MinimapSettings fallback = ui::minimap_settings(&broken, nullptr);
    expect(fallback.fog == data::ui::Rgba8{25, 66, 120, 100} && fallback.guide_rectangle && fallback.diagnostics.size() == 1,
           "a malformed colour keeps FoC's with one warning; a rectangle guide is read");
    const ui::MinimapSettings missing = ui::minimap_settings(nullptr, nullptr);
    expect(missing.backdrop == "i_radar_map_grid.tga" && missing.colorize_selected, "missing files keep FoC's values");

    // MM-07: owners without a lobby colour take their faction's Factions.xml colour.
    data::XmlNode factions = tag("Factions", "");
    for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>>{
             {"Neutral", "100, 100, 100, 255"}, {"Pirates", "255, 128, 0, 255"}, {"Broken", "orange"}}) {
        data::XmlNode faction = tag("Faction", "");
        faction.attributes.push_back(attribute("Name", name));
        faction.children = {tag("Color", value)};
        factions.children.push_back(faction);
    }
    const ui::MinimapSettings coloured = ui::minimap_settings(nullptr, nullptr, &factions);
    expect(ui::faction_colour(coloured, "PIRATES") == data::ui::Rgba8{255, 128, 0, 255}
               && ui::faction_colour(coloured, "Neutral") == data::ui::Rgba8{100, 100, 100, 255},
           "MM-07: faction colours by name, in any case");
    expect(!ui::faction_colour(coloured, "Broken") && !ui::faction_colour(coloured, "Hutts"),
           "a faction without a readable colour has none");
}

// MM-09: the outline's lines stop at the minimap's edge.
void test_clip() {
    const auto inside = ui::minimap_clip({-0.5, -0.5}, {0.5, 0.5});
    expect(inside && (*inside)[0] == ui::MinimapPoint{-0.5, -0.5} && (*inside)[1] == ui::MinimapPoint{0.5, 0.5},
           "a segment inside the minimap is kept whole");
    const auto crossing = ui::minimap_clip({0.0, 0.0}, {2.0, 1.0});
    expect(crossing && near((*crossing)[1].x, 1.0) && near((*crossing)[1].y, 0.5),
           "a segment leaving the minimap stops on its edge along the same line");
    const auto through = ui::minimap_clip({-3.0, 0.25}, {3.0, 0.25});
    expect(through && near((*through)[0].x, -1.0) && near((*through)[1].x, 1.0) && near((*through)[0].y, 0.25),
           "a segment across the minimap is cut on both sides");
    expect(!ui::minimap_clip({1.5, -2.0}, {1.5, 2.0}) && !ui::minimap_clip({2.0, 0.0}, {0.0, 2.5}),
           "a segment wholly outside draws nothing");
}

// MM-02, MM-03.
void test_mapping() {
    const ui::MinimapExtents extents = ui::minimap_extents(-6100.0, 6100.0, -4000.0, 2000.0);
    expect(near(extents.min_x, -6100.0) && near(extents.max_x, 6100.0) && near(extents.min_y, -7100.0)
               && near(extents.max_y, 5100.0),
           "MM-02: the square around the bounds' centre with their larger half extent");
    expect(near(extents.playable_min_y, -4000.0) && near(extents.playable_max_y, 2000.0), "the playable rectangle stays the bounds");
    const ui::MinimapPoint corner = ui::minimap_point(extents, 6100.0, 5100.0);
    const ui::MinimapPoint centre = ui::minimap_point(extents, 0.0, -1000.0);
    expect(near(corner.x, 1.0) && near(corner.y, 1.0) && near(centre.x, 0.0) && near(centre.y, 0.0),
           "MM-03: the square maps to -1..1, +y up");
    const auto back = ui::minimap_world(extents, {0.5, -0.25});
    const ui::MinimapPoint again = ui::minimap_point(extents, back[0], back[1]);
    expect(near(again.x, 0.5) && near(again.y, -0.25), "minimap_world inverts minimap_point");
}

// MM-06, MM-07, MM-08, MM-12.
void test_blips() {
    const ui::MinimapExtents extents = ui::minimap_extents(-1000.0, 1000.0, -1000.0, 1000.0);
    std::map<std::string, ui::MinimapTypeLooks, std::less<>> types;
    types["Frigate"].visible = true;
    types["Frigate"].size = {0.08F, 0.08F};
    types["Hidden"] = {};
    ui::MinimapTypeLooks unseen;
    unseen.visible = true;
    unseen.visible_to_enemy = false;
    types["Probe"] = unseen;
    ui::MinimapTypeLooks fixed;
    fixed.visible = true;
    fixed.show_facing = false;
    fixed.icon = "mini_map_structure.tga";
    types["Station"] = fixed;
    const auto looks = [&](const std::string_view type) -> const ui::MinimapTypeLooks& { return types.find(type)->second; };
    const data::ui::Rgba8 red{255, 0, 0, 255};
    const data::ui::Rgba8 blue{0, 0, 255, 255};
    std::vector<ui::MinimapUnit> units{
        {1, "Frigate", red, false, false, 500.0, 0.0, 90.0},
        {2, "Frigate", blue, true, false, -500.0, 500.0, 30.0},
        {3, "Hidden", red, false, false, 0.0, 0.0, 0.0},
        {4, "Probe", blue, true, false, 0.0, 0.0, 0.0},
        {5, "Probe", red, false, true, 0.0, 0.0, 0.0},
        {6, "Station", red, false, false, 0.0, -500.0, 45.0},
        {7, "Frigate", red, false, false, 5000.0, 0.0, 0.0},
    };
    ui::MinimapSettings settings;
    const std::vector<ui::MinimapBlip> blips = ui::minimap_blips(units, looks, extents, settings);
    expect(blips.size() == 4, "MM-12: hidden types, enemy-hidden enemies and units off the square are left out");
    expect(blips.size() == 4 && blips[0].id == 6 && blips[1].id == 5 && blips[2].id == 2 && blips[3].id == 1,
           "MM-08: the list is drawn from the back, the first unit on top");
    if (blips.size() != 4) return;
    expect(near(blips[3].centre.x, 0.5) && near(blips[3].centre.y, 0.0) && near(blips[3].half_size[0], 0.08, 1e-6),
           "MM-06: centre and Radar_Icon_Size half extents");
    expect(near(blips[3].rotation_degrees, 0.0) && near(blips[2].rotation_degrees, -60.0),
           "MM-08: facing less a quarter turn");
    expect(near(blips[0].rotation_degrees, 0.0) && blips[0].icon == "mini_map_structure.tga",
           "a type without Radar_Show_Facing stays upright with its own icon");
    expect(blips[3].colour == red && blips[2].colour == blue && blips[1].colour == settings.selected,
           "MM-07: owner colour, the selection's colour when selected");
    settings.colorize_selected = false;
    const std::vector<ui::MinimapBlip> plain = ui::minimap_blips(units, looks, extents, settings);
    expect(plain.size() == 4 && plain[1].colour == red, "without Radar_Colorize_Selected_Units a selected unit keeps its colour");
}

// MM-09.
void test_guide() {
    const ui::MinimapExtents extents = ui::minimap_extents(-1000.0, 1000.0, -1000.0, 1000.0);
    const std::array<std::array<double, 2>, 4> ground{{{-600.0, 800.0}, {600.0, 800.0}, {200.0, -200.0}, {-200.0, -200.0}}};
    const auto trapezoid = ui::minimap_guide(ground, extents, false);
    expect(near(trapezoid[0].x, -0.6) && near(trapezoid[0].y, 0.8) && near(trapezoid[2].x, 0.2) && near(trapezoid[3].y, -0.2),
           "MM-09: the four ground points as they are");
    const auto box = ui::minimap_guide(ground, extents, true);
    expect(near(box[0].x, -0.6) && near(box[0].y, 0.8) && near(box[2].x, 0.6) && near(box[2].y, -0.2),
           "a rectangle guide draws the points' bounding box");
}

// MM-10, MM-04.
void test_fog() {
    const ui::MinimapExtents extents = ui::minimap_extents(-1000.0, 1000.0, -500.0, 500.0);
    ui::MinimapFog fog;
    expect(!fog.advance(extents, std::span<const ui::MinimapRevealer>{}, {25, 66, 120, 100}, true), "no size, no layer");
    fog.resize(20, 20);
    const std::vector<ui::MinimapRevealer> revealers{{-500.0, 0.0, 300.0}};
    const data::ui::Rgba8 colour{25, 66, 120, 100};
    expect(fog.advance(extents, revealers, colour, true) && fog.passes() == 1 && fog.next_row() == 0,
           "a new size fills every row at once");
    const auto texel = [&](const std::uint32_t column, const std::uint32_t row) {
        return fog.texels().data() + (static_cast<std::size_t>(row) * fog.width() + column) * 4U;
    };
    // Rows 0 to 4 are above the playable rectangle (y > 500); column 5 at row 10 is x -500, y 0.
    expect(texel(10, 2)[3] == 0, "MM-10: outside the playable rectangle stays clear");
    expect(texel(5, 10)[3] == 0, "a revealer's circle is clear");
    expect(texel(15, 10)[0] == 25 && texel(15, 10)[3] == 100, "an unrevealed playable texel takes the fog colour");
    const std::size_t fogged = fog.fogged();
    expect(fogged > 0 && fogged < 20U * 20U, "part of the layer is fogged");
    // Move the revealer: the shown layer changes only once a complete pass is done.
    const std::vector<ui::MinimapRevealer> moved{{500.0, 0.0, 300.0}};
    expect(fog.advance(extents, moved, colour, true) && fog.next_row() == ui::MinimapFog::rows_per_frame,
           "MM-04: nine rows a frame");
    expect(texel(15, 10)[3] == 100 && fog.passes() == 1, "the shown layer waits for the pass");
    static_cast<void>(fog.advance(extents, moved, colour, true));
    static_cast<void>(fog.advance(extents, moved, colour, true));
    expect(fog.passes() == 2 && texel(15, 10)[3] == 0 && texel(5, 10)[3] == 100, "the finished pass replaces the shown layer");
    fog.resize(10, 10);
    static_cast<void>(fog.advance(extents, moved, colour, false));
    expect(fog.fogged() == 0 && fog.passes() == 3, "fog off leaves the layer clear");
}

// MM-10 read from the fog cells (#494): a cell above zero is revealed, a zero cell or a point
// outside the grid is fogged.
void test_fog_cells() {
    const ui::MinimapExtents extents = ui::minimap_extents(-1000.0, 1000.0, -500.0, 500.0);
    ui::MinimapFog fog;
    fog.resize(20, 20);
    // Four by two cells of 500 from (-1000, 500): (1, 0) held, (2, 1) fading, the rest fogged.
    auto values = std::make_shared<std::vector<std::uint8_t>>(8U, std::uint8_t{0});
    (*values)[1] = 255;
    (*values)[6] = 7;
    const ui::MinimapFogCells cells{-1000.0, 500.0, 500.0, 4, 2, values};
    const data::ui::Rgba8 colour{25, 66, 120, 100};
    expect(fog.advance(extents, cells, colour, true) && fog.passes() == 1, "the cells fill a new size at once");
    const auto alpha = [&](const std::uint32_t column, const std::uint32_t row) {
        return fog.texels()[(static_cast<std::size_t>(row) * fog.width() + column) * 4U + 3U];
    };
    // Texel (column, row) is world x -1000 + 100 column, y 1000 - 100 row.
    expect(alpha(6, 7) == 0, "a held cell is clear");
    expect(alpha(12, 12) == 0, "a fading cell is still clear");
    expect(alpha(15, 7) == 100, "a zero cell is fogged");
    expect(!cells.revealed(-1200.0, 0.0) && !cells.revealed(0.0, 600.0), "outside the grid is fogged");
    auto rows = std::make_shared<std::vector<std::shared_ptr<const std::vector<std::uint8_t>>>>();
    rows->push_back(std::make_shared<const std::vector<std::uint8_t>>(values->begin(), values->begin() + 4));
    rows->push_back(std::make_shared<const std::vector<std::uint8_t>>(values->begin() + 4, values->end()));
    const ui::MinimapFogCells shared{-1000.0, 500.0, 500.0, 4, 2, {}, rows};
    ui::MinimapFog from_rows;
    from_rows.resize(20, 20);
    expect(from_rows.advance(extents, shared, colour, true)
            && std::equal(fog.texels().begin(), fog.texels().end(), from_rows.texels().begin()),
        "shared rows produce the same minimap fog texels as flat cells");
    expect(!shared.revealed(-1200.0, 0.0) && !shared.revealed(0.0, 600.0), "shared rows keep grid clipping");
}

} // namespace

int main() {
    test_settings();
    test_mapping();
    test_blips();
    test_guide();
    test_clip();
    test_fog();
    test_fog_cells();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " minimap check(s) failed\n";
        return 1;
    }
    std::cout << "ui minimap passed\n";
    return 0;
}
