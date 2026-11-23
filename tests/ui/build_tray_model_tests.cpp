// Galactic build tray grouping and row packing (docs/ui/galactic-build-tray.md, GBT-04 to GBT-06,
// GBT-12). Synthetic data only: each case reproduces one pitfall the real mod data shows (a hero
// ship that also carries a targeting name, a composite category, a company whose member is a
// structure dummy, a placeholder with only a marker name, a name the enum does not define) without
// reading any game file.

#include "eawr/presentation/ui/build_tray_model.hpp"
#include "ui_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {
namespace tray = eawr::presentation::ui::build_tray;
using eawr::test::ui::expect;

tray::CategoryEnum stock_like_enum() {
    return {{"Fighter", "Bomber", "Transport", "Corvette", "Frigate", "Capital", "Super", "Infantry", "Vehicle", "Air",
        "Structure", "AntiFighter", "AntiFrigate", "AntiCapital", "LandHero", "Wall", "Upgrade", "SpaceHero",
        "NonCombatHero", "Hero", "All", "None"}};
}

tray::GroupDefinition default_definition() {
    tray::GroupDefinition definition;
    definition.category_priority = {"SpaceHero", "LandHero", "NonCombatHero", "Hero", "Super", "Capital", "Frigate",
        "Corvette", "Transport", "Bomber", "Fighter", "Vehicle", "Infantry", "Air"};
    definition.building_categories = {"Structure", "SpaceStructure"};
    definition.captions = {{"Capital", "TEXT_CAPITAL"}, {"misc", "TEXT_MISC"}};
    return definition;
}

tray::TypeFacts type(std::string name, std::vector<std::string> categories, bool building_role = false,
    std::vector<std::string> members = {}) {
    tray::TypeFacts facts;
    facts.name = std::move(name);
    facts.categories = std::move(categories);
    facts.members = std::move(members);
    facts.building_role = building_role;
    return facts;
}

tray::TypeFacts classed(tray::TypeFacts facts, std::string key, std::string text = {}) {
    facts.encyclopedia_class = std::move(key);
    facts.class_text = std::move(text);
    return facts;
}

const tray::Group* find_group(const std::vector<tray::Group>& groups, const std::string& name) {
    for (const auto& group : groups) if (group.name == name) return &group;
    return nullptr;
}

// ---- GBT-04, GBT-05, GBT-12: classification ----------------------------------------------------------

void test_roles_and_categories() {
    expect(tray::is_building_behavior("dummy_orbital_structure") && tray::is_building_behavior("DUMMY_STAR_BASE")
            && tray::is_building_behavior("DUMMY_GROUND_BASE") && tray::is_building_behavior("DUMMY_GROUND_STRUCTURE"),
        "the four structure and base Behavior tags are building roles, case-insensitively");
    expect(!tray::is_building_behavior("DUMMY_GROUND_COMPANY") && !tray::is_building_behavior(""),
        "a company or an empty Behavior is not a building role");
    const std::vector<std::string> behaviors{"DUMMY_GROUND_COMPANY", "DUMMY_STAR_BASE"};
    expect(tray::has_building_behavior(behaviors), "one role among several marks the building");
}

void test_category_choice() {
    const auto categories = stock_like_enum();
    const auto definition = default_definition();
    const std::vector<std::string> hero_ship{"AntiFrigate", "SpaceHero"};
    expect(tray::choose_category(hero_ship, categories, definition) == std::optional<std::string>("SpaceHero"),
        "a hero ship with a targeting name is filed under the hero name, not the targeting name");
    const std::vector<std::string> targeting_only{"AntiCapital", "AntiFrigate"};
    expect(tray::choose_category(targeting_only, categories, definition) == std::optional<std::string>("AntiFrigate"),
        "names the list does not mention fall back to the enum order");
    const std::vector<std::string> sized{"antifrigate", "frigate"};
    expect(tray::choose_category(sized, categories, definition) == std::optional<std::string>("Frigate"),
        "the choice is case-insensitive and answers with the enum's spelling");
    const std::vector<std::string> ignored{"All", "None", "Gunship"};
    expect(!tray::choose_category(ignored, categories, definition),
        "All, None and a name the enum does not define are never a category");
    expect(!tray::choose_category({}, categories, definition), "no names, no category");
}

