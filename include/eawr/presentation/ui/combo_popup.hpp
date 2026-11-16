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
    // A rectangle that cannot hold one complete row describes only a closed box (a combo whose
    // resource height is its text box, or barely more): the list keeps the viewport-bounded fallback.
    if (list_height > 0.0 && rows < 1.0) return combo_popup_height(0.0, scale, frame_height, row_height, viewport_height);
    return static_cast<int>(std::min(limit, std::max(1.0, std::ceil((frame + rows * row_height) * scale))));
}

// UI-COMBO-01: a list budget that holds at least one complete row bounds the popup, and the popup keeps
// that height whatever the list length (the original keeps the authored box, blank below a short list).
// Without such a budget (a closed-box rectangle) the popup shrinks to its list.
[[nodiscard]] inline bool combo_popup_has_budget(const double list_height, const double scale,
                                                 const double frame_height, const double row_height) {
    if (!(scale > 0.0) || !(row_height > 0.0) || !(list_height > 0.0)) return false;
    return std::floor((list_height - std::max(0.0, frame_height)) / row_height) >= 1.0;
}

} // namespace eawr::presentation::ui
