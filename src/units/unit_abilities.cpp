#include "eawr/assets/map.hpp"
#include "eawr/units/unit_tables.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

// The #76 ability table: the unit tables' Unit_Abilities_Data of the modelled kinds in the shape
// the tactical session binds (docs/behaviour/space-abilities.md AB-01 to AB-04).
namespace eawr::units {
namespace {

namespace tactical = sim::tactical;

[[nodiscard]] core::Diagnostic failure(std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(diagnostic_codes::abilities);
    diagnostic.message = std::move(message);
    return diagnostic;
}

[[nodiscard]] bool iequals(const std::string_view left, const std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::toupper(static_cast<unsigned char>(left[index])) != std::toupper(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

// AB-04: seconds to whole frames, truncated.
[[nodiscard]] std::uint32_t frames(const std::optional<Fixed>& seconds) {
    if (!seconds || seconds->raw() <= 0) return 0;
    const auto product = static_cast<std::uint64_t>(seconds->raw()) * tactical::logical_frames_per_second;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(product / Fixed::scale, tactical::max_ability_frames));
}

} // namespace

bool runs_defend_script(const UnitType& type) noexcept {
    return iequals(type.lua_script, "ObjectScript_PowerToShields");
}

core::Result<tactical::AbilityTable> ability_table(const UnitTables& tables, std::span<const tactical::PlayerId> humans,
    bool (*allowed)(std::string_view, std::string_view), const bool default_autofire) {
    using Result = core::Result<tactical::AbilityTable>;
    tactical::AbilityTable table;
    const auto damage_types = damage_type_index(tables);
    table.beam_damage_type = damage_types.damage({});
    table.humans.assign(humans.begin(), humans.end());
    std::sort(table.humans.begin(), table.humans.end());
    table.humans.erase(std::unique(table.humans.begin(), table.humans.end()), table.humans.end());
    if (default_autofire) table.autofire_defaults = table.humans;
    for (const auto& unit : tables.units) {
        // AB-15: a squadron's own Unit_Abilities_Data is not the one that acts; its craft's is.
        // AB-60 (#561): a team ability acts on the squadron's team container, which is the
        // squadron company in the session, with the team type's data.
        if (unit.kind == UnitKind::squadron) {
            tactical::UnitAbilityProfile team;
            team.type_id = assets::object_type_crc(unit.id);
            team.special = unit.team_special_abilities;
            for (const auto& ability : unit.team_abilities) {
                if (allowed != nullptr && !allowed(unit.id, ability.type)) continue;
                if (tactical::ability_kind(ability.type) != tactical::AbilityKind::ion_cannon_shot) continue;
                tactical::AbilityProfile entry;
                entry.kind = tactical::AbilityKind::ion_cannon_shot;
                entry.recharge_frames = frames(ability.recharge_seconds);
                entry.supports_autofire = ability.supports_autofire;
                entry.team = true;
                team.abilities.push_back(entry);
            }
            if (!team.abilities.empty() || !team.special.empty()) table.profiles.push_back(std::move(team));
            continue;
        }
        tactical::UnitAbilityProfile profile;
        profile.type_id = assets::object_type_crc(unit.id);
        profile.special = unit.special_abilities;
        profile.force_sensitive = unit.force_sensitive;
        for (const auto& ability : unit.abilities) {
            if (allowed != nullptr && !allowed(unit.id, ability.type)) continue;
            const auto kind = tactical::ability_kind(ability.type);
            if (kind == tactical::AbilityKind::none) continue; // AB-03: not modelled
            tactical::AbilityProfile entry;
            entry.kind = kind;
            if (kind == tactical::AbilityKind::hunt) profile.hunt_reveal_range = unit.space_fow_reveal_range.value_or(Fixed{});
            entry.expiration_frames = frames(ability.expiration_seconds);
            entry.recharge_frames = frames(ability.recharge_seconds);
            entry.supports_autofire = ability.supports_autofire;
            if (kind == tactical::AbilityKind::barrage) {
                entry.barrage_target_type = assets::object_type_crc("Dummy_Barrage_Target");
                entry.fixed_inaccuracy = ability.fixed_inaccuracy;
                entry.target_z_offset = ability.target_z_offset.value_or(Fixed{});
            }
            if (kind == tactical::AbilityKind::replenish_wingmen) {
                if (unit.replenish_team.empty()) return Result::failure(failure(unit.id + " replenishment requires a team (WHE-63)"));
                entry.replenish_team = assets::object_type_crc(unit.replenish_team);
                entry.replenish_particle = ability.replenish_particle;
            }
            bool modelled = true;
            if (kind == tactical::AbilityKind::harmonic_bomb || kind == tactical::AbilityKind::weaken_enemy) {
                if (ability.spawned_projectile_index >= tables.projectiles.size())
                    return Result::failure(failure(unit.id + " spawned ability requires a projectile type (WHE-61)"));
                const auto& projectile = tables.projectiles[ability.spawned_projectile_index];
                tactical::SpawnedAbilityProfile spawned;
                spawned.type = assets::object_type_crc(projectile.id);
                spawned.damage = projectile.damage.value_or(Fixed{});
                spawned.damage_type = damage_types.damage(projectile.damage_type);
                spawned.blast = projectile.blast;
                spawned.weaken = projectile.weaken;
                spawned.shield_damage = projectile.does_shield_damage;
                spawned.hitpoint_damage = projectile.does_hitpoint_damage;
                spawned.countdown_frames = frames(ability.bomb_countdown_seconds);
                spawned.reach = ability.effective_radius.value_or(Fixed{});
                spawned.z_offset = ability.target_position_z_offset.value_or(Fixed{});
                entry.spawned = std::move(spawned);
            }
            if (kind == tactical::AbilityKind::concentrate_fire || kind == tactical::AbilityKind::energy_weapon
                || kind == tactical::AbilityKind::tractor_beam || kind == tactical::AbilityKind::missile_shield
                || kind == tactical::AbilityKind::sensor_jamming) {
                entry.effective_radius = ability.effective_radius.value_or(Fixed{});
                entry.gui_activated_ability_name = ability.gui_activated_ability_name;
            }
            for (const auto& modifier : ability.modifiers) {
                auto& m = entry.modifiers;
                const auto& name = modifier.modifier;
                if (iequals(name, "WEAPON_DELAY_MULTIPLIER")) m.weapon_delay = modifier.value;
                else if (iequals(name, "SCATTER_RADIUS_MULTIPLIER")) m.scatter_radius = modifier.value;
                else if (iequals(name, "FIRE_RATE_MULTIPLIER")) m.fire_rate = modifier.value;
                else if (iequals(name, "SHIELD_REGEN_MULTIPLIER")) m.shield_regen = modifier.value;
                else if (iequals(name, "SHIELD_REGEN_INTERVAL_MULTIPLIER")) m.shield_regen_interval = modifier.value;
                else if (iequals(name, "ENERGY_REGEN_MULTIPLIER")) m.energy_regen = modifier.value;
                else if (iequals(name, "ENERGY_REGEN_INTERVAL_MULTIPLIER")) m.energy_regen_interval = modifier.value;
                else if (iequals(name, "SPEED_MULTIPLIER")) m.speed = modifier.value;
                else if (iequals(name, "CAUSE_DAMAGE_MULTIPLIER")) m.cause_damage = modifier.value;
                else if (iequals(name, "TAKE_DAMAGE_MULTIPLIER")) m.take_damage = modifier.value;
                else { modelled = false; break; } // AB-26: expose only completely supported abilities
            }
            if (modelled) profile.abilities.push_back(entry);
        }
        if (profile.abilities.empty() && profile.special.empty() && !profile.force_sensitive) continue;
        if (profile.abilities.size() > tactical::max_abilities_per_type) {
            return Result::failure(failure(unit.id + " authors more than two modelled abilities"));
        }
        // AB-41: the DEFEND stand-in of the PowerToShields object script, for a type with DEFEND.
        profile.defend_script = runs_defend_script(unit)
            && tactical::ability_slot(profile, tactical::AbilityKind::defend).has_value();
        table.profiles.push_back(std::move(profile));
    }
    std::sort(table.profiles.begin(), table.profiles.end(),
        [](const auto& left, const auto& right) { return left.type_id < right.type_id; });
    for (std::size_t index = 1; index < table.profiles.size(); ++index) {
        if (table.profiles[index].type_id == table.profiles[index - 1].type_id) {
            return Result::failure(failure("two unit types share type ID " + std::to_string(table.profiles[index].type_id)));
        }
    }
    auto valid = tactical::validate_abilities(table);
    if (!valid) return Result::failure(failure(valid.error().message));
    return Result::success(std::move(table));
}

} // namespace eawr::units