void test_member_union() {
    tray::TypeIndex index;
    index.add(type("X_Wing", {"Fighter", "AntiFrigate"}));
    index.add(type("Y_Wing", {"Bomber"}));
    index.add(type("Mixed_Squadron", {"Wall"}, false, {"X_Wing", "x_wing", "Y_Wing"}));
    index.add(type("Outer_Company", {}, false, {"Mixed_Squadron"}));
    index.add(type("Loop_A", {"Corvette"}, false, {"Loop_B"}));
    index.add(type("Loop_B", {"Frigate"}, false, {"Loop_A"}));
    index.add(type("Orphan_Company", {"Vehicle"}, false, {"Missing_Member"}));
    const auto squadron = index.categories_of("mixed_squadron");
    expect(squadron == std::vector<std::string>({"Fighter", "AntiFrigate", "Bomber"}),
        "a squadron takes the union of its members' names, once each, not its own");
    expect(index.categories_of("Outer_Company") == squadron, "a company of squadrons counts through them");
    expect(!index.categories_of("Loop_A").empty(), "member cycles end");
    expect(index.categories_of("Orphan_Company") == std::vector<std::string>({"Vehicle"}),
        "a company whose members are unknown keeps its own names");
    expect(index.categories_of("Not_There").empty(), "an unknown type has no names");
}

void test_classification() {
    tray::TypeIndex index;
    index.add(type("Fighter_Squadron", {}, false, {"Fighter_Craft"}));
    index.add(type("Fighter_Craft", {"Fighter", "AntiFrigate"}));
    index.add(type("Hero_Ship", {"AntiFrigate", "SpaceHero"}));
    index.add(type("Frigate_One", {"Frigate"}));
    index.add(type("Frigate_Two", {"Frigate"}));
    index.add(type("Gunship_Only", {"Gunship"}));
    index.add(type("Office_Company", {}, false, {"Office_Dummy"}));
    index.add(type("Office_Dummy", {"Structure"}));
    index.add(type("Barracks", {}, true));
    index.add(type("Menu_Button", {"Wall"}));
    index.add(type("Warlord_Choice_B", {"Wall"}));
    index.add(type("Warlord_Choice_A", {"Wall"}));
    index.add(type("Hero_Marker_Only", {"All"}));
    const std::vector<std::string> choices{"Fighter_Squadron", "Hero_Ship", "Frigate_One", "Gunship_Only",
        "Office_Company", "Barracks", "Menu_Button", "Warlord_Choice_B", "Warlord_Choice_A", "Hero_Marker_Only",
        "Frigate_Two", "Not_In_Index"};
    const auto categories = stock_like_enum();
    auto definition = default_definition();
    const auto groups = tray::classify(choices, index, categories, definition);

    std::vector<std::string> order;
    for (const auto& group : groups) order.push_back(group.name);
    expect(order == std::vector<std::string>({"Fighter", "Frigate", "Wall", "SpaceHero", "Misc", "Buildings"}),
        "category groups in enum order, then misc, then buildings last");

    const auto* fighters = find_group(groups, "Fighter");
    expect(fighters && fighters->choices == std::vector<std::size_t>({0}), "a squadron joins its craft's category");
    const auto* heroes = find_group(groups, "SpaceHero");
    expect(heroes && heroes->choices == std::vector<std::size_t>({1}), "the hero ship is a hero, not anti-frigate");
    const auto* frigates = find_group(groups, "Frigate");
    expect(frigates && frigates->choices == std::vector<std::size_t>({2, 10}), "choice list order inside a group");
    const auto* buildings = find_group(groups, "Buildings");
    expect(buildings && buildings->choices == std::vector<std::size_t>({4, 5}),
        "a structure role and an economy building sold as a company are buildings");
    const auto* misc = find_group(groups, "Misc");
    expect(misc && misc->choices == std::vector<std::size_t>({3, 9, 11}),
        "a name the enum does not define, All, and an unknown type go to misc");
    expect(misc && misc->caption_key == "TEXT_MISC", "the Misc caption is found case-insensitively");
    const auto* capital_less = find_group(groups, "Wall");
    expect(capital_less && capital_less->choices == std::vector<std::size_t>({6, 7, 8}),
        "placeholders with only a marker name form their own group");

    // Type assignments: an extra group, an exact name over a prefix, a sort key, and a role moved out.
    definition.extra_groups = {{"Warlord_Choices", "TEXT_WARLORDS", tray::ExtraGroup::Where::end, {}}};
    definition.assignments = {{"Warlord_Choice_*", "Warlord_Choices", 20}, {"Warlord_Choice_A", "Warlord_Choices", 10},
        {"Barracks", "Fighter", std::nullopt}};
    const auto moved = tray::classify(choices, index, categories, definition);
    order.clear();
    for (const auto& group : moved) order.push_back(group.name);
    expect(order == std::vector<std::string>({"Fighter", "Frigate", "Wall", "SpaceHero", "Warlord_Choices", "Misc", "Buildings"}),
        "an extra group at End follows the category groups and precedes misc");
    const auto* warlords = find_group(moved, "Warlord_Choices");
    expect(warlords && warlords->caption_key == "TEXT_WARLORDS" && warlords->choices == std::vector<std::size_t>({8, 7}),
        "the exact name (key 10) sorts before the prefix match (key 20)");
    expect(find_group(moved, "Fighter")->choices == std::vector<std::size_t>({0, 5}),
        "an assignment wins over a structure role");
    expect(find_group(moved, "Buildings")->choices == std::vector<std::size_t>({4}), "the rest of the buildings stay");

    // Positions of extra groups, and a longer prefix beating a shorter one.
    definition.extra_groups = {{"First", "", tray::ExtraGroup::Where::start, {}},
        {"Mid", "", tray::ExtraGroup::Where::after, "Frigate"}, {"Late", "", tray::ExtraGroup::Where::end, {}}};
    definition.assignments = {{"Warlord_*", "First", std::nullopt}, {"Warlord_Choice_*", "Mid", std::nullopt},
        {"Menu_Button", "Late", std::nullopt}, {"Hero_Ship", "First", std::nullopt}};
    const auto placed = tray::classify(choices, index, categories, definition);
    order.clear();
    for (const auto& group : placed) order.push_back(group.name);
    expect(order == std::vector<std::string>({"First", "Fighter", "Frigate", "Mid", "Late", "Misc", "Buildings"}),
        "After puts the group after its category, Start before all, and the longest prefix wins");

    // A mod's lists replace the default: no building categories means Structure stays a unit category.
    auto bare = definition;
    bare.assignments.clear();
    bare.extra_groups.clear();
    bare.building_categories.clear();
    const auto plain = tray::classify(choices, index, categories, bare);
    expect(find_group(plain, "Structure") && find_group(plain, "Structure")->choices == std::vector<std::size_t>({4}),
        "an empty building categories list leaves the choice in its category");
}

