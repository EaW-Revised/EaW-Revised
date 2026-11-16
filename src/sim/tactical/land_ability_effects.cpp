#include "eawr/sim/tactical/land_abilities.hpp"

#include <algorithm>
#include <limits>
#include <memory>

// Land ability effects, group G2 (#3239, docs/behaviour/walks/land-abilities.md WLAB-12 to WLAB-19, WLAB-37, WLAB-71). The G1
// switch owns the slot, the gate and the timers; a kind's effect is what is left once those ran. The speed, damage and
// weapon-delay multipliers, the spread-out spacing and the diminishing-firepower flag are not applied here: they are
// read by the consumers of the land speed, formation and combat contexts (#1451) through the land_ability_inputs query.
namespace eawr::sim::tactical {
namespace {

// WLAB-12 (PR A): SPREAD_OUT. The slot flags of the container and of every member that holds the ability are G1's delegation
// (WLAB-03); the formation rebuild has no runtime object in the land session yet (docs/behaviour/walks/land-pathing.md,
// WLPA-27: runtime row reassignment is missing), so the switch changes the slot alone and the spacing factor is an input.
class SpreadOutEffect final : public LandAbilityEffect {};

// WLAB-14: SPRINT. The multipliers of the row (speed, damage taken) are read from the active slot, not applied here. The reset of
// the parent container's formation state on a switch-on is deferred with the formation consumer (#1451): there is no runtime
// formation state of a container to reset yet.
class SprintEffect final : public LandAbilityEffect {};

// WLAB-37: MAXIMUM_FIREPOWER has no unit-level effect in the switch (it succeeds and does nothing); its weapon-delay
// multiplier and the diminishing-firepower flag are slot inputs. The projectile override weapons read while it is on is deferred
// (the land loader binds no projectile override and land combat reads none).
class MaximumFirepowerEffect final : public LandAbilityEffect {};

// WLAB-15: STIM_PACK. A switch-on first takes Damage_Percent_When_Activated of the unit's maximum health from it, then
// behaves like SPRINT (speed and weapon-delay multipliers of the row).
class StimPackEffect final : public LandAbilityEffect {
public:
    void switched(const AbilityUse& use, const bool on) const override {
        if (!on || use.world == nullptr || use.ability == nullptr || use.ability->activation_damage.raw() <= 0) return;
        use.world->damage_self(use.unit, use.ability->activation_damage);
    }
};

// WLAB-16 (PR B): JET_PACK is a targeted-location ability. It refuses a point farther than Effective_Radius when that is 0 or
// more (the stock -1 is unlimited), then records the jump as pending with the secondary locomotor in use. The jump itself (the
// one-object coordinated move that follows) belongs to the land movement (#7084). The recharge-at-once timer is G1's.
class JetPackEffect final : public LandAbilityEffect {
public:
    [[nodiscard]] bool admit(const AbilityUse& use) const override {
        if (!use.position) return false;
        if (use.world != nullptr && use.world->jump_in_flight(use.unit)) return false;
        if (use.world == nullptr || use.ability == nullptr || use.ability->effective_radius.raw() < 0) return true;
        const auto distance = use.world->planar_distance(use.unit, *use.position);
        return distance && *distance <= use.ability->effective_radius;
    }
    void switched(const AbilityUse& use, const bool on) const override {
        if (on && use.world != nullptr && use.position) use.world->request_jump(use.unit, *use.position);
    }
    [[nodiscard]] AbilityTargetRule target_rule() const override {
        AbilityTargetRule rule;
        rule.object = false;
        return rule;
    }
    // The jump's reach is its Effective_Radius (admit), never the holder's weapon range: the range context names no limit.
    [[nodiscard]] AbilityRangeContext range() const override {
        AbilityRangeContext context;
        context.activation_max = math::Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
        return context;
    }
};

// WLAB-17 (PR B): DEPLOY. Either way the unit stops (it leaves its movement and ends on a stop order) and its deploy phase
// follows the switch. The targeting notification of a switch-off and the weapon gate of a deployed unit belong to land combat.
class DeployEffect final : public LandAbilityEffect {
public:
    void switched(const AbilityUse& use, const bool on) const override {
        if (use.world != nullptr) use.world->set_deployed(use.unit, on);
    }
};

// WLAB-18 (PR C): ROCKET_ATTACK is modal: the slot goes on or off and the expiration timer follows, both G1's. The effect has
// nothing of its own: the mode is read by the weapon gate, which takes the active slot as rocket_attack in the combat world
// (the same copy a space body gets). `Targeting_Max_Attack_Distance` of the ability has no consumer yet (U-LAB-7).
class RocketAttackEffect final : public LandAbilityEffect {};

// WLAB-19 (PR C): SWAP_WEAPONS is modal and runs no timer. Each switch toggles the unit's current projectile index between 0 and
// 1; a type with one projectile type does nothing and the switch still succeeds. Only the index is kept: the land weapon fire
// (#1451) reads it. A switch with no world (an expiry) changes nothing; the kind has no expiry.
class SwapWeaponsEffect final : public LandAbilityEffect {
public:
    void switched(const AbilityUse& use, const bool) const override {
        if (use.world != nullptr) use.world->swap_weapon(use.unit);
    }
};

} // namespace

std::shared_ptr<const LandAbilityEffects> make_land_ability_effects() {
    auto effects = std::make_shared<LandAbilityEffects>();
    effects->rows = {
        {AbilityKind::deploy, std::make_shared<DeployEffect>()},
        {AbilityKind::spread_out, std::make_shared<SpreadOutEffect>()},
        {AbilityKind::jet_pack, std::make_shared<JetPackEffect>()},
        {AbilityKind::sprint, std::make_shared<SprintEffect>()},
        {AbilityKind::maximum_firepower, std::make_shared<MaximumFirepowerEffect>()},
        {AbilityKind::stim_pack, std::make_shared<StimPackEffect>()},
        {AbilityKind::swap_weapons, std::make_shared<SwapWeaponsEffect>()},
        {AbilityKind::rocket_attack, std::make_shared<RocketAttackEffect>()},
    };
    std::sort(effects->rows.begin(), effects->rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return effects;
}

} // namespace eawr::sim::tactical
