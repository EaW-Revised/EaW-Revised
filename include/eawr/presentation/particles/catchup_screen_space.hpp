#pragma once

// D-ss, "screen-space one-pass catch-up". A frame that owes two or more particle samples normally steps a ship's
// emitters twice: once at the first due tick's pose (the historical pass) and once at the drawn pose. Births in a
// stepped interval are placed along the straight chord from the emitter's last sampled pose to its new pose
// (CpuSystem::schedule_emitter), so dropping the historical pass only changes where they sit relative to the true
// curved path: by at most the sagitta of that path. This header decides, per ship and per frame, whether that
// difference reaches the screen. Everything here reads presentation data only (the two snapshots the frame
// already holds, the drawn camera), so the simulation and its hashes are not touched.
//
// Retail alignment: retail runs one particle update per rendered frame (PS-42: emitters consume the elapsed time of
// the pose just supplied; PS-10: births are placed from the emitter's motion over that interval).

#include "eawr/presentation/particles/render.hpp"
#include "eawr/sim/math/geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace eawr::presentation::particles {

// A ship keeps the historical pass once the predicted error reaches this many pixels, and returns to one pass when
// it falls below the lower bound. The gap only avoids flicker in the work counters: both modes draw continuous
// trails and differ by less than a pixel at the switch.
inline constexpr float catchup_enter_pixels = 0.5F;
inline constexpr float catchup_leave_pixels = 0.25F;

// The current frame's camera in particle space, reduced to what the projection needs.
struct CatchupView final {
    Vec3 position{};
    Vec3 forward{};
    Vec3 right{};
    Vec3 up{};
    float focal_pixels{1.0F};   // pixels per world unit at depth 1
    float tan_half_x{1.0F};     // horizontal half-angle tangent
    float tan_half_y{1.0F};
    float near_plane{1.0F};
};

[[nodiscard]] inline CatchupView catchup_view(const CameraFrame& camera, const float vertical_fov_degrees,
                                              const std::uint32_t width, const std::uint32_t height,
                                              const float near_plane) noexcept {
    CatchupView view;
    view.position = camera.position;
    view.right = camera.right;
    view.up = camera.up;
    // camera_frame_from_render: right = forward x up_hint, up = right x forward, so forward = up x right.
    view.forward = {camera.up.y * camera.right.z - camera.up.z * camera.right.y,
                    camera.up.z * camera.right.x - camera.up.x * camera.right.z,
                    camera.up.x * camera.right.y - camera.up.y * camera.right.x};
    const float half = std::tan(vertical_fov_degrees * 0.5F * 3.14159265358979F / 180.0F);
    const float rows = static_cast<float>(std::max<std::uint32_t>(height, 1U));
    view.tan_half_y = std::max(half, 1.0e-6F);
    view.tan_half_x = view.tan_half_y * static_cast<float>(std::max<std::uint32_t>(width, 1U)) / rows;
    view.focal_pixels = rows * 0.5F / view.tan_half_y;
    view.near_plane = near_plane;
    return view;
}

struct CatchupProjection final {
    float depth{};      // distance along the view direction to the nearest point of the bounding sphere (>= near)
    bool culled{};      // the whole sphere lies outside the view volume's sides or behind the camera
};