// GBT-05 (legacy #8160): a category group splits by the encyclopedia class its choices carry: one group per class in the
// order the classes first appear, headed by the class text; choices without a class stay in the category's own group
// after them; a group with one class (or none) stays whole; nothing outside a category group splits.
void test_a_category_group_splits_by_encyclopedia_class() {
    tray::TypeIndex index;
    index.add(classed(type("Speeder_A", {"Vehicle"}), "TEXT_CLASS_SPEEDER", "Speeder"));
    index.add(classed(type("Droid_A", {"Vehicle"}), "TEXT_CLASS_DROID", "Class: Droid"));
    index.add(classed(type("Speeder_B", {"Vehicle"}), "TEXT_CLASS_SPEEDER_ALT", "Class:  speeder "));
    index.add(classed(type("Tank_A", {"Vehicle"}), "TEXT_CLASS_TANK"));
    index.add(type("Plain_Vehicle", {"Vehicle"}));
    index.add(classed(type("Walker_Company", {}, false, {"Walker_Member"}), "TEXT_CLASS_WALKER", "Walker"));
    index.add(type("Walker_Member", {"Vehicle"}));
    index.add(classed(type("Squad", {"Infantry"}, false, {"Trooper"}), "", ""));
    index.add(classed(type("Trooper", {"Infantry"}), "TEXT_CLASS_INFANTRY", "Infantry"));
    index.add(classed(type("Trooper_Two", {"Infantry"}), "TEXT_CLASS_INFANTRY", "Infantry"));
    index.add(classed(type("Office", {"Structure"}, true), "TEXT_CLASS_STRUCTURE", "Structure"));
    index.add(classed(type("Barracks", {"Structure"}, true), "TEXT_CLASS_OTHER", "Other"));
    index.add(classed(type("Odd_Fighter", {"Fighter"}), "TEXT_CLASS_FIGHTER"));
    const std::vector<std::string> choices{"Speeder_A", "Droid_A", "Plain_Vehicle", "Speeder_B", "Tank_A", "Walker_Company",
        "Squad", "Trooper_Two", "Office", "Barracks", "Odd_Fighter"};
    const auto groups = tray::classify(choices, index, stock_like_enum(), default_definition());

    std::vector<std::string> shown;
    for (const auto& group : groups) {
        shown.push_back(group.name + (group.label.empty() ? "" : "/" + group.label));
    }
    expect(shown == std::vector<std::string>({"Fighter", "Infantry", "Vehicle/Speeder", "Vehicle/Droid",
                "Vehicle/TEXT_CLASS_TANK", "Vehicle/Walker", "Vehicle", "Buildings"}),
        "a category with several classes splits into one group per class in first-appearance order, the unclassed rest last; "
        "a class-less member of a one-class group and a lone class keep the group whole");
    const auto vehicle_classes = std::vector<const tray::Group*>{&groups[2], &groups[3], &groups[4], &groups[5], &groups[6]};
    expect(vehicle_classes[0]->choices == std::vector<std::size_t>({0, 3}) && vehicle_classes[0]->label == "Speeder",
        "two keys whose texts read the same (up to case and spaces after the colon) are one class; the header is the class text");
    expect(vehicle_classes[1]->choices == std::vector<std::size_t>({1}) && vehicle_classes[1]->label == "Droid",
        "the \"Class: \" prefix is dropped: the header keeps what follows the first colon");
    expect(vehicle_classes[2]->choices == std::vector<std::size_t>({4}) && vehicle_classes[2]->label == "TEXT_CLASS_TANK",
        "a class whose text the mod lacks shows its key");
    expect(vehicle_classes[3]->choices == std::vector<std::size_t>({5}) && vehicle_classes[3]->label == "Walker",
        "a company's own authored class splits like any other");
    expect(vehicle_classes[4]->choices == std::vector<std::size_t>({2}) && vehicle_classes[4]->label.empty(),
        "choices without a class stay in the category's own group, headed by its caption");
    const auto* infantry = find_group(groups, "Infantry");
    expect(infantry && infantry->label.empty() && infantry->choices == std::vector<std::size_t>({6, 7}),
        "a squadron takes its member's class; one class in the group, no split");
    const auto* buildings = find_group(groups, "Buildings");
    expect(buildings && buildings->choices == std::vector<std::size_t>({8, 9}) && buildings->label.empty(),
        "the buildings group never splits");
    expect(index.class_of("Squad").key == "TEXT_CLASS_INFANTRY" && index.class_of("Nothing").key.empty(),
        "class_of: own class, else a member's; an unknown type has none");

    // Extra groups and sort keys keep working next to a split group.
    auto definition = default_definition();
    definition.extra_groups = {{"Picks", "TEXT_PICKS", tray::ExtraGroup::Where::after, "Vehicle"}};
    definition.assignments = {{"Tank_A", "Picks", std::nullopt}};
    const auto with_extra = tray::classify(choices, index, stock_like_enum(), definition);
    std::vector<std::string> names;
    for (const auto& group : with_extra) names.push_back(group.name);
    expect(names == std::vector<std::string>({"Fighter", "Infantry", "Vehicle", "Vehicle", "Vehicle", "Vehicle", "Picks", "Buildings"}),
        "an extra group placed after a category comes after all of its split groups");
}

