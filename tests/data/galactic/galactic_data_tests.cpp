// Galactic conquest data loaders (docs/galactic-data.md): a synthetic tree with no game data, and
// the stock FoC campaigns when EAWR_EAW_GAME_ROOT names an install.

#include "../../support/temp_dir.hpp"
#include "../../support/env.hpp"
#include "../../support/check.hpp"
#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <map>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace galactic = eawr::data::galactic;
using galactic::Fixed;
using galactic::IssueKind;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string_view message) {
    local_checker.check(condition, message, std::cerr);
}

std::int64_t raw(const std::int64_t whole) { return whole * Fixed::scale; }

Fixed decimal(const std::string_view text) { return Fixed::from_decimal(text).value(); }

std::optional<std::string> environment(const char* name) {
    return eawr::testing::environment(name);
}

struct TempTree final {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("eawr-galactic-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempTree() { std::filesystem::create_directories(root); }
    ~TempTree() {
        eawr::testing::remove_temp_directory(root);
    }
    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
};

void write(const std::filesystem::path& path, const std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

bool has_issue(const galactic::GalacticData& data, const IssueKind kind, const std::string_view owner,
               const std::string_view tag, const std::string_view value = {}) {
    for (const auto& issue : data.issues) {
        if (issue.kind == kind && issue.owner == owner && issue.tag == tag && (value.empty() || issue.value == value)) return true;
    }
    return false;
}

void print_issues(const galactic::GalacticData& data) {
    for (const auto& issue : data.issues) {
        std::cerr << "  " << galactic::to_string(issue.kind) << ' ' << issue.owner << ' ' << issue.tag << " '" << issue.value
                  << "' " << issue.reason << '\n';
    }
}

constexpr std::string_view planets_xml = R"xml(<?xml version="1.0"?>
<Planets>
  <Planet Name="Alpha">
    <Behavior>PLANET</Behavior>
    <Galactic_Position>10.0, 20.0, 0.0</Galactic_Position>
    <Political_Control>2</Political_Control>
    <Political_Faction>Empire</Political_Faction><Planet_Retains_Residual_Influence>No</Planet_Retains_Residual_Influence><Scale_Factor>2</Scale_Factor>
    <Planet_Credit_Value>60</Planet_Credit_Value>
    <The_Force>Light</The_Force><Force_Strength>0.75</Force_Strength>
    <Population_Value>2</Population_Value>
    <Additional_Population_Capacity>8</Additional_Population_Capacity>
    <Max_Space_Base>3</Max_Space_Base>
    <Planet_Surface_Accessible> Yes </Planet_Surface_Accessible>
    <Planet_Starbase_Stealth>Yes</Planet_Starbase_Stealth>
    <Planet_Groundbase_Stealth>No</Planet_Groundbase_Stealth>
    <Space_Tactical_Map>_Space_Planet_Alpha.ted</Space_Tactical_Map>
    <Bogus_Planet_Tag>1</Bogus_Planet_Tag>
  </Planet>
  <Planet Name="Beta">
    <Variant_Of_Existing_Type>Alpha</Variant_Of_Existing_Type>
    <Galactic_Position>-5.5, 0, 1</Galactic_Position>
    <Planet_Credit_Value>TBD</Planet_Credit_Value>
    <Max_Space_Base>2 bases</Max_Space_Base>
    <Planet_Surface_Accessible>No</Planet_Surface_Accessible>
    <Planet_Groundbase_Stealth>Yes</Planet_Groundbase_Stealth>
  </Planet>
  <Planet Name="Gamma">
    <Behavior>PLANET</Behavior>
    <Galactic_Position>1.0, 2.0</Galactic_Position>
    <Planet_Credit_Value>lots</Planet_Credit_Value>
  </Planet>
  <Planet Name="Delta">
    <Behavior>PLANET</Behavior>
    <Galactic_Position>0, 0, 0</Galactic_Position>
  </Planet>
</Planets>)xml";

constexpr std::string_view objects_xml = R"xml(<?xml version="1.0"?>
<Objects>
  <SpaceUnit Name="Test_Ship"><Affiliation>Rebel</Affiliation></SpaceUnit>
  <SpaceUnit Name="Test_Hero"><Affiliation>Empire</Affiliation></SpaceUnit>
  <SpaceProp Name="Galaxy_Art"></SpaceProp>
</Objects>)xml";

constexpr std::string_view factions_xml = R"xml(<?xml version="1.0"?>
<Factions>
  <Faction Name="Rebel">
    <Force_Alignment>Light</Force_Alignment>
    <Is_Playable>Yes</Is_Playable>
    <Primary_Enemy>Empire</Primary_Enemy>
    <Basic_AI>Mod_Faction_Controller</Basic_AI>
    <Allies>Empire</Allies><Enemies>Empire, Missing_Faction</Enemies>
    <Hyperspace_Speed_Factor>1.0</Hyperspace_Speed_Factor>
    <Displayed_Tech_Level_Adjustment>1</Displayed_Tech_Level_Adjustment>
    <Base_Population_Capacity>12</Base_Population_Capacity>
    <Minimum_Visible_Base_Level>1</Minimum_Visible_Base_Level>
    <AI_Uses_Galactic_Pop_Cap>yes</AI_Uses_Galactic_Pop_Cap>
  </Faction>
  <Faction Name="Empire">
    <Is_Playable>True</Is_Playable>
    <Primary_Enemy>Nobody</Primary_Enemy>
    <Multiplayer_Campaign_Heroes>Test_Hero, Missing_Hero</Multiplayer_Campaign_Heroes>
    <Easily_Bribed>no</Easily_Bribed>
  </Faction>
</Factions>)xml";

