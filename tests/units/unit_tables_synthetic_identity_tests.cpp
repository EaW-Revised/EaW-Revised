#include "unit_tables_support.hpp"
#include "eawr/skirmish/start.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace unit_tables_test_support {
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
    // Duplicate names are a known catalog seam (legacy #3741), not a retail overlay rule.
    // Pin today's deterministic winner position until separate indexed entries are supported.
    {
        TempTree tree;
        write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
        write(tree.root / "XML" / "GameObjectFiles.xml",
            "<Game_Object_Files><File>eawr_units.xml</File><File>later.xml</File></Game_Object_Files>");
        write(tree.root / "XML" / "later.xml",
            "<Objects><SpaceUnit Name=\"Test_Frigate\"><Tactical_Health>7200</Tactical_Health></SpaceUnit></Objects>");
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "PL-20: later-file duplicate fixture loads");
        if (loaded.tables) {
            const auto* duplicate = loaded.tables->find("Test_Frigate");
            // The squadron follows the original frigate, but precedes its later-file duplicate.
            const auto* earlier = loaded.tables->find("Test_Squadron");
            expect(duplicate && earlier && duplicate->table_order > earlier->table_order,
                "PL-20: duplicate names retain the later catalog winner's position pending the retail seam");
            expect(std::count_if(loaded.tables->units.begin(), loaded.tables->units.end(),
                [](const auto& unit) { return unit.id == "Test_Frigate"; }) == 1,
                "PL-20: duplicate fixture exposes the existing single-winner catalog seam");
        }
    }
    TempTree tree;
    write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
    const auto first = load(tree.root, models(-40.0F));
    const auto second = load(tree.root, models(-40.0F));
    if (!first.tables || !second.tables) return;
    const auto identity = eawr::units::content_identity(*first.tables);
    expect(identity == eawr::units::content_identity(*second.tables), "identity is stable across loads");
    // PL-20: table_order is the object's place in the authored files, whatever its name hashes to.
    {
        const auto xml = units_xml("3600", "Unit_Select_Test");
        std::vector<std::pair<std::size_t, std::uint64_t>> placed;
        for (const auto& unit : first.tables->units) {
            const auto at = xml.find("Name=\"" + unit.id + "\"");
            if (at != std::string::npos) placed.emplace_back(at, unit.table_order);
        }
        std::sort(placed.begin(), placed.end());
        expect(placed.size() > 2, "PL-20: the fixture's types have document positions");
        expect(std::is_sorted(placed.begin(), placed.end(), [](const auto& a, const auto& b) { return a.second < b.second; })
                && std::adjacent_find(placed.begin(), placed.end(), [](const auto& a, const auto& b) { return a.second == b.second; }) == placed.end(),
            "PL-20: table_order follows the authored document order");
    }

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

    auto modifier_xml = units_xml("3600", "Unit_Select_Test");
    const std::string fighter_tag = "<SpaceUnit Name=\"Test_Fighter\">";
    const auto fighter_offset = modifier_xml.find(fighter_tag);
    expect(fighter_offset != std::string::npos, "income modifier fixture finds a loaded space type");
    if (fighter_offset == std::string::npos) return;
    const std::string modifier_tags = R"(
      <Previous_Upgrade_Level_Type>Test_Base</Previous_Upgrade_Level_Type>
      <Next_Upgrade_Level_Type> Test_Next_Upgrade </Next_Upgrade_Level_Type>
      <Abilities><Income_Stream_Mod_Ability Name="Fixture_Income_Mod">
        <Target_Stream_Source>Skirmish_Test_Base</Target_Stream_Source>
        <Activation_Style>Space_Automatic</Activation_Style>
        <Affects_All_Allied_Sources>Yes</Affects_All_Allied_Sources>
        <Stacking_Category>7</Stacking_Category><Income_Multiplier>1.2</Income_Multiplier>
        <Income_Additive_Value>3</Income_Additive_Value><Interval_Multiplier>0.5</Interval_Multiplier>
        <Reverse_Application_Logic>Yes</Reverse_Application_Logic>
      </Income_Stream_Mod_Ability></Abilities>)";
    modifier_xml.insert(fighter_offset + fighter_tag.size(), modifier_tags);
    write_fixture(tree.root, modifier_xml);
    const auto modifier_loaded = load(tree.root, models(-40.0F));
    expect(modifier_loaded.tables.has_value(), "WBP-25 modifier fields parse through validated unit loading");
    if (!modifier_loaded.tables) return;
    const auto* fighter = modifier_loaded.tables->find("Test_Fighter");
    expect(fighter && fighter->production.income_bonuses.size() == 1, "one authored modifier loads");
    if (!fighter || fighter->production.income_bonuses.size() != 1) return;
    const auto& bonus = fighter->production.income_bonuses.front();
    expect(bonus.name == "Fixture_Income_Mod" && bonus.target_source == "Skirmish_Test_Base"
        && bonus.activation_style == "Space_Automatic" && bonus.all_allied_sources && bonus.reverse
        && bonus.stacking_category == 7 && bonus.multiplier == Fixed::from_decimal("1.2").value()
        && bonus.additive == Fixed::from_integer(3).value()
        && bonus.interval_multiplier == Fixed::from_decimal("0.5").value(), "WBP-25 all modifier fields retain their authored values");
    expect(fighter->production.removes_previous == "Test_Base", "WBP-24 previous upgrade destruction defaults to true");
    expect(fighter->production.next_upgrade == "Test_Next_Upgrade", "WPR-63: authored menu successor is trimmed");
    auto menu_only = *modifier_loaded.tables;
    menu_only.units[static_cast<std::size_t>(fighter - modifier_loaded.tables->units.data())].production.next_upgrade.clear();
    expect(eawr::units::content_identity(menu_only) == eawr::units::content_identity(*modifier_loaded.tables),
        "WPR-63: menu successor metadata is outside replay content identity");
    const auto modifier_identity = eawr::units::content_identity(*modifier_loaded.tables);
    const auto fighter_index = static_cast<std::size_t>(fighter - modifier_loaded.tables->units.data());
    for (int field = 0; field < 5; ++field) {
        auto variant = *modifier_loaded.tables;
        auto& changed_bonus = variant.units[fighter_index].production.income_bonuses.front();
        switch (field) {
        case 0: changed_bonus.interval_multiplier = Fixed::from_integer(2).value(); break;
        case 1: changed_bonus.activation_style = "Ground_Automatic"; break;
        case 2: changed_bonus.stacking_category = 8; break;
        case 3: changed_bonus.all_allied_sources = false; break;
        default: changed_bonus.reverse = false; break;
        }
        expect(eawr::units::content_identity(variant) != modifier_identity, "each new runtime modifier field binds content identity");
    }
    modifier_xml.insert(fighter_offset + fighter_tag.size(), "<Destroy_Previous_Upgrade_Level>No</Destroy_Previous_Upgrade_Level>");
    write_fixture(tree.root, modifier_xml);
    const auto retained = load(tree.root, models(-40.0F));
    expect(retained.tables && retained.tables->find("Test_Fighter")->production.removes_previous.empty(),
        "WBP-24 explicit false preserves the previous upgrade level");
    auto command_xml = units_xml("3600", "Unit_Select_Test");
    command_xml.insert(command_xml.find(fighter_tag) + fighter_tag.size(), R"(
      <Unique_Space_Unit>Test_Frigate</Unique_Space_Unit>
      <Unique_Ground_Unit>Test_Fighter</Unique_Ground_Unit>
      <Abilities><Combat_Bonus_Ability Name="Fixture_Command">
        <Activation_Style>Space_Automatic</Activation_Style>
        <Specific_Faction>Empire</Specific_Faction><Stacking_Category>3</Stacking_Category>
        <Applicable_Unit_Types>Test_Frigate</Applicable_Unit_Types>
        <Excluded_Unit_Types>Test_Fighter</Excluded_Unit_Types>
        <Health_Bonus_Percentage>-0.25</Health_Bonus_Percentage>
      </Combat_Bonus_Ability></Abilities>)");
    write_fixture(tree.root, command_xml);
    const auto command = load(tree.root, models(-40.0F));
    expect(command.tables.has_value(), "WHE-53 command source fields load");
    if (!command.tables) return;
    const auto* source = command.tables->find("Test_Fighter");
    expect(source && source->production.combat_bonuses.size() == 1, "one passive command bonus loads");
    if (!source || source->production.combat_bonuses.size() != 1) return;
    const auto& passive = source->production.combat_bonuses.front();
    expect(passive.enabled && passive.specific_faction == "Empire" && passive.stacking_category == 3
        && passive.percentages[0] == Fixed::from_decimal("-0.25").value()
        && passive.excluded_containers == std::vector<eawr::sim::tactical::TypeId>{eawr::assets::object_type_crc("Test_Frigate"),
            eawr::assets::object_type_crc("Test_Fighter")}
        && passive.filter.excluded_types == std::vector<eawr::sim::tactical::TypeId>{eawr::assets::object_type_crc("Test_Fighter")},
        "WHE-13/14/55 faction, default enablement, container and signed percentage retain authored values");
    const auto command_identity = eawr::units::content_identity(*command.tables);
    const auto source_index = static_cast<std::size_t>(source - command.tables->units.data());
    for (unsigned field = 0; field < 4; ++field) {
        auto variant = *command.tables;
        auto& value = variant.units[source_index].production.combat_bonuses.front();
        if (field == 0) value.specific_faction = "Rebel";
        else if (field == 1) value.enabled = false;
        else if (field == 2) value.excluded_containers.clear();
        else value.filter.excluded_types.clear();
        expect(eawr::units::content_identity(variant) != command_identity, "command eligibility binds content identity");
    }
    eawr::skirmish::SkirmishStart command_start;
    command_start.setup.players = {{1, 0, 1, 1}};
    const auto compiled = eawr::skirmish::economy_rules(command_start, {}, *command.tables);
    expect(compiled && compiled.value().command_bonuses.size() == 1
        && compiled.value().command_bonuses.front().bonus.applicable.empty()
        && compiled.value().command_bonuses.front().specific_faction == eawr::assets::object_type_crc("Empire"),
        "WHE-13: unique container exclusion wins even over an explicitly allowed type");
    auto allowed_tables = *command.tables;
    auto& allowed = allowed_tables.units[source_index].production.combat_bonuses.front();
    allowed.excluded_containers.clear();
    allowed.filter.excluded_types.push_back(eawr::assets::object_type_crc("Test_Frigate"));
    std::sort(allowed.filter.excluded_types.begin(), allowed.filter.excluded_types.end());
    const auto exact = eawr::skirmish::economy_rules(command_start, {}, allowed_tables);
    expect(exact && exact.value().command_bonuses.front().bonus.applicable
        == std::vector<eawr::sim::tactical::TypeId>{eawr::assets::object_type_crc("Test_Frigate")},
        "WHE-14: exact allowed type bypasses ordinary exclusions");
    allowed.filter.applicable_types.clear();
    allowed.filter.applicable_categories = allowed_tables.find("Test_Frigate")->category_bits;
    const auto excluded = eawr::skirmish::economy_rules(command_start, {}, allowed_tables);
    expect(excluded && !std::binary_search(excluded.value().command_bonuses.front().bonus.applicable.begin(),
        excluded.value().command_bonuses.front().bonus.applicable.end(), eawr::assets::object_type_crc("Test_Frigate")),
        "WHE-14: category match retains exact type exclusions");
    allowed.enabled = false;
    const auto disabled = eawr::skirmish::economy_rules(command_start, {}, allowed_tables);
    expect(disabled && disabled.value().command_bonuses.empty(), "WHE-11: disabled passive source never registers");
}

