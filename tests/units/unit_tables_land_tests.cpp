#include "unit_tables_foc_fixtures.hpp"

#include "eawr/units/land_companies.hpp"
#include "eawr/skirmish/land_content.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace unit_tables_test_support {

namespace {

using eawr::sim::tactical::BattleDomain;
using eawr::units::LandCompany;
using eawr::units::LandObjectKind;

std::string repeat(const std::string_view item, const int count) {
    std::string result;
    for (int index = 0; index < count; ++index) result += (index == 0 ? "" : ", ") + std::string(item);
    return result;
}

std::string land_xml() {
    return R"xml(
<Props_Generic Name="Mod_Static"><LandBehavior>LAND_OBSTACLE</LandBehavior>
  <Land_Model_Name>test_frigate</Land_Model_Name><Scale_Factor>2</Scale_Factor>
  <Obstacle_Width>60</Obstacle_Width><Obstacle_Height>100</Obstacle_Height>
  <Obstacle_X_Offset>-30</Obstacle_X_Offset><Obstacle_Y_Offset>-50</Obstacle_Y_Offset>
  <Fine_Tune_Occupied_Passability>No</Fine_Tune_Occupied_Passability>
</Props_Generic>
<Props_Generic Name="Mod_Proxy_Static"><LandBehavior>LAND_OBSTACLE</LandBehavior><Obstacle_Proxy_Type>Mod_Static</Obstacle_Proxy_Type>
  <Land_Model_Name>test_frigate</Land_Model_Name><Scale_Factor>4</Scale_Factor></Props_Generic>
<Props_Generic Name="Mod_Bad_Static"><LandBehavior>LAND_OBSTACLE</LandBehavior><Obstacle_Width>bad</Obstacle_Width></Props_Generic>
<GroundInfantry Name="Land_Trooper">
  <Create_Team>Yes</Create_Team><Create_Team_Type>Land_Trooper_Team</Create_Team_Type>
  <MovementClass>Infantry</MovementClass>
  <LandBehavior>IDLE, LAND_TEAM_INFANTRY_LOCOMOTOR, WEAPON</LandBehavior>
  <Max_Speed> 1 </Max_Speed>
  <Max_Rate_Of_Turn>12</Max_Rate_Of_Turn><OverrideAcceleration>.0625</OverrideAcceleration>
  <OverrideDeceleration>.125</OverrideDeceleration><Scale_Factor>2</Scale_Factor>
  <Custom_Soft_Footprint_Radius>3</Custom_Soft_Footprint_Radius>
  <Custom_Hard_XExtent>2</Custom_Hard_XExtent><Custom_Hard_YExtent>4</Custom_Hard_YExtent>
  <UnitCollisionClass> Infantry </UnitCollisionClass><FormationSpacing>.4</FormationSpacing>
</GroundInfantry>
<Container Name="Land_Trooper_Team"><LandBehavior>LAND_TEAM_CONTAINER_LOCOMOTOR</LandBehavior><Max_Speed> .8 </Max_Speed></Container>
<GroundVehicle Name="Land_Walker">
  <Create_Team>No</Create_Team>
  <MovementClass>Walker</MovementClass>
  <LandBehavior>WALKER_LOCOMOTOR</LandBehavior>
  <Max_Speed>0.6</Max_Speed>
</GroundVehicle>
<GroundVehicle Name="Land_Hero_Walker"><Variant_Of_Existing_Type>Land_Walker</Variant_Of_Existing_Type></GroundVehicle>
<HeroUnit Name="Land_Hero">
  <Is_Named_Hero>Yes</Is_Named_Hero>
  <Unique_Ground_Unit>Land_Hero_Walker</Unique_Ground_Unit>
</HeroUnit>
<GroundVehicle Name="Land_Hover_Mod"><MovementClass>Hover</MovementClass><LandBehavior>HOVER_LOCOMOTOR</LandBehavior></GroundVehicle>
<GroundStructure Name="Land_Turret"><LandBehavior>TARGETING</LandBehavior></GroundStructure>
<GroundCompany Name="Land_Infantry_Company">
  <Company_Units>)xml" + repeat("Land_Trooper", 18) + R"xml(</Company_Units>
  <Max_Squad_Size> 9 </Max_Squad_Size>
  <Create_Team_Type> Land_Trooper_Team </Create_Team_Type>
</GroundCompany>
<GroundCompany Name="Land_Walker_Company">
  <Company_Units>)xml" + repeat("Land_Walker", 4) + R"xml(</Company_Units>
  <Max_Squad_Size> 1 </Max_Squad_Size>
</GroundCompany>
<GroundCompany Name="Land_Single_Company">
  <Company_Units>Land_Walker</Company_Units>
  <Max_Squad_Size> 1 </Max_Squad_Size>
</GroundCompany>
<GroundCompany Name="Land_Direct_Company">
  <MovementClass>Wheeled</MovementClass>
  <LandBehavior>WHEELED_LOCOMOTOR</LandBehavior>
</GroundCompany>
<HeroCompany Name="Land_Hero_Company"><Company_Units>Land_Hero</Company_Units></HeroCompany>
<GroundCompany Name="Land_Generic_Team_Company">
  <Company_Units>)xml" + repeat("Land_Trooper", 5) + R"xml(</Company_Units>
  <Max_Squad_Size>3</Max_Squad_Size>
</GroundCompany>
<GroundCompany Name="Land_Unsized_Company">
  <Company_Units>)xml" + repeat("Land_Trooper", 4) + R"xml(</Company_Units>
</GroundCompany>
<GroundCompany Name="Land_Mod_Company">
  <Company_Units>Land_Hover_Mod, Land_Turret, Land_Hover_Mod</Company_Units>
</GroundCompany>
<GroundCompany Name="Land_Bad_Size_Company">
  <Company_Units>Land_Walker</Company_Units>
  <Max_Squad_Size>0</Max_Squad_Size>
</GroundCompany>
<GroundCompany Name="Land_Fractional_Size_Company">
  <Company_Units>Land_Walker</Company_Units>
  <Max_Squad_Size>2.5</Max_Squad_Size>
</GroundCompany>
<GroundCompany Name="Land_Bad_Member_Company">
  <Company_Units>Land_Walker, Land_Missing_Member</Company_Units>
</GroundCompany>
<GroundCompany Name="Land_Bad_Team_Company">
  <Company_Units>Land_Trooper</Company_Units>
  <Create_Team_Type>Land_Missing_Team</Create_Team_Type>
</GroundCompany>
<GroundCompany Name="Land_Empty_Company"><Company_Units/></GroundCompany>
<GroundVehicle Name="Mod_Siege_Tank"><MovementClass>ModHover</MovementClass><LandBehavior>HOVER_LOCOMOTOR</LandBehavior>
  <Land_Model_Name>test_frigate</Land_Model_Name><Max_Speed>1</Max_Speed><Max_Rate_Of_Turn>2</Max_Rate_Of_Turn>
  <Custom_Hard_XExtent>-0.4</Custom_Hard_XExtent><Custom_Hard_YExtent>-0.4</Custom_Hard_YExtent>
  <UnitCollisionClass>Vehicle</UnitCollisionClass></GroundVehicle>
<GroundCompany Name="Mod_Siege_Tank_Company"><Company_Units>Mod_Siege_Tank</Company_Units></GroundCompany>
<GroundInfantry Name="Mod_Model_Guard"><MovementClass>Infantry</MovementClass><LandBehavior>IDLE, LAND_TEAM_INFANTRY_LOCOMOTOR</LandBehavior>
  <Land_Model_Name>test_frigate</Land_Model_Name><Max_Speed>0.0</Max_Speed><Max_Rate_Of_Turn>0.0</Max_Rate_Of_Turn>
  <Scale_Factor></Scale_Factor><Custom_Soft_Footprint_Radius>0</Custom_Soft_Footprint_Radius>
  <OverrideAcceleration>1,5</OverrideAcceleration><UnitCollisionClass>Infantry</UnitCollisionClass></GroundInfantry>
<GroundInfantry Name="Mod_Blank_Flags"><Variant_Of_Existing_Type>Land_Trooper</Variant_Of_Existing_Type>
  <Create_Team></Create_Team><Walk_Transition/><Has_Pre_Turn_Anim>TBD</Has_Pre_Turn_Anim><Is_Named_Hero> </Is_Named_Hero></GroundInfantry>
<GroundCompany Name="Mod_Blank_Flags_Company"><Company_Units>Mod_Blank_Flags</Company_Units>
  <Create_Team_Type>Land_Trooper_Team</Create_Team_Type></GroundCompany>
<GroundCompany Name="Land_TBD_Company"><Company_Units>TBD</Company_Units></GroundCompany>
<GroundCompany Name="Mod_Model_Guard_Company"><Company_Units>Mod_Model_Guard</Company_Units><Max_Squad_Size>-1</Max_Squad_Size></GroundCompany>
<GroundCompany Name="Land_Whitespace_Company"><Company_Units>
  </Company_Units></GroundCompany>
)xml";
}

// WLPA-73: every engine class name, a case/padding variant, an absent tag and an unknown name.
const std::array<std::string_view, 11> class_authored{"None", "Landing Transport", " Build Pad ", "Low Obstacle",
    "Medium Obstacle", "High Obstacle", "infantry", "BIKE", "Vehicle", "Large Vehicle", "Giant Vehicle"};

