// Name-index contracts of the unit-table loader. The loader resolves every cross-table name (craft, squadron,
// projectile, priority set) and UnitTables::find resolves ids through a case-folded index. The reference below is the
// plain linear scan those lookups used to be; every link of a loaded table must equal what the scan gives, and a
// duplicate name must keep resolving to its FIRST row.

#include "../support/env.hpp"
#include "unit_tables_support.hpp"
#include "unit_tables_index_reference.hpp"

#include "eawr/core/diagnostic.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/core/text.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace unit_tables_test_support {
using namespace unit_tables_index_reference;

// Synthetic: the loaded fixture's links, then duplicate names edited into a copy.
void unit_name_index_contracts() {
    TempTree tree;
    write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
    auto loaded = load(tree.root, models(-40.0F));
    expect(loaded.tables.has_value(), "name-index fixture loads");
    if (!loaded.tables) return;
    const auto& tables = *loaded.tables;
    expect(tables.unit_names.built_for == tables.units.size() && tables.unit_names.first && !tables.unit_names.first->empty(),
        "the loader hands back a built name index");
    expect(check_links(tables, "synthetic") > tables.units.size(), "the fixture exercises real links");
    check_find(tables, "synthetic");
    check_projectile_use(tables, "synthetic");
    expect(tables.find("TEST_FRIGATE") == &tables.units[1] && tables.find("test_frigate") == &tables.units[1],
        "find folds case");

    // Duplicates keep the first row, in table order, however the second one is cased.
    auto duplicated = tables;
    auto later = duplicated.units[1];
    later.id = "TEST_FRIGATE";
    later.valid_target = !later.valid_target;
    duplicated.units.push_back(later);
    expect(duplicated.find("Test_Frigate") == ref_find(duplicated, "Test_Frigate") && duplicated.find("Test_Frigate") == &duplicated.units[1],
        "an appended duplicate leaves the first row winning (stale index falls back to the scan)");
    duplicated.rebuild_unit_name_index();
    expect(duplicated.find("TEST_frigate") == &duplicated.units[1] && duplicated.find("test_frigate")->valid_target == tables.units[1].valid_target,
        "a rebuilt index keeps the first duplicate");
    // A duplicate placed ahead of the original becomes the first row, as the scan says.
    auto fronted = tables;
    auto early = fronted.units[1];
    early.id = "test_FRIGATE";
    early.valid_target = !early.valid_target;
    fronted.units.insert(fronted.units.begin(), early);
    fronted.rebuild_unit_name_index();
    expect(fronted.find("Test_Frigate") == &fronted.units[0] && fronted.find("Test_Frigate") == ref_find(fronted, "Test_Frigate"),
        "a rebuilt index follows table order across duplicates");
    check_find(fronted, "fronted duplicate");
    // An in-place rename after the load must not leave find answering from the old name.
    auto renamed = tables;
    renamed.units[1].id = "Renamed_Frigate";
    expect(renamed.find("Renamed_Frigate") == &renamed.units[1] && renamed.find("Test_Frigate") == nullptr,
        "an in-place rename is seen by find");
    // Same-size edit (the hole of an unrebuilt index): the contract is to rebuild afterwards, and then the first match wins.
    // (Without the rebuild a debug build asserts in find(); a test cannot trigger that on purpose.)
    auto swapped = tables;
    swapped.units[0].id = "Test_Frigate";
    swapped.rebuild_unit_name_index();
    expect(swapped.find("test_frigate") == &swapped.units[0] && swapped.find("test_frigate") == ref_find(swapped, "test_frigate"),
        "a same-size rename onto a later name, once rebuilt, makes the earlier row the first match");
    check_find(swapped, "same-size rename");
    expect(duplicated == [&] { auto copy = duplicated; copy.unit_names = {}; return copy; }(),
        "the derived index never takes part in table equality");
}

} // namespace unit_tables_test_support