[[nodiscard]] inline float catchup_dot(const Vec3& left, const Vec3& right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

// `origin` and `radius` bound every emitter of the ship (the ship's origin and model bounds in particle space).
[[nodiscard]] inline CatchupProjection catchup_project(const CatchupView& view, const Vec3& origin,
                                                       const float radius) noexcept {
    const Vec3 offset{origin.x - view.position.x, origin.y - view.position.y, origin.z - view.position.z};
    const float depth = catchup_dot(offset, view.forward);
    const float x = catchup_dot(offset, view.right);
    const float y = catchup_dot(offset, view.up);
    // Side planes: a sphere is outside when its centre is farther than `radius` beyond a plane. The plane
    // normal (1, -tan) / sqrt(1 + tan^2) turns that into x - depth * tan > radius * sqrt(1 + tan^2).
    const float side_x = radius * std::sqrt(1.0F + view.tan_half_x * view.tan_half_x);
    const float side_y = radius * std::sqrt(1.0F + view.tan_half_y * view.tan_half_y);
    CatchupProjection result;
    result.depth = std::max(depth - radius, view.near_plane);
    result.culled = !(depth + radius > view.near_plane)
        || std::fabs(x) - depth * view.tan_half_x > side_x || std::fabs(y) - depth * view.tan_half_y > side_y;
    return result;
}

// Predicted screen error (pixels) of replacing the historical pass by one pass. `chord` is the distance the
// ship's origin moves over the frame, `turn` the angle (radians) its heading turns over the frame, `lever` the
// largest distance from the origin to an emitter. The ship's origin follows an arc whose sagitta is chord * turn / 8;
// an emitter at the end of a lever arm adds the sagitta of its own arc, lever * turn^2 / 8.
[[nodiscard]] inline float catchup_error_pixels(const float chord, const float turn, const float lever,
                                                const float depth, const float focal_pixels) noexcept {
    if (!(depth > 0.0F)) return std::numeric_limits<float>::infinity();
    return (chord * turn + lever * turn * turn) * 0.125F * focal_pixels / depth;
}

// True when the ship must keep the historical pass. `per_tick` is last frame's decision (the hysteresis state).
// A culled ship never needs it; a non-finite error keeps it (the conservative answer).
[[nodiscard]] inline bool catchup_keeps_historical_pass(const bool per_tick, const float error_pixels,
                                                        const bool culled) noexcept {
    if (culled) return false;
    return per_tick ? !(error_pixels < catchup_leave_pixels) : !(error_pixels < catchup_enter_pixels);
}

// What one ship's decision needs: where its emitters can be (origin and the radius of a sphere that holds them),
// how far it moves and turns per tick between the two snapshots, and the last decision (hysteresis state).
struct CatchupShip final {
    Vec3 origin{};
    float lever{};            // radius (world units) from the origin that holds every emitter of the ship
    float chord_per_tick{};
    float turn_per_tick{};    // radians
    bool per_tick{};          // last frame's decision
};

struct CatchupDecision final {
    bool keep{};              // keep the historical pass
    bool culled{};
    float error_pixels{};
};

// The whole per-ship decision. A culled ship takes one pass and its hysteresis state is cleared (a ship that leaves
// and re-enters the view starts from "one pass").
[[nodiscard]] inline CatchupDecision decide_catchup_ship(const CatchupView& view, const CatchupShip& ship,
                                                         const std::uint64_t ticks_owed) noexcept {
    const auto projection = catchup_project(view, ship.origin, ship.lever);
    CatchupDecision decision;
    decision.culled = projection.culled;
    if (projection.culled) return decision;
    const float ticks = static_cast<float>(ticks_owed);
    decision.error_pixels = catchup_error_pixels(ship.chord_per_tick * ticks, ship.turn_per_tick * ticks, ship.lever,
                                                 projection.depth, view.focal_pixels);
    decision.keep = catchup_keeps_historical_pass(ship.per_tick, decision.error_pixels, false);
    return decision;
}

// Per multi-sample frame, over ships: kept the historical pass, took one pass, were culled (one pass), or had no
// snapshot pair / bounds ("unknown": they keep the pass). `error_bins` holds the error of the ships that were not
// culled, in bins <1/16, <1/8, <1/4, <1/2, <1, <2, <4, <8, >=8 px; the three decision counters sum to its total.
struct CatchupCensus final {
    std::uint64_t frames{};
    std::uint64_t per_tick{};
    std::uint64_t single_pass{};
    std::uint64_t culled{};
    std::uint64_t unknown{};
    std::array<std::uint64_t, 9> error_bins{};

    void record(const CatchupDecision& decision) noexcept {
        if (decision.culled) { ++culled; return; }
        std::size_t bin = 0;
        for (float edge = 0.0625F; bin + 1 < error_bins.size() && !(decision.error_pixels < edge); edge *= 2.0F) ++bin;
        ++error_bins[bin];
        ++(decision.keep ? per_tick : single_pass);
    }
    [[nodiscard]] std::uint64_t decisions() const noexcept { return per_tick + single_pass + culled + unknown; }
    [[nodiscard]] std::uint64_t binned() const noexcept {
        std::uint64_t total = 0;
        for (const auto count : error_bins) total += count;
        return total;
    }
};

// The angle (radians) between two fixed-point placements' rotations; the columns are normalised first, so a
// uniform scale does not matter.
[[nodiscard]] inline float catchup_turn_radians(const sim::math::Mat3x4& before, const sim::math::Mat3x4& after) noexcept {
    const auto value = [](const sim::math::Fixed fixed) {
        return static_cast<double>(fixed.raw()) / static_cast<double>(sim::math::Fixed::scale);
    };
    double trace = 0.0;
    for (std::size_t column = 0; column < 3; ++column) {
        double dot = 0.0, left = 0.0, right = 0.0;
        for (std::size_t row = 0; row < 3; ++row) {
            const double a = value(before.rows[row][column]);
            const double b = value(after.rows[row][column]);
            dot += a * b;
            left += a * a;
            right += b * b;
        }
        const double norm = std::sqrt(left * right);
        trace += norm > 0.0 ? dot / norm : 1.0;
    }
    return static_cast<float>(std::acos(std::clamp((trace - 1.0) * 0.5, -1.0, 1.0)));
}

} // namespace eawr::presentation::particles