std::string class_xml() {
    std::string members, xml;
    for (std::size_t i = 0; i < class_authored.size(); ++i) {
        const auto id = "Land_Class_" + std::to_string(i);
        xml += "<GroundVehicle Name=\"" + id + "\"><MovementClass>Walker</MovementClass><LandBehavior>WALKER_LOCOMOTOR</LandBehavior>"
            "<UnitCollisionClass>" + std::string(class_authored[i]) + "</UnitCollisionClass></GroundVehicle>\n";
        members += (i == 0 ? "" : ", ") + id;
    }
    xml += "<GroundVehicle Name=\"Land_Class_Absent\"><MovementClass>Walker</MovementClass><LandBehavior>WALKER_LOCOMOTOR</LandBehavior></GroundVehicle>\n";
    xml += "<GroundInfantry Name=\"Land_Class_Bad\"><Variant_Of_Existing_Type>Land_Trooper</Variant_Of_Existing_Type>"
        "<UnitCollisionClass>Mod Foot</UnitCollisionClass></GroundInfantry>\n";
    xml += "<GroundCompany Name=\"Land_Class_Company\"><Company_Units>" + members + ", Land_Class_Absent</Company_Units></GroundCompany>\n";
    xml += "<GroundCompany Name=\"Land_Bad_Class_Company\"><Company_Units>Land_Class_Bad</Company_Units></GroundCompany>\n";
    return xml;
}

const std::vector<std::string> land_types{"Land_Infantry_Company", "Land_Walker_Company", "Land_Single_Company",
    "Land_Direct_Company", "Land_Hero_Company", "Land_Generic_Team_Company", "Land_Unsized_Company", "Land_Mod_Company",
    "Land_Bad_Size_Company", "Land_Fractional_Size_Company", "Land_Bad_Member_Company", "Land_Bad_Team_Company",
    "Land_Empty_Company", "Land_Whitespace_Company", "Land_Class_Company", "Land_Bad_Class_Company",
    "Mod_Siege_Tank_Company", "Mod_Model_Guard_Company", "Mod_Blank_Flags_Company", "Land_TBD_Company"};

Loaded load_types(const std::filesystem::path& root, const BattleDomain domain, std::vector<std::string> types,
    std::vector<std::string> obstacles = {"Mod_Static", "Mod_Proxy_Static", "Mod_Bad_Static"}) {
    Loaded result;
    const std::array mounts{eawr::vfs::MountSpec{"base", root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "L2a: fixture mounts");
    if (!mounted) return result;
    auto catalog = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "L2a: fixture catalog loads");
    if (!catalog) return result;
    auto assets = models(-40.0F);
    eawr::assets::Mesh geometry;
    geometry.collidable = true; geometry.bone = 0;
    geometry.bounds_min = {4, -8, 10}; geometry.bounds_max = {10, 2, 24};
    eawr::assets::Submesh surface;
    for (const auto point : std::array<eawr::assets::Vec3f, 3>{{{4, -8, 10}, {10, -8, 10}, {4, 2, 10}}}) {
        eawr::assets::Vertex vertex; vertex.position = point; surface.vertices.push_back(vertex);
    }
    surface.indices = {0, 1, 2}; geometry.submeshes.push_back(surface);
    assets.at("data/art/models/test_frigate.alo").meshes.push_back(geometry);
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.domain = domain;
    input.filesystem = &mounted.value();
    input.model = [&](const std::string_view path) -> const eawr::assets::Model* {
        const auto found = assets.find(std::string(path));
        return found == assets.end() ? nullptr : &found->second;
    };
    input.types = std::move(types);
    if (domain == BattleDomain::land) input.obstacles = std::move(obstacles);
    auto tables = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(tables), "L2a: fixture tables load");
    if (tables) result.tables = std::move(tables).value();
    return result;
}

// LS shoot PR 1: a stock-shaped land fixture. An infantry and a tank with the weapon, health, armor, targeting, collision
// and death rows land objects author on themselves, one blaster projectile, a tank variant that only changes tags, and a
// damageable static.
std::string combat_xml() {
    return R"xml(
<Projectile Name="Shoot_Proj_Blaster">
  <Max_Speed>11</Max_Speed><Projectile_Category>Laser</Projectile_Category>
  <Projectile_Max_Flight_Distance>400</Projectile_Max_Flight_Distance>
  <Projectile_Damage>2.0</Projectile_Damage><Damage_Type>Damage_Blaster</Damage_Type>
  <Projectile_Does_Shield_Damage>No</Projectile_Does_Shield_Damage>
  <Projectile_Does_Hitpoint_Damage>Yes</Projectile_Does_Hitpoint_Damage>
</Projectile>
<Container Name="Shoot_Team"><LandBehavior>LAND_TEAM_CONTAINER_LOCOMOTOR</LandBehavior><Max_Speed>.8</Max_Speed></Container>
<GroundInfantry Name="Shoot_Trooper">
  <Create_Team>Yes</Create_Team><Create_Team_Type>Shoot_Team</Create_Team_Type>
  <MovementClass>Infantry</MovementClass>
  <LandBehavior>IDLE, LAND_TEAM_INFANTRY_LOCOMOTOR, WEAPON</LandBehavior>
  <Max_Speed>1</Max_Speed><Max_Rate_Of_Turn>12</Max_Rate_Of_Turn><Scale_Factor>2</Scale_Factor>
  <Custom_Soft_Footprint_Radius>3</Custom_Soft_Footprint_Radius>
  <Custom_Hard_XExtent>2</Custom_Hard_XExtent><Custom_Hard_YExtent>4</Custom_Hard_YExtent>
  <UnitCollisionClass>Infantry</UnitCollisionClass>
  <Land_Model_Name>test_frigate</Land_Model_Name>
  <Tactical_Health>48</Tactical_Health><Shield_Points>0</Shield_Points>
  <Armor_Type>Armor_Infantry</Armor_Type><Damage_Type>Damage_Blaster</Damage_Type>
  <CategoryMask>Fighter</CategoryMask><Targeting_Priority_Set>Frigate</Targeting_Priority_Set>
  <Targeting_Max_Attack_Distance>150</Targeting_Max_Attack_Distance>
  <Targeting_Stickiness_Time_Threshold>2</Targeting_Stickiness_Time_Threshold>
  <Idle_Chase_Range>200</Idle_Chase_Range><Guard_Chase_Range>250</Guard_Chase_Range>
  <Attack_Move_Response_Range>300</Attack_Move_Response_Range>
  <Ranged_Target_Z_Adjust>3</Ranged_Target_Z_Adjust>
  <Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living>
  <Collidable_By_Projectile_Dead>Yes</Collidable_By_Projectile_Dead>
  <Projectile_Types>Shoot_Proj_Blaster</Projectile_Types><Projectile_Damage>3.0</Projectile_Damage>
  <Projectile_Fire_Recharge_Seconds>1.5</Projectile_Fire_Recharge_Seconds>
  <Projectile_Fire_Pulse_Count>2</Projectile_Fire_Pulse_Count>
  <Projectile_Fire_Pulse_Delay_Seconds>.2</Projectile_Fire_Pulse_Delay_Seconds>
  <Targeting_Fire_Inaccuracy> Fighter, 4.0 </Targeting_Fire_Inaccuracy>
  <Death_Clone>Damage_Blaster, Shoot_Trooper_Death</Death_Clone>
  <Death_Fade_Time>3</Death_Fade_Time><Death_Persistence_Duration>5</Death_Persistence_Duration>
</GroundInfantry>
<GroundVehicle Name="Shoot_Tank">
  <MovementClass>ModHover</MovementClass><LandBehavior>HOVER_LOCOMOTOR</LandBehavior>
  <Max_Speed>.9</Max_Speed><Max_Rate_Of_Turn>12</Max_Rate_Of_Turn><Scale_Factor>2</Scale_Factor>
  <Custom_Soft_Footprint_Radius>5</Custom_Soft_Footprint_Radius>
  <Custom_Hard_XExtent>6</Custom_Hard_XExtent><Custom_Hard_YExtent>8</Custom_Hard_YExtent>
  <UnitCollisionClass>Vehicle</UnitCollisionClass>
  <Land_Model_Name>test_frigate</Land_Model_Name>
  <Tactical_Health>100</Tactical_Health><Armor_Type>Armor_Tank</Armor_Type><Damage_Type>Damage_Blaster</Damage_Type>
  <CategoryMask>Corvette</CategoryMask><Targeting_Priority_Set>Frigate</Targeting_Priority_Set>
  <Targeting_Max_Attack_Distance>200</Targeting_Max_Attack_Distance>
  <Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living>
  <Projectile_Types>Shoot_Proj_Blaster</Projectile_Types>
  <Projectile_Fire_Recharge_Seconds>2</Projectile_Fire_Recharge_Seconds>
  <Projectile_Fire_Pulse_Count>4</Projectile_Fire_Pulse_Count>
  <Projectile_Fire_Pulse_Delay_Seconds>.2</Projectile_Fire_Pulse_Delay_Seconds>
</GroundVehicle>
<GroundVehicle Name="Shoot_Tank_Mod"><Variant_Of_Existing_Type>Shoot_Tank</Variant_Of_Existing_Type>
  <Tactical_Health>150</Tactical_Health><Projectile_Damage>6.0</Projectile_Damage>
  <Targeting_Max_Attack_Distance>250</Targeting_Max_Attack_Distance>
</GroundVehicle>
<Props_Generic Name="Shoot_Wall"><LandBehavior>LAND_OBSTACLE</LandBehavior>
  <Land_Model_Name>test_frigate</Land_Model_Name><Scale_Factor>1</Scale_Factor>
  <Tactical_Health>500</Tactical_Health><Armor_Type>Armor_Tank</Armor_Type>
  <Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living>
</Props_Generic>
<GroundCompany Name="Shoot_Infantry_Company">
  <Company_Units>Shoot_Trooper, Shoot_Trooper</Company_Units>
  <Max_Squad_Size>2</Max_Squad_Size><Create_Team_Type>Shoot_Team</Create_Team_Type>
</GroundCompany>
<GroundCompany Name="Shoot_Armor_Company">
  <Company_Units>Shoot_Tank, Shoot_Tank_Mod</Company_Units><Max_Squad_Size>1</Max_Squad_Size>
</GroundCompany>
)xml";
}

