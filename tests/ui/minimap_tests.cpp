// #455: the tactical minimap's model (docs/behaviour/foc-minimap.md). Settings, the world square,
// point mapping, blips (visibility, colour, size, facing, order), the camera guide and the fog
// layer's row-by-row update. The Godot half (drawing, the pointer) runs in the viewer
// (tests/presentation/renderer/test_tactical_hud.py and test_battle_input.py).

#include "eawr/presentation/ui/minimap.hpp"
#include "eawr/presentation/ui/battle_messages.hpp"
#include "eawr/presentation/space/fog_field.hpp"
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
                         tag("Space_FOW_Color", "25, 66, 120, 100 "), tag("Space_Is_Guide_Rectangle", "No"),
                         tag("Space_Asteroid_Field_Color", "103, 130, 139, 127"),
                         tag("Space_Asteroid_Field_Border_Color", "174, 171, 200, 127")};
    data::XmlNode space_colour = tag("Color", "12, 30, 51, 255");
    space_colour.attributes.push_back(attribute("name", "space"));
    data::XmlNode dirt_colour = tag("Color", "0, 0, 0, 0");
    dirt_colour.attributes.push_back(attribute("name", "dirt"));
    settings.children.push_back(space_colour);
    settings.children.push_back(dirt_colour);
    data::XmlNode radar = tag("RadarMap", "");
    radar.children = {settings};
    data::XmlNode constants = tag("GameConstants", "");
    constants.children = {tag("Radar_Colorize_Selected_Units", "Yes"), tag("Radar_Selected_Units_Color", "209, 255, 209, 255"),
                          tag("Nebula_Effect_Color", "255, 255, 255, 64"),
                          tag("Radar_Colorize_Multiplayer_Enemy", "Yes"), tag("Radar_Multiplayer_Enemy_Color", "1, 2, 3, 255")};
    const ui::MinimapSettings read = ui::minimap_settings(&radar, &constants);
    expect(read.diagnostics.empty(), "the FoC settings parse without warnings");
    expect(read.backdrop == "i_radar_map_grid.tga" && read.fog == data::ui::Rgba8{25, 66, 120, 100} && !read.guide_rectangle,
           "MM-01 space backdrop, fog colour and trapezoid guide");
    expect(read.colorize_enemy && read.enemy == data::ui::Rgba8{1, 2, 3, 255}, "Radar_*_Multiplayer_Enemy tags are read");
    expect(read.colorize_selected && read.selected == data::ui::Rgba8{209, 255, 209, 255}, "MM-05 selected colour");
    expect(read.background == data::ui::Rgba8{12, 30, 51, 255}, "MM-14: the space colour fills the background");
    // Missing singleton colours diagnose while preserving the same FoC default.
    for (const auto name : {"Space_FOW_Color", "Space_Asteroid_Field_Color", "Space_Asteroid_Field_Border_Color",
                           "Radar_Selected_Units_Color", "Nebula_Effect_Color", "Radar_Multiplayer_Enemy_Color"}) {
        auto missing_radar = radar;
        auto missing_constants = constants;
        std::erase_if(missing_radar.children[0].children, [&](const auto& child) { return child.name == name; });
        std::erase_if(missing_constants.children, [&](const auto& child) { return child.name == name; });
        const auto absent = ui::minimap_settings(&missing_radar, &missing_constants);
        expect(absent.diagnostics.size() == 1 && absent.diagnostics.front().code == "EAWR-UI-0330"
                   && absent.diagnostics.front().message.find(name) != std::string::npos,
               "each missing minimap singleton colour is diagnosed");
        expect(absent.fog == read.fog && absent.field == read.field && absent.field_border == read.field_border
                   && absent.selected == read.selected && absent.nebula == read.nebula,
               "missing colours keep the unchanged FoC defaults");
    }
    data::XmlNode broken_settings = tag("RadarMapSettings", "");
    broken_settings.children = {tag("Space_FOW_Color", "blue"), tag("Space_Is_Guide_Rectangle", "Yes")};
    data::XmlNode broken = tag("RadarMap", "");
    broken.children = {broken_settings};
    const ui::MinimapSettings fallback = ui::minimap_settings(&broken, nullptr);
    expect(fallback.fog == data::ui::Rgba8{25, 66, 120, 100} && fallback.guide_rectangle && fallback.diagnostics.size() == 6,
           "malformed and missing colours keep FoC defaults with warnings; a rectangle guide is read");
    const ui::MinimapSettings missing = ui::minimap_settings(nullptr, nullptr);
    expect(missing.backdrop == "i_radar_map_grid.tga" && missing.colorize_selected, "missing files keep FoC's values");
    expect(missing.diagnostics.size() == 6, "missing roots diagnose all singleton colour fallbacks");

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

