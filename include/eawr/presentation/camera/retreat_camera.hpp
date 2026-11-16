#pragma once

#include "eawr/core/result.hpp"
#include "eawr/presentation/camera/camera.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

// Pure model of the campaign space-battle retreat cinematic camera
// (docs/behaviour/gc-retreat-frame-schedule.md FS-R07..FS-R09, FSC-R07, FC-R03).
//
// The shots are built from constants of the debug build, not loaded from a camera
// XML file or a ship animation (FS-R08). The only authored input is the `Unlocked`
// entry of the tactical camera data, whose clip and fade values the cinematic camera
// applies (FS-R09): those arrive in `Constants` and are copied into the pose, never
// restated here.
//
// Presentation only: a function of the simulation's cinematic frame k and the
// geometry it is given. It owns no engine type, keeps no state and reads no clock.
// Coordinates are source coordinates (X, Y on the map plane, Z up), the space the
// simulation and the map markers use; the caller converts to render space.
namespace eawr::presentation::camera {

namespace retreat_rules {
// FS-R08/FS-R09, debug build. The frame numbers are cinematic frames k, 30 per second (FC-R03).
inline constexpr std::uint32_t station_cut_frame = 150;       // ship shot -> attached station shot
inline constexpr std::uint32_t station_hold_last_frame = 210; // last hold frame; the orbit starts strictly after
inline constexpr std::uint32_t station_only_hold_last_frame = 60;
inline constexpr float orbit_period_frames = 1000.0F;
inline constexpr float orbit_ease = 0.5F; // ease-in and ease-out share this value
inline constexpr float ship_shot_span_fraction = 0.25F;
inline constexpr float ship_shot_perpendicular = 150.0F;
inline constexpr float ship_shot_height = 70.0F;
inline constexpr std::array<float, 3> station_camera_offset{0.0F, -2000.0F, 2000.0F};
} // namespace retreat_rules

namespace diagnostic_codes {
inline constexpr std::string_view invalid_retreat_camera_input = "EAWR-CAMERA-0601";
inline constexpr std::string_view invalid_retreat_camera_clip = "EAWR-CAMERA-0602";
} // namespace diagnostic_codes

enum class RetreatShot : std::uint8_t {
    ship,          // fixed shot from the entry marker geometry
    station_hold,  // attached to the station, offset held
    station_orbit, // attached to the station, eased Z orbit
};

[[nodiscard]] constexpr std::string_view to_string(const RetreatShot shot) noexcept {
    switch (shot) {
    case RetreatShot::ship: return "ship";
    case RetreatShot::station_hold: return "station_hold";
    case RetreatShot::station_orbit: return "station_orbit";
    }
    return "invalid";
}

struct RetreatCameraInput final {
    // FS-R07: the ATTACKER ENTRY POSITION marker.
    std::array<float, 3> entry{};
    // The point the departing ships fly toward, on the marker's departure direction.
    std::array<float, 3> destination{};
    // FS-R07: the departure direction (the marker's local negative X turned by its pitch and yaw), unit length.
    std::array<float, 3> departure_direction{1.0F, 0.0F, 0.0F};
    // The attached station point (its position plus the model path's ranged-target Z adjust, FS-R09) while a
    // living station departs; nullopt without one. A removed attachment keeps its last transform: the caller
    // holds that cache and passes it here.
    std::optional<std::array<float, 3>> station;
    // False for the station-only cinematic (FC-R02): its attached keys start at frame 0 and the roster shot is absent.
    bool ship_shot{true};
};

struct RetreatCameraPose final {
    std::array<float, 3> eye{};
    std::array<float, 3> target{};
    RetreatShot shot{RetreatShot::ship};
    // The Unlocked camera's clip and fade settings (FS-R09), straight from the data.
    float near_clip{};
    float far_clip{};
    float object_fade_begin{};
    float object_fade_end{};

    friend bool operator==(const RetreatCameraPose&, const RetreatCameraPose&) = default;
};

// The shot at cinematic frame `k`. `unlocked` is the loaded `Unlocked` camera definition; its clip and fade
// values are copied into the result (and rejected when they cannot describe a camera). Rejects a non-finite
// input, a station-less input without a ship shot, and a station shot request without a station.
// `fraction` in [0, 1) is the presented time between frame k and k+1: only the orbit moves between frames (the
// shots are held), so a renderer faster than 30 Hz gets a smooth turn. It never changes which shot plays.
[[nodiscard]] core::Result<RetreatCameraPose> retreat_camera(
    const RetreatCameraInput& input, std::uint32_t k, const Constants& unlocked, float fraction = 0.0F);

// FS-R07: the departure direction of an ATTACKER ENTRY POSITION marker: its local negative X turned by the marker's
// pitch (about Y) and then yaw (about Z), both degrees. Unit length. Stock space markers carry no pitch; the pitch
// sign convention is unverified (docs/behaviour/gc-retreat-frame-schedule.md, UFS-R04).
[[nodiscard]] std::array<float, 3> retreat_departure_direction(float pitch_degrees, float yaw_degrees) noexcept;

// Which shot plays at frame k for this input, without building geometry (hold and cut boundaries only).
[[nodiscard]] RetreatShot retreat_shot_at(const RetreatCameraInput& input, std::uint32_t k) noexcept;

// FS-R09 orbit angle, degrees counter-clockwise about Z, `frames` after the last key. Eased in and out
// (retreat_rules::orbit_ease) over one period; zero at the start.
[[nodiscard]] float retreat_orbit_degrees(float frames_after_last_key) noexcept;

} // namespace eawr::presentation::camera