// GBT-05: a class group whose header reads the same as a category group's is merged with it, in tray order (the sort keys
// then put the keyed ones first as in any group); two category groups that read alike without a class group stay apart.
void test_a_class_group_merges_into_a_category_that_reads_the_same_in_tray_order() {
    const tray::CategoryEnum categories{{"Droid", "Vehicle"}};
    tray::TypeIndex index;
    index.add(classed(type("Type_A", {"Vehicle"}), "TEXT_CLASS_DROID", "Class: Droid"));
    index.add(type("Type_B", {"Droid"}));
    index.add(classed(type("Type_C", {"Vehicle"}), "TEXT_CLASS_DROID", "Class: Droid"));
    index.add(classed(type("Type_D", {"Vehicle"}), "TEXT_CLASS_WALKER", "Class: Walker"));
    const std::vector<std::string> choices{"Type_A", "Type_B", "Type_C", "Type_D"};
    tray::GroupDefinition definition;

    const auto merged = tray::classify(choices, index, categories, definition);
    std::vector<std::string> shown;
    for (const auto& group : merged) shown.push_back(group.name + (group.label.empty() ? "" : "/" + group.label));
    expect(shown == std::vector<std::string>({"Droid", "Vehicle/Walker"}),
        "the Droid class group of Vehicle is merged into the Droid category (the header reads the same), Walker stays");
    expect(merged[0].choices == std::vector<std::size_t>({0, 1, 2}),
        "the merged choices are in choice list order (the class group's 0 and 2 around the category's 1), not appended");

    // A sort key keeps first place in the merged group, as in any group.
    definition.assignments = {{"Type_C", "Droid", std::int64_t{7}}};
    const auto keyed = tray::classify(choices, index, categories, definition);
    expect(keyed[0].choices == std::vector<std::size_t>({2, 0, 1}), "a keyed entry of the merged group still comes first");

    // The caller's resolved header decides what reads the same: a category worded like the class header merges...
    const std::vector<std::pair<std::string, std::string>> headers{{"Droid", "Mechanical"}};
    const auto reworded = tray::classify(choices, index, categories, tray::GroupDefinition{}, headers);
    shown.clear();
    for (const auto& group : reworded) shown.push_back(group.name + (group.label.empty() ? "" : "/" + group.label));
    expect(shown == std::vector<std::string>({"Droid", "Vehicle/Droid", "Vehicle/Walker"}),
        "a category whose resolved header differs from the class text is not merged");

    // ...and without any class group, two categories that read alike keep their own groups (nothing changes for such data).
    tray::TypeIndex plain;
    plain.add(type("Plain_A", {"Vehicle"}));
    plain.add(type("Plain_B", {"Droid"}));
    const std::vector<std::string> plain_choices{"Plain_A", "Plain_B"};
    const std::vector<std::pair<std::string, std::string>> alike{{"Droid", "Same"}, {"Vehicle", "Same"}};
    const auto apart = tray::classify(plain_choices, plain, categories, tray::GroupDefinition{}, alike);
    expect(apart.size() == 2, "categories without a class group are never merged, however their headers read");
}

