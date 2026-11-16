// #6563 residual 2: the battlecam reference rotation filter (apps/viewer/src/battlecam_rotation_filter.hpp), the
// header movie_pose runs the ordinary shots and the C-key preview through.
//
// A fighter's rotation is drawn between two published ticks, but its turn rate changes in steps at tick
// boundaries; a camera that turns its offset by that rotation kicks the eye at 30 Hz. These tests feed synthetic
// tick poses with such rate steps through the same interpolation the viewer draws (shortest-arc slerp), build the
// chase eye the way movie_pose does (subject + rotation * offset) and compare the eye's vertical acceleration
// without the filter (the shipped behaviour before the filter: the camera used the drawn rotation as it came) and
// with it, at 60, 120 and 144 Hz. They also bound the lag the filter adds and pin its reset, hold and cut rules.
#include "../../support/check.hpp"
#include "../../../apps/viewer/src/battlecam_rotation_filter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace eawr::presentation::godot_backend;
using rotation_filter_detail::Quat;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(bool condition, std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAILED: ");
}

constexpr double ticks_per_second = 30.0;
constexpr double degrees = std::numbers::pi / 180.0;

[[nodiscard]] Quat qmul(const Quat& a, const Quat& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

// The rotation of `angle` radians about the unit `axis`.
[[nodiscard]] Quat axis_angle(const std::array<double, 3>& axis, const double angle) {
    const double s = std::sin(angle * 0.5);
    return {axis[0] * s, axis[1] * s, axis[2] * s, std::cos(angle * 0.5)};
}

[[nodiscard]] std::array<double, 3> apply(const RotationMatrix& m, const std::array<double, 3>& v) {
    return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
        m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}

// A subject whose turn rate (degrees per tick, about a fixed tilted axis) holds for one tick at a time and steps
// between ticks, the way the craft model's rate-limited turns do; `rates` repeat.
struct Subject final {
    std::vector<Quat> tick_rotation;
    std::vector<std::array<double, 3>> tick_position;
};

[[nodiscard]] Subject make_subject(const std::vector<double>& rates_degrees_per_tick, const std::size_t ticks) {
    Subject subject;
    Quat q{0.0, 0.0, 0.0, 1.0};
    const double n = std::sqrt(0.3 * 0.3 + 0.2 * 0.2 + 0.93 * 0.93);
    const std::array<double, 3> axis{0.3 / n, 0.2 / n, 0.93 / n};
    for (std::size_t k = 0; k <= ticks; ++k) {
        subject.tick_rotation.push_back(q);
        subject.tick_position.push_back({4.0 * static_cast<double>(k), 1.0 * static_cast<double>(k), 0.5 * static_cast<double>(k)});
        q = qmul(axis_angle(axis, rates_degrees_per_tick[k % rates_degrees_per_tick.size()] * degrees), q);
    }
    return subject;
}

// The drawn rotation of the subject at presentation time `t` seconds: slerp between the two bracketing ticks.
[[nodiscard]] Quat drawn_rotation(const Subject& subject, const double t) {
    const auto k = static_cast<std::size_t>(std::floor(t * ticks_per_second));
    const double alpha = t * ticks_per_second - static_cast<double>(k);
    return rotation_filter_detail::slerp(subject.tick_rotation[k], subject.tick_rotation[k + 1], alpha);
}

struct Eye final {
    std::vector<std::array<double, 3>> eye;
    std::vector<double> rotation_error_degrees; // filtered against drawn, per frame
};

// The chase eye of every frame at `hz`: pivot + R * offset, R the drawn rotation or its filtered version.
[[nodiscard]] Eye chase_eye(const Subject& subject, const double hz, const double tau, const std::array<double, 3>& offset,
                            const double scale = 1.0) {
    Eye out;
    BattlecamRotationFilter filter;
    const double dt = 1.0 / hz;
    const std::size_t ticks = subject.tick_rotation.size() - 2;
    for (double t = 0.0; t < static_cast<double>(ticks) / ticks_per_second; t += dt) {
        const Quat drawn = drawn_rotation(subject, t);
        // The matrix movie_pose hands the filter: the drawn rotation times the placement scale (Scale_Factor).
        RotationMatrix target = rotation_filter_detail::matrix_of(drawn);
        for (auto& row : target)
            for (double& value : row) value *= scale;
        filter.commit();
        const RotationMatrix used = filter.evaluate(target, t == 0.0 ? 0.0 : dt, tau);
        const auto k = static_cast<std::size_t>(std::floor(t * ticks_per_second));
        const double alpha = t * ticks_per_second - static_cast<double>(k);
        std::array<double, 3> pivot{};
        for (std::size_t i = 0; i < 3; ++i)
            pivot[i] = subject.tick_position[k][i] + (subject.tick_position[k + 1][i] - subject.tick_position[k][i]) * alpha;
        const auto rotated = apply(used, offset);
        out.eye.push_back({pivot[0] + rotated[0], pivot[1] + rotated[1], pivot[2] + rotated[2]});
        RotationMatrix unit_used = used;
        for (auto& row : unit_used)
            for (double& value : row) value /= scale;
        out.rotation_error_degrees.push_back(rotation_filter_detail::angle_degrees(rotation_filter_detail::quat_of(unit_used), drawn));
    }
    return out;
}

// Per-frame change of the eye's vertical velocity (source z is up), units/s^2.
[[nodiscard]] std::vector<double> vertical_acceleration(const Eye& eye, const double hz) {
    std::vector<double> out;
    const double dt = 1.0 / hz;
    for (std::size_t i = 2; i < eye.eye.size(); ++i) {
        const double v1 = (eye.eye[i - 1][2] - eye.eye[i - 2][2]) / dt;
        const double v2 = (eye.eye[i][2] - eye.eye[i - 1][2]) / dt;
        out.push_back(std::abs(v2 - v1) / dt);
    }
    return out;
}

[[nodiscard]] double percentile(std::vector<double> values, const double p) {
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>(p * static_cast<double>(values.size() - 1))];
}

