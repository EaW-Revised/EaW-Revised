// #425 (P2-20b): the command bar's unit cards (docs/behaviour/foc-unit-cards.md). Squadron folding,
// ability grouping, stacking when the cards overflow the slots, bar levels, the column borders and
// what a card click leaves selected. The Godot half (drawing, the pointer) runs in the viewer
// (tests/presentation/renderer/test_tactical_hud.py and test_battle_input.py).

#include "eawr/presentation/ui/selection.hpp"
#include "eawr/presentation/ui/unit_cards.hpp"
#include "eawr/presentation/ui/battle_results.hpp"
#include "ui_test_support.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace eawr;
namespace ui = presentation::ui;
using test::ui::expect;

constexpr std::uint32_t defend = 1;
constexpr std::uint32_t turbo = 9;
constexpr std::uint32_t ion_cannon_shot = 41;
constexpr std::uint32_t spoiler_lock = 43;

ui::CardUnit unit(const sim::EntityId id, std::string type, const std::uint32_t ability, const double health = 1.0,
                  const std::optional<double> shield = std::nullopt) {
    ui::CardUnit result;
    result.id = id;
    result.members = {id};
    result.type = std::move(type);
    result.ability = ability;
    result.health = health;
    result.shield = shield;
    return result;
}

ui::CardUnit squadron(const sim::EntityId id, std::string type, const std::uint32_t ability,
                      std::vector<sim::EntityId> members, const double health = 1.0) {
    ui::CardUnit result = unit(id, std::move(type), ability, health);
    result.members = std::move(members);
    result.squadron = true;
    return result;
}

void test_ability_index() {
    expect(ui::ability_index("DEFEND") == defend, "DEFEND is UnitAbilityType 1");
    expect(ui::ability_index("power_to_weapons") == 29, "ability names match in any case (the Acclamator's)");
    expect(ui::ability_index("SPOILER_LOCK") == spoiler_lock, "SPOILER_LOCK is 43");
    expect(ui::ability_index("GARRISON_NONE") == 76, "the last ability is 76");
    expect(ui::ability_index("") == ui::ability_none, "no ability is none");
    expect(ui::ability_index("NOT_AN_ABILITY") == ui::ability_none, "an unknown ability is none");
}

void test_health_level() {
    expect(ui::health_level(1.0, 10) == 10, "full health is the top level");
    expect(ui::health_level(0.95, 10) == 10, "levels round up (ceil)");
    expect(ui::health_level(0.25, 10) == 3, "a quarter shows three of ten");
    expect(ui::health_level(0.01, 10) == 1, "any health shows one level");
    expect(ui::health_level(0.0, 10) == 0, "no health is level 0");
    expect(ui::health_level(-1.0, 10) == 0 && ui::health_level(2.0, 10) == 10, "levels clamp");
    expect(ui::health_level(std::numeric_limits<double>::quiet_NaN(), 10) == 0, "NaN is level 0");
}

