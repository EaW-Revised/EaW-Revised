#include "eawr/sim/tactical/damage.hpp"

#include "tactical_internal.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>

namespace eawr::sim::tactical {
namespace {

using math::Fixed;

constexpr std::int64_t one_raw = Fixed::scale;
constexpr std::int64_t multiplier_limit_raw = max_damage_multiplier * Fixed::scale;
constexpr std::int64_t health_limit_raw = max_durability_health * Fixed::scale;
// The largest product of apply_hit: damage x curve value x armor multiplier, in whole units
// (DG-26's out-of-combat factor is clamped to keep it).
static_assert(max_durability_health * max_damage_multiplier * max_damage_multiplier
        <= std::numeric_limits<std::int64_t>::max() / Fixed::scale / 4,
    "the damage pipeline must stay in Q24");
constexpr std::int64_t curve_x_limit_raw = std::int64_t{3600} * Fixed::scale;
// The frames the retail curve sees for a unit never hit before (DG-05).
constexpr std::uint64_t never_hit_frames = 999'999;

// Rounded Q24 arithmetic that remembers its first failure, so a formula reads as one expression.
struct Arithmetic {
    std::optional<core::Diagnostic> error;

    Fixed take(core::Result<Fixed> value) {
        if (!value) {
            if (!error) error = value.error();
            return Fixed{};
        }
        return value.value();
    }
    Fixed add(const Fixed a, const Fixed b) { return take(math::add(a, b)); }
    Fixed sub(const Fixed a, const Fixed b) { return take(math::subtract(a, b)); }
    Fixed mul(const Fixed a, const Fixed b) { return take(math::multiply(a, b)); }
    Fixed div(const Fixed a, const Fixed b) { return take(math::divide(a, b)); }
};

[[nodiscard]] Fixed whole(const std::int64_t value) noexcept { return Fixed::from_raw(value * Fixed::scale); }

[[nodiscard]] Fixed clamp(const Fixed value, const Fixed low, const Fixed high) noexcept {
    return std::min(std::max(value, low), high);
}

// Set_Shields (DG-08): clamped to [0, max]; reaching zero starts the depletion effect unless it
// already runs, and a positive value outside the effect clears it.
void set_shields(const DurabilityProfile& profile, const DamageRules& rules, DurabilityState& state, const Fixed value,
    const std::uint64_t frame) noexcept {
    const auto clamped = clamp(value, Fixed{}, profile.max_shields);
    const bool active = shield_depleted(rules, state, frame);
    if (clamped.raw() <= 0 && !active) {
        state.depleted_frame = frame;
    }
    if (clamped.raw() > 0 && !active) {
        state.depleted_frame.reset();
    }
    state.shields = clamped;
}

[[nodiscard]] bool has_role(const DurabilityProfile& profile, const HardpointRole role) noexcept {
    return std::any_of(profile.hardpoints.begin(), profile.hardpoints.end(),
        [role](const HardpointProfile& hardpoint) { return hardpoint.role == role; });
}

// CubicInterpolator::Process: the natural spline's second derivatives (zero at both ends).
[[nodiscard]] std::vector<Fixed> spline_seconds(const std::vector<CurvePoint>& points, Arithmetic& q) {
    const auto n = points.size();
    std::vector<Fixed> second(n);
    std::vector<Fixed> u(n);
    for (std::size_t i = 1; i + 1 < n; ++i) {
        const auto sig = q.div(q.sub(points[i].x, points[i - 1].x), q.sub(points[i + 1].x, points[i - 1].x));
        const auto p = q.add(q.mul(sig, second[i - 1]), whole(2));
        second[i] = q.div(q.sub(sig, whole(1)), p);
        const auto slope = q.sub(q.div(q.sub(points[i + 1].y, points[i].y), q.sub(points[i + 1].x, points[i].x)),
            q.div(q.sub(points[i].y, points[i - 1].y), q.sub(points[i].x, points[i - 1].x)));
        u[i] = q.div(q.sub(q.div(q.mul(whole(6), slope), q.sub(points[i + 1].x, points[i - 1].x)), q.mul(sig, u[i - 1])), p);
    }
    if (n == 0) return second;
    second[n - 1] = Fixed{};
    for (std::size_t k = n - 1; k-- > 1;) {
        second[k] = q.add(q.mul(second[k], second[k + 1]), u[k]);
    }
    second[0] = Fixed{};
    return second;
}

} // namespace

core::Result<void> validate_damage(const DurabilityTable& table) {
    const auto invalid = [](const std::string& message) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, message));
    };
    if (!table.damage) {
        return core::Result<void>::success();
    }
    const auto& rules = *table.damage;
    if (rules.shield_recharge_frames == 0) {
        return invalid("damage rules: the shield recharge interval must be at least one frame");
    }
    for (const auto value : {rules.depleted_disable_seconds, rules.depleted_increment_seconds, rules.depleted_regen_cap}) {
        if (value.raw() < 0 || value.raw() > health_limit_raw) {
            return invalid("damage rules: depleted-shield values must be >= 0 and bounded by the health limit");
        }
    }
    if (rules.diminishing.size() > max_curve_points) {
        return invalid("damage rules: the diminishing-firepower curve has more than "
            + std::to_string(max_curve_points) + " points");
    }
    for (std::size_t index = 0; index < rules.diminishing.size(); ++index) {
        const auto& point = rules.diminishing[index];
        if (point.x.raw() < 0 || point.x.raw() > curve_x_limit_raw || point.y.raw() < 0
            || point.y.raw() > multiplier_limit_raw
            || (index != 0 && point.x <= rules.diminishing[index - 1].x)) {
            return invalid("damage rules: curve point " + std::to_string(index)
                + " needs x in [0, 3600] strictly increasing and y in [0, "
                + std::to_string(max_damage_multiplier) + "]");
        }
    }
    {
        // The curve must evaluate everywhere: its setup and, per interval, the largest curvature
        // term (|t^3 - t| <= 1 on [0, 1]) stay in Q24.
        Arithmetic q;
        const auto& points = rules.diminishing;
        const auto second = spline_seconds(points, q);
        for (std::size_t i = 1; i < points.size() && !q.error; ++i) {
            const auto h = q.sub(points[i].x, points[i - 1].x);
            const auto reach = q.add(
                Fixed::from_raw(std::llabs(second[i - 1].raw())), Fixed::from_raw(std::llabs(second[i].raw())));
            // Twice the reach leaves room for the evaluation's rounding.
            (void)q.mul(q.mul(q.add(reach, reach), h), h);
        }
        if (q.error) {
            return invalid("damage rules: the diminishing-firepower curve overflows between its points");
        }
    }
    if (rules.damage_types > max_damage_types || rules.armor_types > max_damage_types
        || rules.armor_mods.size() != static_cast<std::size_t>(rules.damage_types) * rules.armor_types) {
        return invalid("damage rules: the armor table must be damage types x armor types, each at most "
            + std::to_string(max_damage_types));
    }
    for (const auto value : rules.armor_mods) {
        if (value.raw() < 0 || value.raw() > multiplier_limit_raw) {
            return invalid("damage rules: armor multipliers must be in [0, " + std::to_string(max_damage_multiplier) + "]");
        }
    }
    for (const auto& profile : table.profiles) {
        const auto context = "durability profile of type " + std::to_string(profile.type_id);
        if (profile.max_shields.raw() < 0 || profile.max_shields.raw() > health_limit_raw
            || profile.shield_refresh.raw() < 0 || profile.shield_refresh.raw() > health_limit_raw) {
            return invalid(context + ": shields must be in [0, " + std::to_string(max_durability_health) + "]");
        }
        for (const auto armor : {profile.armor_type, profile.shield_armor_type}) {
            if (armor != no_type_index && armor >= rules.armor_types) {
                return invalid(context + ": armor type index " + std::to_string(armor) + " is not in the table");
            }
        }
        if (profile.max_energy.raw() < 0 || profile.max_energy.raw() > health_limit_raw
            || profile.energy_refresh.raw() < 0 || profile.energy_refresh.raw() > health_limit_raw) {
            return invalid(context + ": energy must be in [0, " + std::to_string(max_durability_health) + "]");
        }
    }
    if (rules.energy_to_shield.raw() < 0 || rules.energy_to_shield.raw() > multiplier_limit_raw) {
        return invalid("damage rules: the energy-to-shield rate must be in [0, " + std::to_string(max_damage_multiplier) + "]");
    }
    return core::Result<void>::success();
}

