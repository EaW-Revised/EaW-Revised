#include "eawr/assets/map.hpp"
#include "eawr/core/text.hpp"
#include "eawr/units/domain_tags.hpp"
#include "eawr/units/unit_tables.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

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
    return core::iequals(left, right);
}

// AB-04: seconds to whole frames, truncated.
[[nodiscard]] std::uint32_t frames(const std::optional<Fixed>& seconds) {
    if (!seconds || seconds->raw() <= 0) return 0;
    const auto product = static_cast<std::uint64_t>(seconds->raw()) * tactical::logical_frames_per_second;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(product / Fixed::scale, tactical::max_ability_frames));
}

// WAB-70..73: the nested Buzz_Droids_Ability data and the droid type it creates. Speed is the
// droid's Max_Speed under the space multiplier (MV-01, the same speed source as every ship).
[[nodiscard]] core::Result<tactical::BuzzDroidsProfile> buzz_profile(const UnitTables& tables,
    const std::string& owner, const BuzzDroids& data, const std::uint32_t damage_type) {
    using Result = core::Result<tactical::BuzzDroidsProfile>;
    const auto droid = std::find_if(tables.units.begin(), tables.units.end(),
        [&](const UnitType& type) { return iequals(type.id, data.object_type); });
    if (droid == tables.units.end()) return Result::failure(failure(owner + " buzz droids need their object type (WAB-71)"));
    std::optional<Fixed> multiplier;
    for (const auto& constant : tables.constants.scalars)
        if (constant.tag == domain_tag(tables.domain, DomainTag::speed_multiplier)) multiplier = constant.value;
    if (!multiplier) return Result::failure(failure(owner + " buzz droids need Object_Max_Speed_Multiplier_Space (MV-01)"));
    const auto per_frame = [](const std::optional<Fixed>& per_second) {
        const auto raw = per_second.value_or(Fixed{}).raw();
        const auto fps = static_cast<std::int64_t>(tactical::logical_frames_per_second);
        return Fixed::from_raw(raw >= 0 ? (raw + fps / 2) / fps : -((-raw + fps / 2) / fps));
    };
    tactical::BuzzDroidsProfile profile;
    profile.droid_type = assets::object_type_crc(droid->id);
    profile.min_range = data.min_range.value_or(Fixed{});
    profile.max_range = data.max_range.value_or(Fixed{});
    profile.chase_radius = data.chase_radius.value_or(Fixed{});
    profile.damage_radius = data.damage_radius.value_or(Fixed{});
    profile.enemy_damage = per_frame(data.enemy_damage_per_second);
    profile.own_damage = per_frame(data.own_damage_per_second);
    profile.activation_frames = frames(data.activation_seconds);
    auto speed = sim::math::multiply(droid->movement.max_speed.value_or(Fixed{}), *multiplier);
    if (!speed) return Result::failure(failure(owner + " buzz droid speed: " + speed.error().message));
    profile.speed = speed.value();
    profile.victim_categories = droid->special_weapon_targets;
    profile.damage_type = damage_type;
    return Result::success(profile);
}

// #4992: FoC gives each object one slot per ability kind and fills it from the type's
// Unit_Abilities_Data in authored order, so a later entry of a kind replaces an earlier
// one (debug build, object initialization). Keep the last entry of each kind, in place,
// and name the dropped kinds once per type (AaW authors ION_CANNON_SHOT twice on 33 types).
void keep_last_of_each_kind(std::vector<tactical::AbilityProfile>& abilities, const std::string& owner,
    std::vector<core::Diagnostic>* const notes) {
    std::string dropped;
    for (std::size_t index = 0; index < abilities.size();) {
        const auto kind = abilities[index].kind;
        const bool later = std::any_of(abilities.begin() + static_cast<std::ptrdiff_t>(index) + 1, abilities.end(),
            [&](const auto& other) { return other.kind == kind; });
        if (!later) { ++index; continue; }
        if (dropped.find(tactical::to_string(kind)) == std::string::npos)
            dropped += (dropped.empty() ? "" : ", ") + std::string(tactical::to_string(kind));
        abilities.erase(abilities.begin() + static_cast<std::ptrdiff_t>(index));
    }
    if (dropped.empty() || notes == nullptr) return;
    auto note = failure("unit type '" + owner + "' authors Unit_Ability " + dropped
        + " more than once; the last entry is used, as FoC does (#4992)");
    note.severity = core::Severity::warning;
    notes->push_back(std::move(note));
}

} // namespace

