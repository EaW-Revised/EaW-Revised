#include "eawr/sim/tactical/durability.hpp"

#include "eawr/sim/tactical/damage.hpp"

#include "../math/wide.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace eawr::sim::tactical {
namespace {

namespace wide = math::detail;

constexpr std::int64_t health_limit_raw = max_durability_health * math::Fixed::scale;
constexpr auto scale = static_cast<std::uint64_t>(math::Fixed::scale);

// A nonnegative exact integer of at most 192 bits; overflow is sticky.
struct Exact {
    wide::UInt192 value{};
    bool overflow{};
};

[[nodiscard]] Exact exact(const std::int64_t nonnegative) noexcept {
    return {wide::from_u64(static_cast<std::uint64_t>(nonnegative)), false};
}

[[nodiscard]] Exact times(Exact value, const std::int64_t factor) noexcept {
    value.overflow = wide::multiply_by_u64(value.value, static_cast<std::uint64_t>(factor), value.value)
        || value.overflow;
    return value;
}

[[nodiscard]] Exact plus(Exact value, const Exact& addend) noexcept {
    value.overflow = wide::add_magnitude(value.value, addend.value) || value.overflow || addend.overflow;
    return value;
}

// value - subtrahend; the caller has checked value >= subtrahend.
[[nodiscard]] Exact minus(Exact value, const Exact& subtrahend) noexcept {
    wide::subtract_magnitude(value.value, subtrahend.value);
    value.overflow = value.overflow || subtrahend.overflow;
    return value;
}

// numerator / denominator rounded once to nearest, ties to even (docs/fixed-point.md).
[[nodiscard]] bool quotient(const Exact& numerator, const Exact& denominator, std::int64_t& raw) noexcept {
    if (numerator.overflow || denominator.overflow || denominator.value.is_zero()) {
        return false;
    }
    return wide::rounded_divide_to_raw({numerator.value, false}, denominator.value, raw);
}

[[nodiscard]] core::Diagnostic overflow(const char* rule) {
    return detail::diagnostic(diagnostic_codes::resource_limit,
        std::string("durability rule ") + rule + " exceeds the exact arithmetic range");
}

[[nodiscard]] bool within_health(const math::Fixed value) noexcept {
    return value.raw() > 0 && value.raw() <= health_limit_raw;
}

// Sums of maximum (T) and current (S) health over the destroyable hardpoints.
struct HardpointTotals {
    std::int64_t maximum{};
    std::int64_t current{};
};

[[nodiscard]] HardpointTotals totals(const DurabilityProfile& profile, const DurabilityState& state) noexcept {
    HardpointTotals result;
    for (std::size_t index = 0; index < profile.hardpoints.size(); ++index) {
        if (profile.hardpoints[index].destroyable) {
            result.maximum += profile.hardpoints[index].max_health.raw();
            result.current += state.hardpoints[index].raw();
        }
    }
    return result;
}

// Whether the profile has a hardpoint of `role`, and whether one of them is not destroyed.
[[nodiscard]] bool role_alive(
    const DurabilityProfile& profile, const DurabilityState& state, const HardpointRole role, const bool when_absent) noexcept {
    bool present = false;
    for (std::size_t index = 0; index < profile.hardpoints.size(); ++index) {
        if (profile.hardpoints[index].role != role) {
            continue;
        }
        present = true;
        if (!hardpoint_destroyed(profile, state, index) && !hardpoint_disabled(state, index)) {
            return true;
        }
    }
    return !present && when_absent;
}

} // namespace

const DurabilityProfile* DurabilityTable::find(const TypeId type_id) const noexcept {
    const auto found = std::lower_bound(profiles.begin(), profiles.end(), type_id,
        [](const DurabilityProfile& profile, const TypeId id) { return profile.type_id < id; });
    return found != profiles.end() && found->type_id == type_id ? &*found : nullptr;
}

