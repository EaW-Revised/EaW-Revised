#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace eawr::presentation::ui {

// WGUI-70/80: shared presentation timing. Consumers supply their observer,
// modal and tutorial admission gates; this owns no simulation state.
struct EncyclopediaTiming final {
    double delay{}, minimum{}, fade_rate{};
    double hovered_seconds{}, remaining{}, alpha{};
    std::uint64_t candidate{}, shown{};
    void service(std::uint64_t hovered, double elapsed, bool admitted) noexcept {
        if (!std::isfinite(elapsed) || elapsed < 0) return;
        if (!admitted) hovered = 0;
        if (hovered != candidate) { candidate = hovered; hovered_seconds = 0; }
        else if (candidate) hovered_seconds += elapsed;
        remaining = std::max(0.0, remaining - elapsed);
        if (candidate && hovered_seconds >= delay) {
            if (shown != candidate) { shown = candidate; remaining = minimum; alpha = 0; }
            alpha = std::min(1.0, alpha + fade_rate * elapsed);
        } else if (remaining == 0) {
            alpha = std::max(0.0, alpha - fade_rate * elapsed);
            if (alpha == 0) shown = 0;
        }
    }
};

} // namespace eawr::presentation::ui
