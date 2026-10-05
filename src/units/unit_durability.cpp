#include "eawr/assets/map.hpp"
#include "eawr/units/unit_tables.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

// The #72 durability table: the unit tables' hull, hardpoint and repair values in the
// shape the tactical session binds (docs/behaviour/space-hardpoints.md, HD-01 to HD-05).
namespace eawr::units {
namespace {

namespace tactical = sim::tactical;

[[nodiscard]] core::Diagnostic failure(std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(diagnostic_codes::durability);
    diagnostic.message = std::move(message);
    return diagnostic;
}

[[nodiscard]] tactical::HardpointRole role(const HardpointType type) noexcept {
    switch (type) {
    case HardpointType::weapon_laser:
    case HardpointType::weapon_missile:
    case HardpointType::weapon_torpedo:
    case HardpointType::weapon_ion_cannon:
    case HardpointType::weapon_mass_driver:
    case HardpointType::weapon_special:
        return tactical::HardpointRole::weapon;
    case HardpointType::engine:
        return tactical::HardpointRole::engine;
    case HardpointType::shield_generator:
        return tactical::HardpointRole::shield_generator;
    case HardpointType::fighter_bay:
        return tactical::HardpointRole::fighter_bay;
    case HardpointType::enable_special_ability:
        return tactical::HardpointRole::special_ability;
    case HardpointType::unknown:
    case HardpointType::tractor_beam:
    case HardpointType::gravity_well:
    case HardpointType::dummy_art:
        return tactical::HardpointRole::other;
    }
    return tactical::HardpointRole::other;
}

[[nodiscard]] std::string lower(const std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const auto character : text) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    // Tags are trimmed by the scan; a list value may still carry spaces.
    const auto first = result.find_first_not_of(" \t\r\n");
    const auto last = result.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{} : result.substr(first, last - first + 1);
}

[[nodiscard]] std::uint32_t position(const std::vector<std::string>& names, const std::string& name) {
    const auto found = std::lower_bound(names.begin(), names.end(), name);
    return found != names.end() && *found == name ? static_cast<std::uint32_t>(found - names.begin())
                                                  : tactical::no_type_index;
}

// Decimal list values (Diminishing_Firepower) split on commas and whitespace.
[[nodiscard]] std::vector<Fixed> decimals(const std::string_view text) {
    std::vector<Fixed> values;
    std::string token;
    const auto flush = [&] {
        if (token.empty()) return;
        if (auto value = Fixed::from_decimal(token)) values.push_back(value.value());
        token.clear();
    };
    for (const auto character : text) {
        if (character == ',' || std::isspace(static_cast<unsigned char>(character))) {
            flush();
        } else {
            token.push_back(character);
        }
    }
    flush();
    return values;
}

} // namespace

std::uint32_t DamageTypeIndex::damage(const std::string_view name) const {
    return position(damage_types, name.empty() ? std::string("damage_default") : lower(name));
}

std::uint32_t DamageTypeIndex::armor(const std::string_view name) const {
    return position(armor_types, lower(name));
}

DamageTypeIndex damage_type_index(const UnitTables& tables) {
    DamageTypeIndex index;
    const auto add = [](std::vector<std::string>& names, const std::string& name) {
        if (!name.empty()) names.push_back(lower(name));
    };
    for (const auto& row : tables.constants.damage_to_armor) {
        add(index.damage_types, row.damage_type);
        add(index.armor_types, row.armor_type);
    }
    index.damage_types.emplace_back("damage_default");
    for (const auto& unit : tables.units) {
        add(index.armor_types, unit.armor_type);
        add(index.armor_types, unit.shield_armor_type);
        for (const auto& hardpoint : unit.hardpoints) {
            if (hardpoint.weapon) add(index.damage_types, hardpoint.weapon->damage_type);
        }
    }
    for (const auto& projectile : tables.projectiles) add(index.damage_types, projectile.damage_type);
    for (auto* names : {&index.damage_types, &index.armor_types}) {
        std::sort(names->begin(), names->end());
        names->erase(std::unique(names->begin(), names->end()), names->end());
    }
    return index;
}

