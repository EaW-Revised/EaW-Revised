#include "eawr/sim/tactical/durability.hpp"

#include "eawr/sim/tactical/damage.hpp"

#include "../math/wide.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace eawr::sim::tactical {

core::Result<DurabilityTable> prepare_durability(const DurabilityTable& source, core::LoadNoteBuffer& notes) {
    return core::normalize_owned_load_table(source, notes, normalize_durability, validate_durability);
}

math::Fixed healing_fraction(const DurabilityProfile& profile, const DurabilityState& state) {
    auto current = state.hull;
    auto maximum = profile.max_hull;
    if (profile.destroyed_with_hardpoints && std::any_of(profile.hardpoints.begin(), profile.hardpoints.end(),
        [](const auto& point) { return point.destroyable; })) {
        current = {}; maximum = {};
        for (std::size_t i = 0; i < profile.hardpoints.size(); ++i) {
            if (!profile.hardpoints[i].destroyable || profile.hardpoints[i].max_health.raw() <= 0
                || state.hardpoints[i].raw() <= 0) continue;
            current = math::Fixed::from_raw(current.raw() + state.hardpoints[i].raw());
            maximum = math::Fixed::from_raw(maximum.raw() + profile.hardpoints[i].max_health.raw());
        }
    }
    return maximum.raw() > 0 ? math::divide(current, maximum).value() : math::Fixed{};
}

std::optional<math::Fixed> time_to_death(const DurabilityProfile& profile, const DurabilityState& state) noexcept {
    if (state.damage_rate.raw() <= 0) return std::nullopt;
    std::int64_t health = 0;
    bool hardpoints = false;
    for (std::size_t index = 0; index < profile.hardpoints.size() && index < state.hardpoints.size(); ++index) {
        if (!profile.hardpoints[index].destroyable) continue;
        hardpoints = true;
        health += std::max<std::int64_t>(0, state.hardpoints[index].raw());
    }
    if (!hardpoints) health = state.hull.raw();
    health += state.shields.raw();
    math::Fixed result;
    if (!math::try_divide(math::Fixed::from_raw(health), state.damage_rate, result)) return std::nullopt;
    return result;
}

