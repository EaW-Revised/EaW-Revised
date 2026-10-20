#include "unit_tables_support.hpp"
#include "unit_tables_synthetic_fixtures.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace unit_tables_test_support {
void print_rows(const std::string_view label, const std::vector<eawr::units::Unresolved>& rows) {
    for (const auto& row : rows) {
        std::cout << "  " << label << ' ' << row.owner << ' ' << row.field << " '" << row.value << "': " << row.reason << '\n';
    }
}

void mass_driver_type() {
    TempTree tree;
    write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
    std::string points{hardpoints_xml};
    const auto type = points.find("HARD_POINT_WEAPON_LASER");
    points.replace(type, std::string_view{"HARD_POINT_WEAPON_LASER"}.size(), "HARD_POINT_WEAPON_MASS_DRIVER");
    write(tree.root / "XML" / "eawr_hardpoints.xml", points);
    const auto loaded = load(tree.root, models(-40.0F));
    if (!loaded.tables) return;
    const auto& tables = *loaded.tables;
    const auto* unit = tables.find("Test_Frigate");
    expect(unit && !unit->hardpoints.empty()
            && unit->hardpoints[0].type == eawr::units::HardpointType::weapon_mass_driver
            && unit->hardpoints[0].weapon.has_value(), "MD-01: XML mass-driver type loads its weapon");
    expect(eawr::units::to_string(eawr::units::HardpointType::weapon_mass_driver) == "HARD_POINT_WEAPON_MASS_DRIVER",
        "MD-01: mass-driver type retains its XML name");
    auto combat = eawr::units::combat_table(tables);
    auto health = eawr::units::durability_table(tables);
    expect(combat && health, "MD-01: invented mass-driver carrier binds combat and durability");
    if (!combat || !health) return;
    const auto id = eawr::assets::object_type_crc("Test_Frigate");
    const auto* guns = combat.value().find(id);
    const auto* durability = health.value().find(id);
    expect(guns && unit && guns->hardpoint_meshes.size() == unit->hardpoints.size(),
        "WAD-20/21: mesh selector indices retain every authored hardpoint, including unresolved attachments");
    if (guns && unit && guns->hardpoint_meshes.size() == unit->hardpoints.size()) {
        for (std::size_t index = 0; index < unit->hardpoints.size(); ++index)
            expect(guns->hardpoint_meshes[index] == unit->hardpoints[index].collision_mesh,
                "WAD-20/21: authored mesh selectors survive the tactical profile conversion");
    }
    expect(guns && !guns->weapons.empty() && guns->weapons[0].hardpoint == 0 && guns->weapons[0].shot,
        "MD-01: invented mass-driver hardpoint becomes a firing slot");
    expect(durability && !durability->hardpoints.empty()
            && durability->hardpoints[0].role == eawr::sim::tactical::HardpointRole::weapon,
        "MD-01: invented mass-driver hardpoint has the weapon damage role");
}