constexpr std::string_view campaigns_xml = R"xml(<?xml version="1.0"?>
<Campaigns>
  <Campaign Name="Test_Campaign">
    <Campaign_Set>Test_Set</Campaign_Set>
    <Sort_Order>4</Sort_Order>
    <Camera_Distance>1200.0f</Camera_Distance>
    <Supports_Custom_Settings>Yes</Supports_Custom_Settings>
    <Is_Multiplayer>Ture</Is_Multiplayer>
    <Tutorial>TBD</Tutorial>
    <Locations> Galaxy_Art, Alpha, Beta,
        Gamma, Missing_Planet, Alpha </Locations>
    <Trade_Routes>Alpha_Beta, Beta_Gamma, Missing_Route, Alpha_Delta</Trade_Routes>
    <Home_Location> Rebel, Alpha </Home_Location>
    <Home_Location> Empire, Gamma </Home_Location>
    <Starting_Active_Player> Rebel </Starting_Active_Player>
    <Starting_Credits> Rebel, 1000 </Starting_Credits>
    <Starting_Credits> Empire, 2000 </Starting_Credits>
    <Starting_Credits> empire, 3000 </Starting_Credits>
    <Starting_Credits> Rebel </Starting_Credits>
    <Starting_Tech_Level> Rebel, 1 </Starting_Tech_Level>
    <Min_Custom_Tech_Level>2</Min_Custom_Tech_Level>
    <Max_Custom_Tech_Level>9</Max_Custom_Tech_Level>
    <Max_Tech_Level> Rebel, 4.5 </Max_Tech_Level>
    <Starting_Forces> Rebel, Alpha, Test_Ship </Starting_Forces>
    <Starting_Forces> Rebel, Alpha, Test_Ship </Starting_Forces>
    <Starting_Forces>TBD</Starting_Forces>
    <Starting_Forces> Empire, Gamma, Missing_Type </Starting_Forces>
    <Starting_Forces> Rebel, Nowhere, Test_Ship </Starting_Forces>
    <AI_Player_Control> Empire, BasicEmpire </AI_Player_Control>
    <Human_Victory_Conditions> Galactic_All_Planets_Controlled </Human_Victory_Conditions>
    <Human_Victory_Credit_Target>17</Human_Victory_Credit_Target>
    <AI_Victory_Credit_Target>19</AI_Victory_Credit_Target>
    <Human_Victory_Cycle_Limit>23</Human_Victory_Cycle_Limit>
    <AI_Victory_Cycle_Limit>29</AI_Victory_Cycle_Limit>
    <Human_Victory_Galactic_Control>0.25</Human_Victory_Galactic_Control>
    <AI_Victory_Galactic_Control>0.75</AI_Victory_Galactic_Control>
    <Human_Victory_Faction_Names>EMPIRE</Human_Victory_Faction_Names>
    <AI_Victory_Faction_Names>REBEL</AI_Victory_Faction_Names>
    <Human_Victory_Leader_Names>LeaderA</Human_Victory_Leader_Names>
    <AI_Victory_Leader_Names>LeaderB</AI_Victory_Leader_Names>
    <AI_Victory_Planet_Names>Alpha</AI_Victory_Planet_Names>
    <Special_Case_Production> Rebel, Delta, Test_Ship </Special_Case_Production>
    <Bogus_Campaign_Tag>x</Bogus_Campaign_Tag>
  </Campaign>
  <Campaign Name="Second_Campaign">
    <Locations>Alpha</Locations>
  </Campaign>
</Campaigns>)xml";

constexpr std::string_view routes_xml = R"xml(<?xml version="1.0"?>
<TradeRoutes>
  <TradeRoute Name="Alpha_Beta">
    <Point_A>Alpha</Point_A><Point_B>Beta</Point_B>
    <HS_Speed_Factor>5.0</HS_Speed_Factor><Political_Control_Gain>0</Political_Control_Gain>
    <Credit_Gain_Factor>0.5</Credit_Gain_Factor><Visible_Line_Name>DEFAULT</Visible_Line_Name>
  </TradeRoute>
  <TradeRoute Name="Beta_Gamma"><Point_A>Beta</Point_A><Point_B>Gamma</Point_B><Wibble>1</Wibble></TradeRoute>
  <TradeRoute Name="Alpha_Delta"><Point_A>Alpha</Point_A><Point_B>Delta</Point_B></TradeRoute>
  <TradeRoute Name="alpha_beta"><Point_A>Alpha</Point_A><Point_B>Beta</Point_B><HS_Speed_Factor>2.0</HS_Speed_Factor></TradeRoute>
  <TradeRoute Name="Alpha_Nowhere"><Point_A>Alpha</Point_A><Point_B>Nowhere</Point_B></TradeRoute>
</TradeRoutes>)xml";

constexpr std::string_view constants_xml = R"xml(<?xml version="1.0"?>
<GameConstants>
<Raid_Force_Required_Faction>Faction_A</Raid_Force_Required_Faction>
<Raid_Force_Free_Object_Category_Mask>CustomHero</Raid_Force_Free_Object_Category_Mask>
<Raid_Force_Limited_Object_Category_Mask>CustomGround</Raid_Force_Limited_Object_Category_Mask>
<Raid_Force_Max_Heros>3</Raid_Force_Max_Heros>
<Raid_Force_Max_Limited_Objects>7</Raid_Force_Max_Limited_Objects>
  <Object_Max_Speed_Multiplier_Galactic> 5.0 </Object_Max_Speed_Multiplier_Galactic>
  <Corruption_Hyperspace_Bonus>1.75</Corruption_Hyperspace_Bonus>
  <Fiscal_Cycle_Time_In_Secs>45.0 seconds</Fiscal_Cycle_Time_In_Secs>
  <Good_Side_Name>REBEL</Good_Side_Name>
  <Evil_Side_Name>EMPIRE</Evil_Side_Name>
  <Corrupt_Side_Name>UNDERWORLD</Corrupt_Side_Name>
  <Credit_Cap_Per_Planet> 40000.0 </Credit_Cap_Per_Planet>
  <MaxCreditIncomeAlignmentBonus>0.5</MaxCreditIncomeAlignmentBonus>
  <MaxCreditIncomeAlignmentPenalty>0.4</MaxCreditIncomeAlignmentPenalty>
  <MaxInfluenceTransitionAlignmentBonus>0.5</MaxInfluenceTransitionAlignmentBonus>
  <MaxInfluenceTransitionAlignmentPenalty>0.25</MaxInfluenceTransitionAlignmentPenalty>
  <Political_Income_Curve>0,0, 1,2</Political_Income_Curve>
  <Political_Income_Curve>5,1, 0,0, 1,1</Political_Income_Curve>
  <MaximumPoliticalControl> 5 </MaximumPoliticalControl>
  <Political_Control_Change_Time_Seconds>7</Political_Control_Change_Time_Seconds>
  <Max_Ground_Forces_On_Planet>12</Max_Ground_Forces_On_Planet>
  <TradeRouteMovementFactor> 0.5 </TradeRouteMovementFactor>
  <AIUsesFogOfWarGalactic> False </AIUsesFogOfWarGalactic>
  <Strategic_Queue_Tactical_Battles> True </Strategic_Queue_Tactical_Battles>
  <Battle_Pending_Timeout_Seconds> 15 </Battle_Pending_Timeout_Seconds>
