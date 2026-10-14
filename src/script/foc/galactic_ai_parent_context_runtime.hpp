#pragma once

#include "galactic_ai_world.hpp"

namespace eawr::script::foc::galactic_ai {
// WGA-80/81: bind before child creation/load, with the real parent's lifetime.
// The closure is derived, excluded from saves, and never advances either owner.
inline auto galactic_parent_evaluator(std::shared_ptr<const ai::Engine> manager,
    std::shared_ptr<const ai::GalacticWorld> world, sim::galactic::PlanetId conflict_planet) {
    return [manager = std::move(manager), world = std::move(world), conflict_planet]
        (std::string_view equation, sim::tactical::PlayerId player) -> std::optional<numeric::LuaNumber> {
        return ai::evaluate_in_galactic_context({manager.get(), world.get(), conflict_planet, true}, equation, player);
    };
}
} // namespace eawr::script::foc::galactic_ai