void hero_company_tables() {
    for (const auto& [base_tag, variant_tag, expected] : {
        std::tuple{std::string{}, std::string{}, true},
        std::tuple{std::string{"<Is_Homogeneous>yes</Is_Homogeneous>"}, std::string{}, true},
        std::tuple{std::string{"<Is_Homogeneous>no</Is_Homogeneous>"}, std::string{}, false},
        std::tuple{std::string{"<Is_Homogeneous>no</Is_Homogeneous>"},
            std::string{"<Is_Homogeneous>yes</Is_Homogeneous>"}, true}}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string base = "<Squadron Name=\"Test_Squadron\">";
        const std::string variant = "<Squadron Name=\"Test_Squadron_Pair\">";
        xml.insert(xml.find(base) + base.size(), base_tag);
        xml.insert(xml.find(variant) + variant.size(), variant_tag);
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables && loaded.tables->find("Test_Squadron_Pair")->homogeneous == expected,
            "WHE-SQ-01: default, inherited and overriding Is_Homogeneous control card folding");
        if (loaded.tables) {
            auto changed = *loaded.tables;
            for (auto& type : changed.units) type.homogeneous = !type.homogeneous;
            expect(eawr::units::content_identity(changed) == eawr::units::content_identity(*loaded.tables),
                "WHE-SQ-01: card folding is presentation metadata and preserves replay content identity");
        }
    }
    for (const bool fighter_locomotor : {false, true}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        if (fighter_locomotor) {
            const auto locomotor = xml.find("SIMPLE_SPACE_LOCOMOTOR");
            xml.replace(locomotor, std::string_view{"SIMPLE_SPACE_LOCOMOTOR"}.size(), "FIGHTER_LOCOMOTOR");
        }
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        if (!loaded.tables) continue;
        const auto* type = loaded.tables->find("Test_Frigate");
        const auto motion = eawr::units::motion_table(*loaded.tables);
        const auto id = eawr::assets::object_type_crc("Test_Frigate");
        expect(type && type->kind == (fighter_locomotor ? UnitKind::craft : UnitKind::ship) && motion,
            "WHE-SQ-02: locomotor admission is independent of class and category names");
        if (motion) {
            expect((motion.value().find(id) == nullptr) == fighter_locomotor
                && (motion.value().squadrons.find_craft(id) != nullptr) == fighter_locomotor,
                "WHE-SQ-02: fighter locomotor gets fighter flight, simple locomotor gets ship motion");
            const auto* group = motion.value().squadrons.find_squadron(id);
            expect(!fighter_locomotor || (group && group->members == std::vector<eawr::sim::tactical::TypeId>{id}),
                "WHE-SQ-02: parentless fighter profile represents its own one-member flight group");
        }
    }
    for (const bool named_transport : {false, true}) for (const bool unique_first : {false, true}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto station = xml.find("<StarBase Name=\"Skirmish_Test_Base\">");
        xml.insert(station + std::string_view{"<StarBase Name=\"Skirmish_Test_Base\">"}.size(),
            "<Tactical_Buildable_Objects_Multiplayer>Rebel, Test_Hero_Company</Tactical_Buildable_Objects_Multiplayer>");
        if (named_transport) {
            const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
            xml.insert(ship + std::string_view{"<SpaceUnit Name=\"Test_Frigate\">"}.size(), "<Is_Named_Hero>yes</Is_Named_Hero>");
        }
        const std::string definitions =
            "<HeroCompany Name=\"Test_Hero_Company\"><Affiliation>Rebel</Affiliation>"
            "<Is_Named_Hero>yes</Is_Named_Hero><Company_Transport_Unit>Test_Frigate</Company_Transport_Unit>"
            "<Company_Units>First_Hero, Second_Hero</Company_Units></HeroCompany>"
            "<HeroUnit Name=\"First_Hero\"><Affiliation>Rebel</Affiliation><Is_Generic_Hero>yes</Is_Generic_Hero>"
            + std::string(unique_first ? "<Unique_Space_Unit>Test_Fighter</Unique_Space_Unit>" : "") +
            "</HeroUnit><HeroUnit Name=\"Second_Hero\"><Affiliation>Rebel</Affiliation><Is_Named_Hero>yes</Is_Named_Hero>"
            "<Unique_Space_Unit>Test_Fighter</Unique_Space_Unit></HeroUnit>";
        xml.insert(xml.rfind("</"), definitions);
        write_fixture(tree.root, xml);
        write(tree.root / "XML" / "FactionFiles.xml", "<Faction_Files><File>hero_factions.xml</File></Faction_Files>");
        write(tree.root / "XML" / "hero_factions.xml", "<Factions><Faction Name=\"Rebel\" /></Factions>");
        const auto loaded = load(tree.root, models(-40.0F));
        if (!loaded.tables) continue;
        const auto* company = loaded.tables->find("Test_Hero_Company");
        expect(company && company->company_members.size() == 2, "WHE-05: ordered company members load from production menus");
        if (!company) continue;
        expect(company->deployed_space_type == (!named_transport && unique_first ? "Test_Fighter" : "Test_Frigate")
            && company->creates_carried_heroes == !named_transport,
            "WHE-49: named transport bypasses riders; first qualifying hero alone selects unique ship or fallback");
        const auto* ship = loaded.tables->find("Test_Frigate");
        expect(ship && ship->named_hero == named_transport && !ship->generic_hero,
            "WHE-01/02: company flags do not propagate to ship");
        if (!named_transport) {
            const auto* first = loaded.tables->find("First_Hero");
            expect(first && first->generic_hero && !first->named_hero && !first->hull && first->abilities.empty(),
                "WHE-01/07: carried generic hero keeps its identity without ground combat data in space");
        }
        auto altered = *loaded.tables;
        for (auto& unit : altered.units) if (unit.id == "Test_Hero_Company") unit.creates_carried_heroes = !unit.creates_carried_heroes;
        expect(eawr::units::content_identity(altered) != eawr::units::content_identity(*loaded.tables),
            "WHE-49: deployment policy is bound to replay content identity");
    }
}

