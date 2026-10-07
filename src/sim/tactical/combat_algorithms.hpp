#pragma once

#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/abilities.hpp"

#include "../math/wide.hpp"
#include "../replay_internal.hpp"
#include "combat_internal.hpp"
#include "motion_internal.hpp"
#include "tactical_internal.hpp"

#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/math/trig.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace eawr::sim::tactical::detail::combat_detail {

constexpr std::int64_t one_raw = math::Fixed::scale;
// One unit's targeting and fire for one frame. The first arithmetic failure is kept and fails
// the step; the rules never reach one for bounded content and positions.
class UnitCombat final {
public:
    UnitCombat(const CombatWorld& world, const CombatUnit& unit);

    core::Result<CombatStep> run();
    core::Result<bool> admit_manual(const CombatUnit& target, std::uint32_t hardpoint);
    math::Vec3 manual_angles(const ManualTurretProfile& turret, const math::Vec3& point);
    math::Vec3 manual_rotate(const math::Vec3& value, const math::Vec3& pivot,
        const std::array<math::Vec3, 3>& axes, math::Fixed angle, bool pitch, bool direction);
    WeaponProfile manual_pose(const WeaponProfile& weapon, const ManualWeaponState& state);
    void manual_turret(const WeaponProfile& weapon, ManualWeaponState& state);
    [[nodiscard]] CombatState target_state(math::Fixed divert, EntityId formation_target,
        math::Vec3 diversion_anchor, const FormationAttackContext* formation);

    // --- shared helpers ---------------------------------------------------------------------

    template <typename T>
    T take(core::Result<T> result, T fallback = T{}) {
        if (!result) {
            if (!error_) error_ = result.error();
            return fallback;
        }
        return std::move(result).value();
    }

    [[nodiscard]] math::Vec3 world_point(const CombatUnit& owner, const math::Vec3& local) {
        return take(math::transform_point(owner.transform, local), owner.position);
    }

    [[nodiscard]] bool visible_to_me(const CombatUnit& target) const noexcept {
        return target.team == unit_.team || (target.visible_to & (std::uint64_t{1} << unit_.player_index)) != 0U;
    }

    [[nodiscard]] bool hostile(const CombatUnit& target) const noexcept {
        return world_.hostile(unit_.owner, target.owner); // WHZ-51
    }

    [[nodiscard]] std::uint64_t categories(const CombatUnit& target) const noexcept {
        return target.profile != nullptr ? target.profile->category_bits : 0U;
    }

    // R-09 through the resolved set; a parent without a set scores every candidate 1.0.
    [[nodiscard]] std::optional<math::Fixed> priority(const CombatUnit& target) const {
        if (!profile_.priority_set) {
            return math::Fixed::from_raw(one_raw);
        }
        const auto& set = world_.table->priority_sets[*profile_.priority_set];
        const auto found = std::lower_bound(set.rows.begin(), set.rows.end(), target.type_id,
            [](const PriorityRow& row, const TypeId id) { return row.type_id < id; });
        if (found != set.rows.end() && found->type_id == target.type_id) {
            return found->priority;
        }
        return set.unlisted;
    }

    [[nodiscard]] bool hardpoint_destroyed_on(const CombatUnit& target, const std::uint32_t index) const {
        return target.durability_profile != nullptr && target.durability != nullptr
            && index < target.durability_profile->hardpoints.size()
            && tactical::hardpoint_destroyed(*target.durability_profile, *target.durability, index);
    }

    [[nodiscard]] bool weapon_up(const WeaponProfile& weapon) const {
        if (weapon.hardpoint == object_weapon || unit_.durability_profile == nullptr || unit_.durability == nullptr
            || weapon.hardpoint >= unit_.durability_profile->hardpoints.size()) {
            return true;
        }
        return weapon_enabled(*unit_.durability_profile, *unit_.durability, weapon.hardpoint);
    }

    [[nodiscard]] bool restricted(const WeaponProfile& weapon, const CombatUnit& target) const noexcept {
        return (weapon.category_restrictions & categories(target)) != 0U;
    }

    [[nodiscard]] math::Vec3 local_midpoint(const WeaponProfile& weapon) const noexcept {
        if (!weapon.has_fire_b) return weapon.fire_a;
        return math::Vec3{math::Fixed::from_raw((weapon.fire_a.x.raw() + weapon.fire_b.x.raw()) / 2),
            math::Fixed::from_raw((weapon.fire_a.y.raw() + weapon.fire_b.y.raw()) / 2),
            math::Fixed::from_raw((weapon.fire_a.z.raw() + weapon.fire_b.z.raw()) / 2)};
    }

    // Can_Weapon_Point_At for a fixed hardpoint: the point's yaw and pitch in Fire_Bone_A's
    // frame placed at the weapon midpoint lie within half the cone width and height; a zero
    // (unauthored) cone accepts only a point dead ahead (W-07). The object weapon's cone is its
    // turret extents about the unit's own facing, compared whole; without them it has none (W-09).
    [[nodiscard]] bool pointable(const WeaponProfile& weapon, const math::Vec3& midpoint, const math::Vec3& point);

