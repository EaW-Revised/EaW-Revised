// #454: the command bar's ability buttons (docs/behaviour/foc-ability-buttons.md). Which buttons a card
// layout shows, their icon, disabled state, recharge dial and autofire mark, the card marks, what a
// click requests and the default hotkeys. The Godot half (drawing, the pointer) runs in the viewer
// (tests/presentation/renderer/test_tactical_hud.py and test_battle_input.py).

#include "eawr/presentation/ui/ability_buttons.hpp"
#include "ui_test_support.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace eawr;
namespace ui = presentation::ui;
using test::ui::expect;

constexpr std::uint32_t defend = 1;
constexpr std::uint32_t hunt = 6;
constexpr std::uint32_t turbo = 9;
constexpr std::uint32_t power_to_weapons = 29;
constexpr std::uint32_t ion_cannon_shot = 41;
constexpr std::uint32_t spoiler_lock = 43;

ui::CardUnit unit(const sim::EntityId id, std::string type, const std::uint32_t ability,
                  const std::uint32_t second = ui::ability_none) {
    ui::CardUnit result;
    result.id = id;
    result.members = {id};
    result.type = std::move(type);
    result.ability = ability;
    result.second_ability = second;
    return result;
}

// The M2 Rebel start box-selected (docs/behaviour/foc-unit-cards.md K-1) plus an Acclamator-like
// type with two abilities.
std::vector<ui::CardUnit> start() {
    return {unit(1, "Nebulon_B_Frigate", defend), unit(2, "Corellian_Corvette", turbo),
            unit(3, "Y-Wing_Squadron", ion_cannon_shot), unit(4, "X-Wing_Squadron", spoiler_lock),
            unit(5, "X-Wing_Squadron", spoiler_lock)};
}

[[nodiscard]] const ui::AbilityButton* find(const ui::AbilityBar& bar, const std::uint32_t ability) {
    for (const ui::AbilityButton& button : bar.buttons) {
        if (button.ability == ability) return &button;
    }
    return nullptr;
}

// AB-01, AB-02, AB-04, AB-05, AB-06.
void test_buttons() {
    const std::vector<ui::CardUnit> units = start();
    const ui::CardLayout layout = ui::layout_unit_cards(units, 24);
    ui::ReadyAbilities state;
    state.set_units(units);
    const ui::AbilityBar bar = ui::ability_bar(layout, units, state, nullptr);
    expect(bar.buttons.size() == 4, "one button per ability group");
    const ui::AbilityButton* shield = find(bar, defend);
    const ui::AbilityButton* foils = find(bar, spoiler_lock);
    expect(shield != nullptr && shield->component == 0 && shield->icon == "i_sa_defend_mode.tga" && !shield->shifted,
           "AB-01: a one-column group at column 0 takes special_button_00 with the engine icon");
    expect(foils != nullptr && foils->component == 6 && foils->units.size() == 2, "the X-wing group's column 3 gives 3 + 3");
    expect(find(bar, ion_cannon_shot) != nullptr && find(bar, ion_cannon_shot)->icon == "i_sa_ion_cannon_shot.tga",
           "AB-04: ION_CANNON_SHOT's icon");
    expect(shield != nullptr && !shield->disabled && shield->recharge == 1.0 && !shield->autofire,
           "a ready group: enabled, no dial, no autofire mark");
    expect(bar.marks.empty(), "ready units put no marks on their cards");

    state.stage({{4, spoiler_lock, {ui::AbilityStatus::recharging, 0.25, true}},
                 {5, spoiler_lock, {ui::AbilityStatus::recharging, 0.5, true}},
                 {1, defend, {ui::AbilityStatus::disabled, 1.0, false}},
                 {2, turbo, {ui::AbilityStatus::active, 1.0, false}}});
    const ui::AbilityBar staged = ui::ability_bar(layout, units, state, nullptr);
    foils = find(staged, spoiler_lock);
    expect(foils != nullptr && foils->recharge == 0.5 && foils->autofire, "AB-05, AB-06: the largest completion, all on autofire");
    expect(find(staged, defend) != nullptr && find(staged, defend)->disabled, "AB-02: every unit disabled disables the button");
    const auto mark = [&](const std::size_t slot) -> const ui::CardAbilityMark* {
        for (const ui::CardAbilityMark& candidate : staged.marks) {
            if (candidate.slot == slot) return &candidate;
        }
        return nullptr;
    };
    const ui::UnitCard* corvette = nullptr;
    for (const ui::UnitCard& card : layout.cards) {
        if (card.unit == 1) corvette = &card;
    }
    expect(corvette != nullptr && mark(corvette->slot) != nullptr && mark(corvette->slot)->icon == "i_sa_power_to_engines.tga"
               && !mark(corvette->slot)->dial,
           "AB-08: an active unit's card shows its ability icon");
    std::size_t recharging = 0;
    for (const ui::CardAbilityMark& candidate : staged.marks) {
        if (candidate.dial && candidate.autofire && !candidate.icon.empty()) ++recharging;
    }
    expect(recharging == 2, "AB-08: recharging cards show the icon, their dial and the autofire box");

    // AB-02: a unit without state for the ability hides its group's button.
    ui::ReadyAbilities partial;
    std::vector<ui::CardUnit> known = units;
    known.pop_back();
    partial.set_units(known);
    const ui::AbilityBar hidden = ui::ability_bar(layout, units, partial, nullptr);
    expect(find(hidden, spoiler_lock) == nullptr && hidden.buttons.size() == 3, "AB-02: a unit without state hides the button");
}