core::Result<tactical::DurabilityTable> durability_table(const UnitTables& tables) {
    using Result = core::Result<tactical::DurabilityTable>;
    const auto scalar = [&tables](const std::string_view tag) -> std::optional<Fixed> {
        for (const auto& constant : tables.constants.scalars) {
            if (constant.tag == tag) return constant.value;
        }
        return std::nullopt;
    };
    const auto multiplier = scalar("Object_Max_Health_Multiplier_Space");
    const auto constraint = scalar("Hull_Vs_Hard_Points_Health_Constraint");
    const auto engines = scalar("Engines_Disabled_Speed_Modifier");
    const auto damaged = scalar("Health_Low_Percent_Threshold");
    if (!multiplier || !constraint || !engines || !damaged) {
        return Result::failure(failure("durability needs Object_Max_Health_Multiplier_Space, "
                                       "Hull_Vs_Hard_Points_Health_Constraint, Engines_Disabled_Speed_Modifier "
                                       "and Health_Low_Percent_Threshold"));
    }
    const auto scaled = [&](const Fixed value, const std::string& what) -> core::Result<Fixed> {
        auto product = sim::math::multiply(value, *multiplier);
        if (!product) return core::Result<Fixed>::failure(failure(what + ": " + product.error().message));
        return product;
    };

    tactical::DurabilityTable table;
    table.rules = {*constraint, *engines, *damaged};

    // Damage rules (#74, docs/behaviour/space-damage.md).
    const auto text = [&tables](const std::string_view tag) -> std::string {
        for (const auto& constant : tables.constants.scalars) {
            if (constant.tag == tag) return constant.text;
        }
        return {};
    };
    const auto recharge = scalar("ShieldRechargeIntervalInSecs");
    const auto disable = scalar("Depleted_Shield_Disable_Time");
    const auto increment = scalar("Depleted_Shield_Damage_Increment");
    const auto cap = scalar("Depleted_Shield_Regen_Cap");
    const auto energy_interval = scalar("EnergyRechargeIntervalInSecs");
    const auto exchange = scalar("EnergyToShieldExchangeRate");
    if (!recharge || !disable || !increment || !cap || !energy_interval || !exchange) {
        return Result::failure(failure("damage needs ShieldRechargeIntervalInSecs, the Depleted_Shield_* constants, "
                                       "EnergyRechargeIntervalInSecs and EnergyToShieldExchangeRate"));
    }
    const auto types = damage_type_index(tables);
    tactical::DamageRules damage;
    damage.asteroid_damage = scalar("Asteroid_Field_Damage").value_or(Fixed{});
    damage.asteroid_rate = scalar("Asteroid_Field_Damage_Rate").value_or(Fixed{});
    damage.ion_storm_disable_seconds = scalar("Ion_Storm_Shield_Disable_Time").value_or(Fixed{});
    damage.asteroid_damage_type = types.damage("");
    // DG-13, EN-02: trunc(interval x fps + 0.5) frames, the retail service interval.
    const auto service_frames = [](const Fixed seconds, const std::string& tag) -> core::Result<std::uint32_t> {
        auto frames = sim::math::multiply(seconds, Fixed::from_raw(tactical::logical_frames_per_second * Fixed::scale));
        if (!frames) return core::Result<std::uint32_t>::failure(failure(tag + ": " + frames.error().message));
        const auto rounded = (frames.value().raw() + Fixed::scale / 2) / Fixed::scale;
        return core::Result<std::uint32_t>::success(static_cast<std::uint32_t>(std::max<std::int64_t>(1, rounded)));
    };
    auto shield_frames = service_frames(*recharge, "ShieldRechargeIntervalInSecs");
    if (!shield_frames) return Result::failure(shield_frames.error());
    damage.shield_recharge_frames = shield_frames.value();
    auto energy_frames = service_frames(*energy_interval, "EnergyRechargeIntervalInSecs");
    if (!energy_frames) return Result::failure(energy_frames.error());
    damage.energy_recharge_frames = energy_frames.value();
    damage.energy_to_shield = *exchange;
    damage.depleted_disable_seconds = *disable;
    damage.depleted_increment_seconds = *increment;
    damage.depleted_regen_cap = *cap;
    const auto curve = decimals(text("Diminishing_Firepower"));
    for (std::size_t index = 0; index + 1 < curve.size(); index += 2) {
        damage.diminishing.push_back({curve[index], curve[index + 1]});
    }
    damage.damage_types = static_cast<std::uint32_t>(types.damage_types.size());
    damage.armor_types = static_cast<std::uint32_t>(types.armor_types.size());
    // DG-12: a pair without a row multiplies by 1, as FoC fills its table before the rows; a
    // negative entry reads as 1 too; a pair authored twice keeps the later row.
    damage.armor_mods.assign(static_cast<std::size_t>(damage.damage_types) * damage.armor_types,
        Fixed::from_raw(Fixed::scale));
    for (const auto& row : tables.constants.damage_to_armor) {
        const auto d = types.damage(row.damage_type);
        const auto a = types.armor(row.armor_type);
        if (d == tactical::no_type_index || a == tactical::no_type_index) continue;
        damage.armor_mods[static_cast<std::size_t>(d) * damage.armor_types + a] =
            row.multiplier.raw() < 0 ? Fixed::from_raw(Fixed::scale) : row.multiplier;
    }
    table.damage = std::move(damage);
    for (const auto& unit : tables.units) {
        if (!unit.hull) continue; // squadrons, and hulls the scan already reports as unresolved
        tactical::DurabilityProfile profile;
        profile.type_id = assets::object_type_crc(unit.id);
        auto hull = scaled(*unit.hull, unit.id + " hull");
        if (!hull) return Result::failure(hull.error());
        profile.max_hull = hull.value();
        profile.max_speed = unit.movement.max_speed;
        profile.destroyed_with_hardpoints = unit.destroyed_with_hardpoints;
        // DG-06: only a SHIELDED type has a shield; Shield_Points is not scaled by the health multiplier.
        if (unit.shielded && unit.shield_points && unit.shield_points->raw() > 0) {
            profile.max_shields = *unit.shield_points;
            profile.shield_refresh = unit.shield_refresh_rate.value_or(Fixed{});
        }
        // EN-01: only a POWERED type has an energy pool; it starts full.
        profile.ion_stun_effect = unit.ion_stun_effect; // IS-02
        if (unit.powered) {
            profile.powered = true;
            profile.max_energy = unit.energy_capacity.value_or(Fixed{});
            profile.energy_refresh = unit.energy_refresh_rate.value_or(Fixed{});
        }
        profile.armor_type = types.armor(unit.armor_type);
        profile.shield_armor_type = types.armor(unit.shield_armor_type);
        for (const auto& hardpoint : unit.hardpoints) {
            tactical::HardpointProfile entry;
            entry.role = role(hardpoint.type);
            entry.destroyable = hardpoint.destroyable;
            if (hardpoint.destroyable) {
                if (!hardpoint.health) {
                    return Result::failure(failure(unit.id + " hardpoint " + hardpoint.id + " is destroyable without Health"));
                }
                auto health = scaled(*hardpoint.health, unit.id + " hardpoint " + hardpoint.id);
                if (!health) return Result::failure(health.error());
                entry.max_health = health.value();
            }
            entry.repair_amount_per_frame = hardpoint.repair_amount_per_frame.value_or(Fixed{});
            entry.repair_cost_per_frame = hardpoint.repair_cost_per_frame.value_or(Fixed{});
            profile.hardpoints.push_back(entry);
        }
        table.profiles.push_back(std::move(profile));
    }
    std::sort(table.profiles.begin(), table.profiles.end(),
              [](const auto& left, const auto& right) { return left.type_id < right.type_id; });
    for (std::size_t index = 1; index < table.profiles.size(); ++index) {
        if (table.profiles[index].type_id == table.profiles[index - 1].type_id) {
            return Result::failure(failure("two unit types share type ID " + std::to_string(table.profiles[index].type_id)));
        }
    }
    auto valid = tactical::validate_durability(table);
    if (!valid) return Result::failure(failure(valid.error().message));
    return Result::success(std::move(table));
}

} // namespace eawr::units
