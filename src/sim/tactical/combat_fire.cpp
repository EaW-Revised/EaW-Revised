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

core::Result<CombatStep> step_planetary_fire(const CombatWorld& world, const CombatUnit& unit,
    const CombatUnit& target) {
    if (!unit.profile || !unit.profile->planetary_weapon || unit.profile->weapons.size() != 1)
        return core::Result<CombatStep>::success(CombatStep{});
    CombatState state;
    state.weapons.resize(1);
    auto source = unit;
    source.combat = &state;
    return UnitCombat(world, source).fire_planetary(target);
}

namespace combat_detail {

UnitCombat::UnitCombat(const CombatWorld& world, const CombatUnit& unit)
        : world_(world), unit_(unit), profile_(*unit.profile), state_(*unit.combat), charged_blast_(unit.charged_blast) {}

core::Result<CombatStep> UnitCombat::run() {
        if (state_.weapons.size() != profile_.weapons.size()) {
            return core::Result<CombatStep>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                "combat state does not match the type's weapons"));
        }
        ship_target();
        point_object_turret_target();
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
        // FS-33/FC-61: the later turret turn adjusts only when object tracking is off.
        for (std::uint32_t slot = 0; slot < profile_.weapons.size(); ++slot) {
            auto& turret = state_.weapons[slot].turret;
            // FC-60/61: a retained out-of-range target holds, without starting a rest timer.
            if (turret && turret->target == invalid_entity_id) idle_object_turret(profile_.weapons[slot], *turret);
        }
        if (error_) {
            return core::Result<CombatStep>::failure(std::move(*error_));
        }
        return core::Result<CombatStep>::success(
            CombatStep{std::move(state_), std::move(events_), face_, energy_spent_, std::move(aim_offsets_),
                std::move(manual_fired_), std::move(manual_feedback_), charged_blast_});
    }

core::Result<CombatStep> UnitCombat::fire_planetary(const CombatUnit& target) {
    CombatRandom random(world_.seed, world_.frame, unit_.id, 0);
    static_cast<void>(attempt(0, target.id, random, true, &target));
    if (error_) return core::Result<CombatStep>::failure(std::move(*error_));
    return core::Result<CombatStep>::success(CombatStep{std::move(state_), std::move(events_),
        {}, {}, std::move(aim_offsets_), {}, {}, charged_blast_});
}

