#pragma once

// #6563 residual 2: the battlecam's reference rotation of its subject, low-passed.
//
// The ordinary battlecam shots place the eye at the subject plus the shot offset turned by the subject's drawn
// rotation. A fighter's rotation is drawn between two published ticks, but the simulation changes a craft's
// turn rate in steps at tick boundaries, so the camera's lever arm (about a hundred units) turned each step
// into a visible up/down kick of the eye (and shook everything else on screen). The filter below follows the
// subject's rotation with a short exponential lag. Only the rotation the camera uses is filtered: the subject's
// position (the pivot of the shot) is not, the placement scale in the matrix is carried through unchanged (so the offset
// length is exactly the unfiltered camera's), and the drawn mesh is not touched.
//
// Engine-free: no Godot, no simulation headers.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>

namespace eawr::presentation::godot_backend {

// Time constant of the camera's rotation lag. A project choice (no retail evidence for or against it): about
// one and a half simulation ticks, enough to take the tick-rate steps out of a fighter's turn rate while a slow
// ship's rotation (a few degrees per second) is followed to within a fraction of a degree.
inline constexpr double battlecam_rotation_tau_seconds = 0.050;
// A subject rotation further than this from the filtered one is a cut (a flip, teleport, respawn), not a turn:
// the filter jumps to it instead of swinging the camera through the angle.
inline constexpr double battlecam_rotation_snap_degrees = 100.0;

using RotationMatrix = std::array<std::array<double, 3>, 3>;

namespace rotation_filter_detail {

using Quat = std::array<double, 4>; // x, y, z, w

[[nodiscard]] inline Quat quat_of(const RotationMatrix& m) noexcept {
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
    double length = 0.0;
    for (const double c : q) length += c * c;
    length = std::sqrt(length);
    if (!(length > 0.0)) return {0.0, 0.0, 0.0, 1.0};
    for (double& c : q) c /= length;
    return q;
}

[[nodiscard]] inline RotationMatrix matrix_of(const Quat& q) noexcept {
    const auto [x, y, z, w] = q;
    return {{{1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - z * w), 2.0 * (x * z + y * w)},
             {2.0 * (x * y + z * w), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - x * w)},
             {2.0 * (x * z - y * w), 2.0 * (y * z + x * w), 1.0 - 2.0 * (x * x + y * y)}}};
}

[[nodiscard]] inline double dot(const Quat& a, const Quat& b) noexcept {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

// The angle between two rotations, in degrees.
[[nodiscard]] inline double angle_degrees(const Quat& a, const Quat& b) noexcept {
    const double cosine = std::min(1.0, std::abs(dot(a, b)));
    return 2.0 * std::acos(cosine) * 180.0 / 3.14159265358979323846;
}

[[nodiscard]] inline Quat slerp(const Quat& from, Quat to, const double t) noexcept {
    double cosine = dot(from, to);
    if (cosine < 0.0) {
        for (double& c : to) c = -c;
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
    for (std::size_t i = 0; i < 4; ++i) {
        result[i] = a * from[i] + b * to[i];
        length += result[i] * result[i];
    }
    length = std::sqrt(length);
    for (double& c : result) c /= length;
    return result;
}

// The uniform scale k of a matrix that is k times a proper rotation (columns of equal length k, mutually orthogonal, right
// handed); nullopt for anything else. Tolerances are loose enough for Q24 fixed-point input.
[[nodiscard]] inline std::optional<double> uniform_scale(const RotationMatrix& m) noexcept {
    double norm[3]{};
    for (std::size_t col = 0; col < 3; ++col) norm[col] = std::sqrt(m[0][col] * m[0][col] + m[1][col] * m[1][col] + m[2][col] * m[2][col]);
    const double k = norm[0];
    if (!(k > 1.0e-6) || !std::isfinite(k)) return std::nullopt;
    for (std::size_t col = 1; col < 3; ++col)
        if (!(std::abs(norm[col] - k) <= 1.0e-4 * k)) return std::nullopt;
    for (std::size_t a = 0; a < 3; ++a) {
        for (std::size_t b = a + 1; b < 3; ++b) {
            const double dot = m[0][a] * m[0][b] + m[1][a] * m[1][b] + m[2][a] * m[2][b];
            if (!(std::abs(dot) <= 1.0e-4 * k * k)) return std::nullopt;
        }
    }
    const double determinant = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
        m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (!(determinant > 0.0)) return std::nullopt;
    return k;
}

} // namespace rotation_filter_detail

// Frame protocol: `evaluate` may be called more than once in a frame (the camera is composed before and after the
// live step); every call filters from the state `commit` left at the frame's start, so the result depends only on
// the latest target and the frame's dt. `commit` takes the last evaluated rotation as the next frame's start.
class BattlecamRotationFilter final {
public:
    // The next evaluation returns its target unchanged (a cut: new shot, new subject).
    void reset() noexcept {
        has_committed_ = false;
        has_pending_ = false;
    }

    void commit() noexcept {
        if (!has_pending_) return;
        committed_ = pending_;
        has_committed_ = true;
    }

    // `target` is the subject's model-to-world 3x3 as the shot arithmetic uses it: a rotation times the placement's uniform
    // scale (Scale_Factor, e.g. 0.7 on fighters). The filter works on the rotation part and puts the scale back, so the
    // offset length is exactly what the unfiltered camera has; a cut (first evaluation, a jump past the snap angle, tau 0)
    // returns the input matrix itself. A matrix that is not a uniformly scaled rotation (shear, mirror, non-uniform scale,
    // zero or not finite) is passed through unfiltered and restarts the filter.
    // `dt_seconds` is the frame's presentation time step; 0 holds the filtered rotation (a paused battle).
    [[nodiscard]] RotationMatrix evaluate(const RotationMatrix& target, const double dt_seconds,
                                          const double tau_seconds = battlecam_rotation_tau_seconds) noexcept {
        using namespace rotation_filter_detail;
        const auto scale = uniform_scale(target);
        if (!scale) {
            reset();
            return target;
        }
        RotationMatrix unit = target;
        for (auto& row : unit)
            for (double& value : row) value /= *scale;
        const Quat wanted = quat_of(unit);
        Quat result = wanted;
        bool filtered = false;
        if (has_committed_ && tau_seconds > 0.0 && angle_degrees(committed_, wanted) <= battlecam_rotation_snap_degrees) {
            const double alpha = dt_seconds > 0.0 ? 1.0 - std::exp(-dt_seconds / tau_seconds) : 0.0;
            result = slerp(committed_, wanted, alpha);
            filtered = true;
        }
        pending_ = result;
        has_pending_ = true;
        if (!filtered) return target;
        RotationMatrix out = matrix_of(result);
        for (auto& row : out)
            for (double& value : row) value *= *scale;
        return out;
    }

private:
    rotation_filter_detail::Quat committed_{0.0, 0.0, 0.0, 1.0};
    rotation_filter_detail::Quat pending_{0.0, 0.0, 0.0, 1.0};
    bool has_committed_{};
    bool has_pending_{};
};

} // namespace eawr::presentation::godot_backend