bool has_energy_pool(const DurabilityProfile& profile, const DamageRules& rules) noexcept {
    return rules.energy_recharge_frames != 0 && profile.powered;
}

Fixed armor_multiplier(const DamageRules& rules, const std::uint32_t damage_type, const std::uint32_t armor_type) noexcept {
    if (damage_type >= rules.damage_types || armor_type >= rules.armor_types) {
        return Fixed::from_raw(one_raw);
    }
    return rules.armor_mods[static_cast<std::size_t>(damage_type) * rules.armor_types + armor_type];
}

core::Result<Fixed> diminishing_factor(const DamageRules& rules, const std::uint64_t frames) {
    const auto& points = rules.diminishing;
    if (points.empty()) {
        return core::Result<Fixed>::success(Fixed::from_raw(one_raw));
    }
    Arithmetic q;
    // Seconds since the last hit; the curve is flat beyond its last point, so larger gaps clamp.
    const auto capped = std::min<std::uint64_t>(frames, never_hit_frames);
    const auto x = q.div(whole(static_cast<std::int64_t>(capped)), whole(logical_frames_per_second));
    if (q.error) return core::Result<Fixed>::failure(*q.error);
    // CubicInterpolator::Interpolate: the first point above x; the end values outside the range.
    const auto above = std::upper_bound(points.begin(), points.end(), x,
        [](const Fixed value, const CurvePoint& point) { return value < point.x; });
    if (above == points.end()) return core::Result<Fixed>::success(points.back().y);
    if (above == points.begin()) return core::Result<Fixed>::success(points.front().y);

    const auto second = spline_seconds(points, q);

    const auto hi = static_cast<std::size_t>(above - points.begin());
    const auto lo = hi - 1;
    const auto h = q.sub(points[hi].x, points[lo].x);
    const auto a = q.div(q.sub(points[hi].x, x), h);
    const auto b = q.div(q.sub(x, points[lo].x), h);
    const auto cube = [&](const Fixed t) { return q.sub(q.mul(q.mul(t, t), t), t); };
    const auto curve = q.div(q.mul(q.mul(q.add(q.mul(cube(a), second[lo]), q.mul(cube(b), second[hi])), h), h), whole(6));
    const auto value = q.add(q.add(q.mul(a, points[lo].y), q.mul(b, points[hi].y)), curve);
    if (q.error) return core::Result<Fixed>::failure(*q.error);
    // A spline can overshoot its points; the cap keeps apply_hit's products in Q24 and is far
    // above FoC's curve (at most about 1.18).
    return core::Result<Fixed>::success(clamp(value, Fixed{}, Fixed::from_raw(multiplier_limit_raw)));
}