void test_card_units_fold_squadrons() {
    ui::SquadronOf xwings{900, "Rebel_X-Wing_Squadron", spoiler_lock, true, {901, 902, 903}, 0.8};
    ui::SquadronOf mixed{950, "Mixed_Squadron", ui::ability_none, false, {951, 952}, 1.0};
    std::vector<ui::SelectedUnit> selection{
        {901, "X-Wing", spoiler_lock, 1.0, 0.5, xwings},
        {10, "Nebulon_B_Frigate", defend, 0.5, 0.25, std::nullopt},
        {902, "X-Wing", spoiler_lock, 0.6, 1.0, xwings},
        {951, "Y-Wing", ion_cannon_shot, 0.4, std::nullopt, mixed},
    };
    const auto cards = ui::card_units(selection);
    expect(cards.size() == 3, "two craft of one homogeneous squadron fold into one card");
    expect(cards[0].squadron && cards[0].id == 900 && cards[0].type == "Rebel_X-Wing_Squadron",
           "the squadron card stands where its first craft was selected");
    expect(cards[0].members == std::vector<sim::EntityId>{901, 902, 903}, "a squadron card selects its live craft");
    expect(std::abs(cards[0].health - 0.8) < 1e-9 && !cards[0].shield, "a squadron shows team health and no shield");
    expect(cards[1].id == 10 && cards[1].shield && std::abs(*cards[1].shield - 0.25) < 1e-9, "a ship keeps its shield");
    expect(!cards[2].squadron && cards[2].id == 951 && cards[2].type == "Y-Wing", "a mixed squadron's craft show as themselves");

    // WHE-SQ-01 / L-2: homogeneity is authored, not inferred from craft type names.
    ui::SquadronOf hero{960, "Hero_Squadron", ui::ability_index("REPLENISH_WINGMEN"), true,
        {961, 962, 963, 964, 965, 966, 967}, 0.75};
    std::vector<ui::SelectedUnit> hero_selection;
    for (const auto member : hero.members)
        hero_selection.push_back({member, member == 961 ? "Leader_Craft" : "Escort_Craft",
            ui::ability_none, 1.0, std::nullopt, hero});
    const auto hero_cards = ui::card_units(hero_selection);
    expect(hero_cards.size() == 1 && hero_cards.front().id == hero.container
        && hero_cards.front().members == hero.members && hero_cards.front().type == hero.type
        && hero_cards.front().ability == hero.ability && hero_cards.front().health == hero.health,
        "WHE-SQ-01: seven differently named craft retain one parent card, ability and team health");
}

void test_one_ship() {
    const std::vector<ui::CardUnit> units{unit(10, "Nebulon_B_Frigate", defend, 0.55, 0.3)};
    const auto layout = ui::layout_unit_cards(units, 24);
    expect(layout.cards.size() == 1 && layout.cards[0].slot == 0, "one ship fills the first slot");
    expect(!layout.cards[0].stacked && layout.cards[0].count == 1, "a single card is not stacked");
    expect(layout.cards[0].health_level == 6, "55 % health shows six of ten");
    expect(layout.cards[0].shield && std::abs(*layout.cards[0].shield - 0.3) < 1e-9, "the shield bar keeps its percent");
    expect(layout.borders.size() == 1 && layout.borders[0].column == 0 && layout.borders[0].piece == ui::BorderPiece::full,
           "a one-column group has the full border");
    expect(layout.groups == 1, "one ability group");
}

// The M2 Rebel fleet: groups follow UnitAbilityType order, each starting a new column.
void test_mixed_group() {
    const std::vector<ui::CardUnit> units{
        squadron(900, "Rebel_X-Wing_Squadron", spoiler_lock, {901, 902}),
        unit(10, "Nebulon_B_Frigate", defend),
        unit(11, "Corellian_Corvette", turbo),
        squadron(910, "Rebel_X-Wing_Squadron", spoiler_lock, {911}),
        squadron(920, "Y-Wing_Squadron", ion_cannon_shot, {921}),
    };
    const auto layout = ui::layout_unit_cards(units, 24);
    expect(layout.groups == 4, "four ability groups");
    expect(layout.cards.size() == 5, "five cards, none stacked");
    const auto slot_of = [&](const std::size_t unit_index) {
        for (const auto& card : layout.cards) if (card.unit == unit_index) return card.slot;
        return std::size_t{99};
    };
    expect(slot_of(1) == 0, "DEFEND (1) first");
    expect(slot_of(2) == 2, "TURBO (9) starts the next column");
    expect(slot_of(4) == 4, "ION_CANNON_SHOT (41) next");
    expect(slot_of(0) == 6 && slot_of(3) == 7, "SPOILER_LOCK (43) last, both squadrons in one column");
    expect(layout.borders.size() == 4, "one border per column");
    for (const auto& border : layout.borders) expect(border.piece == ui::BorderPiece::full, "one-column groups");
}

