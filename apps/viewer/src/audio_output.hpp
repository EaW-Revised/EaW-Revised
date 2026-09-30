#pragma once

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/os.hpp>

namespace eawr::presentation::godot_backend {

// A GPU lane can require silent output without changing event selection or mixing.
[[nodiscard]] inline bool audio_output_muted() {
    return godot::OS::get_singleton()->get_environment("EAWR_AUDIO_MUTE") == godot::String("1");
}

inline void mute_lane_audio_output() {
    if (audio_output_muted()) godot::AudioServer::get_singleton()->set_bus_mute(0, true);
}

} // namespace eawr::presentation::godot_backend
