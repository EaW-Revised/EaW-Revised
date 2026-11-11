#pragma once
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/array.hpp>

namespace eawr::presentation::godot_backend {

// Loading-only vertex layouts shared by production prewarm and the headless
// engine contract probe. No shader, viewport or draw is needed to build them.
[[nodiscard]] godot::Array prewarm_mesh_arrays(bool skinned);
[[nodiscard]] godot::BitField<godot::RenderingServer::ArrayFormat> prewarm_mesh_format();

} // namespace eawr::presentation::godot_backend
