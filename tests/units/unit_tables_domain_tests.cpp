#include "unit_tables_support.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/units/domain_tags.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace unit_tables_test_support {

namespace {

using eawr::sim::tactical::BattleDomain;
using eawr::units::DomainTag;

// Every XML file of the fixture with `from` spelled `to`: the land twin of the space fixture.
void respell(const std::filesystem::path& root, const std::string& from, const std::string& to) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".xml") continue;
        std::string text;
        {
            std::ifstream input(entry.path(), std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        }
        for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
            text.replace(at, from.size(), to);
        write(entry.path(), text);
    }
}

bool has_scalar(const eawr::units::UnitTables& tables, const std::string_view tag) {
    for (const auto& constant : tables.constants.scalars)
        if (constant.tag == tag) return true;
    return false;
}

} // namespace

// BF-2 (#3779): one selector picks the space or land spelling of the domain tags.
void domain_tags() {
    expect(eawr::units::domain_tag(BattleDomain::space, DomainTag::reveal_range) == "Space_FOW_Reveal_Range"
               && eawr::units::domain_tag(BattleDomain::land, DomainTag::reveal_range) == "Land_FOW_Reveal_Range",
           "BF-2: reveal range selects by domain");
    expect(eawr::units::domain_tag(BattleDomain::space, DomainTag::layer) == "Space_Layer"
               && eawr::units::domain_tag(BattleDomain::land, DomainTag::layer).empty(),
           "BF-2: land authors no layer tag");
    expect(eawr::units::domain_tag(BattleDomain::land, DomainTag::speed_multiplier) == "Object_Max_Speed_Multiplier_Land"
               && eawr::units::domain_tag(BattleDomain::land, DomainTag::health_multiplier)
                      == "Object_Max_Health_Multiplier_Land",
           "BF-2: multipliers select by domain");
    expect(eawr::units::domain_tag(BattleDomain::space, DomainTag::model_name) == "Space_Model_Name"
               && eawr::units::domain_tag(BattleDomain::land, DomainTag::model_name) == "Land_Model_Name",
           "BF-2: the model tag selects by domain (LS shoot PR 1)");
    expect(eawr::units::domain_variant(BattleDomain::space, "Object_Max_Speed_Multiplier_Space") == "Object_Max_Speed_Multiplier_Space"
               && eawr::units::domain_variant(BattleDomain::land, "Object_Max_Speed_Multiplier_Space")
                      == "Object_Max_Speed_Multiplier_Land"
               && eawr::units::domain_variant(BattleDomain::land, "Stealth_Detection_Time") == "Stealth_Detection_Time"
               && eawr::units::domain_variant(BattleDomain::land, "Space_Layer") == "Space_Layer",
           "BF-2: only tags with a land spelling change");
    expect(eawr::units::automatic_activation_name(BattleDomain::space) == "Space_Automatic"
               && eawr::units::automatic_activation_name(BattleDomain::land) == "Ground_Automatic",
           "BF-2: automatic activation spelling by domain");
    using namespace eawr::sim::tactical;
    expect(special_ability_mode(BattleDomain::space) == SpecialAbilityMode::space
               && special_ability_mode(BattleDomain::land) == SpecialAbilityMode::ground,
           "BF-2: abilities take their mode from the domain");
    expect(automatic_special_style(BattleDomain::space) == special_activation_style("Space_Automatic")
               && automatic_special_style(BattleDomain::land) == special_activation_style("Ground_Automatic"),
           "BF-2: sim and loader agree on the automatic style");

    TempTree tree;
    auto xml = units_xml("3600", "Unit_Select_Test");
    const std::string fighter_tag = "<SpaceUnit Name=\"Test_Fighter\">";
    xml.insert(xml.find(fighter_tag) + fighter_tag.size(), R"(
      <Abilities><Combat_Bonus_Ability Name="Fixture_Command">
        <Activation_Style>Space_Automatic</Activation_Style>
        <Stacking_Category>3</Stacking_Category>
        <Applicable_Unit_Types>Test_Frigate</Applicable_Unit_Types>
        <Health_Bonus_Percentage>-0.25</Health_Bonus_Percentage>
      </Combat_Bonus_Ability></Abilities>)");
    write_fixture(tree.root, xml);
    const auto assets = models(-40.0F);
    const auto space = load(tree.root, assets);
    expect(static_cast<bool>(space.tables), "BF-2: the space fixture loads");
    expect(space.tables->domain == BattleDomain::space, "BF-2: tables default to the space domain");
    const auto space_identity = eawr::units::content_identity(*space.tables);
    const auto land_on_space_data = load(tree.root, assets, "Normal_Default", BattleDomain::land);
    expect(land_on_space_data.tables && !land_on_space_data.tables->units.empty()
               && !land_on_space_data.tables->units[0].fow_reveal_range,
           "BF-2: a land load does not read the space reveal tag");

    respell(tree.root, "Space_FOW_Reveal_Range", "Land_FOW_Reveal_Range");
    respell(tree.root, "Object_Max_Speed_Multiplier_Space", "Object_Max_Speed_Multiplier_Land");
    respell(tree.root, "Object_Max_Health_Multiplier_Space", "Object_Max_Health_Multiplier_Land");
    respell(tree.root, "<Object_Max_Health_Multiplier_Land> 1.5 <", "<Object_Max_Health_Multiplier_Land> 3.0 <");
    respell(tree.root, "Space_Automatic", "Ground_Automatic");
    const auto land = load(tree.root, assets, "Normal_Default", BattleDomain::land);
    const auto space_on_land_data = load(tree.root, assets);
    expect(land.tables && land.tables->domain == BattleDomain::land, "BF-2: a land load records its domain");
    expect(space.tables->find("Test_Fighter") && space.tables->find("Test_Fighter")->production.combat_bonuses.size() == 1
               && land_on_space_data.tables && land_on_space_data.tables->find("Test_Fighter")
               && land_on_space_data.tables->find("Test_Fighter")->production.combat_bonuses.empty(),
           "BF-2: a land load skips the space automatic ability variant");
    expect(land.tables && land.tables->find("Test_Fighter") && land.tables->find("Test_Fighter")->production.combat_bonuses.size() == 1,
           "BF-2: a land table reads the ground automatic ability variant");
    expect(space_on_land_data.tables && !space_on_land_data.tables->units.empty()
               && !space_on_land_data.tables->units[0].fow_reveal_range,
           "BF-2: a space load does not read the land reveal tag");
    expect(land.tables && land.tables->units.size() >= 5, "BF-2: the land fixture loads its units");
    if (!land.tables || !space.tables || land.tables->units.size() < 5) return;
    expect(land.tables && land.tables->units.size() >= 2 && land.tables->units[0].fow_reveal_range && land.tables->units[0].fow_reveal_range->raw() == raw(2000)
               && land.tables->units[1].fow_reveal_range && land.tables->units[1].fow_reveal_range->raw() == raw(1200),
           "BF-2: a land fixture table reads Land_FOW_Reveal_Range");
    expect(has_scalar(*land.tables, "Object_Max_Speed_Multiplier_Land")
               && has_scalar(*land.tables, "Object_Max_Health_Multiplier_Land")
               && !has_scalar(*land.tables, "Object_Max_Speed_Multiplier_Space"),
           "BF-2: a land table carries the land multipliers");
    const auto space_durability = eawr::units::durability_table(*space.tables);
    const auto land_durability = eawr::units::durability_table(*land.tables);
    expect(space_durability && land_durability && !space_durability.value().profiles.empty()
               && land_durability.value().profiles.size() == space_durability.value().profiles.size()
               && land_durability.value().profiles[0].max_hull.raw() == 2 * space_durability.value().profiles[0].max_hull.raw(),
           "BF-2: durability follows the land health multiplier (3.0 land vs 1.5 space doubles the hull)");
    expect(eawr::units::content_identity(*land.tables) != space_identity, "BF-2: land content has its own identity");
    auto relabelled = *land.tables;
    relabelled.domain = BattleDomain::space;
    expect(eawr::units::canonical_encoding(relabelled).size() < eawr::units::canonical_encoding(*land.tables).size(),
           "BF-2: only land tables bind the domain record");
    expect(eawr::units::content_identity(*space.tables) == space_identity, "BF-2: space identity is stable");
    {
        // SKI-25/SKI-64 (#4992): Tactical_Health 0 builds no row; a hull above the simulation bound
        // after the 1.5 space multiplier (AaW's Arc_Hammer at 960000 stays inside) is held at the bound with one warning.
        auto edited = *space.tables;
        edited.units[0].hull = eawr::sim::math::Fixed{};
        edited.units[1].hull = eawr::sim::math::Fixed::from_raw(eawr::sim::tactical::max_durability_health * eawr::sim::math::Fixed::scale);
        std::vector<eawr::core::Diagnostic> notes;
        const auto bounded = eawr::units::durability_table(edited, &notes);
        const auto profile_of = [&](const std::string& id) -> const eawr::sim::tactical::DurabilityProfile* {
            if (!bounded) return nullptr;
            for (const auto& profile : bounded.value().profiles)
                if (profile.type_id == eawr::assets::object_type_crc(id)) return &profile;
            return nullptr;
        };
        const auto* over = profile_of(edited.units[1].id);
        expect(bounded && !profile_of(edited.units[0].id) && over
                && over->max_hull.raw() == eawr::sim::tactical::max_durability_health * eawr::sim::math::Fixed::scale
                && notes.size() == 1 && notes[0].severity == eawr::core::Severity::warning
                && notes[0].message.find(edited.units[1].id) != std::string::npos,
            "SKI-25: no row for a zero hull; SKI-64: an over-bound hull is held at the bound and reported");
    }
}

} // namespace unit_tables_test_support
