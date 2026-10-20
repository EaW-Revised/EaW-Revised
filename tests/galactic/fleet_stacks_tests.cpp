#include "galactic/fleet_stacks.hpp"
#include "galactic/fleet_member_placement.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <map>
#include <limits>
#include <vector>

using namespace eawr::viewer::galactic;
using eawr::sim::galactic::ObservedFleet;
using eawr::sim::galactic::ObservedFleetGroup;

namespace {
ObservedFleetGroup group(const std::size_t count, const eawr::sim::galactic::TypeId type,
    const eawr::sim::galactic::FleetId fleet) {
    ObservedFleetGroup row;
    row.count = count; row.type = type; row.fleet = fleet; row.representative = type * 10U;
    return row;
}
ObservedFleet local_card(const eawr::sim::galactic::FleetId fleet, const std::uint8_t slot,
    std::vector<ObservedFleetGroup> groups) {
    ObservedFleet card;
    card.fleet = fleet; card.planet = 101; card.owner = 1; card.orbit_slot = slot; card.local = true;
    for (const auto& row : groups) card.count += row.count;
    card.groups = std::move(groups);
    return card;
}
std::vector<const ObservedFleet*> view(const std::vector<ObservedFleet>& cards) {
    std::vector<const ObservedFleet*> result;
    for (const auto& card : cards) result.push_back(&card);
    return result;
}
}

