#pragma once

#include "battlecam_chase.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>

namespace eawr::presentation::godot_backend {

enum class BattlecamShotKind { chase, circle, flyby, belly, floating, frigate };
struct BattlecamCategory final { bool fighter{}, bomber{}, corvette{}, frigate{}; };

// BCAM-21: use the current fire-bone forward direction to construct the
// zero-roll weapon frame. Vertical or degenerate directions cannot be framed.
[[nodiscard]] inline std::optional<std::array<float, 3>> hardpoint_camera_offset(std::array<float, 3> forward) noexcept {
    const float horizontal = std::hypot(forward[0], forward[1]);
    const float length = std::hypot(horizontal, forward[2]);
    if (!(horizontal > 1.0e-6F) || !(length > 1.0e-6F)) return std::nullopt;
    return std::array<float, 3>{-200.0F * forward[0] * forward[2] / (horizontal * length),
        -200.0F * forward[1] * forward[2] / (horizontal * length), 200.0F * horizontal / length};
}

// BCAM-19: preview uses eight 45-degree azimuths. The owner-requested
// framing and zoom range are expressed relative to the model bounds.
[[nodiscard]] inline std::array<float, 3> preview_offset(float distance, float azimuth, float elevation) noexcept {
    constexpr float radians = std::numbers::pi_v<float> / 180.0F;
    const float a = azimuth * radians, e = elevation * radians;
    return {distance * std::cos(e) * std::sin(a), -distance * std::cos(e) * std::cos(a), distance * std::sin(e)};
}
// #6569: the preview zoom range grows linearly with the model-bounds radius up to a frigate-sized ship (so every
// ship up to there keeps its #6455/#6499 framing), and with the square root of it beyond, so a large ship starts
// proportionally nearer and can be zoomed in much closer. The start and the zoom-out limit are also capped to a
// fraction of the playable map's smaller dimension (`map_extent`, 0 = unknown) so the default view never shows
// the whole map: at a 60 degree vertical field of view a 16:9 view spans about 2 x the distance.
inline constexpr float preview_knee_radius = 200.0F;
inline constexpr float preview_start_map_fraction = 0.2F;
inline constexpr float preview_max_map_fraction = 0.4F;
inline constexpr float preview_wheel_step_floor = 25.0F;
inline constexpr float preview_wheel_step_fraction = 0.05F;
struct PreviewZoom final {
    float start{}, min{}, max{};
};
[[nodiscard]] inline float preview_effective_radius(float radius) noexcept {
    return radius <= preview_knee_radius ? radius : std::sqrt(radius * preview_knee_radius);
}
[[nodiscard]] inline PreviewZoom preview_zoom(float radius, float map_extent = 0.0F) noexcept {
    const float effective = preview_effective_radius(radius);
    PreviewZoom zoom{5.0F * effective, 1.5F * effective, 10.0F * effective};
    if (map_extent > 0.0F) {
        zoom.start = std::min(zoom.start, preview_start_map_fraction * map_extent);
        zoom.max = std::min(zoom.max, preview_max_map_fraction * map_extent);
    }
    // The map caps never push the start inside the zoom-in limit, nor the zoom-out limit inside the start.
    zoom.start = std::max(zoom.start, zoom.min);
    zoom.max = std::max(zoom.max, zoom.start);
    return zoom;
}
[[nodiscard]] inline float preview_distance_clamp(float distance, float radius, float map_extent = 0.0F) noexcept {
    const PreviewZoom zoom = preview_zoom(radius, map_extent);
    return std::clamp(distance, zoom.min, zoom.max);
}
[[nodiscard]] inline float preview_start_distance(float radius, float map_extent = 0.0F) noexcept {
    return preview_zoom(radius, map_extent).start;
}
// One wheel notch: a fixed 25 units for every ship up to a frigate (the radius knee), at any distance, exactly as
// before. A larger ship takes a fraction of the current distance (never less than 25 units), so a far-out large
// ship does not need hundreds of notches.
[[nodiscard]] inline float preview_wheel_step(float distance, float radius) noexcept {
    if (radius <= preview_knee_radius) return preview_wheel_step_floor;
    return std::max(preview_wheel_step_floor, preview_wheel_step_fraction * distance);
}
// BCAM-22: a transfer needs opposing-enough headings and a target near the forward direction.
[[nodiscard]] inline bool frigate_target_eligible(float source_yaw, float target_yaw, float dx, float dy) noexcept {
    const float distance = std::hypot(dx, dy);
    if (!(distance > 0.0F) || std::abs(std::remainder(source_yaw - target_yaw, 360.0F)) <= 80.0F) return false;
    const float angle = source_yaw * std::numbers::pi_v<float> / 180.0F;
    return (std::cos(angle) * dx + std::sin(angle) * dy) / distance >= std::cos(std::numbers::pi_v<float> / 6.0F);
}

struct BattlecamShot final {
    BattlecamShotKind kind{BattlecamShotKind::chase};
    BattlecamChase chase{};
    std::array<float, 3> start{}, middle{}, end{}, target{};
    float duration{}, radius{}, height{}, direction{};

