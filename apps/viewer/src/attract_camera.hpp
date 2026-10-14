#pragma once

#include "eawr/skirmish/attract.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace eawr::presentation::godot_backend {

// What the attract camera may read of the demo battle: the render-basis positions (x, up, -y) of the
// ships in it, and whether the battle is held. Read-only; the camera never touches the simulation.
struct AttractBattleView final {
    std::span<const std::array<float, 3>> ships;
    bool paused{};
};

// Where the attract's camera looks from, in the render basis.
struct AttractCameraPose final {
    std::array<float, 3> eye{};
    std::array<float, 3> target{};

    friend bool operator==(const AttractCameraPose&, const AttractCameraPose&) = default;
};

// The main-menu attract's camera seam (owner 10-11: retail runs FoC's battle camera, #3005, which is
// not built). update(dt, battle) -> pose. This PLACEHOLDER starts at the map's saved camera
// (MAT-10) and then keeps it slowly alive: the look-at drifts toward the centroid of the ships, the
// view orbits slowly and the pitch and distance breathe a little. A real battle camera replaces
// this class behind the same call.
class AttractCamera final {
public:
    explicit AttractCamera(const skirmish::AttractCamera& saved);

    // Advances `delta` seconds of presentation time (no change while the battle is paused).
    [[nodiscard]] AttractCameraPose update(double delta, const AttractBattleView& battle);
    // The pose the last update (or the saved camera, before any) gave.
    [[nodiscard]] const AttractCameraPose& pose() const noexcept { return pose_; }
    // False when the saved values had no valid frame.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    void place();

    bool valid_{};
    double elapsed_{};
    std::array<float, 3> saved_target_{};
    std::array<float, 3> target_{};
    float distance_{};
    float pitch_{};
    float yaw_{};
    AttractCameraPose pose_;
};

} // namespace eawr::presentation::godot_backend