// AB-08 (#521): every M2 roster ability's card mark resolves a real icon in each state that draws
// one (active, recharging, and ready on autofire), never the bare autofire box the sheet-states eye
// check caught on the X-wing's SPOILER_LOCK.
void test_m2_roster_marks_resolve_icons() {
    constexpr std::array<std::uint32_t, 6> roster{defend, hunt, turbo, power_to_weapons, ion_cannon_shot, spoiler_lock};
    const std::array<ui::UnitAbilityState, 4> states{{{ui::AbilityStatus::active, 1.0, false},
                                                     {ui::AbilityStatus::active, 1.0, true},
                                                     {ui::AbilityStatus::recharging, 0.4, false},
                                                     {ui::AbilityStatus::ready, 1.0, true}}};
    for (const std::uint32_t ability : roster) {
        for (const ui::UnitAbilityState& drawn : states) {
            const std::vector<ui::CardUnit> units{unit(1, "Roster_Unit", ability)};
            const ui::CardLayout layout = ui::layout_unit_cards(units, 24);
            ui::ReadyAbilities state;
            state.set_units(units);
            state.stage({{1, ability, drawn}});
            const ui::AbilityBar bar = ui::ability_bar(layout, units, state, nullptr);
            const std::string message =
                "AB-08: " + std::string(ui::ability_name(ability)) + "'s card mark shows a real icon, not a bare box";
            expect(bar.marks.size() == 1 && !bar.marks.front().icon.empty(), message.c_str());
        }
    }
    // A ready ability with autofire off draws nothing on its card.
    const std::vector<ui::CardUnit> units{unit(1, "Roster_Unit", spoiler_lock)};
    ui::ReadyAbilities idle;
    idle.set_units(units);
    idle.stage({{1, spoiler_lock, {ui::AbilityStatus::ready, 1.0, false}}});
    expect(ui::ability_bar(ui::layout_unit_cards(units, 24), units, idle, nullptr).marks.empty(),
           "AB-08: a ready ability off autofire draws no card mark");
}

// AB-05, AB-08: a timed ability that is on draws its dial (the share of the duration left) on the
// button and the card; an untimed one does not.
void test_active_dial() {
    const std::vector<ui::CardUnit> units{unit(1, "Corvette", turbo), unit(2, "X-wing", spoiler_lock)};
    const ui::CardLayout layout = ui::layout_unit_cards(units, 24);
    ui::ReadyAbilities state;
    state.set_units(units);
    state.stage({{1, turbo, {ui::AbilityStatus::active, 0.4, false}}, {2, spoiler_lock, {ui::AbilityStatus::active, 1.0, false}}});
    const ui::AbilityBar bar = ui::ability_bar(layout, units, state, nullptr);
    const ui::AbilityButton* timed = find(bar, turbo);
    const ui::AbilityButton* untimed = find(bar, spoiler_lock);
    expect(timed != nullptr && timed->recharge == 0.4, "AB-05: an active timed ability's button draws its dial");
    expect(untimed != nullptr && untimed->recharge == 1.0, "AB-05: an active untimed ability's button draws no dial");
    std::size_t dials = 0;
    for (const ui::CardAbilityMark& mark : bar.marks) {
        if (mark.dial && *mark.dial == 0.4 && !mark.icon.empty()) ++dials;
        if (!mark.dial) expect(!mark.icon.empty(), "AB-08: the untimed active card shows its icon and no dial");
    }
    expect(dials == 1 && bar.marks.size() == 2, "AB-08: the timed active card shows its icon and its dial");
}

