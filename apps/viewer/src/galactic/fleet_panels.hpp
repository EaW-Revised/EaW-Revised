#pragma once

#include "eawr/sim/galactic/fleet_intelligence.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace eawr::viewer::galactic {

// WGUI-175 (debug build, owner retail capture 2026-10-16): the focused planet's three orbit panels list
// either the local fleets by orbit slot (friendly layout) or, whenever an aware fleet of another owner is
// present, that other fleet in panel 1 with the local fleets moved: slot 1 stays in panel 2, slots 0 and 2
// share panel 3. Presentation only; observer knowledge (WGUI-28/29/42/73) is unchanged and decides which cards exist.
enum class FleetPanelMode : std::uint8_t { friendly, enemy };

struct FleetPanels final {
    FleetPanelMode mode{FleetPanelMode::friendly};
    std::array<std::optional<sim::galactic::ObservedFleet>, 3> panel;
    // The orbit slot a drop on the panel targets; none for the other-owner panel, which accepts no drop.
    std::array<std::optional<std::uint8_t>, 3> drop_slot{std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{2}};
    // Every aware fleet of another owner, for the aggregate icon's count (first card is its representative).
    std::size_t other_count{};
    const sim::galactic::ObservedFleet* first_other{};
};

// Folds `source` into `target` the way the original's unique-object list does: typed groups of one type
// become one group with the summed count; count-only groups stay separate. The merged card names a fleet
// and an orbit slot only while every contributor agrees, so a drag from it never moves another slot's ships.
inline void merge_fleet_card(sim::galactic::ObservedFleet& target, const sim::galactic::ObservedFleet& source,
    const std::size_t group_capacity, const bool first) {
    if (first) {
        target = source;
        if (target.groups.size() > group_capacity) target.groups.resize(group_capacity);
        return;
    }
    target.count += source.count;
    if (target.orbit_slot != source.orbit_slot) target.orbit_slot.reset();
    if (!target.fleet) target.fleet = source.fleet;
    // Two other owners in one panel keep the first owner's identity and colour on the merged card (known gap).
    if (target.owner != source.owner) { target.local = target.local && source.local; }
    if (!target.overview_type) target.overview_type = source.overview_type;
    for (const auto& group : source.groups) {
        const auto same = group.type ? std::find_if(target.groups.begin(), target.groups.end(),
            [&](const auto& row) { return row.type == group.type; }) : target.groups.end();
        if (same != target.groups.end()) {
            same->count += group.count;
            same->superweapon = same->superweapon || group.superweapon;
            same->strongest = same->strongest || group.strongest;
        } else if (target.groups.size() < group_capacity) target.groups.push_back(group);
    }
}

// `cards` are the focused planet's observer cards in publication order. `other_aware` also covers a planet
// where another owner's presence is known but no card exists: the layout still flips (WGUI-42).
[[nodiscard]] inline FleetPanels plan_fleet_panels(
    const std::span<const sim::galactic::ObservedFleet* const> cards, const bool other_aware,
    const std::size_t group_capacity) {
    FleetPanels plan;
    for (const auto* card : cards) {
        if (!card || card->local) continue;
        plan.other_count += card->count;
        if (!plan.first_other) plan.first_other = card;
    }
    plan.mode = other_aware || plan.first_other ? FleetPanelMode::enemy : FleetPanelMode::friendly;
    std::array<bool, 3> started{};
    const auto place = [&](const std::size_t panel, const sim::galactic::ObservedFleet& card) {
        if (!plan.panel[panel]) plan.panel[panel].emplace();
        merge_fleet_card(*plan.panel[panel], card, group_capacity, !started[panel]);
        started[panel] = true;
    };
    if (plan.mode == FleetPanelMode::enemy) {
        plan.drop_slot = {std::nullopt, std::uint8_t{1}, std::uint8_t{0}};
        for (const auto* card : cards) if (card && !card->local) place(0, *card);
        // Orbit slots in order 0, 1, 2: home guard and left flank both land in panel 3.
        for (std::uint8_t slot = 0; slot < 3; ++slot)
            for (const auto* card : cards)
                if (card && card->local && card->orbit_slot == slot) place(slot == 1 ? 1U : 2U, *card);
    } else {
        for (const auto* card : cards)
            if (card && card->local && card->orbit_slot && *card->orbit_slot < 3 && !plan.panel[*card->orbit_slot])
                place(*card->orbit_slot, *card);
    }
    return plan;
}

// The panel that shows a local fleet of this orbit slot under the plan's layout.
[[nodiscard]] constexpr std::size_t fleet_panel_for_slot(const FleetPanelMode mode, const std::uint8_t slot) noexcept {
    return mode == FleetPanelMode::enemy && slot != 1 ? 2U : slot;
}

} // namespace eawr::viewer::galactic