void test_faction_registry() {
    test::ui::TempTree tree("radar-factions");
    test::ui::write_text(tree.root / "XML/RadarMap.xml", "<RadarMap><RadarMapSettings><Space_FOW_Color>25,66,120,100</Space_FOW_Color>"
        "<Space_Asteroid_Field_Color>103,130,139,127</Space_Asteroid_Field_Color>"
        "<Space_Asteroid_Field_Border_Color>174,171,200,127</Space_Asteroid_Field_Border_Color></RadarMapSettings></RadarMap>");
    test::ui::write_text(tree.root / "XML/GameConstants.xml", "<GameConstants><Nebula_Effect_Color>255,255,255,64</Nebula_Effect_Color>"
        "<Radar_Selected_Units_Color>209,255,209,255</Radar_Selected_Units_Color>"
        "<Radar_Multiplayer_Enemy_Color>255,0,0</Radar_Multiplayer_Enemy_Color></GameConstants>");
    test::ui::write_text(tree.root / "XML/FactionFiles.xml",
        "<Faction_Files><File>Factions.xml</File><File>Data/XML/extra.xml</File></Faction_Files>");
    test::ui::write_text(tree.root / "XML/Factions.xml", R"xml(<Factions>
<Faction Name="Neutral"><Color>100,100,100,255</Color></Faction>
<Faction Name="Pirates"><Color>255,128,0,255</Color><No_Colorization_Color>1,2,3,255</No_Colorization_Color></Faction>
</Factions>)xml");
    test::ui::write_text(tree.root / "XML/extra.xml", R"xml(<Factions>
<Faction Name="Hutts"><Color>255,128,0,255</Color><No_Colorization_Color>231,221,67,255</No_Colorization_Color></Faction>
<Faction Name="PIRATES"><Color>9,8,7,255</Color></Faction>
</Factions>)xml");
    const std::array mounts{vfs::MountSpec{"synthetic", tree.root, "data", {}}};
    auto filesystem = vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(filesystem), "WNO-40: faction registry fixture mounts");
    if (!filesystem) return;
    const auto settings = ui::minimap_settings(filesystem.value());
    expect(settings.diagnostics.empty(), "WNO-40: registry includes load without fallback");
    expect(ui::faction_colour(settings, "Hutts") == data::ui::Rgba8{255,128,0,255}
        && ui::faction_colour(settings, "Hutts", true) == data::ui::Rgba8{231,221,67,255},
        "WNO-40/43: an additional faction file supplies ordinary and unassigned colours");
    expect(ui::faction_colour(settings, "Pirates") == data::ui::Rgba8{9,8,7,255}
        && !ui::faction_colour(settings, "Pirates", true),
        "WNO-40: a later definition replaces the complete earlier faction colour input");
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
    {
        const data::ui::Rgba8 green{0, 255, 0, 255};
        ui::MinimapSettings enemy_settings;
        enemy_settings.colorize_enemy = true;
        enemy_settings.enemy = green;
        const auto recoloured = ui::minimap_blips(units, looks, extents, enemy_settings);
        expect(recoloured.size() == 4 && recoloured[2].colour == green && recoloured[3].colour == red,
            "Radar_Colorize_Multiplayer_Enemy recolours hostile blips only");
        enemy_settings.colorize_enemy = false;
        const auto stock = ui::minimap_blips(units, looks, extents, enemy_settings);
        expect(stock.size() == 4 && stock[2].colour == blue, "stock No keeps the owner colour of a hostile blip");
    }
    units[5].owner_colour = blue;
    const auto claimed = ui::minimap_blips(units, looks, extents, settings);
    expect(claimed.size() == 4 && claimed[0].id == 6 && claimed[0].colour == blue,
        "WBP-37 MM-07 a standing pad's blip takes its live captured owner colour");
    units[1].in_nebula = true;
    units[0].in_nebula = true;
    const auto obscured = ui::minimap_blips(units, looks, extents, settings);
    expect(obscured.size() == 3 && std::none_of(obscured.begin(), obscured.end(), [](const auto& blip) { return blip.id == 2; }),
        "WHZ-70: nebula suppresses enemy blips while own units remain on radar");
    types["Frigate"].hazard = true;
    const auto hazards = ui::minimap_blips(units, looks, extents, settings);
    expect(hazards.size() == 2, "WHZ-70: hazard types never produce ordinary blips");
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