// AB-01: a second ability shifts both buttons and takes the next component.
void test_second_ability() {
    const std::vector<ui::CardUnit> units{unit(7, "Acclamator", power_to_weapons, turbo), unit(8, "Acclamator", power_to_weapons, turbo)};
    const ui::CardLayout layout = ui::layout_unit_cards(units, 24);
    ui::ReadyAbilities state;
    state.set_units(units);
    const ui::AbilityBar bar = ui::ability_bar(layout, units, state, [](const ui::CardUnit&, const bool second) {
        return std::string(second ? "custom_second.tga" : "");
    });
    expect(bar.buttons.size() == 2, "two buttons for a group with a second ability");
    if (bar.buttons.size() != 2) return;
    expect(bar.buttons[0].component == 0 && bar.buttons[1].component == 1 && bar.buttons[0].shifted && bar.buttons[1].shifted
               && bar.buttons[1].second,
           "the second ability takes the next component; both shift left");
    expect(bar.buttons[0].icon == "i_sa_power_to_weapons.tga" && bar.buttons[1].icon == "custom_second.tga",
           "AB-04: Alternate_Icon_Name wins over the engine table");
}

// AB-09.
void test_clicks() {
    const std::vector<ui::CardUnit> units = start();
    const ui::CardLayout layout = ui::layout_unit_cards(units, 24);
    ui::ReadyAbilities state;
    state.set_units(units);
    const ui::AbilityBar bar = ui::ability_bar(layout, units, state, nullptr);
    const auto shield = ui::ability_click(*find(bar, defend), false, state);
    expect(shield && shield->kind == ui::AbilityRequest::Kind::activate && !shield->targeted && shield->units == std::vector<sim::EntityId>{1},
           "a ready group activates");
    const auto ion = ui::ability_click(*find(bar, ion_cannon_shot), false, state);
    expect(ion && ion->targeted, "ION_CANNON_SHOT takes a target first");
    const auto autofire = ui::ability_click(*find(bar, spoiler_lock), true, state);
    expect(autofire && autofire->kind == ui::AbilityRequest::Kind::autofire_on && autofire->units.size() == 2,
           "a right click turns autofire on");
    state.stage({{4, spoiler_lock, {ui::AbilityStatus::active, 1.0, true}}, {5, spoiler_lock, {ui::AbilityStatus::active, 1.0, true}},
                 {2, turbo, {ui::AbilityStatus::recharging, 0.3, false}}});
    const ui::AbilityBar staged = ui::ability_bar(layout, units, state, nullptr);
    const auto off = ui::ability_click(*find(staged, spoiler_lock), false, state);
    expect(off && off->kind == ui::AbilityRequest::Kind::deactivate, "every unit active: the click switches it off");
    const auto autofire_off = ui::ability_click(*find(staged, spoiler_lock), true, state);
    expect(autofire_off && autofire_off->kind == ui::AbilityRequest::Kind::autofire_off, "every unit on autofire: off");
    expect(!ui::ability_click(*find(staged, turbo), false, state), "nothing ready: no request");
}

// AB-10.
void test_hotkeys() {
    expect(ui::ability_hotkey('O', {true, false, false}) == defend, "Shift+O: DEFEND");
    expect(ui::ability_hotkey('E', {true, false, false}) == turbo, "Shift+E: TURBO");
    expect(ui::ability_hotkey('W', {true, false, false}) == spoiler_lock, "Shift+W: SPOILER_LOCK");
    expect(ui::ability_hotkey('I', {true, false, false}) == ion_cannon_shot, "Shift+I: ION_CANNON_SHOT");
    expect(ui::ability_hotkey('B', {false, true, false}) == power_to_weapons, "Ctrl+B: POWER_TO_WEAPONS");
    expect(ui::ability_hotkey(']', {false, false, true}) == ui::ability_index("INVULNERABILITY"), "Alt+]: INVULNERABILITY");
    expect(!ui::ability_hotkey('O', {}) && !ui::ability_hotkey('O', {true, true, false}), "modifiers are exact");
    expect(!ui::ability_hotkey('S', {}), "plain S stays the stop key");
}

} // namespace

int main() {
    test_buttons();
    test_m2_roster_marks_resolve_icons();
    test_active_dial();
    test_second_ability();
    test_clicks();
    test_hotkeys();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " ability button check(s) failed\n";
        return 1;
    }
    std::cout << "ui ability buttons passed\n";
    return 0;
}
