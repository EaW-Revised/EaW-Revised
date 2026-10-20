#pragma once

// Retained-state helpers for per-object galactic HUD updates. Engine-free, so a root contract test can check them
// without Godot. Two pieces are shared by the planet labels, the fleet stack badges and the ground roster:
//   * Retained<T>: remember what a control was last given and report whether a new value differs, so an unchanged
//     value costs no native call.
//   * HudBounds: the rectangle a planet's controls cover relative to the planet's screen point, so a planet whose
//     controls all lie outside the screen can be skipped before any text is measured.
// Both only ever remove work whose result would be identical: a "not culled" or "changed" answer is always safe.

#include <algorithm>
#include <bit>
#include <concepts>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace eawr::viewer::galactic {

// Remembers the last value handed to a control.
template <class T>
class Retained final {
public:
    // True when `next` differs from the remembered value (or none is remembered); the value is then remembered.
    bool set(const T& next) {
        if (value_ && *value_ == next) return false;
        value_ = next;
        return true;
    }
    void reset() noexcept { value_.reset(); }
    [[nodiscard]] const std::optional<T>& get() const noexcept { return value_; }
private:
    std::optional<T> value_;
};

// The screen, in pixels, origin at the top left.
struct HudView final {
    float width{};
    float height{};
    [[nodiscard]] bool contains(const float x, const float y) const noexcept {
        return x >= 0.0F && y >= 0.0F && x <= width && y <= height;
    }
};

// A rectangle relative to an anchor point (control positions minus the anchor).
struct HudBounds final {
    float min_x{}, min_y{}, max_x{}, max_y{};
    bool any{};
    // Covers a control placed at (x, y) of size (w, h) around an anchor at (anchor_x, anchor_y).
    void include(const float x, const float y, const float w, const float h,
        const float anchor_x, const float anchor_y) noexcept {
        const float x0 = x - anchor_x, y0 = y - anchor_y;
        if (!any) { min_x = x0; min_y = y0; max_x = x0 + w; max_y = y0 + h; any = true; return; }
        min_x = std::min(min_x, x0); min_y = std::min(min_y, y0);
        max_x = std::max(max_x, x0 + w); max_y = std::max(max_y, y0 + h);
    }
    // True only when every covered control, moved to `anchor`, lies fully outside `view` by at least `margin`.
    // An empty bound set covers nothing, so nothing of the planet can be on screen.
    [[nodiscard]] bool outside(const HudView& view, const float anchor_x, const float anchor_y,
        const float margin) const noexcept {
        if (!any) return true;
        return anchor_x + max_x + margin < 0.0F || anchor_y + max_y + margin < 0.0F
            || anchor_x + min_x - margin > view.width || anchor_y + min_y - margin > view.height;
    }
};

// #4315: what a label pass does with a planet that was placed before. `place` writes the controls from the current
// facts; `keep` leaves them (nothing they read changed); `keep_culled` leaves them although the planet's screen point
// moved, because they sat off screen and still would. Any difference outside the screen point, in the facts digest
// included, is `place`, so a change that lands while the controls are culled is written the next pass.
enum class HudPlacement { place, keep, keep_culled };
template <class Key>
[[nodiscard]] HudPlacement hud_placement(const Key& previous, const Key& next, const bool previous_culled,
    const HudBounds& reach, const HudView& view, const float anchor_x, const float anchor_y, const float margin) {
    if (previous == next) return HudPlacement::keep;
    if (previous_culled && previous.same_but_anchor(next) && reach.outside(view, anchor_x, anchor_y, margin))
        return HudPlacement::keep_culled;
    return HudPlacement::place;
}

// 64-bit FNV-1a, for the per-object change digests (no allocation, no locale). A digest is a 64-bit hash: equal
// digests make an equal input overwhelmingly likely, not certain.
class HudDigest final {
public:
    template <std::integral T>
    void add(const T value) noexcept { mix(static_cast<std::uint64_t>(value)); }
    template <class T> requires std::is_enum_v<T>
    void add(const T value) noexcept { mix(static_cast<std::uint64_t>(value)); }
    // The bytes are hashed, so a value that compares unequal only through NaN still digests the same run to run.
    void add(const double value) noexcept { mix(std::bit_cast<std::uint64_t>(value)); }
    void add(const float value) noexcept { add(static_cast<double>(value)); }
    void add(const std::string_view text) noexcept {
        mix(text.size());
        for (const char c : text) { state_ ^= static_cast<unsigned char>(c); state_ *= 1099511628211ULL; }
    }
    template <class T>
    void add(const std::optional<T>& value) noexcept {
        mix(value.has_value() ? 1U : 0U);
        if (value) add(*value);
    }
    [[nodiscard]] std::uint64_t value() const noexcept { return state_; }
private:
    void mix(const std::uint64_t value) noexcept {
        for (int shift = 0; shift < 64; shift += 8) {
            state_ ^= (value >> shift) & 0xFFU;
            state_ *= 1099511628211ULL;
        }
    }
    std::uint64_t state_{14695981039346656037ULL};
};

} // namespace eawr::viewer::galactic