void test_mixed_types_share_ability_but_not_a_stack() {
    // The debug build's stacking compares the exact object type and ability status (CARD-2).
    // Retail XML gives both corvettes TURBO; Y-wings and B-wings are both bombers but have
    // ION_CANNON_SHOT and SPOILER_LOCK respectively.
    const std::vector<ui::CardUnit> mixed{
        unit(1, "Corellian_Corvette", turbo),
        unit(2, "Corellian_Gunboat", turbo),
        unit(3, "Corellian_Corvette", turbo),
        squadron(4, "Y-Wing_Squadron", ion_cannon_shot, {4}),
        squadron(5, "B-Wing_Squadron", spoiler_lock, {5}),
    };
    const auto layout = ui::layout_unit_cards(mixed, 24);
    expect(layout.groups == 3 && layout.cards.size() == 5, "shared class does not merge distinct ability groups or cards");
    expect(layout.by_ability[turbo] == std::vector<std::size_t>{0, 1, 2},
           "corvette and gunboat join the same TURBO group in selection order");
    expect(layout.cards[0].unit == 0 && layout.cards[1].unit == 2 && layout.cards[2].unit == 1,
           "different types stay in their own first-seen stacks within the TURBO group");
    expect(layout.cards[3].slot == 4 && layout.cards[4].slot == 6,
           "Y-wing and B-wing squadrons occupy different ability columns");
    expect(ui::card_click(layout, mixed, 2, false, std::vector<sim::EntityId>{1, 2, 3, 4, 5})
               == std::vector<sim::EntityId>{1, 2, 3},
           "an ordinary click on the gunboat selects the whole shared ability group");

    std::vector<ui::CardUnit> many;
    for (sim::EntityId id = 1; id <= 30; ++id) many.push_back(unit(id, "Corellian_Corvette", turbo));
    for (sim::EntityId id = 31; id <= 60; ++id) many.push_back(unit(id, "Corellian_Gunboat", turbo));
    many.push_back(squadron(61, "Y-Wing_Squadron", ion_cannon_shot, {61}));
    many.push_back(squadron(62, "B-Wing_Squadron", spoiler_lock, {62}));
    const auto stacked = ui::layout_unit_cards(many, 24);
    expect(stacked.cards.size() == 4 && stacked.cards[0].stacked && stacked.cards[0].count == 30
               && stacked.cards[1].stacked && stacked.cards[1].count == 30,
           "overflow collapses corvettes and gunboats into separate type stacks");
    std::vector<sim::EntityId> all;
    for (sim::EntityId id = 1; id <= 62; ++id) all.push_back(id);
    const auto corvettes = ui::card_click(stacked, many, 0, false, all);
    const auto gunboats = ui::card_click(stacked, many, 1, false, all);
    expect(corvettes && corvettes->size() == 30 && corvettes->front() == 1 && corvettes->back() == 30,
           "a collapsed corvette card selects corvettes only");
    expect(gunboats && gunboats->size() == 30 && gunboats->front() == 31 && gunboats->back() == 60,
           "a collapsed gunboat card selects gunboats only");
}

