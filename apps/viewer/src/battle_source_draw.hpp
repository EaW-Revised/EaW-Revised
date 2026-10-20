#pragma once

#include "space_populate.hpp"
#include <vector>

namespace eawr::presentation::godot_backend {

// Current source implementations bind space art. Keep its complete renderer type
// out of the source interface and shared setup-state declarations.
struct BattleDrawBindings final {
    std::vector<SpacePopulation::Options::PlacedShip> placed_ships;
};

} // namespace eawr::presentation::godot_backend
