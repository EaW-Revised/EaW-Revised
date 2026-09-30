// P2-02 (#65) unit-table contracts. The synthetic catalog, priority sets,
// constants and bone hierarchies are invented here. With EAWR_EAW_GAME_ROOT
// set, the pinned FoC fleet is also loaded read-only from the installation and
// its counts, references and content identity are pinned; nothing from the
// installation is written.

#include "eawr/assets/map.hpp"
#include "eawr/units/unit_tables.hpp"

#include "eawr/data/xml.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/tactical/damage.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/world.hpp"
#include "eawr/vfs/vfs.hpp"

#include "../../src/units/unit_internal.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace {

using eawr::units::Fixed;
using eawr::units::UnitKind;
using eawr::units::Vec3;

int failures{};

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::optional<std::string> environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
#else
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    std::string result(value);
#endif
    if (result.empty()) return std::nullopt;
    return result;
}

std::int64_t raw(const std::int64_t whole) { return whole * Fixed::scale; }

Vec3 point(const std::int64_t x, const std::int64_t y, const std::int64_t z) {
    return {Fixed::from_raw(x), Fixed::from_raw(y), Fixed::from_raw(z)};
}

struct TempTree final {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("eawr-unit-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempTree() { std::filesystem::create_directories(root); }
    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
};

void write(const std::filesystem::path& path, const std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
}

std::string units_xml(const std::string_view frigate_health, const std::string_view select_sfx) {
    return std::string(R"xml(<Objects>
<StarBase Name="Test_Base">
  <Space_Model_Name>Test_Station.alo</Space_Model_Name>
  <Affiliation>Rebel</Affiliation>
  <Tactical_Health>1600</Tactical_Health>
  <Shield_Points>300</Shield_Points>
  <Shield_Refresh_Rate>60</Shield_Refresh_Rate>
  <Armor_Type>Armor_Station</Armor_Type>
  <Shield_Armor_Type>Shield_Capital</Shield_Armor_Type>
  <Targeting_Max_Attack_Distance>800.0</Targeting_Max_Attack_Distance>
  <CategoryMask>Capital</CategoryMask>
  <AI_Combat_Power>5000</AI_Combat_Power>
  <Space_FOW_Reveal_Range>2000.0</Space_FOW_Reveal_Range>
  <Victory_Relevant>yes</Victory_Relevant>
  <Should_Be_Destroyed_When_All_Hardpoints_Destroyed> nO </Should_Be_Destroyed_When_All_Hardpoints_Destroyed>
  <SpaceBehavior>SPACE_OBSTACLE, SPAWN_SQUADRON, REVEAL</SpaceBehavior>
  <Starting_Spawned_Units_Tech_0>Test_Squadron, 2</Starting_Spawned_Units_Tech_0>
  <Reserve_Spawned_Units_Tech_0>Test_Squadron, -1</Reserve_Spawned_Units_Tech_0>
  <Spawned_Squadron_Delay_Seconds>10</Spawned_Squadron_Delay_Seconds>
  <HardPoints>HP_Test_Bay</HardPoints>
</StarBase>
<StarBase Name="Skirmish_Test_Base">
  <Variant_Of_Existing_Type>Test_Base</Variant_Of_Existing_Type>
  <Abilities SubObjectList="Yes">
    <Income_Stream_Ability Name="Test_Income"><Base_Income_Value>30</Base_Income_Value></Income_Stream_Ability>
  </Abilities>
</StarBase>
<SpaceUnit Name="Test_Frigate">
  <Space_Model_Name>TEST_FRIGATE.ALO</Space_Model_Name>
  <Scale_Factor>0.70</Scale_Factor>
  <Affiliation>Rebel</Affiliation>
  <Tactical_Health>)xml") + std::string(frigate_health) + R"xml(</Tactical_Health>
  <Shield_Points>700</Shield_Points>
  <Shield_Refresh_Rate>50</Shield_Refresh_Rate>
  <Max_Speed>2.2</Max_Speed>
  <Max_Rate_Of_Turn>0.7</Max_Rate_Of_Turn>
  <OverrideAcceleration> .04 </OverrideAcceleration>
  <Space_Layer> Frigate </Space_Layer>
  <Custom_Soft_Footprint_Radius> 40.0 </Custom_Soft_Footprint_Radius>
  <Armor_Type> Armor_Test </Armor_Type>
  <Shield_Armor_Type>Shield_Frigate</Shield_Armor_Type>
  <Targeting_Max_Attack_Distance>2000.0</Targeting_Max_Attack_Distance>
  <Targeting_Max_Attack_Distance>1000.0</Targeting_Max_Attack_Distance>
  <Targeting_Priority_Set>Frigate</Targeting_Priority_Set>
  <CategoryMask> Frigate | AntiCorvette</CategoryMask>
  <AI_Combat_Power>2200</AI_Combat_Power>
  <Space_FOW_Reveal_Range>1200.0</Space_FOW_Reveal_Range>
  <Behavior>SELECTABLE</Behavior>
  <SpaceBehavior>TARGETING,REVEAL , HIDE_WHEN_FOGGED</SpaceBehavior>
  <Victory_Relevant>yes</Victory_Relevant>
  <HardPoints> HP_Test_Laser, HP_Test_Engine, HP_Test_Missing </HardPoints>
  <Target_Bones> MISSILE_TARGET00 </Target_Bones>
  <SFXEvent_Select>)xml" + std::string(select_sfx) + R"xml(</SFXEvent_Select>
  <Unit_Abilities_Data SubObjectList="Yes">
    <Unit_Ability>
      <Type>defend</Type>
      <Recharge_Seconds>60</Recharge_Seconds>
      <Mod_Multiplier>SPEED_MULTIPLIER,	0.8f</Mod_Multiplier>
      <Mod_Multiplier>SHIELD_REGEN_MULTIPLIER, -3f</Mod_Multiplier>
      <Expiration_Seconds>15</Expiration_Seconds>
      <Supports_Autofire>True</Supports_Autofire>
    </Unit_Ability>
  </Unit_Abilities_Data>
  <Lua_Script>ObjectScript_Test</Lua_Script>
</SpaceUnit>
<Squadron Name="Test_Squadron">
  <Affiliation>Rebel</Affiliation>
  <Squadron_Units>Test_Fighter, Test_Fighter</Squadron_Units>
  <Squadron_Units>Test_Fighter</Squadron_Units>
  <Squadron_Offsets>30.0,0.0,0.0</Squadron_Offsets>
  <Squadron_Offsets>0.0,15.0,0.0</Squadron_Offsets>
  <Squadron_Offsets>0.0,-15.0,0.5</Squadron_Offsets>
  <Unit_Abilities_Data SubObjectList="Yes"><Unit_Ability><Type>HUNT</Type></Unit_Ability></Unit_Abilities_Data>
</Squadron>
<Squadron Name="Test_Squadron_Pair">
  <Variant_Of_Existing_Type>Test_Squadron</Variant_Of_Existing_Type>
  <Create_Team_Type> Test_Pair_Container </Create_Team_Type>
  <Squadron_Units>Test_Fighter, Test_Fighter</Squadron_Units>
  <Squadron_Offsets>10.0,0.0,0.0</Squadron_Offsets>
  <Squadron_Offsets>-10.0,0.0,0.0</Squadron_Offsets>
</Squadron>
<Container Name="Team">
  <Behavior>TEAM,SELECTABLE,UNIT_AI,TEAM_LOCOMOTOR</Behavior>
  <SpaceBehavior>REVEAL</SpaceBehavior>
  <Space_FOW_Reveal_Range>800.0</Space_FOW_Reveal_Range>
</Container>
<Container Name="Test_Pair_Base_Container">
  <SpaceBehavior>REVEAL</SpaceBehavior>
  <Space_FOW_Reveal_Range>800.0</Space_FOW_Reveal_Range>
  <Space_FOW_Reveal_Range>1000.0</Space_FOW_Reveal_Range>
</Container>
<Container Name="Test_Pair_Container">
  <Variant_Of_Existing_Type>Test_Pair_Base_Container</Variant_Of_Existing_Type>
</Container>
<SpaceUnit Name="Test_Fighter">
  <Space_Model_Name>test_fighter</Space_Model_Name>
  <Affiliation>Rebel</Affiliation>
  <Tactical_Health>60</Tactical_Health>
  <Shield_Points>0</Shield_Points>
  <Max_Speed>4.0</Max_Speed>
  <Max_Rate_Of_Turn>3.0</Max_Rate_Of_Turn>
  <Armor_Type>Armor_Fighter</Armor_Type>
  <Damage_Type>Damage_Fighter</Damage_Type>
  <Targeting_Max_Attack_Distance>450.0</Targeting_Max_Attack_Distance>
  <Targeting_Priority_Set>Fighter</Targeting_Priority_Set>
  <CategoryMask>Fighter | AntiBomber</CategoryMask>
  <Property_Flags> SmallShip </Property_Flags>
  <AI_Combat_Power>60</AI_Combat_Power>
  <Space_FOW_Reveal_Range>500.0</Space_FOW_Reveal_Range>
  <Projectile_Types>Proj_Test_Small</Projectile_Types>
  <Projectile_Fire_Pulse_Count>4</Projectile_Fire_Pulse_Count>
  <Projectile_Fire_Pulse_Delay_Seconds>0.1</Projectile_Fire_Pulse_Delay_Seconds>
  <Projectile_Fire_Recharge_Seconds>1.5</Projectile_Fire_Recharge_Seconds>
  <Fire_Inaccuracy_Distance> Fighter, 1.0 </Fire_Inaccuracy_Distance>
  <Fire_Inaccuracy_Distance> Frigate, 30.0 </Fire_Inaccuracy_Distance>
</SpaceUnit>
<Projectile Name="Proj_Test_Generic">
  <Max_Speed>11.0</Max_Speed>
  <Projectile_Category>Laser</Projectile_Category>
  <Projectile_Max_Flight_Distance>500.0</Projectile_Max_Flight_Distance>
  <Projectile_Does_Shield_Damage>Yes</Projectile_Does_Shield_Damage>
  <Projectile_Does_Hitpoint_Damage>Yes</Projectile_Does_Hitpoint_Damage>
</Projectile>
<Projectile Name="Proj_Test_Small">
  <Variant_Of_Existing_Type>Proj_Test_Generic</Variant_Of_Existing_Type>
  <Projectile_Damage>5.0</Projectile_Damage>
</Projectile>
<Projectile Name="Proj_Test_Turbo">
  <Damage_Type>Damage_Turbolaser</Damage_Type>
  <Max_Speed>25.0</Max_Speed>
  <Projectile_Category>Laser</Projectile_Category>
  <Projectile_Max_Flight_Distance>2200.0</Projectile_Max_Flight_Distance>
  <Projectile_Damage>15.0</Projectile_Damage>
  <Projectile_Does_Shield_Damage>Yes</Projectile_Does_Shield_Damage>
  <Projectile_Does_Hitpoint_Damage>Yes</Projectile_Does_Hitpoint_Damage>
</Projectile>
</Objects>)xml";
}

constexpr std::string_view hardpoints_xml = R"xml(<HardPoints>
<HardPoint Name="HP_Test_Laser">
  <Type> HARD_POINT_WEAPON_LASER </Type>
  <Is_Targetable>Yes</Is_Targetable>
  <Is_Destroyable>Yes</Is_Destroyable>
  <Health>260.0</Health>
  <Model_To_Attach>Test_Turret.alo</Model_To_Attach>
  <Attachment_Bone>HP_F-L_BONE</Attachment_Bone>
  <Damage_Type> Damage_Test </Damage_Type>
  <Projectile_Damage>40.0</Projectile_Damage>
  <Fire_Bone_A>FP_F-L_00</Fire_Bone_A>
  <Fire_Bone_B>FP_Missing</Fire_Bone_B>
  <Fire_Cone_Width>175.0</Fire_Cone_Width>
  <Fire_Cone_Height>160.0</Fire_Cone_Height>
  <Fire_Projectile_Type>Proj_Test_Turbo</Fire_Projectile_Type>
  <Fire_Min_Recharge_Seconds>3.0</Fire_Min_Recharge_Seconds>
  <Fire_Max_Recharge_Seconds>4.0</Fire_Max_Recharge_Seconds>
  <Fire_Pulse_Count>5</Fire_Pulse_Count>
  <Fire_Pulse_Delay_Seconds>0.2</Fire_Pulse_Delay_Seconds>
  <Fire_Range_Distance>1100.0</Fire_Range_Distance>
  <Fire_Inaccuracy_Distance> Fighter, 70.0 </Fire_Inaccuracy_Distance>
  <Fire_Inaccuracy_Distance> Corvette, 1.0 </Fire_Inaccuracy_Distance>
  <Allow_Opportunity_Fire_When_Targeting>Yes</Allow_Opportunity_Fire_When_Targeting>
</HardPoint>
<HardPoint Name="HP_Test_Engine">
  <Type> HARD_POINT_ENGINE </Type>
  <Is_Targetable>Yes</Is_Targetable>
  <Is_Destroyable>Yes</Is_Destroyable>
  <Health>170.0</Health>
  <Attachment_Bone>hp_e_bone</Attachment_Bone>
</HardPoint>
<HardPoint Name="HP_Test_Bay">
  <Type>HARD_POINT_FIGHTER_BAY</Type>
  <Is_Targetable>Yes</Is_Targetable>
  <Is_Destroyable>Yes</Is_Destroyable>
  <Health>1000.0</Health>
  <Repair_Amount_Per_Frame>.50</Repair_Amount_Per_Frame>
  <Repair_Cost_Per_Frame>1.5</Repair_Cost_Per_Frame>
  <Attachment_Bone>SPAWN_00</Attachment_Bone>
</HardPoint>
</HardPoints>)xml";

