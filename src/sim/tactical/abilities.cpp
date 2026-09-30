#include "eawr/sim/tactical/abilities.hpp"

#include "../replay_internal.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace eawr::sim::tactical {
namespace {

using math::Fixed;

constexpr std::int64_t one_raw = Fixed::scale;

struct KindName {
    AbilityKind kind;
    std::string_view name;
};

constexpr std::array<KindName, 5> kind_names{{
    {AbilityKind::defend, "DEFEND"},
    {AbilityKind::turbo, "TURBO"},
    {AbilityKind::power_to_weapons, "POWER_TO_WEAPONS"},
    {AbilityKind::spoiler_lock, "SPOILER_LOCK"},
    {AbilityKind::ion_cannon_shot, "ION_CANNON_SHOT"},
}};

[[nodiscard]] bool iequals(const std::string_view left, const std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::toupper(static_cast<unsigned char>(left[index])) != std::toupper(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

// frames x multiplier, rounded half up; the multiplier is non-negative here.
[[nodiscard]] std::uint32_t scaled_frames(const std::uint32_t frames, const Fixed multiplier) noexcept {
    if (multiplier.raw() <= 0) return 0;
    const auto product = static_cast<std::int64_t>(frames) * multiplier.raw();
    return static_cast<std::uint32_t>((product + one_raw / 2) / one_raw);
}

[[nodiscard]] Fixed product(const Fixed left, const Fixed right) noexcept {
    // Both factors are validated to [-64, 64]: the exact product fits, rounded to nearest.
    const auto raw = left.raw() * right.raw();
    const auto half = one_raw / 2;
    return Fixed::from_raw(raw >= 0 ? (raw + half) / one_raw : -((-raw + half) / one_raw));
}

[[nodiscard]] Fixed modifier_of(const AbilityModifiers& modifiers, const AbilityModifier modifier) noexcept {
    switch (modifier) {
    case AbilityModifier::weapon_delay: return modifiers.weapon_delay;
    case AbilityModifier::shield_regen: return modifiers.shield_regen;
    case AbilityModifier::shield_regen_interval: return modifiers.shield_regen_interval;
    case AbilityModifier::energy_regen: return modifiers.energy_regen;
    case AbilityModifier::energy_regen_interval: return modifiers.energy_regen_interval;
    case AbilityModifier::speed: return modifiers.speed;
    }
    return Fixed::from_raw(one_raw);
}

} // namespace

std::string_view to_string(const AbilityKind kind) noexcept {
    for (const auto& entry : kind_names) {
        if (entry.kind == kind) return entry.name;
    }
    return "NONE";
}

std::string_view to_string(const AbilityAction action) noexcept {
    switch (action) {
    case AbilityAction::activate: return "activate";
    case AbilityAction::deactivate: return "deactivate";
    case AbilityAction::autofire_on: return "autofire_on";
    case AbilityAction::autofire_off: return "autofire_off";
    }
    return "unknown";
}

AbilityKind ability_kind(const std::string_view name) noexcept {
    for (const auto& entry : kind_names) {
        if (iequals(entry.name, name)) return entry.kind;
    }
    return AbilityKind::none;
}

const UnitAbilityProfile* AbilityTable::find(const TypeId type_id) const noexcept {
    const auto found = std::lower_bound(profiles.begin(), profiles.end(), type_id,
        [](const UnitAbilityProfile& profile, const TypeId value) { return profile.type_id < value; });
    return found != profiles.end() && found->type_id == type_id ? &*found : nullptr;
}

bool AbilityTable::human(const PlayerId player) const noexcept {
    return std::binary_search(humans.begin(), humans.end(), player);
}

core::Result<void> validate_abilities(const AbilityTable& table) {
    const auto invalid = [](const std::string& message) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "ability table: " + message));
    };
    for (std::size_t index = 1; index < table.profiles.size(); ++index) {
        if (table.profiles[index - 1].type_id >= table.profiles[index].type_id) {
            return invalid("type IDs must strictly increase");
        }
    }
    for (std::size_t index = 1; index < table.humans.size(); ++index) {
        if (table.humans[index - 1] >= table.humans[index]) return invalid("humans must strictly increase");
    }
    const auto bounded = [](const Fixed value, const bool positive) {
        return value.raw() <= max_ability_multiplier * one_raw && value.raw() >= -max_ability_multiplier * one_raw
            && (!positive || value.raw() > 0);
    };
    for (const auto& profile : table.profiles) {
        const auto type = " (type " + std::to_string(profile.type_id) + ")";
        if (profile.abilities.empty() || profile.abilities.size() > max_abilities_per_type) {
            return invalid("a type has one or two abilities" + type);
        }
        if (profile.abilities.size() == 2 && profile.abilities[0].kind == profile.abilities[1].kind) {
            return invalid("a type's abilities are of distinct kinds" + type);
        }
        for (const auto& ability : profile.abilities) {
            if (ability.kind == AbilityKind::none || static_cast<std::uint8_t>(ability.kind) > 5U) {
                return invalid("unknown ability kind" + type);
            }
            // AB-60: only ION_CANNON_SHOT is a team ability, and it has no time limit.
            if (ability.team != (ability.kind == AbilityKind::ion_cannon_shot && ability.team)
                || (ability.kind == AbilityKind::ion_cannon_shot && ability.expiration_frames != 0)) {
                return invalid("only ION_CANNON_SHOT is a team ability, without a time limit" + type);
            }
            if (ability.expiration_frames > max_ability_frames || ability.recharge_frames > max_ability_frames) {
                return invalid("expiration and recharge frames are at most " + std::to_string(max_ability_frames) + type);
            }
            const auto& m = ability.modifiers;
            if (!bounded(m.weapon_delay, true) || !bounded(m.shield_regen, false) || !bounded(m.shield_regen_interval, true)
                || !bounded(m.energy_regen, false) || !bounded(m.energy_regen_interval, true) || !bounded(m.speed, true)) {
                return invalid("a multiplier is out of bounds" + type);
            }
        }
    }
    return core::Result<void>::success();
}