core::Result<void> validate_durability(const DurabilityTable& table) {
    const auto invalid = [](const std::string& message) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, message));
    };
    if (table.profiles.empty()) {
        return core::Result<void>::success();
    }
    const auto& rules = table.rules;
    if (rules.hull_vs_hardpoints.raw() < 0 || rules.hull_vs_hardpoints.raw() > health_limit_raw
        || rules.engines_disabled_speed.raw() < 0 || rules.engines_disabled_speed.raw() > math::Fixed::scale
        || rules.damaged_fraction.raw() < 0 || rules.damaged_fraction.raw() > math::Fixed::scale) {
        return invalid("durability rules must be >= 0, the speed modifier and damaged fraction at most 1");
    }
    for (std::size_t index = 0; index < table.profiles.size(); ++index) {
        const auto& profile = table.profiles[index];
        const auto context = "durability profile of type " + std::to_string(profile.type_id);
        if (index != 0 && profile.type_id <= table.profiles[index - 1].type_id) {
            return invalid("durability type IDs must strictly increase at index " + std::to_string(index));
        }
        if (!within_health(profile.max_hull)) {
            return invalid(context + ": hull must be in (0, " + std::to_string(max_durability_health) + "]");
        }
        if (profile.max_speed && (profile.max_speed->raw() < 0 || profile.max_speed->raw() > health_limit_raw)) {
            return invalid(context + ": maximum speed must be in [0, " + std::to_string(max_durability_health) + "]");
        }
        if (profile.hardpoints.size() > max_hardpoints_per_type) {
            return invalid(context + ": more than " + std::to_string(max_hardpoints_per_type) + " hardpoints");
        }
        for (std::size_t slot = 0; slot < profile.hardpoints.size(); ++slot) {
            const auto& hardpoint = profile.hardpoints[slot];
            const auto where = context + " hardpoint " + std::to_string(slot);
            if (hardpoint.destroyable ? !within_health(hardpoint.max_health) : hardpoint.max_health.raw() != 0) {
                return invalid(where + ": a destroyable hardpoint needs health in (0, "
                    + std::to_string(max_durability_health) + "], any other hardpoint zero");
            }
            if (hardpoint.repair_amount_per_frame.raw() < 0 || hardpoint.repair_cost_per_frame.raw() < 0
                || hardpoint.repair_amount_per_frame.raw() > health_limit_raw) {
                return invalid(where + ": repair values must be >= 0 and bounded by the health limit");
            }
        }
    }
    return validate_damage(table);
}

DurabilityState full_durability(const DurabilityProfile& profile) {
    DurabilityState state;
    state.hull = profile.max_hull;
    state.shields = profile.max_shields;
    state.energy = profile.max_energy;
    state.hardpoints.reserve(profile.hardpoints.size());
    for (const auto& hardpoint : profile.hardpoints) {
        state.hardpoints.push_back(hardpoint.max_health);
    }
    return state;
}

bool hardpoint_destroyed(const DurabilityProfile& profile, const DurabilityState& state, const std::size_t index) noexcept {
    return profile.hardpoints[index].destroyable && state.hardpoints[index].raw() <= 0;
}

bool hardpoint_disabled(const DurabilityState& state, const std::size_t index) noexcept {
    return index < state.disabled.size() && state.disabled[index];
}

void carry_station_hardpoints(const DurabilityProfile& previous_profile, const DurabilityState& previous,
    DurabilityState& replacement) {
    replacement.disabled.resize(replacement.hardpoints.size());
    replacement.repairing_players.resize(replacement.hardpoints.size());
    const auto count = std::min(previous.hardpoints.size(), replacement.hardpoints.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (index < previous.repairing_players.size() && !previous.repairing_players[index].empty()) {
            replacement.hardpoints[index] = previous.hardpoints[index];
            replacement.repairing_players[index] = previous.repairing_players[index];
            replacement.disabled[index] = true;
        } else if (hardpoint_destroyed(previous_profile, previous, index) || hardpoint_disabled(previous, index)) {
            replacement.hardpoints[index] = math::Fixed::from_decimal("0.1").value(); // WPR-52, debug build
            replacement.disabled[index] = true;
        }
    }
}

HardpointState hardpoint_state(const DurabilityProfile& profile, const DurabilityRules& rules,
    const DurabilityState& state, const std::size_t index) noexcept {
    const auto& hardpoint = profile.hardpoints[index];
    if (!hardpoint.destroyable) {
        return HardpointState::intact;
    }
    const auto health = state.hardpoints[index].raw();
    if (health <= 0) {
        return HardpointState::destroyed;
    }
    // health / max < fraction, compared exactly as health x 2^24 < max x fraction_raw.
    const auto scaled_health = wide::multiply_u64(static_cast<std::uint64_t>(health), scale);
    const auto threshold = wide::multiply_u64(
        static_cast<std::uint64_t>(hardpoint.max_health.raw()), static_cast<std::uint64_t>(rules.damaged_fraction.raw()));
    return wide::compare(scaled_health, threshold) < 0 ? HardpointState::damaged : HardpointState::intact;
}

