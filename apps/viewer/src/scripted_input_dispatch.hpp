#pragma once

namespace eawr::presentation::godot_backend {

// LAN button decoding must finish while the gesture owns its admission epoch.
// Standalone input retains the engine's normal buffered dispatch boundary.
template <typename Engine, typename Event, typename EpochScope>
void dispatch_scripted_input(Engine& engine, const Event& event, const EpochScope& input_epoch) {
    engine.parse_input_event(event);
    if (input_epoch && *input_epoch) engine.flush_buffered_events();
}

} // namespace eawr::presentation::godot_backend
