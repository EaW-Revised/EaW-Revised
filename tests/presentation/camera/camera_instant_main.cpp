// #6448 J1: a followed subject must not jiggle against the picture.
//
// Engine-free harness for the camera side of the presentation instant. A fast, turning craft
// is flown on a 30 Hz simulation trajectory and drawn with the live view's interpolation
// (linear between two ticks). A battlecam shot follows it. The harness runs two reading
// orders at synthetic frame cadences:
//   main order: the camera reads the subject's pose from the previous frame's instant while
//               the mesh is drawn at this frame's instant (what the viewer does today: the
//               camera is set before the live step composes the frame's poses);
//   new order:  the camera and the mesh read the same instant.
// For each, the projected screen position of the subject (and of craft that fly with it) is
// recorded per frame and scored with the residual of the PF/SK working metric (not an owner number):
// e_i = dp_i - dp_hat_i, where dp_i is the projected mesh-origin displacement per frame and
// dp_hat_i the same displacement for a camera at the mesh's own instant. The score is
// Var(e_x) + Var(e_y) in px^2/frame^2. The main order is the negative control: it must show
// the jiggle at uneven cadences, or the harness cannot detect it.
//
// WHAT THIS IS: a MODEL of the frame order, not a regression test of the shipped code. The seams in
// camera_instant.hpp re-implement the viewer's shot geometry; no viewer source includes them, so a regression
// in the shipped path (movie_pose, the post-live pass, the follow focus) would NOT fail here. What it does show
// is that reading the subject one frame behind the mesh produces the jiggle at uneven cadences (the negative
// controls, which a real regression test must also be able to detect), and that the camera path and backdrop
// keep their smoothness against the exact orbit. The same-instant order is the reference by construction and is
// not asserted against itself. Testing the shipped functions needs movie_pose and the follow focus extracted
// into pure functions shared by product and test (a follow-up).
//
// Scope: the camera only. The cursor sequence per frame is an input (the clock's output), so
// clock error (uneven steps against the display interval) belongs to the clock's own replay.
#include "../../support/check.hpp"
#include "../../../apps/viewer/src/camera_instant.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace eawr::presentation::godot_backend;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(bool condition, std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAILED: ");
}

constexpr double screen_width = 1920.0;
constexpr double screen_height = 1080.0;
constexpr double vertical_fov = 60.0 * std::numbers::pi / 180.0;
constexpr double ticks_per_second = 30.0;