void test_derived_captions() {
    expect(tray::derived_caption("SuperCapital") == "Super capital", "camel case becomes words");
    expect(tray::derived_caption("Warlord_Choices") == "Warlord choices", "underscores become spaces");
    expect(tray::derived_caption("Wall") == "Wall" && tray::derived_caption("NonCombatHero") == "Non combat hero",
        "a single word stays and each further word is lower case");
}

void test_handle_label() {
    expect(tray::handle_label("+", 3) == "+3", "the caption is followed by the omitted count");
    expect(tray::handle_label("More: ", 12) == "More: 12", "a caption keeps its own spacing before a count");
    expect(tray::handle_label("+", 0) == "+", "a bar that omits nothing: the caption alone, no count");
    expect(tray::handle_label("More: ", 0) == "More:", "no count: the spaces that stood the caption apart from it go");
    expect(tray::handle_label("", 0).empty() && tray::handle_label("", 2) == "2", "an empty caption shows the count or nothing");
}

// ---- GBT-06: row packing ------------------------------------------------------------------------------

tray::Metrics metrics() {
    tray::Metrics value;
    value.entry_width = 10.0F;
    value.entry_height = 10.0F;
    value.header_height = 4.0F;
    value.entry_gap = 0.0F;
    value.group_gap = 4.0F;
    value.row_gap = 2.0F;
    value.padding = 0.0F;
    value.panel_width = 100.0F;
    return value;
}

std::vector<tray::Group> make_groups(const std::vector<std::pair<std::string, std::size_t>>& sizes,
    const std::size_t buildings = 0) {
    std::vector<tray::Group> groups;
    std::size_t next{};
    for (const auto& [name, count] : sizes) {
        tray::Group group;
        group.name = name;
        for (std::size_t index = 0; index < count; ++index) group.choices.push_back(next++);
        groups.push_back(std::move(group));
    }
    if (buildings) {
        tray::Group group;
        group.kind = tray::GroupKind::buildings;
        group.name = "Buildings";
        for (std::size_t index = 0; index < buildings; ++index) group.choices.push_back(next++);
        groups.push_back(std::move(group));
    }
    return groups;
}

void test_entries_per_line() {
    auto value = metrics();
    expect(tray::entries_per_line(value) == 10, "100 wide, 10 per entry: ten");
    value.padding = 5.0F;
    value.entry_gap = 2.0F;
    expect(tray::entries_per_line(value) == 7, "the padding and the gaps are counted");
    value.panel_width = 1.0F;
    expect(tray::entries_per_line(value) == 1, "never fewer than one entry per line");
}

void test_groups_share_rows_without_backfilling() {
    auto packed = tray::pack(make_groups({{"A", 3}, {"B", 4}, {"C", 3}}), metrics(), {});
    expect(packed.rows.size() == 2 && packed.unit_rows == 2 && !packed.misc_mode && packed.omitted == 0,
        "3 + 4 entries share a row, the third group starts a new one");
    expect(packed.rows[0].runs.size() == 2 && packed.rows[0].runs[1].x == 34.0F && packed.rows[1].runs.size() == 1,
        "the second group sits after the first plus the group gap");

    // Row 1 holds 8 (80 wide), so the 1-entry group would fit there (94), but row 2 is already current.
    packed = tray::pack(make_groups({{"A", 8}, {"B", 5}, {"C", 1}}), metrics(), {});
    expect(packed.rows.size() == 2 && packed.rows[0].runs.size() == 1 && packed.rows[1].runs.size() == 2,
        "an earlier row is never backfilled");
}