void nested_special_tables() {
    TempTree tree;
    auto xml = units_xml("3600", "Unit_Select_Test");
    const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
    xml.insert(ship + std::string_view{"<SpaceUnit Name=\"Test_Frigate\">"}.size(), R"(
      <Abilities SubObjectList="Yes">
        <Tractor_Beam_Attack_Ability Name="first-handler">
          <Activation_Style>User_Input</Activation_Style><Initially_Enabled>Yes</Initially_Enabled><Causes_Despawn>Yes</Causes_Despawn>
          <Applicable_Unit_Types>Test_Fighter, Test_Fighter</Applicable_Unit_Types>
          <Applicable_Unit_Categories>Frigate</Applicable_Unit_Categories>
          <Excluded_Unit_Types>Test_Fighter</Excluded_Unit_Types><Excluded_Unit_Categories>Fighter</Excluded_Unit_Categories>
        </Tractor_Beam_Attack_Ability>
        <Reduce_Production_Price_Ability Name="unclassified-default">
          <Applicable_Unit_Categories>Frigate</Applicable_Unit_Categories>
        </Reduce_Production_Price_Ability>
        <Enable_Radar_Ability Name="comm-radar">
          <Activation_Style>Skirmish_Automatic</Activation_Style>
          <Initially_Enabled>No</Initially_Enabled><Affects_All_Allies>True</Affects_All_Allies>
        </Enable_Radar_Ability>
      </Abilities>)");
    write_fixture(tree.root, xml);
    const auto loaded = load(tree.root, models(-40.0F));
    if (!loaded.tables) return;
    const auto* unit = loaded.tables->find("Test_Frigate");
    expect(unit && unit->special_abilities.size() == 3, "WHE-09/10: typed nested declarations load in authored order");
    if (!unit || unit->special_abilities.size() != 3) return;
    const auto& first = unit->special_abilities.front();
    expect(first.name == "first-handler" && first.service_interval == 1 && first.initially_enabled == true && first.causes_despawn,
        "WHE-09/10: authored gates remain separate from code-derived service intervals");
    expect(eawr::sim::tactical::special_type_matches(first.filter, eawr::assets::object_type_crc("Test_Fighter"), 0)
        && first.filter.applicable_types.size() == 1, "WHE-14: exact XML types are canonical and bypass authored exclusions");
    expect(unit->special_abilities[1].style == eawr::sim::tactical::SpecialActivationStyle::unspecified
        && unit->special_abilities[1].service_interval == 0 && !unit->special_abilities[1].initially_enabled,
        "U-05: missing activation style/default enablement are preserved, without inventing a space policy");
    const auto table = eawr::units::ability_table(*loaded.tables);
    const auto& radar = unit->special_abilities.back();
    expect(radar.kind == eawr::sim::tactical::SpecialAbilityKind::enable_radar
        && radar.style == eawr::sim::tactical::SpecialActivationStyle::skirmish_automatic
        && radar.initially_enabled == false && radar.affects_all_allies && radar.service_interval == 0,
        "WRAD-01/04/FC-75: named skirmish radar binds allied recipients, disabled authoring and no polling");
    const auto* profile = table ? table.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
    expect(profile && profile->special == unit->special_abilities, "WHE-10: nested profiles bind to session content");
    auto changed = *loaded.tables;
    for (auto& type : changed.units) if (type.id == unit->id) type.special_abilities[0].filter.excluded_categories ^= 1;
    expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
        "nested filter content binds to replay identity");
    changed = *loaded.tables;
    for (auto& type : changed.units) if (type.id == unit->id) type.special_abilities.back().affects_all_allies = false;
    expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
        "WRAD-01: allied radar authoring binds replay content identity");
}

