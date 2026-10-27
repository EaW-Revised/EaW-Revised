#pragma once
#include "eawr/sim/galactic/snapshot.hpp"
#include <functional>

namespace eawr::sim::galactic {
// GGS-09: authored presentation traits, never authoritative state.
struct GroundDisplayType {
    std::optional<std::uint32_t> health;
    bool named_hero{}, generic_hero{}, has_icon{};
    std::int64_t hero_rank{25};
};
struct ObservedGroundDisplay {
    PlanetId planet{};
    PlayerId owner{};
    std::size_t count{};
    // Size knowledge permits these two images, not typed contents or identities.
    std::optional<TypeId> strongest, hero;
    [[nodiscard]] std::optional<TypeId> image(bool prefer_hero) const noexcept {
        return prefer_hero ? (hero ? hero : strongest) : (strongest ? strongest : hero);
    }
    friend bool operator==(const ObservedGroundDisplay&, const ObservedGroundDisplay&) = default;
};
// Invoke once per displayed publication. Only the publication's observer may
// request rows; inaccessible, undiscovered and unknown-size surfaces fail closed.
[[nodiscard]] std::vector<ObservedGroundDisplay> observed_ground_display(
    const GalacticSnapshot&, PlayerId,
    const std::function<std::optional<GroundDisplayType>(TypeId)>&);
} // namespace eawr::sim::galactic
