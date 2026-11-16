#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace eawr::viewer::galactic {

// WGUI-31/32/110..112: the local observer's range/path reveal of travelling fleets, on presentation values only.
struct RevealPoint {
    double x{}, y{}, z{};
};
// One fleet-revealing candidate: a local-owned object or planet whose type authors a strictly positive
// Hyperspace_Fleet_Reveal_Range (WGUI-112), or a bribed planet with Bribery_Fleet_Reveal_Range (WGUI-110).
struct RevealCandidate {
    RevealPoint position;
    double range{};
};
// One travelling fleet that the earlier WGUI-30/31 tests (own, view-all, own destination, superweapon, stealth) did not
// already decide. AI fleets carry their stored enroute planet (the final destination); human and ownerless fleets carry
// the planet they left (`previous`) and the planet they fly to (`next`), WGUI-111.
struct RevealTransit {
    std::uint64_t fleet{};
    RevealPoint position;
    bool ai_owned{};
    std::uint64_t enroute{}, previous{}, next{}; // 0 = absent
};
struct RevealResult {
    std::uint64_t fleet{};
    bool visible{};
};

// A fleet's map point: its travel position while in transit, else the position of the planet it orbits. Fleets created in
// orbit (production, starting forces, surface retreat) keep an unset travel position until they first move, so an
// orbiting fleet never uses it. Unverified: the orbit offset of an arrived fleet is ignored (planet centre).
[[nodiscard]] inline RevealPoint fleet_reveal_point(bool in_transit, const RevealPoint& travel,
    const std::optional<RevealPoint>& orbit_planet) {
    return in_transit || !orbit_planet ? travel : *orbit_planet;
}

// WGUI-110: squared three-dimensional distance, strictly below the squared range; a zero range reveals nothing.
[[nodiscard]] inline bool within_reveal_range(const RevealPoint& a, const RevealPoint& b, double range) {
    if (!(range > 0)) return false;
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz < range * range;
}

// WGUI-32/111: a range hit still needs discovered path context: the AI fleet's enroute planet, or either the previous
// or the next path planet of a human/ownerless fleet. Discovery is the local player's.
template <class Discovered>
[[nodiscard]] bool reveal_path_known(const RevealTransit& fleet, const Discovered& discovered) {
    const auto known = [&](std::uint64_t planet) { return planet != 0 && discovered(planet); };
    return fleet.ai_owned ? known(fleet.enroute) : (known(fleet.previous) || known(fleet.next));
}

// One row per travelling fleet: visible when its path context is known and any candidate is in range.
template <class Discovered>
[[nodiscard]] std::vector<RevealResult> transit_reveal(std::span<const RevealTransit> fleets,
    std::span<const RevealCandidate> candidates, const Discovered& discovered) {
    std::vector<RevealResult> out;
    out.reserve(fleets.size());
    for (const auto& fleet : fleets) {
        bool visible = false;
        if (reveal_path_known(fleet, discovered))
            for (const auto& candidate : candidates)
                if (within_reveal_range(candidate.position, fleet.position, candidate.range)) { visible = true; break; }
        out.push_back({fleet.fleet, visible});
    }
    return out;
}

} // namespace eawr::viewer::galactic