void projectile_defence_tables() {
    for (const auto kind : {"MISSILE_SHIELD", "sensor_jamming"}) for (const bool radius_first : {false, true}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
        const auto begin = xml.find("<Unit_Abilities_Data", ship);
        const auto end = xml.find("</Unit_Abilities_Data>", begin);
        const auto type = "<Type>" + std::string(kind) + "</Type>";
        const std::string radius = "<Effective_Radius>750</Effective_Radius>";
        xml.replace(begin, end + std::string_view{"</Unit_Abilities_Data>"}.size() - begin,
            "<Unit_Abilities_Data SubObjectList=\"Yes\"><Unit_Ability>"
                + (radius_first ? radius + type : type + radius)
                + "<Expiration_Seconds>30</Expiration_Seconds><Recharge_Seconds>75</Recharge_Seconds>"
                + "</Unit_Ability></Unit_Abilities_Data>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "WPJ-17/WHE-32: defence radius XML loads in either child order");
        if (!loaded.tables) continue;
        const auto* unit = loaded.tables->find("Test_Frigate");
        expect(unit && unit->abilities.size() == 1 && unit->abilities[0].effective_radius == Fixed::from_raw(raw(750)),
            "WPJ-17/WHE-32: loader retains authored defence radius before or after Type");
        const auto table = eawr::units::ability_table(*loaded.tables);
        const auto* profile = table ? table.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
        expect(profile && profile->abilities.size() == 1
            && profile->abilities[0].kind == eawr::sim::tactical::ability_kind(kind)
            && profile->abilities[0].effective_radius == Fixed::from_raw(raw(750))
            && profile->abilities[0].expiration_frames == 900 && profile->abilities[0].recharge_frames == 2250,
            "WPJ-17/WHE-31/32: XML radius and timers bind to the production ability profile");
    }
}

void concentrate_fire_tables() {
    projectile_defence_tables();
    TempTree tree;
    auto xml = units_xml("3600", "Unit_Select_Test");
    const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
    const auto begin = xml.find("<Unit_Abilities_Data", ship);
    const auto end = xml.find("</Unit_Abilities_Data>", begin);
    xml.replace(begin, end + std::string_view{"</Unit_Abilities_Data>"}.size() - begin, R"(
      <Unit_Abilities_Data SubObjectList="Yes"><Unit_Ability>
        <Type>CONCENTRATE_FIRE</Type><Effective_Radius>6000</Effective_Radius>
        <Expiration_Seconds>15</Expiration_Seconds><Recharge_Seconds>20</Recharge_Seconds>
        <GUI_Activated_Ability_Name>focus</GUI_Activated_Ability_Name>
      </Unit_Ability></Unit_Abilities_Data>
      <Abilities SubObjectList="Yes"><Concentrate_Fire_Attack_Ability Name="focus">
        <Activation_Style>User_Input</Activation_Style><Applicable_Unit_Categories>Frigate</Applicable_Unit_Categories>
        <Applicable_Unit_Types/><Stacking_Category>0</Stacking_Category>
        <Target_Damage_Increase_Percent>0.5</Target_Damage_Increase_Percent>
        <Target_Speed_Decrease_Percent>0</Target_Speed_Decrease_Percent>
      </Concentrate_Fire_Attack_Ability></Abilities>)");
    write_fixture(tree.root, xml);
    const auto loaded = load(tree.root, models(-40.0F));
    expect(loaded.tables.has_value(), "WHE-24/25: concentrate-fire authored data loads");
    if (!loaded.tables) return;
    const auto table = eawr::units::ability_table(*loaded.tables);
    const auto* profile = table ? table.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
    expect(profile && profile->abilities.size() == 1 && profile->special.size() == 1,
        "named concentrate-fire declaration binds to its ordinary timer slot");
    if (!profile || profile->abilities.empty() || profile->special.empty()) return;
    expect(profile->abilities[0].effective_radius == Fixed::from_raw(raw(6000))
        && profile->abilities[0].expiration_frames == 450 && profile->abilities[0].recharge_frames == 600
        && profile->abilities[0].gui_activated_ability_name == "focus"
        && profile->special[0].target_damage_increase == Fixed::from_decimal("0.5").value()
        && profile->special[0].target_speed_decrease == Fixed{}, "radius, binding, timers and target modifiers are data-driven");
    auto changed = *loaded.tables;
    for (auto& unit : changed.units) if (unit.id == "Test_Frigate") unit.abilities[0].effective_radius = Fixed::from_raw(raw(3000));
    expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
        "replay content identity binds concentrate-fire radius");
}

