#pragma once

#include "eawr/sim/galactic/fleet_intelligence.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace eawr::viewer::galactic {

// GHUD-S97: a whole stack carries its count; an ordinary member carries one
// and leaves the authored remainder. This never changes fleet membership.
inline std::pair<std::size_t, std::size_t> fleet_drag_counts(const std::size_t count,
    const bool single_member, const bool should_ghost) {
    const auto carried = single_member ? std::min(count, std::size_t{1}) : count;
    return {carried, should_ghost ? count - carried : 0};
}

// WGUI-166: one cell of the stack icon's hover encyclopedia, one per unit type
// with its count. A count-only group has no type: its cell is a blank icon
// with the count and never names the units (WGUI-73/74).
struct FleetStackEntry final {
    std::optional<sim::galactic::TypeId> type;
    std::size_t count{};
    friend bool operator==(const FleetStackEntry&, const FleetStackEntry&) = default;
};

// GHUD-S65 / GHUDL-16: the planet's per-fleet presence icons. Each of the three
// orbit icons (left, centre, right) is one stack ellipse chosen by ship count
// with the fleet's best ship inside it.
struct FleetStackIcon final {
    std::uint8_t icon{};                // 0 left, 1 centre, 2 right
    sim::galactic::PlayerId owner{};
    std::optional<sim::galactic::FleetId> fleet; // first fleet of the icon, for drag/merge
    std::size_t count{};
    std::size_t texture{};              // index into the authored stack textures
    std::optional<sim::galactic::TypeId> best;
    bool local{};
    bool stealth{};                     // WGUI-92: every fleet of the icon is all-stealth
    std::vector<FleetStackEntry> contents; // WGUI-166 hover encyclopedia cells
    friend bool operator==(const FleetStackIcon&, const FleetStackIcon&) = default;
};

// WGUI-166: the hover encyclopedia lists every fleet of the icon's orbit
// slots, also where the icon itself shows only the first fleet. Typed groups
// of the same type merge across fleets in first-seen order; count-only groups
// carry no type and stay separate cells.
inline void add_stack_contents(std::vector<FleetStackEntry>& contents, const sim::galactic::ObservedFleet& card) {
    for (const auto& group : card.groups) {
        if (group.count == 0) continue;
        const auto same = group.type ? std::find_if(contents.begin(), contents.end(),
            [&](const FleetStackEntry& entry) { return entry.type == group.type; }) : contents.end();
        if (same != contents.end()) same->count += group.count;
        else contents.push_back({group.type, group.count});
    }
}

// WGUI-92: debug build resets the ships-per-stack constant to 5; the authored
// ships-per-stack tag is never read (foc-ignores in the tag registry), so a mod
// cannot change it either.
inline constexpr std::size_t ships_per_stack = 5;

// WGUI-92: ceiling of count / ships per stack, capped at the last texture.
[[nodiscard]] constexpr std::size_t stack_texture_index(const std::size_t count, const std::size_t textures) noexcept {
    if (textures == 0) return 0;
    return std::min((count + ships_per_stack - 1) / ships_per_stack, textures - 1);
}

// Per-type Max tactical health from the authored object; absent when unknown.
using StackHealth = std::function<std::optional<float>(sim::galactic::TypeId)>;

// GHUD-S76: binary32's integer order key preserves every finite authored
// health comparison, including negative values and equal signed zeroes.
// Only this presentation adapter handles floating point; the observer
// projection receives the integer key with no new numeric range bound.
[[nodiscard]] inline std::optional<std::uint32_t> stack_health_rank(const std::optional<float> value) {
    if (!value || !std::isfinite(*value)) return std::nullopt;
    const auto bits = *value == 0.0F ? std::uint32_t{0} : std::bit_cast<std::uint32_t>(*value);
    return (bits & 0x80000000U) ? ~bits : (bits ^ 0x80000000U);
}

// GHUDL-16 best ship: the greatest max tactical health among the visible groups
// (debug build best-ship helper with the hero-category filter clear). Unknown
// health everywhere falls back to the observer's first typed group (GHUD-R11).
// Anonymous count-only groups carry no type, so nothing is chosen for them.
[[nodiscard]] inline std::optional<sim::galactic::TypeId> best_stack_ship(
    std::span<const sim::galactic::ObservedFleetGroup* const> groups, const StackHealth& health) {
    std::optional<sim::galactic::TypeId> best, first;
    float greatest = 0.0F;
    bool measured = false;
    for (const auto* group : groups) {
        if (!group || !group->type) continue;
        if (!first) first = group->type;
        const auto value = health ? health(*group->type) : std::nullopt;
        if (!value) continue;
        if (!measured || *value > greatest) { greatest = *value; best = group->type; measured = true; }
    }
    return measured ? best : first;
}