bool shield_depleted(const DamageRules& rules, const DurabilityState& state, const std::uint64_t frame) noexcept {
    if (!state.depleted_frame || *state.depleted_frame > frame) {
        return false;
    }
    // (frame - depleted) / fps < disable time, compared exactly on raw values.
    const auto elapsed = static_cast<std::int64_t>(std::min<std::uint64_t>(frame - *state.depleted_frame, 1ULL << 30));
    return elapsed * Fixed::scale < rules.depleted_disable_seconds.raw() * logical_frames_per_second;
}

core::Result<HitOutcome> apply_hit(const DurabilityProfile& profile, const DamageRules& rules, DurabilityState& state,
    const Hit& hit, const std::uint64_t frame) {
    HitOutcome outcome;
    Arithmetic q;
    auto amount = std::max(hit.amount, Fixed{});
    if (hit.projectile) {
        if (hit.allow_diminishing_firepower && hit.internal_damage_misc) {
            // DG-05: projectile damage shrinks with the time since the target's last diminishing-
            // eligible hit. Outside this branch (either gate false) the last-hit frame does not
            // advance: a later eligible hit is unaffected by the ones skipped here.
            const auto since = state.last_hit_frame && *state.last_hit_frame <= frame ? frame - *state.last_hit_frame
                                                                                         : never_hit_frames;
            state.last_hit_frame = frame;
            auto factor = diminishing_factor(rules, since);
            if (!factor) return core::Result<HitOutcome>::failure(factor.error());
            amount = q.mul(amount, factor.value());
        }
        // DG-26: the target's combat defense modifier (a craft out of combat: -1, twice the
        // damage; #530 PU-38, an arriving unit -3 more). Independent of DG-05's two gates above:
        // FoC applies it unconditionally to every projectile hit.
        if (hit.defense.raw() != 0) {
            const auto defense = std::clamp(hit.defense, whole(-4), whole(1));
            amount = std::min(q.mul(amount, q.sub(whole(1), defense)),
                Fixed::from_raw(max_durability_health * max_damage_multiplier * Fixed::scale));
        }
    }
    // DG-06, DG-07: the shield takes the damage first, scaled by the shield armor; what it does
    // not absorb goes on, scaled back. Scripted damage has no damage type and no multiplier.
    // EN-07: an energy-damage projectile then drains the pool with what the shield left, still
    // scaled by the shield armor, on a unit with a pool whether or not it has a shield.
    const bool drains = hit.projectile && hit.energy_damage && has_energy_pool(profile, rules);
    if ((profile.max_shields.raw() > 0 || drains) && amount.raw() > 0) {
        const auto modifier = hit.projectile ? armor_multiplier(rules, hit.damage_type, profile.shield_armor_type)
                                             : Fixed::from_raw(one_raw);
        const auto shield_damage = q.mul(amount, modifier);
        Fixed absorbed{};
        if (profile.max_shields.raw() > 0 && (!hit.projectile || hit.shield_damage)) {
            if (shield_depleted(rules, state, frame)) {
                // DG-09: a depleted shield absorbs nothing; each damaging hit may lengthen the effect.
                const auto increment = q.mul(rules.depleted_increment_seconds, whole(logical_frames_per_second));
                const auto frames = increment.raw() / Fixed::scale;
                if (shield_damage.raw() > 0 && frames > 0) {
                    state.depleted_frame = std::min(*state.depleted_frame + static_cast<std::uint64_t>(frames), frame);
                }
            } else {
                const auto before = state.shields;
                const auto after = std::max(q.sub(before, shield_damage), Fixed{});
                absorbed = q.sub(before, after);
                set_shields(profile, rules, state, after, frame);
                outcome.shields_depleted = before.raw() > 0 && after.raw() == 0;
            }
        }
        outcome.absorbed = absorbed;
        outcome.shield_absorbed = absorbed.raw() > 0 && absorbed >= shield_damage;
        auto rest = q.sub(shield_damage, absorbed);
        if (drains && rest.raw() > 0) {
            outcome.drained = std::min(rest, state.energy);
            state.energy = q.sub(state.energy, outcome.drained);
            rest = q.sub(rest, outcome.drained);
        }
        if (modifier.raw() > 0) {
            amount = q.div(rest, modifier);
        }
    }
    // DG-10: the rest is scaled by the hull armor; a projectile without hitpoint damage stops here.
    if (hit.projectile) {
        if (hit.hitpoint_damage && amount.raw() > 0) {
            outcome.armor_multiplier = armor_multiplier(rules, hit.damage_type, profile.armor_type);
        }
        amount = hit.hitpoint_damage ? q.mul(amount, armor_multiplier(rules, hit.damage_type, profile.armor_type)) : Fixed{};
    }
    if (q.error) return core::Result<HitOutcome>::failure(*q.error);
    // DG-11: a hit on a destroyable hardpoint that still has health damages only it; on a
    // destroyed one, nothing; any other hit damages the hull (HD-02, HD-20, HD-21).
    outcome.damage = apply_damage(profile, state, hit.hardpoint, amount);
    if (outcome.damage.destroyed_hardpoint
        && profile.hardpoints[*outcome.damage.destroyed_hardpoint].role == HardpointRole::shield_generator) {
        shield_generators_lost(profile, rules, state, frame);
    }
    return core::Result<HitOutcome>::success(outcome);
}