const eawr::sim::tactical::DurabilityProfile* durability_row(const eawr::sim::tactical::DurabilityTable& table, const std::string_view id) {
    const auto type = eawr::assets::object_type_crc(id);
    for (const auto& row : table.profiles)
        if (row.type_id == type) return &row;
    return nullptr;
}

// Replaces `from` by `to` in one fixture file.
void respell_file(const std::filesystem::path& path, const std::string& from, const std::string& to) {
    std::string text;
    {
        std::ifstream input(path, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
    write(path, text);
}

void land_selection_rows() {
    TempTree tree;
    auto xml = units_xml("3600", "Unit_Select_Test");
    xml.insert(xml.rfind("</Objects>"), R"xml(
<GroundInfantry Name="Mod_Selectable_Member">
  <Behavior>SELECTABLE</Behavior><LandBehavior>LAND_TEAM_INFANTRY_LOCOMOTOR</LandBehavior>
  <Max_Speed>1</Max_Speed><Mouse_Collide_Override_Sphere_Radius>7</Mouse_Collide_Override_Sphere_Radius>
</GroundInfantry>
<GroundInfantry Name="Mod_Stationary_Member"><Variant_Of_Existing_Type>Mod_Selectable_Member</Variant_Of_Existing_Type>
  <Max_Speed>0</Max_Speed></GroundInfantry>
<GroundVehicle Name="Mod_Land_Selectable"><LandBehavior>SELECTABLE, HOVER_LOCOMOTOR</LandBehavior>
  <SpaceBehavior>DUMMY_TOOLTIP</SpaceBehavior><Max_Speed>2</Max_Speed></GroundVehicle>
<GroundVehicle Name="Mod_Unselectable"><Variant_Of_Existing_Type>Mod_Land_Selectable</Variant_Of_Existing_Type>
  <LandBehavior>HOVER_LOCOMOTOR</LandBehavior></GroundVehicle>
<GroundStructure Name="Mod_Selectable_Structure"><Behavior>SELECTABLE</Behavior></GroundStructure>
)xml");
    write_fixture(tree.root, xml);
    const auto loaded = load_types(tree.root, BattleDomain::land,
        {"Mod_Selectable_Member", "Mod_Stationary_Member", "Mod_Land_Selectable", "Mod_Unselectable", "Mod_Selectable_Structure"}, {});
    if (!loaded.tables) return;
    const auto& tables = *loaded.tables;
    const auto* member = tables.find("Mod_Selectable_Member");
    const auto* stationary = tables.find("Mod_Stationary_Member");
    const auto* vehicle = tables.find("Mod_Land_Selectable");
    const auto* unselectable = tables.find("Mod_Unselectable");
    const auto* structure = tables.find("Mod_Selectable_Structure");
    expect(member && stationary && vehicle && unselectable && structure, "land selection fixture types load");
    if (!member || !stationary || !vehicle || !unselectable || !structure) return;
    expect(member->selectable && member->mouse_sensitive && member->bar_admitted && member->locomotion
        && member->movement.max_speed == Fixed::from_integer(1).value()
        && member->mouse_collide_sphere_radius == Fixed::from_integer(7).value(),
        "WSU-10/13/21: land members publish shared SELECTABLE, locomotor, speed and pick sphere");
    expect(stationary->selectable && stationary->mouse_sensitive && stationary->locomotion
        && stationary->movement.max_speed == Fixed{}, "WSU-21: inherited land selection is independent of zero speed");
    expect(vehicle->selectable && vehicle->mouse_sensitive && vehicle->locomotion,
        "land selection also reads LandBehavior");
    expect(!unselectable->selectable && !unselectable->mouse_sensitive && unselectable->locomotion,
        "a mod can remove land selection; SpaceBehavior never admits a land pick");
    expect(structure->selectable && structure->mouse_sensitive && !structure->locomotion,
        "WSU-21: land structures remain direct-click targets outside mobile box selection");
}

void land_combat_rows() {
    using eawr::sim::tactical::object_weapon;
    TempTree tree;
    auto xml = units_xml("3600", "Unit_Select_Test");
    xml.insert(xml.rfind("</Objects>"), combat_xml());
    write_fixture(tree.root, xml);
    write(tree.root / "XML" / "MovementClassTypeDefs.xml",
        "<MovementClassType><Infantry>Clear | ModSurface</Infantry><ModHover>Clear | Water</ModHover></MovementClassType>");
    const auto constants = tree.root / "XML" / "GameConstants.xml";
    respell_file(constants, "Object_Max_Speed_Multiplier_Space", "Object_Max_Speed_Multiplier_Land");
    respell_file(constants, "Object_Max_Health_Multiplier_Space", "Object_Max_Health_Multiplier_Land");
    respell_file(constants, "</GameConstants>", "<Damage_To_Armor_Mod> Damage_Blaster, Armor_Infantry, 0.8 </Damage_To_Armor_Mod></GameConstants>");
    const std::vector<std::string> companies{"Shoot_Infantry_Company", "Shoot_Armor_Company"};
    const auto land = load_types(tree.root, BattleDomain::land, companies, {"Shoot_Wall"});
    if (!land.tables) return;
    const auto& tables = *land.tables;
    // The companies pull their members in: no second pass over the member names.
    const auto* trooper = tables.find("Shoot_Trooper");
    const auto* tank = tables.find("Shoot_Tank");
    const auto* tank_mod = tables.find("Shoot_Tank_Mod");
    const auto* wall = tables.find("Shoot_Wall");
    expect(trooper && tank && tank_mod && wall, "LS-P1: land members and a damageable static load as unit types");
    if (!trooper || !tank || !tank_mod || !wall) return;
    const auto number = [](const std::int64_t whole) { return Fixed::from_integer(whole).value(); };
    expect(trooper->model == "test_frigate" && trooper->model_path == "data/art/models/test_frigate.alo",
        "LS-P1: the land body reads Land_Model_Name through the domain selector");
    expect(trooper->hull == number(48) && trooper->armor_type == "Armor_Infantry" && trooper->shield_armor_type == "Shield_Default"
            && trooper->shield_points == Fixed{} && trooper->damage_type == "Damage_Blaster",
        "LS-P1: infantry health, armor and shield rows");
    expect(trooper->targeting_max_attack_distance == number(150) && trooper->targeting_stickiness_seconds == number(2)
            && trooper->idle_chase_range == number(200) && trooper->guard_chase_range == number(250)
            && trooper->attack_move_response_range == number(300) && trooper->ranged_target_z_adjust == number(3)
            && trooper->targeting_priority_set == "Frigate" && trooper->targeting_priority_set_index != eawr::units::no_index,
        "LS-P1: infantry targeting, chase and aim rows");
    expect(trooper->living_projectile_collision && trooper->dead_projectile_collision && !tank->dead_projectile_collision
            && trooper->death_clones == std::vector<std::string>{"Damage_Blaster, Shoot_Trooper_Death"}
            && trooper->death_fade_time == number(3) && trooper->death_persistence_duration == number(5),
        "LS-P1: infantry projectile collision and death rows");
    expect(trooper->weapon && trooper->weapon->projectile == "Shoot_Proj_Blaster" && trooper->weapon->damage == number(3)
            && trooper->weapon->min_recharge_seconds == Fixed::from_decimal("1.5").value() && trooper->weapon->pulse_count == 2U
            && trooper->weapon->pulse_delay_seconds == Fixed::from_decimal(".2").value() && trooper->weapon->inaccuracy.size() == 1
            && trooper->weapon->projectile_index != eawr::units::no_index,
        "LS-P1: infantry weapon rows (projectile, damage, recharge, pulses, scatter)");
    expect(tank->hull == number(100) && tank->weapon && tank->weapon->pulse_count == 4U && !tank->weapon->damage
            && tank->targeting_max_attack_distance == number(200) && tank->armor_type == "Armor_Tank",
        "LS-P1: tank rows; an object without its own Projectile_Damage keeps the projectile row's");
    expect(wall->hull == number(500) && wall->living_projectile_collision && !wall->weapon,
        "LS-P1: a damageable static loads its durability and collision rows");
    // A tag change reaches the row: the variant changes tags only.
    expect(tank_mod->hull == number(150) && tank_mod->weapon && tank_mod->weapon->damage == number(6)
            && tank_mod->targeting_max_attack_distance == number(250) && tank_mod->weapon->pulse_count == 4U,
        "LS-P1: a variant's changed tags flow to the rows, with no name list");
    const auto projectile = std::find_if(tables.projectiles.begin(), tables.projectiles.end(),
        [](const auto& row) { return row.id == "Shoot_Proj_Blaster"; });
    expect(projectile != tables.projectiles.end() && projectile->max_speed == number(11)
            && projectile->max_flight_distance == number(400) && projectile->damage == number(2)
            && projectile->damage_type == "Damage_Blaster",
        "LS-P1: the blaster projectile row loads by name");

    // The binding turns the rows into the content's durability and combat tables.
    eawr::skirmish::LandContentInputs inputs;
    inputs.unverified_passability_bits = {{"Clear", 1}, {"ModSurface", 8}, {"Water", 4}};
    inputs.collision_class_order = {{"Infantry", 7}, {"Vehicle", 9}};
    inputs.contacts = {100, 5, Fixed{}, Fixed{}};
    const auto built = eawr::skirmish::build_land_bindings(tables, companies, inputs);
    expect(static_cast<bool>(built), "LS-P1: the stock-shaped companies bind");
    if (!built) return;
    const auto& bound = built.value();
    const auto* trooper_life = durability_row(bound.durability, "Shoot_Trooper");
    const auto* tank_life = durability_row(bound.durability, "Shoot_Tank_Mod");
    // The land health multiplier is the fixture's 1.5.
    expect(trooper_life && trooper_life->max_hull == number(72) && trooper_life->max_shields == Fixed{}
            && tank_life && tank_life->max_hull == number(225) && durability_row(bound.durability, "Shoot_Wall"),
        "LS-P1: durability rows scale health by the land multiplier and carry no shield");
    const auto* trooper_fire = bound.combat.find(eawr::assets::object_type_crc("Shoot_Trooper"));
    const auto* tank_fire = bound.combat.find(eawr::assets::object_type_crc("Shoot_Tank_Mod"));
    expect(trooper_fire && trooper_fire->living_projectile_collision && trooper_fire->max_attack_distance == number(150)
            && trooper_fire->ranged_target_z_adjust == number(3) && trooper_fire->weapons.size() == 1
            && trooper_fire->weapons.front().hardpoint == object_weapon && trooper_fire->weapons.front().range == number(150)
            && trooper_fire->weapons.front().min_recharge_hundredths == 150U && trooper_fire->weapons.front().pulse_count == 2U
            && trooper_fire->weapons.front().pulse_delay_frames > 0U && trooper_fire->weapons.front().shot
            && trooper_fire->weapons.front().shot->damage == number(3) && trooper_fire->weapons.front().shot->shield_damage == false
            && trooper_fire->weapons.front().shot->inaccuracy.size() == 1,
        "LS-P1: combat rows carry range, rate, pulses, damage and scatter");
    expect(tank_fire && tank_fire->weapons.size() == 1 && tank_fire->weapons.front().pulse_count == 4U
            && tank_fire->weapons.front().shot && tank_fire->weapons.front().shot->damage == number(6)
            && tank_fire->weapons.front().range == number(250),
        "LS-P1: a tag change flows through to the combat row");
    // The 0.8 table entry is the damage x armor row the shot hits an infantry hull with.
    expect(bound.durability.damage && !bound.durability.damage->armor_mods.empty() && bound.durability.damage->damage_types > 0
            && bound.durability.damage->armor_types > 0,
        "LS-P1: the damage x armor table rides in the durability table");
    expect(bound.static_notes.empty() || std::none_of(bound.static_notes.begin(), bound.static_notes.end(),
            [](const auto& note) { return note.find("skipped unit type") != std::string::npos; }),
        "LS-P1: no stock-shaped type is skipped from the combat table");

    // A second build is the same content; a changed tag moves the binding identity; strict refuses what skip names.
    const auto again = eawr::skirmish::build_land_bindings(tables, companies, inputs);
    expect(again && again.value().identity == bound.identity && again.value().combat == bound.combat,
        "LS-P1: the combat content builds deterministically");
    auto edited = tables;
    for (auto& unit : edited.units)
        if (unit.id == "Shoot_Trooper" && unit.weapon) unit.weapon->damage = number(4);
    const auto moved = eawr::skirmish::build_land_bindings(edited, companies, inputs);
    expect(moved && moved.value().identity != bound.identity, "LS-P1: a weapon row change moves the land content identity");

    // Without a damageable or armed type the binding carries no combat tables (movement-only content).
    auto bare = tables;
    for (auto& unit : bare.units) { unit.hull.reset(); unit.weapon.reset(); }
    const auto bare_bound = eawr::skirmish::build_land_bindings(bare, companies, inputs);
    expect(bare_bound && bare_bound.value().combat.profiles.empty() && bare_bound.value().durability.profiles.empty(),
        "LS-P1: tables without combat rows bind empty combat and durability");

    // Space is untouched by the land rows: a space load of the same data is identical with and without the land objects.
    TempTree space_tree;
    write_fixture(space_tree.root, xml);
    const auto space_with = load_types(space_tree.root, BattleDomain::space, {"Skirmish_Test_Base", "Test_Frigate"});
    TempTree plain;
    write_fixture(plain.root, units_xml("3600", "Unit_Select_Test"));
    const auto space_without = load_types(plain.root, BattleDomain::space, {"Skirmish_Test_Base", "Test_Frigate"});
    expect(space_with.tables && space_without.tables && space_with.tables->projectiles.size() == space_without.tables->projectiles.size()
            && eawr::units::content_identity(*space_with.tables) == eawr::units::content_identity(*space_without.tables),
        "LS-P1: the land combat rows leave the space tables and identity alone");
}

const LandCompany* company(const eawr::units::UnitTables& tables, const std::string_view id) {
    for (const auto& entry : tables.land_companies)
        if (entry.id == id) return &entry;
    return nullptr;
}

std::vector<std::size_t> squad_sizes(const eawr::units::LandCompanyPlan& plan) {
    std::vector<std::size_t> sizes;
    for (const auto& squad : plan.squads) sizes.push_back(squad.members.size());
    return sizes;
}

} // namespace

// L2a (#1450, LC-01..LC-06): land company profiles, the expansion plan and the object-kind routes.
// A raw value times `factor` (the fixture land multiplier 2 times the frame-rate factor), divided by `divisor`.
Fixed converted(const Fixed value, const std::int64_t factor, const std::int64_t divisor = 1) {
    using eawr::sim::math::multiply;
    using eawr::sim::math::divide;
    return divide(multiply(value, Fixed::from_integer(factor).value()).value(), Fixed::from_integer(divisor).value()).value();
}

// LSL-01..11: every land physical tag at -1, 0, a fraction, absent (an empty value reads as absent) and a large value
// binds; none of them refuses the company. The applied meaning per tag is checked where it is defined.
void boundary_values(const eawr::units::UnitTables& tables, const std::span<const std::string> selected,
    const eawr::skirmish::LandContentInputs& inputs) {
    using eawr::units::LandLocomotionData;
    using Field = std::optional<Fixed> LandLocomotionData::*;
    const std::array<std::pair<const char*, Field>, 9> fields{{
        {"OverrideAcceleration", &LandLocomotionData::acceleration}, {"OverrideDeceleration", &LandLocomotionData::deceleration},
        {"Max_Rate_Of_Turn", &LandLocomotionData::rate_of_turn}, {"Stopped_Rate_Of_Turn", &LandLocomotionData::stopped_rate_of_turn},
        {"Scale_Factor", &LandLocomotionData::scale_factor}, {"Custom_Soft_Footprint_Radius", &LandLocomotionData::soft_radius},
        {"Custom_Hard_XExtent", &LandLocomotionData::hard_x}, {"Custom_Hard_YExtent", &LandLocomotionData::hard_y},
        {"FormationSpacing", &LandLocomotionData::formation_spacing}}};
    // LSL-13: 1e11 (inside the fixed range) and the saturated LSL-P1 value (`1e30`, `inf`) bind at the land physical limit.
    const std::array<std::optional<Fixed>, 7> values{Fixed::from_integer(-1).value(), Fixed{}, Fixed::from_decimal("0.5").value(),
        std::nullopt, Fixed::from_integer(100000).value(), Fixed::from_integer(100000000000).value(),
        Fixed::from_raw(std::numeric_limits<std::int64_t>::max())};
    const auto limit = eawr::skirmish::land_physical_limit;
    for (const auto& [tag, field] : fields)
        for (const auto& value : values) {
            auto variant = tables;
            for (auto& member : variant.land_companies.front().locomotion) {
                member.*field = value;
                member.collision_x = Fixed::from_integer(10).value(); member.collision_y = member.collision_x;
            }
            const auto bound = eawr::skirmish::build_land_bindings(variant, selected, inputs);
            expect(static_cast<bool>(bound), std::string("LSL boundary: ") + tag + " binds");
            if (!bound) continue;
            const auto& member = bound.value().land.companies.front().members.front();
            const auto& p = member.locomotion;
            expect(p.maximum_speed >= Fixed{} && p.acceleration >= Fixed{} && p.deceleration >= Fixed{} && p.turn_rate >= Fixed{}
                && p.radius >= Fixed{} && member.hard_x >= Fixed{} && member.hard_y >= Fixed{}
                && member.formation_spacing.value_or(Fixed{}) >= Fixed{}, std::string("LSL boundary: ") + tag + " applies a nonnegative profile");
            expect(p.maximum_speed <= limit && p.acceleration <= limit && p.deceleration <= limit && p.turn_rate <= limit
                && p.radius <= limit && member.hard_x <= limit && member.hard_y <= limit, std::string("LSL-13 boundary: ") + tag + " saturates");
        }
    const auto one_member = [&](auto change) {
        auto variant = tables;
        change(variant.land_companies.front());
        return eawr::skirmish::build_land_bindings(variant, selected, inputs);
    };
    const auto speed = [](const auto& bound) { return bound.value().land.companies.front().members.front().locomotion.maximum_speed; };
    for (const auto& raw : {std::optional<Fixed>{Fixed::from_integer(-1).value()}, std::optional<Fixed>{Fixed{}}}) {
        const auto bound = one_member([&](auto& entry) { for (auto& member : entry.members) member.max_speed = raw; });
        expect(bound && speed(bound) == Fixed{}, "LSL-03: a zero or negative Max_Speed binds an immobile member");
    }
    const auto absent = one_member([](auto& entry) { for (auto& member : entry.members) member.max_speed.reset(); });
    expect(absent && speed(absent) == converted(eawr::skirmish::type_default_max_speed, 60),
        "LSL-02: an absent Max_Speed is the type default 0.25");
    const auto fraction = one_member([](auto& entry) { for (auto& member : entry.members) member.max_speed = Fixed::from_decimal("0.5").value(); });
    expect(fraction && speed(fraction) == Fixed::from_integer(30).value(), "LSL-P1: a fractional Max_Speed converts as authored");
    for (const auto huge : {Fixed::from_integer(100000000000).value(), Fixed::from_raw(std::numeric_limits<std::int64_t>::max())}) {
        const auto bound = one_member([&](auto& entry) { for (auto& member : entry.members) member.max_speed = huge; });
        expect(bound && speed(bound) == eawr::skirmish::land_physical_limit, "LSL-13: a huge or saturated Max_Speed binds at the limit");
    }
    const auto negative_rate = one_member([](auto& entry) { entry.locomotion.front().acceleration = Fixed::from_integer(-1).value(); });
    expect(negative_rate && negative_rate.value().land.companies.front().members.front().locomotion.acceleration
            == converted(Fixed::from_integer(1).value(), 1800), "LSL-09: a negative acceleration takes the maximum-speed rule");
    const auto negative_scale = one_member([](auto& entry) { entry.locomotion.front().scale_factor = Fixed::from_integer(-2).value(); });
    expect(negative_scale && negative_scale.value().land.companies.front().members.front().locomotion.radius == Fixed::from_integer(6).value(),
        "LSL-10: a negative scale sizes the footprint by its magnitude");
    const auto zero_scale = one_member([](auto& entry) { entry.locomotion.front().scale_factor = Fixed{}; });
    expect(zero_scale && zero_scale.value().land.companies.front().members.front().locomotion.radius == Fixed{},
        "LSL-10: a zero scale binds a zero footprint");
    const auto negative_spacing = one_member([](auto& entry) { entry.formation_spacing = Fixed::from_integer(-1).value(); });
    expect(negative_spacing && negative_spacing.value().land.companies.front().formation_spacing == Fixed{}
        && negative_spacing.value().land.companies.front().company.formation_spacing == Fixed::from_integer(-1).value(),
        "LSL-11: a negative company spacing adds no spacing and stays authored in the company data");
    const auto team_zero = one_member([](auto& entry) { entry.team_max_speed = Fixed{}; });
    expect(team_zero && team_zero.value().land.companies.front().effective_team_max_speed == Fixed{},
        "LSL-03: a zero team speed binds an immobile team");
}

// Real mod values: RaW Siege_Tank's -0.4 hard extents and a stationary model prop's 0.0 speed and turn rate, with an
// empty Scale_Factor (Remake turret platforms), a zero soft radius, `1,5` acceleration and Max_Squad_Size -1.
void real_mod_values(const eawr::units::UnitTables& loaded) {
    auto tables = loaded;
    tables.constants.scalars.push_back({"Object_Max_Speed_Multiplier_Land", Fixed::from_raw(2 * Fixed::scale), {}});
    eawr::skirmish::LandContentInputs inputs;
    inputs.unverified_passability_bits = {{"Clear", 1}, {"ModSurface", 8}, {"Water", 4}};
    inputs.collision_class_order = {{"Infantry", 1}, {"Vehicle", 2}};
    inputs.contacts = {100, 5, Fixed{}, Fixed{}};
    const auto* tank = company(tables, "Mod_Siege_Tank_Company");
    expect(tank && tank->locomotion.size() == 1 && tank->locomotion.front().hard_x == Fixed::from_decimal("-0.4").value()
        && tank->locomotion.front().hard_y == tank->locomotion.front().hard_x && !has_row(tables.unresolved, "Mod_Siege_Tank", "Custom_Hard_XExtent"),
        "LSL-04: the loader keeps the authored -0.4 hard extents as parsed");
    const std::array<std::string, 1> tank_selected{"Mod_Siege_Tank_Company"};
    const auto tank_bound = eawr::skirmish::build_land_bindings(tables, tank_selected, inputs);
    expect(tank_bound && tank && tank->locomotion.front().collision_x && tank->locomotion.front().collision_y && tank_bound.value().land.companies.front().members.front().hard_x == *tank->locomotion.front().collision_x
        && tank_bound.value().land.companies.front().members.front().hard_y == *tank->locomotion.front().collision_y,
        "LSL-04: RaW Siege_Tank -0.4 hard extents bind with the model collision extents");
    const auto* guard = company(tables, "Mod_Model_Guard_Company");
    expect(guard && guard->locomotion.size() == 1 && guard->members.front().max_speed == Fixed{}
        && guard->locomotion.front().rate_of_turn == Fixed{} && !guard->locomotion.front().scale_factor
        && guard->locomotion.front().acceleration == Fixed::from_integer(1).value() && !guard->max_squad_size
        && has_row(tables.notes, "Mod_Model_Guard", "OverrideAcceleration"),
        "LSL-P1/P2: 0.0 speed and turn rate, an empty scale, `1,5` and -1 squad size decode as the retail parser reads them");
    // AU-80/81: an empty, blank or TBD boolean is no value, so the inherited or default flag stays and nothing refuses.
    const auto* blank = company(tables, "Mod_Blank_Flags_Company");
    expect(blank && blank->members.size() == 1 && blank->members.front().create_team && !blank->members.front().named_hero
        && !has_row(tables.unresolved, "Mod_Blank_Flags", "Create_Team") && !has_row(tables.unresolved, "Mod_Blank_Flags", "Walk_Transition")
        && !has_row(tables.unresolved, "Mod_Blank_Flags", "Has_Pre_Turn_Anim") && !has_row(tables.unresolved, "Mod_Blank_Flags", "Is_Named_Hero"),
        "AU-80/81: placeholder booleans leave the inherited flags and no unresolved row");
    const std::array<std::string, 1> blank_selected{"Mod_Blank_Flags_Company"};
    expect(static_cast<bool>(eawr::skirmish::build_land_bindings(tables, blank_selected, inputs)),
        "AU-80/81: a member with placeholder booleans binds");
    // LSL-05: an empty or TBD list is LC-01's direct-object path; the company object itself has no locomotor, so the
    // LC-84 formation-admission gate names it instead of an unresolved member list.
    const auto* tbd = company(tables, "Land_TBD_Company");
    expect(tbd && tbd->direct_object && !has_row(tables.unresolved, "Land_TBD_Company", "Company_Units"),
        "AU-81/LSL-05: a TBD member list is no value and takes the direct-object path");
    for (const auto* id : {"Land_Empty_Company", "Land_TBD_Company"}) {
        const std::array<std::string, 1> empty_selected{id};
        const auto empty = eawr::skirmish::build_land_bindings(tables, empty_selected, inputs);
        expect(!empty && empty.error().message.find("no authored locomotor") != std::string::npos,
            "LSL-05/LC-84: a direct-object company without a locomotor is the named formation-admission refusal");
    }
    const std::array<std::string, 1> guard_selected{"Mod_Model_Guard_Company"};
    const auto guard_bound = eawr::skirmish::build_land_bindings(tables, guard_selected, inputs);
    expect(guard_bound && guard_bound.value().land.companies.front().members.front().locomotion.maximum_speed == Fixed{}
        && guard_bound.value().land.companies.front().members.front().locomotion.turn_rate == Fixed{}
        && guard_bound.value().land.companies.front().members.front().locomotion.radius > Fixed{},
        "LSL-02/03/04: a 0.0-speed stationary member binds immobile, with the default scale and model footprint");
}

void land_companies() {
    using eawr::units::plan_land_company;
    TempTree tree;
    const auto base_xml = units_xml("3600", "Unit_Select_Test");
    auto with_land = base_xml;
    with_land.insert(with_land.rfind("</Objects>"), land_xml() + class_xml());
    write_fixture(tree.root, with_land);
    write(tree.root / "XML" / "MovementClassTypeDefs.xml",
        "<MovementClassType><Infantry>Clear | ModSurface</Infantry><ModHover>Clear | Water</ModHover></MovementClassType>");
    const auto land = load_types(tree.root, BattleDomain::land, land_types);
    if (!land.tables) return;
    const auto& tables = *land.tables;
    expect(tables.land_static_objects.size() == 3, "land scene object types decode separately from companies");
    if (tables.land_static_objects.size() == 3) {
        const auto& prop = tables.land_static_objects[0];
        const auto& proxy = tables.land_static_objects[1];
        expect(prop.land_obstacle && prop.width == Fixed::from_integer(60).value() && prop.height == Fixed::from_integer(100).value()
            && proxy.width == prop.width && proxy.x_offset == prop.x_offset,
            "WLPA-07 authored ordinary and proxy obstacle geometry decode without class/name exceptions");
        expect(prop.collision_bounds && prop.collision_bounds->min.z == Fixed::from_integer(10).value()
            && prop.collision_bounds->max.z == Fixed::from_integer(24).value()
            && prop.scale_factor == Fixed::from_integer(2).value() && !prop.collision_meshes.empty()
            && proxy.collision_bounds == prop.collision_bounds && proxy.scale_factor == prop.scale_factor && proxy.model_present,
            "WLPA-61/64: existing model loader retains XYZ bounds and collision triangles for selected proxy geometry");
        eawr::skirmish::LandStaticGate gate; gate.type = "Mod_Bad_Static"; gate.primary_passability = 2;
        const auto& bad = tables.land_static_objects[2];
        expect(bad.width == Fixed{} && !has_row(tables.unresolved, "Mod_Bad_Static", "Obstacle_Width")
            && has_row(tables.notes, "Mod_Bad_Static", "Obstacle_Width"),
            "LSL-P1: a width with no leading number reads 0 (the model fallback) with a note, never an unresolved row");
        const auto refused = eawr::skirmish::build_land_static_binding(tables, bad, gate, {});
        expect(!refused && refused.error().message.find("unverified-static-scale") != std::string::npos,
            "an absent static scale keeps its named gate");
    }

    {
        using namespace eawr;
        const auto n = [](std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); };
        units::LandStaticObject object;
        object.id = "Mod_Unequal_Static"; object.land_obstacle = true;
        object.behaviors = {"LAND_OBSTACLE", "IDLE"}; object.scale_factor = n(2);
        // Off-centre XYZ bounds: only half extents, never centre, enter the accessors.
        object.collision_bounds = units::CollisionBounds{{n(4), n(-8), n(10)}, {n(10), n(2), n(24)}};
        object.model_present = true;
        skirmish::LandStaticGate gate; gate.type = object.id;
        const std::array<sim::tactical::LandMovementMask, 4> bits{{{"Obstacle", 2}, {"Water", 4}, {"ModOverride", 8}, {"EmpireWall", 16}}};
        const auto resolved = skirmish::build_land_static_binding(tables, object, gate, bits);
        expect(resolved && resolved.value().width == n(20) && resolved.value().height == n(12)
            && resolved.value().x_offset == n(-10) && resolved.value().y_offset == n(-6)
            && resolved.value().z_extent == n(14) && resolved.value().fine_tune,
            "WLPA-61/62/64: unequal off-centre bounds exchange XY, apply nonunit scale, and default refinement true");
        object.width = Fixed{}; object.height = n(31); object.x_offset = n(9); object.y_offset = Fixed{};
        object.fine_tune = false;
        object.override_passability = "ModOverride"; object.secondary_passability = "Water";
        const auto mixed = skirmish::build_land_static_binding(tables, object, gate, bits);
        expect(mixed && mixed.value().width == n(20) && mixed.value().height == n(31)
            && mixed.value().x_offset == n(9) && mixed.value().y_offset == n(-6) && !mixed.value().fine_tune
            && mixed.value().primary_passability == 8 && mixed.value().secondary_passability == 4,
            "WLPA-61/63: explicit zero falls back independently, nonzero fields pass through unscaled, override and secondary stay separate");
        object.scale_factor.reset();
        const auto scale_missing = skirmish::build_land_static_binding(tables, object, gate, bits);
        expect(!scale_missing && scale_missing.error().message.find("unverified-static-scale") != std::string::npos,
            "absent Scale_Factor retains a named constructor-default gate");
        gate.scale_factor = n(2);
        expect(static_cast<bool>(skirmish::build_land_static_binding(tables, object, gate, bits)),
            "explicit unverified scale input can qualify an absent scale without overriding authored values");
        object.scale_factor = Fixed{};
        const auto zero_scale = skirmish::build_land_static_binding(tables, object, gate, bits);
        expect(zero_scale && zero_scale.value().scale_factor == Fixed{} && zero_scale.value().width == Fixed{}
            && zero_scale.value().height == n(31), "LSL-10: an authored zero scale binds with zero model geometry");
        object.scale_factor = n(-2);
        const auto negative_scale = skirmish::build_land_static_binding(tables, object, gate, bits);
        expect(negative_scale && negative_scale.value().width == n(20) && negative_scale.value().scale_factor == n(-2),
            "LSL-10: a negative scale sizes the model geometry by its magnitude");
        object.scale_factor = n(2); gate.scale_factor.reset();
        object.width = n(-60);
        const auto negative = skirmish::build_land_static_binding(tables, object, gate, bits);
        expect(negative && negative.value().width == n(-60),
            "LSL-08: a negative authored width is kept as parsed (it rasterizes no cells)");
        object.width = Fixed{};
        object.override_passability = "None"; object.behaviors.push_back("WALL");
        const auto united = skirmish::build_land_static_binding(tables, object, gate, bits);
        expect(united && united.value().primary_passability == 18, "WLPA-63: behavior contributions union before independent secondary occupation");
        object.collision_bounds.reset(); object.width.reset(); object.height.reset(); object.x_offset.reset(); object.y_offset.reset();
        const auto absent = skirmish::build_land_static_binding(tables, object, gate, bits);
        expect(absent && absent.value().width == Fixed{} && absent.value().height == Fixed{}, "WLPA-62: absent model returns zero geometry");
    }

    const auto* infantry = company(tables, "Land_Infantry_Company");
    expect(infantry && infantry->members.size() == 18 && !infantry->direct_object && infantry->max_squad_size == 9U
               && infantry->team_type == "Land_Trooper_Team" && infantry->team_max_speed
               && infantry->team_max_speed->raw() == Fixed::from_decimal("0.8").value().raw(),
           "LC-04: the 18-member, size-9 company decodes with its team type and team speed source");
    expect(infantry && infantry->members.front().kind == LandObjectKind::infantry && infantry->members.front().create_team
               && infantry->members.front().locomotor == "LAND_TEAM_INFANTRY_LOCOMOTOR",
           "LC-05: infantry kind and per-member locomotor class come from authored data");
    if (infantry) {
        auto authored_tables = tables;
        authored_tables.constants.scalars.push_back({"Object_Max_Speed_Multiplier_Land", Fixed::from_raw(2 * Fixed::scale), {}});
        eawr::skirmish::LandContentInputs inputs;
        inputs.unverified_passability_bits = {{"Clear", 1}, {"ModSurface", 8}, {"Water", 4}};
        inputs.collision_class_order = {{"Infantry", 7}};
        inputs.contacts = {100, 5, Fixed{}, Fixed{}};
        const std::array<std::string, 1> selected{infantry->id};
        const auto adapted = eawr::skirmish::build_land_bindings(authored_tables, selected, inputs);
        expect(static_cast<bool>(adapted), "L2b: mod-style authored company adapts without a unit-name whitelist");
        if (adapted) {
            const auto& binding = adapted.value().land.companies.front();
            const auto& profile = binding.members.front().locomotion;
            expect(profile.maximum_speed == Fixed::from_raw(60 * Fixed::scale)
                && profile.acceleration == Fixed::from_decimal("112.5").value()
                && profile.deceleration == Fixed::from_decimal("225").value()
                && profile.turn_rate == Fixed::from_integer(2).value()
                && profile.radius == Fixed::from_integer(6).value(), "L2b: BF-2 speed, frame acceleration, degrees and scaled footprint convert exactly");
            expect(binding.effective_team_max_speed == eawr::sim::math::multiply(*infantry->team_max_speed, Fixed::from_integer(60).value()).value()
                && binding.company == *infantry && binding.formation_spacing == Fixed::from_decimal(".4").value()
                && binding.members.front().collision_class == 7, "L2b: raw company identity, authored spacing and effective team speed stay distinct");
            expect(profile.collision_class == eawr::units::LandCollisionGroup::infantry,
                "WLPA-73: the adapter carries the authored class on the member profile");
            auto role_tables = authored_tables;
            role_tables.land_companies.front().formation_spacing.reset();
            for (auto& member : role_tables.land_companies.front().locomotion) member.formation_spacing.reset();
            const auto zero_spacing = eawr::skirmish::build_land_bindings(role_tables, selected, inputs);
            expect(zero_spacing && zero_spacing.value().land.companies.front().formation_spacing == Fixed{}
                && zero_spacing.value().land.companies.front().members.front().hard_x > Fixed{},
                "LC-79: absent spacing is zero extra spacing and preserves hard geometry");
            auto no_hard = role_tables;
            no_hard.land_companies.front().locomotion.front().hard_x.reset();
            no_hard.land_companies.front().locomotion.front().collision_x.reset();
            const auto no_hard_binding = eawr::skirmish::build_land_bindings(no_hard, selected, inputs);
            expect(!no_hard_binding && no_hard_binding.error().message.find("hard footprint requires") != std::string::npos,
                "soft radius never substitutes for an absent hard footprint source");
            role_tables.land_companies.front().members.back().max_speed = Fixed::from_decimal(".5").value();
            const auto slow_member = eawr::skirmish::build_land_bindings(role_tables, selected, inputs);
            expect(slow_member && slow_member.value().land.companies.front().effective_team_max_speed == Fixed::from_integer(30).value(),
                "LC-82: container base speed is min(type cap, member base speeds) before the common modifier");
            role_tables.land_companies.front().team_locomotor = "TEAM_LOCOMOTOR";
            const auto generic_binding = eawr::skirmish::build_land_bindings(role_tables, selected, inputs);
            expect(generic_binding && !generic_binding.value().land.companies.front().effective_team_max_speed,
                "LC-81/83: generic path is named and never assigned the raw .25 type speed");
            role_tables.land_companies.front().team_locomotor = "LAND_TEAM_CONTAINER_LOCOMOTOR";
            role_tables.land_companies.front().members.front().stay_in_transport = true;
            const auto transport = eawr::skirmish::build_land_bindings(role_tables, selected, inputs);
            expect(!transport && transport.error().message.find("transport-only") != std::string::npos,
                "LC-84: transport-only members do not enter movable formations");
            role_tables.land_companies.front().members.front().stay_in_transport = false;
            role_tables.land_companies.front().members.front().locomotor.clear();
            const auto immobile = eawr::skirmish::build_land_bindings(role_tables, selected, inputs);
            expect(!immobile && immobile.error().message.find("no authored locomotor") != std::string::npos,
                "LC-84: absent locomotor has no implicit walking fallback");
            expect(adapted.value().movement_masks.front().bits == 9, "WLPA-01: movement memberships combine explicitly bound bits");
            auto missing = authored_tables;
            missing.land_companies.front().locomotion.front().rate_of_turn.reset();
            const auto defaulted = eawr::skirmish::build_land_bindings(missing, selected, inputs);
            expect(defaulted && defaulted.value().land.companies.front().members.front().locomotion.turn_rate
                    == converted(eawr::skirmish::type_default_max_rate_of_turn, 60, 360),
                "LSL-02: an absent Max_Rate_Of_Turn is the type default 2.3");
            missing = authored_tables;
            auto& zero = missing.land_companies.front().locomotion.front();
            zero.soft_radius = Fixed{}; zero.collision_x = Fixed::from_integer(10).value(); zero.collision_y = zero.collision_x;
            const auto zero_radius = eawr::skirmish::build_land_bindings(missing, selected, inputs);
            expect(zero_radius && zero_radius.value().land.companies.front().members.front().locomotion.radius == Fixed::from_integer(20).value(),
                "LSL-04: an authored zero soft radius is the unauthored default, so the model bounds apply");
            boundary_values(authored_tables, selected, inputs);
            auto changed = inputs; changed.unverified_passability_bits[1].bits = 16;
            const auto remasked = eawr::skirmish::build_land_bindings(authored_tables, selected, changed);
            expect(remasked && remasked.value().identity != adapted.value().identity, "L2b: external bit gate is part of content identity");
            changed = inputs; changed.unverified_passability_bits.clear();
            expect(!eawr::skirmish::build_land_bindings(authored_tables, selected, changed), "L2b: raw passability gate cannot be omitted");
            missing = authored_tables; missing.domain = BattleDomain::space;
            expect(!eawr::skirmish::build_land_bindings(missing, selected, inputs), "L2b: space tables cannot run the land adapter");
        }
        const auto plan = plan_land_company(*infantry);
        expect(plan && plan->forms_squads && plan->squad_size_authored && plan->team_type == "Land_Trooper_Team"
                   && squad_sizes(*plan) == std::vector<std::size_t>{9, 9} && plan->members.size() == 18
                   && plan->members.front().source_company == "Land_Infantry_Company",
               "LC-03/04: eighteen members group into two squads of nine, each member keeps its company");
        // LC-01: members 3 and 10 failed; the others keep their order and regroup among themselves.
        std::vector<std::uint8_t> created(18, 1);
        created[3] = 0;
        created[10] = 0;
        const auto partial = plan_land_company(*infantry, created);
        expect(partial && partial->members.size() == 16 && partial->members[3].index == 4
                   && partial->members[9].index == 11 && squad_sizes(*partial) == std::vector<std::size_t>{9, 7},
               "LC-01/03: failed members are skipped without reordering the survivors");
        expect(!plan_land_company(*infantry, std::vector<std::uint8_t>(5, 1)), "plan refuses a created mask of the wrong size");
        const auto none = plan_land_company(*infantry, std::vector<std::uint8_t>(18, 0));
        expect(none && none->members.empty() && none->squads.empty(), "LC-01: a company whose members all failed plans nothing");
        auto first_failed = created;
        first_failed[0] = 0;
        first_failed[1] = 0;
        const auto skip_first = plan_land_company(*infantry, first_failed);
        expect(skip_first && skip_first->forms_squads && skip_first->members.front().index == 2,
               "LC-02: the first created member decides team creation");
    }

    const auto* walkers = company(tables, "Land_Walker_Company");
    expect(walkers && walkers->members.size() == 4 && walkers->members.front().kind == LandObjectKind::vehicle
               && walkers->members.front().locomotor == "WALKER_LOCOMOTOR" && walkers->team_type.empty(),
           "LC-04: a four-vehicle company decodes member by member");
    if (walkers) {
        const auto plan = plan_land_company(*walkers);
        expect(plan && !plan->forms_squads && plan->squads.empty() && plan->members.size() == 4,
               "LC-02: members without Create_Team come back directly");
    }
    const auto* single = company(tables, "Land_Single_Company");
    expect(single && single->members.size() == 1, "LC-04: a one-vehicle company has one member");

    const auto* direct = company(tables, "Land_Direct_Company");
    expect(direct && direct->direct_object && direct->members.size() == 1 && direct->members.front().type == "Land_Direct_Company"
               && direct->members.front().kind == LandObjectKind::vehicle,
           "LC-01: a company with no member list attempts its own object");

    const auto* hero = company(tables, "Land_Hero_Company");
    expect(hero && hero->members.size() == 1 && hero->members.front().kind == LandObjectKind::hero
               && hero->members.front().named_hero && hero->members.front().unique_ground_unit == "Land_Hero_Walker",
           "R8: hero identity and the unique ground container type are kept");

    const auto* generic = company(tables, "Land_Generic_Team_Company");
    expect(generic && generic->team_type == "Team" && generic->team_locomotor == "TEAM_LOCOMOTOR", "LC-03: an unauthored team type is the generic fallback");
    if (generic) {
        const auto plan = plan_land_company(*generic);
        expect(plan && plan->forms_squads && plan->team_type == "Team" && squad_sizes(*plan) == std::vector<std::size_t>{3, 2},
               "LC-03: the generic team groups by Max_Squad_Size too");
    }
    const auto* unsized = company(tables, "Land_Unsized_Company");
    expect(unsized && !unsized->max_squad_size, "an absent Max_Squad_Size stays absent");
    if (unsized) {
        const auto plan = plan_land_company(*unsized);
        expect(plan && plan->forms_squads && !plan->squad_size_authored && squad_sizes(*plan) == std::vector<std::size_t>{4},
               "an unsized team company plans one squad and says the size was not authored");
    }

    // A mod-style company with a new member type works without any name list.
    const auto* mod = company(tables, "Land_Mod_Company");
    expect(mod && mod->members.size() == 3 && mod->members[0].kind == LandObjectKind::vehicle
               && mod->members[0].locomotor == "HOVER_LOCOMOTOR" && mod->members[1].kind == LandObjectKind::structure
               && mod->members[2].type == "Land_Hover_Mod",
           "a mod company with a new member type decodes in order, repeats kept");

    // LSL-05: an empty member list is the empty default, as if absent: the company attempts its own object (LC-01).
    for (const auto id : {"Land_Empty_Company", "Land_Whitespace_Company"}) {
        const auto* empty = company(tables, id);
        expect(empty && empty->direct_object && empty->members.size() == 1 && !has_row(tables.unresolved, id, "Company_Units"),
            "LSL-05: an authored-empty member list takes the direct-object path");
    }
    // LSL-P2/LSL-07: Max_Squad_Size is a signed integer; a fraction truncates, a non-positive size groups as unsized.
    const auto* zero_size = company(tables, "Land_Bad_Size_Company");
    expect(zero_size && !zero_size->max_squad_size && has_row(tables.notes, "Land_Bad_Size_Company", "Max_Squad_Size")
        && !has_row(tables.unresolved, "Land_Bad_Size_Company", "Max_Squad_Size"), "LSL-07: a zero Max_Squad_Size is noted, not refused");
    const auto* fraction = company(tables, "Land_Fractional_Size_Company");
    expect(fraction && fraction->max_squad_size == 2U && has_row(tables.notes, "Land_Fractional_Size_Company", "Max_Squad_Size"),
        "LSL-P2: a fractional Max_Squad_Size truncates toward zero");
    real_mod_values(tables);
    expect(!company(tables, "Land_Bad_Member_Company") && has_row(tables.unresolved, "Land_Bad_Member_Company", "Company_Units"),
           "an unresolved member refuses the company");
    expect(!company(tables, "Land_Bad_Team_Company") && has_row(tables.unresolved, "Land_Bad_Team_Company", "Create_Team_Type"),
           "an unresolved team type refuses the company");
    LandCompany broken;
    broken.id = "hand_built";
    broken.members.emplace_back().create_team = true;
    broken.max_squad_size = 0;
    expect(!plan_land_company(broken), "a hand-built zero squad size plans nothing");

    // WLPA-73: the authored collision class decodes from the fixed name set; absent is None, unknown is reported.
    using eawr::units::LandCollisionGroup;
    const auto* classes = company(tables, "Land_Class_Company");
    expect(classes && classes->locomotion.size() == 12, "WLPA-73: class fixture company decodes");
    if (classes && classes->locomotion.size() == 12) {
        bool in_order = true;
        for (std::size_t i = 0; i < 11; ++i)
            in_order = in_order && classes->locomotion[i].collision_class == static_cast<LandCollisionGroup>(i);
        expect(in_order, "WLPA-73: all eleven names decode, case-insensitive and trimmed, in declaration order");
        expect(classes->locomotion[11].collision_class == LandCollisionGroup::none, "WLPA-73: an absent tag is None");
    }
    expect(eawr::units::land_collision_class_count == 11 && eawr::units::to_string(LandCollisionGroup::giant_vehicle) == "Giant Vehicle"
               && !eawr::units::parse_land_collision_class("Mod Foot") && !eawr::units::parse_land_collision_class(""),
           "WLPA-73: the name set is fixed; unknown and empty names do not parse");
    expect(has_row(tables.notes, "Land_Class_Bad", "UnitCollisionClass") && !has_row(tables.unresolved, "Land_Class_Bad", "UnitCollisionClass"),
        "LSL-06: an unknown class name is noted and leaves the None default");
    {
        eawr::skirmish::LandContentInputs refuse_inputs;
        refuse_inputs.unverified_passability_bits = {{"Clear", 1}, {"ModSurface", 8}, {"Water", 4}};
        refuse_inputs.collision_class_order = {{"Infantry", 1}, {"None", 0}};
        refuse_inputs.contacts = {100, 5, Fixed{}, Fixed{}};
        const std::array<std::string, 1> bad_selected{"Land_Bad_Class_Company"};
        auto bad_tables = tables;
        bad_tables.constants.scalars.push_back({"Object_Max_Speed_Multiplier_Land", Fixed::from_raw(2 * Fixed::scale), {}});
        const auto bad = eawr::skirmish::build_land_bindings(bad_tables, bad_selected, refuse_inputs);
        expect(bad && bad.value().land.companies.front().members.front().locomotion.collision_class == LandCollisionGroup::none,
               "LSL-06: a member with an unknown class name binds with the None class");
    }
    {
        auto reclassed = tables;
        for (auto& entry : reclassed.land_companies)
            if (entry.id == "Land_Infantry_Company")
                for (auto& member : entry.locomotion) member.collision_class = LandCollisionGroup::bike;
        expect(eawr::units::content_identity(reclassed) != eawr::units::content_identity(tables),
               "WLPA-73: re-authoring the class moves the land table identity");
    }

    // LC-05: the dispatch rows.
    using eawr::units::land_kind_route;
    expect(eawr::units::land_kind_routes().size() == 5 && land_kind_route(LandObjectKind::vehicle).deployment_gate
               && land_kind_route(LandObjectKind::structure).immobile && !land_kind_route(LandObjectKind::infantry).immobile
               && land_kind_route(LandObjectKind::hero).authored_behaviors_drive_combat
               && !land_kind_route(LandObjectKind::vehicle).authored_behaviors_drive_combat,
           "LC-05: the object-kind routes carry the kind rules");

    // Space is untouched: a space load never decodes land companies and its identity ignores them.
    const auto space_with = load_types(tree.root, BattleDomain::space, {"Skirmish_Test_Base", "Test_Frigate"});
    TempTree plain;
    write_fixture(plain.root, base_xml);
    const auto space_without = load_types(plain.root, BattleDomain::space, {"Skirmish_Test_Base", "Test_Frigate"});
    expect(space_with.tables && space_without.tables && space_with.tables->land_companies.empty()
               && eawr::units::content_identity(*space_with.tables) == eawr::units::content_identity(*space_without.tables),
           "space tables carry no land companies and keep their identity");
    if (space_with.tables) {
        auto unrelated = *space_with.tables;
        unrelated.land_companies = tables.land_companies;
        unrelated.land_movement_classes = tables.land_movement_classes;
        unrelated.land_static_objects = tables.land_static_objects;
        expect(eawr::units::canonical_encoding(unrelated) == eawr::units::canonical_encoding(*space_with.tables),
            "L2b: space encoding excludes even populated land adapter inputs");
    }
    const auto land_empty = load_types(tree.root, BattleDomain::land, {"Skirmish_Test_Base"});
    expect(land_empty.tables && land_empty.tables->land_companies.empty()
               && eawr::units::content_identity(*land_empty.tables) != eawr::units::content_identity(tables),
           "land company profiles bind into the land table identity");
    auto changed = tables;
    changed.land_companies.front().members.front().locomotor = "OTHER_LOCOMOTOR";
    expect(eawr::units::content_identity(changed) != eawr::units::content_identity(tables),
           "a member locomotor change moves the land table identity");
    land_combat_rows();
    land_selection_rows();
}

