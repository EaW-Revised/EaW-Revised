#pragma once
// Reference (pre-index, linear-scan) rules for the unit-table name lookups, and the checks that compare a loaded table
// against them. Shared by the unit-table and the GC Start index tests. A mismatch is counted in `failures`.

#include "eawr/core/diagnostic.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/core/text.hpp"
#include "eawr/units/unit_tables.hpp"
#include "../../src/units/unit_tables_decode.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace unit_tables_test_support {
extern int& failures;
}

namespace unit_tables_index_reference {
using unit_tables_test_support::failures;

using eawr::core::iequals;
using eawr::units::no_index;
using eawr::units::UnitTables;

// The pre-index rules, verbatim: first unit with a loaded type, first projectile, last priority set.
inline std::uint32_t ref_unit(const UnitTables& tables, const std::string& id) {
    for (std::size_t index = 0; index < tables.units.size(); ++index) {
        if (iequals(tables.units[index].id, id) && !tables.units[index].xml_type.empty()) return static_cast<std::uint32_t>(index);
    }
    return no_index;
}

inline std::uint32_t ref_projectile(const UnitTables& tables, const std::string& id) {
    for (std::size_t index = 0; index < tables.projectiles.size(); ++index) {
        if (iequals(tables.projectiles[index].id, id)) return static_cast<std::uint32_t>(index);
    }
    return no_index;
}

inline std::uint32_t ref_priority(const UnitTables& tables, const std::string& id, const std::uint32_t fallback) {
    auto result = fallback;
    for (std::size_t index = 0; index < tables.priority_sets.size(); ++index) {
        if (!id.empty() && iequals(tables.priority_sets[index].id, id)) result = static_cast<std::uint32_t>(index);
    }
    return result;
}

inline const eawr::units::UnitType* ref_find(const UnitTables& tables, const std::string_view id) {
    for (const auto& unit : tables.units) {
        if (iequals(unit.id, id)) return &unit;
    }
    return nullptr;
}

inline std::string upper(std::string value) {
    for (auto& character : value) character = static_cast<char>(eawr::core::ascii_upper(static_cast<unsigned char>(character)));
    return value;
}

// Every link target of every row, against the scan. Returns the number of links compared.
inline std::size_t check_links(const UnitTables& tables, const std::string_view label) {
    std::size_t links = 0;
    std::size_t bad = 0;
    const auto unit_link = [&](const std::uint32_t got, const std::string& name) {
        ++links;
        if (got != ref_unit(tables, name)) { ++bad; std::cerr << label << ": unit link '" << name << "' differs\n"; }
    };
    const auto projectile_link = [&](const std::uint32_t got, const std::string& name) {
        ++links;
        if (got != ref_projectile(tables, name)) { ++bad; std::cerr << label << ": projectile link '" << name << "' differs\n"; }
    };
    for (const auto& unit : tables.units) {
        if (unit.cluster_bomb) projectile_link(unit.cluster_bomb->projectile_index, unit.cluster_bomb->bomb_type);
        if (unit.death_projectile_indices.size() != unit.death_projectiles.size()) {
            ++bad;
            std::cerr << label << ": death projectile index count differs for " << unit.id << '\n';
        } else {
            for (std::size_t index = 0; index < unit.death_projectiles.size(); ++index)
                projectile_link(unit.death_projectile_indices[index], unit.death_projectiles[index]);
        }
        if (!unit.retreat_self_destruct.empty()) projectile_link(unit.retreat_self_destruct_index, unit.retreat_self_destruct);
        else if (unit.retreat_self_destruct_index != no_index) { ++bad; std::cerr << label << ": an unnamed retreat payload is linked\n"; }
        for (const auto& member : unit.members) unit_link(member.craft_index, member.craft);
        if (unit.spawner) {
            for (const auto& entry : unit.spawner->starting) unit_link(entry.squadron_index, entry.squadron);
            for (const auto& entry : unit.spawner->reserves) unit_link(entry.squadron_index, entry.squadron);
            for (const auto* tiers : {&unit.spawner->campaign_starting, &unit.spawner->campaign_reserves})
                for (const auto& tier : *tiers)
                    for (const auto& entry : tier) unit_link(entry.squadron_index, entry.squadron);
        }
        if (unit.weapon) projectile_link(unit.weapon->projectile_index, unit.weapon->projectile);
        for (const auto* list : {&unit.abilities, &unit.team_abilities}) {
            for (const auto& ability : *list) {
                projectile_link(ability.projectile_index, ability.projectile_override);
                projectile_link(ability.spawned_projectile_index, ability.spawned_object);
            }
        }
        for (const auto& hardpoint : unit.hardpoints) {
            if (!hardpoint.weapon) continue;
            projectile_link(hardpoint.weapon->projectile_index, hardpoint.weapon->projectile);
            projectile_link(hardpoint.weapon->charged_projectile_index, hardpoint.weapon->charged_projectile);
        }
        ++links;
        if (unit.targeting_priority_set_index != ref_priority(tables, unit.targeting_priority_set, no_index)) {
            ++bad;
            std::cerr << label << ": priority set link of " << unit.id << " differs\n";
        }
    }
    failures += static_cast<int>(bad);
    return links;
}

// find(): every id in either case, an absent name, and the empty name, against the scan.
inline std::size_t check_find(const UnitTables& tables, const std::string_view label) {
    std::size_t probes = 0;
    std::size_t bad = 0;
    const auto probe = [&](const std::string_view name) {
        ++probes;
        if (tables.find(name) != ref_find(tables, name)) { ++bad; std::cerr << label << ": find('" << name << "') differs\n"; }
    };
    for (const auto& unit : tables.units) {
        probe(unit.id);
        probe(upper(unit.id));
        probe(eawr::core::to_lower(unit.id));
    }
    probe("No_Such_Unit_Anywhere");
    probe("");
    failures += static_cast<int>(bad);
    return probes;
}

// The projectile "who uses it" flags of the loader (used for the required-tag rules), against the per-unit linear walk they replaced.
inline std::size_t check_projectile_use(const UnitTables& tables, const std::string_view label) {
    const auto use = eawr::units::unit_tables_detail::build_projectile_use(tables);
    std::size_t probes = 0;
    std::size_t bad = 0;
    for (const auto& projectile : tables.projectiles) {
        bool death = false, retreat = false, spawned = false, fired = false;
        for (const auto& unit : tables.units) {
            for (const auto& entry : unit.death_projectiles) death |= iequals(entry, projectile.id);
            retreat |= iequals(unit.retreat_self_destruct, projectile.id);
            for (const auto* list : {&unit.abilities, &unit.team_abilities}) for (const auto& ability : *list) {
                spawned |= iequals(ability.spawned_object, projectile.id);
                fired |= iequals(ability.projectile_override, projectile.id);
            }
            if (unit.weapon) fired |= iequals(unit.weapon->projectile, projectile.id);
            for (const auto& point : unit.hardpoints) if (point.weapon)
                fired |= iequals(point.weapon->projectile, projectile.id);
        }
        probes += 4;
        if (death != use.death.contains(projectile.id) || retreat != use.retreat.contains(projectile.id)
            || spawned != use.spawned.contains(projectile.id) || fired != use.fired.contains(projectile.id)) {
            ++bad;
            std::cerr << label << ": projectile usage of '" << projectile.id << "' differs\n";
        }
    }
    failures += static_cast<int>(bad);
    return probes;
}

inline std::string notes_digest(const UnitTables& tables) {
    std::string text;
    for (const auto* rows : {&tables.unresolved, &tables.notes}) {
        for (const auto& row : *rows) text += row.owner + '\t' + row.field + '\t' + row.value + '\t' + row.reason + '\n';
        text += "--\n";
    }
    return eawr::core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

// One line per loaded table; the same line from a build without the index is the old-vs-new comparison.
inline void report_tables(const UnitTables& tables, const std::string_view label) {
    const auto links = check_links(tables, label);
    const auto probes = check_find(tables, label) + check_projectile_use(tables, label);
    const auto identity = eawr::units::content_identity(tables);
    std::string hex;
    for (const auto byte : identity) {
        constexpr std::string_view digits = "0123456789abcdef";
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    std::cout << "unit-table index [" << label << "]: units=" << tables.units.size() << " projectiles=" << tables.projectiles.size()
              << " priority_sets=" << tables.priority_sets.size() << " links_checked=" << links << " find_probes=" << probes
              << " unresolved=" << tables.unresolved.size() << " notes=" << tables.notes.size()
              << " notes_sha256=" << notes_digest(tables) << " identity=" << hex << '\n';
}

} // namespace unit_tables_index_reference