// MM-16/17: the empty-name path is distinct from a missing name; point clipping and
// conditional world scaling must stay independent of squadron member spread and camera zoom.
void test_point_and_scale() {
    test::ui::TempTree tree("radar-style");
    test::ui::write_text(tree.root / "XML/GameObjectFiles.xml", "<Game_Object_Files><File>objects.xml</File></Game_Object_Files>");
    test::ui::write_text(tree.root / "XML/objects.xml", R"xml(<Objects>
<Container Name="Fixed"><Is_Visible_On_Radar>Yes</Is_Visible_On_Radar><Visible_On_Radar_When_Fogged>Yes</Visible_On_Radar_When_Fogged><No_Colorization_Color>11,22,33,255</No_Colorization_Color><Radar_Icon_Scale_Space>200</Radar_Icon_Scale_Space></Container>
<Container Name="Point"><Variant_Of_Existing_Type>Fixed</Variant_Of_Existing_Type><Radar_Icon_Name></Radar_Icon_Name><Radar_Blip_Size>2.9</Radar_Blip_Size></Container>
<Container Name="Scaled"><Variant_Of_Existing_Type>Fixed</Variant_Of_Existing_Type><Radar_Draw_To_Scale>Yes</Radar_Draw_To_Scale></Container>
<Container Name="Bad"><Variant_Of_Existing_Type>Point</Variant_Of_Existing_Type><Radar_Blip_Size>NaN</Radar_Blip_Size></Container>
<Container Name="BlankColour"><Variant_Of_Existing_Type>Fixed</Variant_Of_Existing_Type><No_Colorization_Color></No_Colorization_Color></Container>
<Container Name="TbdColour"><Variant_Of_Existing_Type>BlankColour</Variant_Of_Existing_Type><No_Colorization_Color>TBD</No_Colorization_Color></Container>
<Container Name="OwnColour"><Variant_Of_Existing_Type>TbdColour</Variant_Of_Existing_Type><No_Colorization_Color>1,2,3,255</No_Colorization_Color></Container>
<Container Name="LoneBlank"><No_Colorization_Color></No_Colorization_Color></Container>
</Objects>)xml");
    const std::array mounts{vfs::MountSpec{"synthetic", tree.root, "data", {}}};
    auto filesystem = vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(filesystem), "radar style fixture mounts");
    if (!filesystem) return;
    auto loaded = data::load_catalog(filesystem.value(), data::Profile::foc);
    expect(static_cast<bool>(loaded), "radar style fixture loads");
    if (!loaded) return;
    std::map<std::string, ui::MinimapTypeLooks, std::less<>> types;
    for (const auto name : {"Fixed", "Point", "Scaled", "Bad", "BlankColour", "TbdColour", "OwnColour", "LoneBlank"}) types[name] = ui::minimap_type_looks(name, &loaded.value().catalog);
    expect(types["Point"].visible_when_fogged && types["Point"].no_colorization == data::ui::Rgba8{11,22,33,255}
        && !ui::minimap_type_looks("Absent", &loaded.value().catalog).visible_when_fogged,
        "WNO-41/43: effective inherited fogged flag and unassigned colour, absent flag defaults false");
    // ML-24 (#3631): a blank or TBD variant colour inherits (The_Peacebringer), a real one replaces it.
    expect(types["BlankColour"].no_colorization == data::ui::Rgba8{11,22,33,255}
        && types["TbdColour"].no_colorization == data::ui::Rgba8{11,22,33,255}
        && types["OwnColour"].no_colorization == data::ui::Rgba8{1,2,3,255} && !types["LoneBlank"].no_colorization,
        "ML-24: blank and TBD variant No_Colorization_Color inherit; a real colour replaces; a lone blank has none");
    expect(types["Fixed"].icon == "i_radar_default_blip.tga" && !types["Fixed"].draw_to_scale
        && near(types["Fixed"].space_scale, 200), "MM-17: authored scale alone preserves default fixed icon size");
    expect(types["Point"].icon.empty() && near(types["Point"].point_size, 2.9, 1e-6)
        && near(types["Bad"].point_size, 2), "MM-16: explicit empty name plots points; invalid size keeps default");
    const auto looks = [&](const std::string_view name) -> const ui::MinimapTypeLooks& { return types.find(name)->second; };
    const auto extents = ui::minimap_extents(-1000, 1000, -1000, 1000);
    ui::MinimapUnit unit{7, "Point", {12, 34, 56, 255}, false, true};
    auto blips = ui::minimap_blips(std::span(&unit, 1), looks, extents, {});
    expect(blips.size() == 1 && blips.front().icon.empty() && blips.front().point_pixels == 2
        && blips.front().colour == ui::MinimapSettings{}.selected, "MM-16: selected point survives ordinary admission");
    if (blips.empty()) return;
    auto point = blips.front();
    expect(ui::minimap_point_pixels(point, 10, 8) == ui::MinimapPixelRect{5, 4, 2, 2}, "MM-16: centre is truncated in texture pixels");
    point.centre = {0.99, -0.99};
    expect(ui::minimap_point_pixels(point, 10, 8) == ui::MinimapPixelRect{9, 7, 1, 1}, "MM-16: final row and column clip extra texels");
    point.centre = {-1, 1};
    expect(ui::minimap_point_pixels(point, 10, 8) == ui::MinimapPixelRect{0, 0, 2, 2}, "MM-16: top and left edges remain admitted");
    point.centre = {1, 0};
    expect(!ui::minimap_point_pixels(point, 10, 8), "MM-16: right-edge centre is outside");
    point.centre = {0, -1};
    expect(!ui::minimap_point_pixels(point, 10, 8), "MM-16: bottom-edge centre is outside");
    point.centre = {0, 0};
    point.point_pixels = 3;
    expect(ui::minimap_point_pixels(point, 10, 8) == ui::MinimapPixelRect{5, 4, 1, 1}, "MM-16: every other integer size writes one texel");
    unit.type = "Scaled";
    unit.team = true;
    blips = ui::minimap_blips(std::span(&unit, 1), looks, extents, {});
    expect(blips.size() == 1 && near(blips[0].half_size[0], 0.2) && near(blips[0].half_size[1], 0.2),
        "MM-17: model-free team scale is a world extent, not a multiplier of icon size");
    unit.team = false;
    unit.world_half_size = {3, 5};
    blips = ui::minimap_blips(std::span(&unit, 1), looks, extents, {});
    expect(blips.size() == 1 && near(blips[0].half_size[0], 0.6) && near(blips[0].half_size[1], 1.0),
        "MM-17: model bounds are multiplied by authored scale then projected");
    unit.type = "Fixed";
    blips = ui::minimap_blips(std::span(&unit, 1), looks, extents, {});
    expect(blips.size() == 1 && near(blips[0].half_size[0], 0.05, 1e-6), "MM-17: fixed-size path ignores world bounds and space scale");
    unit.radar.model_present.reset();
    blips = ui::minimap_blips(std::span(&unit, 1), looks, extents, {});
    expect(blips.size() == 1, "MM-17: no population or unknown placement preserves ordinary icons");
    unit.hostile = true;
    unit.radar.model_hidden = true;
    unit.radar.radar_faded = true;
    types["Fixed"].visible_when_fogged = true;
    blips = ui::minimap_blips(std::span(&unit, 1), looks, extents, {});
    expect(blips.size() == 1, "MM-17: admitted fogged enemy with unknown model identity keeps its icon");
    unit.radar.model_present = true;
    blips = ui::minimap_blips(std::span(&unit, 1), looks, extents, {});
    expect(blips.size() == 1, "MM-17: composed fogged model keeps its icon without a visible pick box");
    unit.hostile = false;
    unit.radar.model_present = false;
    unit.radar.replay = true;
    blips = ui::minimap_blips(std::span(&unit, 1), looks, extents, {});
    expect(blips.empty(), "MM-17: model-free ordinary textured icon stays absent in a revealed replay");
    unit.hostile = true;
    blips = ui::minimap_blips(std::span(&unit, 1), looks, extents, {});
    expect(blips.empty(), "MM-17: model admission is independent of ownership");
    unit.hostile = false;
    unit.team = true;
    blips = ui::minimap_blips(std::span(&unit, 1), looks, extents, {});
    expect(blips.size() == 1, "MM-17: a populated model-free team retains its textured icon");
    unit.team = false;
    unit.type = "Point";
    blips = ui::minimap_blips(std::span(&unit, 1), looks, extents, {});
    expect(blips.size() == 1 && blips.front().icon.empty(), "MM-16: model-free empty-icon point remains admitted");
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

