#include "eawr/presentation/space/live_units.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace eawr::presentation::space {
namespace {

constexpr double degrees_per_radian = 180.0 / std::numbers::pi;

[[nodiscard]] double to_double(const sim::math::Fixed value) noexcept {
    return static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale);
}

[[nodiscard]] std::array<double, 3> translation(const sim::math::Mat3x4& transform) noexcept {
    return {to_double(transform.rows[0][3]), to_double(transform.rows[1][3]), to_double(transform.rows[2][3])};
}

[[nodiscard]] double wrapped(const double degrees) noexcept { return degrees <= -180.0 ? 180.0 : degrees; }

// The angle `t` of the way from `from` to `to` turning the short way round.
[[nodiscard]] double short_way(const double from, const double to, const double t) noexcept {
    double turn = std::fmod(to - from, 360.0);
    if (turn > 180.0) turn -= 360.0;
    if (turn <= -180.0) turn += 360.0;
    return from + turn * t;
}

// A rotation as a unit quaternion (x, y, z, w) and back as a row-major 3x3.
using Quat = std::array<double, 4>;
using Rotation = std::array<std::array<double, 3>, 3>;

[[nodiscard]] Rotation rotation_of(const sim::math::Mat3x4& transform) noexcept {
    Rotation matrix{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) matrix[row][column] = to_double(transform.rows[row][column]);
    }
    return matrix;
}

[[nodiscard]] Quat quat_of(const Rotation& m) noexcept {
    Quat q{};
    const double trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0) {
        const double s = 2.0 * std::sqrt(trace + 1.0);
        q = {(m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, 0.25 * s};
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const double s = 2.0 * std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]);
        q = {0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s};
    } else if (m[1][1] > m[2][2]) {
        const double s = 2.0 * std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]);
        q = {(m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s};
    } else {
        const double s = 2.0 * std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]);
        q = {(m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s, (m[1][0] - m[0][1]) / s};
    }
    return q;
}

[[nodiscard]] Rotation rotation_of(const Quat& q) noexcept {
    const auto [x, y, z, w] = q;
    return {{{1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - z * w), 2.0 * (x * z + y * w)},
             {2.0 * (x * y + z * w), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - x * w)},
             {2.0 * (x * z - y * w), 2.0 * (y * z + x * w), 1.0 - 2.0 * (x * x + y * y)}}};
}

// The rotation `t` of the way from `from` to `to` along the shortest arc.
[[nodiscard]] Quat slerp(const Quat& from, Quat to, const double t) noexcept {
    double cosine = from[0] * to[0] + from[1] * to[1] + from[2] * to[2] + from[3] * to[3];
    if (cosine < 0.0) {
        for (double& component : to) component = -component;
        cosine = -cosine;
    }
    double a = 1.0 - t;
    double b = t;
    if (cosine < 0.9995) {
        const double angle = std::acos(std::min(cosine, 1.0));
        const double sine = std::sin(angle);
        a = std::sin((1.0 - t) * angle) / sine;
        b = std::sin(t * angle) / sine;
    }
    Quat result{};
    double length = 0.0;
    for (std::size_t index = 0; index < 4; ++index) {
        result[index] = a * from[index] + b * to[index];
        length += result[index] * result[index];
    }
    length = std::sqrt(length);
    for (double& component : result) component /= length;
    return result;
}

// Yaw, pitch and roll of Rz(yaw) Ry(pitch) Rx(roll) (space-fighters FM-02). With the nose
// straight up or down the yaw carries the whole turn about Z and the roll is zero.
struct Angles {
    double yaw{};
    double pitch{};
    double roll{};
};

[[nodiscard]] Angles angles_of(const Rotation& m) noexcept {
    const double across = std::hypot(m[0][0], m[1][0]);
    Angles angles;
    angles.pitch = std::atan2(-m[2][0], across) * degrees_per_radian;
    if (across < 1.0e-9) {
        angles.yaw = wrapped(std::atan2(-m[0][1], m[1][1]) * degrees_per_radian);
        return angles;
    }
    angles.yaw = wrapped(std::atan2(m[1][0], m[0][0]) * degrees_per_radian);
    angles.roll = (m[2][1] == 0.0 && m[2][2] == 0.0) ? 0.0 : wrapped(std::atan2(m[2][1], m[2][2]) * degrees_per_radian);
    return angles;
}

} // namespace

double instance_yaw_degrees(const sim::math::Mat3x4& transform) noexcept {
    const double x = to_double(transform.rows[0][0]);
    const double y = to_double(transform.rows[1][0]);
    if (x == 0.0 && y == 0.0) return 0.0;
    return wrapped(std::atan2(y, x) * degrees_per_radian);
}

double instance_roll_degrees(const sim::math::Mat3x4& transform) noexcept {
    const double up = to_double(transform.rows[2][2]);
    const double side = to_double(transform.rows[2][1]);
    if (up == 0.0 && side == 0.0) return 0.0;
    return wrapped(std::atan2(side, up) * degrees_per_radian);
}

double instance_pitch_degrees(const sim::math::Mat3x4& transform) noexcept {
    const double down = to_double(transform.rows[2][0]);
    if (down == 0.0) return 0.0;
    return std::atan2(-down, std::hypot(to_double(transform.rows[0][0]), to_double(transform.rows[1][0])))
        * degrees_per_radian;
}