constexpr std::string_view priorities_xml = R"xml(<Targeting_Priority_Sets>
<Priority_Set Name="Frigate">
  <Attack_Priorities> Transport, 1.0, Bomber, 2.0, Corvette, 3.0, test_fighter, 0.5, SmallShip, 4.0, </Attack_Priorities>
  <Property_Exclusions>NotOpportunityTarget</Property_Exclusions>
  <Category_Exclusions> Capital, Frigate | AntiBomber </Category_Exclusions>
  <Unit_Exclusions> Destroyable_Asteroid_Small, Destroyable_Asteroid_Huge </Unit_Exclusions>
  <Hard_Point_Priorities> Shield_Generator, WEAPON_LASER </Hard_Point_Priorities>
  <Hard_Point_Exclusions> Fighter_Bay, Engine_Typo </Hard_Point_Exclusions>
</Priority_Set>
<Priority_Set Name="Fighter">
  <Attack_Priorities> Transport, 1.0, Bomber, 2.0, Fighter, 3.0 </Attack_Priorities>
  <Hard_Point_Priorities> Shield_Generator, Engine </Hard_Point_Priorities>
</Priority_Set>
<Priority_Set Name="Unused">
  <Attack_Priorities> Transport, 1.0 </Attack_Priorities>
</Priority_Set>
</Targeting_Priority_Sets>)xml";

// Invented dynamic enums: the FoC names the fixture uses, not their FoC values.
constexpr std::string_view categories_xml = R"xml(<EnumDefinition>
  <!-- a comment -->
  <Fighter> 0x01 </Fighter>
  <Bomber> 0x02 </Bomber>
  <Transport> 0x04 </Transport>
  <Corvette> 0x08 </Corvette>
  <Frigate> 0x10 </Frigate>
  <Capital> 32 </Capital>
  <AntiBomber> 0x100 </AntiBomber>
  <AntiCorvette> 0x200 </AntiCorvette>
  <All> 0xFFFFFFFFFFFFFFFF </All>
</EnumDefinition>)xml";

constexpr std::string_view properties_xml = R"xml(<EnumDefinition>
  <SmallShip> 0x1 </SmallShip>
  <NotOpportunityTarget> 0x2 </NotOpportunityTarget>
</EnumDefinition>)xml";

constexpr std::string_view constants_xml = R"xml(<GameConstants>
  <ShieldRechargeIntervalInSecs>3.0</ShieldRechargeIntervalInSecs>
  <EnergyRechargeIntervalInSecs>5.0</EnergyRechargeIntervalInSecs>
  <EnergyToShieldExchangeRate>5.0</EnergyToShieldExchangeRate>
  <Depleted_Shield_Damage_Increment>0.0</Depleted_Shield_Damage_Increment>
  <Depleted_Shield_Disable_Time>5.0</Depleted_Shield_Disable_Time>
  <Depleted_Shield_Regen_Cap>0.25</Depleted_Shield_Regen_Cap>
  <Diminishing_Firepower>0, 0.6, 0.3, 0.7, 0.9, 0.9, 1, 1, 2, 1</Diminishing_Firepower>
  <Hull_Vs_Hard_Points_Health_Constraint> 0.2 </Hull_Vs_Hard_Points_Health_Constraint>
  <Hardpoint_Recharge_Cutoff_For_Opportunity_Fire> 3.0 </Hardpoint_Recharge_Cutoff_For_Opportunity_Fire>
  <Engines_Disabled_Speed_Modifier> 0.4 </Engines_Disabled_Speed_Modifier>
  <Space_Elevated_Vulnerability_Duration>5.0</Space_Elevated_Vulnerability_Duration>
  <Space_Elevated_Vulnerability_Factor>-3.0</Space_Elevated_Vulnerability_Factor>
  <Object_Max_Speed_Multiplier_Space> 1.2 </Object_Max_Speed_Multiplier_Space>
  <Auto_Rotate_For_Space_Targeting>False</Auto_Rotate_For_Space_Targeting>
  <Bombing_Run_Reduction_Per_Squadron_Percent>0</Bombing_Run_Reduction_Per_Squadron_Percent>
  <Object_Max_Health_Multiplier_Space> 1.5 </Object_Max_Health_Multiplier_Space>
  <Health_Low_Percent_Threshold> 0.33 </Health_Low_Percent_Threshold>
  <MaxRotationsSpace> 24.0 </MaxRotationsSpace>
  <XYExpansionDistanceSpace> 300.0 </XYExpansionDistanceSpace>
  <TurnInPlaceSlowdownCorvette> 2.0 </TurnInPlaceSlowdownCorvette>
  <TurnInPlaceSlowdownFrigate> 3.0 </TurnInPlaceSlowdownFrigate>
  <TurnInPlaceSlowdownCapital> 4.0 </TurnInPlaceSlowdownCapital>
  <WaitOperatorSpeedCoefficient> .2 </WaitOperatorSpeedCoefficient>
  <WaitOperatorBaseFrameTime> 100 </WaitOperatorBaseFrameTime>
  <WaitOperatorCostCoefficient> .8 </WaitOperatorCostCoefficient>
  <MinObstacleCostSpace> 15.0 </MinObstacleCostSpace>
  <CurrentPathCostCoefficientSpace> .66 </CurrentPathCostCoefficientSpace>
  <OccupationRadiusCoefficientSpace> 1.2 </OccupationRadiusCoefficientSpace>
  <SpacePathFailureDistanceCutoffCoefficient> .25 </SpacePathFailureDistanceCutoffCoefficient>
  <SpacePathFailureMaxExpansionsCoefficient> 1.7 </SpacePathFailureMaxExpansionsCoefficient>
  <SpacePathFailureRotationExpansionIncrement> .5 </SpacePathFailureRotationExpansionIncrement>
  <SpacePathFailureForwardExpansionIncrement> .5 </SpacePathFailureForwardExpansionIncrement>
  <SpacePathfindMaxExpansions> 3500 </SpacePathfindMaxExpansions>
  <SpacePathingTries> 6 </SpacePathingTries>
  <SpaceObjectTrackingInterval> 90 </SpaceObjectTrackingInterval>
  <SpaceObjectTrackingTreeCount> 45 </SpaceObjectTrackingTreeCount>
  <DestinationSearchRadiusIncrementSpace> 50.0 </DestinationSearchRadiusIncrementSpace>
  <FormationMinimumSideError> .1 </FormationMinimumSideError>
  <FormationMaximumSideError> 30.0 </FormationMaximumSideError>
  <Damage_To_Armor_Mod> Damage_Default, Armor_Default, 1 </Damage_To_Armor_Mod>
  <Damage_To_Armor_Mod> Damage_Turbolaser, Armor_Test, 0.5 </Damage_To_Armor_Mod>
  <Damage_To_Armor_Mod> Damage_Fighter, Shield_Frigate, 0.25 </Damage_To_Armor_Mod>
  <Damage_To_Armor_Mod> Damage_Fighter, Armor_Unused, 9 </Damage_To_Armor_Mod>
  <Damage_To_Armor_Mod> Damage_Turbolaser, Armor_Fighter, -2 </Damage_To_Armor_Mod>
</GameConstants>)xml";

void write_fixture(const std::filesystem::path& root, const std::string& units) {
    write(root / "XML" / "GameObjectFiles.xml", "<Game_Object_Files><File>eawr_units.xml</File></Game_Object_Files>");
    write(root / "XML" / "HardpointDataFiles.xml", "<Hard_Point_Files><File>eawr_hardpoints.xml</File></Hard_Point_Files>");
    write(root / "XML" / "FactionFiles.xml", "<Faction_Files></Faction_Files>");
    write(root / "XML" / "CampaignFiles.xml", "<Campaign_Files></Campaign_Files>");
    write(root / "XML" / "SFXEventFiles.xml", "<SFXEvent_Files></SFXEvent_Files>");
    write(root / "XML" / "eawr_units.xml", units);
    write(root / "XML" / "eawr_hardpoints.xml", hardpoints_xml);
    write(root / "XML" / "TargetingPrioritySetFiles.xml",
          "<Targeting_Priority_Set_Files><File>EawrPriorities.xml</File></Targeting_Priority_Set_Files>");
    write(root / "XML" / "EawrPriorities.xml", priorities_xml);
    write(root / "XML" / "Enum" / "GameObjectCategoryType.xml", categories_xml);
    write(root / "XML" / "Enum" / "GameObjectPropertiesType.xml", properties_xml);
    write(root / "XML" / "GameConstants.xml", constants_xml);
}

// Rows of a bone transform: rotation `rows` and translation in the fourth column.
eawr::assets::Bone bone(const std::string& name, const std::int32_t parent, const std::array<float, 3> translation,
                        const bool rotate_quarter_turn = false) {
    eawr::assets::Bone result;
    result.name = name;
    result.parent = parent;
    const std::array<float, 9> identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
    const std::array<float, 9> quarter{0, -1, 0, 1, 0, 0, 0, 0, 1}; // +90 degrees about +Z
    const auto& rotation = rotate_quarter_turn ? quarter : identity;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) result.relative_transform[row * 4 + column] = rotation[row * 3 + column];
        result.relative_transform[row * 4 + 3] = translation[row];
    }
    return result;
}