</GameConstants>)xml";

void write_tree(const std::filesystem::path& root, const std::string_view campaigns) {
    write(root / "XML" / "GameObjectFiles.xml",
          "<Game_Object_Files><File>Planets.xml</File><File>Objects.xml</File></Game_Object_Files>");
    write(root / "XML" / "HardpointDataFiles.xml", "<Hard_Point_Files></Hard_Point_Files>");
    write(root / "XML" / "FactionFiles.xml", "<Faction_Files><File>Factions.xml</File></Faction_Files>");
    write(root / "XML" / "CampaignFiles.xml", "<Campaign_Files><File>Campaigns_Test.xml</File></Campaign_Files>");
    write(root / "XML" / "SFXEventFiles.xml", "<SFXEvent_Files></SFXEvent_Files>");
    write(root / "XML" / "TradeRouteFiles.xml",
          "<Trade_Route_Files><File>TradeRoutes.xml</File><File>Missing_Routes.xml</File></Trade_Route_Files>");
    write(root / "XML" / "Planets.xml", planets_xml);
    write(root / "XML" / "Objects.xml", objects_xml);
    write(root / "XML" / "Factions.xml", factions_xml);
    write(root / "XML" / "Campaigns_Test.xml", campaigns);
    write(root / "XML" / "TradeRoutes.xml", routes_xml);
    write(root / "XML" / "GameConstants.xml", constants_xml);
}

std::optional<galactic::GalacticData> load_tree(const std::filesystem::path& root) {
    const std::array mounts{eawr::vfs::MountSpec{"base", root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "synthetic tree mounts");
    if (!mounted) return std::nullopt;
    auto catalog = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "synthetic catalog loads");
    if (!catalog) return std::nullopt;
    auto loaded = galactic::load_galactic_data(mounted.value(), catalog.value().catalog);
    expect(static_cast<bool>(loaded), "synthetic galactic data loads");
    if (!loaded) {
        std::cerr << loaded.error().message << '\n';
        return std::nullopt;
    }
    return std::move(loaded).value();
}

void story_name_contracts() {
    TempTree tree;
    write_tree(tree.root, R"xml(<Campaigns>
      <Campaign Name="Story_Base"><Locations>Alpha</Locations>
        <Rebel_Story_Name>named.xml</Rebel_Story_Name>
        <Story_Name>Rebel, first.xml</Story_Name><Story_Name>TBD</Story_Name>
        <Story_Name>Empire second.xml, Rebel</Story_Name><Story_Name>repeat.xml</Story_Name>
        <Story_Name>TBD</Story_Name><Story_Name> </Story_Name>
      </Campaign>
      <Campaign Name="Story_Child"><Variant_Of_Existing_Type>Story_Base</Variant_Of_Existing_Type>
        <Story_Name>Renamed_Fourth, fourth.xml</Story_Name>
        <Story_Name>Rebel, last.xml</Story_Name>
      </Campaign>
      <Campaign Name="Story_Inherited"><Variant_Of_Existing_Type>Story_Base</Variant_Of_Existing_Type>
        <Story_Name>TBD</Story_Name><Story_Name> </Story_Name>
      </Campaign>
      <Campaign Name="Story_Odd"><Locations>Alpha</Locations><Story_Name>Rebel, kept.xml, tail</Story_Name></Campaign>
      <Campaign Name="Story_Default"><Locations>Alpha</Locations></Campaign>
    </Campaigns>)xml");
    const auto loaded = load_tree(tree.root);
    if (!loaded) return;
    const auto& base = loaded->campaign("Story_Base")->story_names;
    const auto& child = loaded->campaign("Story_Child")->story_names;
    const auto& inherited = loaded->campaign("Story_Inherited")->story_names;
    expect(base.size() == 3 && base[0].faction == "Rebel" && base[0].name == "first.xml"
        && base[1].faction == "Empire" && base[1].name == "second.xml"
        && base[2].faction == "Rebel" && base[2].name == "repeat.xml",
        "WST-86 a mid-stream TBD is ignored and a pair split across occurrences retains authored order");
    expect(child.size() == 5 && child[0].name == "first.xml" && child[2].name == "repeat.xml"
        && child[3].faction == "Renamed_Fourth" && child[3].name == "fourth.xml" && child[4].name == "last.xml",
        "WST-86 inherited associations precede derived associations, including repeated factions");
    expect(inherited.size() == 3 && inherited.back().name == "repeat.xml"
        && loaded->campaign("Story_Default")->story_names.empty(), "WST-86 placeholders preserve inheritance and omission defaults empty");
    expect(loaded->campaign("Story_Child")->rebel_story_name == "named.xml", "named story inheritance remains separate");
    expect(loaded->campaign("Story_Odd")->story_names.size() == 1
        && has_issue(*loaded, IssueKind::bad_value, "Story_Odd", "Story_Name", "tail"),
        "WST-86 an odd trailing token is diagnosed without losing complete pairs");
    expect(has_issue(*loaded, IssueKind::dangling_reference, "Story_Child", "Story_Name", "Renamed_Fourth"),
        "WST-88 unknown story factions are validated");
    const auto identity = galactic::content_identity(*loaded);
    const auto changed = [&](const auto& mutate) {
        auto copy = *loaded; mutate(copy.campaigns[1].story_names);
        expect(galactic::content_identity(copy) != identity, "story faction/path/count/order participate in content identity");
    };
    changed([](auto& rows) { rows[0].faction = "Renamed"; });
    changed([](auto& rows) { rows[0].name = "other.xml"; });
    changed([](auto& rows) { rows.push_back(rows.front()); });
    changed([](auto& rows) { std::swap(rows[0], rows[1]); });
    // This sequence parser's delimiter set is narrower than arbitrary C whitespace.
    write_tree(tree.root, "<Campaigns><Campaign Name=\"Limits\"><Locations>Alpha</Locations><Story_Name>"
        + std::string(65536, 'x') + "</Story_Name></Campaign></Campaigns>");
    const auto overlong = load_tree(tree.root);
    expect(overlong && overlong->campaign("Limits")->story_names.empty()
        && has_issue(*overlong, IssueKind::bad_value, "Limits", "Story_Name"), "WST-86 oversized parser inputs are diagnosed");
}

