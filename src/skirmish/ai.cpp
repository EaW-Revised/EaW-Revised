#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/placement.hpp"
#include "eawr/skirmish/roster_gate.hpp"

#include "eawr/script/numeric/q24_boundary.hpp"

#include <algorithm>
#include <bit>
#include <set>

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
    setup.allow_special_weapons = start.match.allow_superweapons;
    setup.perception.campaign_game = false; // SAE-01
    for (const auto& category : tables.categories) setup.content.categories.emplace(upper(category.name), category.value);
    for (const auto& property : tables.properties) setup.content.properties.emplace(upper(property.name), property.value);
    for (const auto& projectile : tables.projectiles) {
        // WTA-34: the categories the engine's defensive ability queries test.
        std::uint32_t bits = 0;
        if (upper(projectile.category) == "MISSILE") bits = script::foc::ai_projectile_missile;
        else if (upper(projectile.category) == "ROCKET") bits = script::foc::ai_projectile_rocket;
        else if (upper(projectile.category) == "MPTL_ROCKET") bits = script::foc::ai_projectile_mptl_rocket;
        if (bits != 0) setup.content.projectile_categories.emplace(type_id(projectile.id), bits);
    }
    // PL-20: content.types is the type table in load order; a category team enumerates it in that
    // order, so equal weights pick the type the original lists first. Equal positions keep roster order.
    std::vector<const units::UnitType*> table;
    table.reserve(tables.units.size());
    for (const auto& unit : tables.units) table.push_back(&unit);
    std::stable_sort(table.begin(), table.end(), [](const auto* a, const auto* b) { return a->table_order < b->table_order; });
    for (const auto* row : table) {
        const auto& unit = *row;
        if (roster_disabled_types().contains(type_id(unit.id))) continue;
        script::foc::AiType type;
        type.type_id = type_id(unit.id);
        type.name = upper(unit.id);
        type.decoration = unit.decoration;
        type.lua_script = unit.lua_script;
        const auto scale = unit.scale_factor.value_or(sim::math::Fixed::from_integer(1).value());
        const auto bound = [&](sim::math::Vec3 point) {
            return sim::math::Vec3{sim::math::multiply(point.x, scale).value(),
                sim::math::multiply(point.y, scale).value(), sim::math::multiply(point.z, scale).value()};
        };
        if (unit.collision) type.proximity_bounds = std::array{bound(unit.collision->min), bound(unit.collision->max)};
        type.category_bits = unit.category_bits;
        type.property_bits = unit.property_bits;
        for (const auto& ability : unit.abilities) {
            if (roster_ability_reason(unit.id, ability.type).empty()) type.abilities.push_back(upper(ability.type));
        }
        type.projectile_types = unit.weapon ? 1U : 0U;
        // WTA-34: the projectile types Get_All_Projectile_Types lists, hardpoints in order and then the object weapon.
        const auto projectile_id = [&](const units::Weapon& weapon) -> sim::tactical::TypeId {
            return weapon.projectile_index < tables.projectiles.size()
                ? type_id(tables.projectiles[weapon.projectile_index].id) : sim::tactical::TypeId{};
        };
        for (const auto& hardpoint : unit.hardpoints) {
            type.hardpoint_projectiles.push_back(hardpoint.weapon ? projectile_id(*hardpoint.weapon) : sim::tactical::TypeId{});
        }
        if (unit.weapon) type.object_projectile = projectile_id(*unit.weapon);
        type.point_defence_script = upper(unit.lua_script) == "OBJECTSCRIPT_POINTDEFENSE";
        type.corrupt_systems_script = upper(unit.lua_script) == "OBJECTSCRIPT_IG2000";
        // BEHAVIOR_LOCO: ships that move and squadrons; stations and craft (members) do not
        // enter the freestore on their own, nor do ships without a locomotor (WAB-71 droids).
        type.locomotor = (unit.kind == units::UnitKind::ship && unit.movement.max_speed.has_value() && unit.locomotion) ||
            unit.kind == units::UnitKind::squadron;
        type.star_base = unit.star_base;
        type.space_special_weapon = unit.planetary_weapon.has_value();
        type.targeting = unit.targeting;
        type.unit_ai = unit.unit_ai;
        type.capture_point = unit.capture_point;
        type.build_pad = unit.build_pad;
        type.tactical_cost = unit.production.build_cost_multiplayer.value_or(sim::math::Fixed{});
        // #449 goal system and perception inputs.
        type.space_evaluator = unit.has_space_evaluator;
        type.initial_state_visible_under_fow = unit.initial_state_visible_under_fow;
        type.last_state_visible_under_fow = unit.last_state_visible_under_fow;
        type.tech_level = unit.tech_level;
        type.base_level = unit.base_level;
        type.previous_base_levels = unit.previous_base_levels;
        if (unit.ai_combat_power) type.combat_power = lua(*unit.ai_combat_power);
        if (unit.targeting_max_attack_distance) type.max_attack_distance = lua(*unit.targeting_max_attack_distance);
        if (unit.targeting_min_attack_distance) type.min_attack_distance = lua(*unit.targeting_min_attack_distance);
        // WTA-36: the hero the type builds; a company member keeps its own authored hero flag.
        if (!unit.company_members.empty()) {
            const auto& member = unit.company_members.front();
            const auto* body = tables.find(member.type);
            type.hero = member.named_hero || (body != nullptr && body->special_weapon_in_space);
        } else {
            const auto* body = unit.members.empty() || unit.members.front().craft_index >= tables.units.size()
                ? &unit : &tables.units[unit.members.front().craft_index];
            type.hero = body->named_hero || body->special_weapon_in_space;
        }
        if (unit.movement.max_speed) type.max_speed = lua(*unit.movement.max_speed);
        if (unit.footprint.custom_soft_radius) type.soft_radius = lua(*unit.footprint.custom_soft_radius);
        if (unit.space_fow_reveal_range) type.reveal_range = lua(*unit.space_fow_reveal_range);
        if (const auto box = placement_box(unit)) {
            // WTA-28: the planar half extents of the type's bounds (the PL-02 placement box).
            type.bounds_half_x = lua(sim::math::Fixed::from_raw((box->max_x.raw() - box->min_x.raw()) / 2));
            type.bounds_half_y = lua(sim::math::Fixed::from_raw((box->max_y.raw() - box->min_y.raw()) / 2));
        }
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
    std::set<sim::tactical::TypeId> listed;
    setup.content.types.erase(std::remove_if(setup.content.types.begin(), setup.content.types.end(),
                                  [&](const auto& type) { return !listed.insert(type.type_id).second; }),
        setup.content.types.end());
    for (const auto& player : start.players) {
        script::foc::AiPlayer entry;
        entry.player = player.player.player_id;
        entry.faction = upper(player.faction);
        // MAT-09: a demo map's player runs the AI player type the map's start names, by editor index.
        const bool named = !player.ai_player_type.empty();
        entry.ai = (player.lobby && !player.human) || named;
        entry.human = player.lobby && player.human;
        for (const auto& faction : inputs.factions) {
            if (upper(faction.name) != entry.faction) continue;
            entry.neutral = faction.neutral;
            // WSS-35, AI-11: every lobby AI uses its faction's authored Basic_AI.
            if (entry.ai) entry.player_type = named ? player.ai_player_type : faction.basic_ai;
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
    // AI-16: these are engine-defined directories, not content filenames.
    std::vector<std::string> paths;
    for (const auto directory : {"players", "templates", "goalfunctions", "goals", "perceptualequations"}) {
        const std::string prefix = std::string("data/xml/ai/") + directory + "/";
        auto entries = files.enumerate(prefix, ".xml");
        if (!entries) return core::Result<void>::failure(entries.error());
        for (const auto& entry : entries.value()) {
            if (entry.canonical_path.find('/', prefix.size()) == std::string::npos) paths.push_back(entry.canonical_path);
        }
    }
    for (const auto& path : script::foc::required_xml())
        if (!path.starts_with("data/xml/ai/")) paths.push_back(path);
    std::map<std::string, std::string, std::less<>> xml;
    for (const std::string& path : paths) {
        auto bytes = files.open(path);
        if (!bytes) return core::Result<void>::failure(bytes.error());
        std::string text(bytes.value().size(), '\0');
        std::transform(bytes.value().begin(), bytes.value().end(), text.begin(),
            [](std::byte value) { return static_cast<char>(value); });
        xml.emplace(path, std::move(text));
    }
    setup.xml = std::move(xml);
    return core::Result<void>::success();
}

core::Result<std::map<std::string, std::string>> ai_modules(
    const vfs::Vfs& files, const script::foc::AiSetup& setup, const bool plans) {
    using ModulesResult = core::Result<std::map<std::string, std::string>>;
    std::map<std::string, std::string> modules;
    auto paths = script::foc::required_modules(setup);
    if (plans) {
        const auto list = script::foc::plan_library_modules();
        paths.insert(paths.end(), list.begin(), list.end());
    }
    // L-03a / ML-34: freeze the mounted logical script corpus before creating
    // instances. Unused chunks are not executed or compiled. Existing host
    // quotas also bound this admission policy; require never opens native files.
    std::set<std::string> admitted;
    for (const auto& path : paths) admitted.insert(upper(path));
    for (const auto extension : {".lua", ".lc"}) {
        auto entries = files.enumerate("data/scripts/", extension);
        if (!entries) return ModulesResult::failure(entries.error());
        for (const auto& entry : entries.value())
            if (admitted.insert(upper(entry.canonical_path)).second) paths.push_back(entry.canonical_path);
    }
    const script::authoritative::Quotas quotas;
    std::uint64_t total_bytes{};
    for (const std::string& path : paths) {
        if (modules.contains(path)) continue;
        auto entry = files.stat(path);
        if (!entry) return ModulesResult::failure(entry.error());
        if (modules.size() >= quotas.identities || entry.value().size > quotas.memory_bytes - total_bytes) {
            core::Diagnostic diagnostic;
            diagnostic.code = std::string(script::authoritative::codes::module_rejected);
            diagnostic.message = "mounted script inventory exceeds the host admission quota at " + path;
            return ModulesResult::failure(std::move(diagnostic));
        }
        auto bytes = files.open(path);
        if (!bytes) return ModulesResult::failure(bytes.error());
        std::string text(bytes.value().size(), '\0');
        std::transform(bytes.value().begin(), bytes.value().end(), text.begin(),
            [](std::byte value) { return static_cast<char>(value); });
        total_bytes += static_cast<std::uint64_t>(text.size());
        modules.emplace(path, std::move(text));
    }
    return ModulesResult::success(std::move(modules));
}

core::Result<std::map<std::string, std::string>> select_ai_modules(
    const std::map<std::string, std::string>& cached, const script::foc::AiSetup& setup, const bool plans) {
    using Result = core::Result<std::map<std::string, std::string>>;
    auto paths = script::foc::required_modules(setup);
    if (plans) {
        const auto list = script::foc::plan_library_modules();
        paths.insert(paths.end(), list.begin(), list.end());
    }
    for (const auto& path : paths) {
        const auto canonical = script::authoritative::ModuleManifest::normalise(path);
        const auto found = std::find_if(cached.begin(), cached.end(), [&](const auto& entry) {
            return script::authoritative::ModuleManifest::normalise(entry.first) == canonical;
        });
        if (found == cached.end()) {
            core::Diagnostic diagnostic;
            diagnostic.code = std::string(script::authoritative::codes::module_rejected);
            diagnostic.message = "required module is not in the prepared cache: " + path;
            return Result::failure(std::move(diagnostic));
        }
    }
    // A producer can append a previously optional root using another spelling
    // of its logical path. Preserve one copy of identical winning bytes; never
    // choose between conflicting aliases by case-sensitive map order.
    std::map<std::string, std::string> selected;
    std::map<std::string, std::string> spellings;
    for (const auto& [path, bytes] : cached) {
        const auto canonical = script::authoritative::ModuleManifest::normalise(path);
        if (!canonical) {
            core::Diagnostic diagnostic;
            diagnostic.code = std::string(script::authoritative::codes::module_rejected);
            diagnostic.message = "invalid cached module path: " + path;
            return Result::failure(std::move(diagnostic));
        }
        const auto [found, added] = spellings.emplace(*canonical, path);
        if (!added) {
            if (selected.at(found->second) != bytes) {
                core::Diagnostic diagnostic;
                diagnostic.code = std::string(script::authoritative::codes::module_rejected);
                diagnostic.message = "conflicting cached module bytes: " + *canonical;
                return Result::failure(std::move(diagnostic));
            }
            continue;
        }
        selected.emplace(path, bytes);
    }
    // Keep dependencies, including dynamically constructed require names.
    return Result::success(std::move(selected));
}

} // namespace eawr::skirmish