eawr::assets::Model model(const std::string& path, std::vector<eawr::assets::Bone> bones) {
    eawr::assets::Model result;
    result.source.logical_path = path;
    result.source.source_id = "synthetic";
    result.source.layer_id = "base";
    result.bones = std::move(bones);
    return result;
}

std::map<std::string, eawr::assets::Model> models(const float engine_x) {
    std::map<std::string, eawr::assets::Model> result;
    const auto add = [&](const std::string& path, std::vector<eawr::assets::Bone> bones) {
        result.emplace(path, model(path, std::move(bones)));
    };
    add("data/art/models/test_frigate.alo", {bone("Root", -1, {0, 0, 0}), bone("HP_F-L_BONE", 0, {10, 20, 30}, true),
                                             bone("HP_E_BONE", 0, {engine_x, 0, 2.5F}),
                                             bone("MISSILE_TARGET00", 2, {1, 2, 3})});
    add("data/art/models/test_turret.alo", {bone("Root", -1, {0, 0, 0}), bone("FP_F-L_00", 0, {2, 0, 0})});
    add("data/art/models/test_station.alo", {bone("Root", -1, {0, 0, 0}), bone("SPAWN_00", 0, {0, 0, -150})});
    add("data/art/models/test_fighter.alo", {bone("Root", -1, {0, 0, 0})});
    return result;
}

struct Loaded final {
    std::optional<eawr::units::UnitTables> tables;
};

Loaded load(const std::filesystem::path& root, const std::map<std::string, eawr::assets::Model>& assets) {
    Loaded result;
    const std::array mounts{eawr::vfs::MountSpec{"base", root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "synthetic fixture mounts");
    if (!mounted) return result;
    auto catalog = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "synthetic catalog loads");
    if (!catalog) return result;
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &mounted.value();
    input.model = [&](const std::string_view path) -> const eawr::assets::Model* {
        const auto found = assets.find(std::string(path));
        return found == assets.end() ? nullptr : &found->second;
    };
    input.types = {"Skirmish_Test_Base", "Test_Frigate", "Test_Squadron", "Test_Squadron_Pair"};
    auto tables = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(tables), "synthetic tables load");
    if (tables) result.tables = std::move(tables).value();
    return result;
}

bool has_row(const std::vector<eawr::units::Unresolved>& rows, const std::string_view owner,
             const std::string_view field, const std::string_view value = {}) {
    for (const auto& row : rows) {
        if (row.owner == owner && row.field == field && (value.empty() || row.value == value)) return true;
    }
    return false;
}

void print_rows(const std::string_view label, const std::vector<eawr::units::Unresolved>& rows) {
    for (const auto& row : rows) {
        std::cout << "  " << label << ' ' << row.owner << ' ' << row.field << " '" << row.value << "': " << row.reason << '\n';
    }
}

