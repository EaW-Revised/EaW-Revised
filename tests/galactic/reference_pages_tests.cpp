#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "../support/temp_dir.hpp"
#include "eawr/presentation/ui/icon_fit.hpp"
#include "eawr/vfs/vfs.hpp"
#include "../support/check.hpp"
#include "galactic/reference_catalog.hpp"
#include "galactic/planet_label_scale.hpp"
#include "galactic/command_bar_scale.hpp"
#include "galactic/hud_retained.hpp"
#include "galactic/advisor_hints.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

#include <cmath>
#include <iostream>
#include <exception>

namespace page = eawr::viewer::galactic;
namespace sim = eawr::sim::galactic;
namespace {
void require(bool value, const char* message) {
    if (!value) eawr::testing::require(value, message);
}
struct CatalogFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("eawr-reference-catalog-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    explicit CatalogFixture(bool hyphen_first = false) {
        std::filesystem::create_directories(root / "XML");
        for (const auto& [file, element] : {std::pair{"HardpointDataFiles", "Hard_Point_Files"},
                std::pair{"FactionFiles", "Faction_Files"}, std::pair{"CampaignFiles", "Campaign_Files"},
                std::pair{"SFXEventFiles", "SFXEvent_Files"}})
            std::ofstream(root / "XML" / (std::string(file)+".xml")) << "<" << element << "/>";
        std::ofstream(root / "XML/GameObjectFiles.xml") << "<Game_Object_Files><File>Objects.xml</File></Game_Object_Files>";
        std::ofstream objects(root / "XML/Objects.xml");
        objects << R"xml(<GameObjectData>
          <GroundCompany Name="Fixture_Trooper"><Affiliation>Empire</Affiliation><Icon_Name>trooper.tga</Icon_Name>
            <Icon_Name></Icon_Name>
            <Text_ID>TEXT_TROOPER</Text_ID><Encyclopedia_Unit_Class>TEXT_INFANTRY</Encyclopedia_Unit_Class>
            <Encyclopedia_Text>TEXT_DESCRIPTION</Encyclopedia_Text><Tech_Level>1</Tech_Level></GroundCompany>
          <SpaceUnit Name="Fixture_Ship"><Affiliation>Empire</Affiliation><Icon_Name>ship.tga</Icon_Name>
            <Text_ID>TEXT_SHIP</Text_ID><Tech_Level>2</Tech_Level><Build_Initially_Locked>Yes</Build_Initially_Locked></SpaceUnit>
          <HeroCompany Name="Fixture_Hero"><Icon_Name>first.tga</Icon_Name><Icon_Name>hero.tga</Icon_Name>
            <Icon_Name></Icon_Name><Icon_Name>TBD</Icon_Name></HeroCompany>
          <HeroCompany Name="Fixture_Hero_Variant"><Variant_Of_Existing_Type>Fixture_Hero</Variant_Of_Existing_Type>
            <Icon_Name> </Icon_Name><Icon_Name>TBD</Icon_Name></HeroCompany>
          <SpaceUnit Name="Fixture_Foreign"><Affiliation>Rebel</Affiliation><Icon_Name>foreign.tga</Icon_Name></SpaceUnit>
          <SpaceUnit Name="Renamed-Mod-Ship"><Affiliation>Empire</Affiliation><Icon_Name>renamed.tga</Icon_Name>
            <Text_ID>TEXT_RENAMED</Text_ID><Tech_Level>2</Tech_Level><Build_Initially_Locked>Yes</Build_Initially_Locked></SpaceUnit>
          <SpaceUnit Name="Outside-Palette"><Affiliation>Empire</Affiliation><Icon_Name>outside.tga</Icon_Name></SpaceUnit>
          <SpaceUnit Name="Disabled-Mod-Ship"><Affiliation>Empire</Affiliation><Icon_Name>disabled.tga</Icon_Name></SpaceUnit>
          <SpaceUnit Name="No-Slot"><Affiliation>Empire</Affiliation><Icon_Name>no-slot.tga</Icon_Name></SpaceUnit>
          <Planet Name="Fixture_Planet"><Icon_Name>planet.tga</Icon_Name><Additional_Population_Capacity>5</Additional_Population_Capacity></Planet>
          <StarBase Name="Fixture_Base"><Icon_Name>base.tga</Icon_Name><Additional_Population_Capacity>3</Additional_Population_Capacity></StarBase>
          <GroundStructure Name="Fixture_Mine"><Icon_Name>mine.tga</Icon_Name><Abilities><Planet_Income_Bonus_Ability>
            <Specific_Mod_Source_Text>TEXT_ECONOMY_MINING_FACILITIES</Specific_Mod_Source_Text>
          </Planet_Income_Bonus_Ability></Abilities></GroundStructure>
        )xml";
        const auto exact = R"xml(<SpaceUnit Name="Mod_Dup"><Affiliation>Empire</Affiliation><Icon_Name>dup-a.tga</Icon_Name></SpaceUnit>)xml";
        const auto normalized = R"xml(<SpaceUnit Name="Mod-Dup"><Affiliation>Empire</Affiliation><Icon_Name>dup-b.tga</Icon_Name></SpaceUnit>)xml";
        objects << (hyphen_first ? normalized : exact) << (hyphen_first ? exact : normalized) << "</GameObjectData>";
    }
    ~CatalogFixture() { eawr::testing::remove_temp_directory(root); }
};
void populated_catalog() {
    CatalogFixture fixture;
    auto filesystem = eawr::vfs::Vfs::mount(std::array{eawr::vfs::MountSpec{"fixture", fixture.root, "data", {}}});
    require(static_cast<bool>(filesystem), "reference catalog fixture mounts");
    auto loaded = eawr::data::load_catalog_strict(filesystem.value(), eawr::data::Profile::foc);
    require(static_cast<bool>(loaded), "reference catalog fixture strictly loads");
    auto startup = std::make_shared<const eawr::data::Catalog>(std::move(loaded).value().catalog);
    page::ReferenceCatalog source(*startup, [](std::string_view key) { return "localized:"+std::string(key); });
    startup.reset(); // Reproduce startup releasing its temporary owner before help opens.
    eawr::data::ui::Dialog land, space;
    const auto add = [](auto& dialog, const char* id) {
        eawr::data::ui::DialogControl control; control.id_name = id; dialog.controls.push_back(control);
    };
    add(land, "IDC_TECHTREE_FIXTURE_TROOPER"); add(land, "IDC_TECHTREE_FIXTURE_FOREIGN");
    add(land, "IDC_TECHTREE_MISSING"); add(space, "IDC_TECHTREE_FIXTURE_SHIP");
    const std::map<std::uint64_t, std::string> names{{10,"Fixture_Trooper"},{11,"Fixture_Ship"}};
    page::TreeFacts tree; tree.technology = 2;
    tree.land = source.tree_slots(&land, "Empire", names); tree.space = source.tree_slots(&space, "Empire", names);
    const auto ground = page::tree_view(tree, false), orbit = page::tree_view(tree, true);
    require(ground.size() == 1 && ground[0].control == "IDC_TECHTREE_FIXTURE_TROOPER"
        && ground[0].type.icon == "trooper.tga" && ground[0].type.type == 10, "owned catalog produces expected faction land slot/icon after startup release");
    require(ground[0].type.description == "localized:TEXT_INFANTRY\nlocalized:TEXT_DESCRIPTION", "loaded encyclopedia class and text reach hover");
    require(orbit.size() == 1 && orbit[0].type.icon == "ship.tga" && orbit[0].type.type == 11 && orbit[0].dimmed,
        "owned catalog produces expected space slot/icon and initial lock");
    eawr::data::ui::Dialog mod;
    add(mod, "IDC_TECHTREE_RENAMED_MOD_SHIP"); add(mod, "IDC_TECHTREE_OUTSIDE_PALETTE");
    add(mod, "IDC_TECHTREE_DISABLED_MOD_SHIP"); mod.controls.back().style = {"WS_DISABLED"};
    add(mod, "IDC_TECHTREE_FIXTURE_FOREIGN"); add(mod, "IDC_TECHTREE_UNREGISTERED");
    const std::map<std::uint64_t, std::string> mod_names{{77,"renamed-mod-ship"}};
    page::TreeFacts mod_tree; mod_tree.technology = 2;
    mod_tree.land = source.tree_slots(&mod, "Empire", mod_names);
    const auto mod_view = page::tree_view(mod_tree, false);
    require(mod_view.size() == 2, "complete registry admits outside-palette nodes but excludes missing, disabled and foreign slots");
    const auto renamed = std::find_if(mod_tree.land.begin(), mod_tree.land.end(),
        [](const auto& slot) { return slot.control == "IDC_TECHTREE_RENAMED_MOD_SHIP"; });
    require(renamed != mod_tree.land.end() && renamed->type.icon == "renamed.tga" && renamed->type.type == 77,
        "hyphenated registered name maps to underscore gadget while preserving copied unlock identity");
    require(renamed->initially_locked && renamed->required_tech == 2,
        "hyphen normalization retains registered tech and initial-lock facts");
    renamed->unlocked = true;
    const auto released = page::tree_view(mod_tree, false);
    require(std::none_of(released.begin(), released.end(), [](const auto& slot) { return slot.dimmed; }),
        "copied unlock releases hyphenated node without a production palette entry for the other node");
    require(source.tree_slots(nullptr, "Empire", mod_names).empty(), "missing authored dialog publishes no invented nodes");
    require(!source.metadata("Missing"), "unresolved metadata stays absent instead of a cached default");
    require(source.metadata("Fixture_Mine")->mining_facility && source.metadata("Fixture_Mine")->icon.icon == "mine.tga",
        "mining classification and icon come from the owned data source");
    const auto hero = source.metadata("Fixture_Hero"), inherited_hero = source.metadata("Fixture_Hero_Variant");
    require(hero && inherited_hero && hero->icon.icon == "hero.tga" && inherited_hero->icon.icon == "hero.tga",
        "ML-23/24: reference metadata keeps the last real icon through repeated and inherited placeholders");
    sim::ObserverSetup observer; observer.human_observers = {7}; observer.solo_reference_human = 7;
    sim::ObservedPlanet known; known.player = 7; known.planet = 1; known.discovered = true; known.known_owner = 7;
    known.knowledge.ownership = known.knowledge.structures = known.knowledge.fleet_contents = true;
    auto observations = std::make_shared<std::vector<sim::ObservedPlanet>>(1, known);
    sim::ProductionSetup production; sim::PlanetProduction producer; producer.planet = 1; producer.starbase_type = 12;
    production.planets = {producer};
    sim::Fleet hero_fleet; hero_fleet.id = 100; hero_fleet.owner = 7; hero_fleet.orbit = 1;
    hero_fleet.members = {{101,13},{102,14}};
    sim::GalacticSnapshot snapshot(1, {}, {}, {}, {}, {hero_fleet}, {}, {}, {}, {}, {}, {}, {}, production,
        {}, {}, {}, observer, {}, {}, {}, observations);
    page::PlanetHudFacts hud; hud.planet = 1; hud.discovered = true; hud.observed_owner = 7; hud.display_name = "Fixture";
    std::map<std::uint64_t, page::ReferenceType> types{{12,*source.metadata("Fixture_Base")}, {13,*hero}, {14,*inherited_hero}};
    types[13].gui_visibility_known = types[14].gui_visibility_known = true;
    std::map<page::ObjectId, page::ReferenceType> planets{{1,*source.metadata("Fixture_Planet")}};
    const auto rows = page::published_summary_planets(snapshot, 7, {{1,hud}}, types, planets);
    require(rows.size() == 1 && rows[0].capacity == 8 && rows[0].icon == "planet.tga", "loaded planet and producer metadata publish nonzero Summary capacity and planet icon");
    const auto& portraits = rows[0].orbit[0];
    require(portraits && portraits->size() == 2 && (*portraits)[0].icon == "hero.tga"
        && (*portraits)[1].icon == "hero.tga",
        "#7373: reference summary publication retains both hero portraits instead of skipping blank raw icons");
    require(!page::published_summary_planets(snapshot, 7, {{1,hud}}, types, {})[0].capacity,
        "missing planet metadata leaves Summary capacity unavailable");
    require(!page::published_summary_planets(snapshot, 7, {{1,hud}}, {}, planets)[0].capacity,
        "missing producer metadata leaves Summary capacity unavailable");
}
void colliding_catalog_names() {
    for (const bool hyphen_first : {false, true}) {
        CatalogFixture fixture(hyphen_first);
        auto filesystem = eawr::vfs::Vfs::mount(std::array{eawr::vfs::MountSpec{"fixture", fixture.root, "data", {}}});
        require(static_cast<bool>(filesystem), "collision fixture mounts");
        auto loaded = eawr::data::load_catalog_strict(filesystem.value(), eawr::data::Profile::foc);
        require(static_cast<bool>(loaded), "collision fixture strictly loads");
        page::ReferenceCatalog source(loaded.value().catalog, [](std::string_view key) { return std::string(key); });
        eawr::data::ui::Dialog dialog;
        eawr::data::ui::DialogControl control; control.id_name = "IDC_TECHTREE_MOD_DUP";
        dialog.controls.push_back(control);
        const auto slots = source.tree_slots(&dialog, "Empire", {{81, "Mod_Dup"}, {82, "Mod-Dup"}});
        require(slots.size() == 1, "one authored control yields one node in either definition order");
        require(slots[0].control == "IDC_TECHTREE_MOD_DUP" && slots[0].type.icon == "dup-a.tga"
            && slots[0].type.type == 81, "exact id wins over hyphen-normalized alias regardless of load order");
    }
}
void mod_label_scale() {
    CatalogFixture fixture;
    const auto base = fixture.root / "base", mod = fixture.root / "mod";
    std::filesystem::create_directories(base / "XML");
    std::filesystem::create_directories(mod / "XML");
    std::ofstream(base / "XML/CommandBarComponentFiles.xml")
        << "<CommandBar_Component_Files><File>Components.xml</File></CommandBar_Component_Files>";
    std::ofstream(base / "XML/Components.xml")
        << "<CommandBarComponents><CommandBarComponent Name='g_planet_name'><Type>TextButton</Type><Scale>1</Scale><Text_Offset>0 0</Text_Offset></CommandBarComponent>"
        << "<CommandBarComponent Name='g_ground_level_pips'><Type>Button</Type><Scale>1.0</Scale></CommandBarComponent>"
        << "<CommandBarComponent Name='g_planet_fleet'><Type>Button</Type><Scale>1.0</Scale></CommandBarComponent>"
        << "<CommandBarComponent Name='g_build'><Type>Button</Type><Scale>1.0</Scale></CommandBarComponent></CommandBarComponents>";
    std::ofstream(mod / "XML/Components.xml")
        << "<CommandBarComponents><CommandBarComponent Name='g_planet_name'><Type>TextButton</Type><Scale>0.8</Scale><Text_Offset>0 -15</Text_Offset></CommandBarComponent></CommandBarComponents>";
    auto mounted = eawr::vfs::Vfs::mount(std::array{eawr::vfs::MountSpec{"mod", mod, "data", {}},
        eawr::vfs::MountSpec{"base", base, "data", {}}});
    require(static_cast<bool>(mounted), "mod and base command-bar layers mount");
    auto loaded = eawr::data::ui::load_command_bar(mounted.value());
    require(static_cast<bool>(loaded), "effective command bar loads");
    const auto* style = loaded.value().catalog.find("g_planet_name");
    require(style != nullptr, "effective planet name component exists");
    require(style->vec2(eawr::data::ui::Field::text_offset) == eawr::data::ui::Vec2{0, -15},
        "effective mod layer supplies its name text offset");
    const auto near = [](float a, float b) { return std::abs(a - b) < .0001F; };
    const auto place = [](const auto& component) {
        return page::planet_label_placement(component, 200, 100, 2, 100, 20, 100, 20);
    };
    require(near(page::command_bar_component_scale(*style, 2), 1.6F),
        "authored component scale multiplies viewport scale for overview icon geometry");
    eawr::data::ui::CommandBarComponent face_style;
    face_style.fields.push_back({eawr::data::ui::Field::blank_texture_name, std::vector<std::string>{"blank.tga"}, 0});
    face_style.fields.push_back({eawr::data::ui::Field::icon_texture_name, std::vector<std::string>{"icon.tga"}, 0});
    require(page::command_bar_text_button_face(face_style) == "blank.tga", "text button uses authored blank face");
    face_style.fields.erase(face_style.fields.begin());
    require(page::command_bar_text_button_face(face_style) == "icon.tga", "text button falls back to icon face");
    const eawr::data::ui::ShellBone button_bone{"test_button", {100, -20}};
    const auto button_anchor = page::bone_button_anchor(button_bone, 200, 40,
        page::command_bar_component_scale(*style, 1));
    require(button_anchor.rect == eawr::data::ui::ReferenceRect{20, -36, 160, 32}
        && button_anchor.origin && button_anchor.origin->x == 100 && button_anchor.origin->y == -20,
        "bone-only button centres the authored scaled face on its bone");
    auto mesh_anchor = button_anchor;
    mesh_anchor.rect = {0, 0, 5, 10}; // Mesh bounds need not centre the button's state art.
    require(page::button_face_rect(mesh_anchor, 200, 40, page::command_bar_component_scale(*style, 1))
        == eawr::data::ui::ReferenceRect{20, -36, 160, 32},
        "mesh-backed button artwork uses its bone and texture, not the mesh rectangle");
    // #8369: an authored Scale of 0.001 leaves no pixel of a button face, and its caption goes with it.
    const auto hidden_face = page::button_face_rect(button_anchor, 200, 40, 0.0015F);
    require(page::button_face_collapsed(hidden_face), "a 0.001 Scale collapses the button face");
    require(!page::button_face_collapsed({0, 0, 200, 40}) && !page::button_face_collapsed({0, 0, 1, 1}),
        "an authored face of at least one pixel stays drawn");
    const auto scaled = place(*style);
    require(near(scaled.scale, .8F) && near(scaled.x, 160) && near(scaled.y, 122)
        && near(scaled.bounds.x, 160) && near(scaled.bounds.y, 122)
        && near(scaled.bounds.width, 80) && near(scaled.bounds.height, 16),
        "mod scale and text offset determine placement and culling bounds");
    auto variant = *style;
    const auto set_scale = [&](float value) {
        for (auto& entry : variant.fields) if (entry.field == eawr::data::ui::Field::scale) entry.value = value;
    };
    set_scale(.5F);
    require(near(page::command_bar_component_scale(variant, 1.5F), .75F),
        "half-scale pip and fleet components occupy half the viewport-scaled texture size");
    set_scale(1);
    require(near(page::command_bar_component_scale(variant, 1.5F), 1.5F),
        "unit component scale retains stock overview geometry");
    // Stock-like XML gives each of these components scale 1. Compare the
    // resulting dimensions with the pre-change viewport-only calculation.
    auto stock_files = eawr::vfs::Vfs::mount(std::array{eawr::vfs::MountSpec{"stock", base, "data", {}}});
    require(static_cast<bool>(stock_files), "stock-like command bar mounts");
    auto stock_bar = eawr::data::ui::load_command_bar(stock_files.value());
    require(static_cast<bool>(stock_bar), "stock-like command bar loads");
    for (const auto& [name, texture_width] : {std::pair{"g_ground_level_pips", 16.0F},
            std::pair{"g_planet_fleet", 50.0F}, std::pair{"g_build", 32.0F}}) {
        const auto* component = stock_bar.value().catalog.find(name);
        require(component != nullptr, "stock-like component exists");
        const float before = texture_width * 1.5F;
        const float after = texture_width * page::command_bar_component_scale(*component, 1.5F);
        require(after == before, "stock pip, fleet and status dimensions match main");
    }
    // #8414: a ground pad or structure slot icon is the texture times viewport scale times the authored Scale.
    for (const auto& [authored, expected] : {std::pair{1.5F, 2.25F}, std::pair{0.9F, 1.35F}}) {
        eawr::data::ui::CommandBarComponent slot_style;
        slot_style.fields.push_back({eawr::data::ui::Field::scale, authored, 0});
        const auto drawn = eawr::presentation::ui::fit_icon_rect(100, 100, 40, 20,
            page::command_bar_component_scale(slot_style, 1.5F), eawr::presentation::ui::IconFitMode::natural);
        require(near(drawn.width, 40 * expected) && near(drawn.height, 20 * expected)
            && near(drawn.x, (100 - drawn.width) * .5F) && near(drawn.y, (100 - drawn.height) * .5F),
            "slot icon size follows the authored component scale and stays centred");
    }
    eawr::data::ui::CommandBarComponent stock_slot;
    require(page::command_bar_component_scale(stock_slot, 1.5F) == 1.5F,
        "a slot component with no Scale keeps the viewport-only icon size");
    auto default_scale = variant;
    std::erase_if(default_scale.fields, [](const auto& field) { return field.field == eawr::data::ui::Field::scale; });
    require(page::command_bar_component_scale(default_scale, 1.5F) == 1.5F,
        "an omitted component scale keeps the main-branch dimensions");
    set_scale(0);
    const auto zero = place(variant);
    require(near(zero.x, 200) && near(zero.y, 130) && near(zero.bounds.x, 200)
        && near(zero.bounds.y, 130) && near(zero.bounds.width, 0) && near(zero.bounds.height, 0),
        "zero scale collapses the quad at the offset anchor");
    set_scale(-.8F);
    const auto negative = place(variant);
    require(near(negative.x, 240) && near(negative.y, 138) && near(negative.bounds.x, 160)
        && near(negative.bounds.y, 122) && near(negative.bounds.width, 80) && near(negative.bounds.height, 16),
        "negative scale normalizes the culling bounds around the flipped quad");
    set_scale(1);
    for (auto& entry : variant.fields) if (entry.field == eawr::data::ui::Field::text_offset)
        entry.value = eawr::data::ui::Vec2{};
    const auto stock = place(variant);
    require(near(stock.x, 150) && near(stock.y, 90) && near(stock.bounds.x, 150)
        && near(stock.bounds.y, 90) && near(stock.bounds.width, 100) && near(stock.bounds.height, 20),
        "stock scale preserves the original centered geometry");
    variant.fields.push_back({eawr::data::ui::Field::offset, eawr::data::ui::Vec2{10, 99}, 0});
    const auto offset = place(variant);
    require(near(offset.x, 170) && near(offset.y, 90) && near(offset.bounds.x, 170),
        "template X offset applies at HUD scale while template Y remains unused");
    variant.fields.pop_back();
    set_scale(2);
    const auto edge = page::planet_label_placement(variant, -50, 0, 1, 100, 20, 100, 20);
    page::HudBounds reach; reach.include(edge.bounds.x, edge.bounds.y, edge.bounds.width, edge.bounds.height, -50, 0);
    require(!reach.outside({1280, 720}, -50, 0, 0), "enlarged label remains visible at the screen edge");
}
void history() {
    page::HistoryFacts facts;
    facts.local_player = 7; facts.multiplayer = true; facts.fiscal_frames = 30; facts.planetary_objects = 9;
    facts.factions = {{7, "Local", {1,0,0}, {{30,2,1,20,4},{60,3,2,40,8}}},
        {8, "Other", {0,1,0}, {{0,5,3,100,20},{90,6,4,200,40}}}};
    const auto economy = page::history_view(facts, page::HistoryMetric::economy);
    require(economy.available && economy.maximum == 200 && economy.lines.size() == 1, "MP suppresses lines without changing global scale");
    require(economy.end_cycle == 4 && economy.middle_cycle == 2, "fiscal labels use max frame plus one");
    require(std::abs(economy.lines[0].points[0][0]-1.0/3) < .00001 && economy.lines[0].points[1][1] == .2, "common archive frame domain and metric scaling");
    const auto military = page::history_view(facts, page::HistoryMetric::military);
    require(military.maximum == 40 && military.lines[0].points[1][1] == .2, "military uses its own maximum");
    require(page::history_view(facts, page::HistoryMetric::planets).maximum == 9, "planet object count participates in axis");
    facts.multiplayer = false;
    require(page::history_view(facts, page::HistoryMetric::economy).lines.size() == 2, "SP draws all faction archives");
    facts.factions[1].samples.resize(1);
    require(!page::history_view(facts, page::HistoryMetric::economy).available, "each faction needs two real samples");
    facts.factions.resize(1); facts.factions[0].samples[0].economy = -1; facts.factions[0].samples[1].economy = 0;
    require(page::history_view(facts, page::HistoryMetric::economy).maximum == 1, "nonpositive maximum becomes one");
    // Published MP facts contain no foreign line. Scales are copied separately.
    const auto recorded = [](std::int64_t n) { return sim::HistoryValue::from_fixed(eawr::sim::math::Fixed::from_raw(n * eawr::sim::math::Fixed::scale)); };
    sim::HistoryArchive archives{{7, {{30,2,1,recorded(20),recorded(4)},{60,3,2,recorded(40),recorded(8)}}},
        {8, {{0,5,3,recorded(100),recorded(20)},{90,6,4,recorded(200),recorded(40)}}}};
    const std::array<sim::PlayerId, 2> present{7,8};
    facts.published = sim::publish_history(archives, present, 9, 30, 7, true);
    facts.factions = {{7, "Local", {1,0,0}, {{30,2,1,20,4},{60,3,2,40,8}}}};
    const auto published = page::history_view(facts, page::HistoryMetric::economy);
    require(published.available && published.lines.size() == 1 && published.maximum == 200
        && published.end_cycle == 4 && std::abs(published.lines[0].points[0][0]-1.0/3) < .00001,
        "authorized local line uses published global frame domain and maximum without foreign samples");
    require(page::history_view(facts, page::HistoryMetric::military).maximum == 40
        && page::history_view(facts, page::HistoryMetric::planets).maximum == 9,
        "published military and planetary-object scales remain global");
    facts.published->available = false;
    require(!page::history_view(facts, page::HistoryMetric::economy).available,
        "published archive unavailability is not reconstructed from the visible line");
}
void tree_dialog_choice() {
    const std::map<std::string, bool> dialogs{{"IDD_TECH_TREE_REBEL", true}, {"IDD_TECH_TREE_REBEL_SPACE", true},
        {"IDD_TECH_TREE_EMPIRE", true}, {"IDD_TECH_TREE_EMPIRE_SPACE", true}, {"IDD_TECH_TREE_BLACKMARKET", false},
        {"IDD_TECH_TREE_BLACKMARKET_SPACE", true}, {"IDD_TECH_TREE_FIFTH", true}, {"IDD_TECH_TREE_FIFTH_SPACE", true},
        {"IDD_TECH_TREE_FOURTH", true}, {"IDD_TECH_TREE_FOURTH_COMBINED", false}};
    const auto exists = [&](std::string_view name) { return dialogs.contains(std::string(name)); };
    const auto switch_of = [&](std::string_view name) { const auto found = dialogs.find(std::string(name)); return found != dialogs.end() && found->second; };
    auto pick = [&](std::string_view name) { return page::choose_tree_dialogs(name, exists, switch_of); };
    const auto empire = pick("IDD_TECH_TREE_EMPIRE");
    require(empire.land == "IDD_TECH_TREE_EMPIRE" && empire.space == "IDD_TECH_TREE_EMPIRE_SPACE" && !empire.combined, "authored dialog with radios gets a _SPACE variant");
    const auto combined = pick("IDD_TECH_TREE_BLACKMARKET");
    require(combined.combined && combined.land == combined.space && combined.land == "IDD_TECH_TREE_BLACKMARKET", "dialog without radios is a combined diagram");
    const auto fifth = pick("IDD_TECH_TREE_FIFTH");
    require(fifth.land == "IDD_TECH_TREE_FIFTH" && fifth.space == "IDD_TECH_TREE_FIFTH_SPACE" && !fifth.combined,
        "a synthetic fourth faction's authored name is used, not a faction-name branch");
    const auto fourth = pick("IDD_TECH_TREE_FOURTH");
    require(fourth.land == "IDD_TECH_TREE_FOURTH" && fourth.space == "IDD_TECH_TREE_REBEL_SPACE" && !fourth.combined,
        "an authored dialog with radios but no Space sibling falls back to the Rebel Space dialog, never a blank tab");
    require(pick("IDD_TECH_TREE_FOURTH_COMBINED").combined, "a synthetic combined diagram is read from the dialog, not the faction");
    require(pick("").land == "IDD_TECH_TREE_REBEL" && pick("IDD_TECH_TREE_MISSING").space == "IDD_TECH_TREE_REBEL_SPACE", "missing or unauthored dialog falls back to Rebel");
}
void tree() {
    page::TreeFacts facts; facts.technology = 2;
    facts.land = {{"authored-a", {"a", "A", "encyclopedia", 10}, 2, true, false, false},
        {"authored-b", {"b", "B", "", 11}, 3, true, false, true},
        {"authored-c", {"c", "C", "", 12}, 1, true, true, false},
        {"foreign", {"d", "D", "", 13}, 1, false, false, false}};
    facts.space = {{"space", {"s", "S", "", 14}, 1, true, true, true}};
    const auto land = page::tree_view(facts, false);
    require(land.size() == 3 && !land[0].dimmed && land[1].dimmed && land[2].dimmed, "tech and initial lock are independent, foreign slot omitted");
    require(land[0].control == "authored-a" && land[0].type.description == "encyclopedia", "authored slot and copied hover facts retained");
    require(page::tree_view(facts, true).size() == 1 && !page::tree_view(facts, true)[0].dimmed, "released initial lock is bright in space view");
    facts.combined = true;
    require(page::tree_view(facts, true).size() == 3, "combined tree has no domain switch");
    facts.technology.reset(); require(page::tree_view(facts, false)[0].dimmed, "unknown technology never becomes a known zero");
    require(page::tree_arrow_texture("IDC_DOWN_ARROW3") == "i_tree_arrow_down.tga"
        && page::tree_arrow_texture("IDC_RIGHT_ARROW21") == "i_tree_arrow_right.tga"
        && page::tree_arrow_texture("IDC_DRIGHT_ARROW0") == "i_tree_arrow_2right.tga"
        && page::tree_arrow_texture("IDC_RRIGHT_ARROW99") == "i_tree_arrow_3right.tga", "each arrow id family takes its own atlas image");
    for (const auto* other : {"IDC_DOWN_ARROW", "IDC_DOWN_ARROW100", "IDC_RIGHT_ARROW07", "IDC_RIGHT_ARROW1A", "IDC_TECHTREE_X", "IDC_LEFT_ARROW1"})
        require(page::tree_arrow_texture(other).empty(), other);
}
page::SummaryPlanet planet(std::uint64_t id, std::string name) {
    page::SummaryPlanet row; row.planet = id; row.name = std::move(name); row.owner = 7; row.authorized = true;
    row.basic = 10.5; row.trade = 2.5; row.capacity = 4;
    row.modifiers = std::vector<page::MiningModifier>{{.6, true, {"mine", "Mine", {}}}, {.6, true, {"mine2", "Mine2", {}}}, {9, false, {}}};
    row.upgrades = std::vector<page::ReferenceIcon>{{"upgrade", "Upgrade", {}}};
    row.fronts[0] = page::ProductionItem{"Space", "space", {}, 0, 1};
    row.fronts[1] = page::ProductionItem{"Ground", "ground", {}, 23, 2};
    row.ground = std::vector<page::ReferenceIcon>{{"hero", "Hero", {}, 4}, {"unit", "Unit", {}, 5, 3}};
    row.orbit[0] = std::vector<page::ReferenceIcon>{{"ship", "Ship", {}, 6, 2}};
    return row;
}
void summary() {
    page::ReferenceFacts facts; facts.local_player = 7;
    for (std::uint64_t id = 1; id <= 8; ++id) facts.planets.push_back(planet(id, "Planet"+std::to_string(9-id)));
    facts.planets[0].basic = 100;
    auto foreign = planet(99, "Secret"); foreign.owner = 8; facts.planets.push_back(foreign);
    auto denied = planet(100, "Denied"); denied.authorized = false; facts.planets.push_back(denied);
    page::ReferencePagesModel model; model.publish(facts); model.choose(page::ReferenceTab::summary);
    require(model.summary.planets.size() == 8 && model.summary.planets[0].planet == 8, "summary admits local rows and starts in name order");
    const auto row = page::economy_row(model.summary.planets[0]);
    require(row.basic == 11 && row.trade == 3 && row.mining == 2 && row.total == 16 && row.mines == 2 && row.mine.icon == "mine", "independent rounding and first admitted mining source");
    require(model.summary.totals.total == 217 && model.summary.capacity == 32, "footers include planets outside six row window");
    require(model.summary.planets[0].fronts[0]->progress_percent == 0 && model.summary.planets[0].fronts[1]->progress_percent == 23
        && model.summary.planets[0].upgrades->at(0).name == "Upgrade", "fronts, simultaneous zero progress and persistent upgrade facts retained");
    model.scroll(100); require(model.offset == 2 && model.target(0, false) == 6 && !model.target(0, true), "six row scroll clamp and locked selection");
    model.order(page::SummarySort::total);
    require(model.summary.planets[0].planet == 1 && model.offset == 2, "new numeric sort is descending and retains offset");
    model.order(page::SummarySort::total); require(model.summary.planets.back().planet == 1, "repeated column reverses sort");
    model.order(page::SummarySort::basic); require(model.summary.planets.front().planet == 1, "base component sort uses rounded numeric key");
    model.order(page::SummarySort::trade); require(!model.ascending, "new trade sort is descending");
    model.order(page::SummarySort::mining); require(!model.ascending, "new mining sort is descending");
    model.order(page::SummarySort::capacity); require(model.summary.planets.front().capacity == 4, "capacity sort consumes published contributions");
    model.order(page::SummarySort::fronts); require(!model.ascending, "front sort selects count key");
    model.order(page::SummarySort::upgrades); require(!model.ascending, "upgrade sort selects persistent-count key");
    model.category(page::SummaryKind::fleets);
    require(model.summary.fleet_rows.size() == 16 && model.maximum_offset() == 6, "ground and orbit expand with ten row scrollbar");
    require(model.summary.fleet_rows[0].first && !model.summary.fleet_rows[1].first
        && model.summary.fleet_rows[0].icons[0].name == "Hero" && model.summary.fleet_rows[0].icons[1].count == 3, "hero order and grouped multiplicity survive row layout");
    model.scroll(6); require(!model.target(2, false), "GREF-23 out-of-range expanded row selection does nothing");
    require(model.target(0, false) == model.summary.planets[6].planet, "GREF-23 selection uses sorted planet index rather than expanded row identity");
    model.step(true); require(model.offset == 0, "source next callback has separate owned-planet bounds");
    model.scroll(6); model.publish(facts); require(model.offset == 0, "fleet data reconstruction clamps to owned planet count");
    model.visible = false; model.open(); require(model.kind == page::SummaryKind::production, "help retains tab but summary reopens in production");
    model.choose(page::ReferenceTab::tree); model.visible = false; model.open(); require(model.tab == page::ReferenceTab::tree && model.visible, "help reopens last tab without toggling it closed");
    facts.planets[0].ground->resize(20); model.publish(facts);
    for (const auto& fleet : model.summary.fleet_rows) require(fleet.icons.size() <= 15, "fleet overflow truncates without wrapping");
    auto missing = planet(1, "Missing"); missing.trade.reset();
    require(!page::economy_row(missing).total, "missing provider components never become zero");
}
void publication() {
    sim::ObserverSetup observer; observer.human_observers = {7}; observer.solo_reference_human = 7;
    sim::ObserverPlayer local; local.player = 7; local.allies = {9}; observer.players = {local};
    auto observations = std::make_shared<std::vector<sim::ObservedPlanet>>();
    sim::ObservedPlanet known; known.player = 7; known.planet = 1; known.discovered = true;
    known.known_owner = 7; known.knowledge.ownership = known.knowledge.production = known.knowledge.fleet_contents = true;
    known.knowledge.structures = known.knowledge.ground_contents = known.knowledge.income_breakdown = true;
    observations->push_back(known);
    auto masked = known; masked.planet = 2; masked.knowledge.ownership = false; observations->push_back(masked);
    sim::ProductionSetup production; sim::PlanetProduction producer; producer.planet = 1; producer.starbase_type = 11;
    production.planets = {producer};
    sim::EconomyForecast forecast; forecast.player = 7;
    sim::PlanetIncomeForecast economy; economy.planet = 1; economy.basic = eawr::sim::math::Fixed::from_integer(10).value();
    economy.trade = eawr::sim::math::Fixed::from_integer(2).value(); economy.modifiers.emplace(); forecast.planets = {economy};
    sim::Fleet fleet; fleet.id = 100; fleet.owner = 9; fleet.orbit = 1; fleet.members = {{101,10},{102,10},{103,11}};
    sim::Fleet enemy = fleet; enemy.id = 200; enemy.owner = 8;
    sim::GalacticSnapshot snapshot(1, {}, {}, {}, {}, {fleet, enemy}, {}, {}, {}, {}, {}, {}, {}, production,
        {}, {}, {}, observer, {}, {}, {}, observations, forecast);
    std::map<page::ObjectId, page::PlanetHudFacts> admitted;
    for (auto id : {1U,2U}) { page::PlanetHudFacts facts; facts.planet = id; facts.discovered = true;
        facts.observed_owner = 7; facts.display_name = "Known"; admitted[id] = facts; }
    std::map<std::uint64_t, page::ReferenceType> types;
    types[10].icon = {"ship", "Ship", {}, 10}; types[11].icon = {"hero", "Hero", {}, 11};
    types[11].hero = true; types[11].capacity = 3;
    types[10].gui_visibility_known = types[11].gui_visibility_known = true;
    std::map<page::ObjectId, page::ReferenceType> planet_types; planet_types[1].capacity = 5;
    const auto result = page::published_summary_planets(snapshot, 7, admitted, types, planet_types);
    require(result.size() == 1 && result[0].planet == 1 && result[0].capacity == 8, "projection requires current observer ownership, aggregates authored planet plus starbase capacity");
    require(result[0].orbit[0]->size() == 2 && result[0].orbit[0]->at(0).name == "Hero"
        && result[0].orbit[0]->at(1).count == 2, "allied direct membership filtered, heroes precede type groups, enemies excluded");
    require(result[0].trade == 2 && !result[0].upgrades, "published component admitted and missing built upgrade owner remains unavailable");
    sim::GalacticSnapshot unpublished(1, {}, {}, {}, {}, {fleet, enemy}, {}, {}, {}, {}, {}, {}, {}, production,
        {}, {}, {}, observer, {}, {}, {}, observations);
    require(snapshot.canonical_bytes() == unpublished.canonical_bytes(), "reference publication does not change canonical snapshot bytes");
    require(page::published_summary_planets(snapshot, 8, admitted, types, planet_types).empty(), "another observer cannot request local publication");
    types[10].hidden = true;
    require(page::published_summary_planets(snapshot, 7, admitted, types, planet_types)[0].orbit[0]->size() == 1,
        "GUI-hidden ordinary objects do not enter fleet groups");
    types[10].gui_visibility_known = false;
    require(!page::published_summary_planets(snapshot, 7, admitted, types, planet_types)[0].orbit[0],
        "unknown GUI visibility leaves orbital contents unavailable");
    known.knowledge.income_breakdown = false; observations->at(0) = known;
    sim::GalacticSnapshot filtered(1, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {},
        {}, {}, {}, observer, {}, {}, {}, observations, forecast);
    require(!filtered.economy_forecast()->planets[0].trade && !filtered.economy_forecast()->planets[0].modifiers, "income breakdown masks new component publication");
}
void legend() {
    const auto lookup = [](std::string_view key) { return "L:" + std::string(key); };
    auto entries = page::legend_entries({"a.tga", "b.tga", "c.tga"}, {"T_A", "T_B"}, {0, 255, 100, 255, 0, 51, 9, 9, 9}, lookup);
    require(entries.size() == 2, "GREF-19 shorter authored list wins");
    require(entries[0].icon.icon == "a.tga" && entries[0].text == "L:T_A" && entries[1].icon.icon == "b.tga", "GREF-19 authored order is kept");
    require(entries[0].colour.g == 1.F && entries[1].colour.r == 1.F && entries[1].colour.b == 51.F / 255.F, "GREF-19 authored RGB per entry");
    require(page::legend_entries({"a.tga"}, {"T_A"}, {1, 2}, lookup).empty(), "GREF-19 incomplete RGB triplet drops the entry");
    require(page::legend_entries({}, {}, {}, lookup).empty(), "GREF-19 empty lists give an empty legend");
}
// WGUI-71/104, GFC-28, GFSC-21, WGP-122/125: the hint producer, the checkbox flags and the archive triplets.
void advisor_hints() {
    require(page::hint_interval_frames("300", 30) == 9000U, "GFC-28 stock interval: 300 s at 30 frames per second");
    require(page::hint_interval_frames("0.05", 30) == 2U, "GFC-28 integer conversion adds one half");
    require(!page::hint_interval_frames("", 30) && !page::hint_interval_frames("-1", 30) && !page::hint_interval_frames("5s", 30),
        "GFC-28 a missing or invalid interval leaves no producer, never a default");
    const auto keys = page::hint_keys("\n  TEXT_A,\n TEXT_B ,TEXT_C\tTEXT_D\r\n");
    require(keys == std::vector<std::string>{"TEXT_A", "TEXT_B", "TEXT_C", "TEXT_D"}, "authored hint list order and separators");

    page::HintEntry entry;
    require(entry.enabled && entry.deadline == 90 && entry.index == 0, "WGUI-104 fresh entry: enabled, deadline 90, index 0");
    require(!page::due_hint(entry, 89, 3, 2), "GFC-28 not due before the deadline");
    require(page::due_hint(entry, 90, 3, 2) == 0U && entry.index == 1 && entry.deadline == 92, "GFSC-21 one hint, index advances, rearm from now");
    require(!page::due_hint(entry, 91, 3, 2), "GFSC-21 the next turn before the new deadline is not due");
    require(page::due_hint(entry, 92, 3, 2) == 1U && page::due_hint(entry, 94, 3, 2) == 2U, "WGUI-71 sequential, not random");
    require(page::due_hint(entry, 96, 3, 2) == 0U, "WGUI-71 the index wraps at the list count");
    entry.enabled = false;
    require(!page::due_hint(entry, 200, 3, 2) && entry.deadline == 98 && entry.index == 1, "GFC-28 disabled keeps deadline and index");
    entry.enabled = true;
    require(page::due_hint(entry, 200, 3, 2) == 1U && entry.deadline == 202, "GFSC-21 re-enabled overdue entry attempts now and rearms from now");
    require(!page::due_hint(entry, 500, 0, 2), "no authored hints, no hint");

    {
        // Load after hints: a session fired hints up to frame 9000 (interval 9000), then loads a save at frame 3000.
        page::HintEntries session;
        auto& galactic = session[0];
        page::first_hint_frame(galactic, 0);
        require(page::due_hint(galactic, 90, 3, 9000) == 0U && page::due_hint(galactic, 9090, 3, 9000) == 1U
            && galactic.deadline == 18090 && galactic.index == 2, "the old timeline is ahead");
        session[1].enabled = false;
        page::restart_hint_timeline(session);
        require(galactic.deadline == 90 && galactic.index == 0 && galactic.enabled && !session[1].enabled,
            "a load restarts deadline and index and keeps the checkbox");
        page::first_hint_frame(galactic, 3000);
        require(galactic.deadline == 3090, "the loaded frame gets the restored-save delay, not the old deadline");
        require(!page::due_hint(galactic, 3089, 3, 9000) && page::due_hint(galactic, 3090, 3, 9000) == 0U,
            "the first hint after the load is 90 frames later, from the first key");
        page::HintEntry fresh;
        page::first_hint_frame(fresh, 0);
        require(fresh.deadline == 90, "a new campaign keeps the fresh deadline");
    }

    page::HintEntries entries;
    entries[0].enabled = false; entries[1].enabled = false; entries[2].deadline = 7; entries[2].index = 4;
    require(page::any_hints_enabled(entries), "WGP-125 the checkbox is the OR of the three flags");
    page::set_all_hints(entries, false);
    require(!page::any_hints_enabled(entries) && entries[2].deadline == 7 && entries[2].index == 4,
        "WGP-125 the checkbox sets all three flags and keeps deadline and index");

    std::vector<page::ArchiveEntry> archive;
    page::archive_submission(archive, "Day 3", {255, 0, 0, 255}, "Hint", {0, 193, 91, 255}, "____", {128, 128, 128, 255});
    page::archive_submission(archive, "Day 3", {255, 0, 0, 255}, "Hint", {0, 193, 91, 255}, "____", {128, 128, 128, 255});
    require(archive.size() == 6 && archive[0].text == "Day 3" && archive[1].colour[1] == 193 && archive[2].colour[0] == 128
        && archive[3].text == "Day 3", "WGP-122 three entries per submission, no dedup of repeated cycles");
    require(page::authored_rgba("255 0 0") == page::Rgba{255, 0, 0, 255} && page::authored_rgba("1 2 3 4") == page::Rgba{1, 2, 3, 4},
        "authored date/separator colours");
    require(!page::authored_rgba("255 0") && !page::authored_rgba("256 0 0") && !page::authored_rgba("1 2 x"),
        "invalid authored colours are absent");
}
}
int main() {
    try { advisor_hints(); legend(); populated_catalog(); colliding_catalog_names(); mod_label_scale(); history(); tree(); tree_dialog_choice(); summary(); publication(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "GREF reference page contracts passed\n";
}