void test_stacking_when_the_slots_overflow() {
    std::vector<ui::CardUnit> units;
    for (sim::EntityId id = 1; id <= 30; ++id) units.push_back(unit(id, "TIE_Fighter", ui::ability_none));
    for (sim::EntityId id = 31; id <= 33; ++id) units.push_back(unit(id, "TIE_Bomber", ui::ability_none, 0.5));
    const auto layout = ui::layout_unit_cards(units, 24);
    expect(layout.cards.size() == 4, "33 units: the 30 fighters stack, the 3 bombers stay single");
    expect(layout.cards[0].stacked && layout.cards[0].count == 30 && layout.cards[0].unit == 0,
           "the stacked card counts its units and shows the first");
    expect(layout.cards[1].slot == 1 && !layout.cards[1].stacked && layout.cards[1].health_level == 5,
           "the bombers follow in the same group");
    expect(layout.borders.size() == 2 && layout.borders[0].piece == ui::BorderPiece::left
               && layout.borders[1].piece == ui::BorderPiece::right,
           "a two-column group has left and right border pieces");

    // A selection that fits stays unstacked.
    std::vector<ui::CardUnit> few(units.begin(), units.begin() + 24);
    const auto fits = ui::layout_unit_cards(few, 24);
    expect(fits.cards.size() == 24 && !fits.cards[23].stacked, "24 units fill the 24 slots unstacked");
    expect(fits.borders.size() == 12 && fits.borders[5].piece == ui::BorderPiece::centre, "inner columns are centre pieces");

    // Equal stacks: the first collapses first; the next one only while the cards still overflow.
    std::vector<ui::CardUnit> pairs;
    for (sim::EntityId id = 1; id <= 20; ++id) pairs.push_back(unit(id, "A", ui::ability_none));
    for (sim::EntityId id = 21; id <= 40; ++id) pairs.push_back(unit(id, "B", ui::ability_none));
    const auto equal = ui::layout_unit_cards(pairs, 24);
    expect(equal.cards.size() == 21 && equal.cards[0].stacked && equal.cards[0].count == 20 && !equal.cards[1].stacked,
           "of two equal stacks only the first collapses when that is enough");

    // Units of one type in different ability states never stack together.
    std::vector<ui::CardUnit> states;
    for (sim::EntityId id = 1; id <= 30; ++id) {
        states.push_back(unit(id, "A", ui::ability_none));
        states.back().ability_status = static_cast<std::uint8_t>(id % 2);
    }
    const auto split = ui::layout_unit_cards(states, 24);
    expect(split.cards.size() == 16 && split.cards[0].stacked && split.cards[0].count == 15 && !split.cards[1].stacked,
           "each ability state is its own stack: one collapsed stack of 15 is enough");

    // Nothing left to stack: the slots simply end.
    std::vector<ui::CardUnit> distinct;
    for (sim::EntityId id = 1; id <= 30; ++id) distinct.push_back(unit(id, "T" + std::to_string(id), ui::ability_none));
    const auto cut = ui::layout_unit_cards(distinct, 24);
    expect(cut.cards.size() == 24 && cut.cards.back().slot == 23, "distinct types beyond the slots are not shown");
}

void test_card_click() {
    const std::vector<ui::CardUnit> units{
        unit(10, "Nebulon_B_Frigate", defend),
        unit(11, "Corellian_Corvette", turbo),
        unit(12, "Corellian_Corvette", turbo),
        squadron(900, "Rebel_X-Wing_Squadron", spoiler_lock, {901, 902}),
    };
    const std::vector<sim::EntityId> selection{10, 11, 12, 901, 902};
    const auto layout = ui::layout_unit_cards(units, 24);
    const auto click = [&](const std::size_t slot, const bool shift) {
        return ui::card_click(layout, units, slot, shift, selection);
    };
    // Slots: Nebulon 0, corvettes 2 and 3, the squadron 4.
    expect(click(2, false) == std::vector<sim::EntityId>{11, 12},
           "with several groups a card selects its whole ability group");
    expect(click(4, false) == std::vector<sim::EntityId>{901, 902}, "a squadron card selects its craft");
    expect(click(3, true) == std::vector<sim::EntityId>{10, 11, 901, 902}, "Shift+click takes the unit out");
    expect(click(4, true) == std::vector<sim::EntityId>{10, 11, 12}, "Shift+click on a squadron takes its craft out");
    expect(!click(1, false) && !click(5, false), "a blank slot does nothing");

    const std::vector<ui::CardUnit> one_group{unit(11, "Corellian_Corvette", turbo), unit(12, "Corellian_Corvette", turbo)};
    const auto single = ui::layout_unit_cards(one_group, 24);
    const std::vector<sim::EntityId> both{11, 12};
    expect(ui::card_click(single, one_group, 1, false, both) == std::vector<sim::EntityId>{12},
           "with one ability group a card selects just its unit");

    std::vector<ui::CardUnit> many;
    std::vector<sim::EntityId> all;
    for (sim::EntityId id = 1; id <= 30; ++id) {
        many.push_back(unit(id, "TIE_Fighter", ui::ability_none));
        all.push_back(id);
    }
    many.push_back(unit(31, "TIE_Bomber", ui::ability_none));
    all.push_back(31);
    const auto stacked = ui::layout_unit_cards(many, 24);
    const auto fighters = ui::card_click(stacked, many, 0, false, all);
    expect(fighters && fighters->size() == 30 && fighters->front() == 1, "a stacked card selects every unit of its type");
    expect(ui::card_click(stacked, many, 0, true, all) == std::vector<sim::EntityId>{31},
           "Shift+click on a stacked card takes its whole type out");
}