core::Result<void> recharge_shields(const DurabilityProfile& profile, const DamageRules& rules,
    DurabilityState& state, const std::uint64_t frame, const Fixed multiplier) {
    // DG-17: without a live shield generator (on a type that has them) the shield never recharges.
    if (profile.max_shields.raw() <= 0 || !shields_online(profile, state)) {
        return core::Result<void>::success();
    }
    Arithmetic q;
    // AB-22: the refresh times the ability multiplier, before the generator share.
    auto amount = multiplier.raw() == one_raw ? profile.shield_refresh : q.mul(profile.shield_refresh, multiplier);
    std::int64_t total = 0;
    std::int64_t alive = 0;
    for (std::size_t index = 0; index < profile.hardpoints.size(); ++index) {
        if (profile.hardpoints[index].role != HardpointRole::shield_generator) continue;
        ++total;
        alive += hardpoint_destroyed(profile, state, index) ? 0 : 1;
    }
    // DG-14: scaled by the fraction of shield generators still standing.
    if (total > 0) {
        amount = q.div(q.mul(amount, whole(alive)), whole(total));
    }
    auto raised = clamp(q.add(state.shields, amount), Fixed{}, profile.max_shields);
    // DG-15: while the depletion effect runs the shield recharges only up to the cap.
    if (shield_depleted(rules, state, frame) && raised > rules.depleted_regen_cap) {
        raised = rules.depleted_regen_cap;
    }
    // EN-04 (DG-16): a unit with a pool pays for the shield it gains, at EnergyToShieldExchangeRate
    // per point, and gains nothing when the pool holds less.
    const auto gain = q.sub(raised, state.shields);
    if (gain.raw() > 0 && has_energy_pool(profile, rules)) {
        const auto cost = q.mul(gain, rules.energy_to_shield);
        if (q.error) return core::Result<void>::failure(*q.error);
        if (cost > state.energy) return core::Result<void>::success();
        state.energy = q.sub(state.energy, cost);
    }
    if (q.error) return core::Result<void>::failure(*q.error);
    set_shields(profile, rules, state, raised, frame);
    return core::Result<void>::success();
}

