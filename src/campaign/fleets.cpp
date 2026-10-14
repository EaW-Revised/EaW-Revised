#include "eawr/campaign/fleets.hpp"
#include "eawr/units/unit_tables.hpp"

#include <algorithm>
#include <limits>

namespace eawr::campaign {
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
        const bool children = !transport && (equal(type->object_class, "Container") || equal(type->object_class, "Squadron")
            || !type->squadron_units.empty() || !type->company_units.empty());
        // WGM-100: every original shares the true omission default; specialized
        // container/transport dispatch decides how to consume that capability.
        result.push_back({binding.id, transport, type->can_hyperspace_without_ability.value_or(true), children});
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
    if (!base || !base->number || base->number->raw() <= 0) return fail("missing or invalid Object_Max_Speed_Multiplier_Galactic");
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
            if (!original->hyperspace_speed || original->hyperspace_speed->raw() < 0)
                return fail(binding.original_type + ": negative Hyperspace_Speed", original->file, original->line);
            t.hyperspace_speed = *original->hyperspace_speed;
        }
        if (!binding.squadron && !binding.fleet_container) {
            if (!original->squadron_capacity || *original->squadron_capacity < 0
                || static_cast<std::uint64_t>(*original->squadron_capacity) > std::numeric_limits<std::uint32_t>::max())
                return fail(binding.original_type + ": invalid Squadron_Capacity", original->file, original->line);
            t.squadron_capacity = static_cast<std::uint32_t>(*original->squadron_capacity);
        }
        if (binding.squadron) {
            // WGEP-92: authored absence consumes the shared zero default.
            t.build_cost = original->build_cost_credits.value_or(sim::math::Fixed{});
            if (t.build_cost.raw() < 0)
                return fail(binding.original_type + ": negative Build_Cost_Credits", original->file, original->line);
        }
        out.types.push_back(t);
    }
    sim::galactic::PlayerId previous_player = 0;
    for (const auto& binding : players) {
        const auto* faction = data.faction(binding.faction);
        if (!faction || binding.id <= previous_player) return fail(binding.faction + ": missing faction or invalid player binding");
        previous_player = binding.id;
        if (!faction->hyperspace_speed_factor || faction->hyperspace_speed_factor->raw() < 0 || !faction->benefits_from_corruption)
            return fail(binding.faction + ": missing movement factor or corruption eligibility", faction->file);
        sim::galactic::MovementPlayer p{binding.id, *faction->hyperspace_speed_factor, {}, *faction->benefits_from_corruption};
        if (p.benefits_from_corruption) {
            if (!bonus || !bonus->number || bonus->number->raw() < 0) return fail("missing or invalid Corruption_Hyperspace_Bonus");
            p.corruption_bonus = *bonus->number;
        }
        out.players.push_back(p);
    }
    return core::Result<MovementInputs>::success(std::move(out));
}
} // namespace eawr::campaign