void test_initial_radar_knowledge() {
    using Fixed = sim::math::Fixed;
    const auto fixed = [](const std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); };
    std::array<sim::tactical::UnitState, 3> initial{};
    for (std::size_t index = 0; index < initial.size(); ++index) {
        initial[index].entity_id = index + 1;
        initial[index].type_id = 77;
        initial[index].owner = index == 0 ? 1U : 2U;
        initial[index].position = {fixed(static_cast<std::int64_t>(index) * 100), fixed(50), {}};
    }
    const auto start = initial; // the battle's immutable start tick, independent of render cadence
    auto current = initial;
    current[0].owner = 2;
    current[0].position = {fixed(700), fixed(-500), {}};
    current[0].rotation = {};
    const auto* remembered = ui::minimap_initial_identity(start, current[0].entity_id, current[0].type_id);
    expect(remembered && *remembered == start[0],
        "MM-18: hidden ownership/pose changes before the first presentation cannot refresh start knowledge");
    expect(!ui::minimap_initial_identity(start, 99, 77),
        "MM-18: a later spawn or respawn identity cannot supply initial knowledge");
    expect(!ui::minimap_initial_identity(start, start[0].entity_id, 88),
        "MM-18: a replacement type cannot borrow the start identity's model");

    const data::ui::Rgba8 own{20, 140, 230, 255}, enemy{230, 40, 80, 255};
    std::vector<ui::MinimapMemory> memories;
    for (std::size_t index = 0; index < current.size(); ++index) {
        const auto* identity = ui::minimap_initial_identity(start, current[index].entity_id, current[index].type_id);
        expect(identity != nullptr, "MM-18: start identities are available before sensor discovery");
        if (!identity) continue;
        ui::MinimapMemory memory;
        memory.id = identity->entity_id;
        memory.type = "Known";
        memory.owner_colour = identity->owner == 1 ? own : enemy;
        memory.hostile = identity->owner == 2;
        memory.capture_point = index == 2;
        memory.previously_revealed = true;
        memory.retained_model = true;
        memory.x = static_cast<double>(identity->position.x.raw()) / static_cast<double>(Fixed::scale);
        memory.y = static_cast<double>(identity->position.y.raw()) / static_cast<double>(Fixed::scale);
        memories.push_back(memory);
    }
    ui::MinimapTypeLooks type;
    type.visible = true;
    const auto looks = [&](std::string_view) -> const ui::MinimapTypeLooks& { return type; };
    const auto extents = ui::minimap_extents(-1000, 1000, -1000, 1000);
    const auto draw = [&] { return ui::minimap_blips({}, looks, extents, {}, memories); };
    const auto blips = draw();
    expect(blips.size() == 3, "MM-18: owned, enemy-owned and capture-point initial memories draw");
    if (blips.size() == 3) {
        expect(blips[2].colour == own && blips[1].colour == enemy
            && blips[0].colour == data::ui::Rgba8{100, 100, 100, 255},
            "MM-18: ordinary memories retain stored ownership; enemy capture points remain Neutral grey");
        expect(blips[2].remembered && blips[2].centre == ui::minimap_point(extents, 0, 50),
            "MM-18: hidden current position is not substituted for the immutable initial transform");
    }
    type.visible_to_enemy = false;
    expect(draw().size() == 1, "MM-18: initial knowledge retains enemy radar permission");

    auto transform = sim::math::identity_matrix();
    transform.rows[0][0] = fixed(-2);
    transform.rows[1][1] = fixed(3);
    expect(ui::minimap_world_half_size(transform, {-4, -2, -1}, {4, 2, 1}) == std::array<double, 2>{8, 6},
        "MM-17: copied model bounds preserve scale and reflected axes without accumulating old extents");
}