bool weapon_enabled(const DurabilityProfile& profile, const DurabilityState& state, const std::size_t index) noexcept {
    return profile.hardpoints[index].role == HardpointRole::weapon && !hardpoint_destroyed(profile, state, index)
        && !hardpoint_disabled(state, index);
}

bool engines_online(const DurabilityProfile& profile, const DurabilityState& state) noexcept {
    return !state.engines_disabled_until && role_alive(profile, state, HardpointRole::engine, true);
}

void disable_engines(DurabilityState& state, const std::uint32_t frames, const std::uint64_t frame) noexcept {
    // EN-08: reapplication extends to the later end, rather than stacking durations.
    const auto until = frame > std::numeric_limits<std::uint64_t>::max() - frames
        ? std::numeric_limits<std::uint64_t>::max() : frame + frames;
    state.engines_disabled_until = std::max(state.engines_disabled_until.value_or(0), until);
}

bool service_disabled_engines(DurabilityState& state, const std::uint64_t frame) noexcept {
    if (!state.engines_disabled_until || frame < *state.engines_disabled_until) return false;
    state.engines_disabled_until.reset();
    return true;
}

bool shields_online(const DurabilityProfile& profile, const DurabilityState& state) noexcept {
    return role_alive(profile, state, HardpointRole::shield_generator, true);
}

bool launch_ready(const DurabilityProfile& profile, const DurabilityState& state) noexcept {
    return role_alive(profile, state, HardpointRole::fighter_bay, false);
}

math::Fixed max_speed_factor(
    const DurabilityProfile& profile, const DurabilityRules& rules, const DurabilityState& state) noexcept {
    return engines_online(profile, state) ? math::Fixed::from_raw(math::Fixed::scale) : rules.engines_disabled_speed;
}

bool damage_target_valid(const DurabilityProfile& profile, const std::uint32_t target) noexcept {
    return target == hull_target || (target < profile.hardpoints.size() && profile.hardpoints[target].destroyable);
}

DamageOutcome apply_damage(const DurabilityProfile& profile, DurabilityState& state, const std::uint32_t target,
    const math::Fixed amount) noexcept {
    DamageOutcome outcome;
    const auto hit = [&amount](math::Fixed& health) {
        health = amount.raw() >= health.raw() ? math::Fixed{} : math::Fixed::from_raw(health.raw() - amount.raw());
        return health.raw() == 0;
    };
    if (amount.raw() <= 0) {
        return outcome;
    }
    if (target == hull_target) {
        outcome.unit_destroyed = state.hull.raw() > 0 && hit(state.hull);
        return outcome;
    }
    auto& health = state.hardpoints[target];
    if (health.raw() <= 0 || !hit(health)) {
        return outcome;
    }
    outcome.destroyed_hardpoint = target;
    if (profile.destroyed_with_hardpoints) {
        bool alive = false;
        for (std::size_t index = 0; index < profile.hardpoints.size(); ++index) {
            alive = alive || (profile.hardpoints[index].destroyable && state.hardpoints[index].raw() > 0);
        }
        if (!alive) {
            state.hull = math::Fixed{};
            outcome.unit_destroyed = true;
        }
    }
    return outcome;
}

