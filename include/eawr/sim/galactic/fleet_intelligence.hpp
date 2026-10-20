#pragma once

#include "eawr/sim/galactic/snapshot.hpp"

#include <algorithm>
#include <functional>

namespace eawr::sim::galactic {

// Optional presentation traits supplied by validated content/live owners. The
// projection never guesses hero categories, current Damage or stack metadata.
struct FleetDisplayType {
    TypeId type{};
    bool hero{}, named_hero{}, force_sensitive{}, transport{};
    std::optional<math::Fixed> damage;
    int gui_slot{};
    friend bool operator==(const FleetDisplayType&, const FleetDisplayType&) = default;
};
struct ObservedFleetGroup {
    std::size_t count{};
    int gui_slot{};
    // WGUI-73/74: anonymous counts carry neither type nor object identity.
    // WGUI-72 (#3079): the true (original) type, so a company in orbit is the
    // company and not its transport member.
    std::optional<TypeId> type;
    std::optional<EntityId> representative;
    // The fleet that owns `representative`. A local slot card merges every
    // fleet sharing its orbit slot, so a group's fleet may differ from the
    // card's first fleet. Same disclosure as `representative`: unset when the
    // group is anonymous.
    std::optional<FleetId> fleet;
    bool transport{}, strongest{}, superweapon{};
    bool hero{}; // the disclosed representative is a hero (reports and tests; a hidden group stays false)
    // GHUD-S87: local presentation index, with the representative's disclosure permission.
    std::optional<std::int64_t> gui_index;
    friend bool operator==(const ObservedFleetGroup&, const ObservedFleetGroup&) = default;
};
// WGEP-40/WGEP-66, GQP-11: a completed orbit unit becomes its own fleet at
// logical slot 0 and is never merged, so one local slot can hold several
// fleets. The panel shows every fleet of a slot together, grouped by type
// (debug-build capture, no merge in the placement). One local card therefore
// stands for all local fleets sharing (planet, slot): `fleet` is the first one
// in fleet-id order, `count` the total, and each group names its own fleet.
// Enemy and count-only cards are one fleet each, as before.
struct ObservedFleet {
    std::optional<FleetId> fleet;
    PlanetId planet{};
    PlayerId owner{};
    std::optional<std::uint8_t> orbit_slot;
    bool local{};
    std::size_t count{};
    std::vector<ObservedFleetGroup> groups;
    // GHUD-S76: the overview's best ship image is separate from contents
    // knowledge. It grants no entity, fleet identity or typed contents group.
    std::optional<TypeId> overview_type{};
    friend bool operator==(const ObservedFleet&, const ObservedFleet&) = default;
};
// True when `fleet` is the card's first fleet or the fleet of one of its groups.
[[nodiscard]] inline bool holds_fleet(const ObservedFleet& card, const FleetId fleet) {
    return fleet && (card.fleet == fleet || std::any_of(card.groups.begin(), card.groups.end(),
        [fleet](const ObservedFleetGroup& group) { return group.fleet == fleet; }));
}
struct ObservedFleetPresence {
    PlanetId planet{};
    PlayerId owner{};
    friend bool operator==(const ObservedFleetPresence&, const ObservedFleetPresence&) = default;
};
struct FleetIntelligence {
    std::vector<ObservedFleetPresence> presence;
    std::vector<ObservedFleet> cards;
    // Deterministic work receipts: no fleet x planet scan or hidden recomputation.
    std::size_t observation_rows{}, fleets_examined{}, members_examined{};
    friend bool operator==(const FleetIntelligence&, const FleetIntelligence&) = default;
};
struct FleetDisplayVisibility {
    PlanetId planet{};
    std::optional<bool> current_visible;
};
// WGUI-28/29/42/72..74. Invoke once for each displayed immutable publication,
// never in a frame update. Only the configured local observer may request rows.
// FleetMember is the live direct membership table (no pending-deletion entries).
// Groups use the true (original) type. Pathfinder reservations, raid flags and stack ranking are
// explicit #1154/WGUI-U07 producer seams, separate from these filtered cards.
// GHUD-S76: icon_rank is an integer preference key supplied by the presentation
// data adapter. It never imports floating arithmetic into the simulation boundary.
[[nodiscard]] FleetIntelligence observed_fleets(const GalacticSnapshot& snapshot,
    PlayerId local_player, std::span<const FleetDisplayType> traits,
    std::size_t group_capacity, std::span<const FleetDisplayVisibility> visibility = {},
    const std::function<std::optional<std::uint32_t>(TypeId)>& icon_rank = {},
    std::span<const std::pair<EntityId, std::int64_t>> presentation_indices = {});

} // namespace eawr::sim::galactic