void empty_raid_masks() {
    TempTree tree;
    write_tree(tree.root, campaigns_xml);
    for (const auto value : {std::string_view{}, std::string_view{" \t "}, std::string_view{" | , "}}) {
        write(tree.root / "XML" / "GameConstants.xml", "<GameConstants><Raid_Force_Free_Object_Category_Mask>"
            + std::string(value) + "</Raid_Force_Free_Object_Category_Mask><Raid_Force_Limited_Object_Category_Mask>"
            + std::string(value) + "</Raid_Force_Limited_Object_Category_Mask></GameConstants>");
        const auto loaded = load_tree(tree.root);
        if (!loaded) continue;
        for (const auto tag : {"Raid_Force_Free_Object_Category_Mask", "Raid_Force_Limited_Object_Category_Mask"}) {
            expect(loaded->constant(tag) != nullptr, "CGS-15 empty raid mask retains the initialized category set");
            expect(!has_issue(*loaded, IssueKind::missing_required, "GameConstants", tag), "empty raid mask is not missing");
        }
    }
    write(tree.root / "XML" / "GameConstants.xml", "<GameConstants/>");
    const auto absent = load_tree(tree.root);
    if (absent) for (const auto tag : {"Raid_Force_Free_Object_Category_Mask", "Raid_Force_Limited_Object_Category_Mask"})
        expect(absent->constant(tag) && absent->constant(tag)->text.empty(), "CGS-15 absent raid mask keeps the zero default");
}

void campaign_contracts(const galactic::GalacticData& data) {
    expect(data.campaigns.size() == 2 && data.campaigns[0].name == "Test_Campaign"
        && data.campaigns[1].name == "Second_Campaign", "campaigns load in authored order");
    const auto* campaign = data.campaign("test_campaign");
    expect(campaign != nullptr, "campaign lookup ignores case");
    if (campaign == nullptr) return;
    expect(campaign->campaign_set == "Test_Set" && campaign->sort_order == 4, "selection fields");
    expect(campaign->camera_distance == decimal("1200")
        && has_issue(data, IssueKind::trailing_text, "Test_Campaign", "Camera_Distance", "1200.0f"),
        "AU-82: a number with trailing text reads its leading number and is reported");
    expect(campaign->supports_custom_settings == true && campaign->is_multiplayer == true && !campaign->tutorial,
        "AU-82 booleans; AU-81 a TBD boolean has no value");
    expect(campaign->minimum_custom_tech == 2 && campaign->maximum_custom_tech == 9,
        "WGP-69/190 campaign technology slider bounds follow mod authoring");
    expect((campaign->locations == std::vector<std::string>{"Galaxy_Art", "Alpha", "Beta", "Gamma", "Missing_Planet", "Alpha"}),
        "WGS-01: locations keep their authored order and entries");
    expect((campaign->planets == std::vector<std::string>{"Alpha", "Beta", "Gamma"}),
        "WGS-01: the galaxy art object is a location, not a planet");
    expect(has_issue(data, IssueKind::dangling_reference, "Test_Campaign", "Locations", "Missing_Planet")
        && !has_issue(data, IssueKind::dangling_reference, "Test_Campaign", "Locations", "Galaxy_Art")
        && has_issue(data, IssueKind::duplicate_record, "Test_Campaign", "Locations", "Alpha"),
        "a missing location and a repeated one are reported");
    expect(has_issue(data, IssueKind::dangling_reference, "Test_Campaign", "Trade_Routes", "Missing_Route")
        && has_issue(data, IssueKind::dangling_reference, "Test_Campaign", "Trade_Routes", "Alpha_Delta"),
        "an unknown route and a route leaving the campaign are reported");
    expect(campaign->home_locations.size() == 2 && campaign->home_locations[1].faction == "Empire"
        && campaign->home_locations[1].name == "Gamma", "home location records");
    expect(campaign->starting_credits.size() == 3 && campaign->starting_credits[1].value == 2000
        && campaign->starting_credits[2].faction == "empire" && campaign->starting_credits[2].value == 3000,
        "WGS-11: faction records keep authored order, duplicates included");
    expect(has_issue(data, IssueKind::duplicate_record, "Test_Campaign", "Starting_Credits", "empire, 3000"),
        "WGS-U01: a repeated faction record is reported");
    expect(has_issue(data, IssueKind::bad_value, "Test_Campaign", "Starting_Credits", "Rebel")
        && has_issue(data, IssueKind::bad_value, "Test_Campaign", "Max_Tech_Level", "Rebel, 4.5")
        && campaign->max_tech_levels.empty(), "malformed faction records are skipped and reported");
    expect(campaign->starting_forces.size() == 4 && campaign->starting_forces[1].type == "Test_Ship",
        "every starting-force occurrence is one record; TBD adds none");
    expect(has_issue(data, IssueKind::dangling_reference, "Test_Campaign", "Starting_Forces", "Missing_Type")
        && has_issue(data, IssueKind::dangling_reference, "Test_Campaign", "Starting_Forces", "Nowhere"),
        "starting forces name game objects at campaign locations");
    expect(campaign->special_case_production.size() == 1 && campaign->human_victory_conditions.size() == 1
        && campaign->ai_player_control.size() == 1, "production, victory and AI records");
    expect(campaign->human_victory_credit_target == 17 && campaign->ai_victory_credit_target == 19
        && campaign->human_victory_cycle_limit == 23 && campaign->ai_victory_cycle_limit == 29
        && campaign->human_victory_galactic_control == decimal("0.25") && campaign->ai_victory_galactic_control == decimal("0.75")
        && campaign->human_victory_faction_names == std::vector<std::string>{"EMPIRE"}
        && campaign->ai_victory_faction_names == std::vector<std::string>{"REBEL"}
        && campaign->human_victory_leader_names == std::vector<std::string>{"LeaderA"}
        && campaign->ai_victory_leader_names == std::vector<std::string>{"LeaderB"}
        && campaign->ai_victory_planet_names == std::vector<std::string>{"Alpha"}, "WGS-78 complete human/AI victory parameter families");
    const auto identity = galactic::content_identity(data);
    const auto identity_changes = [&](auto change) {
        auto changed = data;
        change(changed.campaigns.front());
        expect(galactic::content_identity(changed) != identity, "victory parameters participate in campaign content identity");
    };
    identity_changes([](auto& row) { row.human_victory_credit_target = 101; });
    identity_changes([](auto& row) { row.ai_victory_credit_target = 101; });
    identity_changes([](auto& row) { row.human_victory_cycle_limit = 101; });
    identity_changes([](auto& row) { row.human_victory_galactic_control = Fixed::from_raw(1); });
    identity_changes([](auto& row) { row.ai_victory_galactic_control = Fixed::from_raw(1); });
    identity_changes([](auto& row) { row.human_victory_faction_names.push_back("Extra"); });
    identity_changes([](auto& row) { row.ai_victory_faction_names.push_back("Extra"); });
    identity_changes([](auto& row) { row.human_victory_leader_names.push_back("Extra"); });
    identity_changes([](auto& row) { row.ai_victory_leader_names.push_back("Extra"); });
    identity_changes([](auto& row) { row.ai_victory_planet_names.push_back("Extra"); });
    expect(has_issue(data, IssueKind::unknown_tag, "Test_Campaign", "Bogus_Campaign_Tag"), "unknown campaign tag");
}