core::Result<std::uint32_t> heal_health(const DurabilityProfile& profile, DurabilityState& state,
    const math::Fixed percent, const math::Fixed amount) {
    std::uint32_t picked = hull_target;
    math::Fixed lowest = math::Fixed::from_raw(math::Fixed::scale);
    for (std::size_t i = 0; i < profile.hardpoints.size(); ++i) {
        const auto& point = profile.hardpoints[i];
        if (point.destroyable && point.max_health.raw() == 0) {
            state.hardpoints[i] = {}; // HD-06: healing cannot revive a zero-maximum slot.
            continue;
        }
        if (!point.destroyable || state.hardpoints[i].raw() <= 0) continue;
        const auto fraction = math::divide(state.hardpoints[i], point.max_health).value();
        if (fraction < lowest) { lowest = fraction; picked = static_cast<std::uint32_t>(i); }
    }
    auto& health = picked == hull_target ? state.hull : state.hardpoints[picked];
    const auto maximum = picked == hull_target ? profile.max_hull : profile.hardpoints[picked].max_health;
    const auto increment = math::multiply(maximum, percent);
    if (!increment) return core::Result<std::uint32_t>::failure(increment.error());
    const auto flat = math::add(increment.value(), amount);
    if (!flat) return core::Result<std::uint32_t>::failure(flat.error());
    // Clamp before adding so a very large authored increment cannot overflow health.
    health = math::Fixed::from_raw(health.raw() + std::min(std::max<std::int64_t>(flat.value().raw(), 0),
        std::max<std::int64_t>(maximum.raw() - health.raw(), 0)));
    return core::Result<std::uint32_t>::success(picked);
}
namespace {

namespace wide = math::detail;

[[nodiscard]] std::string fixed_decimal(const math::Fixed value) {
    const auto raw = value.raw();
    const auto magnitude =
        raw < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(raw) : static_cast<std::uint64_t>(raw);
    const auto scale = static_cast<std::uint64_t>(math::Fixed::scale);
    auto text = (raw < 0 ? std::string("-") : std::string{}) + std::to_string(magnitude / scale);
    auto remainder = magnitude % scale;
    if (remainder)
        text += '.';
    while (remainder) {
        remainder *= 10;
        text += static_cast<char>('0' + remainder / scale);
        remainder %= scale;
    }
    return text;
}

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

[[nodiscard]] bool within_hardpoint_health(const math::Fixed value) noexcept {
    return value.raw() == 0 || within_health(value); // HD-06: zero starts destroyed.
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

const DifficultyScale* DurabilityTable::difficulty_of(const PlayerId player) const noexcept {
    const auto found = std::lower_bound(difficulty.begin(), difficulty.end(), player,
        [](const DifficultyScale& entry, const PlayerId id) { return entry.player < id; });
    return found != difficulty.end() && found->player == player ? &*found : nullptr;
}

core::Result<DurabilityProfile> scale_profile(const DurabilityProfile& profile, const DifficultyScale& factors) {
    using Result = core::Result<DurabilityProfile>;
    auto scaled = profile;
    const auto bounded_product = [](math::Fixed value, math::Fixed factor, std::int64_t minimum) {
        auto product = math::multiply(value, factor);
        if (!product) return math::Fixed::from_raw((value.raw() < 0) != (factor.raw() < 0) ? minimum : health_limit_raw);
        return math::Fixed::from_raw(std::clamp(product.value().raw(), minimum, health_limit_raw));
    };
    scaled.max_hull = bounded_product(profile.max_hull, factors.health, 1);
    for (auto& hardpoint : scaled.hardpoints) {
        if (hardpoint.destroyable) hardpoint.max_health = bounded_product(hardpoint.max_health, factors.health, 0);
    }
    if (profile.max_shields.raw() > 0) {
        const auto shields = math::multiply(profile.max_shields, factors.shield);
        scaled.max_shields =
            shields ? shields.value() : math::Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    }
    return Result::success(std::move(scaled));
}

void normalize_durability(DurabilityTable& table, core::LoadNoteBuffer& notes) {
    const auto clamp = [&](math::Fixed& value, std::int64_t low, std::int64_t high, const char* tag,
                           const std::string& message) {
        const auto before = value.raw();
        const auto after = std::clamp(before, low, high);
        if (before == after) return;
        core::LoadNote note;
        note.code = "EAWR-LOAD-0100";
        note.tag = tag;
        const auto separator = message.find(": ");
        note.object = separator == std::string::npos ? "durability rules" : message.substr(0, separator);
        constexpr std::string_view prefix = "durability profile of type ";
        if (note.object.starts_with(prefix))
            note.object.erase(0, prefix.size());
        note.authored_value = fixed_decimal(value);
        note.diagnostic_code = "EAWR-SIM-0305";
        note.message = message;
        core::LoadNoteBuffer temporary;
        const auto effective = core::clamp_note(before, low, high, std::move(note), temporary);
        core::LoadNoteCollector collector;
        collector.merge(temporary);
        for (auto emitted : collector.snapshot()) {
            emitted.effective_value = fixed_decimal(math::Fixed::from_raw(effective));
            notes.append(std::move(emitted));
        }
        value = math::Fixed::from_raw(effective);
    };
    const auto sort_unique = [&](auto& entries, const auto key, const char* code, const char* label) {
        const auto original = entries;
        std::stable_sort(entries.begin(), entries.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); });
        const auto end =
            std::unique(entries.begin(), entries.end(), [&](const auto& a, const auto& b) { return key(a) == key(b); });
        entries.erase(end, entries.end());
        if (entries != original) {
            core::LoadNote note;
            note.code = code;
            note.object = label;
            note.action = core::LoadAction::clamped;
            note.authored_value = std::to_string(original.size());
            note.effective_value = std::to_string(entries.size());
            note.diagnostic_code = "EAWR-SIM-0305";
            note.message = std::string(label) + " IDs must strictly increase";
            notes.append(std::move(note));
        }
    };
    sort_unique(
        table.profiles, [](const auto& profile) { return profile.type_id; }, "EAWR-LOAD-0104", "durability type");
    sort_unique(
        table.difficulty, [](const auto& scale) { return scale.player; }, "EAWR-LOAD-0105", "difficulty player");
    const auto rule_message = "durability rules must be >= 0, the speed modifier and damaged fraction at most 1";
    clamp(table.rules.hull_vs_hardpoints, 0, health_limit_raw, "Hull_Vs_Hard_Points_Health_Constraint", rule_message);
    clamp(table.rules.engines_disabled_speed, 0, math::Fixed::scale, "Engines_Disabled_Speed_Modifier", rule_message);
    clamp(table.rules.damaged_fraction, 0, math::Fixed::scale, "Health_Low_Percent_Threshold", rule_message);
    for (auto& profile : table.profiles) {
        const auto type_name = "type#" + std::to_string(profile.type_id);
        const auto context = "durability profile of type " + type_name;
        clamp(profile.max_hull, 1, health_limit_raw, "Tactical_Health",
              context + ": hull must be in (0, " + std::to_string(max_durability_health) + "]");
        if (profile.max_speed)
            clamp(*profile.max_speed, 0, health_limit_raw, "Max_Speed",
                  context + ": maximum speed must be in [0, " + std::to_string(max_durability_health) + "]");
        if (profile.hardpoints.size() > max_hardpoints_per_type) {
            core::LoadNote note;
            note.code = "EAWR-LOAD-0101";
            note.tag = "HardPoints";
            note.object = type_name;
            note.authored_value = std::to_string(profile.hardpoints.size());
            note.action = core::LoadAction::clamped;
            note.effective_value = std::to_string(max_hardpoints_per_type);
            note.diagnostic_code = "EAWR-SIM-0305";
            note.message = context + ": more than " + std::to_string(max_hardpoints_per_type) + " hardpoints";
            notes.append(std::move(note));
            profile.hardpoints.resize(max_hardpoints_per_type);
        }
        for (std::size_t slot = 0; slot < profile.hardpoints.size(); ++slot) {
            auto& hp = profile.hardpoints[slot];
            const auto where = context + " hardpoint " + std::to_string(slot);
            const auto health_message = where + ": a destroyable hardpoint needs health in [0, " +
                                        std::to_string(max_durability_health) + "], any other hardpoint zero";
            if (hp.destroyable) clamp(hp.max_health, 0, health_limit_raw, "Health", health_message);
            else if (hp.max_health.raw() != 0) {
                core::LoadNote note;
                note.code = "EAWR-LOAD-0102";
                note.object = type_name + " hardpoint " + std::to_string(slot);
                note.tag = "Health";
                note.authored_value = fixed_decimal(hp.max_health);
                note.action = core::LoadAction::clamped;
                note.effective_value = "0";
                note.diagnostic_code = "EAWR-SIM-0305";
                note.message = health_message;
                notes.append(std::move(note));
                hp.max_health = {};
            }
            const auto repair_message = where + ": repair values must be >= 0 and bounded by the health limit";
            clamp(hp.repair_amount_per_frame, 0, health_limit_raw, "Repair_Amount_Per_Frame", repair_message);
            clamp(hp.repair_cost_per_frame, 0, std::numeric_limits<std::int64_t>::max(), "Repair_Cost_Per_Frame",
                  repair_message);
        }
    }
    const auto limit = max_damage_multiplier * math::Fixed::scale;
    for (auto& entry : table.difficulty) {
        const auto message = "difficulty scale of player " + std::to_string(entry.player) +
                             ": the health scale must be in (0, " + std::to_string(max_damage_multiplier) +
                             "], the shield and damage scales in [0, " + std::to_string(max_damage_multiplier) + "]";
        clamp(entry.health, 1, limit, "difficulty health", message);
        clamp(entry.shield, 0, limit, "difficulty shield", message);
        clamp(entry.damage, 0, limit, "difficulty damage", message);
        for (const auto& profile : table.profiles) {
            const auto inspect = [&](math::Fixed value, std::int64_t minimum, const char* tag) {
                const auto product = math::multiply(value, entry.health);
                if (product && product.value().raw() >= minimum && product.value().raw() <= health_limit_raw) return;
                core::LoadNote note;
                note.code = "EAWR-LOAD-0103";
                const auto key = std::to_string(entry.player) + ":" + std::to_string(profile.type_id) + ":" + tag;
                if (std::find(table.noted_scaled_health.begin(), table.noted_scaled_health.end(), key) !=
                    table.noted_scaled_health.end())
                    return;
                table.noted_scaled_health.push_back(key);
                note.object = "type#" + std::to_string(profile.type_id);
                note.tag = tag;
                note.authored_value = fixed_decimal(value);
                note.action = core::LoadAction::clamped;
                const auto effective =
                    product ? std::clamp(product.value().raw(), minimum, health_limit_raw) : health_limit_raw;
                note.effective_value = fixed_decimal(math::Fixed::from_raw(effective));
                note.diagnostic_code = "EAWR-SIM-0305";
                note.message = "difficulty scale of player " + std::to_string(entry.player) +
                               ": scaled health outside supported range";
                notes.append(std::move(note));
            };
            inspect(profile.max_hull, 1, "Tactical_Health");
            for (const auto& hp : profile.hardpoints)
                if (hp.destroyable)
                    inspect(hp.max_health, 0, "Health");
        }
    }
    normalize_damage(table, notes);
}