void rocket_variant_inputs() {
    TempTree tree;
    auto xml = units_xml("3600", "Unit_Select_Test");
    const std::string category = "<Projectile_Category>Laser</Projectile_Category>";
    xml.replace(xml.find(category), category.size(), "<Projectile_Category>ROCKET</Projectile_Category>");
    const std::string base = "<Projectile Name=\"Proj_Test_Generic\">";
    xml.insert(xml.find(base) + base.size(),
        "<Projectile_Rocket_Curve_Distance>500</Projectile_Rocket_Curve_Distance>"
        "<Projectile_Rocket_Curve_Offset>0</Projectile_Rocket_Curve_Offset>"
        "<Projectile_Rocket_Straight_Distance>500</Projectile_Rocket_Straight_Distance>"
        "<Explode_When_Reached_Target_Radius>Yes</Explode_When_Reached_Target_Radius>");
    const std::string variant = "<Projectile Name=\"Proj_Test_Small\">";
    xml.insert(xml.find(variant) + variant.size(), "<Max_Speed>12</Max_Speed>"
        "<Projectile_Max_Flight_Distance>3000</Projectile_Max_Flight_Distance>");
    write_fixture(tree.root, xml);
    const auto loaded = load(tree.root, models(-40.0F));
    expect(loaded.tables.has_value(), "RFL: inherited rocket fixture loads");
    if (!loaded.tables) return;
    auto combat = eawr::units::combat_table(*loaded.tables);
    expect(static_cast<bool>(combat), "RFL: inherited rocket profile binds combat");
    if (!combat) return;
    const auto* craft = combat.value().find(eawr::assets::object_type_crc("Test_Fighter"));
    expect(craft && !craft->weapons.empty() && craft->weapons[0].shot,
        "RFL: inherited rocket supplies an ordinary weapon shot");
    if (!craft || craft->weapons.empty() || !craft->weapons[0].shot) return;
    const auto& shot = *craft->weapons[0].shot;
    expect(!shot.homing && shot.speed == Fixed::from_raw(raw(12)) && shot.flight
        && shot.flight->kind == eawr::sim::tactical::FlightKind::rocket
        && shot.flight->authored_distance == Fixed::from_raw(raw(3000))
        && shot.flight->curve_distance == Fixed::from_raw(raw(500))
        && shot.flight->straight_distance == Fixed::from_raw(raw(500))
        && shot.flight->curve_offset == Fixed{} && shot.flight->target_radius,
        "WAD-04/38/RFL-01..05: variant changes speed/flight while retaining rocket construction, never homing");
    expect(shot.blast.damage == Fixed::from_raw(raw(12)) && shot.blast.radius == Fixed::from_raw(raw(80))
        && shot.blast.tiers == 3, "WAD-38: variant retains independent blast inputs");
    const auto identity = eawr::units::content_identity(*loaded.tables);
    auto changed = *loaded.tables;
    for (auto& projectile : changed.projectiles) {
        if (projectile.id == "Proj_Test_Small") projectile.rocket_straight_distance = Fixed::from_raw(raw(400));
    }
    expect(identity != eawr::units::content_identity(changed), "RFL-02: authored rocket geometry changes content identity");

    // RFL-03: synthetic inputs match the projectile reached by UM06_EclipseDefensePad.
    TempTree authored_tree;
    auto authored = xml;
    const auto replace = [&](const std::string& before, const std::string& after) {
        authored.replace(authored.find(before), before.size(), after);
    };
    replace("<Projectile_Rocket_Curve_Distance>500", "<Projectile_Rocket_Curve_Distance>400");
    replace("<Projectile_Rocket_Curve_Offset>0", "<Projectile_Rocket_Curve_Offset>180");
    replace("<Projectile_Rocket_Straight_Distance>500", "<Projectile_Rocket_Straight_Distance>120");
    replace("<Max_Speed>12", "<Max_Speed>20");
    write_fixture(authored_tree.root, authored);
    const auto authored_loaded = load(authored_tree.root, models(-40.0F));
    expect(authored_loaded.tables.has_value(), "RFL-03: authored nonzero rocket inputs load from synthetic XML");
    if (!authored_loaded.tables) return;
    const auto accepted = eawr::units::combat_table(*authored_loaded.tables);
    expect(static_cast<bool>(accepted), "RFL-03: UM06_EclipseDefensePad's reachable rocket profile now passes combat validation");
    if (!accepted) return;
    const auto* accepted_craft = accepted.value().find(eawr::assets::object_type_crc("Test_Fighter"));
    expect(accepted_craft && !accepted_craft->weapons.empty() && accepted_craft->weapons[0].shot
        && accepted_craft->weapons[0].shot->flight && accepted_craft->weapons[0].shot->speed == Fixed::from_raw(raw(20))
        && accepted_craft->weapons[0].shot->flight->curve_offset == Fixed::from_raw(raw(180))
        && accepted_craft->weapons[0].shot->flight->curve_distance == Fixed::from_raw(raw(400))
        && accepted_craft->weapons[0].shot->flight->straight_distance == Fixed::from_raw(raw(120))
        && accepted_craft->weapons[0].shot->flight->authored_distance == Fixed::from_raw(raw(3000)),
        "RFL-02/03/05: accepted loader profile retains all authored route values");
}

void manual_weapon_loading() {
    TempTree tree;
    write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
    std::string points{hardpoints_xml};
    points.insert(points.find("</HardPoint>"),
        "<Requires_Manual_Target_Assignment>Yes</Requires_Manual_Target_Assignment>"
        "<Manual_Hardpoint_Firing_Cooldown_Secs>120</Manual_Hardpoint_Firing_Cooldown_Secs>"
        "<Projectile_Appearance_Delay_Frames>15</Projectile_Appearance_Delay_Frames>");
    write(tree.root / "XML" / "eawr_hardpoints.xml", points);
    auto loaded = load(tree.root, models(-40.0F));
    expect(loaded.tables.has_value(), "MC-08: synthetic manual hardpoint loads");
    if (!loaded.tables) return;
    auto combat = eawr::units::combat_table(*loaded.tables);
    expect(static_cast<bool>(combat), "MC-08: manual data binds ordinary weapon and player clock independently");
    if (!combat) return;
    const auto* guns = combat.value().find(eawr::assets::object_type_crc("Test_Frigate"));
    const auto slot = std::find_if(guns->weapons.begin(), guns->weapons.end(), [](const auto& weapon) {
        return weapon.hardpoint == 0;
    });
    expect(slot != guns->weapons.end() && slot->requires_manual_target && slot->manual_cooldown_frames == 3600
        && slot->manual_min_range == Fixed{} && slot->shot
        && slot->shot->appearance_delay_frames == 15,
        "MC-08: 120-second player cooldown and 15-frame appearance delay retain authored values; stock minimum defaults zero");
    const auto identity = eawr::units::content_identity(*loaded.tables);
    auto changed = *loaded.tables;
    auto* unit = const_cast<eawr::units::UnitType*>(changed.find("Test_Frigate"));
    unit->hardpoints.front().manual_cooldown_seconds = Fixed::from_raw(raw(121));
    expect(identity != eawr::units::content_identity(changed), "MC-08: manual cooldown enters conditional content identity");
    changed = *loaded.tables;
    unit = const_cast<eawr::units::UnitType*>(changed.find("Test_Frigate"));
    unit->hardpoints.front().weapon->appearance_delay_frames = 30;
    expect(identity != eawr::units::content_identity(changed), "MC-08: appearance delay enters weapon identity independently");
    unit->hardpoints.front().weapon->appearance_delay_frames = 0;
    const auto zero_identity = eawr::units::content_identity(changed);
    unit->hardpoints.front().weapon->appearance_delay_frames.reset();
    expect(zero_identity == eawr::units::content_identity(changed), "MC-08: zero and absent appearance delay encode identically");
}