    [[nodiscard]] std::array<float, 3> at(float elapsed) const noexcept {
        if (kind == BattlecamShotKind::chase) return chase.at(elapsed);
        if (kind == BattlecamShotKind::circle) {
            const float angle = direction * elapsed * 30.0F * (2.0F * std::numbers::pi_v<float> / 600.0F);
            return {radius * std::cos(angle), radius * std::sin(angle), height};
        }
        const float t = std::clamp(elapsed / duration, 0.0F, 1.0F);
        std::array<float, 3> result{};
        for (std::size_t i = 0; i < result.size(); ++i) result[i] = start[i] + (end[i] - start[i]) * t;
        return result;
    }
};

// BCAM-09..13: unsupported initializers fall through.
[[nodiscard]] inline BattlecamShot make_battlecam_shot(const std::array<float, 3>& extents,
    const BattlecamCategory category, BattlecamRandom& random,
    const std::optional<std::array<float, 3>>& frigate_target = std::nullopt) {
    const float r = std::sqrt(extents[0]*extents[0] + extents[1]*extents[1] + extents[2]*extents[2]);
    const float t = std::clamp((r - 30.0F) / 470.0F, 0.0F, 1.0F);
    const float smooth = t*t*(3.0F - 2.0F*t); // BCAM-11 provisional helper.
    const auto chase = [&] {
        BattlecamShot shot;
        shot.kind = BattlecamShotKind::chase;
        shot.chase = BattlecamChase::make(r, random);
        shot.duration = shot.chase.duration;
        return shot;
    };
    const auto frigate = [&] {
        BattlecamShot shot;
        shot.kind = BattlecamShotKind::frigate;
        const float half_duration = static_cast<float>(90U + random.next() % 31U) / 30.0F;
        const float forward = static_cast<float>(5U + random.next() % 6U) * extents[1];
        const float opposite = static_cast<float>(5U + random.next() % 6U) * extents[1];
        constexpr float radians = std::numbers::pi_v<float> / 180.0F;
        const float first = static_cast<float>(random.next() % 361U) * radians * radians;
        const float second = static_cast<float>(random.next() % 361U) * radians * radians;
        static_cast<void>(random.next()); // Third angle is drawn by the initializer.
        const float m = std::max(extents[0], extents[2]);
        shot.start = {static_cast<float>(1U + random.next() % 7U) * m * std::sin(first), forward,
                      static_cast<float>(1U + random.next() % 7U) * m * std::cos(first)};
        shot.middle = {extents[0] * std::sin(second), (forward - opposite) * 0.5F, extents[2] * std::cos(second)};
        shot.end = {0.0F, -1.5F * (*frigate_target)[1], 2.0F * (*frigate_target)[2]};
        shot.duration = 2.0F * half_duration;
        return shot;
    };
    if (category.fighter) return chase();
    if (random.next() % 4U == 0U) {
        const float azimuth = random.between(0.0F, 2.0F * std::numbers::pi_v<float>);
        const float elevation = random.between(20.0F, 45.0F) * std::numbers::pi_v<float> / 180.0F
            * ((random.next() & 0x80000000U) ? 1.0F : -1.0F);
        const float distance = random.between(2.0F, 4.0F) * r;
        BattlecamShot shot;
        shot.kind = BattlecamShotKind::floating;
        shot.start = {distance * std::cos(elevation) * std::cos(azimuth),
                      distance * std::cos(elevation) * std::sin(azimuth), distance * std::sin(elevation)};
        shot.end = shot.start;
        shot.duration = 2.0F;
        return shot;
    }
    if (category.bomber || category.corvette) {
        if (random.next() & 0x80000000U) return chase();
        if (frigate_target) return frigate();
    }
    if (category.frigate && frigate_target) return frigate();
    if ((random.next() & 0x80000000U) == 0U) {
        BattlecamShot shot;
        shot.kind = BattlecamShotKind::circle;
        shot.direction = (random.next() & 0x80000000U) ? 1.0F : -1.0F;
        shot.radius = 2.0F * r * ((random.next() & 0x80000000U) ? 1.0F : -1.0F);
        shot.height = 1.25F * extents[2];
        shot.duration = (90.0F + std::floor(180.0F * smooth)) / 30.0F;
        return shot;
    }
    BattlecamShot shot;
    if (random.next() & 0x80000000U) {
        shot.kind = BattlecamShotKind::belly;
        shot.start = {0.0F, -0.6F*extents[1], -extents[2]};
        shot.end = {0.0F, 3.0F*extents[1], -1.5F*extents[2]};
        shot.target = {0.0F, -6.0F*extents[1], 0.0F};
        shot.duration = 8.0F * (1.0F + smooth);
    } else {
        shot.kind = BattlecamShotKind::flyby;
        const float side = (random.next() & 0x80000000U) ? 1.0F : -1.0F;
        shot.start = {2.0F*extents[0], side*2.0F*extents[1], 2.0F*extents[2]};
        shot.end = {2.0F*extents[0], -side*2.0F*extents[1], 2.0F*extents[2]};
        shot.duration = 3.0F + 4.0F * smooth;
    }
    return shot;
}
} // namespace eawr::presentation::godot_backend
