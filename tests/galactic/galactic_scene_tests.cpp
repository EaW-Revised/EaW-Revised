#include "galactic/scene_content.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/core/text.hpp"
#include "eawr/campaign/mp_lobby_catalog.hpp"
#include "../support/env.hpp"
#include "../support/result.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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
    </Faction><Faction Name="Neutral"><Color>100,100,100,255</Color></Faction>
      <Faction Name="Other"><Color>192,64,128,255</Color></Faction></Factions>)XML");
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
    // Stock MP campaigns have faction rows but no solo Starting_Active_Player.
    auto multiplayer_tables = tables.value();
    auto multiplayer = multiplayer_tables.campaigns.front();
    multiplayer.name = "Multiplayer_Sandbox_Origin";
    multiplayer.is_multiplayer = true;
    multiplayer.starting_active_player.clear();
    multiplayer_tables.campaigns.push_back(std::move(multiplayer));
    const auto no_observer = load_scene_content(filesystem.value(), catalog.value().catalog, multiplayer_tables,
        "Multiplayer_Sandbox_Origin");
    assert(!no_observer && no_observer.error().message == "multiplayer scene requires a local observer faction");
    for (const auto faction : {"Test", "Other"}) {
        auto local = load_scene_content(filesystem.value(), catalog.value().catalog, multiplayer_tables,
            "Multiplayer_Sandbox_Origin", faction);
        assert(local);
        const auto player = local.value().snapshot.seats.front().player;
        assert(local.value().factions.at(player).faction == faction);
    }
    assert(!load_scene_content(filesystem.value(), catalog.value().catalog, multiplayer_tables,
        "Multiplayer_Sandbox_Origin", ""));
    assert(!load_scene_content(filesystem.value(), catalog.value().catalog, multiplayer_tables,
        "Multiplayer_Sandbox_Origin", "Absent"));
    assert(!load_scene_content(filesystem.value(), catalog.value().catalog, tables.value(), "Art", "Other"));
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
    // #4575: a roster numbering (the lobby's) replaces the scene's own; the faction behind an id follows the roster.
    auto rostered = scene;
    rebind_players(rostered, {{5, "NEUTRAL"}, {3, "test"}, {9, "Missing"}, {3, "Neutral"}});
    assert(rostered.snapshot.players.size() == 2 && rostered.factions.size() == 2);
    assert(rostered.factions.at(3).faction == "Test" && rostered.factions.at(5).faction == "Neutral");
    assert(!rostered.factions.contains(1) && !rostered.factions.contains(9));
    assert(affiliation_for_owner(rostered, PlayerId{3})->primary == scene.factions.at(1).primary);
    assert(affiliation_for_owner(rostered, std::nullopt)->faction == "Neutral");
    assert(rostered.leader_types.at(3) == "Modded_Ship" && core::iequals(rostered.home_types.at(3), "Alpha"));
    assert(rostered.snapshot.planets[1].owner == std::optional<PlayerId>{3});
    assert(!rostered.snapshot.planets[2].owner);
    assert(initial_home(rostered, PlayerId{3}) && !initial_home(rostered, PlayerId{1}));
    std::filesystem::remove_all(root);
}
}

// #4575: every playable faction of the installed data (base and expansion) draws with its authored Color. The
// expected value is read straight from the faction XML files, not through the catalog the viewer uses.
std::array<float, 4> authored_colour(const std::string& text) {
    std::array<float, 4> values{0, 0, 0, 255};
    std::string cleaned = text;
    std::replace(cleaned.begin(), cleaned.end(), ',', ' ');
    std::istringstream stream(cleaned);
    for (auto& value : values) if (!(stream >> value)) break;
    return values;
}

