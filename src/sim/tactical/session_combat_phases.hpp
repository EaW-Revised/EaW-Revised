#pragma once

#include "session_tick.hpp"

namespace eawr::sim::tactical::session_detail {

// Both steppers supply the same copied world and ordered damage journal. Domain-specific
// movement and target policy stay outside this driver; space keeps its original phase order.
class CombatPhases final {
public:
    explicit CombatPhases(Tick& context) : context_(context) {}
    core::Result<void> launch_shots();
    core::Result<void> apply_direct_impact(const UnitStage::iterator& target,
        const DamageRules* const& damage_rules, const DurabilityProfile& profile, const Projectile& projectile,
        Hit& hit, std::uint32_t& hardpoint, std::optional<detail::ProjectileStep>& flight,
        std::vector<CombatEvent>& combat_events, std::vector<Event>& events, std::vector<UnitState>& killed,
        UnitStage& staged, auto& deliver_blast, std::size_t& flight_index);
    core::Result<void> impacts();
    core::Result<void> commit_survivors();
private:
    Tick& context_;
};

} // namespace eawr::sim::tactical::session_detail