void hunt_tables() {
    TempTree tree;
    auto xml = units_xml("3600", "Unit_Select_Test");
    const std::string_view craft = "<SpaceUnit Name=\"Test_Fighter\">";
    const auto position = xml.find(craft);
    expect(position != std::string::npos, "hunt fixture has a craft");
    if (position == std::string::npos) return;
    xml.insert(position + craft.size(), R"(<Is_Force_Sensitive>Yes</Is_Force_Sensitive>
      <Unit_Abilities_Data SubObjectList="Yes"><Unit_Ability><Type>HUNT</Type></Unit_Ability></Unit_Abilities_Data>)");
    write_fixture(tree.root, xml);
    const auto loaded = load(tree.root, models(-40.0F));
    expect(loaded.tables.has_value(), "Hunt data fixture loads");
    if (!loaded.tables) return;
    const auto compiled = eawr::units::ability_table(*loaded.tables);
    expect(static_cast<bool>(compiled), "Hunt ability table validates");
    if (!compiled) return;
    const auto* profile = compiled.value().find(eawr::assets::object_type_crc("Test_Fighter"));
    expect(profile && profile->force_sensitive && profile->hunt_reveal_range == Fixed::from_raw(raw(500))
        && profile->abilities.size() == 1
        && profile->abilities[0].kind == eawr::sim::tactical::AbilityKind::hunt
        && profile->abilities[0].expiration_frames == 0 && profile->abilities[0].recharge_frames == 0
        && !profile->abilities[0].supports_autofire,
        "WAB-50/54: craft Hunt binds its flag and untimed manual switch");
    expect(!compiled.value().find(eawr::assets::object_type_crc("Test_Squadron")),
        "AB-15: the squadron's authored Hunt does not replace its craft holders");
    auto changed = *loaded.tables;
    for (auto& unit : changed.units) if (unit.id == "Test_Fighter") unit.force_sensitive = false;
    expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
        "WAB-54: force-sensitive Hunt decisions bind replay content identity");
}

