#pragma once

#include "eawr/presentation/audio/sfx.hpp"

namespace eawr::presentation::audio {

struct HealthWarningCues {
    const SfxEvent* low{};
    const SfxEvent* critical{};
    const SfxEvent* enemy_low{};
    const SfxEvent* enemy_critical{};
};

// SND-33 / WCC-54: ordinary hull fractions at one damage delivery, not displayed
// hardpoint health. No retained flag: healing above a threshold re-arms it.
[[nodiscard]] inline const SfxEvent* health_warning_cue(const double before, const double after,
    const double low, const double critical, const bool target_local, const bool source_local,
    const HealthWarningCues& target, const HealthWarningCues& source) noexcept {
    if (!(after > 0.0 && after < before)) return nullptr;
    const bool crossed_critical = before > critical && after <= critical;
    const bool crossed_low = before > low && after <= low;
    if (!crossed_critical && !crossed_low) return nullptr;
    // Owner precedence includes a missing cue; no fallback to the source.
    if (target_local) return crossed_critical ? target.critical : target.low;
    if (source_local) return crossed_critical ? source.enemy_critical : source.enemy_low;
    return nullptr;
}

} // namespace eawr::presentation::audio
