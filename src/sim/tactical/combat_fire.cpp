#include "combat_algorithms.hpp"

namespace eawr::sim::tactical {
using detail::combat_detail::one_raw;


namespace detail {
using namespace combat_detail;

core::Result<CombatStep> step_combat(const CombatWorld& world, const CombatUnit& unit) {
    if (unit.profile == nullptr || unit.combat == nullptr) {
        return core::Result<CombatStep>::success(CombatStep{});
    }
    return UnitCombat(world, unit).run();
}

core::Result<bool> manual_target_admissible(const CombatWorld& world, const CombatUnit& unit,
    const CombatUnit& target, const std::uint32_t hardpoint) {
    if (unit.profile == nullptr || unit.combat == nullptr) return core::Result<bool>::success(false);
    return combat_detail::UnitCombat(world, unit).admit_manual(target, hardpoint);
}

namespace combat_detail {

UnitCombat::UnitCombat(const CombatWorld& world, const CombatUnit& unit)
        : world_(world), unit_(unit), profile_(*unit.profile), state_(*unit.combat) {}

core::Result<CombatStep> UnitCombat::run() {
        if (state_.weapons.size() != profile_.weapons.size()) {
            return core::Result<CombatStep>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                "combat state does not match the type's weapons"));
        }
        ship_target();
        // OR-23: an ordered hardpoint lasts while the order's target stands and so does it; then
        // the unit goes back to picking the target's nearest hardpoint (OR-24).
        if (state_.attack_hardpoint != no_hardpoint) {
            const auto* ordered = state_.direct && state_.attack_target != invalid_entity_id
                ? world_.resolve_target(world_.find(state_.attack_target), unit_.position)
                : nullptr;
            if (ordered == nullptr || standing_hardpoint(*ordered, state_.attack_hardpoint) == nullptr) {
                state_.attack_hardpoint = no_hardpoint;
            }
        }
        face_target();
        for (std::uint32_t slot = 0; slot < profile_.weapons.size(); ++slot) {
            service_weapon(slot);
        }
        if (error_) {
            return core::Result<CombatStep>::failure(std::move(*error_));
        }
        return core::Result<CombatStep>::success(
            CombatStep{std::move(state_), std::move(events_), face_, energy_spent_, std::move(aim_offsets_),
                std::move(manual_fired_), std::move(manual_feedback_)});
    }

bool UnitCombat::attempt(const std::uint32_t slot, const EntityId target_id, CombatRandom& random, const bool burst_begun) {
        // AB-66 (#561): while the squadron's ION_CANNON_SHOT is on, a craft's hardpoint fires only
        // the override shot, at the ion target, until the craft has fired it; otherwise nothing.
        std::optional<WeaponProfile> ion;
        std::optional<WeaponProfile> barrage;
        if (unit_.ion_held) {
            if (!unit_.ion_armed || !profile_.weapons[slot].ability_shot) return false;
            ion = profile_.weapons[slot];
            ion->shot = ion->ability_shot;
        }
        if (!ion && unit_.barrage_target != invalid_entity_id && target_id == unit_.barrage_target) {
            if (!profile_.weapons[slot].barrage_shot) return false;
            barrage = profile_.weapons[slot];
            barrage->shot = barrage->barrage_shot;
        }
        const auto& authored = ion ? *ion : barrage ? *barrage : profile_.weapons[slot];
        const auto& assignment = state_.weapons[slot].manual;
        std::optional<WeaponProfile> posed;
        if (authored.requires_manual_target && assignment && authored.manual_turret) {
            posed = manual_pose(authored, *assignment);
        }
        const auto& weapon = posed ? *posed : authored;
        // WAD-31/39: a missing selected SPECIAL projectile produces no shot;
        // manual-only guns wait for their dedicated assignment consumer.
        if (weapon.special && !weapon.shot) return false;
        if (weapon.requires_manual_target && (!assignment || assignment->target != target_id)) return false;
        // #424: a squadron's team container is fired at through its craft nearest this unit.
        const auto* target = world_.resolve_target(world_.find(target_id), unit_.position);
        if (target == nullptr || target->profile == nullptr || !visible_to_me(*target) || restricted(weapon, *target)) {
            return false;
        }
        if (!hostile(*target)) return false; // WHZ-51: retained and direct targets use the same gate
        if (ion && target->id != unit_.ion_target && world_.container_of(target->id) != unit_.ion_target) {
            return false;
        }
        const auto midpoint = world_point(unit_, local_midpoint(weapon));
        // The nearest live targetable hardpoint (spatial distance from the shooter, first on a
        // tie), else the aim-point search.
        std::optional<Aim> aim;
        // OR-25: the ordered hardpoint of the ordered target, with no fallback to another point.
        if (target_id == state_.attack_target && state_.direct) {
            if (const auto* ordered = standing_hardpoint(*target, state_.attack_hardpoint)) {
                aim = Aim{world_point(*target, ordered->position), ordered->hardpoint};
            }
        }
        if (!aim && target->profile != nullptr) {
            const TargetHardpoint* nearest = nullptr;
            math::Vec3 nearest_point{};
            for (const auto& hardpoint : target->profile->hardpoints) {
                if (!hardpoint.targetable || hardpoint_destroyed_on(*target, hardpoint.hardpoint)) continue;
                const auto point = world_point(*target, hardpoint.position);
                if (nearest == nullptr || closer(unit_.position, point, nearest_point)) {
                    nearest = &hardpoint;
                    nearest_point = point;
                }
            }
            if (nearest != nullptr) aim = Aim{nearest_point, nearest->hardpoint};
            // AB-61: an ion shot at a hardpoint aims at it while it stands (else as above).
            if (ion && unit_.ion_target_hardpoint != no_hardpoint) {
                for (const auto& hardpoint : target->profile->hardpoints) {
                    if (hardpoint.hardpoint != unit_.ion_target_hardpoint || !hardpoint.targetable
                        || hardpoint_destroyed_on(*target, hardpoint.hardpoint)) {
                        continue;
                    }
                    aim = Aim{world_point(*target, hardpoint.position), hardpoint.hardpoint};
                }
            }
        }
        if (!aim) {
            aim = choose_aim(weapon, midpoint, *target, random, RangeMetric::spatial);
            if (!aim) return false;
        }
        // W-05 / WWP-21: planar aim distance includes the target's loaded soft radius.
        const auto reach = take(math::add(weapon.range, target_soft_radius(world_.motion, *target)));
        if (!within_range(midpoint, aim->point, reach, RangeMetric::planar)) {
            return false;
        }
        auto origin = weapon.fire_a;
        if (weapon.has_fire_b && random.uniform(0, 99) >= 50U) origin = weapon.fire_b;
        const auto muzzle = world_point(unit_, origin);
        // W-10, W-11: the shot leads the target; the weapon must point at the led point.
        const auto led = lead(weapon, *target, aim->point, muzzle);
        if (!led) return false;
        math::Vec3 scattered{};
        if (weapon.hardpoint == object_weapon) {
            // DG-24, W-09: the object weapon scatters by the spatial distance from its muzzle to
            // the led point, and its cone is tested on the scattered point.
            const auto spatial = take(math::length(math::Vec3{take(math::subtract(led->x, muzzle.x)),
                take(math::subtract(led->y, muzzle.y)), take(math::subtract(led->z, muzzle.z))}));
            scattered = scatter(weapon, spatial, *target, *led, random);
            if (!pointable(weapon, midpoint, scattered)) return false;
        } else {
            if (!pointable(weapon, midpoint, *led)) return false;
            const auto planar = take(math::length(math::Vec2{
                take(math::subtract(aim->point.x, midpoint.x)), take(math::subtract(aim->point.y, midpoint.y))}));
            scattered = scatter(weapon, planar, *target, *led, random);
        }
        events_.push_back(CombatEvent{world_.frame, CombatEventKind::weapon_fired, unit_.id, weapon.hardpoint,
            target->id, aim->hardpoint, muzzle, scattered, ion ? fired_ability_shot : barrage ? fired_barrage_shot : 0U});
        // MS-03: the scatter off the led point, taken into the target's frame for a homing
        // projectile to keep.
        const math::Vec3 off{take(math::subtract(scattered.x, led->x)),
            take(math::subtract(scattered.y, led->y)), take(math::subtract(scattered.z, led->z))};
        const auto& m = target->transform.rows;
        const auto column = [&](const std::size_t j) {
            return take(math::add(take(math::add(take(math::multiply(m[0][j], off.x)), take(math::multiply(m[1][j], off.y)))),
                take(math::multiply(m[2][j], off.z))));
        };
        aim_offsets_.push_back(math::Vec3{column(0), column(1), column(2)});
        // W-06: a hardpoint spends its pulse with the shot; the object weapon only the first of a
        // burst, whose later pulses it spends before it tries them (W-06a, service_weapon).
        auto& state = state_.weapons[slot];
        if (weapon.hardpoint != object_weapon || !burst_begun) {
            spend_pulse(weapon, state, random);
        }
        return true;
    }

void UnitCombat::spend_pulse(const WeaponProfile& weapon, WeaponState& state, CombatRandom& random) {
        if (state.pulses_left != 0) --state.pulses_left;
        // IS-06: an ion stun's fire rate lengthens a hardpoint's recharge and gaps and shortens its
        // next burst; 1 without a stun changes nothing.
        const bool stunned = weapon.hardpoint != object_weapon && unit_.fire_rate.raw() != math::Fixed::scale;
        const auto object_rate = unit_.object_fire_rate;
        if (state.pulses_left == 0) {
            const auto hundredths = random.uniform(weapon.min_recharge_hundredths, weapon.max_recharge_hundredths);
            auto frames = hundredths * logical_frames_per_second / 100U;
            if (weapon.hardpoint == object_weapon) frames += random.uniform(0, object_recharge_jitter_frames);
            state.countdown = scaled_weapon_delay(frames, unit_.weapon_delay, true);
            state.pulses_left = weapon.pulse_count;
            // WAD-38: object weapons divide the full recharge by the mode's rate and
            // truncate the refilled pulse count; their jitter is drawn before scaling.
            if (weapon.hardpoint == object_weapon && object_rate.raw() != math::Fixed::scale) {
                state.countdown = object_rate.raw() > 0
                    ? static_cast<std::uint32_t>(static_cast<std::int64_t>(state.countdown) * one_raw / object_rate.raw()) : 0;
                state.pulses_left = static_cast<std::uint32_t>(
                    static_cast<std::int64_t>(weapon.pulse_count) * std::max<std::int64_t>(0, object_rate.raw()) / one_raw);
            }
            if (stunned) {
                state.countdown = fire_rate_recharge(state.countdown, unit_.fire_rate);
                state.pulses_left = fire_rate_pulses(weapon.pulse_count, unit_.fire_rate);
            }
        } else {
            state.countdown = scaled_weapon_delay(weapon.pulse_delay_frames, unit_.weapon_delay, false);
            if (weapon.hardpoint == object_weapon && object_rate.raw() != math::Fixed::scale) {
                state.countdown = object_rate.raw() > 0
                    ? static_cast<std::uint32_t>(static_cast<std::int64_t>(state.countdown) * one_raw / object_rate.raw()) : 0;
                if (object_rate.raw() <= 0) state.pulses_left = 0;
            }
            if (stunned) {
                state.countdown = fire_rate_gap(state.countdown, unit_.fire_rate);
                if (unit_.fire_rate.raw() <= 0) state.pulses_left = 0;
            }
        }
    }

void UnitCombat::service_weapon(const std::uint32_t slot) {
        const auto& weapon = profile_.weapons[slot];
        auto& state = state_.weapons[slot];
        // A target that has left the session is dropped at once, whatever the countdown.
        if (state.opportunity.target != invalid_entity_id && world_.find(state.opportunity.target) == nullptr) {
            state.opportunity.target = invalid_entity_id;
        }
        // MC-03: object detachment clears the request independently of weapon readiness.
        if (state.manual && state.manual->target != invalid_entity_id && world_.find(state.manual->target) == nullptr) {
            state.manual->target = invalid_entity_id;
            state.manual->requesting_player = 0;
            state.manual->assigned_frame = 0;
        }
        // MC-06: mechanical service is independent of the weapon's firing disable gates.
        if (state.manual && weapon.manual_turret && !hardpoint_destroyed_on(unit_, weapon.hardpoint)) {
            manual_turret(weapon, *state.manual);
        }
        if (!weapon_up(weapon)) return;
        if (state.countdown != 0) {
            --state.countdown;
            if (state.countdown != 0) return;
        }
        CombatRandom random(world_.seed, world_.frame, unit_.id, slot);
        if (weapon.requires_manual_target) {
            if (!state.manual || state.manual->target == invalid_entity_id) return;
            auto& manual = *state.manual;
            const auto* target = world_.find(manual.target);
            if (target == nullptr) return;
            // WAD-39: state eligibility owns the timeout. Range, fog and cone are later
            // attempt gates and cannot manufacture timeout feedback.
            const bool eligible = target->profile != nullptr && !restricted(weapon, *target)
                && (target->durability == nullptr || target->durability->hull.raw() > 0);
            if (!eligible) {
                if (world_.frame > manual.assigned_frame && world_.frame - manual.assigned_frame > 300) {
                    manual_feedback_.push_back({manual.requesting_player, weapon.hardpoint});
                    manual.target = invalid_entity_id;
                    manual.requesting_player = 0;
                    manual.assigned_frame = 0;
                }
                return;
            }
            const auto requesting_player = manual.requesting_player;
            if (attempt(slot, manual.target, random)) {
                if (state.pulses_left == weapon.pulse_count) {
                    manual_fired_.push_back({requesting_player, weapon.manual_cooldown_frames});
                }
                manual.target = invalid_entity_id;
                manual.requesting_player = 0;
                manual.assigned_frame = 0;
            }
            return;
        }
        // W-06a: once its burst has begun, the object weapon spends each later pulse when its
        // countdown runs out, before it looks for a target, whether or not the shot then fires.
        const bool burst_begun = weapon.hardpoint == object_weapon && state.pulses_left != weapon.pulse_count;
        if (burst_begun) spend_pulse(weapon, state, random);
        // FT-07: an idle squadron craft holds fire; its countdowns keep running.
        if (unit_.squadron_idle) return;
        if (weapon.hardpoint == object_weapon) {
            // The unit's own weapon fires at its ship-level target. EN-05: a shot that costs
            // energy waits, with its countdown at zero and its burst unspent, until the pool holds
            // the cost; a unit without a pool never fires it.
            if (state_.attack_target == invalid_entity_id) return;
            // WAD-38/EN-05: energy admission uses the projectile selected for this target.
            const auto& selected = !unit_.ion_held && unit_.barrage_target != invalid_entity_id
                    && state_.attack_target == unit_.barrage_target
                ? weapon.barrage_shot : weapon.shot;
            const auto cost = selected ? selected->energy_per_shot : math::Fixed{};
            if (cost.raw() > 0) {
                if (world_.damage == nullptr || unit_.durability_profile == nullptr || unit_.durability == nullptr) return;
                auto pool = *unit_.durability;
                pool.energy = math::Fixed::from_raw(pool.energy.raw() - energy_spent_.raw());
                if (!draw_energy(*unit_.durability_profile, *world_.damage, pool, cost)) return;
            }
            if (attempt(slot, state_.attack_target, random, burst_begun)) {
                energy_spent_ = math::Fixed::from_raw(energy_spent_.raw() + cost.raw());
            }
            return;
        }
        // WSQ-47: the team setter also hands an autonomous target to its members' hardpoints.
        // Ordinary ships retain their player-order gate.
        const auto attack = state_.direct || unit_.target_prepared ? state_.attack_target : invalid_entity_id;
        if (attack != invalid_entity_id) {
            const auto* target = world_.resolve_target(world_.find(attack), unit_.position);
            if (target == nullptr || restricted(weapon, *target)) return;
        }
        if (attack == invalid_entity_id && !weapon.opportunity_when_idle) return;
        if (attack != invalid_entity_id && attempt(slot, attack, random)) return;
        if (attack != invalid_entity_id && !weapon.opportunity_when_targeting) return;
        Opportunity opportunity(*this, slot, random);
        const auto outcome = service_opportunity(world_.frame, OpportunityGates{}, state.opportunity, opportunity);
        if (outcome.acquired) {
            // The acquisition event goes before the shot that confirmed it.
            events_.insert(events_.end() - 1, CombatEvent{world_.frame, CombatEventKind::target_acquired, unit_.id,
                weapon.hardpoint, state.opportunity.target, no_hardpoint, {}, {}});
        }
    }

} // namespace combat_detail
} // namespace detail
} // namespace eawr::sim::tactical
