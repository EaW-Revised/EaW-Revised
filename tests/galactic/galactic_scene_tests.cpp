#include "galactic/scene_content.hpp"
#include "eawr/core/diagnostic.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

using namespace eawr;
using namespace viewer::galactic;

namespace {
void planet_bonus_metadata(const data::galactic::GalacticData& tables) {
    data::ui::TextDatabase text;
    const auto entry = [&](std::string key, std::u16string value) {
        text.entries.push_back({data::ui::text_key_crc(key), std::move(key), std::move(value)});
    };
    entry("MOD_NAME", u"Authored name"); entry("MOD_DESCRIPTION", u"Authored description");
    entry("INFO_TITLE", u"Retail title"); entry("ENC_FIRST", u"Retail body");
    entry("ENC_SECOND", u"Must not appear");
    for (std::uint32_t i = 0; i < text.entries.size(); ++i) text.crc_order.push_back(i);
    std::sort(text.crc_order.begin(), text.crc_order.end(),
        [&](const auto a, const auto b) { return text.entries[a].crc < text.entries[b].crc; });
    const auto* planet = tables.planet("Beta");
    assert(planet && planet->planet_ability_icon == "mod_bonus.tga");
    assert(planet->planet_ability_name == "MOD_NAME" && planet->planet_ability_description == "MOD_DESCRIPTION");
    std::vector<std::string> diagnostics;
    const auto bonus = planet_ability_metadata(*planet, &text, diagnostics);
    assert(diagnostics.empty());
    assert(bonus.authored_name == "Authored name" && bonus.authored_description == "Authored description");
    assert(bonus.title == "Retail title" && bonus.description == "Retail body" && bonus.icon == "mod_bonus.tga");
    auto missing = *planet;
    missing.describe_advantage = "MISSING"; missing.encyclopedia_text.clear();
    const auto partial = planet_ability_metadata(missing, &text, diagnostics);
    assert(partial.title.empty() && partial.description.empty() && diagnostics.size() == 2);
    assert(diagnostics[0].find("MISSING") != std::string::npos);
    data::galactic::Planet empty;
    diagnostics.clear();
    const auto absent = planet_ability_metadata(empty, &text, diagnostics);
    assert(absent.icon.empty() && absent.title.empty() && absent.description.empty() && diagnostics.empty());
}

void write(const std::filesystem::path& root, std::string_view path, std::string_view value) {
    const auto file = root / path;
    std::filesystem::create_directories(file.parent_path());
    std::ofstream output(file);
    output << value;
    assert(output.good());
}

// #2898: names resolve once. A second lookup of the display string misses and
// used to restore the raw object ID over a valid localized name.
void production_names_localize_once(const data::Catalog& catalog) {
    data::ui::TextDatabase text;
    const auto entry = [&](std::string key, std::u16string value) {
        text.entries.push_back({data::ui::text_key_crc(key), std::move(key), std::move(value)});
    };
    entry("TEXT_UNIT_GUNSHIP", u"Crusader Gunship");
    entry("TEXT_ENC_GUNSHIP_1", u"Line one");
    entry("TEXT_ENC_GUNSHIP_2", u"Line two");
    entry("TEXT_ENC_MULTIPLAYER", u"Multiplayer only");
    for (std::uint32_t i = 0; i < text.entries.size(); ++i) text.crc_order.push_back(i);
    std::sort(text.crc_order.begin(), text.crc_order.end(),
        [&](const auto a, const auto b) { return text.entries[a].crc < text.entries[b].crc; });
    const auto art = load_production_art(catalog, &text);
    assert(art.at("Crusader_Gunship").text == "Crusader Gunship");
    // The key survives for readers that resolve from it (battle summary).
    assert(art.at("Crusader_Gunship").text_id == "TEXT_UNIT_GUNSHIP");
    // P-1/P-2: the shared resolver preserves an untranslated line's slot.
    assert(art.at("Crusader_Gunship").encyclopedia == "Line one\n\nLine two");
    assert(art.at("Modded_Ship").text == "Modded_Ship" && art.at("Modded_Ship").encyclopedia.empty());
    assert(art.at("Modded_Ship").hero_rank == 25);
    const auto& inherited = art.at("Modded_Variant");
    assert(inherited.text == "Crusader Gunship" && inherited.text_id == "TEXT_UNIT_GUNSHIP");
    assert(inherited.icon == "i_variant.tga" && inherited.gui_row == 7);
    assert(inherited.hero_rank == 90 && art.at("Crusader_Gunship").hero_rank == 90);
    assert(inherited.encyclopedia == "Line one\n\nLine two");
    assert(inherited.encyclopedia_looks.name == "Crusader Gunship"
        && inherited.encyclopedia_looks.icon == "i_variant.tga"
        && inherited.encyclopedia_looks.description == "Line one\n\nLine two");
    assert(inherited.type_order > art.at("Crusader_Gunship").type_order);
    const auto unlocalized = load_production_art(catalog);
    assert(unlocalized.at("Crusader_Gunship").text == "Crusader_Gunship");
    assert(unlocalized.at("Crusader_Gunship").text_id == "TEXT_UNIT_GUNSHIP");
}

void campaign_art_is_data_driven() {
    const auto root = std::filesystem::temp_directory_path() /
        ("eawr-scene-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    write(root, "Data/XML/GameObjectFiles.xml", "<Game_Object_Files><File>Planets.xml</File></Game_Object_Files>");
    write(root, "Data/XML/Planets.xml", R"XML(<Planets>
      <Planet Name="Backdrop"><Behavior>IDLE</Behavior><Galactic_Position>0,0,0</Galactic_Position><Galactic_Model_Name>galaxy.alo</Galactic_Model_Name>
      <Scale_Factor>1</Scale_Factor><In_Background>Yes</In_Background><Loop_Idle_Anim_00>Yes</Loop_Idle_Anim_00></Planet>
      <Planet Name="Alpha"><Behavior>PLANET</Behavior><Galactic_Position>12,34,10</Galactic_Position><Galactic_Model_Name>alpha.alo</Galactic_Model_Name>
      <Scale_Factor>2.5</Scale_Factor><Show_Name>Yes</Show_Name><Name_Adjust>1,2,3</Name_Adjust>
      <Mouse_Collide_Override_Sphere_Radius>17</Mouse_Collide_Override_Sphere_Radius><Text_ID>ALPHA</Text_ID>
      <Planet_Ability_Name>BASE_NAME</Planet_Ability_Name><Planet_Ability_Description>BASE_DESCRIPTION</Planet_Ability_Description>
      <Planet_Ability_Icon>base_bonus.tga</Planet_Ability_Icon><Describe_Advantage>INFO_TITLE</Describe_Advantage>
      <Encyclopedia_Text>ENC_FIRST, ENC_SECOND</Encyclopedia_Text></Planet>
      <Planet Name="Beta"><Variant_Of_Existing_Type>Alpha</Variant_Of_Existing_Type>
      <Galactic_Position>99,0,10</Galactic_Position><Scale_Factor>TBD</Scale_Factor>
      <Planet_Ability_Name>MOD_NAME</Planet_Ability_Name><Planet_Ability_Description>MOD_DESCRIPTION</Planet_Ability_Description>
      <Planet_Ability_Icon>mod_bonus.tga</Planet_Ability_Icon></Planet>
      <SpaceUnit Name="Crusader_Gunship"><Icon_Name>i_gunship.tga</Icon_Name><Text_ID>TEXT_UNIT_GUNSHIP</Text_ID>
      <Encyclopedia_Text>TEXT_ENC_GUNSHIP_1, TEXT_ENC_MISSING TEXT_ENC_GUNSHIP_2</Encyclopedia_Text>
      <MP_Encyclopedia_Text>TEXT_ENC_MULTIPLAYER</MP_Encyclopedia_Text><Ranking_In_Category>90</Ranking_In_Category></SpaceUnit>
      <SpaceUnit Name="Modded_Ship"><Text_ID>TEXT_MISSING</Text_ID></SpaceUnit>
      <SpaceUnit Name="Modded_Variant"><Variant_Of_Existing_Type>Crusader_Gunship</Variant_Of_Existing_Type>
      <Icon_Name>i_variant.tga</Icon_Name><GUI_Row>7</GUI_Row></SpaceUnit>
      <Particle Name="Mod_Base"><In_Midground>Yes</In_Midground></Particle>
      <Particle Name="Mod_Line"><Variant_Of_Existing_Type>Mod_Base</Variant_Of_Existing_Type>
      <In_Midground>No</In_Midground><In_Midforeground>Yes</In_Midforeground></Particle>
    </Planets>)XML");
    write(root, "Data/XML/FactionFiles.xml", "<Faction_Files><File>Factions.xml</File></Faction_Files>");
    write(root, "Data/XML/Factions.xml", R"XML(<Factions><Faction Name="Test">
      <Faction_Leader>Modded_Ship</Faction_Leader>
      <Color>64,128,192,255</Color><Display_Font_Color>32,96,160,255</Display_Font_Color><Icon_Name>icon.tga</Icon_Name>
    </Faction><Faction Name="Neutral"><Color>100,100,100,255</Color></Faction></Factions>)XML");
    write(root, "Data/XML/CampaignFiles.xml", "<Campaign_Files><File>Campaigns.xml</File></Campaign_Files>");
    write(root, "Data/XML/Campaigns.xml", R"XML(<Campaigns><Campaign Name="Art">
      <Locations>Backdrop,Alpha,Beta</Locations><Starting_Active_Player>Test</Starting_Active_Player>
      <Home_Location>tEsT,aLpHa</Home_Location><Home_Location>TEST,Absent</Home_Location></Campaign></Campaigns>)XML");
    write(root, "Data/XML/HardPointDataFiles.xml", "<Hard_Point_Files/>");
    write(root, "Data/XML/SFXEventFiles.xml", "<SFXEvent_Files/>");
    write(root, "Data/XML/AbilityFiles.xml", "<AbilityFiles/>");
    write(root, "Data/XML/TradeRouteFiles.xml", "<Trade_Route_Files/>");
    write(root, "Data/XML/GameConstants.xml", R"XML(<GameConstants>
      <Galactic_Zoom_Out_Light_Angle>999,120,30</Galactic_Zoom_Out_Light_Angle>
      <Particle_Brightness_Per_Corruption_Level>0.2,1.4,99</Particle_Brightness_Per_Corruption_Level>
      <Particle_Scale_Per_Corruption_Level>2,4</Particle_Scale_Per_Corruption_Level>
      <Particle_Energy_Per_Corruption_Level>0,0.8</Particle_Energy_Per_Corruption_Level>
      <Corruption_Particle_Line_Name>Mod_Line</Corruption_Particle_Line_Name>
      <Corruption_Line_Radius>17</Corruption_Line_Radius><Corruption_Line_Start_End_Offset>23</Corruption_Line_Start_End_Offset>
      <Corruption_Line_Grow_Seconds>11</Corruption_Line_Grow_Seconds><Corruption_Path_Width>19</Corruption_Path_Width>
      <Corruption_Path_Color>37,53,11,0</Corruption_Path_Color><Corruption_Path_Offset>1,2,3</Corruption_Path_Offset>
      <Fleet_Hyperspace_Band_Texture_Name>mod_band.tga</Fleet_Hyperspace_Band_Texture_Name>
      <Animate_During_Galactic_Mode_Pause>No</Animate_During_Galactic_Mode_Pause></GameConstants>)XML");
    vfs::MountSpec mount;
    mount.layer_id = "test";
    mount.data_root = root / "Data";
    const std::array<vfs::MountSpec, 1> mounts{mount};
    auto filesystem = vfs::Vfs::mount(mounts);
    assert(filesystem);
    auto catalog = data::load_catalog_strict(filesystem.value(), data::Profile::foc);
    assert(catalog);
    assert(galactic_particle_layer(catalog.value().catalog, "Mod_Base") == SceneLayer::midground);
    assert(galactic_particle_layer(catalog.value().catalog, "Mod_Line") == SceneLayer::midforeground);
    production_names_localize_once(catalog.value().catalog);
    auto tables = data::galactic::load_galactic_data(filesystem.value(), catalog.value().catalog);
    assert(tables);
    planet_bonus_metadata(tables.value());
    assert(!tables.value().planet("Backdrop"));
    assert(tables.value().galaxy_art_object("Backdrop"));
    assert(tables.value().planet("Alpha") && tables.value().planet("Beta"));
    auto content = load_scene_content(filesystem.value(), catalog.value().catalog, tables.value(), "Art");
    if (!content) std::cerr << core::format_diagnostic(content.error()) << std::endl;
    assert(content);
    const auto& scene = content.value();
    assert(std::abs(scene.corruption_parameters.brightness[0]-0.2F) < 0.00001F
        && std::abs(scene.corruption_parameters.brightness[1]-1.4F) < 0.00001F);
    assert(scene.corruption_parameters.scale[1] == 4 && scene.corruption_parameters.energy[0] == 0);
    assert(scene.corruption_style.line_particle == "Mod_Line" && scene.corruption_style.radius == 17);
    assert(scene.corruption_style.endpoint_offset == 23 && scene.corruption_style.grow_seconds == 11);
    assert(scene.corruption_style.path_width == 19 && scene.corruption_style.path_colour[1] == 53);
    assert((scene.corruption_style.path_offset == Vec3{1,2,3}));
    assert(scene.corruption_style.path_texture == "mod_band.tga");
    assert(initial_home(scene, PlayerId{1}) && initial_home(scene, PlayerId{1})->name == "Alpha");
    assert(!initial_home(scene, PlayerId{999}));
    assert(scene.leader_types.at(1) == "Modded_Ship");
    const std::map<sim::galactic::TypeId, std::string> type_names{{7, "mOdDeD_sHiP"}, {9, "Other"}};
    assert(home_leader_type(scene, 1, type_names) == 7);
    assert(home_leader_type(scene, 999, type_names) == 0);
    assert(home_leader_type(scene, 1, {}) == 0);
    auto homes = scene;
    homes.home_types.at(1) = "aLpHa";
    assert(initial_home(homes, PlayerId{1}) == &homes.snapshot.planets[1]);
    homes.home_types.at(1) = "Absent";
    assert(!initial_home(homes, PlayerId{1})); // no fallback to the owned Alpha
    assert(scene.snapshot.planets.size() == 3);
    assert(scene.snapshot.planets[1].ability.icon == "base_bonus.tga");
    assert(scene.snapshot.planets[2].ability.name_key == "MOD_NAME"
        && scene.snapshot.planets[2].ability.description_key == "MOD_DESCRIPTION"
        && scene.snapshot.planets[2].ability.icon == "mod_bonus.tga");
    assert(scene.art.at(1).layer == SceneLayer::background);
    assert(scene.art.at(1).loop_idle);
    assert(scene.art.at(2).show_name && scene.art.at(2).label == "ALPHA");
    assert(scene.art.at(2).pick_radius == 17.0F);
    assert((scene.art.at(2).name_adjust == Vec3{1,2,3}));
    assert(scene.snapshot.planets[1].position[0] == sim::math::Fixed::from_decimal("12").value());
    assert(scene.snapshot.planets[1].scale == sim::math::Fixed::from_decimal("2.5").value());
    assert(scene.snapshot.planets[2].scale == scene.snapshot.planets[1].scale);
    assert(scene.snapshot.planets[1].owner && !scene.snapshot.planets[2].owner);
    assert(scene.factions.at(1).icon == "icon.tga");
    const auto* neutral = affiliation_for_owner(scene, scene.snapshot.planets[2].owner);
    assert(neutral && neutral->faction == "Neutral");
    assert(neutral->primary.r == 100.0F / 255.0F);
    assert(affiliation_for_owner(scene, scene.snapshot.planets[1].owner)->faction == "Test");
    assert(!affiliation_for_owner(scene, PlayerId{999}));
    const std::vector<data::ValueOverride> overrides{{"Alpha", "Scale_Factor", {"4"}}, {"Alpha", "Galactic_Model_Name", {"other.alo"}}};
    auto changed = data::with_overrides(catalog.value().catalog, overrides);
    assert(changed);
    auto changed_tables = data::galactic::load_galactic_data(filesystem.value(), changed.value());
    assert(changed_tables);
    auto changed_scene = load_scene_content(filesystem.value(), changed.value(), changed_tables.value(), "Art");
    assert(changed_scene);
    assert(changed_scene.value().snapshot.planets[1].scale == sim::math::Fixed::from_decimal("4").value());
    assert(changed_scene.value().snapshot.planets[1].model == "data/art/models/other.alo");
    assert(!load_scene_content(filesystem.value(), catalog.value().catalog, tables.value(), "Missing"));
    const std::vector<data::ValueOverride> no_neutral_colour{{"Neutral", "Color", {"invalid"}}};
    auto uncoloured = data::with_overrides(catalog.value().catalog, no_neutral_colour);
    assert(uncoloured);
    const auto rejected = load_scene_content(filesystem.value(), uncoloured.value(), tables.value(), "Art");
    assert(!rejected);
    assert(rejected.error().message == "campaign has no drawable Neutral faction Color");
    auto no_neutral_tables = tables.value();
    std::erase_if(no_neutral_tables.factions, [](const auto& faction) { return faction.name == "Neutral"; });
    const auto missing_neutral = load_scene_content(filesystem.value(), catalog.value().catalog, no_neutral_tables, "Art");
    assert(!missing_neutral);
    assert(missing_neutral.error().message == "campaign has no drawable Neutral faction Color");
    std::filesystem::remove_all(root);
}
}

int main() {
    campaign_art_is_data_driven();
    const auto light = galaxy_lighting({123, 0, 90});
    const auto same = galaxy_lighting({999, 0, 90});
    assert(light.lights[0].direction.x == same.lights[0].direction.x);
    assert(light.lights[0].direction.z == same.lights[0].direction.z);
    assert(std::abs(light.lights[0].direction.z + 1.0F) < 0.0001F);
    assert(light.lights[0].color.a == 0.5F && light.ambient.r == 0.05F);
    assert(light.lights[1].color.a == 0.0F && light.lights[2].color.a == 0.0F);
    assert(animation_elapsed(0.5, true, false) == 0.0);
    assert(animation_elapsed(0.5, true, true) == 0.5);
    assert(animation_elapsed(0.5, false, false) == 0.5);
    Affiliation current;
    Affiliation apparent{{0.1F,0.2F,0.3F}, {0.4F,0.5F,0.6F}, "Faction", "icon", true};
    update_affiliation(current, &apparent);
    update_affiliation(current, nullptr);
    assert(current.primary == apparent.primary && current.icon == "icon" && current.present);
    update_affiliation(current, &apparent, true);
    assert((current.secondary == Rgb{1,1,1}));
}