void synthetic_tables() {
    TempTree tree;
    write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
    const auto loaded = load(tree.root, models(-40.0F));
    if (!loaded.tables) return;
    const auto& tables = *loaded.tables;

    expect(tables.units.size() == 5, "pinned types then the squadron craft");
    if (tables.units.size() != 5) return;
    expect(tables.units[4].id == "Test_Fighter" && tables.units[4].kind == UnitKind::craft, "craft follow the pinned types");

    const auto& station = tables.units[0];
    expect(station.kind == UnitKind::station && station.xml_type == "StarBase", "StarBase is a station");
    expect(station.variant_chain == std::vector<std::string>{"Skirmish_Test_Base", "Test_Base"}, "variant chain");
    expect(station.hull && station.hull->raw() == raw(1600), "station hull through the variant");
    expect(station.victory_relevant, "station is victory relevant");
    expect(station.space_fow_reveal_range && station.space_fow_reveal_range->raw() == raw(2000),
           "sensor range through the variant");
    expect(tables.units[1].space_fow_reveal_range && tables.units[1].space_fow_reveal_range->raw() == raw(1200)
               && tables.units[4].space_fow_reveal_range && tables.units[4].space_fow_reveal_range->raw() == raw(500)
               && !tables.units[2].space_fow_reveal_range,
           "ship and craft authored ranges; a squadron authors none");
    // #271: REVEAL decides; a squadron reveals through its team container, `Team` by
    // default, and a container variant keeps the last of its base's two authored ranges.
    expect(station.reveal && tables.units[1].reveal && !tables.units[4].reveal, "REVEAL on the station and ship only");
    expect(eawr::units::sensor_range(station) == std::optional(Fixed::from_raw(raw(2000)))
               && eawr::units::sensor_range(tables.units[1]) == std::optional(Fixed::from_raw(raw(1200)))
               && !eawr::units::sensor_range(tables.units[4]),
           "sensor ranges: REVEAL types only; the craft has none");
    expect(tables.units[2].team_type == "Team" && eawr::units::sensor_range(tables.units[2]) == std::optional(Fixed::from_raw(raw(800))),
           "a squadron without Create_Team_Type reveals through Team at 800");
    expect(tables.units[3].team_type == "Test_Pair_Container"
               && eawr::units::sensor_range(tables.units[3]) == std::optional(Fixed::from_raw(raw(1000))),
           "a named container reveals with its own (inherited, last) range");
    expect(station.inactive_abilities == std::vector<std::string>{"Income_Stream_Ability Test_Income"},
           "station ability sub-objects are listed as inactive");
    expect(station.spawner.has_value(), "SPAWN_SQUADRON station has a spawner");
    if (station.spawner) {
        const auto& spawner = *station.spawner;
        expect(spawner.starting.size() == 1 && spawner.starting[0].squadron == "Test_Squadron" &&
                   spawner.starting[0].count == 2 && spawner.starting[0].squadron_index == 2,
               "starting list resolves to the squadron table row");
        expect(spawner.delay_seconds && spawner.delay_seconds->raw() == raw(10), "spawn delay");
        expect(spawner.reserves.size() == 1 && spawner.reserves[0].count == -1 && !spawner.reserves_used,
               "reserves are read but flagged unused (SK-36)");
    }
    expect(station.hardpoints.size() == 1 && station.hardpoints[0].attachment.position == point(0, 0, raw(-150)),
           "station bay position from its bone");

    const auto& frigate = tables.units[1];
    expect(frigate.kind == UnitKind::ship && frigate.model_path == "data/art/models/test_frigate.alo", "frigate model path");
    expect(frigate.scale_factor && frigate.scale_factor->raw() == Fixed::from_decimal("0.70").value().raw(), "scale factor");
    expect(frigate.movement.acceleration && frigate.movement.acceleration->raw() == Fixed::from_decimal(".04").value().raw(),
           "acceleration");
    expect(frigate.movement.space_layer == "Frigate", "space layer is trimmed");
    expect(frigate.targeting_max_attack_distance && frigate.targeting_max_attack_distance->raw() == raw(1000),
           "the last of a duplicated single-value tag wins");
    expect(has_row(tables.notes, "Test_Frigate", "targeting_max_attack_distance"), "the duplicate is noted");
    expect(frigate.category_mask == std::vector<std::string>{"Frigate", "AntiCorvette"}, "category mask splits on |");
    expect(frigate.targeting_priority_set_index == 0 && tables.priority_sets.size() == 2,
           "only referenced priority sets load, in first-reference order");
    expect(frigate.category_bits == 0x210 && frigate.property_flags.empty() && frigate.property_bits == 0,
           "category bits from the enum file; no property flags");
    expect(tables.units[4].property_flags == std::vector<std::string>{"SmallShip"} && tables.units[4].property_bits == 1,
           "Property_Flags names and bits");
    expect(tables.categories.size() == 9 && tables.categories[5].name == "Capital" && tables.categories[5].value == 32 &&
               tables.categories[8].value == ~std::uint64_t{0} &&
               tables.properties.size() == 2,
           "dynamic enums load in file order, hexadecimal or decimal, 64 bits");
    if (tables.priority_sets.size() == 2) {
        using eawr::units::PriorityMatch;
        const auto& set = tables.priority_sets[0];
        const auto& entries = set.attack_priorities;
        expect(entries.size() == 5 && entries[0].match == PriorityMatch::category && entries[0].bits == 4 &&
                   entries[3].name == "test_fighter" && entries[3].match == PriorityMatch::type && entries[3].bits == 0 &&
                   entries[4].match == PriorityMatch::property && entries[4].bits == 1,
               "entries are categories, then properties, then exact types");
        expect(set.category_exclusions == std::vector<std::string>{"Capital", "Frigate", "AntiBomber"} &&
                   set.category_exclusion_bits == 0x130 && set.property_exclusion_bits == 2,
               "exclusion masks split on commas and |");
        expect(set.hard_point_priorities == std::vector<std::string>{"Shield_Generator", "WEAPON_LASER"} &&
                   set.hard_point_exclusions == std::vector<std::string>{"Fighter_Bay"},
               "hardpoint names are checked; the unknown one is dropped");
    }
    expect(frigate.lua_script == "ObjectScript_Test", "Lua_Script reference");
    expect(frigate.hardpoints.size() == 3, "every listed hardpoint has a row");
    if (frigate.hardpoints.size() == 3) {
        const auto& laser = frigate.hardpoints[0];
        expect(laser.type == eawr::units::HardpointType::weapon_laser && laser.targetable && laser.destroyable,
               "laser hardpoint flags");
        expect(laser.health && laser.health->raw() == raw(260), "hardpoint health");
        expect(laser.attachment.position == point(raw(10), raw(20), raw(30)), "attachment bone position");
        expect(laser.fire_a.position == point(raw(10), raw(22), raw(30)) && laser.fire_a.from_attached_model,
               "fire bone from the attached model, placed through the rotated attachment frame");
        expect(!laser.fire_b.position, "missing fire bone stays unresolved");
        expect(laser.weapon.has_value(), "weapon hardpoint has a weapon");
        if (laser.weapon) {
            const auto& weapon = *laser.weapon;
            expect(weapon.projectile == "Proj_Test_Turbo" && weapon.projectile_index != eawr::units::no_index &&
                       tables.projectiles[weapon.projectile_index].id == "Proj_Test_Turbo",
                   "weapon projectile index");
            expect(weapon.pulse_count == 5U && weapon.range && weapon.range->raw() == raw(1100), "pulse count and range");
            expect(weapon.inaccuracy.size() == 2 && weapon.inaccuracy[0].category == "Fighter" &&
                       weapon.inaccuracy[0].distance.raw() == raw(70),
                   "every inaccuracy row is kept");
            expect(weapon.damage_type == "Damage_Test" && weapon.opportunity_fire_when_targeting, "weapon damage type and flags");
            expect(weapon.damage && weapon.damage->raw() == raw(40), "a hardpoint's own damage (DG-25)");
        }
        expect(frigate.hardpoints[1].attachment.position == point(raw(-40), 0, raw(5) / 2),
               "bone names match without case");
        expect(frigate.hardpoints[2].id == "HP_Test_Missing" && frigate.hardpoints[2].type_name.empty(),
               "an unknown hardpoint keeps its row");
    }
    expect(frigate.target_bones.size() == 1 && frigate.target_bones[0].position == point(raw(-39), raw(2), raw(11) / 2),
           "target bone composes its parent chain");
    expect(frigate.abilities.size() == 1, "one unit ability");
    if (frigate.abilities.size() == 1) {
        const auto& ability = frigate.abilities[0];
        expect(ability.type == "DEFEND" && ability.authored_type == "defend", "ability type is upper-cased");
        expect(ability.modifiers.size() == 2 && ability.modifiers[0].value.raw() == Fixed::from_decimal("0.8").value().raw() &&
                   ability.modifiers[1].value.raw() == raw(-3),
               "C float suffixes are accepted on modifiers");
        expect(ability.supports_autofire && ability.recharge_seconds && ability.expiration_seconds, "ability timing");
    }

    const auto& squadron = tables.units[2];
    expect(squadron.kind == UnitKind::squadron && squadron.members.size() == 3, "every Squadron_Units line contributes");
    if (squadron.members.size() == 3) {
        expect(squadron.members[2].craft_index == 4 && squadron.members[2].offset == point(0, raw(-15), raw(1) / 2),
               "member offsets by order");
    }
    expect(squadron.abilities.size() == 1 && squadron.abilities[0].type == "HUNT", "squadron ability");
    const auto& pair = tables.units[3];
    expect(pair.members.size() == 2 && pair.members[1].offset == point(raw(-10), 0, 0) &&
               pair.abilities.size() == 1,
           "a list tag comes whole from the most-derived layer that authors it");
    expect(has_row(tables.notes, "Test_Squadron_Pair", "Squadron_Units", "Test_Squadron") &&
               has_row(tables.notes, "Test_Squadron_Pair", "Squadron_Offsets", "Test_Squadron"),
           "a base layer that also authors the list is noted");

    const auto& fighter = tables.units[4];
    expect(fighter.weapon.has_value(), "craft object weapon");
    if (fighter.weapon) {
        const auto index = fighter.weapon->projectile_index;
        expect(index != eawr::units::no_index && tables.projectiles[index].damage &&
                   tables.projectiles[index].damage->raw() == raw(5) && tables.projectiles[index].max_speed &&
                   tables.projectiles[index].max_speed->raw() == raw(11),
               "projectile values through its variant");
        expect(fighter.weapon->min_recharge_seconds == fighter.weapon->max_recharge_seconds, "object weapon recharge");
        expect(fighter.weapon->inaccuracy.empty(),
               "the object weapon scatters by Targeting_Fire_Inaccuracy, not the unit's Fire_Inaccuracy_Distance (DG-24)");
    }

    expect(tables.constants.scalars.size() == 39, "every required combat scalar has a row");
    for (const auto& constant : tables.constants.scalars) {
        // Diminishing_Firepower is a list: it keeps its text (#74).
        expect(constant.value.has_value() || constant.tag == "Diminishing_Firepower", constant.tag);
    }
    expect(tables.constants.damage_to_armor.size() == 3, "only fleet damage/armor rows load");
    expect(has_row(tables.notes, "GameConstants", "Damage_To_Armor_Mod", "damage_fighter, armor_fighter"),
           "missing fleet pairs are noted");

    expect(tables.unresolved.size() == 3 && has_row(tables.unresolved, "Test_Frigate", "hardpoint", "HP_Test_Missing") &&
               has_row(tables.unresolved, "HP_Test_Laser", "Fire_Bone_B", "FP_Missing") &&
               has_row(tables.unresolved, "Frigate", "Hard_Point_Exclusions", "Engine_Typo"),
           "the scan lists exactly the missing hardpoint and bone and the unknown hardpoint name");
    if (tables.unresolved.size() != 3) print_rows("unresolved", tables.unresolved);
    expect(!tables.inputs.empty() && tables.inputs.front().logical_path < tables.inputs.back().logical_path,
           "input files are listed in path order");

    // #72 durability: hull and hardpoint health x Object_Max_Health_Multiplier_Space (1.5).
    expect(station.hardpoints.size() == 1 && station.hardpoints[0].repair_amount_per_frame &&
               station.hardpoints[0].repair_amount_per_frame->raw() == Fixed::from_decimal(".50").value().raw() &&
               station.hardpoints[0].repair_cost_per_frame &&
               station.hardpoints[0].repair_cost_per_frame->raw() == Fixed::from_decimal("1.5").value().raw(),
           "station hardpoint repair values");
    expect(frigate.hardpoints.size() == 3 && !frigate.hardpoints[0].repair_amount_per_frame &&
               frigate.destroyed_with_hardpoints && !station.destroyed_with_hardpoints,
           "the absent hardpoint-death tag defaults to yes; an authored No overrides it");
    const auto durability = eawr::units::durability_table(tables);
    expect(static_cast<bool>(durability), "durability table builds");
    if (!durability) return;
    const auto& table = durability.value();
    expect(table.rules.hull_vs_hardpoints.raw() == Fixed::from_decimal("0.2").value().raw() &&
               table.rules.engines_disabled_speed.raw() == Fixed::from_decimal("0.4").value().raw() &&
               table.rules.damaged_fraction.raw() == Fixed::from_decimal("0.33").value().raw(),
           "durability rules from gameconstants.xml");
    expect(table.profiles.size() == 3, "station, ship and craft have a hull; squadrons do not");
    if (table.damage) {
        // DG-12: an authored row, a negative row that reads as 1, and an unauthored pair at 1.
        const auto types = eawr::units::damage_type_index(tables);
        const auto multiplier = [&](const std::string_view damage, const std::string_view armor) {
            return eawr::sim::tactical::armor_multiplier(*table.damage, types.damage(damage), types.armor(armor)).raw();
        };
        expect(multiplier("Damage_Turbolaser", "Armor_Test") == Fixed::from_decimal("0.5").value().raw(),
               "an authored armor row keeps its multiplier");
        expect(multiplier("Damage_Turbolaser", "Armor_Fighter") == Fixed::scale, "a negative armor row reads as 1");
        expect(multiplier("Damage_Fighter", "Armor_Fighter") == Fixed::scale, "an unauthored armor pair multiplies by 1");
    } else {
        expect(false, "the fixture has damage rules");
    }
    const auto* frigate_profile = table.find(eawr::assets::object_type_crc("Test_Frigate"));
    expect(frigate_profile != nullptr, "profiles are keyed by the object-type CRC");
    const auto* authored_no_profile = table.find(eawr::assets::object_type_crc("Skirmish_Test_Base"));
    if (authored_no_profile != nullptr) {
        auto health = eawr::sim::tactical::full_durability(*authored_no_profile);
        const auto hit = eawr::sim::tactical::apply_damage(*authored_no_profile, health, 0, Fixed::from_raw(raw(1500)));
        expect(!hit.unit_destroyed && health.hull.raw() > 0,
               "an authored No keeps the hull alive after the last hardpoint is destroyed");
    }
    if (frigate_profile != nullptr) {
        auto health = eawr::sim::tactical::full_durability(*frigate_profile);
        const auto first = eawr::sim::tactical::apply_damage(*frigate_profile, health, 0, Fixed::from_raw(raw(390)));
        const auto last = eawr::sim::tactical::apply_damage(*frigate_profile, health, 1, Fixed::from_raw(raw(255)));
        expect(!first.unit_destroyed && last.unit_destroyed && health.hull == Fixed{},
               "a ship without the tag dies when its last destroyable hardpoint is destroyed");
    }
    if (frigate_profile != nullptr) {
        using eawr::sim::tactical::HardpointRole;
        const auto& hp = frigate_profile->hardpoints;
        expect(frigate_profile->max_hull.raw() == raw(5400) && frigate_profile->max_speed &&
                   frigate_profile->max_speed->raw() == Fixed::from_decimal("2.2").value().raw(),
               "hull x 1.5 and the authored speed");
        expect(hp.size() == 3 && hp[0].role == HardpointRole::weapon && hp[0].destroyable && hp[0].max_health.raw() == raw(390) &&
                   hp[1].role == HardpointRole::engine && hp[1].max_health.raw() == raw(255) &&
                   hp[2].role == HardpointRole::other && !hp[2].destroyable && hp[2].max_health.raw() == 0,
               "hardpoint roles and health x 1.5");
        // DG-05 (#440): no unit XML tag turns this off; every M2 unit keeps FoC's default.
        expect(frigate_profile->allow_diminishing_firepower, "the diminishing-firepower flag defaults on");
    }
    const auto* station_profile = table.find(eawr::assets::object_type_crc("Skirmish_Test_Base"));
    expect(station_profile != nullptr && station_profile->max_hull.raw() == raw(2400) && !station_profile->max_speed &&
               station_profile->hardpoints.size() == 1 &&
               station_profile->hardpoints[0].role == eawr::sim::tactical::HardpointRole::fighter_bay &&
               station_profile->hardpoints[0].repair_cost_per_frame.raw() == Fixed::from_decimal("1.5").value().raw(),
           "station profile with its fighter bay and repair values");

    // #70 motion: speeds, accelerations and the rate of turn x Object_Max_Speed_Multiplier_Space (1.2).
    const auto motion = eawr::units::motion_table(tables);
    expect(static_cast<bool>(motion), "motion table builds");
    if (!motion) return;
    const auto times = [](const char* value) {
        return eawr::sim::math::multiply(Fixed::from_decimal(value).value(), Fixed::from_decimal("1.2").value()).value();
    };
    expect(motion.value().rules.arc_degrees == Fixed::from_raw(raw(15)) &&
               motion.value().rules.expansion_distance == Fixed::from_raw(raw(300)),
           "motion rules: 360 / MaxRotationsSpace and XYExpansionDistanceSpace");
    expect(motion.value().profiles.size() == 1, "only the ship moves by motion profile; craft and stations do not");
    const auto* moving = motion.value().find(eawr::assets::object_type_crc("Test_Frigate"));
    expect(moving != nullptr && moving->max_speed == times("2.2") && moving->acceleration == times(".04") &&
               moving->deceleration == moving->max_speed && moving->rate_of_turn == times("0.7") &&
               moving->turn_in_place_slowdown == Fixed::from_raw(raw(3)),
           "frigate: values x 1.2, deceleration defaults to the maximum speed, the frigate slowdown");
    // #351 BK-01: without Max_Rate_Of_Roll and Bank_Turn_Angle the engine defaults apply.
    expect(moving != nullptr && moving->roll_rate == times("2") && moving->bank_angle == Fixed::from_raw(raw(70)),
           "frigate without roll tags: roll rate 2 x 1.2, bank angle 70");
    // #71: the path finder's constants and a footprint per type with a Space_Layer.
    const auto& avoidance = motion.value().avoidance;
    expect(avoidance && avoidance->max_expansions == 3500 && avoidance->tries == 6 && avoidance->tracking_interval == 90
               && avoidance->tracking_windows == 45 && avoidance->wait_frames == Fixed::from_raw(raw(100))
               && avoidance->destination_search_increment == Fixed::from_raw(raw(50)),
           "avoidance rules from gameconstants.xml");
    const auto* footprint = motion.value().footprint(eawr::assets::object_type_crc("Test_Frigate"));
    expect(footprint != nullptr && footprint->layer == eawr::sim::tactical::SpaceLayer::frigate && !footprint->obstacle
               && footprint->radius == eawr::sim::math::multiply(Fixed::from_decimal("40").value(), Fixed::from_decimal("0.70").value()).value(),
           "the frigate is tracked in the frigate layer, soft radius 40 x 0.7");
}