bool runs_defend_script(const UnitType& type) noexcept {
    return iequals(type.lua_script, "ObjectScript_PowerToShields");
}

bool runs_self_destruct_script(const UnitType& type) noexcept {
    return iequals(type.lua_script, "ObjectScript_Krayt");
}

core::Result<tactical::AbilityTable> ability_table(const UnitTables& tables, std::span<const tactical::PlayerId> humans,
    bool (*allowed)(std::string_view, std::string_view), const bool default_autofire, const ContentErrors policy,
    std::vector<core::Diagnostic>* const skipped) {
    using Result = core::Result<tactical::AbilityTable>;
    tactical::AbilityTable table;
    const auto damage_types = damage_type_index(tables);
    table.beam_damage_type = damage_types.damage({});
    table.passive_damage_type = damage_types.damage("Damage_Force_Corruption");
    table.humans.assign(humans.begin(), humans.end());
    std::sort(table.humans.begin(), table.humans.end());
    table.humans.erase(std::unique(table.humans.begin(), table.humans.end()), table.humans.end());
    if (default_autofire) table.autofire_defaults = table.humans;
    std::uint32_t detection_frames = 30; // WST-09: default 1.0 s, trunc(seconds x 30).
    for (const auto& constant : tables.constants.scalars) {
        if (constant.tag != "Stealth_Detection_Time") continue;
        if (!constant.value || constant.value->raw() < 0 || constant.value->raw() > 3600 * Fixed::scale)
            return Result::failure(failure("Stealth_Detection_Time must be in [0, 3600] seconds (WST-09)"));
        detection_frames = frames(constant.value);
    }
    // One type's rows, built whole before they are added (#4589: a type that fails adds nothing).
    const auto build_unit = [&](const UnitType& unit) -> std::optional<core::Diagnostic> {
        // AB-15: a squadron's own Unit_Abilities_Data is not the one that acts; its craft's is.
        // AB-60 (#561): a team ability acts on the squadron's team container, which is the
        // squadron company in the session, with the team type's data.
        if (unit.kind == UnitKind::squadron) {
            tactical::UnitAbilityProfile team;
            team.type_id = assets::object_type_crc(unit.id);
            team.special = unit.team_special_abilities;
            for (const auto& ability : unit.team_abilities) {
                if (allowed != nullptr && !allowed(unit.id, ability.type)) continue;
                const auto kind = tactical::ability_kind(ability.type);
                if (kind != tactical::AbilityKind::ion_cannon_shot && kind != tactical::AbilityKind::buzz_droids) continue;
                tactical::AbilityProfile entry;
                entry.kind = kind;
                entry.recharge_frames = frames(ability.recharge_seconds);
                entry.supports_autofire = ability.supports_autofire;
                entry.targeting_max_attack_distance = ability.targeting_max_attack_distance;
                entry.team = true;
                if (kind == tactical::AbilityKind::buzz_droids) {
                    // WAB-70: the team container holds it; without its nested data it is not modelled.
                    if (!unit.team_buzz_droids) continue;
                    auto buzz = buzz_profile(tables, unit.id, *unit.team_buzz_droids, table.beam_damage_type);
                    if (!buzz) return buzz.error();
                    entry.buzz = buzz.value();
                }
                team.abilities.push_back(entry);
            }
            keep_last_of_each_kind(team.abilities, unit.id, skipped);
            if (!team.abilities.empty() || !team.special.empty()) table.profiles.push_back(std::move(team));
            return std::nullopt;
        }
        tactical::UnitAbilityProfile profile;
        profile.stealth_detection_frames = detection_frames;
        profile.type_id = assets::object_type_crc(unit.id);
        profile.special = unit.special_abilities;
        profile.force_sensitive = unit.force_sensitive;
        for (std::size_t index = 0; index < unit.hardpoints.size(); ++index) {
            if (unit.hardpoints[index].type == HardpointType::gravity_well)
                profile.gravity_well_hardpoints.push_back(static_cast<std::uint32_t>(index));
        }
        for (const auto& ability : unit.abilities) {
            if (allowed != nullptr && !allowed(unit.id, ability.type)) continue;
            const auto kind = tactical::ability_kind(ability.type);
            if (kind == tactical::AbilityKind::none) continue; // AB-03: not modelled
            // WAB-70: a squadron's buzz droids belong to its team container (AB-60), not each craft.
            if (kind == tactical::AbilityKind::buzz_droids && (unit.kind == UnitKind::craft || !unit.buzz_droids)) continue;
            tactical::AbilityProfile entry;
            entry.kind = kind;
            // WHD-01: a surviving point of the type, not a named beam bone.
            const auto required = kind == tactical::AbilityKind::tractor_beam ? HardpointType::tractor_beam
                : HardpointType::weapon_special;
            entry.requires_hardpoint = kind == tactical::AbilityKind::tractor_beam
                || kind == tactical::AbilityKind::blast || kind == tactical::AbilityKind::leech_shields;
            if (entry.requires_hardpoint) {
                for (std::size_t index = 0; index < unit.hardpoints.size(); ++index)
                    if (unit.hardpoints[index].type == required)
                        entry.required_hardpoints.push_back(static_cast<std::uint32_t>(index));
            }
            if (kind == tactical::AbilityKind::hunt) profile.hunt_reveal_range = unit.fow_reveal_range.value_or(Fixed{});
            entry.expiration_frames = frames(ability.expiration_seconds);
            if (kind == tactical::AbilityKind::stealth && entry.expiration_frames != 0) {
                entry.expiration_seconds = *ability.expiration_seconds; // WST-03: the cloak's own count
            }
            entry.recharge_frames = frames(ability.recharge_seconds);
            entry.supports_autofire = ability.supports_autofire;
            entry.targeting_max_attack_distance = ability.targeting_max_attack_distance;
            entry.disable_movement = ability.disable_movement;
            if (kind == tactical::AbilityKind::barrage) {
                entry.barrage_target_type = assets::object_type_crc("Dummy_Barrage_Target");
                entry.fixed_inaccuracy = ability.fixed_inaccuracy;
                entry.target_z_offset = ability.target_z_offset.value_or(Fixed{});
            }
            if (kind == tactical::AbilityKind::buzz_droids) {
                auto buzz = buzz_profile(tables, unit.id, *unit.buzz_droids, table.beam_damage_type);
                if (!buzz) return buzz.error();
                entry.buzz = buzz.value();
            }
            if (kind == tactical::AbilityKind::lure) {
                // WLU-01: the craft authors the pulse; without listed types it lures nothing and
                // stays out of the table (AB-26). The squadron's own entry names no data (AB-15).
                if (unit.kind == UnitKind::squadron || ability.target_types.empty() || !ability.max_pulses) continue;
                tactical::LureProfile lure;
                lure.radius = ability.effective_radius.value_or(Fixed{});
                lure.pulse_frames = frames(ability.pulse_frequency_seconds);
                lure.max_pulses = std::min<std::uint32_t>(*ability.max_pulses, tactical::max_ability_frames);
                for (const auto& name : ability.target_types) lure.target_types.push_back(assets::object_type_crc(name));
                std::sort(lure.target_types.begin(), lure.target_types.end());
                lure.target_types.erase(std::unique(lure.target_types.begin(), lure.target_types.end()), lure.target_types.end());
                lure.particle = ability.lure_particle;
                entry.lure = std::move(lure);
            }
            if (kind == tactical::AbilityKind::replenish_wingmen) {
                if (unit.replenish_team.empty()) return failure(unit.id + " replenishment requires a team (WHE-63)");
                entry.replenish_team = assets::object_type_crc(unit.replenish_team);
                entry.replenish_particle = ability.replenish_particle;
            }
            if (kind == tactical::AbilityKind::harmonic_bomb || kind == tactical::AbilityKind::weaken_enemy
                || kind == tactical::AbilityKind::self_destruct || kind == tactical::AbilityKind::cluster_bomb
                || kind == tactical::AbilityKind::fow_reveal_ping) {
                auto projectile_index = ability.spawned_projectile_index;
                if (kind == tactical::AbilityKind::cluster_bomb) {
                    if (!unit.cluster_bomb || unit.cluster_bomb->name != ability.gui_activated_ability_name
                        || unit.cluster_bomb->style != tactical::SpecialActivationStyle::user_input) continue;
                    projectile_index = unit.cluster_bomb->projectile_index;
                }
                if (projectile_index >= tables.projectiles.size()) {
                    if (projectile_index != no_index)
                        return failure(unit.id + " inconsistent spawned projectile index (WHE-61)");
                    if (ability.target_position_z_offset.value_or(Fixed{}).raw() != 0)
                        return failure(unit.id + " unsupported spawned placement offset (U-10)");
                    entry.unresolved_spawn = kind == tactical::AbilityKind::cluster_bomb
                        ? unit.cluster_bomb->bomb_type : ability.spawned_object;
                    if (skipped != nullptr) {
                        auto warning = failure("unit '" + unit.id + "' ability '" + ability.type
                            + "' unresolved " + (kind == tactical::AbilityKind::cluster_bomb ? "Bomb_Type" : "Spawned_Object_Type")
                            + " '" + *entry.unresolved_spawn
                            + "': creation skipped; ability retained (WHE-61)");
                        warning.severity = core::Severity::warning;
                        skipped->push_back(std::move(warning));
                    }
                } else {
                    const auto& projectile = tables.projectiles[projectile_index];
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
                    if (kind == tactical::AbilityKind::cluster_bomb || kind == tactical::AbilityKind::fow_reveal_ping) {
                        spawned.speed = projectile.max_speed.value_or(Fixed{});
                        spawned.max_travel = projectile.max_flight_distance.value_or(Fixed{});
                        spawned.lifetime = projectile.max_lifetime;
                        spawned.reveal_range = projectile.reveal_range;
                        spawned.dense_reveal_multiplier = projectile.dense_reveal_multiplier;
                    }
                    if (kind == tactical::AbilityKind::cluster_bomb) {
                        spawned.reach = {}; // WCB-03: Effective_Radius is not the launch ring
                        spawned.ring_count = unit.cluster_bomb->count;
                        spawned.countdown_frames = frames(unit.cluster_bomb->detonation_seconds);
                    }
                    entry.spawned = std::move(spawned);
                }
            }
            if (kind == tactical::AbilityKind::concentrate_fire || kind == tactical::AbilityKind::energy_weapon
                || kind == tactical::AbilityKind::tractor_beam || kind == tactical::AbilityKind::missile_shield
                || kind == tactical::AbilityKind::sensor_jamming || kind == tactical::AbilityKind::corrupt_systems
                || kind == tactical::AbilityKind::leech_shields) {
                entry.effective_radius = ability.effective_radius.value_or(Fixed{});
                entry.gui_activated_ability_name = ability.gui_activated_ability_name;
            }
            if (kind == tactical::AbilityKind::blast) {
                entry.gui_activated_ability_name = ability.gui_activated_ability_name;
                const auto handler = std::find_if(unit.special_abilities.begin(), unit.special_abilities.end(),
                    [&](const auto& special) { return special.kind == tactical::SpecialAbilityKind::blast
                        && special.style == tactical::SpecialActivationStyle::user_input
                        && special.name == entry.gui_activated_ability_name; });
                if (handler == unit.special_abilities.end() || !handler->charged_blast) continue;
                entry.charged_blast = handler->charged_blast;
                entry.expiration_frames = 0;
                entry.recharge_at_activation = true; // WHE-35: independent from charge/shot completion
            }
            if (kind == tactical::AbilityKind::stealth) {
                // WST-02: the nested handler named by the ability does the work; without one, or
                // without a time limit (WST-05), the ability stays out of the table (AB-26).
                entry.gui_activated_ability_name = ability.gui_activated_ability_name;
                const auto handler = std::find_if(unit.special_abilities.begin(), unit.special_abilities.end(),
                    [&](const tactical::SpecialAbilityProfile& special) {
                        return special.kind == tactical::SpecialAbilityKind::stealth
                            && special.style == tactical::SpecialActivationStyle::user_input
                            && special.name == ability.gui_activated_ability_name;
                    });
                if (handler == unit.special_abilities.end() || entry.expiration_frames == 0) continue;
            }
            if (kind == tactical::AbilityKind::leech_shields) {
                const auto handler = std::find_if(unit.special_abilities.begin(), unit.special_abilities.end(),
                    [&](const auto& special) { return special.kind == tactical::SpecialAbilityKind::leech_shields
                        && special.name == ability.gui_activated_ability_name; });
                if (handler == unit.special_abilities.end()) continue;
                entry.expiration_frames = handler->leech_duration_frames;
                entry.recharge_at_activation = true; // WLS-03
            }
            if (kind == tactical::AbilityKind::corrupt_systems) {
                const auto handler = std::find_if(unit.special_abilities.begin(), unit.special_abilities.end(),
                    [&](const auto& special) { return special.kind == tactical::SpecialAbilityKind::corrupt_systems
                        && special.name == entry.gui_activated_ability_name; });
                if (handler == unit.special_abilities.end() || handler->corrupt_duration_frames == 0) continue;
                entry.recharge_at_activation = true;
            }
            if (kind == tactical::AbilityKind::laser_defense) {
                // WLD-02, WLD-06: the nested handler's duration is the time limit and the 60 s countdown
                // starts at the switch-on. A handler the sim cannot fly (AB-26) keeps the ability out.
                entry.gui_activated_ability_name = ability.gui_activated_ability_name;
                const auto handler = std::find_if(unit.special_abilities.begin(), unit.special_abilities.end(),
                    [&](const tactical::SpecialAbilityProfile& special) {
                        return special.kind == tactical::SpecialAbilityKind::laser_defense
                            && special.name == ability.gui_activated_ability_name;
                    });
                if (handler == unit.special_abilities.end() || handler->defence_categories == 0
                    || handler->defence_duration_frames == 0) continue;
                entry.expiration_frames = handler->defence_duration_frames;
                entry.recharge_at_activation = true;
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
                else if (iequals(name, "TARGET_SPEED_MULTIPLIER")) m.target_speed = modifier.value;
                else if (iequals(name, "TARGET_TAKE_DAMAGE_MULTIPLIER")) m.target_take_damage = modifier.value;
                // AB-26: an unknown name does not discard the ability or its other modifiers.
            }
            // AB-26: duplicate flags use the last authored value as an inferred host choice;
            // retail duplicate precedence is unverified (see the ability fidelity unknowns).
            for (const auto& flag : ability.modifier_flags) {
                auto& m = entry.modifiers;
                if (iequals(flag.modifier, "WEAPONS_ENABLED_FLAG")) m.weapons_enabled = flag.value;
                else if (iequals(flag.modifier, "CAN_BE_SQUASHED")) m.can_be_squashed = flag.value;
                else if (iequals(flag.modifier, "ALLOW_TARGET_DIMINISHING_FIREPOWER")) m.allow_target_diminishing_firepower = flag.value;
            }
            profile.abilities.push_back(entry);
        }
        keep_last_of_each_kind(profile.abilities, unit.id, skipped);
        if (profile.abilities.empty() && profile.special.empty() && !profile.force_sensitive) return std::nullopt;
        // AB-41: the DEFEND stand-in of the PowerToShields object script, for a type with DEFEND.
        profile.defend_script = runs_defend_script(unit)
            && tactical::ability_slot(profile, tactical::AbilityKind::defend).has_value();
        // WSD-09: the Krayt object script's SELF_DESTRUCT triggers, for a type with SELF_DESTRUCT.
        profile.self_destruct_script = runs_self_destruct_script(unit)
            && tactical::ability_slot(profile, tactical::AbilityKind::self_destruct).has_value();
        // FS-29: attach the visible MC30 consumer only to its authored object script.
        profile.mc30_script = iequals(unit.lua_script, "ObjectScript_MC30")
            && tactical::ability_slot(profile, tactical::AbilityKind::cluster_bomb).has_value();
        // #4666: each type is checked on its own, so a tolerant load skips this type, not the table.
        if (auto valid = tactical::validate_ability_profile(profile); !valid) return valid.error();
        table.profiles.push_back(std::move(profile));
        return std::nullopt;
    };
    for (const auto& unit : tables.units) {
        auto error = build_unit(unit);
        if (!error) continue;
        if (policy == ContentErrors::strict) return Result::failure(std::move(*error));
        error->message = "skipped unit type '" + unit.id + "': " + error->message;
        if (skipped != nullptr) skipped->push_back(std::move(*error));
    }
    // WSH-09: a negative automatic Heal_Amount delivers Damage_Force_Corruption; content without that damage-type name
    // still loads (no armor row applies) and says so once.
    if (skipped != nullptr && table.passive_damage_type == tactical::no_type_index
        && std::any_of(table.profiles.begin(), table.profiles.end(), [](const auto& profile) {
            return std::any_of(profile.special.begin(), profile.special.end(), [](const auto& handler) {
                return handler.kind == tactical::SpecialAbilityKind::force_healing && handler.heal_amount.raw() < 0;
            });
        })) {
        auto warning = failure("negative automatic Heal_Amount: damage type 'Damage_Force_Corruption' is not in the "
            "damage-type list; no armor row applies (WSH-09)");
        warning.severity = core::Severity::warning;
        skipped->push_back(std::move(warning));
    }
    // WSD-10: the AI combat rating of a type is its AI_Combat_Power; a squadron's is the sum over its craft (PL-13).
    for (const auto& unit : tables.units) {
        std::optional<Fixed> rating = unit.ai_combat_power;
        if (!unit.members.empty()) {
            Fixed sum{};
            for (const auto& member : unit.members) {
                if (member.craft_index >= tables.units.size() || !tables.units[member.craft_index].ai_combat_power) continue;
                const auto added = sim::math::add(sum, *tables.units[member.craft_index].ai_combat_power);
                if (!added) return Result::failure(failure(unit.id + " squadron combat rating: " + added.error().message));
                sum = added.value();
            }
            rating = sum;
        }
        if (rating && rating->raw() > 0) table.combat_ratings.emplace_back(assets::object_type_crc(unit.id), *rating);
    }
    std::sort(table.combat_ratings.begin(), table.combat_ratings.end(),
        [](const auto& left, const auto& right) { return left.first < right.first; });
    table.combat_ratings.erase(std::unique(table.combat_ratings.begin(), table.combat_ratings.end(),
        [](const auto& left, const auto& right) { return left.first == right.first; }), table.combat_ratings.end());
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
