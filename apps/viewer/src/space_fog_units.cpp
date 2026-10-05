#include "space_fog_units.hpp"

#include "family_textures.hpp"

#include "eawr/core/diagnostic.hpp"
#include "eawr/presentation/animation/animation.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <map>
#include <utility>

namespace eawr::presentation::godot_backend {


void SpaceFogUnits::release(GodotRenderer& renderer) {
    for (const sim::AssetId asset : uploaded_) static_cast<void>(renderer.release(asset));
    uploaded_.clear();
    instances_.clear();
}

} // namespace eawr::presentation::godot_backend