[[nodiscard]] Vec3d sub(const Vec3d& a, const Vec3d& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
[[nodiscard]] double dot(const Vec3d& a, const Vec3d& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
[[nodiscard]] Vec3d cross(const Vec3d& a, const Vec3d& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
[[nodiscard]] Vec3d unit(const Vec3d& a) {
    const double length = std::sqrt(dot(a, a));
    return {a[0] / length, a[1] / length, a[2] / length};
}

// Pinhole projection through a look-at camera with +z up.
[[nodiscard]] std::array<double, 2> project(const CameraPose& camera, const Vec3d& point) {
    const Vec3d forward = unit(sub(camera.target, camera.eye));
    const Vec3d right = unit(cross(forward, {0.0, 0.0, 1.0}));
    const Vec3d up = cross(right, forward);
    const Vec3d offset = sub(point, camera.eye);
    const double depth = dot(offset, forward);
    const double tangent = std::tan(vertical_fov * 0.5);
    const double aspect = screen_width / screen_height;
    return {(dot(offset, right) / (depth * tangent * aspect) + 1.0) * screen_width * 0.5,
            (1.0 - dot(offset, up) / (depth * tangent)) * screen_height * 0.5};
}

// A craft flying a circle: constant speed, constant turn rate. Its 30 Hz tick poses are exact;
// the drawn pose is linear between two ticks, as the live view interpolates.
struct Craft final {
    double speed{};
    double omega{};
    double phase{};
    Vec3d offset{};
};
struct DrawnPose final {
    Vec3d position{};
    double yaw{};
};

[[nodiscard]] DrawnPose tick_pose(const Craft& craft, const double tick) {
    const double seconds = tick / ticks_per_second;
    const double yaw = craft.omega * seconds + craft.phase;
    const double radius = craft.speed / craft.omega;
    return {{radius * std::sin(yaw) + craft.offset[0], -radius * std::cos(yaw) + craft.offset[1], craft.offset[2]}, yaw};
}

[[nodiscard]] DrawnPose pose_at(const Craft& craft, const double cursor) {
    const double base = std::floor(cursor);
    const double alpha = cursor - base;
    const DrawnPose a = tick_pose(craft, base);
    const DrawnPose b = tick_pose(craft, base + 1.0);
    DrawnPose out;
    for (std::size_t i = 0; i < 3; ++i) out.position[i] = a.position[i] + (b.position[i] - a.position[i]) * alpha;
    out.yaw = a.yaw + (b.yaw - a.yaw) * alpha;
    return out;
}

[[nodiscard]] Mat3d rotation_of(const double yaw) {
    const double c = std::cos(yaw);
    const double s = std::sin(yaw);
    return {{{c, -s, 0.0}, {s, c, 0.0}, {0.0, 0.0, 1.0}}};
}
[[nodiscard]] Vec3d rotate(const Mat3d& m, const Vec3d& v) {
    return {dot(m[0], v), dot(m[1], v), dot(m[2], v)};
}
[[nodiscard]] Vec3d add(const Vec3d& a, const Vec3d& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
[[nodiscard]] Vec3d lerp(const Vec3d& a, const Vec3d& b, const double t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

// Frame cadences: how far the presentation cursor advances each frame, in milliseconds.
struct Cadence final {
    std::string name;
    std::vector<double> step_ms;
};

[[nodiscard]] Cadence steady(const int hz, const std::size_t frames) {
    return {"steady " + std::to_string(hz) + " Hz", std::vector<double>(frames, 1000.0 / hz)};
}
// Vsync FIFO on a 144 Hz panel at ~58 FPS: each frame stays two or three refreshes.
[[nodiscard]] Cadence fifo_alternating(const std::size_t frames) {
    Cadence c{"144 Hz FIFO 2/3 refreshes", {}};
    for (std::size_t i = 0; i < frames; ++i) c.step_ms.push_back(i % 2 == 0 ? 1000.0 / 72.0 : 3000.0 / 144.0);
    return c;
}
// A steady 60 Hz frame rate whose time base is off by up to +/-8.3 ms per frame with the
// error carried to the next frame (the shape of the engine's rounded frame delta).
[[nodiscard]] Cadence rounded_delta(const std::size_t frames) {
    Cadence c{"60 Hz with +/-8.3 ms carried step error", {}};
    std::uint32_t state = 12345U;
    double carried = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        state = state * 1664525U + 1013904223U;
        const double error = (static_cast<double>(state >> 8U) / 16777215.0 * 2.0 - 1.0) * 8.3;
        c.step_ms.push_back(1000.0 / 60.0 + error - carried);
        carried = error;
    }
    return c;
}

enum class Order { main_order, instant_order };

[[nodiscard]] double residual_variance(const std::vector<std::array<double, 2>>& screen,
                                       const std::vector<std::array<double, 2>>& ideal) {
    // e_i = dp_i - dp_hat_i over consecutive frames; population variance of each axis, summed.
    std::vector<std::array<double, 2>> e;
    for (std::size_t i = 1; i < screen.size(); ++i)
        e.push_back({(screen[i][0] - screen[i - 1][0]) - (ideal[i][0] - ideal[i - 1][0]),
                     (screen[i][1] - screen[i - 1][1]) - (ideal[i][1] - ideal[i - 1][1])});
    double result = 0.0;
    for (std::size_t axis = 0; axis < 2; ++axis) {
        double mean = 0.0;
        for (const auto& v : e) mean += v[axis];
        mean /= static_cast<double>(e.size());
        double variance = 0.0;
        for (const auto& v : e) variance += (v[axis] - mean) * (v[axis] - mean);
        result += variance / static_cast<double>(e.size());
    }
    return result;
}
[[nodiscard]] double mean_ideal_speed(const std::vector<std::array<double, 2>>& ideal) {
    double total = 0.0;
    for (std::size_t i = 1; i < ideal.size(); ++i)
        total += std::hypot(ideal[i][0] - ideal[i - 1][0], ideal[i][1] - ideal[i - 1][1]);
    return total / static_cast<double>(ideal.size() - 1);
}

struct Series final {
    std::vector<std::array<double, 2>> subject;   // the followed craft's mesh origin
    std::vector<std::array<double, 2>> wingman;   // a craft flying close to it
    std::vector<std::array<double, 2>> hardpoint; // an enemy aimed at from a weapon attachment
    std::vector<std::array<double, 2>> frigate;   // the aim point of a frigate transfer shot
    double elapsed{};
};

const Craft subject_craft{180.0, 0.6, 0.0, {0.0, 0.0, 0.0}};
const Craft wingman_craft{178.0, 0.58, 0.05, {-12.0, -8.0, 3.0}};
const Craft enemy_craft{150.0, -0.4, 1.0, {600.0, 100.0, 0.0}};

[[nodiscard]] BattlecamShot circle_shot() {
    BattlecamShot shot;
    shot.kind = BattlecamShotKind::circle;
    shot.radius = 60.0F;
    shot.height = 15.0F;
    shot.direction = 1.0F;
    return shot;
}

[[nodiscard]] Series run(const Cadence& cadence, const Order order) {
    Series out;
    const BattlecamShot shot = circle_shot();
    double cursor = 2.0;
    double elapsed = 0.0;
    for (const double step_ms : cadence.step_ms) {
        const double previous_cursor = cursor;
        CameraInstant instant;
        instant.dt_present_ms = step_ms;
        cursor += step_ms * 1.0e-3 * ticks_per_second;
        instant.cursor = cursor;
        elapsed = battlecam_elapsed_after(elapsed, instant);
        const double camera_cursor = order == Order::instant_order ? cursor : previous_cursor;

        // The meshes are always drawn at this frame's instant.
        const DrawnPose subject_mesh = pose_at(subject_craft, cursor);
        const DrawnPose wingman_mesh = pose_at(wingman_craft, cursor);
        const DrawnPose enemy_mesh = pose_at(enemy_craft, cursor);

        // Chase-type shot on the subject.
        const DrawnPose subject_cam = pose_at(subject_craft, camera_cursor);
        const CameraPose chase = battlecam_shot_camera(shot, elapsed, subject_cam.position, rotation_of(subject_cam.yaw), {});
        out.subject.push_back(project(chase, subject_mesh.position));
        out.wingman.push_back(project(chase, wingman_mesh.position));

        // Hardpoint shot: eye at the subject's weapon attachment, aim at the enemy.
        const Mat3d body = rotation_of(subject_cam.yaw);
        const Vec3d attachment = add(subject_cam.position, rotate(body, {4.0, 0.0, 1.0}));
        const Vec3d fire = unit(rotate(body, {0.9, 0.2, 0.3}));
        const DrawnPose enemy_cam = pose_at(enemy_craft, camera_cursor);
        const CameraPose hardpoint = battlecam_hardpoint_camera(attachment, fire, enemy_cam.position);
        out.hardpoint.push_back(project(hardpoint, enemy_mesh.position));

        // Frigate transfer shot: eye flies across both ships, aim slides subject -> enemy.
        const double fraction = std::clamp(elapsed / 6.0, 0.0, 1.0);
        const Vec3d start = add(subject_cam.position, rotate(body, {-100.0, 40.0, 10.0}));
        const Vec3d middle = add(subject_cam.position, rotate(body, {0.0, 80.0, 30.0}));
        const Vec3d end = add(enemy_cam.position, rotate(rotation_of(0.3), {60.0, -30.0, 5.0}));
        const CameraPose frigate = battlecam_frigate_camera(start, middle, end, subject_cam.position, enemy_cam.position, fraction);
        out.frigate.push_back(project(frigate, lerp(subject_mesh.position, enemy_mesh.position, fraction)));
    }
    out.elapsed = elapsed;
    return out;
}

// Followed subjects are centred by construction when the camera reads the mesh's instant:
// their ideal screen displacement is zero.
[[nodiscard]] std::vector<std::array<double, 2>> centred(const std::vector<std::array<double, 2>>& like) {
    return std::vector<std::array<double, 2>>(like.size(), std::array<double, 2>{screen_width * 0.5, screen_height * 0.5});
}

void test_todays_reading_order_jiggles() {
    const std::size_t frames = 600;
    const std::vector<Cadence> cadences{fifo_alternating(frames), rounded_delta(frames)};
    for (const Cadence& cadence : cadences) {
        const Series main_series = run(cadence, Order::main_order);
        const Series new_series = run(cadence, Order::instant_order);  // the camera at the mesh's own instant: the reference
        const std::string label = " (" + cadence.name + ")";

        // The followed subject: the reference is the screen centre, every frame.
        const double subject_main = residual_variance(main_series.subject, centred(main_series.subject));
        expect(subject_main >= 1.0, "negative control: the previous-frame reading jiggles the subject at an uneven cadence" + label);

        // A craft flying with the subject, against the reference camera.
        const double wingman_main = residual_variance(main_series.wingman, new_series.wingman);
        const double wingman_speed = mean_ideal_speed(new_series.wingman);
        expect(wingman_speed > 0.0, "the companion moves on screen (normalisation is defined)" + label);
        expect(wingman_main / (wingman_speed * wingman_speed) >= 0.05,
               "negative control: the previous-frame reading moves the companion unevenly" + label);

        // Hardpoint and frigate shots read bones and a second ship from the same composed frame.
        expect(residual_variance(main_series.hardpoint, centred(main_series.hardpoint)) >= 0.1,
               "negative control: the previous-frame reading jiggles the hardpoint shot" + label);
        expect(residual_variance(main_series.frigate, centred(main_series.frigate)) >= 1.0,
               "negative control: the previous-frame reading jiggles the frigate shot" + label);
    }
}

void test_dt_is_the_display_clock() {
    // Shot time integrates the instant's dt exactly, frame by frame, at every cadence.
    for (const Cadence& cadence : {steady(60, 300), steady(144, 300), fifo_alternating(300), rounded_delta(300)}) {
        double expected = 0.0;
        for (const double step_ms : cadence.step_ms) expected += step_ms * 1.0e-3;
        const Series s = run(cadence, Order::instant_order);
        expect(std::abs(s.elapsed - expected) <= 1.0e-9, "shot elapsed equals the sum of dt_present over the run: " + cadence.name);
    }
    // Pause: the battlecam holds (BCAM-16) while the display clock keeps running for the others.
    CameraInstant paused;
    paused.dt_present_ms = 16.667;
    paused.paused = true;
    expect(battlecam_elapsed_after(1.25, paused) == 1.25, "a paused battlecam shot holds its clock");
    expect(paused.dt_seconds() > 0.0, "free camera, free flight and overview still get dt while paused");
    CameraInstant speed;
    speed.dt_present_ms = 20.0;
    speed.speed_changed = true;
    expect(std::abs(battlecam_elapsed_after(1.0, speed) - 1.02) <= 1.0e-12, "a speed change does not reset or rescale shot time");
    CameraInstant negative;
    negative.dt_present_ms = -5.0;
    expect(battlecam_elapsed_after(1.0, negative) == 1.0, "a negative dt never rewinds a shot");
}

void test_rebase_frame_is_one_jump_not_an_ease() {
    // A group centre that moves with the units, then a clock rebase: the cursor jumps 6 ticks and
    // the centre with it. Easing across the rebase would chase the jump for many frames.
    FollowEase eased;
    FollowEase rebased;
    CameraInstant instant;
    double cursor = 100.0;
    std::array<double, 2> wanted{500.0, 200.0};
    for (int i = 0; i < 30; ++i) {
        cursor += 0.5;
        instant.cursor = cursor;
        wanted[0] += 3.0;
        follow_ease_step(eased, wanted, instant);
        follow_ease_step(rebased, wanted, instant);
    }
    expect(std::abs(rebased.focus[0] - eased.focus[0]) <= 1.0e-12, "both easers agree before the rebase");
    cursor += 6.0;
    wanted = {wanted[0] + 36.0, wanted[1] + 12.0};
    instant.cursor = cursor;
    follow_ease_step(eased, wanted, instant);  // no flag: the main behaviour eases across the jump
    instant.discontinuity = true;
    follow_ease_step(rebased, wanted, instant);
    expect(rebased.focus == wanted, "a discontinuity frame places the focus on the group centre at once");
    expect(eased.focus[0] < wanted[0] - 1.0, "without the flag the focus is still chasing the jump");
    // The next ordinary frame resumes easing from the placed focus (no flag, no jump).
    instant.discontinuity = false;
    instant.cursor = cursor + 0.5;
    const std::array<double, 2> next_wanted{wanted[0] + 3.0, wanted[1]};
    follow_ease_step(rebased, next_wanted, instant);
    expect(rebased.focus[0] > wanted[0] && rebased.focus[0] <= next_wanted[0], "easing resumes after the rebase frame");
    // A pause adds no ticks, so the focus holds on its own.
    const auto held = rebased.focus;
    follow_ease_step(rebased, {next_wanted[0] + 100.0, next_wanted[1]}, instant);
    expect(rebased.focus == held, "a paused (zero-tick) frame holds the focus");
}


// A shot cut: the shot subject changes at frame `cut_frame` and the new shot starts at elapsed 0. The new
// camera must place the new subject for the cut frame itself (not the previous frame's pose of it).
[[nodiscard]] std::vector<std::array<double, 2>> run_cut(const Cadence& cadence, const Order order, const std::size_t cut_frame) {
    std::vector<std::array<double, 2>> out;
    const BattlecamShot shot = circle_shot();
    double cursor = 2.0;
    double elapsed = 0.0;
    for (std::size_t frame = 0; frame < cadence.step_ms.size(); ++frame) {
        const double previous_cursor = cursor;
        CameraInstant instant;
        instant.dt_present_ms = cadence.step_ms[frame];
        cursor += instant.dt_present_ms * 1.0e-3 * ticks_per_second;
        instant.cursor = cursor;
        const bool cut = frame == cut_frame;
        elapsed = cut ? 0.0 : battlecam_elapsed_after(elapsed, instant);
        const Craft& subject = frame < cut_frame ? subject_craft : wingman_craft;
        const double camera_cursor = order == Order::instant_order ? cursor : previous_cursor;
        const DrawnPose mesh = pose_at(subject, cursor);
        const DrawnPose camera = pose_at(subject, camera_cursor);
        out.push_back(project(battlecam_shot_camera(shot, elapsed, camera.position, rotation_of(camera.yaw), {}), mesh.position));
    }
    return out;
}

void test_cut_frame_negative_control() {
    const std::size_t frames = 600;
    const std::size_t cut = 300;
    for (const Cadence& cadence : {fifo_alternating(frames), rounded_delta(frames)}) {
        const auto main_order = run_cut(cadence, Order::main_order, cut);
        const double cx = screen_width * 0.5;
        const double cy = screen_height * 0.5;
        const auto off_centre = [&](const std::array<double, 2>& p) { return std::hypot(p[0] - cx, p[1] - cy); };
        // The cut frame (and the frames after it) under the main order carry the previous-frame offset.
        double cut_error = 0.0;
        for (std::size_t i = cut; i < cut + 20; ++i) cut_error = std::max(cut_error, off_centre(main_order[i]));
        expect(cut_error >= 0.5, "negative control: the main order places the cut frame off the subject: " + cadence.name);
    }
}

// The backdrop: a ring of far directions (a planet, the stars). Their screen motion follows the camera's
// orientation only, so it is the other half of the owner's complaint: the fix must not move the stutter from
// the followed ship to the background. The ideal is the exact analytic orbit at the cursor's time.
constexpr std::size_t backdrop_directions = 12;
struct BackdropFrame final {
    std::array<bool, backdrop_directions> visible{};
    std::array<std::array<double, 2>, backdrop_directions> screen{};
    Vec3d eye{};
    Vec3d target{};
};
[[nodiscard]] bool project_direction(const CameraPose& camera, const Vec3d& direction, std::array<double, 2>& out) {
    const Vec3d forward = unit(sub(camera.target, camera.eye));
    const Vec3d right = unit(cross(forward, {0.0, 0.0, 1.0}));
    const Vec3d up = cross(right, forward);
    const double depth = dot(direction, forward);
    if (depth <= 0.5) return false;  // outside +/-60 degrees of the view axis: not on screen
    const double tangent = std::tan(vertical_fov * 0.5);
    out = {(dot(direction, right) / (depth * tangent * (screen_width / screen_height)) + 1.0) * screen_width * 0.5,
           (1.0 - dot(direction, up) / (depth * tangent)) * screen_height * 0.5};
    return true;
}

enum class Reading { previous_frame, same_instant, ideal_orbit };

[[nodiscard]] std::vector<BackdropFrame> run_backdrop(const Cadence& cadence, const Reading reading) {
    std::vector<BackdropFrame> out;
    const BattlecamShot shot = circle_shot();
    double cursor = 2.0;
    double elapsed = 0.0;
    for (const double step_ms : cadence.step_ms) {
        const double previous_cursor = cursor;
        CameraInstant instant;
        instant.dt_present_ms = step_ms;
        cursor += step_ms * 1.0e-3 * ticks_per_second;
        elapsed = battlecam_elapsed_after(elapsed, instant);
        DrawnPose pose;
        if (reading == Reading::ideal_orbit) pose = tick_pose(subject_craft, cursor);
        else pose = pose_at(subject_craft, reading == Reading::same_instant ? cursor : previous_cursor);
        const CameraPose camera = battlecam_shot_camera(shot, elapsed, pose.position, rotation_of(pose.yaw), {});
        BackdropFrame frame;
        frame.eye = camera.eye;
        frame.target = camera.target;
        for (std::size_t i = 0; i < backdrop_directions; ++i) {
            const double angle = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(backdrop_directions);
            frame.visible[i] = project_direction(camera, {std::cos(angle), std::sin(angle), 0.15}, frame.screen[i]);
        }
        out.push_back(frame);
    }
    return out;
}

// Var(ex)+Var(ey) of the backdrop's per-frame screen displacement against the ideal orbit, over every direction
// and frame pair on screen in both; and the same for the eye/target path in world units^2.
struct PathResidual final {
    double backdrop_px2{};
    double eye_units2{};
    double target_units2{};
    std::size_t samples{};
};
[[nodiscard]] PathResidual path_residual(const std::vector<BackdropFrame>& actual, const std::vector<BackdropFrame>& ideal) {
    std::vector<std::array<double, 2>> e;
    std::vector<Vec3d> eye_e;
    std::vector<Vec3d> target_e;
    for (std::size_t i = 1; i < actual.size(); ++i) {
        for (std::size_t k = 0; k < backdrop_directions; ++k) {
            if (!actual[i].visible[k] || !actual[i - 1].visible[k] || !ideal[i].visible[k] || !ideal[i - 1].visible[k]) continue;
            e.push_back({(actual[i].screen[k][0] - actual[i - 1].screen[k][0]) - (ideal[i].screen[k][0] - ideal[i - 1].screen[k][0]),
                         (actual[i].screen[k][1] - actual[i - 1].screen[k][1]) - (ideal[i].screen[k][1] - ideal[i - 1].screen[k][1])});
        }
        Vec3d de{};
        Vec3d dt{};
        for (std::size_t a = 0; a < 3; ++a) {
            de[a] = (actual[i].eye[a] - actual[i - 1].eye[a]) - (ideal[i].eye[a] - ideal[i - 1].eye[a]);
            dt[a] = (actual[i].target[a] - actual[i - 1].target[a]) - (ideal[i].target[a] - ideal[i - 1].target[a]);
        }
        eye_e.push_back(de);
        target_e.push_back(dt);
    }
    PathResidual r;
    r.samples = e.size();
    const auto variance = [](const auto& values, const std::size_t axis) {
        double mean = 0.0;
        for (const auto& v : values) mean += v[axis];
        mean /= static_cast<double>(values.size());
        double sum = 0.0;
        for (const auto& v : values) sum += (v[axis] - mean) * (v[axis] - mean);
        return sum / static_cast<double>(values.size());
    };
    r.backdrop_px2 = variance(e, 0) + variance(e, 1);
    for (std::size_t a = 0; a < 3; ++a) {
        r.eye_units2 += variance(eye_e, a);
        r.target_units2 += variance(target_e, a);
    }
    return r;
}

void test_backdrop_and_camera_path_stay_smooth() {
    const std::size_t frames = 600;
    const std::vector<Cadence> cadences{steady(60, frames), steady(120, frames), steady(144, frames),
                                        fifo_alternating(frames), rounded_delta(frames)};
    for (const Cadence& cadence : cadences) {
        const auto ideal = run_backdrop(cadence, Reading::ideal_orbit);
        const PathResidual main_order = path_residual(run_backdrop(cadence, Reading::previous_frame), ideal);
        const PathResidual instant_order = path_residual(run_backdrop(cadence, Reading::same_instant), ideal);
        const std::string label = " (" + cadence.name + ")";
        expect(instant_order.samples > 500, "the backdrop ring has on-screen samples" + label);
        // The stutter must not move from the ship to the background: against the exact orbit, the backdrop and the
        // eye/target path are as smooth as the clock allows (interpolation between 30 Hz ticks only).
        expect(instant_order.backdrop_px2 <= 1.0e-6, "backdrop screen motion matches the ideal orbit" + label);
        expect(instant_order.eye_units2 <= 1.0e-2 && instant_order.target_units2 <= 1.0e-2,
               "eye and target per-frame motion matches the ideal orbit" + label);
        expect(instant_order.backdrop_px2 <= main_order.backdrop_px2 * 1.05 + 1.0e-12, "the backdrop is never worse than today" + label);
        expect(instant_order.eye_units2 <= main_order.eye_units2 * 1.05 + 1.0e-4, "the eye path is never worse than today" + label);
        expect(instant_order.target_units2 <= main_order.target_units2 * 1.05 + 1.0e-4, "the aim path is never worse than today" + label);
        if (cadence.name.find("FIFO") != std::string::npos || cadence.name.find("error") != std::string::npos) {
            expect(main_order.backdrop_px2 >= 1.0, "negative control: today's backdrop carries the lag at an uneven cadence" + label);
        }
    }
}

// The C key (unit preview, #6446/#6453/#6455) follows the unit with an orbit camera: eye at the subject plus the
// preview offset rotated by the subject's drawn rotation, aim on the subject. Owner cases: a Corellian corvette
// (slow, large: the preview sits at 3 x its radius) and fighters.
void test_c_mode_negative_control() {
    struct Case {
        const char* name;
        Craft craft;
        float distance;
    };
    const Case cases[] = {{"fighter", subject_craft, 90.0F}, {"corvette", Craft{60.0, 0.2, 0.0, {0.0, 0.0, 0.0}}, 600.0F}};
    const std::size_t frames = 600;
    const std::vector<Cadence> cadences{steady(60, frames), steady(144, frames), fifo_alternating(frames), rounded_delta(frames)};
    for (const Case& c : cases) {
        for (const Cadence& cadence : cadences) {
            std::vector<std::array<double, 2>> previous_reading;
            double cursor = 2.0;
            for (const double step_ms : cadence.step_ms) {
                const double previous_cursor = cursor;
                cursor += step_ms * 1.0e-3 * ticks_per_second;
                const DrawnPose mesh = pose_at(c.craft, cursor);
                const DrawnPose read = pose_at(c.craft, previous_cursor);
                const CameraPose camera = battlecam_preview_camera(read.position, rotation_of(read.yaw), c.distance, 0.0F, 30.0F);
                previous_reading.push_back(project(camera, mesh.position));
            }
            const std::string label = std::string(" (") + c.name + ", " + cadence.name + ")";
            const double before = residual_variance(previous_reading, centred(previous_reading));
            if (cadence.name.find("FIFO") != std::string::npos || cadence.name.find("error") != std::string::npos) {
                expect(before >= 0.05, "negative control: the previous-frame reading jiggles the C-key followed unit at an uneven cadence" + label);
            }
        }
    }
}

} // namespace

int main() {
    test_todays_reading_order_jiggles();
    test_dt_is_the_display_clock();
    test_rebase_frame_is_one_jump_not_an_ease();
    test_cut_frame_negative_control();
    test_backdrop_and_camera_path_stay_smooth();
    test_c_mode_negative_control();
    if (failures == 0) std::cout << "camera_instant_tests passed\n";
    return failures == 0 ? 0 : 1;
}
