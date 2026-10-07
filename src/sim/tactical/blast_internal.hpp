#pragma once

#include "combat_internal.hpp"

namespace eawr::sim::tactical::detail {

[[nodiscard]] core::Result<math::Fixed> projectile_delivery_delay(const Projectile& projectile,
    math::Fixed area_delay, CombatRandom& random);

struct BlastRecipient {
    EntityId id{};
    math::Fixed amount{};
    math::Fixed delay{};
    std::uint32_t hardpoint{hull_target}; // resolved secondary mesh selector, never the projectile's original aim
};

struct BlastPlayerStep {
    std::vector<BlastRecipient> recipients;
    bool capped{}; // this accepted group reaches the whole-blast exit
};

struct BlastStep {
    std::vector<BlastPlayerStep> players; // aligned with the immutable player table
    std::uint64_t examined{}; // copied candidates, including rejects and candidates beyond the cap
};

// WAD-01/03/05: callers provide contact or terminal position and direct-route exclusions.
[[nodiscard]] core::Result<BlastStep> prepare_blast(const CombatWorld& world,
    const Projectile& projectile, const math::Vec3& centre, std::optional<EntityId> direct = std::nullopt,
    bool source_available = true, std::string_view direct_mesh = {});

// The flight service already applied the aimed-mesh override; retain its final selector for WAD-20.
[[nodiscard]] std::string_view direct_blast_mesh(const CombatWorld& world, const ProjectileStep& flight);
[[nodiscard]] core::Result<Hit> area_hit(const Projectile& projectile, const BlastRecipient& recipient,
    math::Fixed defense, std::optional<math::Fixed> source_damage_factor = std::nullopt);

[[nodiscard]] core::Result<math::Fixed> blast_factor(const BlastProfile& profile,
    math::Fixed distance, math::Fixed radius);

[[nodiscard]] math::Fixed primary_damage(const Projectile& projectile) noexcept;
[[nodiscard]] bool blast_after_hit(const Projectile& projectile, const HitOutcome& outcome) noexcept;

} // namespace eawr::sim::tactical::detail
