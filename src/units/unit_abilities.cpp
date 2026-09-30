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

core::Result<tactical::AbilityTable> ability_table(const UnitTables& tables, std::span<const tactical::PlayerId> humans) {
    using Result = core::Result<tactical::AbilityTable>;
    tactical::AbilityTable table;
    table.humans.assign(humans.begin(), humans.end());
    std::sort(table.humans.begin(), table.humans.end());
    table.humans.erase(std::unique(table.humans.begin(), table.humans.end()), table.humans.end());
    for (const auto& unit : tables.units) {
        // AB-15: a squadron's own Unit_Abilities_Data is not the one that acts; its craft's is.
        // AB-60 (#561): a team ability acts on the squadron's team container, which is the
        // squadron company in the session, with the team type's data.
        if (unit.kind == UnitKind::squadron) {
            tactical::UnitAbilityProfile team;
            team.type_id = assets::object_type_crc(unit.id);
            for (const auto& ability : unit.team_abilities) {
                if (tactical::ability_kind(ability.type) != tactical::AbilityKind::ion_cannon_shot) continue;
                tactical::AbilityProfile entry;
                entry.kind = tactical::AbilityKind::ion_cannon_shot;
                entry.recharge_frames = frames(ability.recharge_seconds);
                entry.supports_autofire = ability.supports_autofire;
                entry.team = true;
                team.abilities.push_back(entry);
            }
            if (!team.abilities.empty()) table.profiles.push_back(std::move(team));
            continue;
        }
        tactical::UnitAbilityProfile profile;
        profile.type_id = assets::object_type_crc(unit.id);
        for (const auto& ability : unit.abilities) {
            const auto kind = tactical::ability_kind(ability.type);
            if (kind == tactical::AbilityKind::none) continue; // AB-03: not modelled
            tactical::AbilityProfile entry;
            entry.kind = kind;
            entry.expiration_frames = frames(ability.expiration_seconds);
            entry.recharge_frames = frames(ability.recharge_seconds);
            entry.supports_autofire = ability.supports_autofire;
            for (const auto& modifier : ability.modifiers) {
                auto& m = entry.modifiers;
                const auto& name = modifier.modifier;
                if (iequals(name, "WEAPON_DELAY_MULTIPLIER")) m.weapon_delay = modifier.value;
                else if (iequals(name, "SHIELD_REGEN_MULTIPLIER")) m.shield_regen = modifier.value;
                else if (iequals(name, "SHIELD_REGEN_INTERVAL_MULTIPLIER")) m.shield_regen_interval = modifier.value;
                else if (iequals(name, "ENERGY_REGEN_MULTIPLIER")) m.energy_regen = modifier.value;
                else if (iequals(name, "ENERGY_REGEN_INTERVAL_MULTIPLIER")) m.energy_regen_interval = modifier.value;
                else if (iequals(name, "SPEED_MULTIPLIER")) m.speed = modifier.value;
                else return Result::failure(failure(unit.id + " " + ability.type + ": modifier " + name + " is not modelled"));
            }
            profile.abilities.push_back(entry);
        }
        if (profile.abilities.empty()) continue;
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