void special_weapon_classification() {
    for (const bool projectile : {false, true}) for (const bool manual : {false, true}) {
        TempTree tree;
        write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
        std::string points{hardpoints_xml};
        const std::string original = "HARD_POINT_WEAPON_LASER";
        points.replace(points.find(original), original.size(), "HARD_POINT_WEAPON_SPECIAL");
        if (!projectile) {
            for (const std::string_view tag : {"Fire_Projectile_Type", "Fire_Min_Recharge_Seconds",
                "Fire_Max_Recharge_Seconds", "Fire_Pulse_Count", "Fire_Range_Distance", "Fire_Cone_Width", "Fire_Cone_Height"}) {
                const std::string open = "<" + std::string(tag) + ">";
                const std::string close = "</" + std::string(tag) + ">";
                const auto begin = points.find(open); const auto end = points.find(close, begin);
                if (begin != std::string::npos && end != std::string::npos) points.erase(begin, end + close.size() - begin);
            }
        }
        if (manual) points.insert(points.find("</HardPoint>"),
            "<Requires_Manual_Target_Assignment>Yes</Requires_Manual_Target_Assignment>");
        write(tree.root / "XML" / "eawr_hardpoints.xml", points);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "WAD-31: SPECIAL with or without projectile loads");
        if (!loaded.tables) continue;
        auto changed = *loaded.tables;
        auto* changed_unit = const_cast<eawr::units::UnitType*>(changed.find("Test_Frigate"));
        if (changed_unit && !changed_unit->hardpoints.empty()) {
            changed_unit->hardpoints[0].requires_manual_target = !manual;
            expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
                "WAD-39: the manual fire guard changes the content identity");
        }
        const auto* unit = loaded.tables->find("Test_Frigate");
        expect(unit && !unit->hardpoints.empty() && unit->hardpoints[0].type == eawr::units::HardpointType::weapon_special
            && unit->hardpoints[0].weapon.has_value() == projectile
            && unit->hardpoints[0].requires_manual_target == manual,
            "WAD-31/39: weapon classification is distinct from activation; projectile-free attachment invents no weapon");
        auto combat = eawr::units::combat_table(*loaded.tables);
        auto health = eawr::units::durability_table(*loaded.tables);
        expect(combat && health, "WAD-31/32: SPECIAL binds ordinary combat and durability");
        if (!combat || !health) continue;
        const auto id = eawr::assets::object_type_crc("Test_Frigate");
        const auto* guns = combat.value().find(id); const auto* durability = health.value().find(id);
        expect(durability && !durability->hardpoints.empty() && durability->hardpoints[0].role
            == eawr::sim::tactical::HardpointRole::weapon, "WAD-31: SPECIAL has ordinary weapon durability role");
        if (!guns) { expect(false, "WAD-32: SPECIAL carrier profile retained"); continue; }
        const auto slot = std::find_if(guns->weapons.begin(), guns->weapons.end(), [](const auto& weapon) {
            return weapon.hardpoint == 0;
        });
        if (!projectile) {
            expect(slot == guns->weapons.end(), "WAD-31: projectile-free leech adds no firing slot or default projectile");
        } else {
            expect(slot != guns->weapons.end() && slot->special && slot->requires_manual_target == manual && slot->shot
                && slot->pulse_count == 5 && slot->pulse_delay_frames == 6 && slot->range == Fixed::from_raw(raw(1100))
                && slot->cone_width == Fixed::from_raw(raw(175)) && slot->cone_height == Fixed::from_raw(raw(160))
                && slot->shot->damage == Fixed::from_raw(raw(40)), "WAD-32/37: SPECIAL uses authored ordinary weapon values");
        }
    }
    expect(eawr::units::to_string(eawr::units::HardpointType::weapon_special) == "HARD_POINT_WEAPON_SPECIAL"
        && eawr::units::HardpointType::weapon_special != eawr::units::HardpointType::enable_special_ability,
        "WAD-31: SPECIAL and ENABLE_SPECIAL_ABILITY remain distinct enums");
}

