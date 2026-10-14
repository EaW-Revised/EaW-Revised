#include "combat_algorithms.hpp"
#include "eawr/sim/tactical/snapshot.hpp"

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

std::optional<math::Vec3> lead_history(const math::Vec3 now, const math::Vec3 previous,
    const math::Vec3 older, const math::Vec3 aim, const math::Vec3 muzzle, const math::Fixed speed) {
    using motion_detail::whole;
    motion_detail::Calc calc;
    const auto add = [&](const math::Vec3 a, const math::Vec3 b) {
        return math::Vec3{calc.add(a.x, b.x), calc.add(a.y, b.y), calc.add(a.z, b.z)};
    };
    const auto sub = [&](const math::Vec3 a, const math::Vec3 b) {
        return math::Vec3{calc.sub(a.x, b.x), calc.sub(a.y, b.y), calc.sub(a.z, b.z)};
    };
    const auto mul = [&](const math::Vec3 a, const math::Fixed scale) {
        return math::Vec3{calc.mul(a.x, scale), calc.mul(a.y, scale), calc.mul(a.z, scale)};
    };
    const auto dot = [&](const math::Vec3 a, const math::Vec3 b) {
        return calc.add(calc.add(calc.mul(a.x, b.x), calc.mul(a.y, b.y)), calc.mul(a.z, b.z));
    };
    const auto cross = [&](const math::Vec3 a, const math::Vec3 b) {
        return math::Vec3{calc.sub(calc.mul(a.y, b.z), calc.mul(a.z, b.y)),
            calc.sub(calc.mul(a.z, b.x), calc.mul(a.x, b.z)),
            calc.sub(calc.mul(a.x, b.y), calc.mul(a.y, b.x))};
    };
    const auto normalize = [&](const math::Vec3 a) {
        const auto length = calc.length(a);
        return math::Vec3{calc.div(a.x, length), calc.div(a.y, length), calc.div(a.z, length)};
    };
    const auto velocity = sub(now, previous);
    const auto fallback = [&]() { return lead_point(aim, muzzle, velocity, speed); };
    if (speed.raw() <= 0) return aim;

    // WWP-51: the two chords share the oldest position. Aligned motion stays linear.
    const auto a = sub(previous, older), b = sub(now, older);
    const auto la = calc.length(a), lb = calc.length(b);
    constexpr auto tolerance = math::Fixed::from_raw(16777); // 0.001
    if (!calc.ok() || (la <= tolerance && lb <= tolerance)) return fallback();
    if (la.raw() != 0 && lb.raw() != 0) {
        const auto alignment = dot(mul(a, calc.div(whole(1), la)), mul(b, calc.div(whole(1), lb)));
        if (calc.abs(calc.sub(alignment, whole(1))) <= tolerance) return fallback();
    }
    // Solve the circumcentre from the two equal-distance constraints in the chord plane.
    const auto plane = cross(a, b);
    const auto denominator = calc.mul(whole(2), dot(plane, plane));
    if (!calc.ok() || denominator.raw() == 0) return fallback();
    const auto numerator = add(mul(cross(b, plane), dot(a, a)), mul(cross(plane, a), dot(b, b)));
    const math::Vec3 displacement{calc.div(numerator.x, denominator), calc.div(numerator.y, denominator),
        calc.div(numerator.z, denominator)};
    const auto centre = add(older, displacement);
    const auto radius = calc.length(displacement);
    if (!calc.ok() || radius.raw() == 0 || radius > whole(1000)) return fallback();
    const auto radial = sub(now, centre), normal = cross(radial, sub(older, centre));
    if (calc.length(radial).raw() == 0 || calc.length(normal).raw() == 0 || !calc.ok())
        return fallback();
    const auto x = normalize(radial), z = normalize(normal);
    const auto tangent = cross(radial, z);
    if (calc.length(tangent).raw() == 0 || !calc.ok()) return fallback();
    const auto y = normalize(tangent);
    const auto relative = sub(muzzle, centre);
    const math::Vec3 local{dot(relative, x), dot(relative, y), dot(relative, z)};
    const auto angular_speed = calc.div(calc.length(velocity), radius);
    const auto point = [&](const math::Fixed time) {
        const auto angle = calc.div(calc.mul(time, angular_speed), motion_detail::two_pi);
        const auto trig = math::sin_cos_turn(angle);
        return math::Vec3{calc.mul(radius, trig.cosine), calc.mul(radius, trig.sine), {}};
    };
    const auto residual = [&](const math::Fixed time) {
        return calc.sub(calc.length(sub(point(time), local)), calc.mul(speed, time));
    };
    // WWP-51: seconds boundaries expressed in logical frames. Fifteen iterations,
    // starting at 0.1 and 1000 seconds, stop when successive times differ by <0.001 s.
    auto time = whole(3), previous_time = whole(30000);
    auto previous_residual = residual(previous_time);
    int count = 0;
    while (calc.abs(calc.sub(time, previous_time)) >= math::Fixed::from_raw(503316) && count < 15) {
        ++count;
        const auto current_residual = residual(time);
        const auto difference = calc.sub(current_residual, previous_residual);
        if (!calc.ok() || difference.raw() == 0) return fallback();
        const auto next = calc.sub(time,
            calc.div(calc.mul(current_residual, calc.sub(time, previous_time)), difference));
        previous_time = time;
        time = next;
        previous_residual = current_residual;
    }
    if (!calc.ok() || count >= 15) return fallback();
    const auto predicted = point(time);
    const auto result = add(add(centre, add(mul(x, predicted.x), mul(y, predicted.y))), sub(aim, now));
    return calc.ok() ? std::optional<math::Vec3>{result} : fallback();
}

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