// Turn rates in degrees per tick that step every tick, the case that kicked the owner's camera.
const std::vector<double> stepping_rates{0.6, 0.6, 4.4, 4.7, 1.5, 1.1, -1.4, -3.0, 0.3, 2.6, 5.1, 0.9, -2.2, -0.4, 3.3, 1.8};
const std::array<double, 3> chase_offset{-90.0, 0.0, 35.0};

void test_filter_takes_the_tick_steps_out_of_the_eye() {
    const Subject subject = make_subject(stepping_rates, 480);
    for (const double hz : {60.0, 120.0, 144.0}) {
        const auto rigid = vertical_acceleration(chase_eye(subject, hz, 0.0, chase_offset), hz);
        const auto filtered = vertical_acceleration(chase_eye(subject, hz, battlecam_rotation_tau_seconds, chase_offset), hz);
        const std::string label = std::to_string(static_cast<int>(hz)) + " Hz";
        const double rigid_p99 = percentile(rigid, 0.99);
        const double filtered_p99 = percentile(filtered, 0.99);
        const double rigid_p90 = percentile(rigid, 0.90);
        const double filtered_p90 = percentile(filtered, 0.90);
        std::cout << label << ": eye vertical acceleration p90 " << rigid_p90 << " -> " << filtered_p90 << ", p99 " << rigid_p99
                  << " -> " << filtered_p99 << " units/s^2\n";
        // The unfiltered camera (main before this change) must show the kick this test exists for ...
        expect(rigid_p99 > 4.0 * percentile(rigid, 0.50) + 1.0, label + ": the rigid camera shows tick-rate kicks (control)");
        // ... and the filtered one must take at least half of the tail out.
        expect(filtered_p99 <= 0.5 * rigid_p99, label + ": filtered eye acceleration p99 is at most half the rigid one");
        expect(filtered_p90 <= 0.8 * rigid_p90, label + ": filtered eye acceleration p90 is below the rigid one");
    }
}

void test_lag_bound() {
    // A steady turn of w degrees per second is followed with a lag of w * tau at rest state (exponential follower).
    for (const double rate_per_second : {3.0, 30.0, 90.0, 180.0}) {
        const Subject subject = make_subject({rate_per_second / ticks_per_second}, 150);
        for (const double hz : {60.0, 120.0, 144.0}) {
            const Eye eye = chase_eye(subject, hz, battlecam_rotation_tau_seconds, chase_offset);
            // Skip the first second (the filter has not settled onto the ramp).
            double worst = 0.0;
            for (std::size_t i = static_cast<std::size_t>(hz); i < eye.rotation_error_degrees.size(); ++i)
                worst = std::max(worst, eye.rotation_error_degrees[i]);
            const double bound = rate_per_second * battlecam_rotation_tau_seconds * 1.15 + 0.05;
            expect(worst <= bound, "steady turn of " + std::to_string(rate_per_second) + " deg/s at " + std::to_string(hz) +
                                       " Hz: rotation lag " + std::to_string(worst) + " deg stays within " + std::to_string(bound));
            if (rate_per_second <= 3.0) expect(worst < 0.2, "a slow ship's rotation is followed to within 0.2 degrees");
        }
    }
}