std::vector<LiveUnitPose> interpolate_units(const sim::tactical::TacticalSnapshot& previous,
                                            const sim::tactical::TacticalSnapshot& latest,
                                            const double alpha,
                                            const sim::tactical::PlayerId viewer,
                                            const bool reveal,
                                            const std::span<const sim::EntityId> fading) {
    const double t = std::clamp(alpha, 0.0, 1.0);
    const std::vector<sim::EntityId> visible = reveal ? std::vector<sim::EntityId>{} : latest.visible_entities(viewer);
    const auto before = previous.instances();
    std::vector<LiveUnitPose> poses;
    poses.reserve(reveal ? latest.instances().size() : visible.size() + fading.size());
    auto earlier = before.begin();
    for (const sim::tactical::TacticalInstance& instance : latest.instances()) {
        const bool shown = reveal || std::binary_search(visible.begin(), visible.end(), instance.entity_id)
            || std::binary_search(fading.begin(), fading.end(), instance.entity_id);
        if (!shown) continue;
        const Angles latest_angles = angles_of(rotation_of(instance.fixed_transform));
        LiveUnitPose pose{instance.entity_id, instance.type_id, instance.owner,
                          translation(instance.fixed_transform), instance_yaw_degrees(instance.fixed_transform),
                          instance_roll_degrees(instance.fixed_transform), 0.0, &instance};
        if (instance.fixed_transform.rows[2][0].raw() != 0) {
            // #506: a pitched craft (FM-02) draws its whole rotation.
            pose.yaw_degrees = latest_angles.yaw;
            pose.pitch_degrees = latest_angles.pitch;
            pose.roll_degrees = latest_angles.roll;
        }
        // Both lists ascend by ID.
        earlier = std::lower_bound(earlier, before.end(), instance.entity_id,
            [](const sim::tactical::TacticalInstance& item, const sim::EntityId id) { return item.entity_id < id; });
        if (earlier != before.end() && earlier->entity_id == instance.entity_id) {
            const auto from = translation(earlier->fixed_transform);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                pose.position[axis] = from[axis] + (pose.position[axis] - from[axis]) * t;
            }
            if (instance.fixed_transform.rows[2][0].raw() != 0 || earlier->fixed_transform.rows[2][0].raw() != 0) {
                // #506: the shortest arc between the two rotations, so the nose leads the flight
                // through a climb, a dive and a loop over the vertical (FM-06).
                const Quat start = quat_of(rotation_of(earlier->fixed_transform));
                const Quat end = quat_of(rotation_of(instance.fixed_transform));
                const Angles eased = angles_of(rotation_of(slerp(start, end, t)));
                pose.yaw_degrees = eased.yaw;
                pose.pitch_degrees = eased.pitch;
                pose.roll_degrees = eased.roll;
            } else {
                pose.yaw_degrees = short_way(instance_yaw_degrees(earlier->fixed_transform), pose.yaw_degrees, t);
                // #479: a craft rolling out of a loop upside down crosses +-180; the short way
                // keeps it from spinning a whole turn between two ticks.
                pose.roll_degrees = short_way(instance_roll_degrees(earlier->fixed_transform), pose.roll_degrees, t);
            }
        }
        poses.push_back(pose);
    }
    return poses;
}

std::vector<LiveUnitPose> interpolate_spinning(const sim::tactical::TacticalSnapshot& previous,
                                               const sim::tactical::TacticalSnapshot& latest,
                                               const double alpha,
                                               const sim::tactical::PlayerId viewer,
                                               const bool reveal) {
    const double t = std::clamp(alpha, 0.0, 1.0);
    const auto players = latest.players();
    const auto seat = std::find_if(players.begin(), players.end(),
        [viewer](const sim::tactical::SnapshotPlayer& player) { return player.player_id == viewer; });
    std::vector<LiveUnitPose> poses;
    if (seat == players.end() && !reveal) return poses;
    const auto bit = reveal ? std::uint64_t{} : std::uint64_t{1} << static_cast<std::size_t>(seat - players.begin());
    const auto spun = previous.spinning();
    const auto lived = previous.instances();
    for (const sim::tactical::SpinningCraft& craft : latest.spinning()) {
        if (!reveal && (craft.visible_to & bit) == 0U) continue;
        LiveUnitPose pose{craft.entity_id, craft.type_id, craft.owner, translation(craft.fixed_transform),
                          to_double(craft.yaw), to_double(craft.roll), to_double(craft.pitch), nullptr, true};
        const sim::math::Mat3x4* from = nullptr;
        const auto was_spinning = std::lower_bound(spun.begin(), spun.end(), craft.entity_id,
            [](const sim::tactical::SpinningCraft& item, const sim::EntityId id) { return item.entity_id < id; });
        const auto was_live = std::lower_bound(lived.begin(), lived.end(), craft.entity_id,
            [](const sim::tactical::TacticalInstance& item, const sim::EntityId id) { return item.entity_id < id; });
        if (was_spinning != spun.end() && was_spinning->entity_id == craft.entity_id) {
            from = &was_spinning->fixed_transform;
        } else if (was_live != lived.end() && was_live->entity_id == craft.entity_id) {
            from = &was_live->fixed_transform;
        }
        if (from != nullptr) {
            const auto from_position = translation(*from);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                pose.position[axis] = from_position[axis] + (pose.position[axis] - from_position[axis]) * t;
            }
            // The shortest arc between the two rotations, as for a pitched live craft (#506).
            const Quat start = quat_of(rotation_of(*from));
            const Quat end = quat_of(rotation_of(craft.fixed_transform));
            const Angles eased = angles_of(rotation_of(slerp(start, end, t)));
            pose.yaw_degrees = eased.yaw;
            pose.pitch_degrees = eased.pitch;
            pose.roll_degrees = eased.roll;
        }
        poses.push_back(pose);
    }
    return poses;
}

} // namespace eawr::presentation::space
