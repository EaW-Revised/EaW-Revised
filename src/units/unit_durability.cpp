#include "eawr/assets/map.hpp"
#include "eawr/core/ascii.hpp"
#include "eawr/core/text.hpp"
#include "eawr/units/domain_tags.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/sim/tactical/damage.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// The #72 durability table: the unit tables' hull, hardpoint and repair values in the
// shape the tactical session binds (docs/behaviour/space-hardpoints.md, HD-01 to HD-05).
namespace eawr::units {
namespace {

namespace tactical = sim::tactical;

[[nodiscard]] std::string fixed_decimal(const Fixed value) {
    const auto raw = value.raw();
    const auto magnitude =
        raw < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(raw) : static_cast<std::uint64_t>(raw);
    const auto scale = static_cast<std::uint64_t>(Fixed::scale);
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
    return core::to_lower(core::trim_view(text, core::xml_whitespace));
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
        if (character == ',' || core::ascii_space(static_cast<unsigned char>(character))) {
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

core::Result<tactical::DurabilityTable> durability_table(const UnitTables& tables,
                                                         std::vector<core::Diagnostic> *const notes,
                                                         core::LoadNoteBuffer *const load_notes) {
    using Result = core::Result<tactical::DurabilityTable>;
    const auto scalar = [&tables](const std::string_view tag) -> std::optional<Fixed> {
        for (const auto& constant : tables.constants.scalars) {
            if (constant.tag == tag) return constant.value;
        }
        return std::nullopt;
    };
    core::LoadNoteBuffer ignored_notes;
    auto& load = load_notes ? *load_notes : ignored_notes;
    const auto fallback = [&](std::string_view tag, Fixed value, const char* message) {
        const auto found = scalar(tag);
        if (found) return *found;
        core::LoadNote note;
        note.code = "EAWR-LOAD-0120";
        note.tag = std::string(tag);
        note.retail = "LSA-P5";
        note.diagnostic_code = "EAWR-UNITS-0003";
        note.message = message;
        core::LoadNoteBuffer temporary;
        const auto effective = core::default_note<std::int64_t>(std::nullopt, value.raw(), std::move(note), temporary);
        core::LoadNoteCollector collector;
        collector.merge(temporary);
        for (auto emitted : collector.snapshot()) {
            emitted.classification = core::LoadCategory::retail_accepts;
            emitted.effective_value = fixed_decimal(Fixed::from_raw(effective));
            load.append(std::move(emitted));
        }
        return Fixed::from_raw(effective);
    };
    const auto raw = [](std::int64_t numerator, std::int64_t denominator = 1) {
        return Fixed::from_raw(numerator * Fixed::scale / denominator);
    };
    const auto durability_message =
        "durability needs Object_Max_Health_Multiplier_Space, Hull_Vs_Hard_Points_Health_Constraint, "
        "Engines_Disabled_Speed_Modifier and Health_Low_Percent_Threshold";
    const auto multiplier_tag = domain_tag(tables.domain, DomainTag::health_multiplier);
    auto multiplier = fallback(multiplier_tag, tables.domain == tactical::BattleDomain::land ? raw(1) : raw(3, 2),
                               durability_message);
    const auto original_multiplier = multiplier;
    multiplier =
        Fixed::from_raw(std::clamp(multiplier.raw(), std::int64_t{1}, tactical::max_damage_multiplier * Fixed::scale));
    if (multiplier != original_multiplier) {
        core::LoadNote note;
        note.code = "EAWR-LOAD-0124";
        note.object = "durability rules";
        note.tag = std::string(multiplier_tag);
        const auto source = std::find_if(tables.constants.scalars.begin(), tables.constants.scalars.end(),
                                         [&](const auto& entry) { return entry.tag == multiplier_tag; });
        note.authored_value =
            source == tables.constants.scalars.end() ? fixed_decimal(original_multiplier) : source->text;
        note.effective_value = fixed_decimal(multiplier);
        note.action = core::LoadAction::clamped;
        note.diagnostic_code = "EAWR-UNITS-0003";
        note.message = "health multiplier is outside the supported range";
        load.append(std::move(note));
    }
    const auto constraint = fallback("Hull_Vs_Hard_Points_Health_Constraint", raw(1,5), durability_message);
    const auto engines = fallback("Engines_Disabled_Speed_Modifier", raw(2,5), durability_message);
    const auto damaged = fallback("Health_Low_Percent_Threshold", raw(33,100), durability_message);
    const auto scaled = [&](const Fixed value, const std::string& what, const std::string& tag) -> core::Result<Fixed> {
        auto product = sim::math::multiply(value, multiplier);
        if (product) return product;
        const auto saturated = (value.raw() < 0) != (multiplier.raw() < 0) ? std::numeric_limits<std::int64_t>::min()
                                                                           : std::numeric_limits<std::int64_t>::max();
        core::LoadNote note;
        note.code = "EAWR-LOAD-0121";
        note.object = what;
        note.tag = tag;
        note.action = core::LoadAction::clamped;
        note.authored_value = fixed_decimal(value);
        note.effective_value = fixed_decimal(Fixed::from_raw(saturated));
        note.diagnostic_code = "EAWR-UNITS-0003";
        note.message = what + ": " + product.error().message;
        load.append(std::move(note));
        return core::Result<Fixed>::success(Fixed::from_raw(saturated));
    };
    tactical::DurabilityTable table;
    table.rules = {constraint, engines, damaged};
    const auto text = [&tables](const std::string_view tag) -> std::string {
        for (const auto& constant : tables.constants.scalars)
            if (constant.tag == tag)
                return constant.text;
        return {};
    };
    const auto damage_message = "damage needs ShieldRechargeIntervalInSecs, the Depleted_Shield_* constants, "
                                "EnergyRechargeIntervalInSecs and EnergyToShieldExchangeRate";
    const auto recharge = fallback("ShieldRechargeIntervalInSecs", raw(3), damage_message);
    const auto disable = fallback("Depleted_Shield_Disable_Time", raw(5), damage_message);
    const auto increment = fallback("Depleted_Shield_Damage_Increment", raw(0), damage_message);
    const auto cap = fallback("Depleted_Shield_Regen_Cap", raw(1,4), damage_message);
    const auto energy_interval = fallback("EnergyRechargeIntervalInSecs", raw(5), damage_message);
    const auto exchange = fallback("EnergyToShieldExchangeRate", raw(5), damage_message);
    const auto types = damage_type_index(tables);
    tactical::DamageRules damage;
    damage.asteroid_damage = scalar("Asteroid_Field_Damage").value_or(Fixed{});
    damage.asteroid_rate = scalar("Asteroid_Field_Damage_Rate").value_or(Fixed{});
    damage.ion_storm_disable_seconds = scalar("Ion_Storm_Shield_Disable_Time").value_or(Fixed{});
    damage.asteroid_damage_type = types.damage("");
    damage.default_damage_type = types.damage("");
    // DG-13, EN-02: trunc(interval x fps + 0.5) frames, the retail service interval.
    const auto service_frames = [&](const Fixed seconds, const std::string& tag) -> std::uint32_t {
        // Split whole and fractional parts to avoid overflowing Q24 during frame conversion.
        const auto whole = seconds.raw() / Fixed::scale;
        const auto fraction = seconds.raw() % Fixed::scale;
        constexpr auto fps = tactical::logical_frames_per_second;
        std::int64_t frames = seconds.raw() < 0 ? 0 : 1;
        if (whole > static_cast<std::int64_t>(UINT32_MAX) / fps + 1) frames = static_cast<std::int64_t>(UINT32_MAX) + 1;
        else if (whole >= 0)
            frames = whole * fps + (fraction * fps + Fixed::scale / 2) / Fixed::scale;
        if (seconds.raw() >= 0 && frames < 1)
            return 1;
        core::LoadNote note;
        note.code = "EAWR-LOAD-0122";
        note.tag = tag;
        const auto source = std::find_if(tables.constants.scalars.begin(), tables.constants.scalars.end(),
                                         [&](const auto& constant) { return constant.tag == tag; });
        note.authored_value = source == tables.constants.scalars.end() ? fixed_decimal(seconds) : source->text;
        note.diagnostic_code = "EAWR-UNITS-0003";
        note.message = tag + ": service interval out of range";
        if (frames < 1) {
            return static_cast<std::uint32_t>(
                core::clamp_note<std::int64_t>(frames, 1, UINT32_MAX, std::move(note), load));
        }
        return core::saturate_note<std::uint32_t>(frames, std::move(note), load);
    };
    damage.shield_recharge_frames = service_frames(recharge, "ShieldRechargeIntervalInSecs");
    damage.energy_recharge_frames = service_frames(energy_interval, "EnergyRechargeIntervalInSecs");
    damage.energy_to_shield = exchange;
    damage.depleted_disable_seconds = disable;
    damage.depleted_increment_seconds = increment;
    damage.depleted_regen_cap = cap;
    const auto curve = decimals(text("Diminishing_Firepower"));
    for (std::size_t index = 0; index + 1 < curve.size(); index += 2) {
        damage.diminishing.push_back({curve[index], curve[index + 1]});
    }
    damage.damage_types =
        static_cast<std::uint32_t>(std::min<std::size_t>(types.damage_types.size(), tactical::max_damage_types));
    damage.armor_types =
        static_cast<std::uint32_t>(std::min<std::size_t>(types.armor_types.size(), tactical::max_damage_types));
    if (types.damage_types.size() > damage.damage_types || types.armor_types.size() > damage.armor_types) {
        core::LoadNote note;
        note.code = "EAWR-LOAD-0113";
        note.tag = "Damage_To_Armor_Mod";
        note.authored_value =
            std::to_string(types.damage_types.size()) + "x" + std::to_string(types.armor_types.size());
        note.action = core::LoadAction::clamped;
        note.effective_value = std::to_string(damage.damage_types) + "x" + std::to_string(damage.armor_types);
        note.diagnostic_code = "EAWR-SIM-0305";
        note.message = "damage rules: the armor table must be damage types x armor types, each at most " +
                       std::to_string(tactical::max_damage_types);
        load.append(std::move(note));
    }
    // DG-12: a pair without a row multiplies by 1, as FoC fills its table before the rows; a
    // negative entry reads as 1 too; a pair authored twice keeps the later row.
    damage.armor_mods.assign(static_cast<std::size_t>(damage.damage_types) * damage.armor_types,
        Fixed::from_raw(Fixed::scale));
    for (const auto& row : tables.constants.damage_to_armor) {
        const auto d = types.damage(row.damage_type);
        const auto a = types.armor(row.armor_type);
        if (d >= damage.damage_types || a >= damage.armor_types) continue;
        damage.armor_mods[static_cast<std::size_t>(d) * damage.armor_types + a] =
            row.multiplier.raw() < 0 ? Fixed::from_raw(Fixed::scale) : row.multiplier;
    }
    table.damage = std::move(damage);
    for (const auto& unit : tables.units) {
        if (!unit.hull)
            continue; // squadrons, and hulls the scan already reports as unresolved
        // SKI-25: a Tactical_Health of 0 or less builds no durability row (Empire_Gate_Large_Dest
        // authors 0 in FoC and in mods that copy it), as the land reader already skips it.
        if (unit.hull->raw() <= 0) continue;
        tactical::DurabilityProfile profile;
        profile.type_id = assets::object_type_crc(unit.id);
        auto hull = scaled(*unit.hull, unit.id + " hull", "Tactical_Health");
        if (!hull) return Result::failure(hull.error());
        profile.max_hull = hull.value();
        // SKI-64: FoC has no health cap; hold a hull above the simulation bound at the bound.
        const auto bound = Fixed::from_raw(tactical::max_durability_health * Fixed::scale);
        if (profile.max_hull > bound) {
            if (notes != nullptr) {
                auto note =
                    failure("unit type '" + unit.id + "': " + unit.id + " hull " +
                            std::to_string(profile.max_hull.trunc_to_integer()) + " is above the simulation bound " +
                            std::to_string(tactical::max_durability_health) + "; the bound is used (SKI-64)");
                note.severity = core::Severity::warning;
                notes->push_back(std::move(note));
            }
            profile.max_hull = bound;
        }
        profile.max_speed = unit.movement.max_speed;
        profile.destroyed_with_hardpoints = unit.destroyed_with_hardpoints;
        // DG-06: only a SHIELDED type has a shield; Shield_Points is not scaled by the health multiplier.
        if (unit.shielded && unit.shield_points && unit.shield_points->raw() > 0) {
            profile.max_shields = *unit.shield_points;
            profile.shield_refresh = unit.shield_refresh_rate.value_or(Fixed{});
        }
        // EN-01: only a POWERED type has an energy pool; it starts full.
        profile.ion_stun_effect = unit.ion_stun_effect; // IS-02
        profile.damage_tracking = unit.damage_tracking; // WCC-82
        if (unit.powered) {
            profile.powered = true;
            profile.max_energy = unit.energy_capacity.value_or(Fixed{});
            profile.energy_refresh = unit.energy_refresh_rate.value_or(Fixed{});
        }
        profile.armor_type = types.armor(unit.armor_type);
        profile.shield_armor_type = types.armor(unit.shield_armor_type);
        bool zero_health_hardpoint = false;
        for (const auto& hardpoint : unit.hardpoints) {
            tactical::HardpointProfile entry;
            entry.role = role(hardpoint.type);
            entry.destroyable = hardpoint.destroyable;
            if (hardpoint.destroyable) {
                // HD-06: omitted Health was defaulted to zero; authored parse failures still fail.
                if (!hardpoint.health) {
                    return Result::failure(
                        failure(unit.id + " hardpoint " + hardpoint.id + " is destroyable without Health"));
                }
                const auto authored = *hardpoint.health;
                zero_health_hardpoint = zero_health_hardpoint || authored.raw() == 0;
                auto health = scaled(authored, unit.id + " hardpoint " + hardpoint.id, "Health");
                if (!health) return Result::failure(health.error());
                entry.max_health = health.value();
            }
            entry.repair_amount_per_frame = hardpoint.repair_amount_per_frame.value_or(Fixed{});
            entry.repair_cost_per_frame = hardpoint.repair_cost_per_frame.value_or(Fixed{});
            entry.allows_special_weapon_use = hardpoint.allows_special_weapon_use;
            if (hardpoint.type == HardpointType::enable_special_ability)
                entry.special_ability_name = hardpoint.special_ability_name;
            profile.hardpoints.push_back(entry);
        }
        if (zero_health_hardpoint && notes != nullptr) {
            auto note = failure(unit.id + " has destroyable hardpoints with omitted or zero Health; "
                "they start destroyed and cannot fire (HD-06)");
            note.severity = core::Severity::warning;
            notes->push_back(std::move(note));
        }
        table.profiles.push_back(std::move(profile));
    }
    std::stable_sort(table.profiles.begin(), table.profiles.end(),
              [](const auto& left, const auto& right) { return left.type_id < right.type_id; });
    auto duplicate =
        std::unique(table.profiles.begin(), table.profiles.end(), [&](const auto& left, const auto& right) {
            if (left.type_id != right.type_id)
                return false;
            core::LoadNote note;
            note.code = "EAWR-LOAD-0123";
            const auto source = std::find_if(tables.units.begin(), tables.units.end(), [&](const auto& unit) {
                return assets::object_type_crc(unit.id) == right.type_id;
            });
            note.object = source == tables.units.end() ? "type#" + std::to_string(right.type_id) : source->id;
            note.action = core::LoadAction::skipped_object;
            note.message = "two unit types share type ID " + std::to_string(right.type_id);
            note.diagnostic_code = "EAWR-UNITS-0003";
            note.effective_value = "first type retained";
            load.append(std::move(note));
            return true;
        });
    table.profiles.erase(duplicate, table.profiles.end());
    core::LoadNoteBuffer normalized_notes;
    tactical::normalize_durability(table, normalized_notes);
    core::LoadNoteCollector collector;
    collector.merge(normalized_notes);
    for (auto note : collector.snapshot()) {
        const auto scalar_source = std::find_if(tables.constants.scalars.begin(), tables.constants.scalars.end(),
                                                [&](const auto& constant) { return constant.tag == note.tag; });
        if (scalar_source != tables.constants.scalars.end())
            note.authored_value = scalar_source->text;
        for (const auto& unit : tables.units) {
            const auto key = "type#" + std::to_string(assets::object_type_crc(unit.id));
            const auto position = note.object.find(key);
            if (position != std::string::npos) {
                note.object.replace(position, key.size(), unit.id);
                break;
        }
    }
        load.append(std::move(note));
    }
    auto valid = tactical::validate_durability(table);
    if (!valid) return Result::failure(failure(valid.error().message));
    return Result::success(std::move(table));
}

} // namespace eawr::units