core::Result<void> validate_durability(const DurabilityTable& table) {
    const auto invalid = [](const std::string& message) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, message));
    };
    if (table.profiles.empty()) {
        return core::Result<void>::success();
    }
    const auto& rules = table.rules;
    if (rules.hull_vs_hardpoints.raw() < 0 || rules.hull_vs_hardpoints.raw() > health_limit_raw ||
        rules.engines_disabled_speed.raw() < 0 || rules.engines_disabled_speed.raw() > math::Fixed::scale ||
        rules.damaged_fraction.raw() < 0 || rules.damaged_fraction.raw() > math::Fixed::scale) {
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
            if (hardpoint.destroyable && !within_hardpoint_health(hardpoint.max_health)) {
                return invalid(where + ": a destroyable hardpoint needs health in [0, "
                    + std::to_string(max_durability_health) + "], any other hardpoint zero");
            }
            if (hardpoint.repair_amount_per_frame.raw() < 0 || hardpoint.repair_cost_per_frame.raw() < 0
                || hardpoint.repair_amount_per_frame.raw() > health_limit_raw) {
                return invalid(where + ": repair values must be >= 0 and bounded by the health limit");
            }
        }
    }
    for (std::size_t index = 0; index < table.difficulty.size(); ++index) {
        const auto& entry = table.difficulty[index];
        const auto context = "difficulty scale of player " + std::to_string(entry.player);
        if (index != 0 && entry.player <= table.difficulty[index - 1].player) {
            return invalid("difficulty player IDs must strictly increase at index " + std::to_string(index));
        }
        const auto bounded = [](const math::Fixed value) {
            return value.raw() >= 0 && value.raw() <= max_damage_multiplier * math::Fixed::scale;
        };
        if (entry.health.raw() <= 0 || !bounded(entry.health) || !bounded(entry.shield) || !bounded(entry.damage)) {
            return invalid(context + ": the health scale must be in (0, " + std::to_string(max_damage_multiplier)
                + "], the shield and damage scales in [0, " + std::to_string(max_damage_multiplier) + "]");
        }
        for (const auto& profile : table.profiles) {
            const auto scaled = scale_profile(profile, entry);
            if (!scaled) return invalid(context + ": " + scaled.error().message);
            if (!within_health(scaled.value().max_hull)) {
                return invalid(context + ": the scaled hull of type " + std::to_string(profile.type_id)
                    + " must be in (0, " + std::to_string(max_durability_health) + "]");
            }
            for (const auto& hardpoint : scaled.value().hardpoints) {
                if (hardpoint.destroyable && !within_hardpoint_health(hardpoint.max_health)) {
                    return invalid(context + ": a scaled hardpoint health of type " + std::to_string(profile.type_id)
                        + " must be in [0, " + std::to_string(max_durability_health) + "]");
                }
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
    const DurabilityProfile& replacement_profile, DurabilityState& replacement) {
    replacement.disabled.resize(replacement.hardpoints.size());
    replacement.repairing_players.resize(replacement.hardpoints.size());
    const auto count = std::min(previous.hardpoints.size(), replacement.hardpoints.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (replacement_profile.hardpoints[index].max_health.raw() == 0) {
            replacement.hardpoints[index] = {};
            replacement.disabled[index] = false;
            replacement.repairing_players[index].clear();
            continue; // HD-06: neither repairing nor disabled carry may introduce positive health.
        }
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
    if (frames == indefinite_engine_disable_frames) {
        state.engines_disabled_until = std::numeric_limits<std::uint64_t>::max();
        return;
    }
    // EN-08: reapplication extends to the later end, rather than stacking durations.
    const auto until = frame > std::numeric_limits<std::uint64_t>::max() - frames
        ? std::numeric_limits<std::uint64_t>::max() : frame + frames;
    state.engines_disabled_until = std::max(state.engines_disabled_until.value_or(0), until);
}

bool service_disabled_engines(DurabilityState& state, const std::uint64_t frame) noexcept {
    if (!state.engines_disabled_until || *state.engines_disabled_until == std::numeric_limits<std::uint64_t>::max()
        || frame < *state.engines_disabled_until) return false;
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
    if (hardpoint.destroyable && hardpoint.max_health.raw() == 0) {
        state.hardpoints[index] = {}; // HD-06: never complete a repair by reviving this slot.
        return core::Result<RepairOutcome>::success(outcome);
    }
    // HR-05: another healing source may have filled this slot before its paid service.
    // Complete even without credits; no health increment means no additional debit.
    if (!hardpoint.destroyable || state.hardpoints[index].raw() <= 0) {
        return core::Result<RepairOutcome>::success(outcome);
    }
    if (state.hardpoints[index] >= hardpoint.max_health) {
        if (index < state.disabled.size()) state.disabled[index] = false;
        return core::Result<RepairOutcome>::success(outcome);
    }
    // WSL-41/42: zero authored amount still pays while below maximum.
    if (hardpoint.repair_cost_per_frame > credits) {
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

std::vector<std::vector<PlayerId>> reserve_hardpoint_repairs(
    const DurabilityProfile& profile, const DurabilityState& state, const std::span<RepairBudget> budgets) {
    std::vector<std::vector<PlayerId>> paid(state.repairing_players.size());
    for (std::size_t slot = 0; slot < state.repairing_players.size(); ++slot) {
        const auto& authored = profile.hardpoints[slot];
        auto health = state.hardpoints[slot].raw();
        if (health <= 0 || !authored.destroyable || health >= authored.max_health.raw()) continue;
        for (const auto payer : state.repairing_players[slot]) {
            const auto account = std::find_if(budgets.begin(), budgets.end(),
                [&](const RepairBudget& budget) { return budget.player == payer; });
            if (account == budgets.end() || account->credits < authored.repair_cost_per_frame) continue;
            account->credits = math::Fixed::from_raw(account->credits.raw() - authored.repair_cost_per_frame.raw());
            paid[slot].push_back(payer);
            health = std::min(health + authored.repair_amount_per_frame.raw(), authored.max_health.raw());
            if (health == authored.max_health.raw()) break;
        }
    }
    return paid;
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
