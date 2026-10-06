#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/roster_gate.hpp"

#include "eawr/script/numeric/q24_boundary.hpp"

#include <algorithm>
#include <bit>

namespace eawr::skirmish {
namespace {

using numeric_value = script::numeric::LuaNumber;

std::string upper(std::string_view text) {
    std::string out(text);
    for (char& character : out) {
        if (character >= 'a' && character <= 'z') character = static_cast<char>(character - ('a' - 'A'));
    }
    return out;
}

numeric_value lua(sim::math::Fixed value) { return script::numeric::from_fixed(value); }

// A binary32 bit pattern widened exactly to binary64 (normal numbers and zero).
numeric_value lua_float(std::uint32_t bits) {
    const std::uint64_t sign = static_cast<std::uint64_t>(bits >> 31) << 63;
    const std::uint32_t exponent = (bits >> 23) & 0xffU;
    const std::uint64_t mantissa = bits & 0x7fffffU;
    if (exponent == 0) return numeric_value::from_repr(sign);
    return numeric_value::from_repr(sign | (static_cast<std::uint64_t>(exponent - 127 + 1023) << 52) | (mantissa << 29));
}

} // namespace

script::foc::AiSetup ai_setup(const SkirmishStart& start, const StartInputs& inputs, const units::UnitTables& tables) {
    script::foc::AiSetup setup;
    setup.seed = start.setup.seed;
    setup.perception.campaign_game = false; // SAE-01
    for (const auto& category : tables.categories) setup.content.categories.emplace(upper(category.name), category.value);
    for (const auto& property : tables.properties) setup.content.properties.emplace(upper(property.name), property.value);
    for (const auto& unit : tables.units) {
        if (roster_disabled_types().contains(type_id(unit.id))) continue;
        script::foc::AiType type;
        type.type_id = type_id(unit.id);
        type.name = upper(unit.id);
        type.category_bits = unit.category_bits;
        type.property_bits = unit.property_bits;
        for (const auto& ability : unit.abilities) {
            if (roster_ability_reason(unit.id, ability.type).empty()) type.abilities.push_back(upper(ability.type));
        }
        type.projectile_types = unit.weapon ? 1U : 0U;
        // BEHAVIOR_LOCO: ships that move and squadrons; stations and craft (members) do not
        // enter the freestore on their own.
        type.locomotor = (unit.kind == units::UnitKind::ship && unit.movement.max_speed.has_value()) ||
            unit.kind == units::UnitKind::squadron;
        type.star_base = unit.kind == units::UnitKind::station;
        type.capture_point = unit.capture_point;
        type.build_pad = unit.build_pad;
        type.tactical_cost = unit.production.build_cost_multiplayer.value_or(sim::math::Fixed{});
        // #449 goal system and perception inputs.
        type.space_evaluator = unit.has_space_evaluator;
        type.initial_state_visible_under_fow = unit.initial_state_visible_under_fow;
        type.last_state_visible_under_fow = unit.last_state_visible_under_fow;
        type.tech_level = unit.tech_level;
        type.base_level = unit.base_level;
        if (unit.ai_combat_power) type.combat_power = lua(*unit.ai_combat_power);
        if (unit.targeting_max_attack_distance) type.max_attack_distance = lua(*unit.targeting_max_attack_distance);
        if (unit.movement.max_speed) type.max_speed = lua(*unit.movement.max_speed);
        if (unit.footprint.custom_soft_radius) type.soft_radius = lua(*unit.footprint.custom_soft_radius);
        if (unit.space_fow_reveal_range) type.reveal_range = lua(*unit.space_fow_reveal_range);
        type.squadron = unit.kind == units::UnitKind::squadron;
        type.craft = unit.kind == units::UnitKind::craft;
        type.squadron_units = static_cast<std::uint32_t>(unit.members.size());
        if (!unit.members.empty()) type.squadron_unit = type_id(unit.members.front().craft);
        // PL-13: a type with squadron units takes the union of its craft's categories and
        // properties and the sum of their combat power at load, in place of its own tags.
        // PG-08's squad-size scale is 1 for every FoC squadron (at most 7 craft, squad size
        // 10 unless set, and 6 or 8 where set), so it is not applied.
        if (!unit.members.empty()) {
            type.category_bits = 0;
            type.property_bits = 0;
            type.combat_power = numeric_value{};
            for (const auto& member : unit.members) {
                if (member.craft_index >= tables.units.size()) continue;
                const auto& craft = tables.units[member.craft_index];
                type.category_bits |= craft.category_bits;
                type.property_bits |= craft.property_bits;
                if (craft.ai_combat_power) type.combat_power = type.combat_power + lua(*craft.ai_combat_power);
            }
        }
        // PG-02: each weapon hardpoint carries its projectile's share of the type's power (A-06).
        const auto projectile_of = [&](const units::Weapon& weapon) -> const units::Projectile* {
            return weapon.projectile_index < tables.projectiles.size() ? &tables.projectiles[weapon.projectile_index] : nullptr;
        };
        numeric_value share_total{};
        for (const auto& hardpoint : unit.hardpoints) {
            if (!hardpoint.weapon) continue;
            if (const auto* projectile = projectile_of(*hardpoint.weapon); projectile != nullptr && projectile->ai_combat_power) {
                share_total = share_total + lua(*projectile->ai_combat_power);
            }
        }
        for (std::uint32_t index = 0; index < unit.hardpoints.size(); ++index) {
            const auto& hardpoint = unit.hardpoints[index];
            if (!hardpoint.weapon) continue;
            const auto* projectile = projectile_of(*hardpoint.weapon);
            if (projectile != nullptr && projectile->damage) type.weapon_damage[index] = lua(*projectile->damage);
            if (!hardpoint.targetable) continue;
            script::foc::AiWeapon weapon;
            weapon.hardpoint = index;
            weapon.targetable = hardpoint.targetable;
            weapon.destroyable = hardpoint.destroyable;
            if (hardpoint.weapon->range) weapon.range = lua(*hardpoint.weapon->range);
            if (projectile != nullptr && projectile->ai_combat_power && numeric_value{} < share_total) {
                weapon.combat_power = lua(*projectile->ai_combat_power) / share_total * type.combat_power;
            }
            type.weapons.push_back(weapon);
        }
        if (unit.weapon) {
            if (const auto* projectile = projectile_of(*unit.weapon); projectile != nullptr && projectile->damage) {
                type.weapon_damage[0xffffffffU] = lua(*projectile->damage);
            }
        }
        setup.content.types.push_back(std::move(type));
    }
    std::sort(setup.content.types.begin(), setup.content.types.end(),
        [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    setup.content.types.erase(std::unique(setup.content.types.begin(), setup.content.types.end(),
                                  [](const auto& a, const auto& b) { return a.type_id == b.type_id; }),
        setup.content.types.end());
    for (const auto& player : start.players) {
        script::foc::AiPlayer entry;
        entry.player = player.player.player_id;
        entry.faction = upper(player.faction);
        entry.ai = player.lobby && !player.human;
        entry.human = player.lobby && player.human;
        for (const auto& faction : inputs.factions) {
            if (upper(faction.name) != entry.faction) continue;
            entry.neutral = faction.neutral;
            // WSS-35, AI-11: every lobby AI uses its faction's authored Basic_AI.
            if (entry.ai) entry.player_type = faction.basic_ai;
            break;
        }
        setup.players.push_back(std::move(entry));
    }
    // PG-01: the declared extents, centred on the origin (Coruscant: 13000 x 13000 from -6500).
    if (inputs.map_extents) {
        const numeric_value width = lua_float(inputs.map_extents->first);
        const numeric_value height = lua_float(inputs.map_extents->second);
        const numeric_value half(2);
        setup.bounds = script::foc::AiBounds{-(width / half), height / half, width / half, -(height / half)};
    }
    return setup;
}

core::Result<void> enable_goal_system(const vfs::Vfs& files, script::foc::AiSetup& setup) {
    for (const std::string& path : script::foc::required_xml()) {
        auto bytes = files.open(path);
        if (!bytes) return core::Result<void>::failure(bytes.error());
        std::string text(bytes.value().size(), '\0');
        std::transform(bytes.value().begin(), bytes.value().end(), text.begin(),
            [](std::byte value) { return static_cast<char>(value); });
        setup.xml.emplace(path, std::move(text));
    }
    return core::Result<void>::success();
}

core::Result<std::map<std::string, std::string>> ai_modules(
    const vfs::Vfs& files, const script::foc::AiSetup& setup, const bool plans) {
    using ModulesResult = core::Result<std::map<std::string, std::string>>;
    std::map<std::string, std::string> modules;
    auto paths = script::foc::required_modules(setup);
    if (plans) {
        for (auto list : {script::foc::plan_library_modules(), script::foc::selected_plans()}) {
            paths.insert(paths.end(), list.begin(), list.end());
        }
    }
    for (const std::string& path : paths) {
        auto bytes = files.open(path);
        if (!bytes) return ModulesResult::failure(bytes.error());
        std::string text(bytes.value().size(), '\0');
        std::transform(bytes.value().begin(), bytes.value().end(), text.begin(),
            [](std::byte value) { return static_cast<char>(value); });
        modules.emplace(path, std::move(text));
    }
    return ModulesResult::success(std::move(modules));
}

} // namespace eawr::skirmish
