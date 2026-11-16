#pragma once
// Hands the process-wide diagnostic switches (--eawr-diagnostic-off) to the presentation modules that read them
// through their own state rather than through diagnostic_switches::active. The shared viewer host calls this once at
// startup and names no module itself (the effect-mode test keeps particle types out of the host).
#include "eawr/presentation/diagnostic_switches.hpp"
#include "eawr/presentation/particles/render.hpp"

namespace eawr::presentation::godot_backend {

inline void apply_diagnostic_switch_hooks(const diagnostic_switches::Switches& switches) noexcept {
    // particles: no particle system is stepped, culled, built or drawn through any registry call.
    particles::EffectRegistry::set_diagnostic_inert(switches.particles);
}

} // namespace eawr::presentation::godot_backend
