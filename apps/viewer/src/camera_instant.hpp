#pragma once

// #6448 J1: the camera reads the same presentation instant as the meshes.
//
// `CameraInstant` is a local stand-in with the field set of the presentation clock's
// per-frame instant (one value per battle frame, built before any consumer, read-only
// afterwards). It is replaced by an alias to the clock's own type when that lands; only
// the include and the alias change. Nothing in the viewer builds one yet: this header and
// its test fix the camera-side contract (what a camera reads, which dt it integrates, what
// it resets) before any product code is switched over.
//
// Status: the seams below are a MODEL of the camera frame order for the harness
// (tests/presentation/camera/camera_instant_main.cpp). No viewer source includes this header; it is not what the
// viewer runs, and the harness is not a regression test of the shipped camera code.
//
// Engine-free: no Godot, no simulation headers beyond a forward declaration.

#include "battlecam_shots.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

namespace eawr::sim::tactical {
class TacticalSnapshot;
}

namespace eawr::presentation::godot_backend {

enum class PacingMode : std::uint8_t { free, fifo_locked, fifo_unlocked };

struct CameraInstant final {
    std::int64_t t_frame_us{};
    // The display clock's estimate of how long this frame stays on screen. It keeps running
    // while the game is paused or its speed changes: a free camera, free flight and the
    // overview still move then. Only cursor/base_tick/alpha freeze with the game clock.
    double dt_present_ms{};
    double cursor{};
    std::uint64_t base_tick{};
    double alpha{};
    std::shared_ptr<const sim::tactical::TacticalSnapshot> previous;
    std::shared_ptr<const sim::tactical::TacticalSnapshot> next;
    bool paused{};
    bool speed_changed{};
    bool discontinuity{};
    PacingMode pacing_mode{PacingMode::free};

    // The only dt a camera integrates (never the engine's delta, never a raw timestamp difference).
    [[nodiscard]] double dt_seconds() const noexcept { return dt_present_ms * 1.0e-3; }
};

// BCAM-16: a battlecam shot holds its clock while the battle is paused; every other camera
// integrator keeps taking dt_seconds().
[[nodiscard]] inline double battlecam_elapsed_after(double elapsed, const CameraInstant& instant) noexcept {
    return instant.paused ? elapsed : elapsed + std::max(instant.dt_seconds(), 0.0);
}

using Vec3d = std::array<double, 3>;
using Mat3d = std::array<std::array<double, 3>, 3>;

struct CameraPose final {
    Vec3d eye{};
    Vec3d target{};
};

// The shot arithmetic of the battlecam's ordinary, non-hardpoint, non-frigate path: the eye
// is the subject (or, for a floating shot, its start anchor) plus the shot offset rotated by
// the subject's drawn rotation; the aim is the subject plus the target offset. Inputs are the
// pose the subject's mesh is drawn with in the same frame (see the test for the ordering).
[[nodiscard]] inline CameraPose battlecam_shot_camera(const BattlecamShot& shot, const double elapsed,
                                                      const Vec3d& subject, const Mat3d& rotation,
                                                      const Vec3d& floating_anchor) noexcept {
    const auto offset = shot.at(static_cast<float>(elapsed));
    const bool floating = shot.kind == BattlecamShotKind::floating;
    CameraPose pose;
    for (std::size_t row = 0; row < 3; ++row) {
        pose.eye[row] = floating ? floating_anchor[row] : subject[row];
        pose.target[row] = floating ? floating_anchor[row] : subject[row];
        for (std::size_t col = 0; col < 3; ++col) {
            pose.eye[row] += floating ? (row == col ? static_cast<double>(offset[col]) : 0.0)
                                      : rotation[row][col] * static_cast<double>(offset[col]);
            pose.target[row] += rotation[row][col] * static_cast<double>(shot.target[col]);
        }
    }
    return pose;
}

// BCAM-19 unit preview (the C key): the eye orbits the subject at a user-chosen distance, azimuth and elevation in
// the subject's own frame; the aim is the subject. Same inputs as the preview branch of the viewer's shot pose.
[[nodiscard]] inline CameraPose battlecam_preview_camera(const Vec3d& subject, const Mat3d& rotation, const float distance,
                                                         const float azimuth, const float elevation) noexcept {
    const auto offset = preview_offset(distance, azimuth, elevation);
    CameraPose pose;
    for (std::size_t row = 0; row < 3; ++row) {
        pose.eye[row] = subject[row];
        pose.target[row] = subject[row];
        for (std::size_t col = 0; col < 3; ++col) pose.eye[row] += rotation[row][col] * static_cast<double>(offset[col]);
    }
    return pose;
}

// BCAM-21: the eye sits at the weapon's attachment point displaced by the fire-direction
// offset; the aim is the weapon's target. nullopt-equivalent: a degenerate direction gives
// the attachment point itself.
[[nodiscard]] inline CameraPose battlecam_hardpoint_camera(const Vec3d& attachment, const Vec3d& fire_forward,
                                                           const Vec3d& target) noexcept {
    const auto offset = hardpoint_camera_offset({static_cast<float>(fire_forward[0]),
        static_cast<float>(fire_forward[1]), static_cast<float>(fire_forward[2])});
    CameraPose pose;
    for (std::size_t i = 0; i < 3; ++i) {
        pose.eye[i] = attachment[i] + (offset ? static_cast<double>((*offset)[i]) : 0.0);
        pose.target[i] = target[i];
    }
    return pose;
}

// BCAM-22: a frigate shot flies the eye start -> middle -> end across two ships while the aim
// slides from the subject to its target. `t` is the shot's fraction in [0, 1].
[[nodiscard]] inline CameraPose battlecam_frigate_camera(const Vec3d& start, const Vec3d& middle, const Vec3d& end,
                                                         const Vec3d& subject, const Vec3d& target,
                                                         const double t) noexcept {
    const double fraction = std::clamp(t, 0.0, 1.0);
    const double phase = fraction < 0.5 ? fraction * 2.0 : (fraction - 0.5) * 2.0;
    CameraPose pose;
    for (std::size_t i = 0; i < 3; ++i) {
        const double from = fraction < 0.5 ? start[i] : middle[i];
        const double to = fraction < 0.5 ? middle[i] : end[i];
        pose.eye[i] = from + (to - from) * phase;
        pose.target[i] = subject[i] + (target[i] - subject[i]) * fraction;
    }
    return pose;
}

// Chase-type smoothing: the scripted follow group's focus eases toward the group's centre with
// a time constant of 20 ticks of game time (the same constant BattleInput::follow uses today).
// A clock rebase (`discontinuity`) is one visible jump, never an ease across it; a pause adds
// no ticks, so it holds on its own.
struct FollowEase final {
    bool started{};
    std::array<double, 2> focus{};
    double tick{};
};

inline void follow_ease_step(FollowEase& state, const std::array<double, 2>& wanted, const CameraInstant& instant) noexcept {
    if (!state.started || instant.discontinuity) {
        state.started = true;
        state.focus = wanted;
    } else {
        const double ease = 1.0 - std::exp(-std::max(0.0, instant.cursor - state.tick) / 20.0);
        state.focus[0] += (wanted[0] - state.focus[0]) * ease;
        state.focus[1] += (wanted[1] - state.focus[1]) * ease;
    }
    state.tick = instant.cursor;
}

} // namespace eawr::presentation::godot_backend