void test_subject_stays_centred_and_offset_length_is_kept() {
    // The shot pivots on the subject's position, which is not filtered, and the aim is pivot + R * target_offset;
    // with the usual zero target offset the subject is exactly at the aim whatever the rotation lag is.
    const Subject subject = make_subject(stepping_rates, 240);
    const Eye eye = chase_eye(subject, 120.0, battlecam_rotation_tau_seconds, chase_offset);
    const double length = std::sqrt(90.0 * 90.0 + 35.0 * 35.0);
    for (std::size_t i = 0; i < eye.eye.size(); ++i) {
        const double t = static_cast<double>(i) / 120.0;
        const auto k = static_cast<std::size_t>(std::floor(t * ticks_per_second));
        const double alpha = t * ticks_per_second - static_cast<double>(k);
        double distance = 0.0;
        for (std::size_t a = 0; a < 3; ++a) {
            const double pivot = subject.tick_position[k][a] + (subject.tick_position[k + 1][a] - subject.tick_position[k][a]) * alpha;
            distance += (eye.eye[i][a] - pivot) * (eye.eye[i][a] - pivot);
        }
        if (std::abs(std::sqrt(distance) - length) > 1.0e-6) {
            expect(false, "the eye keeps its distance from the subject (frame " + std::to_string(i) + ")");
            return;
        }
    }
}