void world_contracts(const galactic::GalacticData& data) {
    expect(data.planets.size() == 4, "every Planet object loads");
    expect(data.planet("Alpha")->force_alignment == "Light"
        && data.planet("Alpha")->force_strength == decimal("0.75"), "planet alignment fields load");
    expect(data.faction("Rebel")->force_alignment == "Light", "faction alignment fields load");
    const auto* curve = data.constant("Political_Income_Curve");
    expect(curve && curve->text == "5,1, 0,0, 1,1" && !curve->number, "last curve occurrence replaces earlier pairs");
    const auto* alpha = data.planet("Alpha");
    const auto* beta = data.planet("Beta");
    const auto* gamma = data.planet("Gamma");
    expect(alpha && alpha->planet_starbase_stealth == true && alpha->planet_groundbase_stealth == false
        && beta && beta->planet_starbase_stealth == true && beta->planet_groundbase_stealth == true,
        "observer base stealth parses independent inherited values and explicit overrides");
    expect(alpha && alpha->galactic_position == galactic::Position{Fixed::from_raw(raw(10)), Fixed::from_raw(raw(20)), Fixed{}}
        && alpha->political_control == Fixed::from_raw(raw(2)) && alpha->max_space_base == 3
        && alpha->planet_surface_accessible == true && alpha->space_tactical_map == "_Space_Planet_Alpha.ted",
          "planet fields");
      expect(alpha && beta && alpha->political_faction == "Empire" && beta->political_faction == "Empire"
          && alpha->planet_retains_residual_influence == false && beta->planet_retains_residual_influence == false,
          "political faction and residual-influence flag retain inherited original values");
    expect(alpha && beta && alpha->scale_factor == decimal("2") && beta->scale_factor == decimal("2"),
        "planet model scale retains effective inherited authoring");
    expect(beta && beta->galactic_position == galactic::Position{decimal("-5.5"), Fixed{}, Fixed::from_raw(raw(1))}
        && beta->planet_credit_value == Fixed::from_raw(raw(60)) && beta->max_space_base == 2
        && beta->planet_surface_accessible == false && beta->space_tactical_map == "_Space_Planet_Alpha.ted",
        "AU-80/81/83: a variant planet inherits, and a TBD value keeps the base's");
    expect(has_issue(data, IssueKind::trailing_text, "Beta", "Max_Space_Base", "2 bases"), "trailing text on a count");
    expect(gamma && !gamma->galactic_position && !gamma->planet_credit_value
        && has_issue(data, IssueKind::bad_value, "Gamma", "Galactic_Position", "1.0, 2.0")
        && has_issue(data, IssueKind::missing_required, "Gamma", "Galactic_Position")
        && has_issue(data, IssueKind::bad_value, "Gamma", "Planet_Credit_Value", "lots"),
        "a malformed position or number is reported and left absent");
    expect(has_issue(data, IssueKind::unknown_tag, "Alpha", "Bogus_Planet_Tag"), "unknown planet tag");

    expect(data.trade_routes.size() == 4, "trade routes load; a repeated name keeps one record");
    const auto* route = data.trade_route("Alpha_Beta");
    expect(route && route->name == "alpha_beta" && route->hs_speed_factor == Fixed::from_raw(raw(2))
        && has_issue(data, IssueKind::duplicate_record, "alpha_beta", "@Name", "alpha_beta"),
        "a repeated trade route: the later definition replaces the earlier");
    expect(has_issue(data, IssueKind::unknown_tag, "Beta_Gamma", "Wibble")
        && has_issue(data, IssueKind::dangling_reference, "Alpha_Nowhere", "Point", "Nowhere")
        && has_issue(data, IssueKind::dangling_reference, "TradeRouteFiles", "File", "Missing_Routes.xml"),
        "route issues: unknown tag, end point and a missing include");

    const auto* rebel = data.faction("Rebel");
    expect(rebel && rebel->minimum_visible_base_level == 1, "observer faction base threshold retains authored integer");
    expect(data.planet("Alpha")->population_value == 2 && data.planet("Alpha")->additional_population_capacity == 8,
        "planet original population and capacity preserve authored XML");
    const auto* empire = data.faction("Empire");
    expect(rebel && rebel->basic_ai == "Mod_Faction_Controller" && empire && empire->basic_ai.empty(),
        "Basic_AI preserves a mod-authored player reference and its omission");
    expect(rebel && rebel->allies == std::vector<std::string>{"Empire"}
        && rebel->enemies == std::vector<std::string>{"Empire", "Missing_Faction"}
        && empire && empire->allies.empty() && empire->enemies.empty()
        && has_issue(data, IssueKind::dangling_reference, "Rebel", "Enemies", "Missing_Faction"),
        "directional faction relation lists preserve authoring, omissions and reference diagnostics");
    expect(rebel && rebel->is_playable == true && rebel->primary_enemy == "Empire"
        && rebel->hyperspace_speed_factor == Fixed::from_raw(raw(1)) && rebel->displayed_tech_level_adjustment == 1,
        "faction fields");
    expect(rebel && rebel->base_population_capacity == 12 && rebel->ai_uses_galactic_pop_cap == true
        && empire && !empire->base_population_capacity && !empire->ai_uses_galactic_pop_cap, "faction cap values preserve explicit versus omitted XML");
    expect(empire && empire->multiplayer_campaign_heroes.size() == 2 && empire->easily_bribed == false
        && has_issue(data, IssueKind::dangling_reference, "Empire", "Primary_Enemy", "Nobody")
        && has_issue(data, IssueKind::dangling_reference, "Empire", "Multiplayer_Campaign_Heroes", "Missing_Hero"),
        "faction references");

    const auto* fiscal = data.constant("Fiscal_Cycle_Time_In_Secs");
    const auto* side = data.constant("Corrupt_Side_Name");
    expect(fiscal && fiscal->number == Fixed::from_raw(raw(45)) && side && side->text == "UNDERWORLD" && !side->number,
        "galactic constants keep text and number");
    const auto* fog = data.constant("AIUsesFogOfWarGalactic");
    expect(fog && fog->text == "False" && !fog->number,
        "WGA-08 publishes authored galactic AI fog as a boolean text constant");
    const auto* surface_limit = data.constant("Max_Ground_Forces_On_Planet");
    expect(surface_limit && surface_limit->number == Fixed::from_raw(raw(12)),
        "WGM-39 preserves the authored surface limit without a stock fallback");
    const auto* transition_bonus = data.constant("MaxInfluenceTransitionAlignmentBonus");
    const auto* transition_penalty = data.constant("MaxInfluenceTransitionAlignmentPenalty");
    expect(transition_bonus && transition_bonus->number == decimal("0.5")
        && transition_penalty && transition_penalty->number == decimal("0.25"),
        "CGS-16 publishes authored control alignment constants for campaign setup");
    const auto* hyperspace_bonus = data.constant("Corruption_Hyperspace_Bonus");
    expect(hyperspace_bonus && hyperspace_bonus->text == "1.75" && hyperspace_bonus->number == decimal("1.75"),
        "WGM-27 retains the authored corruption movement bonus without a stock default");
    expect(has_issue(data, IssueKind::trailing_text, "GameConstants", "Fiscal_Cycle_Time_In_Secs", "45.0 seconds"),
        "AU-82 also applies to numeric galactic constants");
    expect(!data.constant("MaximumFleetMovementDistance")
        && has_issue(data, IssueKind::missing_required, "GameConstants", "MaximumFleetMovementDistance"),
        "an absent constant is reported, not defaulted");
    expect(!data.inputs.empty() && std::is_sorted(data.inputs.begin(), data.inputs.end(),
        [](const galactic::InputFile& left, const galactic::InputFile& right) { return left.logical_path < right.logical_path; }),
        "input receipts are sorted");
}