AbilityState initial_abilities(const UnitAbilityProfile& profile) {
    AbilityState state;
    state.slots.resize(profile.abilities.size());
    return state;
}

std::optional<std::size_t> ability_slot(const UnitAbilityProfile& profile, const AbilityKind kind) noexcept {
    for (std::size_t index = 0; index < profile.abilities.size(); ++index) {
        if (profile.abilities[index].kind == kind) return index;
    }
    return std::nullopt;
}

bool ability_ready(const AbilityProfile& profile, const AbilitySlot& slot, const AbilityGate& gate,
    const std::uint64_t tick) noexcept {
    // AB-13: a recharging ability is not ready; an active one is (switching it on again does nothing).
    if (!slot.active && tick < slot.ready_tick) return false;
    // AB-14: DEFEND needs a shield that is online and outside its depletion effect.
    if (profile.kind == AbilityKind::defend) {
        return gate.shielded && gate.shields_online && !gate.shield_depleted && !gate.ion_stunned;
    }
    // AB-16: lost engines keep TURBO and SPOILER_LOCK off.
    if (profile.kind == AbilityKind::turbo || profile.kind == AbilityKind::spoiler_lock) {
        return gate.engines_online;
    }
    return true;
}

AbilitySwitch activate_ability(const AbilityProfile& profile, AbilitySlot& slot, const AbilityGate& gate,
    const std::uint64_t tick) noexcept {
    if (slot.active || !ability_ready(profile, slot, gate, tick)) return {};
    slot.active = true;
    slot.started_tick = tick;
    slot.expires_tick = profile.expiration_frames != 0 ? tick + profile.expiration_frames : 0;
    return {true, profile.modifiers.speed.raw() != one_raw};
}