// #270 ranking rules (space-targeting R-09) on invented sets; the FoC sets are checked in foc_fleet.
void priority_rules() {
    using eawr::units::attack_priority;
    using eawr::units::hard_point_priority;
    using eawr::units::HardpointType;
    using eawr::units::PriorityMatch;
    using eawr::units::unlisted_priority;
    const auto weight = [](const char* text) { return Fixed::from_decimal(text).value(); };
    const auto candidate = [](const char* id, const std::uint32_t categories, const std::uint32_t properties) {
        eawr::units::UnitType unit;
        unit.id = id;
        unit.category_bits = categories;
        unit.property_bits = properties;
        return unit;
    };
    // Categories: 1 Fighter, 2 Bomber, 4 Corvette, 8 Capital. Properties: 1 SmallShip, 2 NotOpportunityTarget.
    eawr::units::TargetingPrioritySet set;
    set.attack_priorities = {{"Fighter", weight("3.0"), PriorityMatch::category, 1},
                             {"Bomber", weight("2.0"), PriorityMatch::category, 2},
                             {"Hero_Ship", weight("0.5"), PriorityMatch::type, 0},
                             {"Ace_Fighter", weight("6.0"), PriorityMatch::type, 0},
                             {"SmallShip", weight("5.0"), PriorityMatch::property, 1}};
    set.unit_exclusions = {"Asteroid", "Hero_Ship"};
    set.category_exclusion_bits = 8;
    set.property_exclusion_bits = 2;

    const auto score = [&](const eawr::units::UnitType& unit) { return attack_priority(set, unit); };
    expect(score(candidate("X", 1, 0)) == weight("3.0"), "a category entry gives its weight");
    expect(score(candidate("XY", 3, 0)) == weight("2.0"), "several matching categories take the smallest weight");
    expect(score(candidate("Corvette", 4, 0)) == unlisted_priority, "an unlisted category still has a priority");
    expect(unlisted_priority > weight("1000000"), "unlisted ranks after every listed weight");
    expect(score(candidate("hero_ship", 8, 2)) == weight("0.5"),
           "an exact type entry wins over its categories and every exclusion");
    expect(score(candidate("Ace_Fighter", 1, 0)) == weight("6.0"), "an exact type entry wins over a smaller category weight");
    expect(!score(candidate("asteroid", 0, 0)), "a type exclusion gives no priority");
    expect(!score(candidate("Capital", 8, 0)), "a category exclusion gives no priority");
    expect(!score(candidate("Tagged_Fighter", 1, 2)), "a property exclusion gives no priority");
    expect(score(candidate("Small_Capital", 8, 1)) == weight("5.0"), "a listed property overrides a category exclusion");
    expect(score(candidate("Small_Fighter", 1, 1)) == weight("5.0"),
           "the first matching property entry replaces a smaller category score (FoC order rule)");
    set.attack_priorities.push_back({"Fighter", weight("1.0"), PriorityMatch::category, 1});
    expect(score(candidate("Small_Fighter", 1, 1)) == weight("1.0"), "a later category entry takes the minimum again");

    const eawr::units::TargetingPrioritySet empty;
    expect(attack_priority(empty, candidate("X", 1, 0)) == unlisted_priority, "an empty set ranks everything unlisted");

    set.hard_point_priorities = {"Shield_Generator", "engine", "Fighter_Bay"};
    set.hard_point_exclusions = {"Fighter_Bay", "Weapon_Laser"};
    expect(hard_point_priority(set, HardpointType::shield_generator) == weight("1.0") &&
               hard_point_priority(set, HardpointType::engine) == weight("2.0"),
           "hardpoint priority is the 1-based list position, without case");
    expect(hard_point_priority(set, HardpointType::fighter_bay) == weight("3.0"), "a listed hardpoint ignores its exclusion");
    expect(!hard_point_priority(set, HardpointType::weapon_laser), "an excluded hardpoint has no priority");
    expect(hard_point_priority(set, HardpointType::tractor_beam) == unlisted_priority, "an unlisted hardpoint ranks last");
}

void content_identity() {
    TempTree tree;
    write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
    const auto first = load(tree.root, models(-40.0F));
    const auto second = load(tree.root, models(-40.0F));
    if (!first.tables || !second.tables) return;
    const auto identity = eawr::units::content_identity(*first.tables);
    expect(identity == eawr::units::content_identity(*second.tables), "identity is stable across loads");

    const auto moved = load(tree.root, models(-41.0F));
    if (moved.tables) expect(identity != eawr::units::content_identity(*moved.tables), "a moved bone changes the identity");

    write_fixture(tree.root, units_xml("3600", "Unit_Select_Other"));
    const auto unread = load(tree.root, models(-40.0F));
    if (unread.tables) expect(identity == eawr::units::content_identity(*unread.tables), "unread tags do not change it");

    write_fixture(tree.root, units_xml("3601", "Unit_Select_Test"));
    const auto changed = load(tree.root, models(-40.0F));
    if (changed.tables) expect(identity != eawr::units::content_identity(*changed.tables), "a changed value changes it");

    eawr::sim::Replay replay;
    replay.tick_numerator = 1;
    replay.tick_denominator = 30;
    replay.content_identity = identity;
    auto bytes = eawr::sim::write_replay(replay);
    expect(static_cast<bool>(bytes), "replay with the unit identity writes");
    if (!bytes) return;
    auto parsed = eawr::sim::parse_replay(bytes.value());
    expect(parsed && parsed.value().content_identity == identity, "the replay header carries the unit identity");
}

void bind_frame_errors() {
    auto cycle = model("data/art/models/cycle.alo", {bone("A", 1, {0, 0, 0}), bone("B", 0, {0, 0, 0})});
    expect(!eawr::units::bind_frames(cycle), "a bone cycle fails");
    auto huge = model("data/art/models/huge.alo", {bone("A", -1, {std::numeric_limits<float>::infinity(), 0, 0})});
    expect(!eawr::units::bind_frames(huge), "a non-finite bone fails");
    auto chain = model("data/art/models/chain.alo", {bone("A", -1, {1, 0, 0}, true), bone("B", 0, {1, 0, 0}, true)});
    const auto frames = eawr::units::bind_frames(chain);
    expect(frames && frames.value().size() == 2 && frames.value()[1].rows[0][3].raw() == raw(1) &&
               frames.value()[1].rows[1][3].raw() == raw(1) && frames.value()[1].rows[0][0].raw() == -Fixed::scale,
           "frames compose root to bone");
}

// #71 collision footprint: each axis of the union box starts from its own bounds, so an X-wide
// mesh keeps its narrow Y (PR #343 review).
void collision_extents() {
    auto wide = model("data/art/models/wide.alo", {bone("Root", -1, {0, 0, 0})});
    eawr::assets::Mesh hull;
    hull.collidable = true;
    hull.bone = 0;
    hull.bounds_min = {-100.0F, -10.0F, -5.0F};
    hull.bounds_max = {100.0F, 10.0F, 5.0F};
    wide.meshes.push_back(hull);
    const auto frames = eawr::units::bind_frames(wide);
    expect(static_cast<bool>(frames), "the wide model's frames bind");
    if (!frames) return;
    const auto one = eawr::units::detail::collision_half_extents(wide, frames.value());
    expect(one && one->first.raw() == raw(100) && one->second.raw() == raw(10), "an X-wide mesh: half extents 100 and 10");
    // A second, Y-long mesh off to the side widens only the axes it reaches past.
    eawr::assets::Mesh mast;
    mast.collidable = true;
    mast.bone = 0;
    mast.bounds_min = {-20.0F, -30.0F, -5.0F};
    mast.bounds_max = {-10.0F, 50.0F, 5.0F};
    wide.meshes.push_back(mast);
    const auto two = eawr::units::detail::collision_half_extents(wide, frames.value());
    expect(two && two->first.raw() == raw(100) && two->second.raw() == raw(40), "two meshes: the union box's half extents");
}

// #384: Corellian_Gunboat's HP_Corellian_Gunship_04 fires along MuzzleA_03, whose z axis is
// horizontal. A TIE fighter on that axis from the weapon midpoint is at the cone's pole: the tick
// must not fail, and the 45-degree cone height leaves its 90-degree pitch outside (W-07). Its
// combat table is validated too, fire frames included.
void foc_gunboat_pole(eawr::units::LoadInput input) {
    namespace tactical = eawr::sim::tactical;
    input.types = {"Corellian_Gunboat", "TIE_Fighter"};
    input.obstacles.clear();
    auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "gunboat tables load");
    if (!loaded) return;
    auto combat = eawr::units::combat_table(loaded.value());
    expect(static_cast<bool>(combat), "the gunboat's combat table (fire frames) validates");
    if (!combat) return;
    const auto* gunboat_type = loaded.value().find("Corellian_Gunboat");
    const auto* gunboat = combat.value().find(eawr::assets::object_type_crc("Corellian_Gunboat"));
    expect(gunboat_type != nullptr && gunboat != nullptr, "gunboat profile");
    if (gunboat_type == nullptr || gunboat == nullptr) return;
    std::optional<std::uint32_t> slot;
    for (std::uint32_t index = 0; index < gunboat->weapons.size(); ++index) {
        const auto hardpoint = gunboat->weapons[index].hardpoint;
        if (hardpoint < gunboat_type->hardpoints.size() && gunboat_type->hardpoints[hardpoint].id == "HP_Corellian_Gunship_04") {
            slot = index;
        }
    }
    expect(slot && gunboat->weapons[*slot].fire_axes.has_value(), "HP_Corellian_Gunship_04 has a fire frame");
    if (!slot || !gunboat->weapons[*slot].fire_axes) return;
    const auto& weapon = gunboat->weapons[*slot];
    // The loaded axes are orthonormal only to a few raw units, so 300 x z misses the Q24 pole;
    // 300 x (x cross y) is where both the x and y projections round to zero.
    const auto pole = eawr::sim::math::cross((*weapon.fire_axes)[0], (*weapon.fire_axes)[1]);
    expect(static_cast<bool>(pole), "the gunboat frame's pole");
    if (!pole) return;
    const auto on_pole = [&](const Fixed mid_a, const Fixed mid_b, const Fixed axis) {
        return Fixed::from_raw((mid_a.raw() + mid_b.raw()) / 2 + 300 * axis.raw());
    };
    const Vec3 target{on_pole(weapon.fire_a.x, weapon.fire_b.x, pole.value().x),
        on_pole(weapon.fire_a.y, weapon.fire_b.y, pole.value().y), on_pole(weapon.fire_a.z, weapon.fire_b.z, pole.value().z)};

    tactical::TacticalSetup setup;
    setup.seed = 384;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    setup.units = {{1, gunboat->type_id, 1, Vec3{}, eawr::sim::math::identity_quat(), {}},
        {2, eawr::assets::object_type_crc("TIE_Fighter"), 2, target, eawr::sim::math::identity_quat(), {}}};
    std::vector<tactical::SensorProfile> sensors{
        {setup.units[0].type_id, Fixed::from_raw(Fixed::scale * 2000)}, {setup.units[1].type_id, Fixed::from_raw(Fixed::scale * 2000)}};
    std::sort(sensors.begin(), sensors.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    auto session = tactical::TacticalSession::create(setup, sensors, {}, {}, std::nullopt, combat.value());
    expect(static_cast<bool>(session), "gunboat session is created");
    if (!session) {
        std::cerr << eawr::core::format_diagnostic(session.error()) << '\n';
        return;
    }
    const eawr::sim::InlineExecutor executor;
    bool pole_shot = false;
    for (int tick = 0; tick < 150; ++tick) {
        auto stepped = session.value().step(executor);
        expect(static_cast<bool>(stepped), "a target on HP_Corellian_Gunship_04's bone z axis does not fail the step");
        if (!stepped) break;
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            pole_shot = pole_shot || (event.kind == tactical::CombatEventKind::weapon_fired && event.weapon == *slot);
        }
    }
    expect(!pole_shot, "HP_Corellian_Gunship_04's 45-degree cone height does not reach its pole");
}