core::Result<void> recharge_energy(
    const DurabilityProfile& profile, const DamageRules& rules, DurabilityState& state, const Fixed multiplier) {
    if (!has_energy_pool(profile, rules)) return core::Result<void>::success();
    Arithmetic q;
    const auto refresh = multiplier.raw() == one_raw ? profile.energy_refresh : q.mul(profile.energy_refresh, multiplier);
    const auto raised = clamp(q.add(state.energy, refresh), Fixed{}, profile.max_energy);
    if (q.error) return core::Result<void>::failure(*q.error);
    state.energy = raised;
    return core::Result<void>::success();
}

bool draw_energy(const DurabilityProfile& profile, const DamageRules& rules, DurabilityState& state, const Fixed amount) noexcept {
    if (amount.raw() <= 0) return true;
    if (!has_energy_pool(profile, rules) || amount > state.energy) return false;
    state.energy = Fixed::from_raw(state.energy.raw() - amount.raw());
    return true;
}

void shield_generators_lost(
    const DurabilityProfile& profile, const DamageRules& rules, DurabilityState& state, const std::uint64_t frame) noexcept {
    if (profile.max_shields.raw() > 0 && has_role(profile, HardpointRole::shield_generator)
        && !shields_online(profile, state)) {
        set_shields(profile, rules, state, Fixed{}, frame);
    }
}

core::Result<std::optional<Fixed>> segment_enters_box(
    const CollisionBox& box, const math::Mat3x4& transform, const math::Vec3& from, const math::Vec3& to) {
    Arithmetic q;
    const auto& m = transform.rows;
    const math::Vec3 origin{m[0][3], m[1][3], m[2][3]};
    // Model-space coordinates: the rotation's columns are the model axes in the world.
    const auto local = [&](const math::Vec3& point) {
        const math::Vec3 delta{q.sub(point.x, origin.x), q.sub(point.y, origin.y), q.sub(point.z, origin.z)};
        std::array<Fixed, 3> result{};
        for (std::size_t column = 0; column < 3; ++column) {
            result[column] = q.take(math::dot(math::Vec3{m[0][column], m[1][column], m[2][column]}, delta));
        }
        return result;
    };
    const auto start = local(from);
    const auto end = local(to);
    if (q.error) return core::Result<std::optional<Fixed>>::failure(*q.error);
    const std::array<Fixed, 3> low{box.min.x, box.min.y, box.min.z};
    const std::array<Fixed, 3> high{box.max.x, box.max.y, box.max.z};
    // Slab test on raw values. A fraction beyond [-4, 4] is saturated: only [0, 1] matters.
    const auto fraction = [&](const std::int64_t numerator, const std::int64_t denominator) {
        const auto limit = std::llabs(denominator) * 4;
        if (std::llabs(numerator) > limit) {
            return Fixed::from_raw(((numerator < 0) != (denominator < 0) ? -4 : 4) * one_raw);
        }
        return q.div(Fixed::from_raw(numerator), Fixed::from_raw(denominator));
    };
    auto enter = Fixed::from_raw(-4 * one_raw);
    auto leave = Fixed::from_raw(4 * one_raw);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto p = start[axis].raw();
        const auto d = end[axis].raw() - p;
        if (d == 0) {
            if (p < low[axis].raw() || p > high[axis].raw()) {
                return core::Result<std::optional<Fixed>>::success(std::nullopt);
            }
            continue;
        }
        auto first = fraction(low[axis].raw() - p, d);
        auto second = fraction(high[axis].raw() - p, d);
        if (second < first) std::swap(first, second);
        enter = std::max(enter, first);
        leave = std::min(leave, second);
    }
    if (q.error) return core::Result<std::optional<Fixed>>::failure(*q.error);
    if (enter > leave || leave.raw() < 0 || enter.raw() > one_raw) {
        return core::Result<std::optional<Fixed>>::success(std::nullopt);
    }
    return core::Result<std::optional<Fixed>>::success(std::max(enter, Fixed{}));
}

} // namespace eawr::sim::tactical