    // R-10, R-11: target bones in order, then the target's hardpoints that are not destroyed in
    // circular order from one draw, then its position. The first point in range and pointable.
    struct Aim {
        math::Vec3 point{};
        std::uint32_t hardpoint{no_hardpoint};
    };
    [[nodiscard]] std::optional<Aim> choose_aim(const WeaponProfile& weapon, const math::Vec3& midpoint,
        const CombatUnit& target, CombatRandom& random, const RangeMetric metric);

    // Attempt_Fire_At_Target (R-13 and the audit's firing attempt): on success the shot is
    // recorded and the weapon's burst and recharge advance.
    bool attempt(const std::uint32_t slot, const EntityId target_id, CombatRandom& random, const bool burst_begun = false);

    // One pulse of the burst (W-06, W-06a): while pulses remain the countdown becomes the pulse
    // delay; after the last the recharge, and the burst refills. The object weapon's recharge
    // adds a draw of 0 to 10 frames. AB-21: an active ability's weapon delay multiplier
    // lengthens the gap within a burst and shortens, never lengthens, the recharge after it.
    void spend_pulse(const WeaponProfile& weapon, WeaponState& state, CombatRandom& random);

    // W-10: the shot leads the target, which keeps the velocity of its last frame's move. No lead
    // without a projectile.
    [[nodiscard]] std::optional<math::Vec3> lead(
        const WeaponProfile& weapon, const CombatUnit& target, const math::Vec3& aim, const math::Vec3& muzzle);

    // DG-24, W-11: a projectile shot is aimed off its led point by a draw in [-r, r] on each axis,
    // r = `distance` x the inaccuracy / range. A hardpoint measures the planar distance from the
    // weapon midpoint to the aim point before the lead, the object weapon (#607) the spatial
    // distance from its muzzle to the led point against its Targeting_Max_Attack_Distance. The
    // inaccuracy is the first Fire_Inaccuracy_Distance row that names one of the target's
    // categories, doubled: in space the authored distance counts twice (#536).
    [[nodiscard]] math::Vec3 scatter(const WeaponProfile& weapon, const math::Fixed distance, const CombatUnit& target,
        const math::Vec3& point, CombatRandom& random);

    // Exact: |from - p|^2 < |from - q|^2.
    [[nodiscard]] static bool closer(const math::Vec3& from, const math::Vec3& p, const math::Vec3& q) {
        return math::detail::compare(squared(from, p, RangeMetric::spatial), squared(from, q, RangeMetric::spatial)) < 0;
    }

    // Exact squared distance: at most three squares below 2^128 each, so the 192-bit sum cannot overflow.
    [[nodiscard]] static math::detail::UInt192 squared(const math::Vec3& a, const math::Vec3& b, const RangeMetric metric) {
        math::detail::UInt192 total{};
        const auto add_square = [&](const std::int64_t left, const std::int64_t right) {
            const auto d = left >= right ? static_cast<std::uint64_t>(left) - static_cast<std::uint64_t>(right)
                                         : static_cast<std::uint64_t>(right) - static_cast<std::uint64_t>(left);
            static_cast<void>(math::detail::add_magnitude(total, math::detail::multiply_u64(d, d)));
        };
        add_square(a.x.raw(), b.x.raw());
        add_square(a.y.raw(), b.y.raw());
        if (metric == RangeMetric::spatial) add_square(a.z.raw(), b.z.raw());
        return total;
    }

    // --- hardpoint opportunity world (space-targeting R-05 to R-12) ------------------------

    class Opportunity final : public OpportunityWorld {
    public:
        Opportunity(UnitCombat& owner, const std::uint32_t slot, CombatRandom& random)
            : owner_(owner), slot_(slot), random_(random) {}