void cluster_bomb_inputs() {
    TempTree tree;
    auto xml = units_xml("3600", "Unit_Select_Test");
    const auto begin = xml.find("<Unit_Abilities_Data");
    const auto end = xml.find("</Unit_Abilities_Data>", begin) + std::string_view{"</Unit_Abilities_Data>"}.size();
    xml.replace(begin, end - begin, R"xml(<Unit_Abilities_Data><Unit_Ability>
        <Type>CLUSTER_BOMB</Type><Recharge_Seconds>50</Recharge_Seconds><Supports_Autofire>True</Supports_Autofire>
        <GUI_Activated_Ability_Name>Test_Cluster</GUI_Activated_Ability_Name><Effective_Radius>999</Effective_Radius>
        </Unit_Ability></Unit_Abilities_Data><Abilities><Cluster_Bomb_Ability Name="Test_Cluster">
        <Activation_Style>User_Input</Activation_Style><Bomb_Type>Proj_Test_Small</Bomb_Type>
        <Number_Of_Bombs>12</Number_Of_Bombs><Detonation_Time_In_Secs>1.5</Detonation_Time_In_Secs>
        </Cluster_Bomb_Ability></Abilities>)xml");
    write_fixture(tree.root, xml);
    const auto loaded = load(tree.root, models(-40.0F));
    expect(loaded.tables.has_value(), "WCB-01: nested ring data loads");
    if (!loaded.tables) return;
    const auto built = eawr::units::ability_table(*loaded.tables, {});
    expect(static_cast<bool>(built), "WCB-01: named ring handler binds");
    if (!built) return;
    const auto* profile = built.value().find(eawr::assets::object_type_crc("Test_Frigate"));
    const auto slot = profile ? eawr::sim::tactical::ability_slot(*profile, eawr::sim::tactical::AbilityKind::cluster_bomb) : std::nullopt;
    expect(slot.has_value(), "WCB-01: cluster slot is exposed");
    if (slot) {
        const auto& ability = profile->abilities[*slot];
        expect(ability.spawned && ability.spawned->ring_count == 12 && ability.spawned->countdown_frames == 45
            && ability.recharge_frames == 1500 && ability.supports_autofire && ability.spawned->reach.raw() == 0,
            "WCB-01..03: ring uses nested count/countdown, never the unit ability radius");
    }
    auto changed = *loaded.tables;
    const auto unit = std::find_if(changed.units.begin(), changed.units.end(), [](const auto& type) { return type.cluster_bomb.has_value(); });
    if (unit == changed.units.end()) return;
    unit->cluster_bomb->count = 8;
    expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
        "WCB-01: authored ring count participates in content identity");
}

void barrage_inputs() {
    TempTree tree;
    auto xml = units_xml("3600", "Unit_Select_Test");
    const auto begin = xml.find("<Unit_Abilities_Data");
    const auto end = xml.find("</Unit_Abilities_Data>", begin) + std::string_view{"</Unit_Abilities_Data>"}.size();
    xml.replace(begin, end - begin, R"xml(<Unit_Abilities_Data><Unit_Ability>
        <Type>BARRAGE</Type><Expiration_Seconds>10</Expiration_Seconds><Recharge_Seconds>40</Recharge_Seconds>
        <Mod_Multiplier>FIRE_RATE_MULTIPLIER, 3f</Mod_Multiplier>
        <Projectile_Types_Override>Proj_Test_Small</Projectile_Types_Override>
        <Targeting_Fire_Inaccuracy_Fixed_Radius_Override>320</Targeting_Fire_Inaccuracy_Fixed_Radius_Override>
        <Target_Position_Z_Offset>-150</Target_Position_Z_Offset>
        </Unit_Ability></Unit_Abilities_Data>)xml");
    xml.insert(xml.rfind("</"), R"xml(<MiscObject Name="Dummy_Barrage_Target">
        <Model_Name>test_fighter.alo</Model_Name><SpaceBehavior>MARKER</SpaceBehavior>
        <Scale_Factor>100</Scale_Factor><CategoryMask>Corvette</CategoryMask>
        <Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living>
        <Influences_Capture_Point>No</Influences_Capture_Point><Immune_To_Damage>True</Immune_To_Damage>
        </MiscObject>)xml");
    write_fixture(tree.root, xml);
    const auto loaded = load(tree.root, models(-40.0F));
    expect(loaded.tables.has_value(), "WAD-38: barrage and marker data load without ordinary hull/locomotor requirements");
    if (!loaded.tables) return;
    auto ability = eawr::units::ability_table(*loaded.tables, {});
    auto combat = eawr::units::combat_table(*loaded.tables);
    auto health = eawr::units::durability_table(*loaded.tables);
    expect(ability && combat && health, "WAD-38: barrage binds existing ability/combat/durability tables");
    if (!ability || !combat || !health) return;
    const auto* profile = ability.value().find(eawr::assets::object_type_crc("Test_Frigate"));
    expect(profile && profile->abilities.size() == 1, "WAD-38: barrage is an existing ability slot");
    if (profile && !profile->abilities.empty()) {
        const auto& barrage = profile->abilities[0];
        expect(barrage.kind == eawr::sim::tactical::AbilityKind::barrage && barrage.expiration_frames == 300
            && barrage.recharge_frames == 1200 && barrage.modifiers.fire_rate == Fixed::from_raw(raw(3))
            && barrage.fixed_inaccuracy == Fixed::from_raw(raw(320)) && barrage.target_z_offset == Fixed::from_raw(raw(-150)),
            "WAD-38: duration, recharge, rate, accuracy and height are retained independently");
    }
    const auto* guns = combat.value().find(eawr::assets::object_type_crc("Test_Frigate"));
    expect(guns && !guns->weapons.empty() && guns->weapons[0].barrage_shot
        && guns->weapons[0].shot && guns->weapons[0].shot->speed != guns->weapons[0].barrage_shot->speed,
        "WAD-38: override has its own shot while normal weapon clocks are retained");
    const auto marker_id = eawr::assets::object_type_crc("Dummy_Barrage_Target");
    const auto* marker = combat.value().find(marker_id);
    expect(marker && marker->category_bits == 8 && marker->living_projectile_collision
        && !health.value().find(marker_id), "WAD-38: enemy marker retains category/collision gate and no damageable hull");
    auto changed = *loaded.tables;
    for (auto& unit : changed.units) for (auto& entry : unit.abilities) {
        if (entry.type == "BARRAGE") entry.fixed_inaccuracy = Fixed::from_raw(raw(321));
    }
    expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
        "WAD-38: override-only content changes participate in identity");
}