bool UnitCombat::attempt(const std::uint32_t slot, const EntityId target_id, CombatRandom& random,
    const bool burst_begun, const CombatUnit* planetary_target) {
        if (!unit_.weapons_enabled) return false; // WR-44/46: covers direct/manual/burst fire too
        // WAD-34, FD-54: these gates read the budget after each earlier successful attempt.
        if (charged_blast_ && charged_blast_->charge_remaining != 0) return false;
        const bool charged = charged_blast_ && charged_blast_->shots_remaining != 0;
        if (charged && !profile_.weapons[slot].special) return false;
        std::optional<WeaponProfile> blast;
        if (charged && profile_.weapons[slot].charged_shot) {
            blast = profile_.weapons[slot];
            blast->shot = blast->charged_shot;
        }
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
        // WAD-35: charged selection precedes the ordinary unit-ability override.
        const auto& authored = ion ? *ion : barrage ? *barrage : blast ? *blast : profile_.weapons[slot];
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
        // WWP-49: only a parent with ordinary targeting substitutes its centre for the next
        // muzzle. A team's target metadata alone is not an ordinary combat binding.
        // A hardpoint weapon takes a random craft of the team each attempt (WWP-19).
        const bool own_weapon = weapon.hardpoint == object_weapon;
        const bool muzzled = own_weapon && !weapon.muzzles.empty();
        const auto muzzle_index = muzzled ? state_.weapons[slot].next_muzzle % weapon.muzzles.size() : std::size_t{0};
        const auto* parent = own_weapon ? world_.find(world_.container_of(unit_.id)) : nullptr;
        const auto reference = parent != nullptr && parent->combat != nullptr ? parent->position
            : muzzled ? world_point(unit_, weapon.muzzles[muzzle_index]) : unit_.position;
        const auto* held = planetary_target ? planetary_target : world_.find(target_id);
        const auto* target = planetary_target ? planetary_target
            : own_weapon                         ? world_.resolve_target(held, reference)
                                                 : world_.random_member(held, random);
        if (target == nullptr || target->profile == nullptr
            || (planetary_target && !stealth_targetable(*target))
            || (!planetary_target && (!visible_to_me(*target) || restricted(weapon, *target)))) {
            return false;
        }
        if (!planetary_target && !hostile(*target)) return false; // WHZ-51; WSW-20 continuation retains its target.
        if (weapon.object_turret_required) {
            const auto& turret = state_.weapons[slot].turret;
            if (!turret || !turret->ready) return false;
        }
        if (ion && target->id != unit_.ion_target && world_.container_of(target->id) != unit_.ion_target) {
            return false;
        }
        const auto midpoint = world_point(unit_, local_midpoint(weapon));
        // WWP-47: the object weapon's next muzzle bone, else the unit's origin. The shot leaves it,
        // the lead starts there and the weapon's cone opens about the unit's facing there (WWP-53).
        std::optional<Aim> aim;
        // WWP-49: a team container resolved to one of its craft has no aimed hardpoint; a hardpoint
        // weapon's random craft goes straight to the R-10 aim-point search (WWP-19).
        const bool resolved_craft = target != held;
        // A hardpoint weapon aims at the nearest live targetable hardpoint (WWP-16), else the
        // aim-point search. For the unit's own weapon the targeting service has attached the unit's
        // best hardpoint (WCC-18) to the attack target when it acquired it (G-W3, OR-24), so it is
        // the weapon's aimed hardpoint just like an ordered one (WWP-50).
        // OR-25: the ordered hardpoint of the ordered target, with no fallback to another point.
        if (!resolved_craft && target_id == state_.attack_target && state_.direct) {
            if (const auto* ordered = standing_hardpoint(*target, state_.attack_hardpoint)) {
                aim = Aim{world_point(*target, ordered->position), ordered->hardpoint};
            }
        }
        if (!aim && !resolved_craft) {
            // WWP-16/72: the hardpoint distance starts at the shooter's adjusted position.
            auto reference_point = unit_.position;
            reference_point.z = take(math::add(reference_point.z, profile_.ranged_target_z_adjust));
            const auto* chosen = own_weapon ? best_hardpoint(world_.table, &profile_, *target, unit_.position)
                                            : nearest_hardpoint(*target, reference_point);
            if (chosen != nullptr) aim = Aim{world_point(*target, chosen->position), chosen->hardpoint};
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
        if (!aim && own_weapon) {
            // WWP-50: no hardpoint to aim at: a keyed draw over the target type's Target_Bones (at
            // their bind positions until animated poses exist), else the height-adjusted position
            // (WWP-72). There is no range test in the choice; the reach test below is G-05's.
            if (!target->profile->target_bones.empty()) {
                const auto& bones = target->profile->target_bones;
                const auto pick = random.uniform(0, static_cast<std::uint32_t>(bones.size() - 1));
                aim = Aim{world_point(*target, bones[pick]), no_hardpoint};
            } else {
                auto point = target->position;
                point.z = take(math::add(point.z, target->profile->ranged_target_z_adjust));
                aim = Aim{point, no_hardpoint};
            }
        }
        if (!aim) {
            aim = choose_aim(weapon, midpoint, *target, random, RangeMetric::spatial);
            if (!aim) return false;
        }
        // W-05 / WWP-21: planar aim distance includes the target's loaded soft radius.
        const auto reach = take(math::add(weapon.range, target_soft_radius(world_.motion, *target)));
        if (!planetary_target && !within_range(midpoint, aim->point, reach, RangeMetric::planar)) {
            return false;
        }
        // WWP-47: the weapon takes its next muzzle and advances the index before the launch can fail
        // on its lead or cone (debug build), so a rejected shot still uses its muzzle up.
        if (muzzled) state_.weapons[slot].next_muzzle = static_cast<std::uint32_t>((muzzle_index + 1) % weapon.muzzles.size());
        auto origin = muzzled ? weapon.muzzles[muzzle_index] : weapon.fire_a;
        if (!muzzled && weapon.has_fire_b && random.uniform(0, 99) >= 50U) origin = weapon.fire_b;
        if (muzzled && state_.weapons[slot].turret) origin = object_turret_muzzle(weapon, *state_.weapons[slot].turret, origin);
        const auto muzzle = world_point(unit_, origin);
        // WWP-53: the object weapon's cone is tested at its muzzle; the range above stays the unit's.
        const auto cone_origin = muzzled ? muzzle : midpoint;
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
            if (!pointable(weapon, cone_origin, scattered)) return false;
        } else {
            if (!pointable(weapon, midpoint, *led)) return false;
            const auto planar = take(math::length(math::Vec2{
                take(math::subtract(aim->point.x, midpoint.x)), take(math::subtract(aim->point.y, midpoint.y))}));
            scattered = scatter(weapon, planar, *target, *led, random);
        }
        events_.push_back(CombatEvent{world_.frame, CombatEventKind::weapon_fired, unit_.id, weapon.hardpoint,
            target->id, aim->hardpoint, muzzle, scattered,
            (ion ? fired_ability_shot : barrage ? fired_barrage_shot : 0U) | (charged ? fired_charged_shot : 0U)});
        // FS-35: spend inside creation, so the next hardpoint can read ordinary state at zero.
        if (charged) --charged_blast_->shots_remaining;
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

math::Fixed UnitCombat::delay_factor(const WeaponProfile& weapon) const noexcept {
    if (!unit_.full_salvo || weapon.hardpoint == object_weapon || weapon.full_salvo_delay.raw() == one_raw) {
        return unit_.weapon_delay;
    }
    // Both factors are validated to [0, 64] x [-64, 64]: the exact product fits.
    const auto raw = unit_.weapon_delay.raw() * weapon.full_salvo_delay.raw();
    const auto half = one_raw / 2;
    return math::Fixed::from_raw(raw >= 0 ? (raw + half) / one_raw : -((-raw + half) / one_raw));
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
            state.countdown = scaled_weapon_delay(frames, delay_factor(weapon), true);
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
            state.countdown = scaled_weapon_delay(weapon.pulse_delay_frames, delay_factor(weapon), false);
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
        // WWP-05/FD-06: clearing a dead ordered target in ship targeting must
        // not turn this hardpoint's death-frame service into idle opportunity fire.
        const auto held = unit_.combat->attack_target;
        if ((unit_.combat->direct || unit_.target_prepared) && held != invalid_entity_id) {
            const auto* previous = world_.find(held);
            if (previous == nullptr || (previous->durability && previous->durability->hull.raw() <= 0)) return;
        }
        const auto attack = state_.direct || unit_.target_prepared ? state_.attack_target : invalid_entity_id;
        if (attack != invalid_entity_id) {
            const auto* target = world_.resolve_target(world_.find(attack), unit_.position);
            if (target == nullptr || restricted(weapon, *target)) return;
        }
        // R-01: an AI owner's quick-recharge hardpoint counts both flags as on.
        const bool forced = weapon.ai_opportunity_forced && unit_.ai_owned;
        if (attack == invalid_entity_id && !weapon.opportunity_when_idle && !forced) return;
        if (attack != invalid_entity_id && attempt(slot, attack, random)) return;
        if (attack != invalid_entity_id && !weapon.opportunity_when_targeting && !forced) return;
        Opportunity opportunity(*this, slot, random);
        OpportunityGates gates;
        gates.parent_suppressed = unit_.stealth_on; // R-05, WST-10
        const auto outcome = service_opportunity(world_.frame, gates, state.opportunity, opportunity);
        if (outcome.acquired) {
            // The acquisition event goes before the shot that confirmed it.
            events_.insert(events_.end() - 1, CombatEvent{world_.frame, CombatEventKind::target_acquired, unit_.id,
                weapon.hardpoint, state.opportunity.target, no_hardpoint, {}, {}});
        }
    }

} // namespace combat_detail
} // namespace detail
} // namespace eawr::sim::tactical
