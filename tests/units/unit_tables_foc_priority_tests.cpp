#include "unit_tables_foc_fixtures.hpp"
#include "../../src/units/unit_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace unit_tables_test_support {
namespace {

// One stock Priority_Set, with counts pinned from an independent parse of the effective XML (comma, space and
// `|` separated tokens; the test itself reads only the set names, then decodes with the loader under test):
// its entries' kinds (c category, p property, t exact type), the number of entries, twice the sum of the weights
// (every stock weight is a multiple of 0.5), and the token counts of the three exclusion lists.
struct StockSet {
    std::string_view name;
    std::string_view kinds;
    std::size_t entries;
    std::int64_t double_weight_sum;
    std::size_t category_exclusions;
    std::size_t property_exclusions;
    std::size_t unit_exclusions;
};

constexpr StockSet stock_sets[] = {
    {"AntiInfantryTurret", "cc", 2, 6, 2, 0, 0},
    {"AntiVehicleTurret", "cc", 2, 6, 2, 0, 0},
    {"AntiAirTurret", "c", 1, 2, 4, 0, 0},
    {"MassDriverTurret", "ccc", 3, 12, 2, 0, 0},
    {"TorpTurret", "ccc", 3, 12, 3, 0, 0},
    {"MDURocketTurret", "cccc", 4, 20, 0, 0, 0},
    {"Transport", "c", 1, 2, 0, 1, 4},
    {"Fighter", "ccc", 3, 12, 0, 1, 4},
    {"Bomber", "cccccc", 6, 31, 0, 1, 4},
    {"Corvette", "cccc", 4, 20, 0, 1, 4},
    {"Frigate", "ccccc", 5, 27, 0, 1, 4},
    {"Capital", "ccccccc", 7, 56, 0, 1, 4},
    {"SpaceArtillery", "ccccccc", 7, 47, 0, 1, 4},
    {"Vengeance_TP", "ccccccc", 7, 56, 0, 1, 4},
    {"Arc_Hammer_TP", "ccccccc", 7, 56, 0, 1, 4},
    {"Fighter_Attack_Move", "cccc", 4, 20, 2, 0, 4},
    {"Bomber_Attack_Move", "cccc", 4, 33, 2, 0, 4},
    {"Corvette_Attack_Move", "cccc", 4, 20, 2, 0, 4},
    {"Frigate_Attack_Move", "ccccc", 5, 27, 1, 0, 4},
    {"Capital_Attack_Move", "cccccc", 6, 35, 0, 0, 4},
    {"Bomber_Hit_And_Run", "ccccc", 5, 21, 2, 0, 4},
    {"AntiBomber", "c", 1, 2, 0, 0, 4},
    {"AntiHero", "cc", 2, 3, 0, 0, 4},
    {"AntiInfantry", "ccpcp", 5, 21, 3, 1, 0},
    {"AntiVehicle", "ccpcp", 5, 23, 3, 1, 0},
    {"AntiVehicleStructure", "ccpccp", 6, 31, 2, 1, 0},
    {"AntiAir", "ccpcp", 5, 30, 2, 1, 0},
    {"ATATAntiEverything", "ccpccp", 6, 33, 1, 1, 0},
    {"Lancet_TP", "c", 1, 2, 2, 1, 0},
    {"Vornskr_Attack_Priority", "cc", 2, 6, 4, 1, 5},
    {"Destroyer_Droid_Attack_Priority", "cccpp", 5, 30, 3, 1, 0},
    {"Rocket_Infantry_Attack_Priority", "ccpcccp", 7, 54, 1, 1, 0},
    {"Rocket_Vehicle_Attack_Priority", "cpcccp", 6, 40, 2, 1, 0},
    {"Anti_All_Attack_Priority", "cpccpcc", 7, 50, 1, 1, 0},
    {"Felucia_Plant_Attack_Priority", "ccc", 3, 12, 3, 1, 0},
    {"Rancor_Dark_Ally_Attack_Priority", "ccp", 3, 7, 3, 1, 3},
    {"Indigenous_Vornskr_Attack_Priority", "ccc", 3, 12, 3, 1, 5},
    {"AntiStructure", "pccp", 4, 20, 2, 1, 0},
    {"AntiInfantry_No_Structures", "ccp", 3, 7, 3, 1, 0},
    {"AntiVehicle_No_Structures", "ccp", 3, 9, 3, 1, 0},
    {"AvoidAntiInfantryTurrets", "ccpcp", 5, 21, 3, 1, 3},
    {"Infantry_Attack_Move", "cccpp", 5, 30, 2, 1, 3},
    {"Rocket_Infantry_Attack_Move", "tttcppccc", 9, 60, 1, 1, 3},
    {"Air_Attack_Move", "tcppcccc", 8, 72, 1, 1, 7},
    {"Heavy_Vehicle_Attack_Move", "tttcppcc", 8, 46, 2, 1, 5},
    {"Light_Vehicle_Attack_Move", "cccpp", 5, 30, 2, 1, 5},
    {"Artillery_Attack_Move", "ppcccc", 6, 42, 2, 1, 0},
};

std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

const eawr::units::TargetingPrioritySet* set_named(const eawr::units::UnitTables& tables, const std::string_view name) {
    for (const auto& set : tables.priority_sets) {
        if (set.id == name) return &set;
    }
    return nullptr;
}

// A candidate type with the given category and property bits (LC-28).
eawr::units::UnitType candidate(const std::string& id, const std::uint64_t categories, const std::uint64_t properties = 0) {
    eawr::units::UnitType unit;
    unit.id = id;
    unit.category_bits = categories;
    unit.property_bits = properties;
    return unit;
}

} // namespace

