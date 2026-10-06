#include "combat_algorithms.hpp"

namespace eawr::sim::tactical {
using detail::combat_detail::one_raw;

std::optional<math::Vec3> lead_point(
    const math::Vec3 aim, const math::Vec3 muzzle, const math::Vec3 velocity, const math::Fixed speed) {
    if (speed.raw() <= 0 || (velocity.x.raw() == 0 && velocity.y.raw() == 0 && velocity.z.raw() == 0)) return aim;
    motion_detail::Calc calc;
    // In frames and projectile flights: |u + w t| = t with u = (aim - muzzle) / speed and
    // w = velocity / speed, so (w.w - 1) t^2 + 2 (u.w) t + u.u = 0.
    const auto per_shot = [&](const math::Vec3& value) {
        return math::Vec3{calc.div(value.x, speed), calc.div(value.y, speed), calc.div(value.z, speed)};
    };
    const auto dot = [&](const math::Vec3& l, const math::Vec3& r) {
        return calc.add(calc.add(calc.mul(l.x, r.x), calc.mul(l.y, r.y)), calc.mul(l.z, r.z));
    };
    const auto u = per_shot(math::Vec3{calc.sub(aim.x, muzzle.x), calc.sub(aim.y, muzzle.y), calc.sub(aim.z, muzzle.z)});
    const auto w = per_shot(velocity);
    const auto a = calc.sub(dot(w, w), math::Fixed::from_raw(one_raw));
    const auto b = calc.mul(dot(u, w), math::Fixed::from_raw(2 * one_raw));
    const auto c = dot(u, u);
    std::optional<math::Fixed> time;
    if (a.raw() == 0) {
        if (b.raw() != 0) {
            const auto root = calc.div(calc.neg(c), b);
            if (root.raw() >= 0) time = root;
        }
    } else {
        const auto discriminant = calc.sub(calc.mul(b, b), calc.mul(calc.mul(a, c), math::Fixed::from_raw(4 * one_raw)));
        if (discriminant.raw() >= 0) {
            const auto root = calc.sqrt(discriminant);
            const auto twice = calc.add(a, a);
            const auto first = calc.div(calc.add(calc.neg(b), root), twice);
            const auto second = calc.div(calc.sub(calc.neg(b), root), twice);
            if (first.raw() > 0 && second.raw() > 0) {
                time = std::min(first, second);
            } else if (first.raw() > 0) {
                time = first;
            } else if (second.raw() > 0) {
                time = second;
            }
        }
    }
    if (!calc.ok()) return aim;
    if (!time) return std::nullopt;
    const math::Vec3 point{calc.add(aim.x, calc.mul(velocity.x, *time)), calc.add(aim.y, calc.mul(velocity.y, *time)),
        calc.add(aim.z, calc.mul(velocity.z, *time))};
    return calc.ok() ? point : aim;
}


namespace detail {
using namespace combat_detail;

const TargetHardpoint* standing_hardpoint(const CombatUnit& target, const std::uint32_t index) {
    if (index == no_hardpoint || target.profile == nullptr) return nullptr;
    for (const auto& hardpoint : target.profile->hardpoints) {
        if (hardpoint.hardpoint != index) continue;
        if (!hardpoint.targetable) return nullptr;
        if (target.durability_profile != nullptr && target.durability != nullptr
            && index < target.durability_profile->hardpoints.size()
            && tactical::hardpoint_destroyed(*target.durability_profile, *target.durability, index)) {
            return nullptr;
        }
        return &hardpoint;
    }
    return nullptr;
}

math::Vec3 ordered_aim_point(const CombatUnit& target, const math::Vec3& from, const std::uint32_t ordered_index) {
    auto aim = target.position;
    if (target.profile == nullptr) return aim;
    if (const auto* ordered = standing_hardpoint(target, ordered_index)) {
        if (auto point = math::transform_point(target.transform, ordered->position)) return point.value();
    }
    bool found = false;
    for (const auto& hardpoint : target.profile->hardpoints) {
        if (!hardpoint.targetable) continue;
        if (target.durability_profile != nullptr && target.durability != nullptr
            && hardpoint.hardpoint < target.durability_profile->hardpoints.size()
            && hardpoint_destroyed(*target.durability_profile, *target.durability, hardpoint.hardpoint)) {
            continue;
        }
        auto point = math::transform_point(target.transform, hardpoint.position);
        if (!point) continue;
        if (!found || UnitCombat::closer(from, point.value(), aim)) {
            aim = point.value();
            found = true;
        }
    }
    return aim;
}


namespace combat_detail {

math::Vec3 UnitCombat::manual_angles(const ManualTurretProfile& turret, const math::Vec3& point) {
    const auto origin = world_point(unit_, turret.coordinate_pivot);
    const math::Vec3 delta{take(math::subtract(point.x, origin.x)), take(math::subtract(point.y, origin.y)),
        take(math::subtract(point.z, origin.z))};
    const auto& m = unit_.transform.rows;
    const auto axis = [&](const std::size_t column) {
        return take(math::dot(math::Vec3{m[0][column], m[1][column], m[2][column]}, delta));
    };
    const math::Vec3 local{axis(0), axis(1), axis(2)};
    const auto x = take(math::dot(turret.coordinate_axes[0], local));
    const auto y = take(math::dot(turret.coordinate_axes[1], local));
    const auto z = take(math::dot(turret.coordinate_axes[2], local));
    const auto yaw = x.raw() == 0 && y.raw() == 0 ? math::Fixed{} : take(math::atan2_turn(y, x));
    const auto planar = take(math::length(math::Vec2{x, y}));
    const auto pitch = planar.raw() == 0 && z.raw() == 0 ? math::Fixed{} : take(math::atan2_turn(z, planar));
    // The sim uses X-forward bearings; source pitch about Y is positive down (FM-02).
    return {{}, math::Fixed::from_raw(-pitch.raw() * 360), math::Fixed::from_raw(yaw.raw() * 360)};
}

core::Result<bool> UnitCombat::admit_manual(const CombatUnit& target, const std::uint32_t hardpoint) {
    const auto slot = std::find_if(profile_.weapons.begin(), profile_.weapons.end(),
        [&](const WeaponProfile& weapon) { return weapon.hardpoint == hardpoint; });
    if (slot == profile_.weapons.end()) return core::Result<bool>::success(false);
    const auto index = static_cast<std::size_t>(slot - profile_.weapons.begin());
    if (!slot->requires_manual_target || slot->hardpoint == object_weapon || slot->pulse_count != 1
        || !slot->shot || index >= state_.weapons.size() || !state_.weapons[index].manual
        || state_.weapons[index].manual->target != invalid_entity_id
        || (slot->manual_turret_required && !slot->manual_turret)
        || target.profile == nullptr || !hostile(target) || !visible_to_me(target) || restricted(*slot, target)) {
        return core::Result<bool>::success(false);
    }
    const auto posed = manual_pose(*slot, *state_.weapons[index].manual);
    const auto midpoint = world_point(unit_, local_midpoint(posed));
    bool points = true;
    if (slot->manual_turret) {
        // WAD-39: turret admission tests yaw extent only, not current fire cone or pitch.
        points = std::llabs(manual_angles(*slot->manual_turret, target.position).z.raw())
            <= slot->manual_turret->yaw_extent.raw();
    } else points = pointable(posed, midpoint, target.position);
    const auto distance = squared(midpoint, target.position, RangeMetric::planar);
    const auto minimum = squared({}, {slot->manual_min_range, {}, {}}, RangeMetric::planar);
    const bool accepted = points && within_range(midpoint, target.position, slot->range, RangeMetric::planar)
        && math::detail::compare(distance, minimum) >= 0;
    if (error_) return core::Result<bool>::failure(*error_);
    return core::Result<bool>::success(accepted);
}

math::Vec3 UnitCombat::manual_rotate(const math::Vec3& value, const math::Vec3& pivot,
    const std::array<math::Vec3, 3>& axes, const math::Fixed angle, const bool pitch, const bool direction) {
    if (angle.raw() == 0) return value;
    const auto delta = direction ? value : math::Vec3{take(math::subtract(value.x, pivot.x)),
        take(math::subtract(value.y, pivot.y)), take(math::subtract(value.z, pivot.z))};
    const auto x = take(math::dot(axes[0], delta));
    const auto y = take(math::dot(axes[1], delta));
    const auto z = take(math::dot(axes[2], delta));
    const auto trig = math::sin_cos_turn(math::Fixed::from_raw(angle.raw() / 360));
    const auto mul = [&](const math::Fixed a, const math::Fixed b) { return take(math::multiply(a, b)); };
    const math::Vec3 rotated = pitch
        ? math::Vec3{take(math::add(mul(trig.cosine, x), mul(trig.sine, z))), y,
            take(math::subtract(mul(trig.cosine, z), mul(trig.sine, x)))}
        : math::Vec3{take(math::subtract(mul(trig.cosine, x), mul(trig.sine, y))),
            take(math::add(mul(trig.sine, x), mul(trig.cosine, y))), z};
    const auto component = [&](const std::size_t index) {
        const auto get = [&](const math::Vec3& v) { return index == 0 ? v.x : index == 1 ? v.y : v.z; };
        auto sum = take(math::add(take(math::add(mul(get(axes[0]), rotated.x), mul(get(axes[1]), rotated.y))),
            mul(get(axes[2]), rotated.z)));
        return direction ? sum : take(math::add(sum, get(pivot)));
    };
    return {component(0), component(1), component(2)};
}

WeaponProfile UnitCombat::manual_pose(const WeaponProfile& weapon, const ManualWeaponState& state) {
    auto posed = weapon;
    if (!weapon.manual_turret) return posed;
    const auto& turret = *weapon.manual_turret;
    const auto rotate = [&](math::Vec3 value, const bool direction) {
        if (turret.barrel) value = manual_rotate(value, turret.barrel_pivot, turret.barrel_axes, state.pitch, true, direction);
        return manual_rotate(value, turret.pivot, turret.axes, state.yaw, false, direction);
    };
    posed.fire_a = rotate(weapon.fire_a, false);
    posed.fire_b = rotate(weapon.fire_b, false);
    if (weapon.fire_axes) {
        auto axes = *weapon.fire_axes;
        for (auto& axis : axes) axis = rotate(axis, true);
        posed.fire_axes = axes;
    }
    return posed;
}

void UnitCombat::manual_turret(const WeaponProfile& weapon, ManualWeaponState& state) {
    const auto& turret = *weapon.manual_turret;
    const auto wrap = [](const std::int64_t raw) {
        constexpr auto circle = 360 * one_raw;
        auto value = raw % circle;
        if (value >= circle / 2) value -= circle;
        if (value < -circle / 2) value += circle;
        return value;
    };
    if (const auto* target = world_.find(state.target)) {
        auto desired = manual_angles(turret, target->position);
        const auto clamp = [&](const math::Fixed angle, const math::Fixed extent, const math::Fixed offset) {
            auto value = wrap(angle.raw());
            if (extent.raw() > 0 && extent.raw() < 180 * one_raw) {
                value = wrap(std::clamp(value, -extent.raw(), extent.raw()) + offset.raw());
            }
            return math::Fixed::from_raw(value);
        };
        state.desired_yaw = clamp(desired.z, turret.yaw_extent, turret.offset.z);
        state.desired_pitch = clamp(desired.y, turret.pitch_extent, turret.offset.y);
    }
    const auto move = [&](const std::int64_t current, const std::int64_t desired) {
        return current + std::clamp(desired - current, -turret.speed.raw(), turret.speed.raw());
    };
    state.pitch = math::Fixed::from_raw(move(state.pitch.raw(), state.desired_pitch.raw()));
    // WAD-40: retail applies the linear yaw step, then another wrapped residual step.
    // Both use the authored per-frame speed; keep the second step rather than simplifying it.
    auto yaw = move(state.yaw.raw(), state.desired_yaw.raw());
    yaw += std::clamp(wrap(state.desired_yaw.raw() - yaw), -turret.speed.raw(), turret.speed.raw());
    state.yaw = math::Fixed::from_raw(wrap(yaw));
}

[[nodiscard]] bool UnitCombat::pointable(const WeaponProfile& weapon, const math::Vec3& midpoint, const math::Vec3& point) {
        const bool turret = weapon.hardpoint == object_weapon;
        if (turret && (weapon.cone_width.raw() == 0 || weapon.cone_height.raw() == 0)) {
            return true;
        }
        const math::Vec3 delta{take(math::subtract(point.x, midpoint.x)), take(math::subtract(point.y, midpoint.y)),
            take(math::subtract(point.z, midpoint.z))};
        const auto& m = unit_.transform.rows;
        const auto axis = [&](const std::size_t column) {
            return take(math::dot(math::Vec3{m[0][column], m[1][column], m[2][column]}, delta));
        };
        math::Vec3 local{axis(0), axis(1), axis(2)};
        if (!turret && weapon.fire_axes) {
            const auto& bone = *weapon.fire_axes;
            local = math::Vec3{take(math::dot(bone[0], local)), take(math::dot(bone[1], local)),
                take(math::dot(bone[2], local))};
        }
        const auto x = local.x;
        const auto y = local.y;
        const auto z = local.z;
        // FoC's facing takes no atan2 without a planar part: a point on the bone's z axis has yaw 0
        // and pitch +-90 degrees, the weapon midpoint itself yaw and pitch 0 (research WA-14).
        const auto yaw = x.raw() == 0 && y.raw() == 0 ? math::Fixed{} : take(math::atan2_turn(y, x));
        const auto planar = take(math::length(math::Vec2{x, y}));
        const auto pitch = planar.raw() == 0 && z.raw() == 0 ? math::Fixed{} : take(math::atan2_turn(z, planar));
        // |angle in turns| <= cone degrees / 720 (a turret extent: / 360), compared exactly on raw values.
        const std::int64_t turn_degrees = turret ? 360 : 720;
        return std::llabs(yaw.raw()) * turn_degrees <= weapon.cone_width.raw()
            && std::llabs(pitch.raw()) * turn_degrees <= weapon.cone_height.raw();
    }

[[nodiscard]] std::optional<UnitCombat::Aim> UnitCombat::choose_aim(const WeaponProfile& weapon, const math::Vec3& midpoint,
        const CombatUnit& target, CombatRandom& random, const RangeMetric metric) {
        const auto radius = target_soft_radius(world_.motion, target);
        // R-11: bone/hardpoint aim adds radius only for a parent with an object projectile;
        // fallback centre aim always adds it. Keep the weapon's exact cone unchanged.
        const bool parent_projectile = std::any_of(profile_.weapons.begin(), profile_.weapons.end(),
            [](const WeaponProfile& entry) { return entry.hardpoint == object_weapon && entry.shot.has_value(); });
        const auto acceptable = [&](const math::Vec3& point, const bool fallback = false) {
            const auto reach = parent_projectile || fallback ? take(math::add(weapon.range, radius)) : weapon.range;
            return within_range(midpoint, point, reach, metric) && pointable(weapon, midpoint, point);
        };
        if (target.profile != nullptr) {
            for (const auto& bone : target.profile->target_bones) {
                const auto point = world_point(target, bone);
                if (acceptable(point)) return Aim{point, no_hardpoint};
            }
            const auto& hardpoints = target.profile->hardpoints;
            if (!hardpoints.empty()) {
                const auto start = random.uniform(0, static_cast<std::uint32_t>(hardpoints.size() - 1));
                for (std::size_t step = 0; step < hardpoints.size(); ++step) {
                    const auto& hardpoint = hardpoints[(start + step) % hardpoints.size()];
                    if (hardpoint_destroyed_on(target, hardpoint.hardpoint)) continue;
                    const auto point = world_point(target, hardpoint.position);
                    if (acceptable(point)) return Aim{point, hardpoint.hardpoint};
                }
            }
        }
        // WWP-19/50/72: fallback raises the aim in world Z, without moving the target model.
        auto fallback = target.position;
        if (target.profile != nullptr) fallback.z = take(math::add(fallback.z, target.profile->ranged_target_z_adjust));
        if (acceptable(fallback, true)) return Aim{fallback, no_hardpoint};
        return std::nullopt;
    }

[[nodiscard]] std::optional<math::Vec3> UnitCombat::lead(
        const WeaponProfile& weapon, const CombatUnit& target, const math::Vec3& aim, const math::Vec3& muzzle) {
        if (!weapon.shot || weapon.shot->speed.raw() <= 0) return aim;
        const math::Vec3 velocity{take(math::subtract(target.position.x, target.previous_position.x)),
            take(math::subtract(target.position.y, target.previous_position.y)),
            take(math::subtract(target.position.z, target.previous_position.z))};
        return lead_point(aim, muzzle, velocity, weapon.shot->speed);
    }

[[nodiscard]] math::Vec3 UnitCombat::scatter(const WeaponProfile& weapon, const math::Fixed distance, const CombatUnit& target,
        const math::Vec3& point, CombatRandom& random) {
        if (!weapon.shot || weapon.range.raw() <= 0) {
            return point;
        }
        if (unit_.fixed_inaccuracy && unit_.barrage_target == target.id) {
            const auto radius = unit_.fixed_inaccuracy->raw();
            return math::Vec3{
                take(math::add(point.x, math::Fixed::from_raw(random.symmetric_raw(radius)))),
                take(math::add(point.y, math::Fixed::from_raw(random.symmetric_raw(radius)))),
                take(math::add(point.z, math::Fixed::from_raw(random.symmetric_raw(radius))))};
        }
        const auto row = std::find_if(weapon.shot->inaccuracy.begin(), weapon.shot->inaccuracy.end(),
            [&](const InaccuracyRow& entry) { return (entry.category_bits & categories(target)) != 0U; });
        if (row == weapon.shot->inaccuracy.end() || row->distance.raw() <= 0) {
            return point;
        }
        const auto doubled = math::Fixed::from_raw(row->distance.raw() * 2); // validated to 2^18 units, so at most 2^19
        const auto radius = take(math::divide(take(math::multiply(distance, doubled)), weapon.range));
        const auto offset = [&](const math::Fixed value) {
            return take(math::add(value, math::Fixed::from_raw(random.symmetric_raw(radius.raw()))));
        };
        const auto x = offset(point.x);
        const auto y = offset(point.y);
        const auto z = offset(point.z);
        return math::Vec3{x, y, z};
    }

[[nodiscard]] std::int64_t UnitCombat::firepower_adjust(motion_detail::Calc& calc, const math::Fixed delta) {
        if (total_hardpoints() == 0) return 0;
        math::Fixed forward{};
        math::Fixed left{};
        math::Fixed right{};
        for (const auto& weapon : profile_.weapons) {
            if (weapon.hardpoint == object_weapon || weapon.special || !weapon_up(weapon)
                || !weapon.shot || !weapon.shot->hitpoint_damage) {
                continue;
            }
            // The XY direction of Fire_Bone_A's x axis in the unit's frame (+X ahead, +Y left);
            // without a planar part FoC's facing has yaw 0.
            auto x = math::Fixed::from_raw(one_raw);
            auto y = math::Fixed{};
            if (weapon.fire_axes && ((*weapon.fire_axes)[0].x.raw() != 0 || (*weapon.fire_axes)[0].y.raw() != 0)) {
                x = (*weapon.fire_axes)[0].x;
                y = (*weapon.fire_axes)[0].y;
            }
            const auto planar = calc.length(x, y);
            const auto half = std::clamp<std::int64_t>(weapon.cone_width.raw(), 0, 180 * one_raw) / 2;
            // A direction is covered when the vector's cosine to it is at least cos(half the cone).
            const auto limit = calc.mul(calc.cos_deg(math::Fixed::from_raw(half)), planar);
            if (x >= limit) forward = calc.add(forward, weapon.ai_combat_power);
            if (calc.neg(y) >= limit) right = calc.add(right, weapon.ai_combat_power);
            if (y >= limit) left = calc.add(left, weapon.ai_combat_power);
        }
        std::int64_t adjust = 0;
        auto best = forward;
        if (forward < right) {
            adjust = 90;
            best = right;
        }
        if (left > best) {
            adjust = -90;
        } else if (left == best && adjust != 0) {
            const auto other = calc.abs(motion_detail::clamp180(calc.sub(motion_detail::whole(90), delta)));
            if (other < calc.abs(delta)) adjust = -90;
        }
        return adjust;
    }

void UnitCombat::face_target() {
        if (!unit_.can_turn) return;
        const bool destination_turn = unit_.facing_destination.has_value();
        const auto* target = world_.resolve_target(world_.find(state_.attack_target), unit_.position);
        if (!destination_turn
            && (target == nullptr || !profile_.max_attack_distance || (!state_.direct && total_hardpoints() != 0))) return;
        // WMV-20: the movement destination uses the unit centre and has no attack-distance
        // or player-direct gate. Ordinary targeting retains A-04/A-05 and its hardpoint aim.
        auto aim = destination_turn ? *unit_.facing_destination : target->position;
        // HO-08 (OR-25): a unit ordered to attack a hardpoint turns to that hardpoint.
        const auto* ordered = !destination_turn && state_.direct ? standing_hardpoint(*target, state_.attack_hardpoint) : nullptr;
        if (ordered != nullptr) aim = world_point(*target, ordered->position);
        if (!destination_turn && ordered == nullptr && target->profile != nullptr) {
            bool found = false;
            for (const auto& hardpoint : target->profile->hardpoints) {
                if (!hardpoint.targetable || hardpoint_destroyed_on(*target, hardpoint.hardpoint)) continue;
                const auto point = world_point(*target, hardpoint.position);
                if (!found || closer(unit_.position, point, aim)) {
                    aim = point;
                    found = true;
                }
            }
        }
        if (!destination_turn) {
            const auto reach = take(target_attack_distance(world_.motion, *target, unit_.position,
                *profile_.max_attack_distance, has_aim_hardpoint(*target)));
            if (!within_range(unit_.position, aim, reach, RangeMetric::planar)) return;
        }
        motion_detail::Calc calc;
        const auto dx = calc.sub(aim.x, unit_.position.x);
        const auto dy = calc.sub(aim.y, unit_.position.y);
        if (!calc.ok() || (dx.raw() == 0 && dy.raw() == 0)) return;
        const auto& m = unit_.transform.rows;
        const auto yaw = calc.atan2_deg(m[1][0], m[0][0]);
        const auto bearing = calc.atan2_deg(dy, dx);
        const auto adjust = firepower_adjust(calc, motion_detail::clamp180(calc.sub(bearing, yaw)));
        const auto heading = calc.add(bearing, motion_detail::whole(adjust));
        const auto off = calc.abs(motion_detail::clamp180(calc.sub(yaw, heading)));
        if (!calc.ok()) {
            if (!error_) error_ = calc.error("attack turn");
            return;
        }
        if (off < motion_detail::whole(10) || (!destination_turn && off == motion_detail::whole(10))) return;
        // The point the turn faces: the aim point, or it turned by the adjustment about the unit.
        auto face_x = dx;
        auto face_y = dy;
        if (adjust == 90) {
            face_x = calc.neg(dy);
            face_y = dx;
        } else if (adjust == -90) {
            face_x = dy;
            face_y = calc.neg(dx);
        }
        face_ = math::Vec3{calc.add(unit_.position.x, face_x), calc.add(unit_.position.y, face_y), unit_.position.z};
        if (!calc.ok()) {
            if (!error_) error_ = calc.error("attack turn");
            face_.reset();
        }
    }

} // namespace combat_detail
} // namespace detail
} // namespace eawr::sim::tactical
