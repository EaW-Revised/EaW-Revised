// #424: FoC's battle UI in the world (docs/behaviour/foc-battle-world-ui.md): the selection circle,
// the bar sizes, scale, levels, colours and visibility rules, the squadron icon's health and the
// hardpoint reticle art and tint. The Godot half draws them (tests/presentation/renderer/
// test_battle_input.py).

#include "eawr/presentation/ui/world_ui.hpp"
#include "ui_test_support.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <vector>

namespace {
using namespace eawr;
namespace ui = presentation::ui;
using test::ui::expect;

bool close_to(const float a, const float b) { return std::abs(a - b) <= 1.0e-3F; }

void ability_overlays() {
    const auto foils = ui::ability_index("SPOILER_LOCK");
    const auto defend = ui::ability_index("DEFEND");
    const auto turbo = ui::ability_index("TURBO");
    struct State final : ui::AbilityState {
        ui::UnitAbilityState value;
        bool present{true};
        mutable std::size_t queries{};
        std::optional<ui::UnitAbilityState> state(sim::EntityId, std::uint32_t) const override {
            ++queries;
            return present ? std::optional<ui::UnitAbilityState>(value) : std::nullopt;
        }
    } state;
    ui::WorldOverlayUnit unit{2, {foils, turbo}, true, true, true, true, false};
    for (const auto status : {ui::AbilityStatus::ready, ui::AbilityStatus::active,
                              ui::AbilityStatus::recharging, ui::AbilityStatus::disabled}) {
        for (const bool autofire : {false, true}) {
            state.value = {status, 0.5, autofire};
            auto shown = ui::world_ability_overlays(unit, state);
            expect(shown[0] == (status == ui::AbilityStatus::active ? foils : ui::ability_none)
                       && shown[1] == ui::ability_none,
                   "WSU-33: only active, including timed active, shows; autofire alone does not");
        }
    }
    state.value.status = ui::AbilityStatus::active;
    unit.abilities[0] = ui::ability_none;
    expect(ui::world_ability_overlays(unit, state)[1] == turbo, "an active second slot substitutes on a squadron icon");
    unit = {6, {defend, turbo}, true, true, true, false, true};
    const auto both = ui::world_ability_overlays(unit, state);
    expect(both[0] == defend && both[1] == turbo, "WU-44: a bracket can show both active slots");
    for (int hidden = 0; hidden < 4; ++hidden) {
        auto copy = unit;
        if (hidden == 0) copy.ally = false;
        if (hidden == 1) copy.visible = false;
        if (hidden == 2) copy.on_screen = false;
        if (hidden == 3) copy.bracket = false;
        state.queries = 0;
        expect(ui::world_ability_overlays(copy, state) == std::array<std::uint32_t, 2>{},
               "enemy, fogged, off-screen or hidden bracket draws no ability");
        expect(state.queries == 0, "hidden overlays do no state work");
        copy.squadron = true;
        if (hidden != 3) {
            expect(ui::world_ability_overlays(copy, state) == std::array<std::uint32_t, 2>{},
                   "enemy, fogged and off-screen squadron also draws no ability");
        }
    }
    state.present = false;
    expect(ui::world_ability_overlays(unit, state) == std::array<std::uint32_t, 2>{}, "a missing ability state draws nothing");
    state.present = true;
    state.queries = 0;
    static_cast<void>(ui::world_ability_overlays(unit, state));
    expect(state.queries == 2, "world overlay state work is bounded to the two authored slots");
    expect(close_to(ui::bracket_ability_y(100.0F, 4.0F, 24.0F, 1.0F), 83.0F),
           "WU-44: icon bottom is three reference pixels above the bar top");
    // WU-43 / #983: retail st_grab_bar Lower_Effect_Offset 0 30, native 26px art,
    // independent of the frame's 0.6 scale; the 30px squadron identity remains uncovered.
    for (const float scale : {1.0F, 720.0F / 768.0F, 2.0F}) {
        const auto rect = ui::world_ability_rect({510.0F, 221.0F}, {26.0F, 26.0F}, {0.0F, 30.0F}, 1.0F, scale);
        expect(close_to(rect.x + rect.width * 0.5F, 510.0F)
                   && close_to(rect.y + rect.height * 0.5F, 221.0F - 30.0F * scale)
                   && close_to(rect.width, 26.0F * scale) && close_to(rect.height, 26.0F * scale),
               "WU-43: lower effect keeps native size and authored reference offset at every viewport scale");
        expect(rect.y + rect.height < 221.0F - 15.0F * scale,
               "#983: ability art lies entirely above the squadron identity");
    }
    const auto modded = ui::world_ability_rect({100.0F, 200.0F}, {20.0F, 12.0F}, {7.0F, -9.0F}, 1.0F, 2.0F);
    expect(close_to(modded.x, 94.0F) && close_to(modded.y, 206.0F)
               && close_to(modded.width, 40.0F) && close_to(modded.height, 24.0F),
           "WU-43: mod-authored lower effect offset is applied with +Y up, never a fixed 30px literal");
}

void group_numbers() {
    ui::Selection selection;
    const std::array<sim::EntityId, 2> units{2, 6};
    selection.replace(units);
    selection.assign_group(3); // The battle input's Ctrl+3 path.
    expect(selection.group_of(2) == 3 && selection.group_of(6) == 3,
           "#768: Ctrl+3 labels both a squadron icon and a frigate bracket with 3");
    expect(!selection.group_of(4), "WSU-33, WSU-55: an ungrouped unit has no number");
    const std::array<sim::EntityId, 1> frigate{6};
    selection.replace(frigate);
    selection.assign_group(0);
    expect(selection.group_of(6) == 0 && selection.group_of(2) == 3, "0 is a visible number; reassignment moves only its units");
    selection.assign_group(3);
    expect(!selection.group_of(2) && selection.group_of(6) == 3, "replacing a group clears its old members' numbers");
    const std::array<sim::EntityId, 1> squadron{2};
    selection.replace(squadron);
    selection.add_to_group(3, 1.0, {});
    expect(selection.group_of(2) == 3 && selection.group_of(6) == 3, "Alt+3 updates the same membership index");
    selection.retain(frigate);
    expect(!selection.group_of(2) && selection.group_of(6) == 3, "a lost squadron leaves no stale group number");
}

void circles() {
    // WU-02: Nebulon-B (Select_Box_Scale 300, Scale_Factor 0.7) and a fighter (70, 0.7).
    const auto nebulon = ui::selection_circle_side(300.0F, 0.7F);
    expect(nebulon && close_to(*nebulon, 210.0F), "the Nebulon-B circle is 210 units across");
    const auto fighter = ui::selection_circle_side(70.0F, 0.7F);
    expect(fighter && close_to(*fighter, 49.0F), "a fighter's circle is 49 units across");
    expect(!ui::selection_circle_side(0.0F, 1.0F), "no Select_Box_Scale, no circle (a squadron container)");
}

void bars() {
    expect(ui::bar_size("fighter", std::nullopt) == ui::BarSize::small, "a fighter has the small bars");
    expect(ui::bar_size("bomber", std::nullopt) == ui::BarSize::small, "a bomber has the small bars");
    expect(ui::bar_size("corvette", std::nullopt) == ui::BarSize::medium, "a corvette has the medium bars");
    expect(ui::bar_size("frigate", std::nullopt) == ui::BarSize::large, "a frigate has the large bars");
    expect(ui::bar_size("capital_ship", std::nullopt) == ui::BarSize::large, "a capital ship has the large bars");
    expect(ui::bar_size("", std::nullopt) == ui::BarSize::medium, "a station without a class has the medium bars");
    expect(ui::bar_size("frigate", 0) == ui::BarSize::small, "GUI_Bracket_Size wins");
    expect(close_to(ui::bar_width(ui::BarSize::large), 102.0F) && close_to(ui::bar_width(ui::BarSize::small), 34.0F),
           "the bar widths are the atlas widths");

    expect(close_to(ui::bar_scale(3000.0F), 1.0F), "far away the bar keeps its size");
    expect(close_to(ui::bar_scale(750.0F), 2.0F), "at half Health_Bar_Scale it doubles");

    expect(ui::bar_level(1.0F) == 10 && ui::bar_level(0.0F) == 0, "full and empty levels");
    expect(ui::bar_level(0.91F) == 10 && ui::bar_level(0.9F) == 9, "the level is the ceiling of ten times the fraction");
    expect(ui::bar_level(0.01F) == 1, "any health shows level 1");
    expect(ui::health_bar_colour(10) == ui::Rgb{0, 255, 0}, "full health is green");
    expect(ui::health_bar_colour(5) == ui::Rgb{255, 147, 0}, "half health is orange");
    expect(ui::health_bar_colour(3) == ui::Rgb{255, 0, 0}, "a third is red");

    ui::BarUnit ship;
    ship.has_health = true;
    ship.health = 1.0F;
    ship.shielded = true;
    expect(!ui::bar_visibility(ship).health, "an unselected, unhovered, healthy ship shows no bars");
    ship.selected = true;
    auto shown = ui::bar_visibility(ship);
    expect(shown.health && shown.shield, "a selected ship shows its shield and health bars");
    ship.selected = false;
    ship.hovered = true;
    shown = ui::bar_visibility(ship);
    expect(shown.health && shown.shield, "a hovered ship shows them");
    ship.hovered = false;
    ship.health = 0.05F;
    expect(ui::bar_visibility(ship).health, "a ship below 10 % health shows them");
    ship.fogged = true;
    expect(!ui::bar_visibility(ship).health, "a fogged ship shows none");

    ui::BarUnit craft;
    craft.has_health = true;
    craft.health = 1.0F;
    craft.shielded = true;
    craft.squadron_member = true;
    craft.selected = true;
    expect(!ui::bar_visibility(craft).health, "a craft of a selected squadron shows no bar of its own");
    craft.selected = false;
    craft.hovered = true;
    shown = ui::bar_visibility(craft);
    expect(shown.health && !shown.shield, "a hovered craft shows its own health bar, never a shield bar");
    craft.hovered = false;
    expect(!ui::bar_visibility(craft).health, "#502: the pointer over the squadron icon shows no craft bars");

    // WU-18: a 10 x 20 x 30 half box seen from straight above (camera up +Y) lifts by 20.
    expect(close_to(ui::bar_anchor_lift({10.0F, 20.0F, 30.0F}, {0.0F, 1.0F, 0.0F}, 1.0F), 20.0F),
           "the bars sit at the bounds' top along the camera's up");
    expect(close_to(ui::bar_anchor_lift({3.0F, 4.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, 0.5F), 2.5F),
           "another GUI_Bounds_Scale scales the bounds' half diagonal");
    expect(close_to(ui::squadron_health(150.0F, 200.0F), 0.75F), "a squadron's health is its craft's sum over their maximum");

    // WU-24 (#500): the icon's Y offset is 4.8 % of the screen height, so it hovers below the
    // squadron's projected centre instead of covering it (never over 5 % even at a tall viewport).
    expect(close_to(ui::squadron_icon_screen_offset(720.0F), 34.56F), "the offset is 0.048 of the screen height");
    expect(close_to(ui::squadron_icon_screen_offset(1080.0F), 51.84F), "it scales with the viewport, not a fixed pixel count");
    expect(ui::squadron_icon_screen_offset(720.0F) > 0.0F, "it moves the icon down the screen (+Y), never up or in place");
}

// WU-16 (#435 review, #502): FoC's candidates are the hovered unit and the selection only, so
// critical health alone never gives an unhovered, unselected unit its bars, and hovering a
// squadron's icon (never a candidate itself: bar_candidate has no such input any more) shows none
// of its craft.
void candidates() {
    expect(!ui::bar_candidate(false, false), "an unhovered, unselected unit is no bar candidate");
    expect(ui::bar_candidate(true, false), "the hovered unit is a candidate");
    expect(ui::bar_candidate(false, true), "a selected unit is a candidate");
    ui::BarUnit dying;
    dying.has_health = true;
    dying.health = 0.05F;
    expect(ui::bar_visibility(dying).health, "a candidate below 10 % shows its health bar");

    // #502 (owner): hovering the squadron icon must not draw a bar over every member craft.
    ui::BarUnit craft;
    craft.has_health = true;
    craft.health = 1.0F;
    craft.squadron_member = true;
    expect(!ui::bar_visibility(craft).health, "an unhovered, unselected squadron craft shows no bar");
}

// WU-25 to WU-27: the combat grid and the dogfight icon layout.
void dogfight_grid() {
    // WSU-34: old-speed stepping, velocity cap, and braking in either held grid.
    ui::SquadronIconAnchor moving{{0.0F, 0.0F, 0.0F}, 0.0F, false};
    ui::slide_squadron_icon(moving, {100.0F, 0.0F, 0.0F}, 2.0F, 3.0F, false, false);
    expect(moving.position[0] == 0.0F && moving.speed == 2.0F, "the first render frame accelerates without moving");
    ui::slide_squadron_icon(moving, {100.0F, 0.0F, 0.0F}, 2.0F, 3.0F, false, false);
    expect(moving.position[0] == 2.0F && moving.speed == 3.0F, "move with the old speed then cap to member velocity");
    moving = {{0.0F, 0.0F, 0.0F}, 10.0F, false};
    ui::slide_squadron_icon(moving, {20.0F, 0.0F, 0.0F}, 2.0F, 20.0F, true, false);
    expect(moving.position[0] == 10.0F && moving.speed == 8.0F, "idle and combat grid anchors brake within stopping distance");
    ui::slide_squadron_icon(moving, {11.0F, 0.0F, 0.0F}, 2.0F, 20.0F, true, false);
    expect(moving.position[0] == 11.0F, "within one old-speed step the anchor lands");
    ui::slide_squadron_icon(moving, {100.0F, 0.0F, 0.0F}, 2.0F, 20.0F, false, true);
    expect(moving.position[0] == 100.0F, "fast forward places the anchor at the desired point");

    // WSU-36: world-distance admission, then fixed placement even after a camera move.
    ui::SquadronIconAnchor joining{{0.0F, 0.0F, 0.0F}, 5.0F, false};
    expect(!ui::settle_squadron_icon(joining, {35.01F, 0.0F, 0.0F}, 35.0F, false)
               && joining.position[0] == 0.0F, "a distant newly occupied cell keeps its sliding icon");
    expect(ui::settle_squadron_icon(joining, {35.0F, 0.0F, 0.0F}, 35.0F, false), "the snap threshold is inclusive");
    expect(ui::settle_squadron_icon(joining, {500.0F, 0.0F, 0.0F}, 35.0F, true)
               && joining.position[0] == 500.0F, "an admitted icon follows its slot immediately when the camera changes");
    expect(!ui::settle_squadron_icon(joining, {600.0F, 0.0F, 0.0F}, 35.0F, false), "a new cell requires admission again");

    // WSU-60: independent snapping keeps atlas sampling fixed within a pixel.
    const auto frame_a = ui::squadron_icon_rect({100.1F, 200.1F}, 33.75F);
    const auto frame_b = ui::squadron_icon_rect({100.8F, 200.8F}, 33.75F);
    expect(frame_a.x == frame_b.x && frame_a.y == frame_b.y && frame_a.width == 33.75F,
           "fractional movement within a raster pixel keeps the identity quad stable");
    const auto inner = ui::squadron_icon_rect({100.1F, 200.1F}, 28.125F);
    expect(inner.x == 86.0F && inner.y == 187.0F && inner.width == 28.125F, "the inner icon snaps separately and keeps its scaled extent");
    const auto unsnapped = ui::squadron_icon_rect({100.1F, 200.1F}, 33.75F, false);
    expect(close_to(unsnapped.x, 83.225F) && unsnapped.width == 33.75F, "Pixel_Align false retains fractional placement");
    expect(ui::combat_cell_of({100.0F, 100.0F}, {0.0F, 0.0F}) == ui::CombatCell{0, 0}, "a point in the first cell");
    expect(ui::combat_cell_of({100.0F, 500.0F}, {0.0F, 0.0F}) == ui::CombatCell{-1, 1}, "an odd row is shifted half a cell");
    const auto centre = ui::combat_cell_point({0, 0}, {0.0F, 0.0F});
    expect(close_to(centre[0], 200.0F) && close_to(centre[1], 200.0F), "a cell's point is its centre");
    const auto odd = ui::combat_cell_point({-1, 1}, {0.0F, 0.0F});
    expect(close_to(odd[0], 0.0F) && close_to(odd[1], 600.0F), "an odd row's point is half a cell further along x");

    const auto lone = ui::combat_grid_slot(0, 1);
    expect(close_to(lone[0], -15.0F) && close_to(lone[1], 0.0F), "one icon: half a step left of the cell point");
    const auto second = ui::combat_grid_slot(1, 2);
    expect(close_to(second[0], 0.0F) && close_to(second[1], 0.0F), "two icons share one row");
    const auto fourth = ui::combat_grid_slot(3, 5);
    const auto fifth = ui::combat_grid_slot(4, 5);
    expect(close_to(fourth[0], -45.0F) && close_to(fourth[1], 30.0F) && close_to(fifth[0], -15.0F),
           "five icons: three columns, the second row below the first");

    // #457: the sim's records: joined squadrons hold their cell in order; a record alone draws nothing.
    {
        ui::CombatGrid sim_cells;
        const std::array<ui::CombatGrid::Record, 3> records{{{10, {1, 2}, true}, {20, {1, 2}, true}, {30, {4, 4}, false}}};
        sim_cells.adopt(records);
        expect(sim_cells.cells().size() == 1 && sim_cells.cells().front().squadrons == std::vector<sim::EntityId>{10, 20},
               "adopted: two joined squadrons share one cell in ID order");
        expect(!sim_cells.cell_of(30) && sim_cells.record_of(30) == ui::CombatCell{4, 4},
               "adopted: a squadron that only records a cell is not drawn in it");
        sim_cells.adopt({});
        expect(sim_cells.cells().empty() && !sim_cells.record_of(10), "adopted: an empty frame clears the grid");
    }
    using Fight = ui::CombatGrid::Fight;
    constexpr std::array<float, 2> origin{0.0F, 0.0F};
    constexpr auto none = sim::invalid_entity_id;
    ui::CombatGrid grid;
    // Squadron 10 closes on squadron 20 from beyond its strafe reach: no cell (FA-01).
    grid.update(std::vector<Fight>{{10, 20, {100.0F, 100.0F}, true}}, origin);
    expect(!grid.record_of(10) && !grid.cell_of(10), "a squadron closing from beyond its strafe reach holds no cell");
    // Inside the reach, while its target records no cell, it records the searched cell nearest the
    // target (100, 100) without joining it, so its icon stays over itself.
    grid.update(std::vector<Fight>{{10, 20, {100.0F, 100.0F}, false}}, origin);
    expect(grid.record_of(10) == ui::CombatCell{0, 0}, "the attacker records the cell nearest its target");
    expect(!grid.cell_of(10) && grid.cells().empty(), "a recorded cell is not joined");
    // Squadron 20 fights back: it joins the cell 10 records; the next frame 10 joins 20's. Both
    // rejoin every frame, so the cell lists them in service order. A squadron shooting a ship does
    // not dogfight.
    const std::vector<Fight> both{{10, 20, {100.0F, 100.0F}, false}, {20, 10, {150.0F, 150.0F}, false},
        {30, none, {0.0F, 0.0F}, false}};
    grid.update(both, origin);
    expect(grid.cells().size() == 1 && grid.cells()[0].squadrons == std::vector<sim::EntityId>{20},
           "the target joins the cell its attacker records");
    grid.update(both, origin);
    expect(grid.cells().size() == 1 && grid.cells()[0].squadrons == std::vector<sim::EntityId>{10, 20},
           "the two dogfighting squadrons share one cell, in service order");
    expect(!grid.cell_of(30) && !grid.record_of(30), "a squadron attacking a ship holds no cell");
    // A pair next door searches its nine cells: the joined cell (0, 0) scores 0 and beats the
    // nearer free cell (1, 0), whose point (600, 200) is almost on the target.
    const std::vector<Fight> neighbours{{10, 20, {100.0F, 100.0F}, false}, {20, 10, {150.0F, 150.0F}, false},
        {40, 50, {590.0F, 210.0F}, false}};
    grid.update(neighbours, origin);
    expect(grid.record_of(40) == ui::CombatCell{0, 0}, "a joined cell wins the search over a nearer free one");
    // A shared cell keeps a squadron in the dogfight even while its leader is beyond the reach.
    grid.update(std::vector<Fight>{{10, 20, {100.0F, 100.0F}, true}, {20, 10, {150.0F, 150.0F}, false}}, origin);
    expect(grid.cell_of(10) == ui::CombatCell{0, 0}, "a squadron sharing its target's cell stays beyond the reach");
    // Squadron 20 stops: it leaves and forgets its cell; 10, closing with nothing shared, leaves too.
    grid.update(std::vector<Fight>{{10, 20, {100.0F, 100.0F}, true}}, origin);
    expect(!grid.record_of(20) && !grid.cell_of(10) && grid.cells().empty(), "a squadron that stops leaves its cell");
    // A one-sided attack: 10 records nothing, so 20 records its search's cell and no grid is drawn.
    grid.update(std::vector<Fight>{{20, 10, {150.0F, 150.0F}, false}}, origin);
    expect(!grid.cell_of(20) && grid.record_of(20) == ui::CombatCell{0, 0}, "a one-sided attack draws no grid");
    // Equal scores: the first cell in row order from the low corner wins.
    const ui::CombatGrid tie;
    expect(tie.search({400.0F, 200.0F}, origin) == ui::CombatCell{0, 0}, "a tie goes to the first cell in row order");
}

void stable_icon_grid() {
    const ui::CombatCell cell{1, 2};
    ui::CombatIconGrid grid;
    std::vector<ui::CombatIconGrid::Cell> cells{{cell, {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110}, 10.0F}};
    grid.update(cells);
    std::vector<std::array<float, 2>> initial;
    for (const auto squadron : cells.front().squadrons) initial.push_back(grid.position(cell, squadron).value().offset);
    expect(initial.front() == std::array<float, 2>{-60.0F, 0.0F}
        && initial.back() == std::array<float, 2>{0.0F, 60.0F},
        "stable grid: eleven icons occupy four columns at the retail pitch");

    // Crossing the retail sqrt boundary (11 -> 9) must not shift a surviving
    // slot, including when the first squadron's different-height craft leave.
    cells.front().squadrons = {20, 30, 40, 60, 70, 80, 90, 100, 110};
    cells.front().height = 99.0F;
    grid.update(cells);
    expect(!grid.position(cell, 10) && !grid.position(cell, 50), "stable grid: departing icons are not drawn");
    for (const auto squadron : cells.front().squadrons) {
        const auto position = grid.position(cell, squadron).value();
        expect(position.offset == initial[static_cast<std::size_t>(squadron / 10 - 1)] && position.height == 10.0F,
               "stable grid: leave retains every other offset and the initial anchor height");
    }
    cells.front().squadrons.insert(cells.front().squadrons.begin(), 50);
    cells.front().squadrons.push_back(10);
    grid.update(cells);
    expect(grid.position(cell, 10).value().offset == initial[0]
        && grid.position(cell, 50).value().offset == initial[4],
        "stable grid: a returning owner recovers its vacancy despite changed service order");

    // Lower-ID newcomers may fill vacancies but never steal an existing slot.
    std::erase(cells.front().squadrons, sim::EntityId{10});
    cells.front().squadrons.insert(cells.front().squadrons.begin(), 5);
    grid.update(cells);
    expect(grid.position(cell, 5).value().offset == initial[0] && !grid.position(cell, 10)
        && grid.position(cell, 110).value().offset == initial[10],
        "stable grid: a newcomer fills a vacancy without moving later-ID owners");
    for (sim::EntityId newcomer = 1000; newcomer < 1100; ++newcomer) {
        cells.front().squadrons.front() = newcomer;
        grid.update(cells);
        expect(grid.position(cell, newcomer).value().offset == initial[0],
               "stable grid: repeated churn reuses slots rather than extending the layout");
    }
    cells.push_back({{3, 4}, {200, 210}, 30.0F});
    grid.update(cells);
    expect(grid.position({3, 4}, 200).value().offset == initial[0]
        && grid.position({3, 4}, 200).value().height == 30.0F
        && grid.position(cell, 110).value().height == 10.0F,
        "stable grid: independent cells keep independent anchor lifetimes");
    cells.front().squadrons.clear();
    grid.update(cells);
    expect(!grid.position(cell, 20), "stable grid: an empty cell ends the slot lifetime");
    cells.front().squadrons = {110, 20};
    cells.front().height = 20.0F;
    grid.update(cells);
    expect(grid.position(cell, 110).value().offset == initial[0] && grid.position(cell, 110).value().height == 20.0F,
        "stable grid: a new dogfight starts compactly with its own initial height");
    grid.update({});
    expect(!grid.position(cell, 110) && !grid.position({3, 4}, 200), "stable grid: no held cells releases all slots");
}

void reticles() {
    expect(ui::hardpoint_reticle_tint(1.0F, false) == ui::Rgb{32, 255, 32}, "a healthy hardpoint is green");
    expect(ui::hardpoint_reticle_tint(0.5F, false) == ui::Rgb{255, 255, 32}, "a damaged one is yellow");
    expect(ui::hardpoint_reticle_tint(0.2F, false) == ui::Rgb{255, 32, 32}, "a badly damaged one is red");
    expect(ui::hardpoint_reticle_tint(1.0F, true) == ui::Rgb{128, 128, 128}, "a disabled one is grey");
    expect(ui::hardpoint_reticle_texture("HARD_POINT_WEAPON_LASER") == "i_hard_point_reticle_weapons", "a laser's art");
    expect(ui::hardpoint_reticle_texture("HARD_POINT_ENGINE") == "i_hard_point_reticle_engines", "an engine's art");
    expect(ui::hardpoint_reticle_texture("HARD_POINT_FIGHTER_BAY") == "i_hard_point_reticle_docking_bay", "a bay's art");
    expect(ui::hardpoint_reticle_texture("HARD_POINT_SHIELD_GENERATOR") == "i_hard_point_reticle_shield_gen",
           "a shield generator's art");
    expect(ui::hardpoint_reticle_texture("NOT_A_TYPE").empty(), "an unknown type has no reticle");
}

// A pinhole camera on the -X side of the origin looking along +X (screen right is +Y, screen down
// is -Z), `distance` units away, with a vertical field of view of 50 degrees.
std::array<float, 2> project(const std::array<float, 3>& point, const float distance,
                             const std::array<float, 2>& viewport) {
    const float depth = point[0] + distance;
    const float focal = 0.5F * viewport[1] / std::tan(25.0F * 3.14159265F / 180.0F);
    return {0.5F * viewport[0] + focal * point[1] / depth, 0.5F * viewport[1] - focal * point[2] / depth};
}

void reticle_size() {
    // WU-31 (#515): the reticle is 0.03 of the screen wide and 0.04 of it high at every camera
    // distance, so a corvette seen from far away shrinks under reticles that do not.
    for (const std::array<float, 2> viewport : {std::array<float, 2>{1920.0F, 1080.0F}, std::array<float, 2>{1280.0F, 720.0F},
                                                std::array<float, 2>{1434.0F, 946.0F}}) {
        float previous_hull = 0.0F;
        for (const float distance : {300.0F, 1000.0F, 3000.0F, 9000.0F}) {
            // A corvette-sized hull, 150 units long, and a hardpoint 40 units off its centre.
            const auto bow = project({0.0F, 75.0F, 0.0F}, distance, viewport);
            const auto stern = project({0.0F, -75.0F, 0.0F}, distance, viewport);
            const float hull = bow[0] - stern[0];
            if (previous_hull > 0.0F) expect(hull < previous_hull, "the hull shrinks as the camera backs off");
            previous_hull = hull;
            const auto centre = project({0.0F, 40.0F, 10.0F}, distance, viewport);
            const ui::ReticleRect rect = ui::hardpoint_reticle_rect(centre, viewport);
            expect(close_to(rect.width, 0.03F * viewport[0]), "the width is 0.03 of the screen width");
            expect(close_to(rect.height, 0.04F * viewport[1]), "the height is 0.04 of the screen height");
            expect(close_to(rect.x + rect.width * 0.5F, centre[0]) && close_to(rect.y + rect.height * 0.5F, centre[1]),
                   "the reticle is centred on the hardpoint's screen point");
        }
    }
    const ui::ReticleRect hd = ui::hardpoint_reticle_rect({960.0F, 540.0F}, {1920.0F, 1080.0F});
    expect(close_to(hd.width, 57.6F) && close_to(hd.height, 43.2F), "57.6 x 43.2 pixels at 1920 x 1080");
}

void reticle_anchor() {
    const std::array<float, 3> ship{100.0F, 200.0F, 50.0F};
    // WU-34: yaw only turns the attachment point about +Z.
    const auto turned = ui::hardpoint_reticle_anchor(ship, 90.0F, 0.0F, 0.0F, {10.0F, 0.0F, 5.0F});
    expect(close_to(turned[0], 100.0F) && close_to(turned[1], 210.0F) && close_to(turned[2], 55.0F),
           "a quarter yaw takes forward to +Y");
    // A 90 degree bank lifts a point on the ship's left (+Y) to straight above it.
    const auto banked = ui::hardpoint_reticle_anchor(ship, 0.0F, 0.0F, 90.0F, {0.0F, 20.0F, 0.0F});
    expect(close_to(banked[0], 100.0F) && close_to(banked[1], 200.0F) && close_to(banked[2], 70.0F),
           "a bank rolls the attachment point with the hull");
    // A 30 degree pitch (R-ROT-01 sends +X to (cp, 0, -sp)) moves the bow down.
    const auto pitched = ui::hardpoint_reticle_anchor(ship, 0.0F, 30.0F, 0.0F, {40.0F, 0.0F, 0.0F});
    expect(close_to(pitched[0], 100.0F + 40.0F * std::cos(0.5235988F)) && close_to(pitched[2], 50.0F - 20.0F),
           "a pitch tilts the attachment point with the hull");
    // All three, against the rotation matrix's columns.
    const float yaw = 30.0F * 3.14159265F / 180.0F;
    const float pitch = 20.0F * 3.14159265F / 180.0F;
    const float roll = 15.0F * 3.14159265F / 180.0F;
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll), sr = std::sin(roll);
    const auto left = ui::hardpoint_reticle_anchor({0.0F, 0.0F, 0.0F}, 30.0F, 20.0F, 15.0F, {0.0F, 1.0F, 0.0F});
    expect(close_to(left[0], cy * sp * sr - sy * cr) && close_to(left[1], sy * sp * sr + cy * cr)
               && close_to(left[2], cp * sr),
           "the ship's +Y goes where the model's rotation sends it");
}

} // namespace