// The placement scale (Scale_Factor: 0.7 on the A-Wing, 0.5 on corvettes) rides in the matrix movie_pose hands the filter.
// Review finding: reading such a matrix as a rotation shrank the offset to unit length and turned the camera by the wrong
// angle. The unfiltered camera must be exactly main's, and the filtered one must keep the scaled offset length and filter
// the same rotation as the unscaled ship.
void test_scaled_subjects() {
    const Subject subject = make_subject(stepping_rates, 240);
    const double hz = 120.0;
    const Eye unscaled = chase_eye(subject, hz, battlecam_rotation_tau_seconds, chase_offset, 1.0);
    const double length = std::sqrt(90.0 * 90.0 + 35.0 * 35.0);
    for (const double scale : {0.5, 0.7, 1.2, 1.5}) {
        const std::string label = "scale " + std::to_string(scale) + ": ";
        // tau 0: the matrix comes back exactly as given, so the eye is main's.
        {
            BattlecamRotationFilter filter;
            const RotationMatrix scaled{{{0.0, -0.6 * scale, 0.8 * scale}, {1.0 * scale, 0.0, 0.0}, {0.0, 0.8 * scale, 0.6 * scale}}};
            filter.commit();
            const RotationMatrix first = filter.evaluate(scaled, 0.016, 0.0);
            filter.commit();
            const RotationMatrix second = filter.evaluate(scaled, 0.016, 0.0);
            bool same = true;
            for (std::size_t r = 0; r < 3; ++r)
                for (std::size_t c = 0; c < 3; ++c) same = same && first[r][c] == scaled[r][c] && second[r][c] == scaled[r][c];
            expect(same, label + "tau 0 returns the scaled matrix unchanged");
        }
        const Eye rigid = chase_eye(subject, hz, 0.0, chase_offset, scale);
        const Eye filtered = chase_eye(subject, hz, battlecam_rotation_tau_seconds, chase_offset, scale);
        double worst_length = 0.0;
        double worst_rotation = 0.0;
        double worst_rigid = 0.0;
        for (std::size_t i = 0; i < filtered.eye.size(); ++i) {
            const double t = static_cast<double>(i) / hz;
            const auto k = static_cast<std::size_t>(std::floor(t * ticks_per_second));
            const double alpha = t * ticks_per_second - static_cast<double>(k);
            double distance = 0.0;
            double rigid_distance = 0.0;
            double scaled_difference = 0.0;
            for (std::size_t a = 0; a < 3; ++a) {
                const double pivot = subject.tick_position[k][a] + (subject.tick_position[k + 1][a] - subject.tick_position[k][a]) * alpha;
                distance += (filtered.eye[i][a] - pivot) * (filtered.eye[i][a] - pivot);
                rigid_distance += (rigid.eye[i][a] - pivot) * (rigid.eye[i][a] - pivot);
                // The same ship at scale 1 has the same filtered rotation: its eye offset times the scale is this one.
                const double expected = pivot + (unscaled.eye[i][a] - pivot) * scale;
                scaled_difference = std::max(scaled_difference, std::abs(filtered.eye[i][a] - expected));
            }
            worst_length = std::max(worst_length, std::abs(std::sqrt(distance) - scale * length));
            worst_rigid = std::max(worst_rigid, std::abs(std::sqrt(rigid_distance) - scale * length));
            worst_rotation = std::max(worst_rotation, scaled_difference);
        }
        expect(worst_rigid < 1.0e-9, label + "the unfiltered eye keeps the scaled offset length (main)");
        expect(worst_length < 1.0e-9, label + "the filtered eye keeps the scaled offset length");
        expect(worst_rotation < 1.0e-9, label + "the filtered rotation is the unscaled ship's, scaled");
    }
    // Anything that is not a uniformly scaled rotation is passed through untouched and restarts the filter.
    BattlecamRotationFilter filter;
    const RotationMatrix rotation = rotation_filter_detail::matrix_of(axis_angle({0.0, 0.0, 1.0}, 10.0 * degrees));
    filter.commit();
    static_cast<void>(filter.evaluate(rotation, 0.016));
    filter.commit();
    RotationMatrix stretched = rotation;
    for (std::size_t r = 0; r < 3; ++r) stretched[r][0] *= 1.5;
    const RotationMatrix passed = filter.evaluate(stretched, 0.016);
    bool same = true;
    for (std::size_t r = 0; r < 3; ++r)
        for (std::size_t c = 0; c < 3; ++c) same = same && passed[r][c] == stretched[r][c];
    expect(same, "a non-uniformly scaled matrix is passed through unchanged");
    RotationMatrix mirrored = rotation;
    for (std::size_t r = 0; r < 3; ++r) mirrored[r][1] = -mirrored[r][1];
    same = true;
    const RotationMatrix mirrored_out = filter.evaluate(mirrored, 0.016);
    for (std::size_t r = 0; r < 3; ++r)
        for (std::size_t c = 0; c < 3; ++c) same = same && mirrored_out[r][c] == mirrored[r][c];
    expect(same, "a mirrored matrix is passed through unchanged");
    const RotationMatrix zero{};
    const RotationMatrix zero_out = filter.evaluate(zero, 0.016);
    expect(zero_out[0][0] == 0.0 && zero_out[1][1] == 0.0, "a zero matrix is passed through unchanged");
}