namespace {

bool destroyed_on(const CombatUnit& target, const TargetHardpoint& hardpoint) {
    const auto* health = target.durability_profile;
    return health != nullptr && target.durability != nullptr && hardpoint.hardpoint < health->hardpoints.size()
        && hardpoint_destroyed(*health, *target.durability, hardpoint.hardpoint);
}

} // namespace

const TargetHardpoint* nearest_hardpoint(const CombatUnit& target, const math::Vec3& reference) {
    if (target.profile == nullptr) return nullptr;
    const TargetHardpoint* nearest = nullptr;
    math::Vec3 nearest_point{};
    for (const auto& hardpoint : target.profile->hardpoints) {
        if (!hardpoint.targetable || destroyed_on(target, hardpoint)) continue;
        auto point = math::transform_point(target.transform, hardpoint.position);
        if (!point) continue;
        if (nearest == nullptr || UnitCombat::closer(reference, point.value(), nearest_point)) {
            nearest = &hardpoint;
            nearest_point = point.value();
        }
    }
    return nearest;
}

const TargetHardpoint* best_hardpoint(
    const CombatTable* table, const CombatProfile* attacker, const CombatUnit& target, const math::Vec3& from) {
    if (target.profile == nullptr) return nullptr;
    auto reference = from;
    if (attacker != nullptr) {
        auto raised = math::add(reference.z, attacker->ranged_target_z_adjust);
        if (raised) reference.z = raised.value();
    }
    if (table == nullptr || attacker == nullptr || !attacker->priority_set
        || *attacker->priority_set >= table->priority_sets.size()) {
        return nearest_hardpoint(target, reference);
    }
    const auto& set = table->priority_sets[*attacker->priority_set];
    const auto* health = target.durability_profile;
    const TargetHardpoint* best = nullptr;
    math::Fixed best_rank{};
    math::Vec3 best_point{};
    for (const auto& hardpoint : target.profile->hardpoints) {
        // A type without a health profile has no hardpoint health; its hardpoints count as destroyable.
        if (health != nullptr
            && (hardpoint.hardpoint >= health->hardpoints.size() || !health->hardpoints[hardpoint.hardpoint].destroyable)) {
            continue;
        }
        if (destroyed_on(target, hardpoint)) continue;
        const auto rank = hardpoint.type < set.hardpoint_ranks.size() ? set.hardpoint_ranks[hardpoint.type]
                                                                     : std::optional<math::Fixed>{set.unlisted};
        if (!rank) continue; // Hard_Point_Exclusions
        auto point = math::transform_point(target.transform, hardpoint.position);
        if (!point) continue;
        if (best == nullptr || *rank < best_rank
            || (*rank == best_rank && UnitCombat::closer(reference, point.value(), best_point))) {
            best = &hardpoint;
            best_rank = *rank;
            best_point = point.value();
        }
    }
    return best;
}