void test_a_wide_group_wraps_and_the_next_one_uses_the_rest() {
    const auto packed = tray::pack(make_groups({{"Wide", 25}, {"Next", 4}}), metrics(), {});
    expect(packed.rows.size() == 3, "25 entries make three lines of at most ten");
    expect(packed.rows[0].runs[0].header && !packed.rows[1].runs[0].header && !packed.rows[2].runs[0].header
            && packed.rows[0].runs[0].count == 10 && packed.rows[2].runs[0].count == 5,
        "only the first line has the header");
    expect(packed.rows[2].runs.size() == 2 && packed.rows[2].runs[1].group == 1 && packed.rows[2].runs[1].x == 54.0F,
        "the next group continues on what is left of the last line");
}

void test_buildings_take_their_own_rows_at_the_bottom() {
    const auto packed = tray::pack(make_groups({{"A", 3}}, 12), metrics(), {});
    expect(packed.rows.size() == 3 && packed.unit_rows == 1 && packed.building_rows == 2,
        "twelve buildings need two lines and never share a row with units");
    expect(packed.groups.back().kind == tray::GroupKind::buildings
            && packed.rows[1].runs[0].group == packed.groups.size() - 1,
        "the building runs refer to the buildings group");
    expect(packed.height == 3 * 14.0F + 2 * 2.0F, "height: rows of header plus entry, and the row gaps");
}

void test_misc_takes_the_last_rows_when_the_budget_is_exceeded() {
    // Four 6-entry groups plus two single choices need four rows; the budget is 4 in total with one
    // buildings row, so three unit rows.
    auto groups = make_groups({{"A", 6}, {"B", 6}, {"C", 6}, {"D", 6}, {"E", 1}, {"F", 1}}, 2);
    tray::PackOptions options;
    options.max_rows = 4;
    options.misc_caption_key = "TEXT_MISC";
    auto packed = tray::pack(groups, metrics(), options);
    expect(packed.misc_mode && packed.omitted == 0, "the groups do not fit, misc takes the overflow");
    expect(packed.rows.size() == 4 && packed.unit_rows == 3 && packed.building_rows == 1, "never more than the budget");
    expect(packed.groups.size() == 3 && packed.groups[0].name == "A" && packed.groups[1].kind == tray::GroupKind::misc
            && packed.groups[1].caption_key == "TEXT_MISC" && packed.groups[2].kind == tray::GroupKind::buildings,
        "the largest group stays (the first of equals), the rest goes to misc");
    expect(packed.groups[1].choices.size() == 20, "misc holds B, C, D and both single choices");

    // The single choices stay groups of their own when folding is switched off.
    options.fold_single_choice_groups = false;
    packed = tray::pack(groups, metrics(), options);
    expect(packed.misc_mode && packed.groups.size() >= 4 && packed.groups[0].name == "A" && packed.groups[1].name == "E"
            && packed.groups[2].name == "F" && packed.groups[3].kind == tray::GroupKind::misc,
        "without folding, one-choice groups that fit stay in the rows above misc");

    // A real misc group shares a row while everything fits.
    auto fitting = make_groups({{"A", 3}});
    tray::Group real_misc;
    real_misc.kind = tray::GroupKind::misc;
    real_misc.name = "Misc";
    real_misc.choices = {50, 51};
    fitting.push_back(real_misc);
    packed = tray::pack(fitting, metrics(), {});
    expect(!packed.misc_mode && packed.rows.size() == 1 && packed.rows[0].runs.size() == 2,
        "while everything fits, misc is placed like any other group");
}

void test_the_capacity_limit_is_reported() {
    // Thirty entries cannot fit one unit row of ten: the end of misc is cut and counted.
    tray::PackOptions options;
    options.max_rows = 2;
    const auto packed = tray::pack(make_groups({{"A", 10}, {"B", 10}, {"C", 10}}, 1), metrics(), options);
    expect(packed.misc_mode && packed.unit_rows == 1 && packed.building_rows == 1,
        "one unit row remains next to the buildings row");
    expect(packed.groups[0].kind == tray::GroupKind::misc && packed.groups[0].choices.size() == 10
            && packed.omitted == 20,
        "ten entries are kept and the twenty cut from the end of misc are counted");
}

