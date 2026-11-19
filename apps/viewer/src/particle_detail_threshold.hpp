#pragma once

#include "eawr/data/xml_fwd.hpp"

namespace eawr::presentation::godot_backend {

// PS-45: locale-independent binary32 threshold, inherited/default value included.
[[nodiscard]] float particle_detail_threshold(const data::EffectiveObject& object);

} // namespace eawr::presentation::godot_backend