// LC-28 (#5895): every stock targeting priority set decodes from the effective game data as integers
// (Fixed weights, enum bits), with no row reported against it. The set names come from the three files the
// registry lists, so a set added by data cannot be skipped by a stale list.
void foc_priority_sets(eawr::units::LoadInput input) {
    namespace units = eawr::units;
    expect(input.filesystem != nullptr, "LC-28: the installed game is mounted");
    if (input.filesystem == nullptr) return;
    constexpr std::string_view registry = "data/xml/targetingprioritysetfiles.xml";
    const auto listed = eawr::data::load_document(*input.filesystem, registry);
    expect(static_cast<bool>(listed), "LC-28: the priority set registry loads");
    if (!listed) return;
    std::vector<std::string> names;
    for (const auto& file : listed.value().root.children) {
        if (file.name != "File") continue;
        const auto path = eawr::data::registry_include_path(registry, trimmed(file.raw_text));
        const auto document = eawr::data::load_document(*input.filesystem, path);
        expect(static_cast<bool>(document), "LC-28: priority set file loads: " + path);
        if (!document) continue;
        for (const auto& set : document.value().root.children) {
            for (const auto& attribute : set.attributes) {
                if (attribute.name == "Name") names.push_back(attribute.value);
            }
        }
    }
    expect(names.size() == std::size(stock_sets), "LC-28: the three files hold the 47 stock sets this test counts");

    units::UnitTables tables;
    units::detail::Report report;
    units::detail::load_enums(*input.filesystem, tables, report);
    units::detail::load_priority_sets(*input.filesystem, names, tables, report);
    expect(report.unresolved.empty(), "LC-28: every stock priority set decodes with no unresolved row");
    for (const auto& row : report.unresolved) {
        expect(false, "LC-28: unresolved row " + row.owner + "/" + row.field + " '" + row.value + "': " + row.reason);
    }
    expect(tables.priority_sets.size() == names.size(), "LC-28: every listed stock set is decoded");

    for (const auto& stock : stock_sets) {
        const auto* set = set_named(tables, stock.name);
        expect(set != nullptr, "LC-28: stock set decodes: " + std::string(stock.name));
        if (set == nullptr) continue;
        std::string kinds;
        std::int64_t doubled = 0;
        bool exact = true;
        for (const auto& entry : set->attack_priorities) {
            kinds += entry.match == units::PriorityMatch::category ? 'c' : entry.match == units::PriorityMatch::property ? 'p' : 't';
            exact = exact && entry.weight.raw() > 0 && entry.weight.raw() % (units::Fixed::scale / 2) == 0
                && (entry.match == units::PriorityMatch::type ? entry.bits == 0 : entry.bits != 0);
            doubled += entry.weight.raw() / (units::Fixed::scale / 2);
        }
        expect(set->attack_priorities.size() == stock.entries && kinds == stock.kinds,
            "LC-28: entry count and kinds as authored: " + std::string(stock.name));
        expect(exact && doubled == stock.double_weight_sum,
            "LC-28: weights are the authored multiples of 0.5, exactly: " + std::string(stock.name));
        expect(set->category_exclusions.size() == stock.category_exclusions
                && set->property_exclusions.size() == stock.property_exclusions
                && set->unit_exclusions.size() == stock.unit_exclusions,
            "LC-28: the three exclusion lists as authored: " + std::string(stock.name));
        expect((stock.category_exclusions == 0) == (set->category_exclusion_bits == 0)
                && (stock.property_exclusions == 0) == (set->property_exclusion_bits == 0),
            "LC-28: exclusion lists resolve to enum bits: " + std::string(stock.name));
        // An exact type entry decides at once, before every exclusion (the precedence: unit name first).
        for (const auto& entry : set->attack_priorities) {
            if (entry.match != units::PriorityMatch::type) continue;
            const auto score = units::attack_priority(*set, candidate(entry.name, set->category_exclusion_bits,
                set->property_exclusion_bits));
            expect(score == entry.weight, "LC-28: a type entry beats every exclusion: " + std::string(stock.name) + "/" + entry.name);
        }
        // A unit exclusion drops a type that no entry names, whatever its categories.
        for (const auto& excluded : set->unit_exclusions) {
            const bool named = std::any_of(set->attack_priorities.begin(), set->attack_priorities.end(),
                [&](const units::PriorityEntry& entry) { return entry.match == units::PriorityMatch::type && entry.name == excluded; });
            if (named) continue;
            expect(!units::attack_priority(*set, candidate(excluded, ~std::uint64_t{0})),
                "LC-28: a unit exclusion removes the type: " + std::string(stock.name) + "/" + excluded);
        }
    }

    // The three ordinary precedence steps on the stock land sets, with the enum values of the game.
    const auto bits = [&](const std::string_view name) {
        const auto category = units::detail::enum_value(tables.categories, name);
        const auto property = units::detail::enum_value(tables.properties, name);
        return std::pair{category.value_or(0), property.value_or(0)};
    };
    const auto weight = [](const char* text) { return units::Fixed::from_decimal(text).value(); };
    const auto infantry = bits("Infantry").first, vehicle = bits("Vehicle").first, land_hero = bits("LandHero").first;
    const auto air = bits("Air").first, structure = bits("Structure").first, wall = bits("Wall").first;
    const auto turret = bits("Turret").second, tactical_structure = bits("TacticalStructure").second;
    const auto not_opportunity = bits("NotOpportunityTarget").second;
    expect(infantry && vehicle && land_hero && air && structure && wall && turret && tactical_structure && not_opportunity,
        "LC-28: the land category and property names are in the game's enums");
    const auto* anti_infantry = set_named(tables, "AntiInfantry");
    const auto* anti_vehicle = set_named(tables, "AntiVehicle");
    if (anti_infantry == nullptr || anti_vehicle == nullptr) return;
    const auto score = [&](const units::TargetingPrioritySet& set, const std::uint64_t categories, const std::uint64_t properties) {
        return units::attack_priority(set, candidate("Stock_Candidate", categories, properties));
    };
    expect(score(*anti_infantry, land_hero, 0) == weight("0.5") && score(*anti_infantry, infantry, 0) == weight("1.0")
            && score(*anti_infantry, vehicle, 0) == weight("3.0"),
        "LC-28: AntiInfantry ranks land heroes 0.5, infantry 1, vehicles 3 (category entries)");
    expect(score(*anti_infantry, structure, turret) == weight("2.0"),
        "LC-28: a Turret property entry overrides the Structure category exclusion");
    expect(score(*anti_infantry, structure, tactical_structure) == weight("4.0"),
        "LC-28: a TacticalStructure property entry overrides the Structure category exclusion");
    expect(!score(*anti_infantry, structure, 0) && !score(*anti_infantry, wall, 0) && !score(*anti_infantry, air, 0),
        "LC-28: Wall, Structure and Air are excluded categories");
    expect(!score(*anti_infantry, infantry, not_opportunity),
        "LC-28: a property exclusion removes a candidate that a category entry would rank");
    expect(score(*anti_vehicle, vehicle, 0) == weight("1.0") && score(*anti_vehicle, land_hero, 0) == weight("1.5"),
        "LC-28: AntiVehicle ranks vehicles 1 and land heroes 1.5");
    expect(score(*anti_infantry, vehicle | infantry, 0) == weight("1.0"),
        "LC-28: several matching categories take the best (smallest) weight");
}

} // namespace unit_tables_test_support
