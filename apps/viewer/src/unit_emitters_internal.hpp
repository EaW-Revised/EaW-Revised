#pragma once

#include "unit_emitters.hpp"

#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/particles/map_attachment_owner.hpp"
#include "eawr/presentation/particles/map_effect_plan.hpp"
#include "eawr/presentation/particles/prewarmed_capacity.hpp"
#include "eawr/presentation/particles/proxy_binding.hpp"
#include "eawr/presentation/space/debris.hpp"
#include "eawr/presentation/space/live_units.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace eawr::presentation::godot_backend::unit_emitters_detail {
using namespace godot;
namespace tactical = sim::tactical;


// #429 (BP-48): a death clone's proxy whose piece shows again is reset, as the debug
// build resets the group when its host shows again: its old drain goes.
constexpr auto clone_reappearance = particles::ReappearancePolicy::reset;
} // namespace eawr::presentation::godot_backend::unit_emitters_detail