// Read-only load of the pinned FoC fleet (#64 unit table, SK-20 to SK-24).
void foc_fleet() {
    const auto root = environment("EAWR_EAW_GAME_ROOT");
    if (!root) {
        std::cout << "FoC fleet: skipped (set EAWR_EAW_GAME_ROOT)\n";
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
    eawr::scene::VfsAssetCache cache(filesystem.value());
    const auto access = cache.access();
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &filesystem.value();
    input.model = access.model;
    auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "FoC unit tables load");
    if (!loaded) return;
    const auto& tables = loaded.value();

    const std::vector<std::string> ids{"Skirmish_Rebel_Star_Base_1", "Skirmish_Empire_Star_Base_1", "Corellian_Corvette",
        "Nebulon_B_Frigate", "Tartan_Patrol_Cruiser", "Acclamator_Assault_Ship", "Calamari_Cruiser", "Rebel_X-Wing_Squadron",
        "Y-Wing_Squadron",
        "TIE_Interceptor_Squadron", "TIE_Fighter_Squadron", "TIE_Bomber_Squadron", "X-Wing", "Y-Wing", "TIE_Interceptor",
        "TIE_Fighter", "TIE_Bomber"};
    expect(tables.units.size() == ids.size(), "pinned fleet plus five craft");
    for (std::size_t index = 0; index < ids.size() && index < tables.units.size(); ++index) {
        expect(tables.units[index].id == ids[index], "unit order: " + ids[index]);
    }
    const auto unit = [&](const std::string_view id) { return tables.find(id); };
    const auto power = [&](const std::string_view id) -> std::int64_t {
        const auto* found = unit(id);
        if (found == nullptr) return 0;
        if (found->kind != UnitKind::squadron) return found->ai_combat_power ? found->ai_combat_power->raw() / Fixed::scale : 0;
        std::int64_t sum = 0;
        for (const auto& member : found->members) {
            const auto* craft = unit(member.craft);
            if (craft != nullptr && craft->ai_combat_power) sum += craft->ai_combat_power->raw() / Fixed::scale;
        }
        return sum;
    };
    expect(power("Skirmish_Rebel_Star_Base_1") + 2 * power("Rebel_X-Wing_Squadron") + power("Y-Wing_Squadron") +
                   power("Corellian_Corvette") + power("Nebulon_B_Frigate") + power("Calamari_Cruiser") == 13525,
           "SK-24 Rebel tick-zero power");
    expect(power("Skirmish_Empire_Star_Base_1") + 2 * power("TIE_Interceptor_Squadron") + power("Tartan_Patrol_Cruiser") +
                   power("Acclamator_Assault_Ship") == 9590,
           "SK-24 Empire tick-zero power");

    const std::map<std::string, std::pair<std::size_t, std::size_t>> hardpoints{
        {"Corellian_Corvette", {8, 0}}, {"Nebulon_B_Frigate", {5, 5}}, {"Tartan_Patrol_Cruiser", {5, 0}},
        {"Acclamator_Assault_Ship", {8, 8}}, {"Calamari_Cruiser", {7, 7}}, {"Skirmish_Rebel_Star_Base_1", {7, 7}}, {"Skirmish_Empire_Star_Base_1", {7, 7}}};
    for (const auto& [id, counts] : hardpoints) {
        const auto* found = unit(id);
        if (found == nullptr) {
            expect(false, "hardpoint owner loads: " + id);
            continue;
        }
        std::size_t targetable = 0;
        for (const auto& hardpoint : found->hardpoints) {
            targetable += hardpoint.targetable ? 1U : 0U;
            if (hardpoint.targetable) expect(hardpoint.attachment.position.has_value(), "targetable hardpoint has a position: " + hardpoint.id);
        }
        expect(found->hardpoints.size() == counts.first && targetable == counts.second, "hardpoint counts: " + id);
    }
    const std::map<std::string, std::size_t> members{{"Rebel_X-Wing_Squadron", 5}, {"Y-Wing_Squadron", 3},
        {"TIE_Interceptor_Squadron", 7}, {"TIE_Fighter_Squadron", 7}, {"TIE_Bomber_Squadron", 4}};
    for (const auto& [id, count] : members) {
        const auto* found = unit(id);
        expect(found != nullptr && found->members.size() == count, "squadron composition: " + id);
    }
    const auto* nebulon = unit("Nebulon_B_Frigate");
    expect(nebulon != nullptr && nebulon->lua_script == "ObjectScript_PowerToShields", "SK-47 Lua_Script reference");
    expect(nebulon != nullptr && nebulon->abilities.size() == 1 && nebulon->abilities[0].type == "DEFEND" &&
               nebulon->abilities[0].supports_autofire,
           "Nebulon-B DEFEND supports autofire");
    // SK-22 (owner, Q1 amended 2026-09-28): the Rebel MC80. SK-47's script and DEFEND autofire, the
    // 8500 hull and 2000 shield, and no hangar (SK-23).
    const auto* mc80 = unit("Calamari_Cruiser");
    expect(mc80 != nullptr && mc80->lua_script == "ObjectScript_PowerToShields" && !mc80->spawner &&
               mc80->abilities.size() == 1 && mc80->abilities[0].type == "DEFEND" && mc80->abilities[0].supports_autofire,
           "SK-22 MC80: SK-47 script, DEFEND autofire, no hangar");
    if (mc80 != nullptr) {
        std::size_t ion = 0;
        std::size_t laser = 0;
        std::size_t engines = 0;
        for (const auto& hardpoint : mc80->hardpoints) {
            ion += hardpoint.type == eawr::units::HardpointType::weapon_ion_cannon ? 1U : 0U;
            laser += hardpoint.type == eawr::units::HardpointType::weapon_laser ? 1U : 0U;
            engines += hardpoint.type == eawr::units::HardpointType::engine ? 1U : 0U;
        }
        expect(ion == 2 && laser == 4 && engines == 1, "SK-22 MC80: 2 ion cannons, 4 lasers and the engines");
    }
    const auto* acclamator = unit("Acclamator_Assault_Ship");
    expect(acclamator != nullptr && acclamator->spawner && acclamator->spawner->starting.size() == 2 &&
               acclamator->spawner->delay_seconds && acclamator->spawner->delay_seconds->raw() == raw(5) &&
               acclamator->spawner->reserves.size() == 1 && !acclamator->spawner->reserves_used,
           "SK-23 Acclamator hangar; SK-36 reserves unused");
    expect(acclamator != nullptr && acclamator->abilities.size() == 1 && acclamator->abilities[0].type == "POWER_TO_WEAPONS" &&
               acclamator->abilities[0].authored_type == "power_to_weapons",
           "lower-case ability type");
    for (const auto* id : {"Skirmish_Rebel_Star_Base_1", "Skirmish_Empire_Star_Base_1"}) {
        const auto* station = unit(id);
        expect(station != nullptr && station->spawner && station->spawner->starting.size() == 2 &&
                   station->spawner->starting[0].count == 2 && station->spawner->delay_seconds &&
                   station->spawner->delay_seconds->raw() == raw(10),
               std::string("SK-23 station launches: ") + id);
    }
    // #68 authored ranges (Space_FOW_Reveal_Range); Skirmish_Rebel_Star_Base_1 inherits its
    // 2000 from Rebel_Star_Base_1, Corellian_Corvette authors 1200 then 1000 (the last
    // wins, as for every single-value tag).
    const std::map<std::string, std::int64_t> authored{{"Skirmish_Rebel_Star_Base_1", 2000},
        {"Skirmish_Empire_Star_Base_1", 2000}, {"Corellian_Corvette", 1000}, {"Nebulon_B_Frigate", 1200},
        {"Tartan_Patrol_Cruiser", 1200}, {"Acclamator_Assault_Ship", 1200}, {"Calamari_Cruiser", 1200}, {"X-Wing", 500}, {"TIE_Fighter", 500},
        {"TIE_Interceptor", 500}, {"Y-Wing", 600}, {"TIE_Bomber", 600}};
    // #271 sensors (V-01, V-03): REVEAL types with their own range; squadrons through their
    // team container (`Team` 800; Y-Wing_Squadron names Y_Wing_Squadron_Container, a variant
    // of Darth_Vader_TIE_Fighter_Container, 800 then 1000). Of the craft only the Y-Wing
    // authors REVEAL.
    const std::map<std::string, std::int64_t> sensors{{"Skirmish_Rebel_Star_Base_1", 2000},
        {"Skirmish_Empire_Star_Base_1", 2000}, {"Corellian_Corvette", 1000}, {"Nebulon_B_Frigate", 1200},
        {"Tartan_Patrol_Cruiser", 1200}, {"Acclamator_Assault_Ship", 1200}, {"Calamari_Cruiser", 1200}, {"Y-Wing", 600},
        {"Rebel_X-Wing_Squadron", 800}, {"TIE_Interceptor_Squadron", 800}, {"TIE_Fighter_Squadron", 800},
        {"TIE_Bomber_Squadron", 800}, {"Y-Wing_Squadron", 1000}};
    for (const auto& item : tables.units) {
        const auto expected = authored.find(item.id);
        if (expected == authored.end()) {
            expect(item.kind == UnitKind::squadron && !item.space_fow_reveal_range, "squadron authors no range: " + item.id);
        } else {
            expect(item.space_fow_reveal_range && item.space_fow_reveal_range->raw() == raw(expected->second),
                   "authored range: " + item.id);
        }
        const auto sensor = sensors.find(item.id);
        const auto range = eawr::units::sensor_range(item);
        expect(sensor == sensors.end() ? !range : range && range->raw() == raw(sensor->second), "sensor range: " + item.id);
    }
    // #665 (walk WSU-12): every M2 craft sets Mouse_Collide_Override_Sphere_Radius 50; ships and
    // squadron containers set none and are picked by their collision meshes.
    const std::set<std::string> pick_spheres{"X-Wing", "Y-Wing", "TIE_Fighter", "TIE_Interceptor", "TIE_Bomber"};
    for (const auto& item : tables.units) {
        const bool sphere = pick_spheres.contains(item.id);
        expect(sphere ? item.mouse_collide_sphere_radius && item.mouse_collide_sphere_radius->raw() == raw(50)
                      : !item.mouse_collide_sphere_radius,
               "pick sphere (WSU-12): " + item.id);
    }
    std::size_t spawners = 0;
    for (const auto& item : tables.units) spawners += item.spawner ? 1U : 0U;
    expect(spawners == 3, "no other roster ship spawns squadrons (SK-23)");
    expect(tables.priority_sets.size() == 5, "Corvette, Frigate, Capital, Fighter and Bomber priority sets");
    // #270: the FoC space sets (SpaceUnitTargetingPriorities.xml) and R-09 on the fleet.
    for (const auto& set : tables.priority_sets) {
        bool categories = !set.attack_priorities.empty();
        for (const auto& entry : set.attack_priorities) {
            categories = categories && entry.match == eawr::units::PriorityMatch::category && entry.bits != 0;
        }
        expect(categories, "every FoC space entry is a category: " + set.id);
        expect(set.property_exclusions == std::vector<std::string>{"NotOpportunityTarget"} &&
                   set.property_exclusion_bits == 0x100 && set.category_exclusion_bits == 0 &&
                   set.unit_exclusions == std::vector<std::string>{"Destroyable_Asteroid_Small",
                       "Destroyable_Asteroid_Medium", "Destroyable_Asteroid_Large", "Destroyable_Asteroid_Huge"} &&
                   set.hard_point_priorities.empty() && set.hard_point_exclusions.empty(),
               "every FoC space set excludes NotOpportunityTarget and the destroyable asteroids: " + set.id);
    }
    const auto set_of = [&](const std::string_view id) -> const eawr::units::TargetingPrioritySet* {
        for (const auto& set : tables.priority_sets) {
            if (set.id == id) return &set;
        }
        return nullptr;
    };
    const auto* fighter_set = set_of("Fighter");
    const auto* x_wing = unit("X-Wing");
    const auto* y_wing = unit("Y-Wing");
    const auto* corellian = unit("Corellian_Corvette");
    const auto* frigate = unit("Nebulon_B_Frigate");
    expect(fighter_set != nullptr && x_wing != nullptr && y_wing != nullptr && corellian != nullptr && frigate != nullptr,
           "the Fighter set and the ranked fleet types load");
    if (fighter_set != nullptr && x_wing != nullptr && y_wing != nullptr && corellian != nullptr && frigate != nullptr) {
        const auto& entries = fighter_set->attack_priorities;
        expect(entries.size() == 3 && entries[0].name == "Transport" && entries[0].weight.raw() == raw(1) &&
                   entries[1].name == "Bomber" && entries[1].weight.raw() == raw(2) && entries[2].name == "Fighter" &&
                   entries[2].weight.raw() == raw(3),
               "FoC Fighter set: Transport 1, Bomber 2, Fighter 3");
        const auto y_score = eawr::units::attack_priority(*fighter_set, *y_wing);
        const auto x_score = eawr::units::attack_priority(*fighter_set, *x_wing);
        expect(y_score && x_score && *y_score < *x_score && x_score->raw() == raw(3),
               "S-01: the Fighter set ranks a Y-Wing (Bomber) above an X-Wing (Fighter)");
        expect(eawr::units::attack_priority(*fighter_set, *corellian) == eawr::units::unlisted_priority,
               "the Fighter set does not list Corvette: ranked last, not excluded");
        auto flagged = *frigate;
        flagged.property_bits |= 0x100;
        expect(!eawr::units::attack_priority(*fighter_set, flagged), "a NotOpportunityTarget type has no priority");
        auto asteroid = *frigate;
        asteroid.id = "Destroyable_Asteroid_Large";
        expect(!eawr::units::attack_priority(*fighter_set, asteroid), "a destroyable asteroid has no priority");
    }
    expect(tables.categories.size() == 30 && tables.properties.size() == 14, "FoC category and property enums");
    for (const auto& item : tables.units) {
        // Squadrons are containers; only Rebel_X-Wing_Squadron authors a CategoryMask.
        expect((item.kind == UnitKind::squadron || item.category_bits != 0) && (item.property_bits & 0x100) == 0,
               "fleet type has categories and is an opportunity target: " + item.id);
    }
    expect(tables.constants.scalars.size() == 39, "combat scalars");
    // #70: the five roster ships move; Max_Speed, OverrideAcceleration/Deceleration and
    // Max_Rate_Of_Turn x 1.2 (docs/behaviour/space-movement.md MV-01).
    const auto motion = eawr::units::motion_table(tables);
    expect(motion && motion.value().profiles.size() == 5 && motion.value().rules.arc_degrees == Fixed::from_raw(raw(15)) &&
               motion.value().rules.expansion_distance == Fixed::from_raw(raw(300)),
           "FoC motion table: five ships, 15 degree arcs, 300 unit expansion distance");
    // #599 (space-fighters FO-11): the group move's lane steer reads FormationMinimumSideError
    // and FormationMaximumSideError.
    expect(motion && motion.value().squadrons.side_error_max == Fixed::from_raw(raw(30))
               && motion.value().squadrons.side_error_min.raw() > 0
               && motion.value().squadrons.side_error_min < Fixed::from_raw(raw(1)),
           "FoC squadron table: side errors 0.1 and 30");
    if (motion) {
        const auto* corvette = motion.value().find(eawr::assets::object_type_crc("Corellian_Corvette"));
        const auto* assault = motion.value().find(eawr::assets::object_type_crc("Acclamator_Assault_Ship"));
        const auto near = [](const Fixed value, const char* expected) {
            const auto difference = value.raw() - Fixed::from_decimal(expected).value().raw();
            return difference >= -2 && difference <= 2;
        };
        expect(corvette != nullptr && near(corvette->max_speed, "3.72") && near(corvette->acceleration, "0.06") &&
                   near(corvette->deceleration, "0.06") && corvette->rate_of_turn == Fixed::from_decimal("1.5").value() &&
                   corvette->turn_in_place_slowdown == Fixed::from_raw(raw(2)),
               "Corellian corvette: 3.72 per frame, 0.06 per frame squared, 1.5 degrees per frame, slowdown 2");
        expect(assault != nullptr && near(assault->max_speed, "2.64") && near(assault->rate_of_turn, "0.6") &&
                   assault->turn_in_place_slowdown == Fixed::from_raw(raw(3)),
               "Acclamator: 2.64 per frame, 0.6 degrees per frame, frigate slowdown 3");
        // #351 BK-01: Max_Rate_Of_Roll 0.2 x 1.2 for all four; Bank_Turn_Angle as authored.
        const auto* nebulon_motion = motion.value().find(eawr::assets::object_type_crc("Nebulon_B_Frigate"));
        const auto* tartan = motion.value().find(eawr::assets::object_type_crc("Tartan_Patrol_Cruiser"));
        const auto banks = [&near](const eawr::sim::tactical::MotionProfile* profile, const std::int64_t angle) {
            return profile != nullptr && near(profile->roll_rate, "0.24") && profile->bank_angle == Fixed::from_raw(raw(angle));
        };
        expect(banks(corvette, 15) && banks(tartan, 15) && banks(nebulon_motion, 5) && banks(assault, 20),
               "banking: roll rate 0.24 per frame; bank 15 (corvette, Tartan), 5 (Nebulon-B), 20 (Acclamator)");
        // #447 SP-01: the M2 craft spin away on death, rebel 0.2 and imperial 0.4, for 2 s.
        const auto spins = [&](const char* id, const char* chance) {
            const auto* craft = motion.value().squadrons.find_craft(eawr::assets::object_type_crc(id));
            return craft != nullptr && craft->spin_away && near(craft->spin_away->chance, chance)
                && craft->spin_away->time == Fixed::from_raw(raw(2));
        };
        expect(spins("X-Wing", "0.2") && spins("Y-Wing", "0.2") && spins("TIE_Fighter", "0.4")
                   && spins("TIE_Interceptor", "0.4") && spins("TIE_Bomber", "0.4"),
               "spin-away: X-wing and Y-wing 0.2, TIE fighter, interceptor and bomber 0.4, all 2 s");
        // #71 footprints (docs/behaviour/space-movement.md AV-05, docs/unit-data.md).
        using eawr::sim::tactical::SpaceLayer;
        const auto close = [](const Fixed value, const char* expected) {
            const auto difference = value.raw() - Fixed::from_decimal(expected).value().raw();
            return difference >= -(std::int64_t{1} << 14) && difference <= (std::int64_t{1} << 14);
        };
        const auto print = [&](const char* id) {
            const auto* footprint = motion.value().footprint(eawr::assets::object_type_crc(id));
            if (footprint == nullptr) {
                std::cout << "footprint " << id << ": none" << std::endl;
                return footprint;
            }
            std::cout << "footprint " << id << ": layer " << static_cast<int>(footprint->layer) << " x "
                      << footprint->x_extent.raw() << " y " << footprint->y_extent.raw() << " radius "
                      << footprint->radius.raw() << (footprint->obstacle ? " obstacle" : "") << std::endl;
            return footprint;
        };
        const auto* corvette_print = print("Corellian_Corvette");
        const auto* nebulon_print = print("Nebulon_B_Frigate");
        print("Tartan_Patrol_Cruiser");
        print("Acclamator_Assault_Ship");
        print("Skirmish_Rebel_Star_Base_1");
        print("Skirmish_Empire_Star_Base_1");
        const auto* pad = print("Mineral_Extractor_Pad");
        print("Defense_Satellite_Laser_Pad");
        print("N_Gravity_Well_Station");
        print("Skirmish_Merchant_Dock");
        const auto* container = print("Orbital_Resource_Container");
        expect(corvette_print != nullptr && corvette_print->layer == SpaceLayer::corvette
                   && close(corvette_print->x_extent, "17.755") && close(corvette_print->y_extent, "41.822")
                   && corvette_print->radius == corvette_print->y_extent,
               "corvette footprint: the RV_CORVETTE.ALO collision box x 0.5");
        expect(nebulon_print != nullptr && nebulon_print->layer == SpaceLayer::frigate && close(nebulon_print->x_extent, "23.48")
                   && close(nebulon_print->y_extent, "109.949"),
               "Nebulon-B footprint: the RV_NEBULONB.ALO collision box x 0.7");
        expect(pad != nullptr && pad->layer == SpaceLayer::static_object && pad->obstacle
                   && pad->radius == Fixed::from_decimal("206.25").value(),
               "the mining pad is a static obstacle of 275 x 0.75");
        expect(container == nullptr, "a type without Space_Layer is not tracked");
        // #372 review: validate_motion rejects a radius whose outer destination search ring holds
        // more than 2^13 points; FoC's smallest soft radius, the corvette's, puts 245 there.
        const eawr::sim::tactical::Footprint* smallest = nullptr;
        for (const auto& footprint : motion.value().footprints) {
            if (footprint.radius.raw() > 0 && (smallest == nullptr || footprint.radius < smallest->radius)) smallest = &footprint;
        }
        if (smallest != nullptr) {
            std::string name;
            for (const auto& type : tables.units) {
                if (eawr::assets::object_type_crc(type.id) == smallest->type_id) name = type.id;
            }
            for (const auto& obstacle : tables.obstacles) {
                if (eawr::assets::object_type_crc(obstacle.id) == smallest->type_id) name = obstacle.id;
            }
            std::cout << "smallest soft radius: " << name << " radius " << smallest->radius.raw() << std::endl;
        }
        expect(smallest != nullptr && smallest == corvette_print, "the corvette has FoC's smallest soft radius");
        expect(motion.value().avoidance.has_value(), "FoC avoidance rules load");
    }

    // #76 abilities from FoC data (docs/behaviour/space-abilities.md AB-01 to AB-04): the four
    // modelled kinds on six types, squadron types and the cut HUNT left out; #561 adds the Y-wing
    // squadron's ION_CANNON_SHOT (AB-60) on the squadron and its craft. The MC80 (SK-22) carries the
    // Nebulon-B's DEFEND multipliers and script, with a 40 s recharge.
    const std::vector<eawr::sim::tactical::PlayerId> humans{1};
    const auto abilities = eawr::units::ability_table(tables, humans);
    expect(static_cast<bool>(abilities), "FoC ability table builds");
    if (abilities) {
        using eawr::sim::tactical::AbilityKind;
        const auto& table = abilities.value();
        expect(table.profiles.size() == 8 && table.humans == humans, "eight types carry a modelled ability");
        const auto decimal = [](const char* text) { return Fixed::from_decimal(text).value(); };
        struct Expected {
            const char* id;
            AbilityKind kind;
            std::uint32_t expiration;
            std::uint32_t recharge;
            const char* weapon_delay;
            const char* shield_regen;
            const char* shield_interval;
            const char* energy_regen;
            const char* energy_interval;
            const char* speed;
            bool autofire;
            bool script;
        };
        const Expected rows[] = {
            {"Nebulon_B_Frigate", AbilityKind::defend, 450, 1800, "1", "1", "0.1", "3", "0.1", "0.8", true, true},
            {"Calamari_Cruiser", AbilityKind::defend, 450, 1200, "1", "1", "0.1", "3", "0.1", "0.8", true, true},
            {"Corellian_Corvette", AbilityKind::turbo, 600, 1500, "3", "0", "1", "1", "1", "2", false, false},
            {"Tartan_Patrol_Cruiser", AbilityKind::power_to_weapons, 210, 1800, "0.2", "-25", "1", "1", "1", "0.5", false, false},
            {"Acclamator_Assault_Ship", AbilityKind::power_to_weapons, 600, 1800, "0.5", "-3", "1", "1", "1", "0.5", false, false},
            {"X-Wing", AbilityKind::spoiler_lock, 0, 0, "3", "3", "1", "3", "1", "1.3", false, false},
        };
        for (const auto& row : rows) {
            const auto* found = table.find(eawr::assets::object_type_crc(row.id));
            const bool shaped = found != nullptr && found->abilities.size() == 1;
            expect(shaped, std::string("one modelled ability: ") + row.id);
            if (!shaped) continue;
            const auto& ability = found->abilities[0];
            const auto& m = ability.modifiers;
            expect(ability.kind == row.kind && ability.expiration_frames == row.expiration && ability.recharge_frames == row.recharge
                       && m.weapon_delay == decimal(row.weapon_delay) && m.shield_regen == decimal(row.shield_regen)
                       && m.shield_regen_interval == decimal(row.shield_interval) && m.energy_regen == decimal(row.energy_regen)
                       && m.energy_regen_interval == decimal(row.energy_interval) && m.speed == decimal(row.speed)
                       && ability.supports_autofire == row.autofire && found->defend_script == row.script,
                std::string("FoC ability values: ") + row.id);
        }
        // AB-60: the team ability from Y_Wing_Squadron_Container (Recharge_Seconds 20, autofire),
        // and each craft's own slot of the kind.
        const auto* team = table.find(eawr::assets::object_type_crc("Y-Wing_Squadron"));
        expect(team != nullptr && team->abilities.size() == 1 && team->abilities[0].kind == AbilityKind::ion_cannon_shot
                   && team->abilities[0].team && team->abilities[0].recharge_frames == 600
                   && team->abilities[0].supports_autofire,
            "AB-60: the Y-wing squadron holds the team ION_CANNON_SHOT");
        const auto* craft = table.find(eawr::assets::object_type_crc("Y-Wing"));
        expect(craft != nullptr && craft->abilities.size() == 1 && craft->abilities[0].kind == AbilityKind::ion_cannon_shot
                   && !craft->abilities[0].team,
            "AB-60: each Y-wing has its own ION_CANNON_SHOT slot");
        for (const char* id : {"Rebel_X-Wing_Squadron", "TIE_Fighter", "TIE_Fighter_Squadron"}) {
            expect(table.find(eawr::assets::object_type_crc(id)) == nullptr, std::string("no modelled ability profile: ") + id);
        }
    }

    // #72 durability from FoC data (docs/behaviour/space-hardpoints.md HD-01 to HD-05).
    const auto durability = eawr::units::durability_table(tables);
    expect(static_cast<bool>(durability), "FoC durability table builds");
    if (durability) {
        using eawr::sim::tactical::HardpointRole;
        const auto& table = durability.value();
        expect(table.rules.hull_vs_hardpoints.raw() == Fixed::from_decimal("0.2").value().raw() &&
                   table.rules.engines_disabled_speed.raw() == Fixed::from_decimal("0.4").value().raw() &&
                   table.rules.damaged_fraction.raw() == Fixed::from_decimal("0.33").value().raw(),
               "FoC durability rules 0.2, 0.4 and 0.33");
        expect(table.profiles.size() == 12, "7 stations and ships and 5 craft have a hull");
        const auto profile = [&](const std::string_view id) { return table.find(eawr::assets::object_type_crc(id)); };
        const auto roles = [](const eawr::sim::tactical::DurabilityProfile& item) {
            std::string text;
            for (const auto& hardpoint : item.hardpoints) {
                text += std::string(eawr::sim::tactical::to_string(hardpoint.role)) + (hardpoint.destroyable ? "+" : "-") +
                        std::to_string(hardpoint.max_health.raw() / Fixed::scale) + " ";
            }
            return text;
        };
        const auto* nebulon_profile = profile("Nebulon_B_Frigate");
        expect(nebulon_profile != nullptr && nebulon_profile->max_hull.raw() == raw(5400) &&
                   roles(*nebulon_profile) == "weapon+390 weapon+390 weapon+390 weapon+390 engine+390 ",
               "Nebulon-B: 3600 x 1.5 hull, 4 lasers and the engines at 260 x 1.5");
        const auto* acclamator_profile = profile("Acclamator_Assault_Ship");
        expect(acclamator_profile != nullptr && acclamator_profile->max_hull.raw() == raw(3000) &&
                   roles(*acclamator_profile) ==
                       "weapon+210 weapon+210 weapon+240 weapon+210 weapon+210 weapon+240 engine+255 fighter_bay+150 ",
               "Acclamator: 6 weapons, engines and the fighter bay");
        const auto* mc80_profile = profile("Calamari_Cruiser");
        expect(mc80_profile != nullptr && mc80_profile->max_hull.raw() == raw(12750) &&
                   roles(*mc80_profile) == "weapon+630 weapon+630 weapon+630 weapon+630 weapon+630 weapon+630 engine+735 ",
               "MC80: 8500 x 1.5 hull, 2 ion cannons and 4 lasers at 420 x 1.5, the engines at 490 x 1.5: "
                   + (mc80_profile == nullptr ? std::string("none")
                                              : std::to_string(mc80_profile->max_hull.raw() / Fixed::scale) + " " + roles(*mc80_profile)));
        for (const auto* id : {"Corellian_Corvette", "Tartan_Patrol_Cruiser"}) {
            const auto* corvette = profile(id);
            bool none_destroyable = corvette != nullptr && !corvette->hardpoints.empty();
            if (corvette != nullptr) {
                for (const auto& hardpoint : corvette->hardpoints) none_destroyable = none_destroyable && !hardpoint.destroyable;
            }
            expect(none_destroyable, std::string("corvette weapons are not destroyable: ") + id);
        }
        for (const auto* id : {"Skirmish_Rebel_Star_Base_1", "Skirmish_Empire_Star_Base_1"}) {
            const auto* station_profile = profile(id);
            std::size_t shield_generators = 0;
            bool repairable = station_profile != nullptr;
            if (station_profile != nullptr) {
                for (const auto& hardpoint : station_profile->hardpoints) {
                    if (hardpoint.role == HardpointRole::shield_generator) {
                        ++shield_generators;
                        repairable = repairable && hardpoint.max_health.raw() == raw(975) &&
                                     hardpoint.repair_amount_per_frame.raw() == Fixed::from_decimal(".50").value().raw() &&
                                     hardpoint.repair_cost_per_frame.raw() == Fixed::from_decimal("1.5").value().raw();
                    }
                }
            }
            expect(station_profile != nullptr && station_profile->max_hull.raw() == raw(2400) && shield_generators == 1 &&
                       repairable,
                   std::string("station: 1600 x 1.5 hull, one repairable shield generator at 650 x 1.5: ") + id);
        }
        for (const auto& item : tables.units) {
            if (item.kind == UnitKind::squadron) continue;
            if (item.kind != UnitKind::station) {
                for (const auto& hardpoint : item.hardpoints) {
                    expect(!hardpoint.repair_amount_per_frame && !hardpoint.repair_cost_per_frame,
                           "no ship or craft hardpoint authors repair: " + hardpoint.id);
                }
            }
            expect(item.destroyed_with_hardpoints, "FoC fleet type defaults to death with its hardpoints: " + item.id);
        }
    }

    const auto identity = eawr::units::content_identity(tables);
    constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (const auto byte : identity) {
        hex.push_back(digits[byte >> 4U]);
        hex.push_back(digits[byte & 15U]);
    }
    std::cout << "FoC fleet: " << tables.units.size() << " units, " << tables.projectiles.size() << " projectiles, "
              << tables.constants.damage_to_armor.size() << " damage/armor rows, " << tables.unresolved.size()
              << " unresolved, " << tables.notes.size() << " notes, identity " << hex << std::endl;
    print_rows("unresolved", tables.unresolved);
    print_rows("note", tables.notes);
    // 13 projectiles: #561 adds the Y-wing squadron's ion override (AB-60).
    expect(tables.projectiles.size() == 13 && tables.constants.damage_to_armor.size() == 168, "projectile and damage rows");
    expect(tables.unresolved.empty(), "every pinned field and reference resolves");
    expect(tables.notes.size() == 6, "duplicate-tag notes");
    expect(hex == "984c71a99d5f6f58cef57bd3498754a5c2211bf7aec2b4f9ed7626273a0eb113",
           "pinned FoC content identity; update it only for an intended table or data change");

    // The Q24 root-to-bone composition agrees with a double-precision
    // composition of the same stored binary32 transforms.
    double worst = 0;
    std::size_t bones = 0;
    for (const auto& item : tables.units) {
        if (item.model_path.empty()) continue;
        const auto* model = access.model(item.model_path);
        if (model == nullptr) continue;
        const auto frames = eawr::units::bind_frames(*model);
        expect(static_cast<bool>(frames), "bind frames: " + item.model_path);
        if (!frames) continue;
        std::vector<std::array<double, 12>> reference(model->bones.size());
        for (std::size_t index = 0; index < model->bones.size(); ++index) {
            std::array<double, 12> local{};
            for (std::size_t element = 0; element < 12; ++element) local[element] = model->bones[index].relative_transform[element];
            const auto parent = model->bones[index].parent;
            if (parent < 0) {
                reference[index] = local;
                continue;
            }
            const auto& p = reference[static_cast<std::size_t>(parent)];
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t column = 0; column < 4; ++column) {
                    double value = column == 3 ? p[row * 4 + 3] : 0.0;
                    for (std::size_t k = 0; k < 3; ++k) value += p[row * 4 + k] * local[k * 4 + column];
                    reference[index][row * 4 + column] = value;
                }
            }
        }
        for (std::size_t index = 0; index < model->bones.size(); ++index) {
            for (std::size_t row = 0; row < 3; ++row) {
                const double fixed = static_cast<double>(frames.value()[index].rows[row][3].raw()) / Fixed::scale;
                worst = std::max(worst, std::abs(fixed - reference[index][row * 4 + 3]));
            }
            ++bones;
        }
    }
    std::cout << "FoC fleet: " << bones << " bone translations, worst Q24 vs double difference " << worst << std::endl;
    expect(bones > 0 && worst < 1.0e-4, "Q24 bone composition matches the double reference");
    foc_gunboat_pole(input);
}

} // namespace

int main() {
    synthetic_tables();
    priority_rules();
    content_identity();
    {
        eawr::units::UnitTables before;
        before.units.emplace_back();
        auto after = before;
        expect(after == before, "structural table equality of a copy");
        after.units.front().tech_level = 1;
        expect(after != before, "structural comparison includes AI tech level");
        expect(eawr::units::content_identity(after) == eawr::units::content_identity(before),
               "structural comparison leaves replay identity unchanged");
        after = before;
        after.units.front().spin_away_time = Fixed::from_raw(Fixed::scale);
        expect(after != before, "structural comparison includes spin-away fields");
        after = before;
        after.units.front().abilities.emplace_back();
        before.units.front().abilities.emplace_back();
        after.units.front().abilities.front().projectile_override = "another_projectile";
        expect(after != before, "structural comparison includes ability projectile override");
    }
    bind_frame_errors();
    collision_extents();
    foc_fleet();
    if (failures != 0) {
        std::cerr << failures << " unit table contract(s) failed\n";
        return 1;
    }
    std::cout << "unit table contracts passed\n";
    return 0;
}
