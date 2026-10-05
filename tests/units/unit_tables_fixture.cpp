#include "unit_tables_support.hpp"

namespace unit_tables_test_support {

TempTree::TempTree() { std::filesystem::create_directories(root); }

TempTree::~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }


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
  <Is_Community_Property>Yes</Is_Community_Property>
  <Abilities SubObjectList="Yes">
    <Income_Stream_Ability Name="Test_Income"><Base_Income_Value>30</Base_Income_Value><Base_Interval_In_Secs>10</Base_Interval_In_Secs></Income_Stream_Ability>
  </Abilities>
</StarBase>
<SpaceUnit Name="Test_Frigate">
  <Collidable_By_Projectile_Living>yes</Collidable_By_Projectile_Living>
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
    <SpaceBehavior>TARGETING,REVEAL , HIDE_WHEN_FOGGED, SIMPLE_SPACE_LOCOMOTOR</SpaceBehavior>
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
  <Collidable_By_Projectile_Living>yes</Collidable_By_Projectile_Living>
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
  <Projectile_Blast_Area_Damage>9</Projectile_Blast_Area_Damage>
  <Projectile_Blast_Area_Range>80</Projectile_Blast_Area_Range>
  <Projectile_Blast_Area_Dropoff>Yes</Projectile_Blast_Area_Dropoff>
  <Projectile_Blast_Area_Dropoff_Tiers>3</Projectile_Blast_Area_Dropoff_Tiers>
  <Projectile_Blast_Area_Max_Victims>0</Projectile_Blast_Area_Max_Victims>
  <Max_Secs_For_AE_Delayed_Damage>0</Max_Secs_For_AE_Delayed_Damage>
  <Max_Speed>11.0</Max_Speed>
  <Projectile_Category>Laser</Projectile_Category>
  <Projectile_Max_Flight_Distance>500.0</Projectile_Max_Flight_Distance>
  <Projectile_Does_Shield_Damage>Yes</Projectile_Does_Shield_Damage>
  <Projectile_Does_Hitpoint_Damage>Yes</Projectile_Does_Hitpoint_Damage>
</Projectile>
<Projectile Name="Proj_Test_Small">
  <Variant_Of_Existing_Type>Proj_Test_Generic</Variant_Of_Existing_Type>
  <Projectile_Damage>5.0</Projectile_Damage>
  <Projectile_Blast_Area_Damage>12</Projectile_Blast_Area_Damage>
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
<Space_Reinforcement_Collision_Check_Distance>200</Space_Reinforcement_Collision_Check_Distance>
  <ShieldRechargeIntervalInSecs>3.0</ShieldRechargeIntervalInSecs>
  <EnergyRechargeIntervalInSecs>5.0</EnergyRechargeIntervalInSecs>
  <EnergyToShieldExchangeRate>5.0</EnergyToShieldExchangeRate>
  <Depleted_Shield_Damage_Increment>0.0</Depleted_Shield_Damage_Increment>
  <Depleted_Shield_Disable_Time>5.0</Depleted_Shield_Disable_Time>
  <Depleted_Shield_Regen_Cap>0.25</Depleted_Shield_Regen_Cap>
  <Diminishing_Firepower>0, 0.6, 0.3, 0.7, 0.9, 0.9, 1, 1, 2, 1</Diminishing_Firepower>
  <Asteroid_Field_Damage>20</Asteroid_Field_Damage>
  <Asteroid_Field_Damage_Rate>0.20</Asteroid_Field_Damage_Rate>
  <Nebula_Ability_Disable_Time>5</Nebula_Ability_Disable_Time>
  <Ion_Storm_Shield_Disable_Time>5</Ion_Storm_Shield_Disable_Time>
  <Hull_Vs_Hard_Points_Health_Constraint> 0.2 </Hull_Vs_Hard_Points_Health_Constraint>
  <Hardpoint_Recharge_Cutoff_For_Opportunity_Fire> 3.0 </Hardpoint_Recharge_Cutoff_For_Opportunity_Fire>
  <Engines_Disabled_Speed_Modifier> 0.4 </Engines_Disabled_Speed_Modifier>
  <Space_Elevated_Vulnerability_Duration>5.0</Space_Elevated_Vulnerability_Duration>
  <Space_Elevated_Vulnerability_Factor>-3.0</Space_Elevated_Vulnerability_Factor>
  <MP_Default_Credits>6000</MP_Default_Credits>
  <Tactical_Build_Time_Multiplier>1.0</Tactical_Build_Time_Multiplier>
  <Allow_Reinforcement_Percentage_Normalized>0</Allow_Reinforcement_Percentage_Normalized>
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
                        const bool rotate_quarter_turn) {
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
             const std::string_view field, const std::string_view value) {
    for (const auto& row : rows) {
        if (row.owner == owner && row.field == field && (value.empty() || row.value == value)) return true;
    }
    return false;
}

} // namespace unit_tables_test_support
