// PR-C1/C2/C4, LSL-P2: exercise integer coercion at each SK F1 reader, before consumer validation.
#include "unit_tables_support.hpp"
#include "../../src/units/unit_internal.hpp"
#include "eawr/data/retail_values.hpp"

#include <algorithm>
#include <iostream>
#include <limits>

namespace unit_tables_test_support {
namespace { int failure_count = 0; }
int& failures = failure_count;
void expect(const bool condition, const std::string_view message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
}

namespace {
using namespace unit_tables_test_support;
struct Case { std::string_view text; std::optional<std::int32_t> expected; };
constexpr Case cases[] = {
    {"2.7", 2}, {" 5", 5}, {"-3", -3}, {"abc", 0}, {"", std::nullopt},
    {"99999999999", 2147483647}, {"TBD", std::nullopt}, {"-99999999999", (-2147483647 - 1)},
    {"+12tail", 12}, {"-0.5", 0}, {"1e3", 1}, {"0x10", 0}, {"70000", 70000}, {"1.0", 1}
};
std::string tag(const std::string_view name, const std::string_view value) {
    return "<" + std::string(name) + ">" + std::string(value) + "</" + std::string(name) + ">";
}
void append(std::string& xml, const std::string_view opening, const std::string_view closing, const std::string& fields) {
    const auto begin = xml.find(opening);
    const auto end = xml.find(closing, begin);
    expect(begin != std::string::npos && end != std::string::npos, "fixture insertion has its object");
    if (end != std::string::npos) xml.insert(end, fields);
}
void boundary(const Case& test) {
    const auto parsed = eawr::data::retail::integer_value(test.text);
    expect(parsed ? test.expected && parsed->value == *test.expected : !test.expected,
        "PR-C4: shared parser matches the retail boundary");
    TempTree tree;
    auto xml = units_xml("3600", "Unit_Select_Test");
    // Duplicate scalar assignments prove that empty/TBD keeps an earlier value (PR-C1/C2).
    std::string fields;
    for (const auto name : {"Population_Value", "Space_Override_Population_Value", "GUI_Row",
            "Build_Limit_Lifetime_Per_Player", "Build_Limit_Current_Per_Player",
            "Build_Limit_Lifetime_For_All_Allies", "Build_Limit_Current_For_All_Allies"})
        fields += tag(name, "7") + tag(name, test.text);
    fields += tag("Faction_Anim_Subindex", "Neutral, " + std::string(test.text));
    fields += "<Abilities><Laser_Defense_Ability Name=\"IntDefense\">"
        + tag("Laser_Beam_Frames", test.text) + "</Laser_Defense_Ability></Abilities>";
    append(xml, "<SpaceUnit Name=\"Test_Frigate\">", "</SpaceUnit>", fields);
    append(xml, "<SpaceUnit Name=\"Test_Frigate\">", "</Unit_Abilities_Data>",
        "<Unit_Ability><Type>LURE</Type>" + tag("Max_Number_Of_Pulses", test.text) + "</Unit_Ability>");
    append(xml, "<Projectile Name=\"Proj_Test_Small\">", "</Projectile>",
        tag("Projectile_Blast_Area_Dropoff_Tiers", "7") + tag("Projectile_Blast_Area_Dropoff_Tiers", test.text)
        + tag("Projectile_Blast_Area_Max_Victims", "7") + tag("Projectile_Blast_Area_Max_Victims", test.text));
    append(xml, "<StarBase Name=\"Test_Base\">", "</StarBase>",
        tag("Reserve_Spawned_Units_Tech_0", "Test_Squadron, " + std::string(test.text)));
    write_fixture(tree.root, xml);
    std::string hardpoints(hardpoints_xml);
    append(hardpoints, "<HardPoint Name=\"HP_Test_Laser\">", "</HardPoint>",
        tag("Fire_Pulse_Count", "7") + tag("Fire_Pulse_Count", test.text)
        + tag("Projectile_Appearance_Delay_Frames", "7") + tag("Projectile_Appearance_Delay_Frames", test.text));
    write(tree.root / "XML" / "eawr_hardpoints.xml", hardpoints);
    const auto loaded = load(tree.root, models(-40.0F));
    expect(loaded.tables.has_value(), "F1 boundary catalog loads");
    if (!loaded.tables) return;
    const auto& tables = *loaded.tables;
    const auto* ship = tables.find("Test_Frigate");
    const auto* station = tables.find("Skirmish_Test_Base");
    expect(ship && station, "F1 retains the unit and station");
    if (!ship || !station) return;
    const auto signed_value = test.expected.value_or(7);
    const auto stored = static_cast<std::uint32_t>(signed_value);
    const std::optional<std::uint32_t> limit = signed_value < 0 ? std::nullopt : std::optional{stored};
    expect(ship->production.population_value == stored, "SKI-01/40: integer path avoids Q24 and retains signed bits");
    expect(ship->production.space_override_population_value == limit, "SKI-40: override keeps the negative-unbounded convention");
    expect(ship->gui_row == stored, "SKI-42: GUI_Row retains retail integer bits");
    expect(ship->production.lifetime_player == limit && ship->production.current_player == limit
        && ship->production.lifetime_allies == limit && ship->production.current_allies == limit,
        "SKI-39: every build-limit reader parses before the unbounded convention");
    const auto& weapon = ship->hardpoints.front().weapon;
    expect(weapon && weapon->pulse_count == stored, "SKI-02: count uses the integer parser");
    expect(weapon && weapon->appearance_delay_frames == stored, "SKI-04: appearance frames accept signed and fractional text");
    const auto projectile = std::find_if(tables.projectiles.begin(), tables.projectiles.end(),
        [](const auto& row) { return row.id == "Proj_Test_Small"; });
    expect(projectile != tables.projectiles.end() && projectile->blast.tiers == signed_value
        && projectile->blast.max_victims == signed_value, "SKI-06: both signed blast integers retain retail values");
    expect(!ship->special_abilities.empty() && ship->special_abilities.front().defence_beam_frames
        == static_cast<std::uint32_t>(test.expected.value_or(5)), "SKI-19: beam frames parse with the retail default");
    const auto lure = std::find_if(ship->abilities.begin(), ship->abilities.end(),
        [](const auto& row) { return row.type == "LURE"; });
    const auto pulses = test.expected ? std::optional{static_cast<std::uint32_t>(*test.expected)} : std::nullopt;
    expect(lure != ship->abilities.end() && lure->max_pulses == pulses, "SKI-21: lure count retains retail values or stays absent");
    const auto animation = test.expected ? pulses : std::nullopt;
    expect(ship->neutral_fog_animation_index == animation, "SKI-43: faction pair index has no 16-bit restriction");
    expect(station->spawner && station->spawner->reserves.size() == (test.expected ? 2U : 1U),
        "SKI-34: malformed numeric count retains the pair; empty count skips the incomplete pair");
    if (station->spawner && test.expected && station->spawner->reserves.size() == 2)
        expect(station->spawner->reserves.back().count == *test.expected, "SKI-34: spawned pair keeps its signed integer count");
    if (test.expected && (!parsed->exact || *test.expected < 0)) {
        for (const auto name : {"Population_Value", "GUI_Row", "Faction_Anim_Subindex", "Laser_Beam_Frames", "Max_Number_Of_Pulses"})
            expect(has_row(tables.notes, "Test_Frigate", name), "F1: previously rejected numeric input leaves a note");
        expect(has_row(tables.notes, "HP_Test_Laser", "Fire_Pulse_Count"), "SKI-02: count coercion is diagnosed");
    }

    if (test.expected) {
        for (const auto name : {"Population_Value", "Space_Override_Population_Value", "GUI_Row",
                "Build_Limit_Lifetime_Per_Player", "Build_Limit_Current_Per_Player",
                "Build_Limit_Lifetime_For_All_Allies", "Build_Limit_Current_For_All_Allies",
                "Faction_Anim_Subindex", "Laser_Beam_Frames", "Max_Number_Of_Pulses"})
            expect(!has_row(tables.unresolved, "Test_Frigate", name), "F1: numeric coercion is never an unresolved input");
        if (!parsed->exact) {
            expect(has_row(tables.notes, "Proj_Test_Small", "Projectile_Blast_Area_Dropoff_Tiers")
                && has_row(tables.notes, "Proj_Test_Small", "Projectile_Blast_Area_Max_Victims"),
                "SKI-06: both blast integer coercions leave notes");
            expect(has_row(tables.notes, "Skirmish_Test_Base", "Reserve_Spawned_Units_Tech_0"),
                "SKI-34: spawned pair count coercion leaves a note");
        }
    }

    // SKI-36 is supplied by the shared land-loader dependency; exercise its integer boundary here too.
    auto land_xml = units_xml("3600", "Unit_Select_Test");
    const std::string opening = "<SpaceUnit Name=\"Test_Frigate\">";
    const auto begin = land_xml.find(opening);
    const auto end = land_xml.find("</SpaceUnit>", begin);
    land_xml.replace(begin, end + std::string_view{"</SpaceUnit>"}.size() - begin,
        "<GroundCompany Name=\"Test_Frigate\"><Company_Units>Test_Fighter</Company_Units>"
        + tag("Max_Squad_Size", test.text) + "</GroundCompany>");
    write_fixture(tree.root, land_xml);
    const auto land = load(tree.root, models(-40.0F), "Normal_Default", eawr::sim::tactical::BattleDomain::land);
    expect(land.tables.has_value(), "SKI-36: land boundary loads");
    if (!land.tables) return;
    const auto company = std::find_if(land.tables->land_companies.begin(), land.tables->land_companies.end(),
        [](const auto& row) { return row.id == "Test_Frigate"; });
    const auto size = test.expected && *test.expected > 0 ? pulses : std::nullopt;
    expect(company != land.tables->land_companies.end() && company->max_squad_size == size,
        "SKI-36: land size truncates/saturates, with nonpositive grouping left to the land consumer");
}
void appearance_contexts() {
    // ET-T060 is a scalar on hardpoints; ET-T053 is an object-definition sequence.
    // F1 changes only the former. The existing object-sequence consumer seam stays unbound.
    for (const auto& [text, expected] : {std::pair{"15,20", 15U}, std::pair{"2,1", 2U}}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        append(xml, "<SpaceUnit Name=\"Test_Fighter\">", "</SpaceUnit>",
            tag("Projectile_Appearance_Delay_Frames", text));
        write_fixture(tree.root, xml);
        std::string hardpoints(hardpoints_xml);
        append(hardpoints, "<HardPoint Name=\"HP_Test_Laser\">", "</HardPoint>",
            tag("Projectile_Appearance_Delay_Frames", text));
        write(tree.root / "XML" / "eawr_hardpoints.xml", hardpoints);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "SKI-04: appearance contexts load");
        if (!loaded.tables) continue;
        const auto* fighter = loaded.tables->find("Test_Fighter");
        expect(fighter && fighter->weapon && !fighter->weapon->appearance_delay_frames,
            "ET-T053: object appearance sequence remains unbound, preserving existing stock behavior");
        const auto* frigate = loaded.tables->find("Test_Frigate");
        expect(frigate && !frigate->hardpoints.empty() && frigate->hardpoints.front().weapon
            && frigate->hardpoints.front().weapon->appearance_delay_frames == expected,
            "ET-T060 / PR-C4: hardpoint scalar reads the leading integer and notes its suffix");
        expect(has_row(loaded.tables->notes, "HP_Test_Laser", "Projectile_Appearance_Delay_Frames"),
            "ET-T060: hardpoint scalar coercion is diagnosed");
    }
}

}

int main() {
    for (const auto& test : cases) {
        std::cerr << "F1 input: [" << test.text << "]\n";
        boundary(test);
    }
    appearance_contexts();
    return unit_tables_test_support::failures == 0 ? 0 : 1;
}