AbilitySwitch deactivate_ability(const AbilityProfile& profile, AbilitySlot& slot, const std::uint64_t tick) noexcept {
    if (!slot.active) return {};
    slot.active = false;
    // AB-12: an ability without a running time limit ends without a recharge; one ended early
    // recharges for the share of its duration that ran, rounded half up (none in its first tick).
    if (slot.expires_tick != 0 && profile.expiration_frames != 0) {
        const auto elapsed = std::min<std::uint64_t>(tick - slot.started_tick, profile.expiration_frames);
        const auto frames = (2 * elapsed * profile.recharge_frames + profile.expiration_frames)
            / (2 * static_cast<std::uint64_t>(profile.expiration_frames));
        slot.ready_tick = tick + frames;
    }
    slot.expires_tick = 0;
    return {true, profile.modifiers.speed.raw() != one_raw};
}

AbilitySwitch expire_abilities(const UnitAbilityProfile& profile, AbilityState& state, const std::uint64_t tick) noexcept {
    AbilitySwitch result;
    for (std::size_t index = 0; index < state.slots.size() && index < profile.abilities.size(); ++index) {
        auto& slot = state.slots[index];
        if (!slot.active || slot.expires_tick == 0 || tick < slot.expires_tick) continue;
        const auto& ability = profile.abilities[index];
        slot.active = false;
        slot.expires_tick = 0;
        slot.ready_tick = tick + ability.recharge_frames;
        result.changed = true;
        result.speed = result.speed || ability.modifiers.speed.raw() != one_raw;
    }
    return result;
}

Fixed ability_multiplier(const UnitAbilityProfile& profile, const AbilityState& state, const AbilityModifier modifier) noexcept {
    auto value = Fixed::from_raw(one_raw);
    for (std::size_t index = 0; index < state.slots.size() && index < profile.abilities.size(); ++index) {
        if (!state.slots[index].active) continue;
        const auto factor = modifier_of(profile.abilities[index].modifiers, modifier);
        value = value.raw() == one_raw ? factor : product(value, factor);
    }
    return value;
}

std::uint32_t scaled_weapon_delay(const std::uint32_t frames, const Fixed multiplier, const bool full) noexcept {
    if (multiplier.raw() == one_raw || (full && multiplier.raw() >= one_raw)) return frames;
    return scaled_frames(frames, multiplier);
}

std::uint32_t scaled_interval(const std::uint32_t frames, const Fixed multiplier) noexcept {
    if (multiplier.raw() == one_raw) return frames;
    return std::max<std::uint32_t>(1, scaled_frames(frames, multiplier));
}

bool rate_window_closes(const std::uint64_t tick) noexcept {
    return tick % rate_window_frames == 0;
}

void close_rate_window(AbilityState& state) noexcept {
    // AB-42: damage over a 30-frame window, per second: the window's sum.
    state.damage_rate = state.window_damage;
    state.window_damage = Fixed{};
}

void append_abilities(std::vector<std::uint8_t>& bytes, const AbilityState& state) {
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(state.slots.size()));
    for (const auto& slot : state.slots) {
        // Bit 2 (#561): the slot holds a target, whose ID and hardpoint follow the ticks.
        const bool aimed = slot.target != invalid_entity_id;
        sim::detail::append_u32(bytes, (slot.active ? 1U : 0U) | (slot.autofire ? 2U : 0U) | (aimed ? 4U : 0U));
        sim::detail::append_u64(bytes, slot.started_tick);
        sim::detail::append_u64(bytes, slot.expires_tick);
        sim::detail::append_u64(bytes, slot.ready_tick);
        if (aimed) {
            sim::detail::append_u64(bytes, slot.target);
            sim::detail::append_u32(bytes, slot.target_hardpoint);
        }
    }
    sim::detail::append_i64(bytes, state.window_damage.raw());
    sim::detail::append_i64(bytes, state.damage_rate.raw());
    sim::detail::append_u32(bytes, state.replan_due ? 1U : 0U);
}

} // namespace eawr::sim::tactical