void test_radar_fog_contract() {
    ui::MinimapTypeLooks type;
    type.visible = true;
    const auto looks = [&](std::string_view) -> const ui::MinimapTypeLooks& { return type; };
    const auto extents = ui::minimap_extents(-1000, 1000, -500, 500);
    ui::MinimapUnit unit;
    unit.id = 8;
    unit.owner_colour = {255, 128, 0, 255};
    const auto draw = [&] { return ui::minimap_blips(std::span(&unit, 1), looks, extents, {}); };
    unit.radar.model_hidden = true;
    expect(draw().empty(), "WNO-41: ordinary hidden model rejects radar");
    type.visible_when_fogged = true;
    expect(draw().size() == 1, "WNO-41: true flag bypasses hidden model even for neutral ownership");
    unit.radar.radar_faded = true;
    expect(draw().size() == 1, "WNO-41: true flag also bypasses radar fade");
    type.visible_when_fogged = false;
    unit.radar.replay = true;
    expect(draw().size() == 1, "WNO-41: replay bypasses the same early checks");
    for (const bool replay : {false, true}) {
        type.visible_when_fogged = true;
        unit.radar.replay = replay;
        unit.radar.locally_visible = false;
        expect(draw().empty(), "WNO-41: flag and replay retain final local visibility gate");
        unit.radar.interdicted = true;
        expect(draw().size() == 1, "WNO-41: active interdiction is a separate final admission");
        unit.radar.interdicted = false;
        unit.radar.locally_visible = true;
        for (auto member : {&ui::MinimapLiveState::limbo, &ui::MinimapLiveState::jammed,
                            &ui::MinimapLiveState::stealthed}) {
            unit.hostile = true;
            unit.radar.*member = true;
            expect(draw().empty(), "WNO-41: flag/replay never bypass limbo, jamming or stealth");
            unit.radar.*member = false;
        }
        unit.in_nebula = true;
        expect(draw().empty(), "WNO-41: enemy nebula exclusion survives flag/replay");
        unit.in_nebula = false;
        type.visible_to_enemy = false;
        expect(draw().empty(), "WNO-41: enemy-radar permission survives flag/replay");
        type.visible_to_enemy = true;
        unit.radar.alive = false;
        expect(draw().empty(), "WNO-41: dead live identities never survive flag/replay");
        unit.radar.alive = true;
    }
    unit.hostile = false;
    unit.y = 700;
    expect(draw().empty(), "WNO-41: noncapital outside playable bounds rejects even inside radar square");
    unit.radar.capital_layer = true;
    expect(draw().size() == 1, "WNO-41: capital-layer routing preserves radar square admission");
    unit.y = 0;
    unit.selected = true;
    unit.capture = ui::MinimapCaptureColour{false, {100,100,100,255}, {200,0,100,255}, 0.25};
    expect(draw().front().colour == data::ui::Rgba8{125,75,100,255}, "WNO-43: progress overrides selected colour");
    unit.capture->raw_local_fog = true;
    expect(draw().front().colour == data::ui::Rgba8{100,100,100,255}, "WNO-43: raw fog overrides owner and progress");
    unit.hostile = true;
    unit.radar.display_enemies = false;
    expect(draw().size() == 1, "WNO-41: capture bypasses only enemy-display switch");
    unit.capture.reset();
    expect(draw().empty(), "WNO-41: ordinary enemies respect enemy-display switch");
    unit.hostile = false;
    unit.selected = false;
    expect(draw().front().colour == unit.owner_colour, "WNO-40: ordinary Hutt container keeps owner colour");
    const std::array<ui::MinimapPlayerColour, 5> players{{
        {1,0,false,{10,20,30,255}}, {2,0,false,{40,50,60,255}},
        {3,1,false,{70,80,90,255}}, {4,1,false,{100,110,120,255}}, {5,2,true,{100,100,100,255}}}};
    const data::ui::Rgba8 fallback{1,2,3,255};
    expect(ui::minimap_community_colour(players, 2, 1, true, fallback) == players[0].colour,
        "WNO-43: local allied community property uses local colour");
    expect(ui::minimap_community_colour(players, 4, 1, true, fallback) == players[2].colour,
        "WNO-43: enemy team uses representative colour");
    expect(ui::minimap_community_colour(players, 5, 1, true, fallback) == fallback
        && ui::minimap_community_colour(players, 99, 1, true, fallback) == fallback
        && ui::minimap_community_colour(players, 2, 1, false, fallback) == fallback,
        "WNO-43: neutral, unassigned and nonmultiplayer retain fallback");
    ui::MinimapMemory memory;
    memory.id = unit.id;
    memory.owner_colour = players[3].colour;
    memory.x = 123;
    memory.y = 321;
    memory.yaw_degrees = 40;
    const auto remembered = [&] { return ui::minimap_blips({}, looks, extents, {}, std::span(&memory,1)); };
    expect(remembered().empty(), "WNO-44: undiscovered identity has no remembered icon");
    memory.previously_revealed = true;
    expect(remembered().empty(), "WNO-44: known identity without retained model has no icon");
    memory.retained_model = true;
    type.visible_when_fogged = false;
    auto blips = remembered();
    expect(blips.size() == 1 && blips[0].remembered && blips[0].colour == memory.owner_colour
        && blips[0].centre == ui::minimap_point(extents, 123, 321),
        "WNO-44: stored transform/owner draw independently of live fogged flag");
    memory.capture_point = true;
    expect(remembered().front().colour == data::ui::Rgba8{100,100,100,255}, "WNO-44: capture memory is neutral");
    type.visible = false;
    expect(remembered().size() == 1 && draw().empty(), "WNO-44: memory does not repeat live radar-type admission");
    type.visible = true;
    memory.y = 700;
    expect(remembered().empty(), "WNO-44: remembered transform must lie inside playable bounds");
    memory.y = 321;
    memory.hostile = true;
    memory.display_enemies = false;
    expect(remembered().empty(), "WNO-44: capture memory does not bypass enemy-display switch");
    memory.display_enemies = true;
    type.visible_to_enemy = false;
    expect(remembered().empty(), "WNO-44: remembered enemy requires enemy-radar permission");
    type.visible_to_enemy = true;
    type.hazard = true;
    expect(remembered().empty() && draw().empty(), "WNO-40/44: hazards never produce either identity");
}

} // namespace

