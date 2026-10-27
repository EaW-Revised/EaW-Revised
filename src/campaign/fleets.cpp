#include "eawr/campaign/fleets.hpp"
#include "eawr/units/unit_tables.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include "eawr/data/retail_values.hpp"

namespace eawr::campaign {
namespace gc = sim::galactic;
using sim::math::Fixed;
core::Result<std::optional<gc::RaidSetup>> build_raid_inputs(const units::GalacticTypeTable& types,
    const data::galactic::GalacticData& data, std::span<const FleetTypeBinding> bindings,
    std::span<const RaidFactionBinding> factions) {
    using Out = std::optional<gc::RaidSetup>;
    const auto fail = [](std::string message) {
        core::Diagnostic error; error.code = std::string(gc::diagnostic_codes::invalid_setup); error.message = std::move(message);
        return core::Result<Out>::failure(std::move(error));
    };
    const auto key = [](std::string text) {
        for (auto& c : text) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        return text;
    };
    const auto words = [&](const std::string& text) {
        std::istringstream stream(text); std::vector<std::string> result;
        for (std::string word; stream >> word;) if (word != ",") result.push_back(key(word));
        return result;
    };
    const auto contains = [&](const std::vector<std::string>& names, const std::string& name) {
        return std::find(names.begin(), names.end(), key(name)) != names.end();
    };
    const auto* faction = data.constant("Raid_Force_Required_Faction");
    const auto* free = data.constant("Raid_Force_Free_Object_Category_Mask");
    const auto* limited = data.constant("Raid_Force_Limited_Object_Category_Mask");
    const auto* heroes = data.constant("Raid_Force_Max_Heros");
    const auto* count = data.constant("Raid_Force_Max_Limited_Objects");
    if (!faction && (!free || (free->text.empty() && !free->number))
        && (!limited || (limited->text.empty() && !limited->number)) && !heroes && !count)
        return core::Result<Out>::success({});
    if (!faction || !free || !limited || !heroes || !count) return fail("incomplete raid policy");
    const auto cap = [](const auto* constant) -> std::optional<std::uint32_t> {
        const auto value = constant->number;
        if (!value || value->raw() < 0 || value->raw() % Fixed::scale != 0
            || static_cast<std::uint64_t>(value->raw() / Fixed::scale) > std::numeric_limits<std::uint32_t>::max()) return {};
        return static_cast<std::uint32_t>(value->raw() / Fixed::scale);
    };
    const auto max_heroes = cap(heroes), max_limited = cap(count);
    if (!max_heroes || !max_limited) return fail("invalid raid caps");
    gc::RaidSetup out;
    // Two internal bits classify membership in the two authored masks. They
    // are representation values, not a fixed list of game categories.
    out.rules = {0, 1, 2, *max_heroes, *max_limited};
    if (!data::retail::is_placeholder(faction->text)) {
        gc::FactionId unused{};
        for (const auto& binding : factions) unused = std::max(unused, binding.id);
        if (unused == std::numeric_limits<gc::FactionId>::max()) return fail("raid faction identity exhausted");
        out.rules.required_faction = unused + 1; // absent roster faction permits no player
        for (const auto& binding : factions) if (key(binding.faction) == key(faction->text))
            out.rules.required_faction = binding.id;
    }
    auto free_text = free->text, limited_text = limited->text;
    std::replace_if(free_text.begin(), free_text.end(), [](char c) { return c == '|' || c == ','; }, ' ');
    std::replace_if(limited_text.begin(), limited_text.end(), [](char c) { return c == '|' || c == ','; }, ' ');
    const auto free_names = words(free_text), limited_names = words(limited_text);
    gc::TypeId previous{};
    for (const auto& binding : bindings) {
        if (!binding.id || binding.id <= previous) return fail("invalid raid type binding order");
        previous = binding.id;
        const auto* type = types.find(binding.original_type);
        if (!type) return fail("unbound raid type");
        gc::RaidType row; row.type = binding.id;
        for (const auto& category : type->category_mask) {
            if (contains(free_names, category)) row.categories |= 1;
            if (contains(limited_names, category)) row.categories |= 2;
        }
        row.hero = type->named_hero.value_or(false); row.stays_in_transport = type->stays_in_transport.value_or(false);
        out.types.push_back(row);
    }
    return core::Result<Out>::success(std::move(out));
}


core::Result<sim::galactic::MovementPlanet> prepare_initial_movement_planet(
    sim::galactic::PlanetId planet, const assets::Model& model, sim::math::Fixed scale_factor) {
    using Out = sim::galactic::MovementPlanet;
    const auto fail = [] {
        core::Diagnostic error; error.code = std::string(sim::galactic::diagnostic_codes::invalid_setup);
        error.message = "initial planet model bounds have invalid identity, scale or Q24 range";
        return core::Result<Out>::failure(std::move(error));
    };
    if (!planet || scale_factor.raw() <= 0) return fail();
    const auto bounds = units::prepare_galactic_collision_bounds(model);
    if (!bounds) return core::Result<Out>::failure(bounds.error());
    sim::math::Fixed extent, scaled, half;
    if (!sim::math::try_subtract(bounds.value().max.z, bounds.value().min.z, extent)
        || !sim::math::try_multiply(extent, scale_factor, scaled)
        || !sim::math::try_divide(scaled, sim::math::Fixed::from_raw(2 * sim::math::Fixed::scale), half)) return fail();
    return core::Result<Out>::success({planet, half, false});
}
core::Result<std::vector<sim::galactic::FleetTypeTraits>> build_fleet_type_traits(
    const units::GalacticTypeTable& types, const std::span<const FleetTypeBinding> bindings) {
    using Out = std::vector<sim::galactic::FleetTypeTraits>;
    Out result;
    sim::galactic::TypeId previous{};
    const auto equal = [](std::string a, std::string b) {
        const auto upper = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; };
        std::transform(a.begin(), a.end(), a.begin(), upper); std::transform(b.begin(), b.end(), b.begin(), upper);
        return a == b;
    };
    for (const auto& binding : bindings) {
        const auto* type = types.find(binding.original_type);
        const auto fail = [&](const std::string& message) {
            core::Diagnostic diagnostic; diagnostic.code = std::string(sim::galactic::diagnostic_codes::invalid_setup);
            diagnostic.severity = core::Severity::error; diagnostic.message = binding.original_type + ": " + message;
            if (type) { diagnostic.logical_path = type->file; diagnostic.line = type->line; }
            return core::Result<Out>::failure(std::move(diagnostic));
        };
        if (!type || binding.id <= previous) return fail("missing original type or invalid identity binding");
        previous = binding.id;
        const bool transport = std::any_of(type->behavior.begin(), type->behavior.end(),
            [&](const auto& behavior) { return equal(behavior, "TRANSPORT"); });
        // WGEP-41: production completion dispatches on the galactic behaviour; a DUMMY_STARSHIP
        // type takes the single-ship fleet route even when its class or Squadron_Units make it a
        // tactical squadron (AaW Kom'rk_Starfighter_Squadron), so it has no live galactic children.
        const bool single_ship = std::any_of(type->behavior.begin(), type->behavior.end(),
            [&](const auto& behavior) { return equal(behavior, "DUMMY_STARSHIP"); });
        const bool children = !transport && !single_ship && (equal(type->object_class, "Container")
            || equal(type->object_class, "Squadron") || !type->squadron_units.empty() || !type->company_units.empty());
        // WGM-100: every original shares the true omission default; specialized
        // container/transport dispatch decides how to consume that capability.
        result.push_back({binding.id, transport, type->can_hyperspace_without_ability.value_or(true), children,
            transport || type->can_participate_in_space.value_or(true)});
    }
    return core::Result<Out>::success(std::move(result));
}
core::Result<MovementInputs> build_movement_inputs(const units::GalacticTypeTable& types,
    const data::galactic::GalacticData& data, std::span<const MovementTypeBinding> objects,
    std::span<const MovementPlayerBinding> players) {
    MovementInputs out;
    const auto fail = [](const std::string& message, const std::string& file = {}, std::uint64_t line = 0) {
        core::Diagnostic d; d.code = std::string(sim::galactic::diagnostic_codes::invalid_setup);
        d.severity = core::Severity::error; d.message = message; d.logical_path = file; d.line = line;
        return core::Result<MovementInputs>::failure(std::move(d));
    };
    const auto* base = data.constant("Object_Max_Speed_Multiplier_Galactic");
    if (!base || !base->number) return fail("missing or invalid Object_Max_Speed_Multiplier_Galactic");
    out.base_mode_modifier = *base->number; // WGM-97: initialized GC base, never constructor/slider default
    const auto* bonus = data.constant("Corruption_Hyperspace_Bonus");
    sim::galactic::TypeId previous_type = 0;
    for (const auto& binding : objects) {
        const auto resolved = units::resolve_initial_galactic_type(types, binding.original_type);
        if (!resolved) return core::Result<MovementInputs>::failure(resolved.error());
        const auto* original = &resolved.value();
        if (!original || binding.id <= previous_type || (binding.squadron && binding.fleet_container)
            || (binding.transport && (binding.squadron || binding.fleet_container))) return fail(binding.original_type + ": missing original type or invalid binding");
        previous_type = binding.id;
        sim::galactic::MovementType t{binding.id, {}, {}, 0, binding.hyperspace_eligible, binding.squadron, binding.fleet_container, binding.skips_combat, binding.transport};
        // WGM-26 uses live craft for fleet speed; WGM-118/151 uses the squadron
        // template speed only for its packing candidate key.
        if (binding.squadron || (!binding.fleet_container && binding.hyperspace_eligible)) {
            if (!original->hyperspace_speed)
                return fail(binding.original_type + ": missing Hyperspace_Speed", original->file, original->line);
            t.hyperspace_speed = *original->hyperspace_speed;
        }
        if (!binding.squadron && !binding.fleet_container) {
            if (!original->squadron_capacity || *original->squadron_capacity < 0
                || static_cast<std::uint64_t>(*original->squadron_capacity) > std::numeric_limits<std::uint32_t>::max())
                return fail(binding.original_type + ": invalid Squadron_Capacity", original->file, original->line);
            t.squadron_capacity = static_cast<std::uint32_t>(*original->squadron_capacity);
        }
        // WGM-150: only the leaf original type carries the departure piracy value.
        if (!binding.squadron && !binding.fleet_container) t.piracy_value = original->piracy_value_credits.value_or(sim::math::Fixed{});
        // WGM-85: movement admission prices each leaf member by its original type; an omitted cost is the shared zero default.
        if (!binding.squadron && !binding.fleet_container) {
            t.cheap_bribe = original->cheap_bribe_cost.value_or(sim::math::Fixed{});
            t.expensive_bribe = original->expensive_bribe_cost.value_or(sim::math::Fixed{});
            if (t.cheap_bribe.raw() < 0 || t.expensive_bribe.raw() < 0)
                return fail(binding.original_type + ": negative Cheap/Expensive_Bribe_Cost", original->file, original->line);
        }
        if (binding.squadron) {
            // WGEP-92: authored absence consumes the shared zero default.
            t.build_cost = original->build_cost_credits.value_or(sim::math::Fixed{});
        }
        out.types.push_back(t);
    }
    sim::galactic::PlayerId previous_player = 0;
    for (const auto& binding : players) {
        const auto* faction = data.faction(binding.faction);
        if (!faction || binding.id <= previous_player) return fail(binding.faction + ": missing faction or invalid player binding");
        previous_player = binding.id;
        if (!faction->hyperspace_speed_factor || !faction->benefits_from_corruption)
            return fail(binding.faction + ": missing movement factor or corruption eligibility", faction->file);
        sim::galactic::MovementPlayer p{binding.id, *faction->hyperspace_speed_factor, {}, *faction->benefits_from_corruption,
            faction->easily_bribed.value_or(false)};
        if (p.benefits_from_corruption) {
            if (!bonus || !bonus->number) return fail("missing or invalid Corruption_Hyperspace_Bonus");
            p.corruption_bonus = *bonus->number;
        }
        out.players.push_back(p);
    }
    return core::Result<MovementInputs>::success(std::move(out));
}
} // namespace eawr::campaign
