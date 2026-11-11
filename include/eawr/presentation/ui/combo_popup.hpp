#pragma once

#include <algorithm>
#include <cmath>

namespace eawr::presentation::ui {

// UI-COMBO-01: the authored list budget includes the frame and complete rows.
// Inputs are local UI units except for the viewport and the returned pixel height.
[[nodiscard]] inline int combo_popup_height(const double list_height, const double scale,
                                            const double frame_height, const double row_height,
                                            const double viewport_height) {
    if (!(scale > 0.0) || !(row_height > 0.0) || !(viewport_height > 0.0)) return 1;
    const double limit = std::max(1.0, std::floor(std::min(
        list_height > 0.0 ? list_height * scale : viewport_height, viewport_height)));
    const double frame = std::max(0.0, frame_height);
    const double rows = std::max(0.0, std::floor((limit / scale - frame) / row_height));
    return static_cast<int>(std::min(limit, std::max(1.0, std::ceil((frame + rows * row_height) * scale))));
}

} // namespace eawr::presentation::ui