void test_height_steps() {
    auto full = metrics();
    auto compact = metrics();
    compact.entry_width = 8.0F;
    compact.entry_height = 8.0F;
    compact.header_height = 3.0F;
    full.padding = compact.padding = 3.0F;
    full.panel_width = compact.panel_width = 106.0F;
    const auto groups = make_groups({{"A", 10}, {"B", 10}, {"C", 10}}, 3);
    tray::PackOptions options;

    auto fit = tray::fit_to_height(groups, full, compact, options, 100.0F);
    expect(!fit.compact && fit.packed.rows.size() == 4, "enough height: full size, all rows");
    fit = tray::fit_to_height(groups, full, compact, options, 60.0F);
    expect(fit.compact && fit.packed.rows.size() == 4 && fit.packed.height <= 60.0F,
        "short: the smaller size step first, with the same rows");
    fit = tray::fit_to_height(groups, full, compact, options, 40.0F);
    expect(fit.compact && fit.packed.rows.size() == 2 && fit.packed.building_rows == 1 && fit.packed.height <= 40.0F,
        "shorter: fewer unit rows, never fewer than the buildings plus one");
    fit = tray::fit_to_height(groups, full, compact, options, 5.0F);
    expect(fit.packed.rows.size() == 2 && fit.packed.building_rows == 1,
        "no height at all still keeps the buildings row and one unit row");
}

void test_empty_and_single_inputs() {
    const std::vector<std::string> none;
    const tray::TypeIndex no_types;
    const tray::CategoryEnum no_categories;
    const tray::GroupDefinition no_definition;
    expect(tray::classify(none, no_types, no_categories, no_definition).empty(), "no choices: no groups, not even misc");

    const auto empty = tray::pack({}, metrics(), {});
    expect(empty.rows.empty() && empty.groups.empty() && empty.unit_rows == 0 && empty.building_rows == 0
            && !empty.misc_mode && empty.omitted == 0 && empty.height == 0.0F,
        "no groups pack to zero rows of height 0 with no misc group");
    const auto fit = tray::fit_to_height({}, metrics(), metrics(), {}, 0.0F);
    expect(!fit.compact && fit.packed.rows.empty() && fit.packed.groups.empty() && fit.packed.height == 0.0F,
        "nothing to fit stays at full size with no rows");

    const auto single = tray::pack(make_groups({{"A", 1}}), metrics(), {});
    expect(single.rows.size() == 1 && single.rows[0].runs.size() == 1 && single.rows[0].runs[0].header
            && single.rows[0].runs[0].x == 0.0F && single.rows[0].runs[0].count == 1 && !single.misc_mode
            && single.height == 14.0F,
        "one choice in one group: one row, one run with its header at the left edge");

    const auto full_line = tray::pack(make_groups({{"A", 10}}), metrics(), {});
    expect(full_line.rows.size() == 1 && full_line.rows[0].runs.size() == 1 && full_line.rows[0].runs[0].count == 10
            && full_line.rows[0].runs[0].header,
        "a group of exactly one line of entries fills one row without wrapping");
}