void identity_contracts(const std::filesystem::path& root, const galactic::GalacticData& data) {
    auto controller_changed = data; controller_changed.factions[0].basic_ai = "Other_Mod_Controller";
    expect(galactic::content_identity(controller_changed) != galactic::content_identity(data),
        "authored faction Basic_AI participates in galactic content identity");
    auto relation_changed = data; relation_changed.factions[0].allies.push_back("Extra");
    expect(galactic::content_identity(relation_changed) != galactic::content_identity(data), "authored allies participate in identity");
    relation_changed = data; relation_changed.factions[0].enemies.push_back("Extra");
    expect(galactic::content_identity(relation_changed) != galactic::content_identity(data), "authored enemies participate in identity");
    relation_changed = data; relation_changed.planets[0].scale_factor = decimal("3");
    expect(galactic::content_identity(relation_changed) != galactic::content_identity(data), "model scale participates in identity");
    const auto again = load_tree(root);
    expect(again && galactic::content_identity(*again) == galactic::content_identity(data),
        "loading the same data twice gives the same identity");
    auto changed = data;
    changed.factions[0].base_population_capacity = 99;
    expect(galactic::content_identity(changed) != galactic::content_identity(data), "faction capacity participates in identity");
    changed = data;
    changed.factions[0].ai_uses_galactic_pop_cap = false;
    expect(galactic::content_identity(changed) != galactic::content_identity(data), "AI cap flag participates in identity");
    changed = data;
    changed.campaigns[0].starting_credits[0].value += 1;
    expect(galactic::content_identity(changed) != galactic::content_identity(data), "a credit record joins the identity");
    changed = data;
    changed.issues.clear();
    changed.inputs.clear();
    expect(galactic::content_identity(changed) == galactic::content_identity(data),
        "issues and input receipts stay out of the identity");
}

void movement_float_parsing() {
    for (const auto value : {"-1.25", "0", "text", "1e99"}) {
        TempTree tree; write_tree(tree.root, campaigns_xml);
        const std::string text(value);
        write(tree.root / "XML" / "GameConstants.xml", "<GameConstants>"
            "<Object_Max_Speed_Multiplier_Galactic>" + text + "</Object_Max_Speed_Multiplier_Galactic>"
            "<Corruption_Hyperspace_Bonus>" + text + "</Corruption_Hyperspace_Bonus>"
            "<MaximumFleetMovementDistance>" + text + "</MaximumFleetMovementDistance></GameConstants>");
        write(tree.root / "XML" / "Factions.xml", "<Factions><Faction Name='Rebel'>"
            "<Hyperspace_Speed_Factor>" + text + "</Hyperspace_Speed_Factor></Faction></Factions>");
        const auto loaded = load_tree(tree.root);
        expect(loaded.has_value(), "retail movement float fixture must load for every authored value");
        if (!loaded) return;
        const auto expected = text == "-1.25" ? Fixed::from_ratio(-5, 4).value()
            : text == "1e99" ? Fixed::from_raw(std::numeric_limits<std::int64_t>::max()) : Fixed{};
        for (const auto tag : {"Object_Max_Speed_Multiplier_Galactic", "Corruption_Hyperspace_Bonus", "MaximumFleetMovementDistance"})
            expect(loaded->constant(tag) && loaded->constant(tag)->number == expected, "scoped float constants keep signs and accept text/overflow");
        expect(loaded->faction("Rebel") && loaded->faction("Rebel")->hyperspace_speed_factor == expected,
            "faction movement float uses the same retail parser");
    }
}