math::Vec3 ordered_aim_point(const CombatTable* table, const CombatProfile* attacker, const CombatUnit& target,
    const math::Vec3& from, const std::uint32_t ordered_index) {
    if (target.profile == nullptr) return target.position;
    const auto* chosen = standing_hardpoint(target, ordered_index);
    if (chosen == nullptr) chosen = best_hardpoint(table, attacker, target, from);
    if (chosen == nullptr) return target.position;
    auto point = math::transform_point(target.transform, chosen->position);
    return point ? point.value() : target.position;
}


namespace combat_detail {

bool UnitCombat::point_object_turret(const WeaponProfile& weapon, WeaponState::TurretState& state,
    const CombatUnit& target) {
    state.target = target.id;
    state.ready = false;
    if (!weapon.object_turret) return false;
    const auto& turret = *weapon.object_turret;
    auto point = target.position;
    if (target.profile) point.z = take(math::add(point.z, target.profile->ranged_target_z_adjust));
    const auto origin = world_point(unit_, turret.pivot);
    const math::Vec3 delta{take(math::subtract(point.x, origin.x)), take(math::subtract(point.y, origin.y)),
        take(math::subtract(point.z, origin.z))};
    const auto& m = unit_.transform.rows;
    const auto axis = [&](const std::size_t column) {
        return take(math::dot(math::Vec3{m[0][column], m[1][column], m[2][column]}, delta));
    };
    const auto x = axis(0), y = axis(1), z = axis(2);
    const auto planar = take(math::length(math::Vec2{x, y}));
    const auto yaw = x.raw() == 0 && y.raw() == 0 ? math::Fixed{} : take(math::atan2_turn(y, x));
    const auto pitch = planar.raw() == 0 && z.raw() == 0 ? math::Fixed{} : take(math::atan2_turn(z, planar));
    const auto wrap = [](const std::int64_t angle) {
        auto value = angle % (360 * one_raw);
        if (value >= 180 * one_raw) value -= 360 * one_raw;
        if (value < -180 * one_raw) value += 360 * one_raw;
        return value;
    };
    const auto desired_yaw = wrap(yaw.raw() * 360);
    const auto desired_pitch = turret.xy_only ? 0 : -pitch.raw() * 360;
    // UA-05/WWP-17: reject authored extents; one shortest yaw and one pitch step per pointing call.
    if (std::llabs(desired_yaw) > turret.yaw_extent.raw()
        || (!turret.xy_only && std::llabs(desired_pitch) > turret.pitch_extent.raw())) return false;
    state.desired_yaw = math::Fixed::from_raw(desired_yaw);
    state.desired_pitch = math::Fixed::from_raw(desired_pitch);
    state.rest_started_frame.reset();
    state.returning_to_rest = false;
    advance_object_turret(turret, state);
    state.ready = state.yaw == state.desired_yaw && state.pitch == state.desired_pitch;
    return true; // Geometry admitted; readiness is the separate firing permission.
}

void UnitCombat::advance_object_turret(const ManualTurretProfile& turret, WeaponState::TurretState& state) {
    const auto wrap = [](const std::int64_t angle) {
        auto value = angle % (360 * one_raw);
        if (value >= 180 * one_raw) value -= 360 * one_raw;
        if (value < -180 * one_raw) value += 360 * one_raw;
        return value;
    };
    const auto speed = turret.speed.raw();
    state.yaw = math::Fixed::from_raw(std::clamp(wrap(state.yaw.raw()
        + std::clamp(wrap(state.desired_yaw.raw() - state.yaw.raw()), -speed, speed)),
        -turret.yaw_extent.raw(), turret.yaw_extent.raw()));
    state.pitch = math::Fixed::from_raw(state.pitch.raw()
        + std::clamp(state.desired_pitch.raw() - state.pitch.raw(), -speed, speed));
}

void UnitCombat::idle_object_turret(const WeaponProfile& weapon, WeaponState::TurretState& state) {
    state.target = invalid_entity_id;
    state.ready = false;
    if (!weapon.object_turret) return;
    const auto& turret = *weapon.object_turret;
    const auto rest_pitch = turret.xy_only ? math::Fixed{} : turret.rest.y;
    if (state.yaw == turret.rest.z && state.pitch == rest_pitch) {
        state.returning_to_rest = false;
        state.rest_started_frame.reset();
        return;
    }
    // FC-61/FSC-54: first idle turn holds; strict synchronized-time expiry releases rest.
    if (!state.returning_to_rest) {
        state.returning_to_rest = true;
        state.rest_started_frame = world_.frame;
        state.desired_yaw = state.yaw;
        state.desired_pitch = state.pitch;
        return;
    }
    if (state.rest_started_frame) {
        const auto elapsed = world_.frame >= *state.rest_started_frame ? world_.frame - *state.rest_started_frame : 0;
        constexpr auto maximum_delay_frames = std::uint64_t{3600} * logical_frames_per_second;
        if (elapsed <= maximum_delay_frames && static_cast<std::int64_t>(elapsed) * one_raw
            / logical_frames_per_second <= weapon.object_turret_rest_delay.raw()) return;
        state.rest_started_frame.reset();
    }
    state.desired_yaw = turret.rest.z;
    state.desired_pitch = rest_pitch;
    advance_object_turret(turret, state);
    if (state.yaw == state.desired_yaw && state.pitch == state.desired_pitch) state.returning_to_rest = false;
}

math::Vec3 UnitCombat::object_turret_muzzle(const WeaponProfile& weapon, const WeaponState::TurretState& state,
    math::Vec3 muzzle) {
    if (!weapon.object_turret) return muzzle;
    const auto& turret = *weapon.object_turret;
    if (!turret.xy_only) {
        muzzle = manual_rotate(muzzle, turret.barrel ? turret.barrel_pivot : turret.pivot,
            turret.barrel ? turret.barrel_axes : turret.axes, state.pitch, true, false);
    }
    return manual_rotate(muzzle, turret.pivot, turret.axes, state.yaw, false, false);
}

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
    // WSW-18: hardpoint coordinates require the debug build's quarter-turn before
    // yaw admission and turret desire. Source pitch about Y is positive down (FM-02).
    auto yaw_degrees = yaw.raw() * 360 + 90 * one_raw;
    if (yaw_degrees >= 180 * one_raw) yaw_degrees -= 360 * one_raw;
    return {{}, math::Fixed::from_raw(-pitch.raw() * 360), math::Fixed::from_raw(yaw_degrees)};
}

