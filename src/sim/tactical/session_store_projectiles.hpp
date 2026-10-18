#pragma once

#include "session_types.hpp"
#include "combat_internal.hpp"

namespace eawr::sim::tactical::session_detail {

// Ordered fragments preserve the session's original member lifetime order.
struct SessionStoreProjectiles {
    std::vector<Projectile> projectiles; // in flight, ascending ID (#74)
    struct PendingBlastDamage {
        std::uint64_t due{};
        PlayerId owner{};
        std::uint32_t damage_type{no_type_index};
        bool internal_damage_misc{true};
        EntityId recipient{};
        math::Fixed amount{};
        std::string contacted_mesh;
    };
    std::vector<PendingBlastDamage> pending_blast_damage;
    // #636: the projectile phase's candidate buffers, one per partition, kept from tick to tick
    // (not state: they only hold capacity and the tick's work counts).
    std::array<detail::ProjectileScratch, tick_partition_count> projectile_scratch;
};

struct SessionStoreProjectileIds {
    std::uint64_t next_projectile{1};
};

} // namespace eawr::sim::tactical::session_detail
