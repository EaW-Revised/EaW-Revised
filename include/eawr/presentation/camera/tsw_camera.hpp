#pragma once

#include "eawr/core/result.hpp"

#include <array>
#include <cstdint>
#include <string_view>

// Pure model of the tactical super weapon's power-up cinematic camera (docs/behaviour/space-tactical-superweapon.md, TSW-16).
//
// The cinematic starts in the frame the power-up is accepted: the camera cuts to a close shot of the object's dish, the
// battle UI hides, and after about 570 frames the player's camera and UI come back (played runs, TSW-C4). The played runs
// kept only stills, so the shot's keys are least-visible choices (a static shot on the dish, side-on, held to the end)
// and the numbers below are marked unverified in the behaviour note. A function of the cinematic frame k and the
// geometry it is given. It owns no engine type, keeps no state and reads no clock. Coordinates are source coordinates
// (X, Y on the map plane, Z up).
namespace eawr::presentation::camera {

namespace tsw_rules {
// TSW-C4 (two played runs: 571 and 568 frames from the command to the battle music resuming, UI and camera back).
// The data's own comment for the post-destruction wait puts the activation music at 19.15 s = 575 frames, which fits;
// whether the camera is tied to the music or has its own length is unverified, so the shot is held for the observed
// length. Frames are cinematic frames k, 30 per second.
inline constexpr std::uint32_t duration_frames = 570;
// Unverified, least visible: the camera stands off to the side of the dish axis so the dish reads as a bowl on the
// sphere's limb, as in the one still of the shot (TSW-C4); the dish itself is where the model puts its fire bone.
inline constexpr float view_angle_degrees = 45.0F;  // the camera's bearing off the dish axis, seen from above
inline constexpr float view_distance_radii = 1.4F;  // the camera's distance from the dish centre, in model radii
inline constexpr float near_clip = 1.0F;            // the frustum the other cinematic camera sets (SK-ARR-05)
inline constexpr float far_clip = 10000.0F;
} // namespace tsw_rules

namespace diagnostic_codes {
inline constexpr std::string_view invalid_tsw_camera_input = "EAWR-CAMERA-0604";
} // namespace diagnostic_codes

struct TswCameraInput final {
    // The dish centre and the direction it fires along (the dish axis, need not be a unit vector), in source coordinates.
    std::array<float, 3> dish{};
    std::array<float, 3> dish_axis{1.0F, 0.0F, 0.0F};
    // The model's radius in world units: half the largest extent of its box. It scales the camera's distance.
    float radius{};
};

struct TswCameraPose final {
    std::array<float, 3> eye{};
    std::array<float, 3> target{};
    float near_clip{tsw_rules::near_clip};
    float far_clip{tsw_rules::far_clip};

    friend bool operator==(const TswCameraPose&, const TswCameraPose&) = default;
};

// The shot at cinematic frame `k`. The shot is held: every frame below the duration gives the same pose. Rejects a
// non-finite input, a radius that is not positive, an axis that is not finite or has no length, and a frame at or past
// the end of the cinematic (the caller ends it before asking).
[[nodiscard]] core::Result<TswCameraPose> tsw_camera(const TswCameraInput& input, std::uint32_t k);

// The dish point on the hull as drawn: where the line from the object's centre `origin` through the fire bone's world
// position `bone` meets the sphere of `radius` around the centre. The fire bone is an emitter point that floats above the
// hull, so aiming at it directly points the camera beside the dish. A bone on the centre has no direction and keeps the
// bone position; a non-finite input or a radius that is not positive returns the bone position unchanged.
[[nodiscard]] std::array<float, 3> tsw_dish_on_hull(const std::array<float, 3>& origin, const std::array<float, 3>& bone,
                                                    float radius) noexcept;

// True while the cinematic owns the screen: frames 0..569.
[[nodiscard]] constexpr bool tsw_cinematic_running(const std::uint32_t k) noexcept {
    return k < tsw_rules::duration_frames;
}

} // namespace eawr::presentation::camera