void object_script_inheritance() {
    for (const bool blank : {false, true}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto add = [&](std::string_view opening, std::string_view tags) {
            const auto position = xml.find(opening);
            expect(position != std::string::npos, "SOS-01: fixture object exists");
            if (position != std::string::npos) xml.insert(position + opening.size(), tags);
        };
        add("<StarBase Name=\"Test_Base\">", "<Lua_Script>ObjectScript_MillenniumFalcon</Lua_Script>");
        add("<Container Name=\"Test_Pair_Base_Container\">", "<Lua_Script>ObjectScript_VaderTIESquadron</Lua_Script>");
        // ML-24/SOS-01: a blank or TBD Lua_Script on a variant changes nothing; the parent's script stays.
        if (blank) {
            add("<StarBase Name=\"Skirmish_Test_Base\">", "<Lua_Script/>");
            add("<Container Name=\"Test_Pair_Container\">", "<Lua_Script></Lua_Script>");
        }
        // The purchase type's script cannot override the actual created team.
        add("<Squadron Name=\"Test_Squadron_Pair\">", "<Lua_Script>PurchaseOnly</Lua_Script>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "SOS-01: inherited object-script tables load");
        if (!loaded.tables) continue;
        const auto* ship = loaded.tables->find("Skirmish_Test_Base");
        const auto* squadron = loaded.tables->find("Test_Squadron_Pair");
        expect(ship && ship->lua_script == "ObjectScript_MillenniumFalcon",
            "SOS-01: object attachment resolves variant; a blank override keeps the inherited script");
        expect(squadron && squadron->lua_script == "ObjectScript_VaderTIESquadron",
            "SOS-01/19: runtime team attachment resolves inherited container; a blank override keeps it");
    }
    // An unresolved team is reported and attaches nothing; the purchase script is not substituted.
    TempTree tree;
    auto xml = units_xml("3600", "Unit_Select_Test");
    const std::string team = "<Create_Team_Type> Test_Pair_Container </Create_Team_Type>";
    const auto position = xml.find(team);
    expect(position != std::string::npos, "SOS-01: fixture team reference exists");
    if (position == std::string::npos) return;
    xml.replace(position, team.size(), "<Create_Team_Type>Missing_Test_Team</Create_Team_Type><Lua_Script>PurchaseOnly</Lua_Script>");
    write_fixture(tree.root, xml);
    const auto loaded = load(tree.root, models(-40.0F));
    expect(loaded.tables.has_value(), "SOS-01: unresolved team still loads");
    if (!loaded.tables) return;
    const auto* squadron = loaded.tables->find("Test_Squadron_Pair");
    expect(squadron && squadron->lua_script.empty(), "SOS-01: unresolved team attaches no script");
    expect(std::any_of(loaded.tables->unresolved.begin(), loaded.tables->unresolved.end(), [](const auto& row) {
        return row.owner == "Test_Squadron_Pair" && row.field == "Create_Team_Type";
    }), "SOS-01: unresolved team is reported");
}


} // namespace unit_tables_test_support