core::Result<bool> UnitCombat::admit_manual(const CombatUnit& target, const std::uint32_t hardpoint,
    const std::optional<math::Fixed> soft_radius) {
    const auto slot = std::find_if(profile_.weapons.begin(), profile_.weapons.end(),
        [&](const WeaponProfile& weapon) { return weapon.hardpoint == hardpoint; });
    if (slot == profile_.weapons.end()) return core::Result<bool>::success(false);
    const auto index = static_cast<std::size_t>(slot - profile_.weapons.begin());
    if (!slot->requires_manual_target || slot->hardpoint == object_weapon || slot->pulse_count != 1 || !weapon_up(*slot)
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
    const auto reach = take(math::add(slot->range, soft_radius.value_or(target_soft_radius(world_.motion, target))));
    const bool accepted = points && within_range(midpoint, target.position, reach, RangeMetric::planar)
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
        if (target.before_previous_position) return lead_history(target.position, target.previous_position,
            *target.before_previous_position, aim, muzzle, weapon.shot->speed);
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
        const auto fixed = unit_.corrupt_inaccuracy ? unit_.corrupt_inaccuracy
            : unit_.barrage_target == target.id ? unit_.fixed_inaccuracy : std::nullopt;
        if (fixed && (!unit_.corrupt_inaccuracy || fixed->raw() > 0)) {
            const auto radius = fixed->raw();
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
        // A-05: a scanned (not player-ordered) target turns a unit with hardpoints only when
        // Auto_Rotate_For_Space_Targeting is set; FoC ships it false.
        const bool scan_turn_allowed = world_.table != nullptr && world_.table->auto_rotate_for_targeting;
        if (!destination_turn
            && (target == nullptr || !profile_.max_attack_distance
                || (!state_.direct && !scan_turn_allowed && total_hardpoints() != 0))) return;
        // WMV-20: the movement destination uses the unit centre and has no attack-distance
        // or player-direct gate. Ordinary targeting retains A-04/A-05 and its hardpoint aim.
        auto aim = destination_turn ? *unit_.facing_destination : target->position;
        // HO-08 (OR-25): a unit ordered to attack a hardpoint turns to that hardpoint.
        const auto* ordered = !destination_turn && state_.direct ? standing_hardpoint(*target, state_.attack_hardpoint) : nullptr;
        if (ordered != nullptr) aim = world_point(*target, ordered->position);
        if (!destination_turn && ordered == nullptr) {
            // WCC-19: else the attacker's best hardpoint of the target (WCC-18).
            if (const auto* best = best_hardpoint(world_.table, &profile_, *target, unit_.position)) {
                aim = world_point(*target, best->position);
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

RejectReason planetary_special_fire_target_admission(
    const TacticalSnapshot& snapshot, const EntityId source_id, const EntityId target_id) noexcept {
    const auto instances = snapshot.instances();
    const auto find = [&](const EntityId id) -> const TacticalInstance* {
        const auto found = std::lower_bound(instances.begin(), instances.end(), id,
            [](const TacticalInstance& unit, const EntityId key) { return unit.entity_id < key; });
        return found != instances.end() && found->entity_id == id ? &*found : nullptr;
    };
    const auto* source = find(source_id);
    const auto* target = find(target_id);
    if (!source) return RejectReason::unit_not_live;
    if (!target) return RejectReason::target_not_live;
    return planetary_special_fire_target_admission(snapshot.players(), *source, *target);
}

RejectReason planetary_special_fire_target_admission(const std::span<const SnapshotPlayer> players,
    const TacticalInstance& source, const TacticalInstance& target) noexcept {
    if (!source.planetary_weapon) return RejectReason::ability_unavailable;
    if (target.durability && target.durability->hull.raw() <= 0) return RejectReason::target_not_live;
    if (players_allied(players, source.owner, target.owner)) return RejectReason::target_not_hostile;
    if (source.entity_id == target.entity_id || !(target.category_bits & source.planetary_weapon->valid_targets))
        return RejectReason::ability_unavailable;
    return RejectReason::none;
}

core::Result<std::optional<std::uint32_t>> manual_special_fire_admission(
    const TacticalSnapshot& snapshot, const EntityId source_id, const EntityId target_id, const bool require_gun_ready) {
    using Result = core::Result<std::optional<std::uint32_t>>;
    const auto instances = snapshot.instances();
    const auto find = [&](const EntityId id) -> const TacticalInstance* {
        const auto found = std::lower_bound(instances.begin(), instances.end(), id,
            [](const TacticalInstance& unit, const EntityId key) { return unit.entity_id < key; });
        return found != instances.end() && found->entity_id == id ? &*found : nullptr;
    };
    const auto* source = find(source_id);
    const auto* target = find(target_id);
    if (!source || !target) return Result::success(std::nullopt);
    return manual_special_fire_admission(snapshot.players(), *source, *target, require_gun_ready);
}

core::Result<std::optional<std::uint32_t>> manual_special_fire_admission(
    const std::span<const SnapshotPlayer> players, const TacticalInstance& source_view,
    const TacticalInstance& target_view, const bool require_gun_ready, const std::optional<std::uint32_t> hardpoint) {
    using Result = core::Result<std::optional<std::uint32_t>>;
    const auto* source = &source_view;
    const auto* target = &target_view;
    if (source->planetary_weapon) {
        const auto admitted = planetary_special_fire_target_admission(players, *source, *target);
        return Result::success(admitted == RejectReason::none ? std::optional(attack_hull) : std::nullopt);
    }
    if (source->manual_weapons.empty() || source->arrival
        || (target->durability && target->durability->hull.raw() <= 0)) return Result::success(std::nullopt);
    const auto found = hardpoint ? std::find_if(source->manual_weapons.begin(), source->manual_weapons.end(),
        [&](const ManualWeaponView& gun) { return gun.hardpoint == *hardpoint; }) : source->manual_weapons.begin();
    if (found == source->manual_weapons.end()) return Result::success(std::nullopt);
    const auto& gun = *found;
    if (!gun.available || (require_gun_ready && gun.countdown) || gun.target != invalid_entity_id || !gun.admission_profile)
        return Result::success(std::nullopt);
    CombatProfile own_profile, target_profile;
    own_profile.weapons.push_back(*gun.admission_profile);
    own_profile.target_stealth = source->target_stealth;
    target_profile.category_bits = target->category_bits;
    CombatState state;
    state.weapons.resize(1);
    ManualWeaponState assignment;
    assignment.target = gun.target;
    assignment.yaw = gun.yaw;
    assignment.pitch = gun.pitch;
    state.weapons.front().manual = assignment;
    detail::CombatWorld world;
    world.relationships = players;
    detail::CombatUnit own, enemy;
    own.id = source->entity_id; own.type_id = source->type_id; own.owner = source->owner; own.team = source->team;
    own.transform = source->fixed_transform;
    own.position = {own.transform.rows[0][3], own.transform.rows[1][3], own.transform.rows[2][3]};
    own.profile = &own_profile; own.combat = &state;
    const auto player = std::find_if(world.relationships.begin(), world.relationships.end(),
        [&](const SnapshotPlayer& entry) { return entry.player_id == own.owner; });
    if (player == world.relationships.end()) return Result::success(std::nullopt);
    own.player_index = static_cast<std::size_t>(player - world.relationships.begin());
    enemy.id = target->entity_id; enemy.owner = target->owner; enemy.team = target->team;
    enemy.transform = target->fixed_transform;
    enemy.position = {enemy.transform.rows[0][3], enemy.transform.rows[1][3], enemy.transform.rows[2][3]};
    enemy.visible_to = target->visible_to; enemy.profile = &target_profile;
    enemy.stealth_invisible = target->stealth_invisible;
    auto admitted = detail::combat_detail::UnitCombat(world, own).admit_manual(enemy, gun.hardpoint, target->soft_radius);
    if (!admitted) return Result::failure(admitted.error());
    return Result::success(admitted.value() ? std::optional(gun.hardpoint) : std::nullopt);
}

} // namespace eawr::sim::tactical
