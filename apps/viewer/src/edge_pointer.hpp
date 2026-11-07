#pragma once

#include "eawr/presentation/ui/input_routing.hpp"
#include <cmath>

namespace eawr::presentation::godot_backend {

enum class PointerWindowMode { windowed, fullscreen, exclusive_fullscreen };

[[nodiscard]] inline bool pointer_confined(const PointerWindowMode mode, const bool focused,
    const bool minimized, const bool blocked) noexcept {
    return mode != PointerWindowMode::windowed && focused && !minimized && !blocked;
}

[[nodiscard]] inline bool map_pointer_feed_allowed(const presentation::ui::PointerFeedState& feed,
    const bool focused, const bool menu_open, const bool blocked) noexcept {
    return focused && !menu_open && !blocked && presentation::ui::world_feeds_pointer_position(feed);
}

[[nodiscard]] inline bool galactic_pointer_feed_allowed(const bool world_allowed, const bool focused,
    const bool minimized, const bool modal_open) noexcept {
    return world_allowed && focused && !minimized && !modal_open;
}

template <typename Sample, typename Clear>
inline void route_pointer_sample(const bool allowed, const float x, const float y, Sample sample, Clear clear) {
    if (allowed && std::isfinite(x) && std::isfinite(y)) sample(x, y);
    else clear();
}

} // namespace eawr::presentation::godot_backend