int main() {
    const std::array<ui::MinimapSquadronMember, 3> members{{{0, 0, 35, true},
        {6, 3, 70, false}, {0, 0, 100, true}}};
    const auto pose = ui::minimap_squadron_pose(members);
    expect(pose && near(pose->x, 2) && near(pose->y, 1),
        "MM-15: radar uses the member mean, including hidden members, rather than the box centre");
    expect(pose && pose->yaw_degrees == 35,
        "MM-15: facing comes from the first live member");
    auto hidden_leader = members;
    hidden_leader[0].visible = false;
    expect(!ui::minimap_squadron_pose(hidden_leader), "MM-15: another seen member cannot bypass leader fog");
    expect(!ui::minimap_squadron_pose({}), "MM-15: an empty or docked team has no identity");
    const auto survivor = ui::minimap_squadron_pose(std::span(members).last(1));
    expect(survivor && survivor->yaw_degrees == 100 && survivor->x == 0,
        "MM-15: deleting members changes the centre and the first live member together");
    if (const auto filesystem = test::ui::foc_corpus("hazard minimap")) {
        const auto catalog = data::load_catalog(*filesystem, data::Profile::foc);
        expect(static_cast<bool>(catalog), "stock catalog loads for inherited asteroid-base radar policy");
        if (catalog) {
            const auto hutt = ui::minimap_type_looks("Skirmish_Hutt_Asteroid_Base", &catalog.value().catalog);
            expect(hutt.visible && hutt.visible_when_fogged && hutt.icon == "i_radar_station1_p.tga",
                "MM-06/12: the skirmish asteroid base inherits radar permission and icon from its parent type");
            // MM-06/07/15: a no-slot owner supplies colour, not radar type permission.
            const auto settings = ui::minimap_settings(*filesystem);
            const auto pirates = ui::faction_colour(settings, "Pirates");
            expect(pirates == data::ui::Rgba8{255, 128, 0, 255}, "MM-07: Pirates supplies orange without a lobby slot");
            const std::array<ui::MinimapSquadronMember, 2> pirate_members{{{100, 50, 30, true}, {120, 70, 60, true}}};
            const auto pirate_pose = ui::minimap_squadron_pose(pirate_members);
            expect(pirate_pose.has_value(), "MM-15: a visible pirate leader admits the squadron pose");
            if (pirate_pose && pirates) {
                ui::MinimapUnit pirate;
                pirate.id = 4153;
                pirate.type = "Team";
                pirate.team = true;
                pirate.hostile = true;
                pirate.owner_colour = *pirates;
                pirate.x = pirate_pose->x;
                pirate.y = pirate_pose->y;
                pirate.yaw_degrees = pirate_pose->yaw_degrees;
                const auto team = ui::minimap_type_looks("Team", &catalog.value().catalog);
                const auto starviper = ui::minimap_type_looks("StarViper_Team", &catalog.value().catalog);
                const auto looks = [&](const std::string_view type) -> const ui::MinimapTypeLooks& {
                    return type == "Team" ? team : starviper;
                };
                const auto draw = [&] {
                    return ui::minimap_blips(std::span(&pirate, 1), looks,
                        ui::minimap_extents(-1000, 1000, -1000, 1000), settings);
                };
                const auto blips = draw();
                expect(blips.size() == 1 && blips.front().id == pirate.id && blips.front().colour == *pirates,
                    "MM-07/15: a radar-enabled pirate squadron draws one orange blip without a seat");
                pirate.type = "StarViper_Team";
                expect(!starviper.visible && draw().empty(),
                    "MM-06: stock StarViper_Team omits radar permission; owner colour cannot override it");
                pirate.team_radar_visible = ui::minimap_type_looks("Starviper_Fighter", &catalog.value().catalog).visible;
                const auto starviper_blips = draw();
                expect(starviper_blips.size() == 1 && starviper_blips.front().colour == *pirates
                    && starviper_blips.front().icon == starviper.icon,
                    "MM-07/15: radar-visible Starviper craft admit one orange team-styled blip without a seat");
                pirate.team_radar_visible = false;
                expect(draw().empty(), "MM-15: a squadron with no radar-visible live craft remains hidden");
                pirate.team = false;
                pirate.team_radar_visible = true;
                expect(draw().empty(), "MM-06: the member permission override applies only to teams");
            }
        }
        const auto constants = data::load_document(*filesystem, "data/xml/gameconstants.xml");
        expect(static_cast<bool>(constants), "stock GameConstants load for fallback contracts");
        if (constants) {
            expect(presentation::space::fog_looks(constants.value().root).diagnostics.empty(),
                   "stock FoC fog constants load with zero problems");
            expect(ui::battle_message_looks(constants.value().root).diagnostics.empty(),
                   "stock FoC battle-message constants load with zero problems");
        }
        const auto settings = ui::minimap_settings(*filesystem);
        expect(settings.diagnostics.empty(), "WHZ-71/72: installed FoC hazard settings load without fallback diagnostics");
        expect(settings.nebula == data::ui::Rgba8{255, 255, 255, 64}, "WHZ-71: installed nebula effect RGBA matches FoC");
        expect(settings.field == data::ui::Rgba8{103, 130, 139, 127}, "WHZ-72: installed hazard fill RGBA matches FoC");
        expect(settings.field_border == data::ui::Rgba8{174, 171, 200, 127}, "WHZ-72: installed hazard border RGBA matches FoC");
        expect(ui::faction_colour(settings, "Hutts") == data::ui::Rgba8{255,128,0,255},
            "WNO-40: installed expansion Hutt faction supplies ordinary orange radar colour");
    }
    const auto extents = ui::minimap_extents(-1000, 1000, -1000, 1000);
    const ui::MinimapHazard hazard{0, 0, 1000, 600};
    const auto single = ui::minimap_hazards(std::span(&hazard, 1), extents, {}, 80, 80);
    const std::array<ui::MinimapHazard, 2> overlap{hazard, hazard};
    expect(single == ui::minimap_hazards(overlap, extents, {}, 80, 80), "WHZ-72: overlap forms one mask without additive colour");
    expect(single.size() == 80 * 80 * 4 && single[(40 * 80 + 39) * 4 + 3] == 127,
        "WHZ-72: the field interior has the authored fill alpha");
    const auto clear = ui::minimap_hazards({}, extents, {}, 80, 80);
    expect(std::all_of(clear.begin(), clear.end(), [](const auto value) { return value == 0; }),
        "WHZ-72: rebuilding without registered hazards clears the map");
    expect(ui::minimap_hazards(overlap, extents, {}, 0, 80).empty(), "WHZ-72: invalid drawable dimensions leave retry to the view");
    test_settings();
    test_faction_registry();
    test_mapping();
    test_blips();
    test_point_and_scale();
    test_guide();
    test_clip();
    test_fog();
    test_fog_cells();
    test_radar_fog_contract();
    test_initial_radar_knowledge();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " minimap check(s) failed\n";
        return 1;
    }
    std::cout << "ui minimap passed\n";
    return 0;
}