core::Result<ServiceOutcome> service_durability(
    const DurabilityProfile& profile, const DurabilityRules& rules, DurabilityState& state) {
    ServiceOutcome outcome;
    const auto sums = totals(profile, state);
    if (sums.maximum == 0 || state.hull.raw() <= 0) {
        return core::Result<ServiceOutcome>::success(std::move(outcome));
    }
    const auto t = sums.maximum;
    const auto s = sums.current;
    const auto m = profile.max_hull.raw();
    const auto c = rules.hull_vs_hardpoints.raw();
    const auto k = math::Fixed::scale;

    // HS-02: the hull may not exceed min(1, S / T + C) of its maximum.
    if (profile.destroyed_with_hardpoints) {
        std::int64_t cap{};
        const auto numerator = plus(times(times(exact(s), m), k), times(times(exact(c), m), t));
        if (!quotient(numerator, times(exact(t), k), cap)) {
            return core::Result<ServiceOutcome>::failure(overflow("HS-02"));
        }
        cap = std::min(cap, m);
        if (state.hull.raw() > cap) {
            state.hull = math::Fixed::from_raw(cap);
            if (cap == 0) {
                outcome.unit_destroyed = true;
                return core::Result<ServiceOutcome>::success(std::move(outcome));
            }
        }
    }

    // HS-03: excess = S - T x min(1, H / M + C), over the common denominator M x 2^24.
    const auto h = state.hull.raw();
    const auto current = times(times(exact(s), m), k);
    const auto ceiling = times(times(exact(t), m), k);
    const auto allowed = plus(times(times(exact(t), h), k), times(times(exact(t), c), m));
    const auto limit = wide::compare(ceiling.value, allowed.value) <= 0 ? ceiling : allowed;
    if (current.overflow || limit.overflow) {
        return core::Result<ServiceOutcome>::failure(overflow("HS-03"));
    }
    if (wide::compare(current.value, limit.value) <= 0) {
        return core::Result<ServiceOutcome>::success(std::move(outcome));
    }
    const auto excess = minus(current, limit);
    const auto denominator = times(times(exact(m), k), t);
    // HS-04: each live destroyable hardpoint loses excess x current_i / T, all from the values
    // before this service.
    const auto before = state.hardpoints;
    for (std::size_t index = 0; index < profile.hardpoints.size(); ++index) {
        if (!profile.hardpoints[index].destroyable || before[index].raw() <= 0) {
            continue;
        }
        std::int64_t loss{};
        if (!quotient(times(excess, before[index].raw()), denominator, loss)) {
            return core::Result<ServiceOutcome>::failure(overflow("HS-04"));
        }
        if (loss <= 0) {
            continue;
        }
        const auto remaining = before[index].raw() > loss ? before[index].raw() - loss : 0;
        state.hardpoints[index] = math::Fixed::from_raw(remaining);
        if (remaining == 0) {
            outcome.destroyed_hardpoints.push_back(static_cast<std::uint32_t>(index));
        }
    }
    return core::Result<ServiceOutcome>::success(std::move(outcome));
}

core::Result<RepairOutcome> repair_frame(
    const DurabilityProfile& profile, DurabilityState& state, const std::size_t index, const math::Fixed credits) {
    RepairOutcome outcome;
    outcome.stopped = true;
    if (index >= profile.hardpoints.size()) {
        return core::Result<RepairOutcome>::success(outcome);
    }
    const auto& hardpoint = profile.hardpoints[index];
    // HR-02, HR-03: a destroyed or unrepairable hardpoint stops; so does a player who cannot pay.
    if (!hardpoint.destroyable || state.hardpoints[index].raw() <= 0 || hardpoint.repair_amount_per_frame.raw() <= 0
        || hardpoint.repair_cost_per_frame > credits) {
        return core::Result<RepairOutcome>::success(outcome);
    }
    outcome.paid = true;
    outcome.cost = hardpoint.repair_cost_per_frame;
    const auto raised = std::min(
        state.hardpoints[index].raw() + hardpoint.repair_amount_per_frame.raw(), hardpoint.max_health.raw());
    state.hardpoints[index] = math::Fixed::from_raw(raised);
    if (raised == hardpoint.max_health.raw() && index < state.disabled.size()) state.disabled[index] = false;

    // HR-04: a hull below the combined hardpoint fraction rises to H x (1 + S / T - H / M).
    const auto sums = totals(profile, state);
    const auto h = state.hull.raw();
    const auto m = profile.max_hull.raw();
    const auto hull_side = times(exact(h), sums.maximum);
    const auto hardpoint_side = times(exact(sums.current), m);
    if (h > 0 && wide::compare(hull_side.value, hardpoint_side.value) < 0) {
        const auto factor = minus(plus(times(exact(sums.maximum), m), hardpoint_side), hull_side);
        std::int64_t hull{};
        if (!quotient(times(factor, h), times(exact(sums.maximum), m), hull)) {
            return core::Result<RepairOutcome>::failure(overflow("HR-04"));
        }
        state.hull = math::Fixed::from_raw(std::min(hull, m));
    }
    // HR-05: the repair ends when the hardpoint is back at full health.
    outcome.stopped = raised == hardpoint.max_health.raw();
    return core::Result<RepairOutcome>::success(outcome);
}

std::string_view to_string(const HardpointRole role) noexcept {
    switch (role) {
    case HardpointRole::other:
        return "other";
    case HardpointRole::weapon:
        return "weapon";
    case HardpointRole::engine:
        return "engine";
    case HardpointRole::shield_generator:
        return "shield_generator";
    case HardpointRole::fighter_bay:
        return "fighter_bay";
    case HardpointRole::special_ability:
        return "special_ability";
    }
    return "unknown";
}

std::string_view to_string(const HardpointState state) noexcept {
    switch (state) {
    case HardpointState::intact:
        return "intact";
    case HardpointState::damaged:
        return "damaged";
    case HardpointState::destroyed:
        return "destroyed";
    }
    return "unknown";
}

} // namespace eawr::sim::tactical