void test_selection_replace() {
    ui::Selection selection;
    const std::vector<sim::EntityId> units{3, 1, 3, 2};
    expect(selection.replace(units), "replace changes an empty selection");
    expect(selection.units() == std::vector<sim::EntityId>{3, 1, 2}, "in order, without repeats");
    expect(!selection.replace(std::vector<sim::EntityId>{3, 1, 2}), "the same list is no change");
    expect(selection.replace({}) && selection.empty(), "an empty list clears it");
}

void test_battle_results() {
    namespace tactical = sim::tactical;
    const std::vector<tactical::SnapshotPlayer> players{{1, 10, false}, {2, 10, false}, {3, 20, false}, {4, 10, true}};
    std::vector<tactical::BattleLoss> losses{{1, 10, 3, 1, 0}, {1, 10, 3, 2, 1}, {2, 10, 3, 7, 1}, {3, 20, 1, 4, 2},
        {3, 99, 1, 8, 3}, {4, 10, 1, 5, 4}, {3, 30, 1, 1, 5}};
    const tactical::TacticalSnapshot snapshot(600, players, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {},
        std::make_shared<const std::vector<tactical::BattleLoss>>(std::move(losses)));
    const auto result = ui::battle_results(snapshot, 1, [](const tactical::TypeId type) {
        return ui::ResultType{std::to_string(type), type != 99, type == 30,
            sim::math::Fixed::from_raw(100 * sim::math::Fixed::scale), 2.0};
    });
    expect(result.losses[0] == std::vector<ui::ResultLoss>{{10, "10", 3}},
        "WBF-45: local losses merge by type, including earlier ticks without current events");
    expect(result.losses[1] == std::vector<ui::ResultLoss>{{20, "20", 4}}
        && result.heroes[1] == std::vector<ui::ResultLoss>{{30, "30", 1}},
        "WBF-45: named heroes use their own slots and zero-score craft do not occupy loss rows");
    expect(result.totals == std::array<std::uint64_t, 2>{3, 5},
        "WBF-45: allied and neutral losses are excluded and hero deaths remain in side totals");
    expect(result.score_cost == std::array<double, 2>{300.0, 500.0}
        && result.combat_power == std::array<double, 2>{6.0, 10.0},
        "WBF-45: every scored loss contributes authored cost and combat power, including heroes");
    expect(ui::battle_results(snapshot, 9, [](auto) { return ui::ResultType{}; }).totals[0] == 0,
        "an absent local player cannot classify losses");
    expect(ui::loss_scroll_max(0) == 0 && ui::loss_scroll_max(12) == 0 && ui::loss_scroll_max(15) == 3,
        "WBF-45: scrolling exposes overflow beyond the twelve visible entries");
    expect(ui::battle_time_text(1'234) == "00:00:01" && ui::battle_time_text(3'723'999) == "01:02:03",
        "WBF-44: milliseconds floor seconds and format hours, minutes and seconds");
    const tactical::TacticalSnapshot without_results(600, players, {} , {});
    expect(snapshot.canonical_bytes() == without_results.canonical_bytes(),
        "derived lifetime loss rows do not change canonical snapshot bytes");
}

} // namespace

int main() {
    test_ability_index();
    test_health_level();
    test_card_units_fold_squadrons();
    test_one_ship();
    test_mixed_group();
    test_mixed_types_share_ability_but_not_a_stack();
    test_stacking_when_the_slots_overflow();
    test_card_click();
    test_selection_replace();
    test_battle_results();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " unit card check(s) failed\n";
        return 1;
    }
    std::cout << "unit cards: all checks passed\n";
    return 0;
}
