#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/combat.hpp"

#include "../replay_internal.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <utility>
#include <cstdlib>
#include <string>

namespace eawr::sim::tactical {
namespace {

using math::Fixed;

constexpr std::int64_t one_raw = Fixed::scale;

struct KindName {
    AbilityKind kind;
    std::string_view name;
};

constexpr std::array<KindName, 16> kind_names{{
    {AbilityKind::defend, "DEFEND"},
    {AbilityKind::turbo, "TURBO"},
    {AbilityKind::power_to_weapons, "POWER_TO_WEAPONS"},
    {AbilityKind::spoiler_lock, "SPOILER_LOCK"},
    {AbilityKind::ion_cannon_shot, "ION_CANNON_SHOT"},
    {AbilityKind::invulnerability, "INVULNERABILITY"},
    {AbilityKind::concentrate_fire, "CONCENTRATE_FIRE"},
    {AbilityKind::barrage, "BARRAGE"},
    {AbilityKind::energy_weapon, "ENERGY_WEAPON"},
    {AbilityKind::tractor_beam, "TRACTOR_BEAM"},
    {AbilityKind::harmonic_bomb, "HARMONIC_BOMB"},
    {AbilityKind::weaken_enemy, "WEAKEN_ENEMY"},
    {AbilityKind::replenish_wingmen, "REPLENISH_WINGMEN"},
    {AbilityKind::hunt, "HUNT"},
    {AbilityKind::missile_shield, "MISSILE_SHIELD"},
    {AbilityKind::sensor_jamming, "SENSOR_JAMMING"},
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
    case AbilityModifier::scatter_radius: return modifiers.scatter_radius;
    case AbilityModifier::cause_damage: return modifiers.cause_damage;
    case AbilityModifier::take_damage: return modifiers.take_damage;
    case AbilityModifier::fire_rate: return modifiers.fire_rate;
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

SpecialAbilityKind special_ability_kind(const std::string_view name) noexcept {
    constexpr std::array<std::pair<std::string_view, SpecialAbilityKind>, 11> names{{
        {"Combat_Bonus_Ability", SpecialAbilityKind::combat_bonus},
        {"Reduce_Production_Price_Ability", SpecialAbilityKind::production_price},
        {"Concentrate_Fire_Attack_Ability", SpecialAbilityKind::concentrate_fire},
        {"Tractor_Beam_Attack_Ability", SpecialAbilityKind::tractor_beam},
        {"Energy_Weapon_Attack_Ability", SpecialAbilityKind::energy_weapon},
        {"Sensor_Jamming_Ability", SpecialAbilityKind::sensor_jamming},
        {"Corrupt_Systems_Ability", SpecialAbilityKind::corrupt_systems},
        {"Blast_Ability", SpecialAbilityKind::blast},
        {"Stealth_Ability", SpecialAbilityKind::stealth},
        {"Find_Weakness_Ability", SpecialAbilityKind::find_weakness},
        {"Maximum_Firepower_Attack_Ability", SpecialAbilityKind::maximum_firepower},
    }};
    for (const auto& [spelling, kind] : names) if (iequals(name, spelling)) return kind;
    return SpecialAbilityKind::none;
}
SpecialActivationStyle special_activation_style(const std::string_view name) noexcept {
    constexpr std::array<std::pair<std::string_view, SpecialActivationStyle>, 5> names{{
        {"Space_Automatic", SpecialActivationStyle::space_automatic},
        {"Ground_Automatic", SpecialActivationStyle::ground_automatic},
        {"Ground_Activated", SpecialActivationStyle::ground_activated},
        {"Galactic_Automatic", SpecialActivationStyle::galactic_automatic},
        {"User_Input", SpecialActivationStyle::user_input},
    }};
    for (const auto& [spelling, style] : names) if (iequals(name, spelling)) return style;
    return SpecialActivationStyle::unspecified;
}
std::uint32_t special_service_interval(const SpecialAbilityKind kind) noexcept {
    // WHE-09: five traced active handlers use one logical frame; the base uses zero.
    return kind == SpecialAbilityKind::energy_weapon || kind == SpecialAbilityKind::concentrate_fire
        || kind == SpecialAbilityKind::tractor_beam || kind == SpecialAbilityKind::sensor_jamming
        || kind == SpecialAbilityKind::blast ? 1U : 0U;
}
bool special_type_matches(const SpecialAbilityFilter& filter, const TypeId type, const std::uint64_t categories) noexcept {
    // WHE-14: explicit inclusion precedes both category admission and exclusions.
    if (std::binary_search(filter.applicable_types.begin(), filter.applicable_types.end(), type)) return true;
    return (filter.applicable_categories & categories) != 0 && (filter.excluded_categories & categories) == 0
        && !std::binary_search(filter.excluded_types.begin(), filter.excluded_types.end(), type);
}
SpecialAbilityState initial_special_abilities(const std::span<const SpecialAbilityProfile> profiles, const bool default_enabled) {
    SpecialAbilityState state;
    state.slots.resize(profiles.size());
    for (std::size_t slot = 0; slot < profiles.size(); ++slot)
        state.slots[slot].enabled = profiles[slot].initially_enabled.value_or(default_enabled);
    return state;
}
std::size_t activate_special_abilities(const std::span<const SpecialAbilityProfile> profiles, SpecialAbilityState& state,
    const SpecialActivationStyle style, const SpecialActivationContext& context, const bool first_only, SpecialAbilityHandler& handler) {
    if (state.service_cancelled || profiles.size() != state.slots.size() || !context.owner_exists || !context.type_exists
        || style == SpecialActivationStyle::unspecified) return 0;
    std::size_t succeeded = 0;
    for (std::size_t slot = 0; slot < profiles.size(); ++slot) {
        const auto& profile = profiles[slot];
        auto& current = state.slots[slot];
        // WHE-10: retain declared order and short-circuit the gates in retail order.
        if (profile.style != style || !handler.ready(slot, context) || !handler.appropriate_mode(slot, context.mode)
            || !current.enabled || current.cancelled || !handler.appropriate_target(slot, context)
            || !handler.available(slot, context)) continue;
        current.context = context;
        struct ClearContext {
            SpecialAbilitySlot& slot;
            ~ClearContext() { slot.context.reset(); }
        } clear{current};
        if (!handler.apply(slot, current, context)) continue;
        ++succeeded;
        current.despawn_success = profile.causes_despawn;
        if (first_only) break;
    }
    return succeeded;
}
std::size_t service_special_abilities(const std::span<const SpecialAbilityProfile> profiles, SpecialAbilityState& state,
    const SpecialActivationContext& context, const std::uint64_t frame, SpecialAbilityHandler& handler) {
    // WHE-09: no catch-up loop and no base-interval polling.
    if (state.service_cancelled || profiles.size() != state.slots.size() || !context.owner_exists || !context.type_exists
        || context.death_clone || context.map_editor
        || std::all_of(state.slots.begin(), state.slots.end(), [](const auto& slot) { return slot.cancelled; })) return 0;
    std::size_t calls = 0;
    for (std::size_t slot = 0; slot < profiles.size(); ++slot) {
        const auto interval = profiles[slot].service_interval;
        auto& current = state.slots[slot];
        if (interval == 0 || !current.enabled || current.cancelled || current.next_service_frame > frame) continue;
        current.next_service_frame = frame <= UINT64_MAX - interval ? frame + interval : UINT64_MAX;
        handler.service(slot, current, frame);
        ++calls;
    }
    return calls;
}
void delete_special_owner(const std::span<const SpecialAbilityProfile> profiles, SpecialAbilityState& state,
    const SpecialAbilityMode mode, SpecialAbilityHandler& handler) {
    state.service_cancelled = true;
    if (profiles.size() != state.slots.size()) return;
    // WHE-12: sparse tracked recipients only, never a scan of the world.
    for (std::size_t slot = 0; slot < profiles.size(); ++slot) {
        auto& current = state.slots[slot];
        if (!current.enabled || !handler.appropriate_mode(slot, mode) || current.despawn_success) continue;
        for (const auto target : current.targets)
            if (handler.target_live(target)) handler.remove_effect(slot, target);
        current.targets.clear();
        current.context.reset();
        handler.terminate(slot);
    }
}
void delete_special_target(SpecialAbilityState& state, const EntityId target) {
    for (auto& slot : state.slots) std::erase(slot.targets, target);
}
void append_special_abilities(std::vector<std::uint8_t>& bytes, const SpecialAbilityState& state) {
    sim::detail::append_u32(bytes, state.service_cancelled ? 1U : 0U);
    sim::detail::append_u32(bytes, 0);
    sim::detail::append_u64(bytes, state.slots.size());
    for (const auto& slot : state.slots) {
        sim::detail::append_u32(bytes, (slot.enabled ? 1U : 0U) | (slot.cancelled ? 2U : 0U) | (slot.despawn_success ? 4U : 0U));
        sim::detail::append_u32(bytes, slot.context ? 1U : 0U);
        sim::detail::append_u64(bytes, slot.next_service_frame);
        if (slot.context) {
            const auto& context = *slot.context;
            sim::detail::append_u64(bytes, context.owner); sim::detail::append_u64(bytes, context.target);
            sim::detail::append_u32(bytes, static_cast<std::uint32_t>(context.mode));
            sim::detail::append_u32(bytes, (context.owner_exists ? 1U : 0U) | (context.type_exists ? 2U : 0U)
                | (context.death_clone ? 4U : 0U) | (context.map_editor ? 8U : 0U));
        }
        sim::detail::append_u64(bytes, slot.targets.size());
        for (const auto target : slot.targets) sim::detail::append_u64(bytes, target);
    }
}

const UnitAbilityProfile* AbilityTable::find(const TypeId type_id) const noexcept {
    const auto found = std::lower_bound(profiles.begin(), profiles.end(), type_id,
        [](const UnitAbilityProfile& profile, const TypeId value) { return profile.type_id < value; });
    return found != profiles.end() && found->type_id == type_id ? &*found : nullptr;
}

bool AbilityTable::human(const PlayerId player) const noexcept {
    return std::binary_search(humans.begin(), humans.end(), player);
}

bool AbilityTable::autofire_default(const PlayerId player) const noexcept {
    return std::binary_search(autofire_defaults.begin(), autofire_defaults.end(), player);
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
    for (std::size_t index = 0; index < table.autofire_defaults.size(); ++index) {
        if ((index != 0 && table.autofire_defaults[index - 1] >= table.autofire_defaults[index])
            || !table.human(table.autofire_defaults[index])) {
            return invalid("autofire defaults must strictly increase and name human owners");
        }
    }
    const auto bounded = [](const Fixed value, const bool positive) {
        return value.raw() <= max_ability_multiplier * one_raw && value.raw() >= -max_ability_multiplier * one_raw
            && (!positive || value.raw() > 0);
    };
    for (const auto& profile : table.profiles) {
        const auto type = " (type " + std::to_string(profile.type_id) + ")";
        if ((profile.abilities.empty() && profile.special.empty() && !profile.force_sensitive) || profile.abilities.size() > max_abilities_per_type) {
            return invalid("a type has one or two abilities" + type);
        }
        if (profile.special.size() > 256) return invalid("too many nested handlers" + type);
        for (const auto& special : profile.special) {
            const auto sorted_unique = [](const auto& ids) {
                return std::adjacent_find(ids.begin(), ids.end(), std::greater_equal<TypeId>{}) == ids.end()
                    && (ids.empty() || ids.front() != 0);
            };
            if (special.name.empty() || special.kind == SpecialAbilityKind::none
                || static_cast<unsigned>(special.kind) > static_cast<unsigned>(SpecialAbilityKind::maximum_firepower)
                || static_cast<unsigned>(special.style) > static_cast<unsigned>(SpecialActivationStyle::user_input)
                || special.service_interval > max_ability_frames
                || !sorted_unique(special.filter.applicable_types) || !sorted_unique(special.filter.excluded_types))
                return invalid("invalid nested handler" + type);
        }
        if (profile.abilities.size() == 2 && profile.abilities[0].kind == profile.abilities[1].kind) {
            return invalid("a type's abilities are of distinct kinds" + type);
        }
        for (const auto& ability : profile.abilities) {
            if (ability.kind == AbilityKind::none || to_string(ability.kind) == "NONE") {
                return invalid("unknown ability kind" + type);
            }
            if ((ability.kind == AbilityKind::missile_shield || ability.kind == AbilityKind::sensor_jamming)
                && (ability.effective_radius.raw() < 0 || ability.effective_radius.raw() > max_combat_distance * one_raw))
                return invalid("projectile defence radius out of range" + type);
            if (ability.kind == AbilityKind::replenish_wingmen && ability.replenish_team == 0)
                return invalid("wingman replenishment requires an authored team" + type);
            if (ability.kind == AbilityKind::harmonic_bomb || ability.kind == AbilityKind::weaken_enemy) {
                if (!ability.spawned || ability.spawned->type == 0 || ability.spawned->damage.raw() < 0
                    || ability.spawned->countdown_frames > max_ability_frames || ability.spawned->reach.raw() < 0
                    || ability.spawned->reach.raw() > 65536 * one_raw || ability.spawned->z_offset.raw() != 0)
                    return invalid("invalid spawned ability profile or unsupported placement offset (U-10)" + type);
                const auto& weaken = ability.spawned->weaken;
                if (weaken.on_detonation && (weaken.radius.raw() <= 0 || weaken.radius.raw() > 65536 * one_raw
                    || weaken.duration_frames == 0 || weaken.duration_frames > max_ability_frames || weaken.categories == 0
                    || weaken.take_damage_increase.raw() < 0 || weaken.take_damage_increase.raw() > 4 * one_raw
                    || weaken.cause_damage_reduction.raw() < 0 || weaken.cause_damage_reduction.raw() > one_raw))
                    return invalid("invalid weaken radius, duration, categories or contribution" + type);
            }
            if (ability.kind == AbilityKind::energy_weapon || ability.kind == AbilityKind::tractor_beam) {
                const auto expected = ability.kind == AbilityKind::energy_weapon
                    ? SpecialAbilityKind::energy_weapon : SpecialAbilityKind::tractor_beam;
                const auto handler = std::find_if(profile.special.begin(), profile.special.end(), [&](const auto& special) {
                    return special.name == ability.gui_activated_ability_name && special.kind == expected
                        && special.style == SpecialActivationStyle::user_input;
                });
                if (handler == profile.special.end() || handler->beam_min_range.raw() < 0
                    || handler->beam_max_range.raw() < 0 || handler->beam_max_range.raw() > 65536 * one_raw
                    || handler->beam_min_range.raw() > 65536 * one_raw
                    || handler->damage_per_frame.raw() < 0 || handler->damage_per_frame.raw() > 65536 * one_raw
                    || handler->target_speed_decrease.raw() < 0 || handler->target_speed_decrease.raw() > one_raw)
                    return invalid("invalid beam handler, range or target contribution" + type);
            }
            if (ability.kind == AbilityKind::concentrate_fire) {
                const auto handler = std::find_if(profile.special.begin(), profile.special.end(), [&](const auto& special) {
                    return special.name == ability.gui_activated_ability_name
                        && special.kind == SpecialAbilityKind::concentrate_fire
                        && special.style == SpecialActivationStyle::user_input;
                });
                if (ability.effective_radius.raw() < 0 || handler == profile.special.end()
                    || handler->target_damage_increase.raw() < 0 || handler->target_damage_increase.raw() > 4 * one_raw
                    || handler->target_speed_decrease.raw() != 0) {
                    // Stock Home One authors zero speed change; nonzero policies remain U-07.
                    return invalid("invalid concentrate-fire radius, handler or target modifiers" + type);
                }
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
                || !bounded(m.energy_regen, false) || !bounded(m.energy_regen_interval, true) || !bounded(m.speed, true)
                || !bounded(m.scatter_radius, true) || !bounded(m.cause_damage, false) || !bounded(m.take_damage, false)
                || !bounded(m.fire_rate, false) || m.cause_damage.raw() < 0 || m.take_damage.raw() < 0) {
                return invalid("a multiplier is out of bounds" + type);
            }
            if (ability.kind == AbilityKind::barrage
                && (ability.barrage_target_type == 0 || ability.expiration_frames == 0
                    || (ability.fixed_inaccuracy && (ability.fixed_inaccuracy->raw() < 0
                        || ability.fixed_inaccuracy->raw() > (std::int64_t{1} << 18) * one_raw))
                    || std::abs(ability.target_z_offset.raw()) > (std::int64_t{1} << 18) * one_raw)) {
                return invalid("invalid barrage target or accuracy" + type);
            }
        }
    }
    return core::Result<void>::success();
}

AbilityState initial_abilities(const UnitAbilityProfile& profile, const bool autofire_default) {
    AbilityState state;
    state.slots.resize(profile.abilities.size());
    // AB-45: the creation preference arms supported abilities without activating them.
    for (std::size_t index = 0; index < profile.abilities.size(); ++index) {
        state.slots[index].autofire = autofire_default && profile.abilities[index].supports_autofire;
    }
    state.special = initial_special_abilities(profile.special);
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
    if (gate.in_nebula) return false; // WHZ-25
    // AB-13: a recharging ability is not ready; an active one is (switching it on again does nothing).
    if (!slot.active && tick < slot.ready_tick) return false;
    // AB-14: DEFEND needs a shield that is online and outside its depletion effect.
    if (profile.kind == AbilityKind::defend) {
        return gate.shielded && gate.shields_online && !gate.shield_depleted && !gate.ion_stunned && !gate.in_ion_storm;
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

core::Result<math::Vec3> hunt_destination(const math::Vec3 position,
    const std::optional<std::array<Fixed, 4>>& bounds, const Fixed reveal_range,
    const bool force_sensitive, const std::span<const HuntEnemy> enemies,
    const std::function<bool(math::Vec3)>& fogged, CombatRandom& random) {
    bool valid = true;
    const auto add = [&](Fixed a, Fixed b) { Fixed value; valid = math::try_add(a, b, value) && valid; return value; };
    const auto sub = [&](Fixed a, Fixed b) { Fixed value; valid = math::try_subtract(a, b, value) && valid; return value; };
    const auto mul = [&](Fixed a, Fixed b) { Fixed value; valid = math::try_multiply(a, b, value) && valid; return value; };
    const auto div = [&](Fixed a, Fixed b) { Fixed value; valid = math::try_divide(a, b, value) && valid; return value; };
    const auto units = [](std::int64_t n) { return Fixed::from_raw(n * Fixed::scale); };
    const auto draw = [&](Fixed low, Fixed high) {
        const auto fraction = Fixed::from_raw(random.uniform(0, static_cast<std::uint32_t>(Fixed::scale - 1)));
        return add(low, mul(sub(high, low), fraction));
    };
    auto destination = position;
    bool selected = false;
    if (bounds) {
        const auto& area = *bounds;
        // WAB-54: the branch draw precedes both point draws, even for a force-sensitive hunter.
        const auto chance = draw(Fixed{}, units(1));
        if (!force_sensitive && chance.raw() > Fixed::scale / 2) {
            std::vector<math::Vec3> points;
            const auto dx = div(sub(area[2], area[0]), units(100));
            const auto dy = div(sub(area[3], area[1]), units(100));
            for (std::int64_t cell = 0; cell < 99; ++cell) {
                const auto x = add(area[0], mul(dx, units(cell)));
                const auto y = add(area[1], mul(dy, units(cell)));
                math::Vec3 point{draw(x, add(x, dx)), draw(y, add(y, dy)), position.z};
                if (fogged(point)) points.push_back(point);
            }
            if (!points.empty()) {
                destination = points[random.uniform(0, static_cast<std::uint32_t>(points.size() - 1))];
                selected = true;
            }
        }
        if (!selected && !enemies.empty()) {
            std::vector<std::size_t> hidden, sensitive;
            const auto count = std::min<std::size_t>(128, enemies.size());
            for (std::size_t i = 0; i < count; ++i) {
                if (enemies[i].fogged) hidden.push_back(i);
                if (enemies[i].force_sensitive) sensitive.push_back(i);
            }
            if (force_sensitive && !sensitive.empty()) {
                destination = enemies[sensitive[random.uniform(0, static_cast<std::uint32_t>(sensitive.size() - 1))]].position;
            } else if (!hidden.empty()) {
                destination = enemies[hidden[random.uniform(0, static_cast<std::uint32_t>(hidden.size() - 1))]].position;
            } else {
                destination = enemies[random.uniform(0, static_cast<std::uint32_t>(count - 1))].position;
            }
            selected = true;
        }
        if (!selected) {
            destination = {draw(area[0], area[2]), draw(area[1], area[3]), Fixed{}};
            const math::Vec3 direction{sub(destination.x, position.x), sub(destination.y, position.y), sub(destination.z, position.z)};
            const auto length = math::length(direction);
            if (!length) return core::Result<math::Vec3>::failure(length.error());
            if (length.value() < units(500) || length.value() > units(1000)) {
                const auto distance = draw(units(500), units(1000));
                if (length.value().raw() != 0) {
                    destination = {add(position.x, mul(div(direction.x, length.value()), distance)),
                        add(position.y, mul(div(direction.y, length.value()), distance)),
                        add(position.z, mul(div(direction.z, length.value()), distance))};
                } else destination = position;
                // Only the adjusted fallback is clamped, before the positive offsets (WAB-54).
                const auto half = Fixed::from_raw(reveal_range.raw() / 2);
                const auto clamp = [&](Fixed value, Fixed low, Fixed high) {
                    low = add(low, half); high = sub(high, half);
                    return low <= high ? std::clamp(value, low, high) : low;
                };
                destination.x = clamp(destination.x, area[0], area[2]);
                destination.y = clamp(destination.y, area[1], area[3]);
            }
        }
    }
    if (!force_sensitive) {
        destination.x = add(destination.x, draw(units(30), units(400)));
        destination.y = add(destination.y, draw(units(30), units(400)));
        destination.z = position.z;
    }
    if (!valid) return core::Result<math::Vec3>::failure(detail::diagnostic(diagnostic_codes::worker_failure, "hunt destination exceeds fixed-point range"));
    return core::Result<math::Vec3>::success(destination);
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