        [[nodiscard]] std::size_t player_count() const override { return owner_.world_.players.size(); }
        [[nodiscard]] PlayerRelation relation(const std::size_t index) const override {
            const auto& player = owner_.world_.players[index];
            if (player.player_id == owner_.unit_.owner) return PlayerRelation::owner;
            return owner_.world_.hostile(owner_.unit_.owner, player.player_id) ? PlayerRelation::hostile : PlayerRelation::other;
        }
        [[nodiscard]] bool nebula_fogged(const std::size_t index) const override {
            return owner_.unit_.in_nebula && (owner_.unit_.visible_to & (std::uint64_t{1} << index)) == 0;
        }
        [[nodiscard]] std::vector<OpportunityCandidate> candidates(const std::size_t index) override {
            const auto& weapon = owner_.profile_.weapons[slot_];
            // R-07: box of half extent 1.1 x range on every axis, one player's objects.
            const auto half = math::Fixed::from_raw(weapon.range.raw() * 11 / 10);
            const auto ids = owner_.world_.candidates(owner_.unit_.position, half, owner_.world_.players[index].player_id);
            std::vector<OpportunityCandidate> result;
            result.reserve(ids.size());
            for (const auto id : ids) {
                const auto* target = owner_.world_.find(id);
                OpportunityCandidate candidate;
                candidate.id = id;
                candidate.suitable = target != nullptr && target->profile != nullptr
                    && target->profile->valid_target // R-08: valid-type admission precedes turret pointing
                    && owner_.hostile(*target) && owner_.visible_to_me(*target);
                // R-08: pads can be hostile and visible while living projectiles cannot hit
                // them. Reject them before priority and aim so they cannot retain the weapon.
                candidate.eligible = target != nullptr && target->profile != nullptr
                    && target->profile->living_projectile_collision && !owner_.restricted(weapon, *target);
                candidate.priority = target != nullptr ? owner_.priority(*target) : std::nullopt;
                result.push_back(candidate);
            }
            return result;
        }
        [[nodiscard]] bool aim_acceptable(const OpportunityCandidate& candidate) override {
            const auto& weapon = owner_.profile_.weapons[slot_];
            const auto* target = owner_.world_.find(candidate.id);
            if (target == nullptr) return false;
            const auto midpoint = owner_.world_point(owner_.unit_, owner_.local_midpoint(weapon));
            return owner_.choose_aim(weapon, midpoint, *target, random_, RangeMetric::spatial).has_value();
        }
        [[nodiscard]] bool attempt(const EntityId target, bool) override { return owner_.attempt(slot_, target, random_); }
        [[nodiscard]] std::uint32_t draw_player_start(const std::uint32_t count) override {
            return random_.uniform(0, count - 1U);
        }

    private:
        UnitCombat& owner_;
        std::uint32_t slot_;
        CombatRandom& random_;
    };

    // Service_Hard_Point_Weapon, one frame (docs/behaviour/space-weapon-fire.md W-rules).
    void service_weapon(const std::uint32_t slot);

    // --- ship-level target choice (docs/behaviour/space-weapon-fire.md T-rules) -------------

    struct Suitability {
        bool suitable{};
        std::optional<math::Fixed> priority{};
    };

    [[nodiscard]] math::Fixed member_attack_reach(const CombatUnit& target, const math::Vec3& from);

    [[nodiscard]] bool in_attack_range(const CombatUnit& target);

    [[nodiscard]] Suitability ship_suitable(const CombatUnit& target);

    [[nodiscard]] bool damaged(const CombatUnit& target);

    // Is_Better_Target (T-04).
    [[nodiscard]] bool better(const CombatUnit& candidate, const std::optional<math::Fixed> candidate_priority,
        const CombatUnit* current, const std::optional<math::Fixed> current_priority);

    // Scan_For_New_Target_In_Attack_Range (T-02, T-03).
    [[nodiscard]] std::pair<const CombatUnit*, std::optional<math::Fixed>> scan();

    // T-01: the drop runs for every unit. Without an attack distance FoC's scan finds nothing,
    // so the remake skips it (its next-scan frame is then never read).
    void ship_target();

    // --- turning to bring weapons to bear (docs/behaviour/space-weapon-fire.md A-04 to A-07) ----

    // Get_Total_Hard_Points: every hardpoint of the type, weapon or not.
    [[nodiscard]] std::size_t total_hardpoints() const noexcept {
        if (unit_.durability_profile != nullptr) return unit_.durability_profile->hardpoints.size();
        const auto weapons = static_cast<std::size_t>(std::count_if(profile_.weapons.begin(), profile_.weapons.end(),
            [](const WeaponProfile& weapon) { return weapon.hardpoint != object_weapon; }));
        return std::max(profile_.hardpoints.size(), weapons);
    }

    // Get_Max_Firepower_Z_Angle_Adjust (A-06): 0 to face the target, +90 to turn the right side
    // toward it, -90 the left, by the AI combat power each side's live weapon hardpoints bring.
    // `delta` is the bearing minus the unit's yaw, wrapped to [-180, 180).
    [[nodiscard]] std::int64_t firepower_adjust(motion_detail::Calc& calc, const math::Fixed delta);

    // Service_Space_Based_Targeting's facing branch (A-04 to A-07; research AT-01 to AT-13): a
    // unit at rest whose player-ordered target is within its attack distance turns in place
    // toward the target's nearest live hardpoint, adjusted for firepower, when that heading lies
    // more than 10 degrees from its own. FoC's Auto_Rotate_For_Space_Targeting is false, so
    // only an attack order (or a type without hardpoints) turns a unit.
    void face_target();

private:
    const CombatWorld& world_;
    const CombatUnit& unit_;
    const CombatProfile& profile_;
    CombatState state_;
    math::Fixed scan_divert_{};
    std::optional<math::Vec3> diversion_anchor_;
    EntityId formation_target_{};
    const FormationAttackContext* formation_context_{};
    std::vector<CombatEvent> events_;
    std::optional<math::Vec3> face_;
    math::Fixed energy_spent_{};
    std::vector<math::Vec3> aim_offsets_;
    std::vector<ManualFire> manual_fired_;
    std::vector<ManualFeedback> manual_feedback_;
    std::optional<core::Diagnostic> error_;
};


} // namespace eawr::sim::tactical::detail::combat_detail