void synthetic_contracts() {
    TempTree tree;
    write_tree(tree.root, campaigns_xml);
    eawr::data::tag_trace::Recording recording;
    const auto data = load_tree(tree.root);
    const auto trace = recording.finish();
    if (!data) return;
    expect(std::any_of(trace.begin(), trace.end(), [](const auto& entry) {
        return entry.kind == eawr::data::tag_trace::Kind::object && entry.element == "TradeRoute"
            && entry.name == "Alpha_Beta";
    }), "trade route trace uses its record class, not the TradeRoutes document wrapper");
    campaign_contracts(*data);
    world_contracts(*data);
    identity_contracts(tree.root, *data);
    if (failures != 0) print_issues(*data);

    TempTree broken;
    write_tree(broken.root, campaigns_xml);
    std::filesystem::remove(broken.root / "XML" / "TradeRouteFiles.xml");
    const std::array mounts{eawr::vfs::MountSpec{"base", broken.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "broken tree mounts");
    if (!mounted) return;
    auto catalog = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::foc);
    expect(catalog && !galactic::load_galactic_data(mounted.value(), catalog.value().catalog),
        "an unreadable trade route registry fails the load");
}

void art_contracts() {
    TempTree tree;
    write_tree(tree.root, R"xml(<Campaigns><Campaign Name="Art_Test">
      <Locations>Renamed_Art, Alpha, Beta, Gamma, Delta, Background_Planet, Galactic_Planet, Art_Variant</Locations>
      <Home_Location>Rebel, renamed_art</Home_Location>
      <Home_Location>Empire, Background_Planet</Home_Location>
      <Starting_Forces>Rebel, RENAMED_ART, Test_Ship</Starting_Forces>
      <Starting_Forces>Empire, Galactic_Planet, Test_Ship</Starting_Forces>
    </Campaign></Campaigns>)xml");
    std::string xml(planets_xml);
    xml.insert(xml.find("</Planets>"), R"xml(
      <Planet Name="Renamed_Art"><Behavior>IDLE</Behavior><In_Background>yes</In_Background>
        <Galactic_Position>0,0,0</Galactic_Position><Galactic_Model_Name>art.alo</Galactic_Model_Name>
        <Zoomed_Terrain_Index>7</Zoomed_Terrain_Index></Planet>
      <Planet Name="Background_Planet"><Behavior>planet</Behavior><In_Background>yes</In_Background>
        <Galactic_Position>1,1,1</Galactic_Position></Planet>
      <Planet Name="Galactic_Planet"><Behavior>IDLE</Behavior><GalacticBehavior>PlAnEt</GalacticBehavior>
        <Galactic_Position>2,2,2</Galactic_Position><Zoomed_Terrain_Index>40</Zoomed_Terrain_Index></Planet>
      <Planet Name="Art_Variant"><Variant_Of_Existing_Type>Renamed_Art</Variant_Of_Existing_Type>
        <GalacticBehavior>PLANET</GalacticBehavior></Planet>
    )xml");
    write(tree.root / "XML" / "Planets.xml", xml);
    const auto loaded = load_tree(tree.root); if (!loaded) return;
    const auto& data = *loaded;
    const auto* art = data.galaxy_art_object("renamed_art");
    expect(art && !data.planet("Renamed_Art") && !art->has_planetary_behavior()
        && art->in_background == true && art->galactic_model_name == "art.alo",
        "WGS-01a: Planet-typed IDLE-only art retains presentation data in a separate lookup");
    expect(data.planets.size() == 7 && data.galaxy_art_objects.size() == 1, "planet/art table classification");
    const auto* campaign = data.campaign("Art_Test");
    expect(campaign && campaign->locations.size() == 8 && campaign->locations.front() == "Renamed_Art"
        && campaign->planets == std::vector<std::string>{"Alpha", "Beta", "Gamma", "Delta", "Background_Planet", "Galactic_Planet", "Art_Variant"},
        "authored Locations retain art while campaign planets exclude it");
    expect(has_issue(data, IssueKind::dangling_reference, "Art_Test", "Home_Location", "renamed_art")
        && has_issue(data, IssueKind::dangling_reference, "Art_Test", "Starting_Forces", "RENAMED_ART"),
        "WGS-01a: case-insensitive art homes and starting-force locations are diagnosed");
    expect(!has_issue(data, IssueKind::dangling_reference, "Art_Test", "Home_Location", "Background_Planet")
        && !has_issue(data, IssueKind::dangling_reference, "Art_Test", "Starting_Forces", "Galactic_Planet"),
        "WGS-01a: real background and galactic planets remain valid placement locations");
    for (const auto name : {"Background_Planet", "Galactic_Planet", "Art_Variant", "Beta"}) {
        const auto* planet = data.planet(name);
        expect(planet && planet->has_planetary_behavior() && !data.galaxy_art_object(name),
            "common/galactic PLANET tokens and inherited behavior qualify without an In_Background heuristic");
    }
    // GHUD-S52: the panel index is read as authored; an index past the stock range (a mod) is not clamped.
    expect(art->zoomed_terrain_index == 7 && data.planet("Galactic_Planet")->zoomed_terrain_index == 40
        && !data.planet("Beta")->zoomed_terrain_index,
        "GHUD-S52: Zoomed_Terrain_Index loads as authored, absent stays absent");
    expect(data.planet("Art_Variant")->zoomed_terrain_index == 7, "GHUD-S52: the index is inherited through a variant");
    auto changed = data; changed.galaxy_art_objects[0].galactic_model_name = "other.alo";
    expect(galactic::content_identity(changed) != galactic::content_identity(data), "art presentation data joins content identity");
    changed = data; changed.galaxy_art_objects[0].zoomed_terrain_index = 8;
    expect(galactic::content_identity(changed) != galactic::content_identity(data), "the terrain index joins content identity");
    changed = data; changed.planets[0].galactic_behaviors.push_back("IDLE");
    expect(galactic::content_identity(changed) != galactic::content_identity(data), "qualification inputs join content identity");
}