namespace detail {
struct StackAccumulator final {
    bool used{};
    sim::galactic::PlayerId owner{};
    std::optional<sim::galactic::FleetId> fleet;
    std::size_t count{};
    bool local{};
    bool stealth{true};
    std::vector<const sim::galactic::ObservedFleetGroup*> groups;
    std::vector<FleetStackEntry> contents;
    std::optional<sim::galactic::TypeId> overview_type;
};
} // namespace detail

// WGUI-92 / debug build planet fleet display data. `cards` are the observer
// cards of one planet; `aware_other` is a non-local owner the observer knows is
// present there (a presence row), also when it has no card (count unknown).
//  - Only the local player's own fleets (observer sees no other owner): a local
//    fleet at slot s fills icon s. Slot 0 collects every fleet at that slot;
//    slots 1 and 2 show their first fleet only (a second fleet at the same
//    slot draws nothing).
//  - Any other owner present: every non-local fleet merges into the left icon;
//    the local slot 1 fills the centre and local slots 0 and 2 merge into the
//    right icon. GHUD-S76: presence alone does not draw a stack; a card is
//    required, so an unknown count never produces an empty disk.
// The raid overlay (no producer yet), true-type remapping and transported contents are
// not published yet and stay seams.
[[nodiscard]] inline std::vector<FleetStackIcon> fleet_stack_icons(
    std::span<const sim::galactic::ObservedFleet* const> cards, const StackHealth& health,
    const std::size_t textures, const std::optional<sim::galactic::PlayerId> aware_other = std::nullopt) {
    std::array<detail::StackAccumulator, 3> slots;
    const bool foreign = aware_other || std::any_of(cards.begin(), cards.end(),
        [](const auto* card) { return card && !card->local; });
    const auto add = [&health](detail::StackAccumulator& slot, const sim::galactic::ObservedFleet& card,
                        const bool whole) {
        if (!slot.used) { slot.owner = card.owner; slot.local = card.local; slot.fleet = card.fleet; }
        slot.used = true;
        slot.stealth = slot.stealth && card.all_stealth;
        if (!card.local && card.overview_type) {
            const auto candidate = health ? health(*card.overview_type) : std::nullopt;
            const auto current = health && slot.overview_type ? health(*slot.overview_type) : std::nullopt;
            if (!slot.overview_type || (candidate && (!current || *candidate > *current)))
                slot.overview_type = card.overview_type;
        }
        add_stack_contents(slot.contents, card);
        if (whole) {
            slot.count += card.count;
            for (const auto& group : card.groups) slot.groups.push_back(&group);
            return;
        }
        // A local card merges every fleet of its slot; a single-fleet icon is
        // its first fleet only.
        std::size_t own{};
        for (const auto& group : card.groups) {
            if (group.fleet != card.fleet) continue;
            slot.groups.push_back(&group);
            own += group.count;
        }
        slot.count += card.groups.empty() ? card.count : own;
    };
    for (const auto* card : cards) {
        if (!card) continue;
        if (!card->local) { add(slots[0], *card, true); continue; }
        const std::size_t slot = card->orbit_slot ? *card->orbit_slot : 0;
        if (slot > 2) continue;
        if (foreign) add(slots[slot == 1 ? 1 : 2], *card, true);
        else if (slot == 0) add(slots[0], *card, true);
        else if (!slots[slot].used) add(slots[slot], *card, false);
    }
    std::vector<FleetStackIcon> icons;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        const auto& slot = slots[index];
        if (!slot.used) continue;
        FleetStackIcon icon;
        icon.icon = static_cast<std::uint8_t>(index);
        icon.owner = slot.owner; icon.fleet = slot.fleet; icon.local = slot.local;
        icon.count = slot.count;
        icon.stealth = slot.stealth;
        icon.texture = stack_texture_index(slot.count, textures);
        icon.best = slot.overview_type ? slot.overview_type : best_stack_ship(slot.groups, health);
        icon.contents = slot.contents;
        icons.push_back(std::move(icon));
    }
    return icons;
}

} // namespace eawr::viewer::galactic
