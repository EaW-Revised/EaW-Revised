#include "galactic/reference_catalog.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace page = eawr::viewer::galactic;
namespace sim = eawr::sim::galactic;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
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
            <Text_ID>TEXT_TROOPER</Text_ID><Encyclopedia_Unit_Class>TEXT_INFANTRY</Encyclopedia_Unit_Class>
            <Encyclopedia_Text>TEXT_DESCRIPTION</Encyclopedia_Text><Tech_Level>1</Tech_Level></GroundCompany>
          <SpaceUnit Name="Fixture_Ship"><Affiliation>Empire</Affiliation><Icon_Name>ship.tga</Icon_Name>
            <Text_ID>TEXT_SHIP</Text_ID><Tech_Level>2</Tech_Level><Build_Initially_Locked>Yes</Build_Initially_Locked></SpaceUnit>
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
    ~CatalogFixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
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
    sim::ObserverSetup observer; observer.local_player = 7;
    sim::ObservedPlanet known; known.player = 7; known.planet = 1; known.discovered = true; known.known_owner = 7;
    known.knowledge.ownership = known.knowledge.structures = true;
    auto observations = std::make_shared<std::vector<sim::ObservedPlanet>>(1, known);
    sim::ProductionSetup production; sim::PlanetProduction producer; producer.planet = 1; producer.starbase_type = 12;
    production.planets = {producer};
    sim::GalacticSnapshot snapshot(1, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, production,
        {}, {}, {}, observer, {}, {}, {}, observations);
    page::PlanetHudFacts hud; hud.planet = 1; hud.discovered = true; hud.observed_owner = 7; hud.display_name = "Fixture";
    std::map<std::uint64_t, page::ReferenceType> types{{12,*source.metadata("Fixture_Base")}};
    std::map<page::ObjectId, page::ReferenceType> planets{{1,*source.metadata("Fixture_Planet")}};
    const auto rows = page::published_summary_planets(snapshot, 7, {{1,hud}}, types, planets);
    require(rows.size() == 1 && rows[0].capacity == 8 && rows[0].icon == "planet.tga", "loaded planet and producer metadata publish nonzero Summary capacity and planet icon");
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
    sim::ObserverSetup observer; observer.local_player = 7;
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
}
int main() {
    try { populated_catalog(); colliding_catalog_names(); history(); tree(); summary(); publication(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "GREF reference page contracts passed\n";
}
