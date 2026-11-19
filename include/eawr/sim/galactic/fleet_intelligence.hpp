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
    // WGUI-92: every member skips combat (WGM-38/163), so the stack shows the stealth overlay. A local
    // card sets it only when every fleet it merges is all-stealth; an enemy all-stealth fleet is never
    // published (WGUI-28/29), so the flag stays false there.
    bool all_stealth{};
    // GHUDL-16 / #6745: the lowest-ranked hero among the real members, a named hero carried inside another ship
    // included (the original's best-ship helper asks each ship whether it holds a named hero). Same disclosure as
    // `overview_type`: an image type only, no entity, fleet identity or contents group.
    std::optional<TypeId> overview_hero_type{};
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
    // Planets the observer currently sees with fleet-presence knowledge, whether or not a fleet still orbits them. The
    // advisor's battle result line asks the planet's visibility at resolution, not for a surviving fleet (legacy #7805).
    // Sorted by planet id.
    std::vector<PlanetId> presence_planets;
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
// hero_rank (#6745) is the presentation's category ranking of a type that can win the hero-first image (a named
// hero, or a generic hero with an icon); the lowest rank wins, the first member on a tie.
[[nodiscard]] FleetIntelligence observed_fleets(const GalacticSnapshot& snapshot,
    PlayerId local_player, std::span<const FleetDisplayType> traits,
    std::size_t group_capacity, std::span<const FleetDisplayVisibility> visibility = {},
    const std::function<std::optional<std::uint32_t>(TypeId)>& icon_rank = {},
    std::span<const std::pair<EntityId, std::int64_t>> presentation_indices = {},
    const std::function<std::optional<std::int64_t>(TypeId)>& hero_rank = {},
    std::span<const EntityId> departing_transports = {}); // GGS-12: sorted presentation-only exclusions

// #4375 / WGSI-1..4: which named heroes the local player sees at a planet, for the overview's hero icons. A planet row
// is listed only when a hero qualifies. `ally` is set by an allied hero, `foreign` by a foreign hero the player's
// knowledge admits; `owners` lists the owner players (ascending, unique) of the qualifying heroes. A hero landed on
// a surface sets its owner's faction whatever its own visibility, but only while ground contents, major-stealth or
// force-sensitive knowledge applies. Presentation projection: it grants no entity or fleet identity and changes no
// state.
// `heroes` lists the named heroes the hover tooltip of those icons shows (WGSI-8/9, legacy #7177): every allied hero, and
// every non-allied hero the player's knowledge admits, in the order found (orbiting fleets, then landed forces).
struct ObservedPlanetHero {
    PlayerId owner{};
    TypeId type{};
    bool allied{};
    friend bool operator==(const ObservedPlanetHero&, const ObservedPlanetHero&) = default;
};
struct ObservedPlanetHeroes {
    PlanetId planet{};
    bool ally{}, foreign{};
    std::vector<PlayerId> owners;
    std::vector<ObservedPlanetHero> heroes;
    friend bool operator==(const ObservedPlanetHeroes&, const ObservedPlanetHeroes&) = default;
};
[[nodiscard]] std::vector<ObservedPlanetHeroes> observed_planet_heroes(const GalacticSnapshot& snapshot,
    PlayerId local_player, std::span<const FleetDisplayType> traits, std::span<const FleetDisplayVisibility> visibility);

} // namespace eawr::sim::galactic
