#pragma once

#include "session_types.hpp"

namespace eawr::sim::tactical::session_detail {

// Ordered fragments preserve the session's original member lifetime order.
struct SessionStoreHealing {
    struct HealingPulse { std::size_t source{}, target{}, slot{}; std::uint32_t hardpoint{hull_target}; };
    struct HealingJob { std::size_t target{}, begin{}, end{}; };
    // WSH-09: target == invalid_entity_id marks a due service (the deadline moves even with no victim).
    struct PassiveDamagePulse { EntityId source{invalid_entity_id}, target{invalid_entity_id}; std::size_t slot{}; };
    struct HealingScratch {
        std::vector<math::Fixed> fractions;
        std::array<std::vector<HealingPulse>, tick_partition_count> outputs;
        std::vector<HealingPulse> pulses;
        std::vector<HealingJob> jobs;
        std::vector<std::optional<core::Diagnostic>> errors;
    } healing_scratch;
};

} // namespace eawr::sim::tactical::session_detail