int case_galactic_fleet_stacks_tests() {
    {
        // GHUD-S97: whole/Ctrl stacks vacate; an ordinary member retains the
        // remainder only when the authored component requests ghost art.
        assert((fleet_drag_counts(9, false, true) == std::pair<std::size_t, std::size_t>{9, 0}));
        assert((fleet_drag_counts(9, true, true) == std::pair<std::size_t, std::size_t>{1, 8}));
        assert((fleet_drag_counts(2, true, true) == std::pair<std::size_t, std::size_t>{1, 1}));
        assert((fleet_drag_counts(1, true, true) == std::pair<std::size_t, std::size_t>{1, 0}));
        assert((fleet_drag_counts(9, true, false) == std::pair<std::size_t, std::size_t>{1, 0}));
        assert((fleet_drag_counts(0, true, true) == std::pair<std::size_t, std::size_t>{0, 0}));
    }
    {
        // GHUD-S87: renamed synthetic types, saved source and destination order.
        namespace ui = eawr::presentation::ui;
        using eawr::sim::EntityId;
        const std::vector<std::optional<ui::FleetCellCoordinate>> cells{
            ui::FleetCellCoordinate{0,0}, ui::FleetCellCoordinate{0,1}, ui::FleetCellCoordinate{0,2},
            ui::FleetCellCoordinate{0,3}, ui::FleetCellCoordinate{1,0}, ui::FleetCellCoordinate{1,1},
            ui::FleetCellCoordinate{1,2}, ui::FleetCellCoordinate{1,3}};
        const std::vector<std::optional<eawr::sim::galactic::TypeId>> types{471, 902, 471};
        const std::vector<ui::FleetCellIcon> kept{{26,26,2,false}, {26,26,3,false}, {26,26,6,false}};
        const ui::FleetCellIcon moving{26,26,{},false};
        const auto join = fleet_member_cell_drop(cells, types, kept, moving, 471, 2);
        assert(join && join->first == 2 && join->second);
        const std::vector<EntityId> incoming{81,82,83}, matching{91,92};
        std::map<EntityId, std::int64_t> indices{{81,0},{82,4},{83,5},{91,2},{92,6},{93,3},{84,0}};
        remember_fleet_member_drop(indices, incoming, matching, *join);
        for (const auto id : {81U,82U,83U,91U,92U}) assert(indices.at(id) == 2);
        assert(indices.at(93) == 3 && indices.at(84) == 0); // other type and source remainder
        namespace gc = eawr::sim::galactic;
        gc::ObserverSetup observer; observer.local_player = 1;
        observer.players = {{1,false,false,0,0,{}}};
        observer.planets = {{10,1,1,1,1}};
        auto observations = std::make_shared<std::vector<gc::ObservedPlanet>>();
        gc::ObservedPlanet seen; seen.player = 1; seen.planet = 10;
        observations->push_back(seen);
        const gc::GalacticSnapshot snapshot(0, {}, {{1,1,0}}, {{10,{},1}}, {},
            {{11,1,10,{{81,471},{82,471},{83,471},{91,471},{92,471},{93,902}},0},
             {12,1,10,{{84,471}},1}}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, observer, {}, {}, {}, observations);
        const auto bytes = snapshot.canonical_bytes();
        const std::vector<std::pair<EntityId,std::int64_t>> remembered(indices.begin(), indices.end());
        const std::array<gc::FleetDisplayVisibility,1> sight{{{10,true}}};
        const auto projected = gc::observed_fleets(snapshot, 1, {}, 8, sight, {}, remembered);
        assert(projected.cards.size() == 2 && projected.cards[0].groups.size() == 2);
        assert(projected.cards[0].groups[0].type == 471 && projected.cards[0].groups[0].count == 5
            && projected.cards[0].groups[0].gui_index == 2);
        assert(projected.cards[0].groups[1].type == 902 && projected.cards[0].groups[1].gui_index == 3);
        assert(projected.cards[1].groups[0].count == 1 && projected.cards[1].groups[0].gui_index == 0);
        assert(snapshot.canonical_bytes() == bytes);
        const auto free = fleet_member_cell_drop(cells, types, kept, moving, 471, 7);
        assert(free && free->first == 7 && free->second); // explicit safe cell gathers destination type
        const auto split = fleet_member_cell_drop(cells, types, kept, moving, 471, 3);
        assert(split && split->first == 1 && !split->second); // unlike type: below, then above
        indices = {{81,0},{91,2},{92,6},{93,3},{84,0}};
        const std::vector<EntityId> single{81};
        remember_fleet_member_drop(indices, single, matching, *split);
        assert(indices.at(81) == 1 && indices.at(91) == 2 && indices.at(92) == 6 && indices.at(84) == 0);
        const std::vector<ui::FleetCellIcon> wide{{52,26,0,false}};
        const std::vector<std::optional<eawr::sim::galactic::TypeId>> wide_type{471};
        const auto anchor = fleet_member_cell_drop(cells, wide_type, wide, wide.front(), 471, 0);
        assert(anchor && anchor->first == 0 && anchor->second);
        const auto neighbour = fleet_member_cell_drop(cells, wide_type, wide, moving, 471, 1);
        assert(neighbour && neighbour->first == 2 && !neighbour->second); // same-type footprint is reserved
        // The right half of a visible wide stack is its native receive button,
        // not an explicit drop onto the hidden reserved authored neighbour.
        const std::array<ui::PixelRect,3> buttons{{{100,200,52,26},{},{152,200,26,26}}};
        const auto hit = fleet_member_target_cell(1, buttons, {139,213});
        assert(hit == 0);
        const auto linked_join = fleet_member_cell_drop(cells, wide_type, wide, wide.front(), 471, hit);
        assert(linked_join && linked_join->first == 0 && linked_join->second);
        assert(fleet_member_target_cell(2, buttons, {165,213}) == 2); // other visible button
        assert(fleet_member_target_cell(3, buttons, {191,213}) == 3); // empty authored cell
        assert(fleet_member_target_cell(1, {}, {139,213}) == 1); // hidden linked button cannot receive
        assert(!fleet_member_cell_drop(cells, types, kept, moving, 471, cells.size()));
        const auto empty = fleet_member_cell_drop(cells, {}, {}, moving, 471, 5);
        assert(empty && empty->first == 5 && empty->second);
    }
    // Ranking neither quantizes tiny authored values nor clamps large ones.
    // Integer comparisons agree with native finite binary32 comparisons.
    const std::array<float, 8> values{-std::numeric_limits<float>::max(), -1.0F, -0.0F, 0.0F,
        std::numeric_limits<float>::denorm_min(), 1.0F, 1.0e20F, std::numeric_limits<float>::max()};
    for (const auto first : values) for (const auto second : values) {
        const auto left = stack_health_rank(first), right = stack_health_rank(second);
        assert(left && right);
        assert((*left < *right) == (first < second));
        assert((*left == *right) == (first == second));
    }
    assert(!stack_health_rank(std::nullopt));
    assert(!stack_health_rank(std::numeric_limits<float>::quiet_NaN()));
    assert(!stack_health_rank(std::numeric_limits<float>::infinity()));
    constexpr std::size_t textures = 6; // fleet_empty plus fleet_stacks_01..05
    const std::map<eawr::sim::galactic::TypeId, float> hull{{1000, 9000.0F}, {1002, 2500.0F}, {1003, 40000.0F}};
    const StackHealth health = [&](const eawr::sim::galactic::TypeId type) -> std::optional<float> {
        const auto found = hull.find(type);
        return found == hull.end() ? std::nullopt : std::optional<float>(found->second);
    };

    // WGUI-92: ceiling of count / 5, capped at the last texture.
    assert(stack_texture_index(1, textures) == 1 && stack_texture_index(5, textures) == 1);
    assert(stack_texture_index(6, textures) == 2 && stack_texture_index(25, textures) == 5);
    assert(stack_texture_index(26, textures) == 5 && stack_texture_index(1000, textures) == 5);
    assert(stack_texture_index(0, textures) == 0 && stack_texture_index(3, 0) == 0);

    {
        // Local only: slot s fills icon s. Slot 0 holds two fleets (merged card),
        // slot 1 holds two fleets of which only the first draws.
        std::vector<ObservedFleet> cards;
        cards.push_back(local_card(601, 0, {group(3, 1000, 601), group(1, 1003, 601), group(2, 1002, 605)}));
        cards.push_back(local_card(602, 1, {group(2, 1002, 602), group(4, 1003, 610)}));
        cards.push_back(local_card(603, 2, {group(1, 1000, 603)}));
        const auto icons = fleet_stack_icons(view(cards), health, textures);
        assert(icons.size() == 3);
        assert(icons[0].icon == 0 && icons[0].count == 6 && icons[0].texture == 2);
        assert(icons[0].best == eawr::sim::galactic::TypeId{1003}); // greatest hull among the groups
        assert(icons[0].fleet == eawr::sim::galactic::FleetId{601} && icons[0].local);
        assert(icons[1].icon == 1 && icons[1].count == 2 && icons[1].texture == 1);
        assert(icons[1].best == eawr::sim::galactic::TypeId{1002}); // the later fleet 610 is not drawn
        assert(icons[2].icon == 2 && icons[2].count == 1 && icons[2].best == eawr::sim::galactic::TypeId{1000});
    }
    {
        // Another owner present: foreign fleets merge into the left icon, local
        // slots 0 and 2 merge into the right one, slot 1 stays the centre.
        std::vector<ObservedFleet> cards;
        cards.push_back(local_card(601, 0, {group(3, 1000, 601)}));
        cards.push_back(local_card(602, 1, {group(1, 1002, 602)}));
        cards.push_back(local_card(603, 2, {group(4, 1003, 603)}));
        ObservedFleet enemy;
        enemy.planet = 101; enemy.owner = 2; enemy.count = 12; // count-only: no identity, no groups
        cards.push_back(enemy);
        const auto icons = fleet_stack_icons(view(cards), health, textures);
        assert(icons.size() == 3);
        assert(icons[0].icon == 0 && icons[0].owner == 2 && !icons[0].local);
        assert(icons[0].count == 12 && icons[0].texture == 3 && !icons[0].best);
        assert(icons[1].icon == 1 && icons[1].count == 1 && icons[1].best == eawr::sim::galactic::TypeId{1002});
        assert(icons[2].icon == 2 && icons[2].owner == 1 && icons[2].count == 7 && icons[2].texture == 2);
        assert(icons[2].best == eawr::sim::galactic::TypeId{1003});
        // GHUD-S76: a count-only stack still has its independent overview
        // representative; the card contents and identities remain hidden.
        cards.back().overview_type = 1000;
        const auto with_image = fleet_stack_icons(view(cards), health, textures);
        assert(with_image[0].best == eawr::sim::galactic::TypeId{1000});
        assert(with_image[0].contents.empty() && !with_image[0].fleet);
    }
    {
        // Unknown hulls fall back to the observer's first typed group; an
        // enemy's strongest-only group still names its ship.
        std::vector<ObservedFleet> cards;
        ObservedFleet enemy;
        enemy.planet = 101; enemy.owner = 2; enemy.count = 30;
        ObservedFleetGroup strongest = group(1, 4242, 0);
        strongest.fleet.reset(); strongest.strongest = true;
        enemy.groups = {strongest};
        cards.push_back(enemy);
        const auto icons = fleet_stack_icons(view(cards), health, textures);
        assert(icons.size() == 1 && icons[0].best == eawr::sim::galactic::TypeId{4242});
        assert(icons[0].texture == 5);
    }
    {
        // A presence-only enemy (no card, count unknown) still flips the planet
        // to the mixed layout: the enemy draws nothing and the local
        // slot 0 fleet moves to the right icon.
        std::vector<ObservedFleet> cards;
        cards.push_back(local_card(601, 0, {group(3, 1000, 601)}));
        const auto icons = fleet_stack_icons(view(cards), health, textures, eawr::sim::galactic::PlayerId{2});
        assert(icons.size() == 1);
        assert(icons[0].icon == 2 && icons[0].owner == 1 && icons[0].local && icons[0].count == 3);
        assert(icons[0].fleet == eawr::sim::galactic::FleetId{601});
        // A carded enemy keeps its own left icon when presence names it too.
        ObservedFleet enemy;
        enemy.planet = 101; enemy.owner = 2; enemy.count = 4;
        cards.push_back(enemy);
        const auto both = fleet_stack_icons(view(cards), health, textures, eawr::sim::galactic::PlayerId{2});
        assert(both.size() == 2 && both[0].count == 4 && both[0].texture == 1 && both[1].icon == 2);
        // Presence with no card at all draws nothing.
        const auto alone = fleet_stack_icons({}, health, textures, eawr::sim::galactic::PlayerId{3});
        assert(alone.empty());
    }
    {
        // WGUI-166 hover contents. Local only: each icon lists every fleet of
        // its slot, also the later slot 1 fleet the icon itself does not draw.
        std::vector<ObservedFleet> cards;
        cards.push_back(local_card(601, 0, {group(3, 1000, 601), group(2, 1002, 605)}));
        cards.push_back(local_card(602, 1, {group(2, 1002, 602), group(4, 1003, 610)}));
        const auto icons = fleet_stack_icons(view(cards), health, textures);
        assert(icons.size() == 2);
        assert((icons[0].contents == std::vector<FleetStackEntry>{{1000, 3}, {1002, 2}}));
        assert((icons[1].contents == std::vector<FleetStackEntry>{{1002, 2}, {1003, 4}}));
        assert(icons[1].count == 2); // the drawn stack still counts its first fleet only
    }
    {
        // Mixed layout: the right icon merges local slots 0 and 2 by type in
        // first-seen order; the left icon lists the enemies. A count-only
        // enemy's groups stay anonymous cells (no type, never merged), its
        // strongest ship keeps its type.
        std::vector<ObservedFleet> cards;
        cards.push_back(local_card(601, 0, {group(3, 1000, 601), group(1, 1003, 601)}));
        cards.push_back(local_card(603, 2, {group(2, 1000, 603)}));
        ObservedFleet enemy;
        enemy.planet = 101; enemy.owner = 2; enemy.count = 9;
        ObservedFleetGroup hidden_a; hidden_a.count = 4;
        ObservedFleetGroup hidden_b; hidden_b.count = 2;
        ObservedFleetGroup strongest = group(3, 4242, 0);
        strongest.fleet.reset(); strongest.strongest = true;
        enemy.groups = {hidden_a, strongest, hidden_b};
        cards.push_back(enemy);
        const auto icons = fleet_stack_icons(view(cards), health, textures);
        assert(icons.size() == 2 && icons[0].icon == 0 && icons[1].icon == 2);
        assert((icons[0].contents == std::vector<FleetStackEntry>{{std::nullopt, 4}, {4242, 3}, {std::nullopt, 2}}));
        assert((icons[1].contents == std::vector<FleetStackEntry>{{1000, 5}, {1003, 1}}));
        // Two enemy fleets of one owner merge typed cells across fleets.
        ObservedFleet second = enemy;
        second.groups = {strongest};
        cards.push_back(second);
        const auto merged = fleet_stack_icons(view(cards), health, textures);
        assert((merged[0].contents == std::vector<FleetStackEntry>{{std::nullopt, 4}, {4242, 6}, {std::nullopt, 2}}));
    }
    {
        // A presence-only or group-less card has nothing to list: no popup.
        std::vector<ObservedFleet> cards;
        ObservedFleet enemy;
        enemy.planet = 101; enemy.owner = 2; enemy.count = 12;
        cards.push_back(enemy);
        const auto icons = fleet_stack_icons(view(cards), health, textures, eawr::sim::galactic::PlayerId{2});
        assert(icons.size() == 1 && icons[0].contents.empty());
        const auto alone = fleet_stack_icons({}, health, textures, eawr::sim::galactic::PlayerId{3});
        assert(alone.empty());
    }
    assert(fleet_stack_icons({}, health, textures).empty());
    return 0;
}
