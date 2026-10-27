#include "eawr/campaign/control.hpp"
#include "eawr/core/text.hpp"

#include <limits>
#include <algorithm>
#include <map>

namespace eawr::campaign {
namespace {
void political_note(core::LoadNoteBuffer* notes, std::string_view code, std::string_view object,
    std::string_view file, std::string_view tag, std::string authored, std::string effective,
    std::string message, core::LoadCategory category = core::LoadCategory::unknown_degrade,
    core::LoadAction action = core::LoadAction::default_used) {
    if (!notes) return;
    core::LoadNote note; note.code = code; note.object = object; note.file = file; note.tag = tag;
    note.authored_value = std::move(authored); note.effective_value = std::move(effective);
    note.classification = category; note.action = action;
    note.diagnostic_code = sim::galactic::diagnostic_codes::invalid_setup; note.message = std::move(message);
    if (category == core::LoadCategory::retail_accepts) note.retail = "LSA-19: signed integer sum, then planet clamp -1..1";
    notes->append(std::move(note));
}
} // namespace
core::Result<sim::math::Fixed> prepare_control_alignment_multiplier(
    sim::math::Fixed strength, bool like, sim::math::Fixed maximum) {
    const auto one = sim::math::Fixed::from_raw(sim::math::Fixed::scale);
    auto effect = sim::math::multiply(strength, maximum);
    if (!effect) return effect;
    if (effect.value().raw() <= 0) return core::Result<sim::math::Fixed>::success(one);
    return like ? sim::math::subtract(one, effect.value()) : sim::math::add(one, effect.value());
}

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
    const sim::galactic::GalacticSetup& galaxy, sim::galactic::PoliticalControlSetup presence, core::LoadNoteBuffer* notes) {
    namespace gc = sim::galactic;
    using Out = gc::PoliticalControlSetup;
    // A root start cannot reuse a prior allegiance/capture/death journal.
    if (!presence.state.empty() || !presence.captured_types.empty() || !presence.absent_forces.empty()) {
        core::Diagnostic error; error.code = std::string(gc::diagnostic_codes::invalid_setup);
        error.message = "initial political control requires fresh placement presence";
        return core::Result<Out>::failure(std::move(error));
    }
    presence.ability_sources.clear();
    if (galaxy.census) for (const auto& actor : galaxy.census->objects) gc::update_control_actor(presence, actor);
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
        if (notes) {
            core::LoadNoteCollector collector; collector.merge(calculated.value().notes);
            for (auto note : collector.snapshot()) { note.phase = core::LoadPhase::load; notes->append(std::move(note)); }
        }
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
    const std::span<const ControlObjectBinding> planets, const std::span<const ControlObjectBinding> routes,
    core::LoadNoteBuffer* notes) {
    using Out = sim::galactic::PoliticalControlSetup;
    const auto fail = [](const std::string& message, const std::string& path = {}) {
        core::Diagnostic error; error.code = std::string(sim::galactic::diagnostic_codes::invalid_setup);
        error.severity = core::Severity::error; error.logical_path = path; error.message = message;
        return core::Result<Out>::failure(std::move(error));
    };
    const auto* duration = data.constant("Political_Control_Change_Time_Seconds");
    const auto* limit = data.constant("Max_Ground_Forces_On_Planet");
    // Unknown consumers: a one-second transition and one surface slot are the
    // documented inert minimums, not retail defaults (LSA-20/21).
    const auto one = sim::math::Fixed::from_raw(sim::math::Fixed::scale);
    presence.change_seconds = duration && duration->number ? *duration->number : one;
    sim::math::Fixed denominator{}, delta{};
    if (presence.change_seconds.raw() <= 0
        || !sim::math::try_multiply(presence.change_seconds, sim::math::Fixed::from_raw(30 * sim::math::Fixed::scale), denominator)
        || !sim::math::try_divide(one, denominator, delta) || delta.raw() <= 0) {
        political_note(notes, "EAWR-LOAD-1101", "GameConstants", {}, "Political_Control_Change_Time_Seconds",
            duration ? duration->text : "omitted", "1", "missing or invalid authored political transition duration or surface limit");
        presence.change_seconds = one;
    } else if (!duration || !duration->number) {
        political_note(notes, "EAWR-LOAD-1101", "GameConstants", {}, "Political_Control_Change_Time_Seconds",
            duration ? duration->text : "omitted", "1", "missing or invalid authored political transition duration or surface limit");
    }
    const auto authored_limit = limit && limit->number ? limit->number->trunc_to_integer() : 1;
    std::size_t placed = 1;
    for (const auto& planet : presence.planets) placed = std::max(placed, planet.landed.size());
    presence.max_ground_forces = static_cast<std::uint32_t>(std::clamp(authored_limit,
        static_cast<std::int64_t>(std::min(placed, sim::galactic::galactic_max_objects)),
        static_cast<std::int64_t>(sim::galactic::galactic_max_objects)));
    if (!limit || !limit->number || limit->number->raw() % sim::math::Fixed::scale != 0
        || authored_limit != presence.max_ground_forces)
        political_note(notes, "EAWR-LOAD-1102", "GameConstants", {}, "Max_Ground_Forces_On_Planet", limit ? limit->text : "omitted",
            std::to_string(presence.max_ground_forces), "missing or invalid authored political transition duration or surface limit");
    presence.types.clear();
    presence.ability_types.clear(); presence.ability_sources.clear();
    sim::galactic::TypeId previous{};
    for (const auto& binding : objects) {
        const auto resolved = units::resolve_initial_galactic_type(types, binding.original_type);
        if (!resolved) return core::Result<Out>::failure(resolved.error());
        const auto* type = &resolved.value();
        if (binding.id <= previous) return fail("control type bindings require ordered nonzero identities");
        previous = binding.id;
        const auto value = type->political_control.value_or(sim::math::Fixed{}).trunc_to_integer();
        sim::galactic::ControlType control_type{binding.id, static_cast<std::uint8_t>(value == 1), binding.skips_combat, binding.ground_capable};
        if (value != 0 && value != 1) control_type.signed_political_control = value;
        if (!type->political_control || *type->political_control != sim::math::Fixed::from_raw(control_type.political_control * sim::math::Fixed::scale))
            political_note(notes, "EAWR-LOAD-1100", type->name, type->file, "Political_Control",
                type->political_control ? std::to_string(type->political_control->raw()) : "omitted", std::to_string(value),
                binding.original_type + ": missing or invalid orbital Political_Control",
                type->political_control ? core::LoadCategory::retail_accepts : core::LoadCategory::unknown_degrade,
                type->political_control ? core::LoadAction::accepted : core::LoadAction::default_used);
        presence.types.push_back(control_type);
        sim::galactic::ControlAbilityType abilities; abilities.type = binding.id;
        for (const auto& ability : type->control_abilities) {
            if (!ability.initially_enabled || !core::iequals(ability.activation_style, "Galactic_Automatic")) {
                political_note(notes, "EAWR-LOAD-1103", type->name, type->file, "Political_Ability", "unqualified activation", "skip ability",
                    binding.original_type + ": automatic_control_activation_unqualified", core::LoadCategory::unknown_degrade, core::LoadAction::skipped_object);
                continue;
            }
            sim::galactic::ControlAbility row;
            row.protection = ability.protection; row.enabled = *ability.initially_enabled;
            if (!row.protection) {
                if (!ability.multiplier || !ability.category || *ability.category < std::numeric_limits<std::int32_t>::min()
                    || *ability.category > std::numeric_limits<std::int32_t>::max())
                {
                    political_note(notes, "EAWR-LOAD-1103", type->name, type->file, "Political_Ability", "invalid quantity", "skip ability",
                        binding.original_type + ": missing or invalid political transition ability quantity", core::LoadCategory::unknown_degrade, core::LoadAction::skipped_object);
                    continue;
                }
                // WGM-197: this tag is a direct time multiplier; invalid ranges become zero.
                row.multiplier = *ability.multiplier >= sim::math::Fixed{} && *ability.multiplier < sim::math::Fixed::from_raw(sim::math::Fixed::scale)
                    ? *ability.multiplier : sim::math::Fixed{};
                row.category = static_cast<std::uint32_t>(*ability.category);
            }
            abilities.abilities.push_back(row);
        }
        if (!abilities.abilities.empty()) presence.ability_types.push_back(std::move(abilities));
    }
    if (planets.size() != presence.planets.size() || routes.size() != presence.routes.size())
        return fail("control planet/route bindings must cover runtime presence");
    for (std::size_t i = 0; i < planets.size(); ++i) {
        const auto* planet = data.planet(planets[i].original_type);
        if (!planet || planets[i].id != presence.planets[i].planet || !planet->political_control)
            return fail(planets[i].original_type + ": missing political leaning", planet ? planet->file : std::string{});
        // RGC-200: the reward is a signed integer kept as authored; an absent tag is 0 (WGS-90).
        auto reward = sim::math::Fixed::from_integer(planet->planet_capture_bonus_reward.value_or(0));
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