int stock_faction_colours() {
    const auto root = eawr::testing::game_root();
    if (!root) { std::cout << "SKIPPED: installed game data requires EAWR_EAW_GAME_ROOT\n"; return 77; }
    std::vector<vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(eawr::testing::must(vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data")).mount);
    auto files = eawr::testing::must(vfs::Vfs::mount(mounts));
    const auto loaded = eawr::testing::must(data::load_catalog_strict(files, data::Profile::foc));
    const auto tables = eawr::testing::must(data::galactic::load_galactic_data(files, loaded.catalog));
    const auto scene = eawr::testing::must(load_scene_content(files, loaded.catalog, tables, "Sandbox_Equal_Footing_Empire"));

    std::map<std::string, std::array<float, 4>> expected;
    const auto registry = eawr::testing::must(data::load_document(files, "data/xml/factionfiles.xml"));
    for (const auto& entry : registry.root.children) {
        if (!core::iequals(entry.name, "File")) continue;
        std::string path = entry.raw_text;
        path = core::trim(path, core::xml_whitespace);
        const auto document = eawr::testing::must(data::load_document(files, "data/xml/" + path));
        for (const auto& faction : document.root.children) {
            if (!core::iequals(faction.name, "Faction")) continue;
            for (const auto& attribute : faction.attributes) if (core::iequals(attribute.name, "Name"))
                for (const auto& tag : faction.children)
                    if (core::iequals(tag.name, "Color")) expected[core::to_lower(attribute.value)] = authored_colour(tag.raw_text);
        }
    }
    std::size_t checked = 0;
    bool underworld = false;
    for (const auto& faction : tables.factions) {
        if (!faction.is_playable.value_or(false)) continue;
        const auto want = expected.find(core::to_lower(faction.name));
        if (want == expected.end()) { std::cerr << "no authored Color for " << faction.name << std::endl; return 1; }
        const auto player = std::find_if(scene.snapshot.players.begin(), scene.snapshot.players.end(),
            [&](const auto& row) { return core::iequals(row.faction, faction.name); });
        if (player == scene.snapshot.players.end()) {
            std::cerr << "playable faction has no scene player: " << faction.name << '\n'; return 1;
        }
        const auto* appearance = affiliation_for_owner(scene, player->id);
        if (!appearance) { std::cerr << "no affiliation for " << faction.name << '\n'; return 1; }
        const std::array<float, 3> shown{appearance->primary.r, appearance->primary.g, appearance->primary.b};
        for (std::size_t i = 0; i < 3; ++i) {
            if (std::abs(shown[i] - want->second[i] / 255.0F) > 0.0001F
                || player->colour[i] != static_cast<std::uint8_t>(want->second[i])) {
                std::cerr << "faction colour differs from its authored Color: " << faction.name << '\n'; return 1;
            }
        }
        if (core::iequals(faction.name, "Underworld")) {
            underworld = true;
            assert(player->colour[0] == 247 && player->colour[1] == 201 && player->colour[2] == 13);
        }
        ++checked;
    }
    if (checked < 3 || !underworld) { std::cerr << "too few playable factions checked: " << checked << std::endl; return 1; }
    // The multiplayer lobby numbers only a campaign's own factions, so its ids differ from the scene's.
    std::size_t lobby_checked = 0, renumbered = 0;
    for (const auto& lobby : campaign::list_mp_lobby_campaigns(tables)) {
        // The stock multiplayer campaigns author no active faction, so no scene loads for them; faction colours do not
        // depend on the campaign, so the solo scene stands in and only the lobby's numbering is under test.
        auto rostered = scene;
        std::vector<std::pair<PlayerId, std::string>> roster;
        for (const auto& faction : lobby.factions) roster.emplace_back(faction.player, faction.name);
        for (const auto& faction : lobby.factions) {
            const auto* before = affiliation_for_owner(scene, PlayerId{faction.player});
            if (faction.playable && (!before || !core::iequals(before->faction, faction.name))) ++renumbered;
        }
        rebind_players(rostered, roster);
        for (const auto& faction : lobby.factions) {
            if (!faction.playable) continue;
            const auto want = expected.find(core::to_lower(faction.name));
            if (want == expected.end()) { std::cerr << "no authored Color for " << faction.name << std::endl; return 1; }
            const auto* appearance = affiliation_for_owner(rostered, PlayerId{faction.player});
            if (!appearance || !core::iequals(appearance->faction, faction.name)
                || std::abs(appearance->primary.r - want->second[0] / 255.0F) > 0.0001F
                || std::abs(appearance->primary.g - want->second[1] / 255.0F) > 0.0001F
                || std::abs(appearance->primary.b - want->second[2] / 255.0F) > 0.0001F) {
                std::cerr << "lobby " << lobby.name << ": " << faction.name << " does not draw with its authored Color\n"; return 1;
            }
            ++lobby_checked;
        }
    }
    // The scene's own numbering really does name another faction for lobby ids (the Underworld case of this bug).
    if (renumbered == 0) { std::cerr << "no lobby id differs from the scene numbering" << std::endl; return 1; }
    if (lobby_checked == 0) { std::cerr << "no lobby faction checked" << std::endl; return 1; }
    std::cout << "faction colours checked: " << checked << ", lobby: " << lobby_checked << '\n';
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--stock-colours") {
        try { return stock_faction_colours(); }
        catch (const std::exception& error) { std::cerr << "stock colours: " << error.what() << std::endl; return 1; }
    }
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