void test_cut_hold_and_frame_protocol() {
    const RotationMatrix identity{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    const auto turned = [](const double angle) {
        return rotation_filter_detail::matrix_of(axis_angle({0.0, 0.0, 1.0}, angle * degrees));
    };
    const auto close = [](const RotationMatrix& a, const RotationMatrix& b) {
        for (std::size_t r = 0; r < 3; ++r)
            for (std::size_t c = 0; c < 3; ++c)
                if (std::abs(a[r][c] - b[r][c]) > 1.0e-9) return false;
        return true;
    };
    BattlecamRotationFilter filter;
    // The first evaluation after a reset returns the target (a cut starts on the subject's own rotation), and so
    // does the second evaluation of that frame.
    expect(close(filter.evaluate(turned(10.0), 0.016), turned(10.0)), "the first evaluation returns its target");
    expect(close(filter.evaluate(turned(12.0), 0.016), turned(12.0)), "the frame's second evaluation still returns its target");
    filter.commit();
    // A new frame filters from the committed state; repeating the evaluation does not advance the filter.
    const RotationMatrix first = filter.evaluate(turned(30.0), 0.016);
    const RotationMatrix again = filter.evaluate(turned(30.0), 0.016);
    expect(close(first, again), "evaluating twice in one frame gives one result");
    expect(!close(first, turned(30.0)) && !close(first, turned(12.0)), "a turn is followed with a lag, not skipped or ignored");
    // Pause: dt 0 holds the filtered rotation whatever the target does.
    filter.commit();
    const RotationMatrix held = filter.evaluate(turned(60.0), 0.0);
    expect(close(held, first), "dt 0 holds the filtered rotation");
    // A jump past the snap angle is a cut, followed at once.
    filter.commit();
    expect(close(filter.evaluate(turned(60.0 + 2.0 * battlecam_rotation_snap_degrees), 0.016), turned(60.0 + 2.0 * battlecam_rotation_snap_degrees)),
        "a rotation further than the snap angle is taken at once");
    // Reset.
    filter.commit();
    filter.reset();
    expect(close(filter.evaluate(identity, 0.016), identity), "reset: the next evaluation returns its target");
    // tau 0 is the pass-through (the camera before the filter): the same matrix comes back, whatever its scale.
    BattlecamRotationFilter plain;
    plain.commit();
    expect(close(plain.evaluate(turned(40.0), 0.016, 0.0), turned(40.0)), "tau 0 passes the rotation through");
}

// #6687: the first frame after a cut (new shot, new subject, a subject change inside one shot, battlecam re-entry, a
// loaded save) returns the target matrix bit for bit, never the previous subject's filtered rotation. The call
// sequences below are the ones map_mode_hud.cpp makes: reset(), then commit() (advance_movie), then evaluate() (movie_pose),
// evaluate() again (the post-live pass), then commit() at the next frame's start.
void test_first_frame_after_a_cut() {
    const auto turned = [](const double angle, const double scale = 1.0) {
        RotationMatrix m = rotation_filter_detail::matrix_of(axis_angle({0.0, 0.0, 1.0}, angle * degrees));
        for (auto& row : m)
            for (double& value : row) value *= scale;
        return m;
    };
    const auto identical = [](const RotationMatrix& a, const RotationMatrix& b) {
        for (std::size_t r = 0; r < 3; ++r)
            for (std::size_t c = 0; c < 3; ++c)
                if (a[r][c] != b[r][c]) return false;
        return true;
    };
    // The frame after a cut filters a constant target from the cut pose: equal up to the quaternion round trip.
    const auto near = [](const RotationMatrix& a, const RotationMatrix& b) {
        for (std::size_t r = 0; r < 3; ++r)
            for (std::size_t c = 0; c < 3; ++c)
                if (std::abs(a[r][c] - b[r][c]) > 1.0e-9) return false;
        return true;
    };
    // A filter that has been following a subject turning at about 100 degrees per second for a while.
    const auto following = [&](BattlecamRotationFilter& filter) {
        for (int frame = 0; frame < 30; ++frame) {
            filter.commit();
            static_cast<void>(filter.evaluate(turned(2.0 * frame), 1.0 / 120.0));
            static_cast<void>(filter.evaluate(turned(2.0 * frame), 1.0 / 120.0));
        }
    };
    // The next subject sits well inside the snap angle of the old one, so only the reset can make the frame a cut.
    const RotationMatrix next_subject = turned(70.0, 0.7);
    {
        // New shot: reset, the shot's zero-time advance (commit + evaluate), the post-live pass (evaluate), next frame.
        BattlecamRotationFilter filter;
        following(filter);
        filter.reset();
        filter.commit();
        expect(identical(filter.evaluate(next_subject, 0.0), next_subject), "new shot: the first evaluation is the target bit for bit");
        expect(identical(filter.evaluate(next_subject, 1.0 / 120.0), next_subject), "new shot: the post-live evaluation is the target too");
        filter.commit();
        expect(near(filter.evaluate(next_subject, 1.0 / 120.0), next_subject), "new shot: the next frame starts from the cut pose");
    }
    {
        // A shot that expires inside advance_movie: the frame's commit ran first, then the new shot resets.
        BattlecamRotationFilter filter;
        following(filter);
        filter.commit();
        filter.reset();
        filter.commit();
        expect(identical(filter.evaluate(next_subject, 0.0), next_subject), "expiry: reset after this frame's commit still cuts");
    }
    {
        // Subject change without a shot change: movie_pose resets between the frame's commit and its evaluate.
        BattlecamRotationFilter filter;
        following(filter);
        filter.commit();
        filter.reset();
        expect(identical(filter.evaluate(next_subject, 1.0 / 120.0), next_subject), "new subject: the first evaluation is the target");
        expect(identical(filter.evaluate(next_subject, 1.0 / 120.0), next_subject), "new subject: a second evaluation in the frame too");
        filter.commit();
        const RotationMatrix after = filter.evaluate(next_subject, 1.0 / 120.0);
        expect(near(after, next_subject), "new subject: the following frame has no memory of the old subject");
    }
    {
        // A reset with no evaluation before the next commit (the subject was missing for a frame) must not bring the old
        // rotation back.
        BattlecamRotationFilter filter;
        following(filter);
        filter.reset();
        filter.commit();
        filter.commit();
        expect(identical(filter.evaluate(next_subject, 1.0 / 120.0), next_subject), "reset then commits without an evaluation: still a cut");
    }
    {
        // Re-entry (battlecam off, later on again) and a loaded save both come down to a reset on a used filter, or a fresh one.
        BattlecamRotationFilter fresh;
        fresh.commit();
        expect(identical(fresh.evaluate(next_subject, 1.0 / 120.0), next_subject), "fresh filter (load, first entry): the target");
        BattlecamRotationFilter filter;
        following(filter);
        filter.reset();
        filter.reset();
        filter.commit();
        expect(identical(filter.evaluate(next_subject, 1.0 / 120.0), next_subject), "re-entry: the target");
        // The cut then follows normally: a turn on the second frame is lagged, not taken at once.
        filter.commit();
        const RotationMatrix lagged = filter.evaluate(turned(95.0, 0.7), 1.0 / 120.0);
        expect(!identical(lagged, turned(95.0, 0.7)) && !identical(lagged, next_subject), "after the cut the filter works again");
    }
}

void test_frame_rate_independence_and_convergence() {
    // After a 20 degree step the remaining error after 0.1 s (two time constants) is the same at any frame rate.
    std::vector<double> remaining;
    for (const double hz : {60.0, 120.0, 144.0}) {
        BattlecamRotationFilter filter;
        const RotationMatrix start = rotation_filter_detail::matrix_of({0.0, 0.0, 0.0, 1.0});
        const RotationMatrix goal = rotation_filter_detail::matrix_of(axis_angle({0.0, 0.0, 1.0}, 20.0 * degrees));
        filter.commit();
        static_cast<void>(filter.evaluate(start, 0.0));
        RotationMatrix used = start;
        const auto frames = static_cast<int>(std::round(0.1 * hz));
        for (int frame = 0; frame < frames; ++frame) {
            filter.commit();
            used = filter.evaluate(goal, 1.0 / hz);
        }
        remaining.push_back(rotation_filter_detail::angle_degrees(rotation_filter_detail::quat_of(used), rotation_filter_detail::quat_of(goal)));
    }
    expect(std::abs(remaining[0] - remaining[1]) < 0.2 && std::abs(remaining[1] - remaining[2]) < 0.2,
        "the step response after 0.1 s is the same at 60, 120 and 144 Hz");
    expect(remaining[1] > 2.0 && remaining[1] < 3.4, "two time constants leave about 13.5% of a 20 degree step");
    // It converges and holds.
    BattlecamRotationFilter filter;
    const RotationMatrix goal = rotation_filter_detail::matrix_of(axis_angle({0.0, 0.0, 1.0}, 20.0 * degrees));
    filter.commit();
    static_cast<void>(filter.evaluate(rotation_filter_detail::matrix_of({0.0, 0.0, 0.0, 1.0}), 0.0));
    RotationMatrix used{};
    for (int frame = 0; frame < 240; ++frame) {
        filter.commit();
        used = filter.evaluate(goal, 1.0 / 120.0);
    }
    expect(rotation_filter_detail::angle_degrees(rotation_filter_detail::quat_of(used), rotation_filter_detail::quat_of(goal)) < 1.0e-6,
        "the filter converges onto a constant target");
    // The result stays a rotation.
    const auto m = used;
    double determinant = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
        m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    expect(std::abs(determinant - 1.0) < 1.0e-9, "the filtered matrix is a proper rotation");
}

} // namespace

int main() {
    test_filter_takes_the_tick_steps_out_of_the_eye();
    test_lag_bound();
    test_subject_stays_centred_and_offset_length_is_kept();
    test_scaled_subjects();
    test_cut_hold_and_frame_protocol();
    test_first_frame_after_a_cut();
    test_frame_rate_independence_and_convergence();
    if (failures == 0) std::cout << "battlecam_rotation_tests passed\n";
    return failures == 0 ? 0 : 1;
}