int main() {
    ui::SquadronIconAnchor arrival_anchor{{-3300.0F, 20.0F, -90.0F}, 12.0F, false};
    const std::array<float, 3> landing{300.0F, 20.0F, -90.0F};
    for (const float flight_x : {-3300.0F, -2000.5F, -100.25F, 300.0F}) {
        expect(ui::place_squadron_arrival_icon(arrival_anchor, landing, {flight_x, 20.0F, -90.0F}),
               "WU-49: arrival owns the icon despite zero ordinary craft velocity");
        expect(arrival_anchor.position == landing && arrival_anchor.speed == 0.0F,
               "WU-49: the icon waits at the destination while the craft approach");
    }
    const std::array<float, 3> formation{301.25F, 24.5F, -90.0F};
    expect(ui::place_squadron_arrival_icon(arrival_anchor, std::nullopt, formation)
               && arrival_anchor.position == formation && arrival_anchor.speed == 0.0F,
           "WU-49: landing hands off at the presented formation without residual slide");
    expect(!ui::place_squadron_arrival_icon(arrival_anchor, std::nullopt, {400.0F, 24.5F, -90.0F})
               && arrival_anchor.position == formation,
           "WU-49: subsequent movement belongs to ordinary smoothing");
    expect(ui::hero_world_identity(true, false), "WU-47: a named space hero needs no explicit head tag");
    expect(ui::hero_world_identity(false, true), "WU-47: an explicit head admits an unnamed hero");
    expect(!ui::hero_world_identity(false, false), "WU-47: ordinary ships and generic identity alone get no head");
    ability_overlays();
    group_numbers();
    circles();
    bars();
    candidates();
    dogfight_grid();
    stable_icon_grid();
    reticles();
    reticle_size();
    reticle_anchor();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " world UI contract(s) failed\n";
        return 1;
    }
    std::cout << "world UI contracts passed\n";
    return 0;
}