// Every company object of the installed game decodes in a land load, with no row against it.
void foc_land_companies(eawr::units::LoadInput input) {
    input.domain = BattleDomain::land;
    input.types.clear();
    for (const auto& definition : input.catalog->definitions()) {
        if (!definition.namespace_winner || definition.category != eawr::data::Category::game_object) continue;
        std::string lowered = definition.type_name;
        for (auto& letter : lowered) letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
        if (lowered.ends_with("company")) input.types.push_back(definition.id);
    }
    expect(!input.types.empty(), "L2a: the installed game has company objects");
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "L2a: installed land company tables load");
    if (!loaded) return;
    const auto& tables = loaded.value();
    std::size_t refused = 0;
    for (const auto& id : input.types) {
        if (company(tables, id)) continue;
        ++refused;
        expect(false, "L2a: installed company decodes: " + id);
    }
    expect(refused == 0 && tables.land_companies.size() == input.types.size(), "L2a: every installed company decodes once");
    const auto* squad = company(tables, "Imperial_Stormtrooper_Squad");
    expect(squad && squad->members.size() == 18 && squad->max_squad_size == 9U, "LC-04: the stock stormtrooper company has 18 members, size 9");
    if (squad) {
        const auto plan = eawr::units::plan_land_company(*squad);
        expect(plan && squad_sizes(*plan) == std::vector<std::size_t>{9, 9}, "LC-04: the stock stormtrooper company plans two squads");
    }
    std::size_t undecoded_class = 0;
    for (const auto& row : tables.unresolved) if (row.field == "UnitCollisionClass") ++undecoded_class;
    for (const auto& row : tables.notes) if (row.field == "UnitCollisionClass") ++undecoded_class;
    expect(undecoded_class == 0, "WLPA-73: every installed company member decodes its collision class");
    // Stock data is unchanged by the retail parse: every stock land value reads exactly as written (LSL-P1/P2).
    std::size_t inexact = 0;
    for (const auto& row : tables.notes)
        if (row.reason.find("(LSL-P") != std::string::npos || row.reason.find("(LSL-07") != std::string::npos) {
            ++inexact; std::cout << "stock retail-parse note: " << row.owner << ' ' << row.field << " '" << row.value << "'\n";
        }
    expect(inexact == 0, "LSL-P1/P2: no installed land value relies on a retail-parse reinterpretation");
    for (const auto& entry : tables.land_companies) {
        if (entry.members.empty() || entry.members.size() != entry.locomotion.size()) continue;
        if (entry.members.front().type == "M1_Repulsor_Tank")
            expect(entry.locomotion.front().collision_class == eawr::units::LandCollisionGroup::vehicle, "WLPA-73: stock repulsor tank is Vehicle");
        if (entry.members.front().type == "AT_AT_Walker")
            expect(entry.locomotion.front().collision_class == eawr::units::LandCollisionGroup::giant_vehicle, "WLPA-73: stock AT-AT is Giant Vehicle");
    }
    const auto* armor = company(tables, "Imperial_Armor_Group");
    expect(armor && armor->members.size() == 4 && armor->max_squad_size == 1U, "LC-04: the stock armor group has four vehicles");
}

} // namespace unit_tables_test_support
