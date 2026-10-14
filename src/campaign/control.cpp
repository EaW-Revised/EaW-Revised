#include "eawr/campaign/control.hpp"

#include <limits>
#include <algorithm>
#include <map>

namespace eawr::campaign {
core::Result<std::vector<sim::galactic::GroundCapabilityType>> prepare_ground_capability_types(
    const units::GalacticTypeTable& types, std::span<const FleetTypeBinding> bindings) {
    using Out = std::vector<sim::galactic::GroundCapabilityType>;
    Out result; sim::galactic::TypeId previous{};
    for (const auto& binding : bindings) {
        auto resolved = units::resolve_initial_galactic_type(types, binding.original_type);
        if (!resolved) return core::Result<Out>::failure(resolved.error());
        if (binding.id <= previous) {
            core::Diagnostic error; error.code = "EAWR-SIM-0501"; error.message = "ground capability requires ordered nonzero type bindings";
            return core::Result<Out>::failure(std::move(error));
        }
        previous = binding.id;
        const auto& type = resolved.value();
        const bool transport = std::find(type.behavior.begin(), type.behavior.end(), "TRANSPORT") != type.behavior.end();
        result.push_back({binding.id, type.ai_combat_power.value_or(sim::math::Fixed{}), transport, type.stays_in_transport.value_or(false)});
    }
    return core::Result<Out>::success(std::move(result));
}
core::Result<sim::galactic::PoliticalControlSetup> prepare_initial_control(
    const sim::galactic::GalacticSetup& galaxy, sim::galactic::PoliticalControlSetup presence) {
    namespace gc = sim::galactic;
    using Out = gc::PoliticalControlSetup;
    // A root start cannot reuse a prior allegiance/capture/death journal.
    if (!presence.state.empty() || !presence.captured_types.empty() || !presence.absent_forces.empty()) {
        core::Diagnostic error; error.code = std::string(gc::diagnostic_codes::invalid_setup);
        error.message = "initial political control requires fresh placement presence";
        return core::Result<Out>::failure(std::move(error));
    }
    auto candidate = galaxy;
    candidate.control = presence;
    if (auto valid = gc::validate_control_setup(candidate); !valid)
        return core::Result<Out>::failure(valid.error());
    for (const auto& planet : galaxy.planets) {
        gc::PlanetControl prior; prior.planet = planet.id; prior.current_player = planet.owner;
        presence.state.push_back(prior);
    }
    const auto inputs = presence;
    std::map<gc::PlanetId, std::vector<gc::Fleet>> orbit_by_planet;
    for (const auto& fleet : galaxy.fleets) orbit_by_planet[fleet.orbit].push_back(fleet);
    for (auto& [planet, orbit] : orbit_by_planet) {
        (void)planet;
        std::stable_sort(orbit.begin(), orbit.end(), [](const auto& a, const auto& b) {
            return a.motion.orbit_entry < b.motion.orbit_entry;
        });
    }
    for (std::size_t i = 0; i < inputs.planets.size(); ++i) {
        auto calculated = gc::calculate_planet_control(inputs, inputs.planets[i], inputs.state[i], orbit_by_planet[inputs.planets[i].planet],
            galaxy.movement ? &*galaxy.movement : nullptr, {}, 0);
        if (!calculated) return core::Result<Out>::failure(calculated.error());
        presence.state[i] = calculated.value().state;
        candidate.planets[i].owner = presence.state[i].current_player;
    }
    candidate.control = presence;
    if (auto valid = gc::validate_control_setup(candidate); !valid)
        return core::Result<Out>::failure(valid.error());
    return core::Result<Out>::success(std::move(presence));
}
core::Result<sim::galactic::PoliticalControlSetup> bind_control_data(
    const data::galactic::GalacticData& data, const units::GalacticTypeTable& types,
    sim::galactic::PoliticalControlSetup presence, const std::span<const ControlTypeBinding> objects,
    const std::span<const ControlObjectBinding> planets, const std::span<const ControlObjectBinding> routes) {
    using Out = sim::galactic::PoliticalControlSetup;
    const auto fail = [](const std::string& message, const std::string& path = {}) {
        core::Diagnostic error; error.code = std::string(sim::galactic::diagnostic_codes::invalid_setup);
        error.severity = core::Severity::error; error.logical_path = path; error.message = message;
        return core::Result<Out>::failure(std::move(error));
    };
    const auto* duration = data.constant("Political_Control_Change_Time_Seconds");
    const auto* limit = data.constant("Max_Ground_Forces_On_Planet");
    if (!duration || !duration->number || duration->number->raw() <= 0
        || !limit || !limit->number || limit->number->raw() <= 0
        || limit->number->raw() % sim::math::Fixed::scale != 0
        || limit->number->trunc_to_integer() > static_cast<std::int64_t>(sim::galactic::galactic_max_objects))
        return fail("missing or invalid authored political transition duration or surface limit");
    presence.change_seconds = *duration->number;
    presence.max_ground_forces = static_cast<std::uint32_t>(limit->number->trunc_to_integer());
    presence.types.clear();
    sim::galactic::TypeId previous{};
    for (const auto& binding : objects) {
        const auto resolved = units::resolve_initial_galactic_type(types, binding.original_type);
        if (!resolved) return core::Result<Out>::failure(resolved.error());
        const auto* type = &resolved.value();
        if (binding.id <= previous || !type || !type->political_control
            || (type->political_control->raw() != 0 && type->political_control->raw() != sim::math::Fixed::scale))
            return fail(binding.original_type + ": missing or invalid orbital Political_Control", type ? type->file : std::string{});
        previous = binding.id;
        presence.types.push_back({binding.id, static_cast<std::uint8_t>(type->political_control->trunc_to_integer()), binding.skips_combat, binding.ground_capable});
    }
    if (planets.size() != presence.planets.size() || routes.size() != presence.routes.size())
        return fail("control planet/route bindings must cover runtime presence");
    for (std::size_t i = 0; i < planets.size(); ++i) {
        const auto* planet = data.planet(planets[i].original_type);
        if (!planet || planets[i].id != presence.planets[i].planet || !planet->political_control
            || !planet->planet_capture_bonus_reward || *planet->planet_capture_bonus_reward < 0)
            return fail(planets[i].original_type + ": missing political leaning or capture reward", planet ? planet->file : std::string{});
        auto reward = sim::math::Fixed::from_integer(*planet->planet_capture_bonus_reward);
        if (!reward) return fail(planets[i].original_type + ": capture reward overflows", planet->file);
        presence.planets[i].leaning = *planet->political_control;
        presence.planets[i].capture_reward = reward.value();
        // WGS-92: the shared constructor retains residual influence by default.
        presence.planets[i].retains_residual = planet->planet_retains_residual_influence.value_or(true);
    }
    for (std::size_t i = 0; i < routes.size(); ++i) {
        const auto* route = data.trade_route(routes[i].original_type);
        if (!route || routes[i].id != presence.routes[i].route || !route->political_control_gain)
            return fail(routes[i].original_type + ": missing route Political_Control_Gain", route ? route->file : std::string{});
        presence.routes[i].political_gain = *route->political_control_gain;
    }
    return core::Result<Out>::success(std::move(presence));
}
} // namespace eawr::campaign
