#pragma once

#include <array>
#include <cmath>
#include <optional>

namespace eawr::viewer::camera_input {

// #5990: FoC keeps the cursor where a middle-button camera drag started and
// drives the camera from relative motion. This is only the state: the press
// position while a drag is held. The host warps the pointer back to anchor()
// each frame; the adapter ends the hold on every release or cancel, so a
// pointer can never stay pinned after the drag is gone.
// The hold exists only where the host can warp the pointer back (Windows);
// elsewhere a drag neither holds the pointer nor suppresses edge scrolling.
#ifdef _WIN32
inline constexpr bool pointer_hold_supported = true;
#else
inline constexpr bool pointer_hold_supported = false;
#endif

// What the host does with the pointer this frame.
enum class HoldStep { none, warp, release };

// While holding: warp back if the pointer feed is allowed, else release the
// drag (focus loss, minimize, menu, modal). Without a hold: nothing.
[[nodiscard]] constexpr HoldStep hold_step(const bool holding, const bool feed_allowed) noexcept {
    if (!holding) return HoldStep::none;
    return feed_allowed ? HoldStep::warp : HoldStep::release;
}

// The galaxy's shared drag hold (right-button scroll and middle-button drag). `dragging` is "a drag flag is
// set"; a hold with no drag flag is stale (a menu, dialog or panel cleared the flags) and must end, so the next
// press anchors afresh. A press begins a hold only in the overview (retail tracks the middle button only there)
// and only where the host can warp; a second button during a drag keeps the first anchor.
enum class DragHold { keep, end, begin };
[[nodiscard]] constexpr DragHold drag_hold_step(const bool dragging, const bool hold_active, const bool pressing,
    const bool overview, const bool supported) noexcept {
    if (!dragging) return hold_active ? DragHold::end : DragHold::keep;
    if (pressing && !hold_active && overview && supported) return DragHold::begin;
    return DragHold::keep;
}

class PointerHold final {
public:
    // Viewport coordinates of the press. A non-finite position starts no hold.
    void begin(const float x, const float y) noexcept {
        if (std::isfinite(x) && std::isfinite(y)) anchor_ = std::array<float, 2>{x, y};
        else anchor_.reset();
        if (anchor_) last_ = *anchor_;
    }
    void end() noexcept { anchor_.reset(); }
    // The host warped the pointer to anchor(): the next position is measured
    // from there.
    void note_warp() noexcept { if (anchor_) last_ = *anchor_; }
    // The drag's motion since the last call, from positions. Godot's relative
    // part is not used while held: it carries the warp's own move and merges
    // with real motion on a busy frame, but a position minus the position the
    // pointer was last known at is the user's motion either way. A warp echo
    // sits on the anchor and gives zero.
    [[nodiscard]] std::array<float, 2> delta(const float x, const float y) noexcept {
        const std::array<float, 2> moved{x - last_[0], y - last_[1]};
        last_ = {x, y};
        return moved;
    }
    [[nodiscard]] bool active() const noexcept { return anchor_.has_value(); }
    [[nodiscard]] const std::optional<std::array<float, 2>>& anchor() const noexcept { return anchor_; }

private:
    std::optional<std::array<float, 2>> anchor_;
    std::array<float, 2> last_{};
};

} // namespace eawr::viewer::camera_input