void bind_frame_errors() {
    // W-07: a model's slightly skewed frame must not discard its weapon or its unit.
    TempTree tree;
    write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
    auto assets = models(-40.0F);
    assets.at("data/art/models/test_frigate.alo").bones[1].relative_transform[2] = 0.002F;
    const auto loaded = load(tree.root, assets);
    expect(loaded.tables.has_value(), "W-07: nonorthogonal model frame loads");
    if (loaded.tables) {
        const auto combat = eawr::units::combat_table(*loaded.tables);
        expect(static_cast<bool>(combat), "W-07: nonorthogonal model frame binds without a loader refusal");
        const auto* ship = combat ? combat.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
        expect(ship && !ship->weapons.empty() && ship->weapons.front().fire_axes,
            "W-07: the skewed weapon remains present with its fire frame");
        if (ship && !ship->weapons.empty() && ship->weapons.front().fire_axes) {
            const auto& axes = *ship->weapons.front().fire_axes;
            const auto product = eawr::sim::math::dot(axes[1], axes[2]);
            expect(product && (product.value().raw() < -eawr::sim::tactical::frame_tolerance_raw
                    || product.value().raw() > eawr::sim::tactical::frame_tolerance_raw),
                "W-07: binding preserves skew rather than orthogonalizing the authored frame");
        }
        expect(std::any_of(loaded.tables->notes.begin(), loaded.tables->notes.end(), [](const auto& row) {
            return row.field == "Fire_Bone_A" && row.reason == "W-07: nonorthogonal bone axes retained";
        }), "W-07: the retained fire frame has a counted data diagnostic");
    }
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


} // namespace unit_tables_test_support
