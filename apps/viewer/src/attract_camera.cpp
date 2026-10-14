#include "attract_camera.hpp"

#include "eawr/presentation/camera/camera.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace eawr::presentation::godot_backend {
namespace {

// The placeholder's motion: slow enough to read as a held establishing shot that is alive.
constexpr double orbit_degrees_per_second = 1.2;
constexpr double drift_seconds = 6.0;         // time constant of the look-at's approach to the fight
constexpr double pitch_swing_degrees = 3.0;
constexpr double pitch_period_seconds = 70.0;
constexpr double breathe_fraction = 0.06;     // of the saved distance
constexpr double breathe_period_seconds = 110.0;
constexpr double pi = 3.14159265358979323846;

[[nodiscard]] float float_of(const std::uint32_t bits) noexcept {
    float value{};
    static_assert(sizeof value == sizeof bits);
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

} // namespace

AttractCamera::AttractCamera(const skirmish::AttractCamera& saved)
    : saved_target_{float_of(saved.look_at_bits[0]), float_of(saved.look_at_bits[2]),
                    -float_of(saved.look_at_bits[1])},
      target_(saved_target_),
      distance_(float_of(saved.distance_bits)),
      pitch_(float_of(saved.pitch_degrees_bits)),
      yaw_(float_of(saved.yaw_degrees_bits)) {
    const auto eye = camera::eye_position(saved_target_, distance_, pitch_, yaw_);
    if (!eye) return;
    valid_ = true;
    pose_.eye = eye.value();
    pose_.target = target_;
}

void AttractCamera::place() {
    const double yaw = static_cast<double>(yaw_) + orbit_degrees_per_second * elapsed_;
    const double pitch = static_cast<double>(pitch_)
        + pitch_swing_degrees * std::sin(2.0 * pi * elapsed_ / pitch_period_seconds);
    const double distance = static_cast<double>(distance_)
        * (1.0 + breathe_fraction * std::sin(2.0 * pi * elapsed_ / breathe_period_seconds));
    const auto eye = camera::eye_position(target_, static_cast<float>(distance), static_cast<float>(pitch),
                                          static_cast<float>(std::fmod(yaw, 360.0)));
    if (!eye) return; // keep the last good frame
    pose_.eye = eye.value();
    pose_.target = target_;
}

AttractCameraPose AttractCamera::update(const double delta, const AttractBattleView& battle) {
    if (!valid_ || battle.paused || !(delta > 0.0)) return pose_;
    elapsed_ += delta;
    if (!battle.ships.empty()) {
        std::array<double, 3> centroid{};
        for (const auto& ship : battle.ships) {
            for (std::size_t axis = 0; axis < 3; ++axis) centroid[axis] += static_cast<double>(ship[axis]);
        }
        const double count = static_cast<double>(battle.ships.size());
        const double blend = 1.0 - std::exp(-delta / drift_seconds);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const double goal = centroid[axis] / count;
            target_[axis] = static_cast<float>(static_cast<double>(target_[axis])
                                               + (goal - static_cast<double>(target_[axis])) * blend);
        }
    }
    place();
    return pose_;
}

} // namespace eawr::presentation::godot_backend