// The stock campaigns (gc-session-save walk, "Stock campaign inventory", XML EGS-01).
void foc_contracts() {
    const auto root = environment("EAWR_EAW_GAME_ROOT");
    if (!root) {
        std::cout << "FoC galactic data: skipped (set EAWR_EAW_GAME_ROOT)\n";
        return;
    }
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                     std::pair{std::string("base"), std::string("GameData")}}) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data");
        expect(static_cast<bool>(manifest), "FoC layer mounts");
        if (!manifest) return;
        specs.push_back(std::move(manifest).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    expect(static_cast<bool>(filesystem), "FoC vfs mounts");
    if (!filesystem) return;
    auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "FoC catalog loads");
    if (!catalog) return;
    const auto loaded = galactic::load_galactic_data(filesystem.value(), catalog.value().catalog);
    expect(static_cast<bool>(loaded), "FoC galactic data loads");
    if (!loaded) return;
    const auto& data = loaded.value();

    expect(data.campaigns.size() == 33, "WGS-01: the five stock campaign files hold 33 definitions");
    // GHUD-S52: stock planets author the organize-shell panel index (spot values from the stock XML).
    for (const auto& [name, index] : {std::pair<std::string_view, std::int64_t>{"Honoghr", 1}, {"Abregado_Rae", 0},
             {"Bespin", 11}, {"Utapau", 10}}) {
        const auto* planet = data.planet(name);
        expect(planet && planet->zoomed_terrain_index == index, std::string("GHUD-S52 stock terrain index: ") + std::string(name));
    }
    std::size_t authored_indexes = 0;
    for (const auto& planet : data.planets) authored_indexes += planet.zoomed_terrain_index ? 1U : 0U;
    expect(authored_indexes * 2U > data.planets.size(), "GHUD-S52: most stock planets author a terrain index");
    struct Family {
        std::string_view name;
        std::size_t locations;
        std::size_t routes;
        std::size_t forces;
    };
    // Underworld-human variants and the story campaign: locations / routes / force records.
    for (const auto& family : {Family{"Sandbox_Gateways_Underworld", 28, 23, 152},
             Family{"Sandbox_Equal_Footing_Underworld", 56, 15, 396}, Family{"Sandbox_Origin_Underworld", 22, 10, 142},
             Family{"Sandbox_Heart_Of_The_Maw_Underworld", 13, 10, 83}, Family{"Sandbox_Rim_Worlds_Underworld", 37, 19, 264},
             Family{"Sandbox_Clusters_Underworld", 34, 21, 127}, Family{"Multiplayer_Sandbox_Equal_Footing", 56, 15, 559},
             Family{"Full_Story_Campaign_Underworld", 32, 16, 431}, Family{"Tutorial_Underworld", 5, 3, 37}}) {
        const auto* campaign = data.campaign(family.name);
        expect(campaign && campaign->locations.size() == family.locations && campaign->trade_routes.size() == family.routes
            && campaign->starting_forces.size() == family.forces, std::string("EGS-01 inventory: ") + std::string(family.name));
    }
    const auto credits = [&](const std::string_view campaign_name, const std::string_view faction) -> std::optional<std::int64_t> {
        const auto* campaign = data.campaign(campaign_name);
        if (campaign == nullptr) return std::nullopt;
        for (const auto& record : campaign->starting_credits) {
            if (record.faction.size() == faction.size() && std::equal(record.faction.begin(), record.faction.end(),
                    faction.begin(), [](const char a, const char b) { return (a | 0x20) == (b | 0x20); })) {
                return record.value; // WGS-11: the first match
            }
        }
        return std::nullopt;
    };
    expect(credits("Sandbox_Gateways_Underworld", "Underworld") == 12000 && credits("Sandbox_Gateways_Underworld", "Rebel") == 8000,
        "EGS-01: human 12000, others 8000");
    expect(credits("Full_Story_Campaign_Underworld", "Rebel") == 5000 && credits("Full_Story_Campaign_Underworld", "Underworld") == 0,
        "EGS-01: story credits 5000 / 5000 / 0");
    const auto* story = data.campaign("Full_Story_Campaign_Underworld");
    expect(story && story->is_story_campaign == true && story->supports_custom_settings == false,
        "EGS-01: the story campaign flags");
    expect(has_issue(data, IssueKind::duplicate_record, "Sandbox_Rim_Worlds_Empire", "Starting_Credits"),
        "WGS-U01: Rim Worlds' Empire-human definition repeats Rebel credits");

    std::map<std::string, std::size_t> by_kind;
    std::set<std::string> unknown;
    for (const auto& issue : data.issues) {
        ++by_kind[std::string(galactic::to_string(issue.kind))];
        if (issue.kind == IssueKind::unknown_tag) unknown.insert(issue.tag);
    }
    std::cout << "FoC galactic data: " << data.campaigns.size() << " campaigns, " << data.planets.size() << " planets, "
              << data.trade_routes.size() << " trade routes, " << data.factions.size() << " factions, "
              << data.constants.size() << " constants, " << data.issues.size() << " issues";
    for (const auto& [kind, count] : by_kind) std::cout << ' ' << kind << '=' << count;
    std::cout << '\n';
    // Stock schema-unknown tags across all three catalog classes, not just Campaign. Keep this
    // set strict: a newly unknown tag must be investigated, while authored legacy planet/faction
    // tags remain named issues (Facing_Adjust, Max_Ground_Base and the faction flight tag have
    // registry DB-NOTAG evidence; other schema-unknown tags are not inferred to be ignored).
    const std::set<std::string> legacy{"Evil_Victory_Conditions", "Evil_Victory_Cycle_Limit",
        "Galactic_Control_Percentage_For_Victory", "Good_Victory_Conditions", "Good_Victory_Cycle_Limit",
        "Good_Victory_Planet_Names", "Facing_Adjust", "Max_Ground_Base", "Space_Retreat_Flight_Increment",
        "Special_Structures", "Trade_Routes"};
    for (const auto& tag : unknown) expect(legacy.contains(tag), "stock unknown tag: " + tag);
    expect(data.constants.size() == galactic::galactic_constant_tags().size(), "every galactic constant is authored");
    const auto* hyperspace_bonus = data.constant("Corruption_Hyperspace_Bonus");
    expect(hyperspace_bonus && hyperspace_bonus->number == decimal("1.5"), "WGM-27 stock corruption movement bonus loads");
    const auto again = galactic::load_galactic_data(filesystem.value(), catalog.value().catalog);
    expect(again && galactic::content_identity(again.value()) == galactic::content_identity(data),
        "the stock load is deterministic");
}

} // namespace

int main() {
    story_name_contracts();
    synthetic_contracts();
    movement_float_parsing();
    empty_raid_masks();
    art_contracts();
    foc_contracts();
    if (failures == 0) std::cout << "galactic data contracts passed\n";
    return failures == 0 ? 0 : 1;
}