// The Remake 4.0 + Holdouts + French Fried Taters stack (owner test stack, 2026-11-19): its category enum is the
// object enum (the top layer's 64-bit copy, names below in file order), frigates carry their category through
// Variant_Of_Existing_Type, squadrons only through their craft, and the mod's own menu entries carry none. The
// choices group by the mod's names; with the old 6-row budget at the bar's width (8 per line) GBT-06 folded every
// group but the largest into misc, which the owner saw as "everything in Other". With 7 rows and the automatic width
// every group keeps its place and misc holds only the menu entries.
void test_a_remake_shaped_stack() {
    const tray::CategoryEnum categories{{"Fighter", "Bomber", "Gunship", "Transport", "Corvette", "Frigate", "Capital",
        "Super", "SpaceStructure", "Regiment", "Infantry", "Droid", "Speeder", "Vehicle", "Air", "Structure", "AntiFighter",
        "AntiCorvette", "AntiFrigate", "AntiCapital", "AntiInfantry", "AntiVehicle", "AntiAir", "AntiStructure", "Wall",
        "Upgrade", "Research", "SpaceHero", "NonCombatHero", "ExtraCategoryMask03", "Hero", "All", "None"}};
    tray::GroupDefinition definition;
    definition.category_priority = {"SpaceHero", "LandHero", "InfantryHero", "VehicleHero", "NonCombatHero", "Hero",
        "SuperCapital", "Super", "Capital", "Frigate", "Corvette", "Gunship", "Transport", "Bomber", "Fighter", "Vehicle",
        "Infantry", "Air", "AirGunship", "AirSpeeder"};
    definition.building_categories = {"Structure", "SpaceStructure"};

    tray::TypeIndex index;
    std::vector<std::string> choices;
    const auto add = [&](const std::string& name, std::vector<std::string> names, std::vector<std::string> members = {}) {
        index.add(type(name, std::move(names), false, std::move(members)));
        choices.push_back(name);
    };
    index.add(type("TIE_Craft", {"Fighter", "AntiFighter"}));
    index.add(type("Skipray_Craft", {"Bomber", "AntiCorvette"}));
    index.add(type("Gozanti_Craft", {"Corvette", "AntiCorvette"}));
    index.add(type("Probe_Droid", {"NonCombatHero"}));
    for (int n = 0; n < 7; ++n) add("Fighter_Squadron_" + std::to_string(n), {}, {"TIE_Craft", "TIE_Craft", "TIE_Craft"});
    for (int n = 0; n < 15; ++n) add("Frigate_" + std::to_string(n), {"Frigate", n % 2 ? "AntiCapital" : "AntiFrigate"});
    for (int n = 0; n < 2; ++n) add("Bomber_Squadron_" + std::to_string(n), {}, {"Skipray_Craft", "Skipray_Craft"});
    for (int n = 0; n < 2; ++n) add("Corvette_Squadron_" + std::to_string(n), {}, {"Gozanti_Craft", "Gozanti_Craft"});
    add("Vigil_Corvette", {"Corvette", "AntiCorvette"});
    add("Lancer", {"Corvette", "AntiFighter"});
    add("Probe_Droid_Team", {}, {"Probe_Droid"});
    for (const auto* name : {"Filter_Buttons", "Unit_Previews", "Starcraft_Attributes", "Starcraft_Attributes2", "Attrition_Cost_E"})
        add(name, {});
    for (int n = 0; n < 12; ++n) add("Station_" + std::to_string(n), {n % 3 ? "Structure" : "SpaceStructure"});

    const auto groups = tray::classify(choices, index, categories, definition);
    std::vector<std::pair<std::string, std::size_t>> shape;
    for (const auto& group : groups) shape.emplace_back(group.name, group.choices.size());
    expect(shape == std::vector<std::pair<std::string, std::size_t>>({{"Fighter", 7}, {"Bomber", 2}, {"Corvette", 4},
               {"Frigate", 15}, {"NonCombatHero", 1}, {"Misc", 5}, {"Buildings", 12}}),
        "the stack's own category names group every choice that carries one, in the enum's order; misc holds the menu entries");

    auto size = metrics(); // 10 per line at 100
    size.panel_width = 80.0F; // the bar's width: 8 per line
    tray::PackOptions six;
    six.max_rows = 6;
    const auto old = tray::pack(groups, size, six);
    expect(old.misc_mode && old.groups.size() < groups.size(),
        "the old budget at the bar's width folds groups into misc (the owner's picture)");

    tray::PackOptions seven;
    seven.max_rows = 7;
    const float width = tray::automatic_width(groups, size, seven, 80.0F, 400.0F, 10000.0F);
    size.panel_width = width;
    const auto packed = tray::pack(groups, size, seven);
    expect(!packed.misc_mode && packed.groups.size() == 7 && packed.rows.size() <= 7 && packed.omitted == 0,
        "7 rows and the automatic width keep every group in its own place");
}

} // namespace

int main() {
    test_roles_and_categories();
    test_category_choice();
    test_member_union();
    test_classification();
    test_a_category_group_splits_by_encyclopedia_class();
    test_a_class_group_merges_into_a_category_that_reads_the_same_in_tray_order();
    test_derived_captions();
    test_handle_label();
    test_entries_per_line();
    test_groups_share_rows_without_backfilling();
    test_a_wide_group_wraps_and_the_next_one_uses_the_rest();
    test_buildings_take_their_own_rows_at_the_bottom();
    test_misc_takes_the_last_rows_when_the_budget_is_exceeded();
    test_the_capacity_limit_is_reported();
    test_height_steps();
    test_empty_and_single_inputs();
    test_a_remake_shaped_stack();
    if (eawr::test::ui::failures() != 0) {
        std::cerr << eawr::test::ui::failures() << " build tray model check(s) failed\n";
        return 1;
    }
    std::cout << "build tray model: all checks passed\n";
    return 0;
}
