// P1-09 cinematic free-camera controller contracts.
//
// Exercises include/eawr/presentation/camera/free_camera.hpp through its public
// API only. Every setting below is project-authored test data; nothing here is
// a retail speed, sensitivity or bound. Expectations that involve trigonometry
// use a stated tolerance; expectations about rejection compare exact state, so
// a rejected step is proven to leave nothing partially applied.

#include "eawr/presentation/camera/camera.hpp"
#include "eawr/presentation/camera/free_camera.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>

namespace {
namespace camera = eawr::presentation::camera;

int failures{};

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void expect_close(const float actual, const float expected, const std::string_view message,
                  const float tolerance = 1e-3F) {
    if (!(std::abs(actual - expected) <= tolerance)) {
        std::cerr << "FAILED: " << message << " (expected " << expected << ", got " << actual
                  << ")\n";
        ++failures;
    }
}

constexpr float infinity = std::numeric_limits<float>::infinity();
constexpr float nan = std::numeric_limits<float>::quiet_NaN();
constexpr float largest = std::numeric_limits<float>::max();

camera::FreeCameraSettings settings() {
    camera::FreeCameraSettings value;
    value.move_speed = 100.0F;
    value.vertical_speed = 50.0F;
    value.look_degrees_per_unit = 0.5F;
    value.pitch_min_degrees = -80.0F;
    value.pitch_max_degrees = 80.0F;
    return value;
}

camera::FreeCameraPose level_pose() {
    return camera::FreeCameraPose{{10.0F, 20.0F, 30.0F}, 0.0F, 0.0F};
}

float length(const std::array<float, 3>& value) {
    return std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
}

std::array<float, 3> displacement(const camera::FreeCameraPose& from,
                                  const camera::FreeCameraPose& to) {
    return {to.eye[0] - from.eye[0], to.eye[1] - from.eye[1], to.eye[2] - from.eye[2]};
}

camera::FreeCameraPose advance(const camera::FreeCameraPose& pose,
                               const camera::FreeCameraIntent& intent, const float seconds,
                               const std::string_view what) {
    auto next = camera::advance_free_camera(settings(), pose, intent, seconds);
    expect(next.has_value(), what);
    return next ? next.value() : pose;
}

void expect_rejected(const camera::FreeCameraSettings& tuning, const camera::FreeCameraPose& pose,
                     const camera::FreeCameraIntent& intent, const float seconds,
                     const std::string_view code, const std::string_view message) {
    auto result = camera::advance_free_camera(tuning, pose, intent, seconds);
    if (result) {
        std::cerr << "FAILED: " << message << " (unexpectedly accepted)\n";
        ++failures;
        return;
    }
    if (result.error().code != code) {
        std::cerr << "FAILED: " << message << " (expected " << code << ", got "
                  << result.error().code << ")\n";
        ++failures;
    }
}

void test_settings_validation() {
    expect(camera::validate(settings()).has_value(), "project test settings validate");
    const auto rejects = [](const camera::FreeCameraSettings& value, const std::string_view what) {
        auto valid = camera::validate(value);
        expect(!valid.has_value()
                   && valid.error().code == camera::diagnostic_codes::invalid_free_settings,
               what);
    };
    auto value = settings();
    value.move_speed = 0.0F;
    rejects(value, "zero move speed is rejected");
    value = settings();
    value.vertical_speed = -1.0F;
    rejects(value, "negative vertical speed is rejected");
    value = settings();
    value.look_degrees_per_unit = 0.0F;
    rejects(value, "zero look rate is rejected");
    value = settings();
    value.move_speed = infinity;
    rejects(value, "infinite speed is rejected");
    value = settings();
    value.pitch_max_degrees = nan;
    rejects(value, "NaN pitch bound is rejected");
    value = settings();
    value.pitch_min_degrees = 10.0F;
    value.pitch_max_degrees = 10.0F;
    rejects(value, "empty pitch range is rejected");
    value = settings();
    value.pitch_min_degrees = 20.0F;
    value.pitch_max_degrees = 10.0F;
    rejects(value, "inverted pitch range is rejected");
    value = settings();
    value.pitch_max_degrees = 90.0F;
    rejects(value, "pitch bound at the vertical is rejected");
    value = settings();
    value.pitch_min_degrees = -89.5F;
    rejects(value, "pitch bound beyond the limit is rejected");
    value = settings();
    value.pitch_min_degrees = -camera::free_pitch_limit_degrees;
    value.pitch_max_degrees = camera::free_pitch_limit_degrees;
    expect(camera::validate(value).has_value(), "the full permitted pitch range validates");
}

void test_entry_matches_the_tactical_view() {
    // A tactical camera looking at a target from yaw/pitch: the free camera's
    // forward vector must point from that eye back at the target.
    const std::array<float, 3> target{5.0F, 0.0F, -7.0F};
    const float distance = 300.0F;
    const float pitch = 40.0F;
    for (const float yaw : {0.0F, 37.0F, -135.0F, 725.0F, -999.0F}) {
        auto eye = camera::eye_position(std::span<const float, 3>{target}, distance, pitch, yaw);
        expect(eye.has_value(), "tactical eye solves");
        auto entered = camera::enter_free_camera(
            settings(), std::span<const float, 3>{eye.value()}, yaw, pitch);
        expect(entered.has_value(), "entry from a tactical pose succeeds");
        if (!entered) continue;
        const camera::FreeCameraPose& pose = entered.value();
        expect(pose.eye == eye.value(), "entry keeps the exact eye");
        expect(pose.pitch_degrees == pitch, "entry keeps the exact pitch");
        expect(pose.yaw_degrees >= -180.0F && pose.yaw_degrees < 180.0F, "entry wraps yaw");
        auto forward = camera::free_camera_forward(pose);
        expect(forward.has_value(), "forward is defined");
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const float expected = (target[axis] - eye.value()[axis]) / distance;
            expect_close(forward.value()[axis], expected, "entry view points at the target", 1e-4F);
        }
    }

    const std::array<float, 3> eye{0.0F, 0.0F, 0.0F};
    auto steep = camera::enter_free_camera(settings(), std::span<const float, 3>{eye}, 0.0F, 85.0F);
    expect(!steep.has_value()
               && steep.error().code == camera::diagnostic_codes::invalid_free_request,
           "entry outside the pitch bounds is rejected rather than turned");
    const std::array<float, 3> bad_eye{0.0F, nan, 0.0F};
    expect(!camera::enter_free_camera(settings(), std::span<const float, 3>{bad_eye}, 0.0F, 0.0F)
                .has_value(),
           "entry with a non-finite eye is rejected");
    expect(!camera::enter_free_camera(settings(), std::span<const float, 3>{eye}, infinity, 0.0F)
                .has_value(),
           "entry with a non-finite yaw is rejected");
    auto broken = settings();
    broken.move_speed = 0.0F;
    expect(!camera::enter_free_camera(broken, std::span<const float, 3>{eye}, 0.0F, 0.0F)
                .has_value(),
           "entry with invalid settings is rejected");
}

void test_translation_axes() {
    const camera::FreeCameraPose pose = level_pose();
    auto forward = advance(pose, {0.0F, 1.0F, 0.0F, 0.0F, 0.0F}, 0.5F, "forward step");
    expect_close(forward.eye[0], 10.0F, "level forward at yaw 0 keeps X");
    expect_close(forward.eye[1], 20.0F, "level forward keeps Y");
    expect_close(forward.eye[2], 30.0F - 50.0F, "level forward at yaw 0 flies -Z at move_speed");
    auto back = advance(pose, {0.0F, -1.0F, 0.0F, 0.0F, 0.0F}, 0.5F, "back step");
    expect_close(back.eye[2], 30.0F + 50.0F, "back flies +Z");
    auto right = advance(pose, {1.0F, 0.0F, 0.0F, 0.0F, 0.0F}, 0.5F, "strafe step");
    expect_close(right.eye[0], 10.0F + 50.0F, "strafe right at yaw 0 is +X");
    expect_close(right.eye[2], 30.0F, "strafe keeps Z");
    auto rise = advance(pose, {0.0F, 0.0F, 1.0F, 0.0F, 0.0F}, 0.5F, "rise step");
    expect(rise.eye[0] == pose.eye[0] && rise.eye[2] == pose.eye[2],
           "rise changes only the vertical axis");
    expect_close(rise.eye[1], 20.0F + 25.0F, "rise uses vertical_speed");
    expect(rise.yaw_degrees == pose.yaw_degrees && rise.pitch_degrees == pose.pitch_degrees,
           "translation never turns the view");

    // Flight follows the view direction, including its pitch.
    camera::FreeCameraPose down = pose;
    down.pitch_degrees = 30.0F;
    auto dive = advance(down, {0.0F, 1.0F, 0.0F, 0.0F, 0.0F}, 1.0F, "pitched forward step");
    expect_close(length(displacement(down, dive)), 100.0F, "pitched forward keeps move_speed");
    expect_close(dive.eye[1] - down.eye[1], -100.0F * std::sin(30.0F * 0.017453292F),
                 "pitched forward descends along the view", 1e-2F);

    // Yaw rotates the basis: at yaw 90 the view looks along -X.
    camera::FreeCameraPose turned = pose;
    turned.yaw_degrees = 90.0F;
    auto turned_forward = advance(turned, {0.0F, 1.0F, 0.0F, 0.0F, 0.0F}, 1.0F, "yaw 90 forward");
    expect_close(turned_forward.eye[0], 10.0F - 100.0F, "yaw 90 forward is -X", 1e-2F);
    expect_close(turned_forward.eye[2], 30.0F, "yaw 90 forward keeps Z", 1e-2F);
}

void test_diagonal_normalisation() {
    const camera::FreeCameraPose pose = level_pose();
    auto diagonal = advance(pose, {1.0F, 1.0F, 0.0F, 0.0F, 0.0F}, 1.0F, "diagonal step");
    expect_close(length(displacement(pose, diagonal)), 100.0F,
                 "forward+strafe does not exceed move_speed", 1e-2F);
    expect_close(diagonal.eye[0] - pose.eye[0], 100.0F / std::sqrt(2.0F),
                 "diagonal splits evenly", 1e-2F);
    auto all_axes = advance(pose, {1.0F, 1.0F, 1.0F, 0.0F, 0.0F}, 1.0F, "three-axis step");
    const float third = 1.0F / std::sqrt(3.0F);
    expect_close(all_axes.eye[1] - pose.eye[1], 50.0F * third,
                 "three-axis intent is normalised before per-axis speeds", 1e-2F);
    expect_close(all_axes.eye[0] - pose.eye[0], 100.0F * third, "three-axis strafe share", 1e-2F);
    // Analog intent shorter than unit length is kept, not stretched.
    auto half = advance(pose, {0.0F, 0.5F, 0.0F, 0.0F, 0.0F}, 1.0F, "half forward");
    expect_close(length(displacement(pose, half)), 50.0F, "short intent is not normalised up");
    auto opposed = advance(pose, {0.0F, 0.0F, 0.0F, 0.0F, 0.0F}, 1.0F, "no intent");
    expect(opposed == pose, "zero intent leaves the pose bit-identical");
}

// A pitched forward is not orthogonal to world up, so forward plus a vertical
// axis can reinforce. The world displacement is capped at the length the same
// (normalised) intent would give on orthogonal axes.
float orthogonal_length(const camera::FreeCameraSettings& tuning,
                        const camera::FreeCameraIntent& intent, const float seconds) {
    const float squared = intent.strafe * intent.strafe + intent.forward * intent.forward
        + intent.rise * intent.rise;
    const float normaliser = squared > 1.0F ? 1.0F / std::sqrt(squared) : 1.0F;
    const float planar = tuning.move_speed * seconds * normaliser;
    const float vertical = tuning.vertical_speed * seconds * normaliser;
    return std::sqrt((intent.strafe * intent.strafe + intent.forward * intent.forward) * planar
                         * planar
                     + intent.rise * intent.rise * vertical * vertical);
}

camera::FreeCameraPose advance_with(const camera::FreeCameraSettings& tuning,
                                    const camera::FreeCameraPose& pose,
                                    const camera::FreeCameraIntent& intent,
                                    const std::string_view what) {
    auto next = camera::advance_free_camera(tuning, pose, intent, 1.0F);
    expect(next.has_value(), what);
    return next ? next.value() : pose;
}

void test_pitched_vertical_cap() {
    camera::FreeCameraSettings tuning = settings();
    tuning.move_speed = 600.0F;
    tuning.vertical_speed = 400.0F;
    const camera::FreeCameraPose origin{{0.0F, 0.0F, 0.0F}, 25.0F, 60.0F};

    // The reported case: forward+descend at pitch 60 used to fly 683.99 u/s.
    const camera::FreeCameraIntent dive_down{0.0F, 1.0F, -1.0F, 0.0F, 0.0F};
    const auto dove = advance_with(tuning, origin, dive_down, "pitched forward+descend");
    const float dove_length = length(displacement(origin, dove));
    expect(dove_length <= 600.0F, "pitched forward+descend never exceeds move_speed");
    expect_close(dove_length, orthogonal_length(tuning, dive_down, 1.0F),
                 "pitched forward+descend is capped at its orthogonal length", 1e-2F);
    expect_close(dove_length, std::sqrt(0.5F * 600.0F * 600.0F + 0.5F * 400.0F * 400.0F),
                 "pitched forward+descend flies sqrt((600^2 + 400^2) / 2)", 1e-2F);

    // The cap scales uniformly, so the direction is the uncapped one.
    const auto forward = camera::free_camera_forward(origin).value();
    const float half = 1.0F / std::sqrt(2.0F);
    const std::array<float, 3> raw{forward[0] * 600.0F * half,
                                   forward[1] * 600.0F * half - 400.0F * half,
                                   forward[2] * 600.0F * half};
    const auto moved = displacement(origin, dove);
    const float raw_length = length(raw);
    expect(raw_length > 683.9F && raw_length < 684.1F, "uncapped pitched dive would be 684");
    for (std::size_t axis = 0; axis < 3; ++axis) {
        expect_close(moved[axis] / dove_length, raw[axis] / raw_length,
                     "the cap keeps the combined direction", 1e-4F);
    }

    // Backward+rise is the exact mirror, and so is backward+descend of
    // forward+rise.
    const auto climbed = advance_with(tuning, origin, {0.0F, -1.0F, 1.0F, 0.0F, 0.0F},
                                      "pitched backward+rise");
    for (std::size_t axis = 0; axis < 3; ++axis) {
        expect(climbed.eye[axis] == -dove.eye[axis], "backward+rise mirrors forward+descend");
    }
    const auto against = advance_with(tuning, origin, {0.0F, 1.0F, 1.0F, 0.0F, 0.0F},
                                      "pitched forward+rise");
    const auto against_back = advance_with(tuning, origin, {0.0F, -1.0F, -1.0F, 0.0F, 0.0F},
                                           "pitched backward+descend");
    for (std::size_t axis = 0; axis < 3; ++axis) {
        expect(against_back.eye[axis] == -against.eye[axis],
               "backward+descend mirrors forward+rise");
    }
    // Opposing forward and rise partially cancel; the cap never boosts them.
    const float cancelled = std::sqrt(
        (600.0F * 600.0F + 400.0F * 400.0F + 2.0F * 600.0F * 400.0F * forward[1]) * 0.5F);
    expect_close(length(displacement(origin, against)), cancelled,
                 "pitched forward+rise keeps its uncapped, shorter length", 1e-2F);

    // Single axes keep their authored speeds at any pitch.
    const auto ahead = advance_with(tuning, origin, {0.0F, 1.0F, 0.0F, 0.0F, 0.0F},
                                    "pitched pure forward");
    expect_close(length(displacement(origin, ahead)), 600.0F, "pure forward keeps move_speed",
                 1e-2F);
    const auto up = advance_with(tuning, origin, {0.0F, 0.0F, 1.0F, 0.0F, 0.0F}, "pure rise");
    expect(up.eye[0] == 0.0F && up.eye[1] == 400.0F && up.eye[2] == 0.0F,
           "pure rise keeps vertical_speed exactly");
    const auto down = advance_with(tuning, origin, {0.0F, 0.0F, -1.0F, 0.0F, 0.0F},
                                   "pure descend");
    expect(down.eye[1] == -400.0F, "pure descend keeps vertical_speed exactly");

    // Analog intent on the unit circle is not normalised, but is still capped,
    // and a shorter analog intent is capped proportionally, never stretched.
    const camera::FreeCameraIntent analog{0.0F, 0.6F, -0.8F, 0.0F, 0.0F};
    const auto analog_step = advance_with(tuning, origin, analog, "analog pitched dive");
    expect_close(length(displacement(origin, analog_step)),
                 std::sqrt(360.0F * 360.0F + 320.0F * 320.0F),
                 "analog pitched dive is capped at its orthogonal length", 1e-2F);
    const auto analog_half = advance_with(tuning, origin, {0.0F, 0.3F, -0.4F, 0.0F, 0.0F},
                                          "half analog pitched dive");
    expect_close(length(displacement(origin, analog_half)),
                 0.5F * length(displacement(origin, analog_step)),
                 "half analog intent flies half as far", 1e-2F);

    // Unequal speeds the other way round, looking up: forward+rise reinforces
    // and is capped at vertical_speed's side of the ellipse.
    camera::FreeCameraSettings climber = settings();
    climber.move_speed = 100.0F;
    climber.vertical_speed = 300.0F;
    const camera::FreeCameraPose looking_up{{0.0F, 0.0F, 0.0F}, -40.0F, -45.0F};
    const camera::FreeCameraIntent climb{0.0F, 1.0F, 1.0F, 0.0F, 0.0F};
    const auto climbed_up = advance_with(climber, looking_up, climb, "upward forward+rise");
    const float climbed_length = length(displacement(looking_up, climbed_up));
    expect(climbed_length <= 300.0F, "upward forward+rise never exceeds vertical_speed");
    expect_close(climbed_length, orthogonal_length(climber, climb, 1.0F),
                 "upward forward+rise is capped at its orthogonal length", 1e-2F);

    // Sweep: every pitch and a grid of intents stays inside both the
    // orthogonal length and the larger authored speed, and negating an
    // intent negates the displacement exactly.
    const std::array<float, 5> levels{-1.0F, -0.5F, 0.0F, 0.5F, 1.0F};
    int rejected{};
    int over_orthogonal{};
    int over_ceiling{};
    int asymmetric{};
    for (const auto& sweep : {tuning, climber}) {
        const float ceiling = std::max(sweep.move_speed, sweep.vertical_speed);
        for (float pitch = -80.0F; pitch <= 80.0F; pitch += 10.0F) {
            const camera::FreeCameraPose from{{0.0F, 0.0F, 0.0F}, 70.0F, pitch};
            for (const float s : levels) {
                for (const float f : levels) {
                    for (const float r : levels) {
                        const camera::FreeCameraIntent intent{s, f, r, 0.0F, 0.0F};
                        const camera::FreeCameraIntent mirror{-s, -f, -r, 0.0F, 0.0F};
                        auto step = camera::advance_free_camera(sweep, from, intent, 1.0F);
                        auto back = camera::advance_free_camera(sweep, from, mirror, 1.0F);
                        if (!step || !back) {
                            ++rejected;
                            continue;
                        }
                        const float moved_length = length(step.value().eye);
                        const float limit = orthogonal_length(sweep, intent, 1.0F);
                        if (moved_length > limit * (1.0F + 1e-5F) + 1e-3F) ++over_orthogonal;
                        if (moved_length > ceiling * (1.0F + 1e-5F)) ++over_ceiling;
                        for (std::size_t axis = 0; axis < 3; ++axis) {
                            if (back.value().eye[axis] != -step.value().eye[axis]) ++asymmetric;
                        }
                    }
                }
            }
        }
    }
    expect(rejected == 0, "every sweep step is accepted");
    expect(over_orthogonal == 0, "no sweep step exceeds its orthogonal length");
    expect(over_ceiling == 0, "no sweep step exceeds the larger authored speed");
    expect(asymmetric == 0, "every sweep mirror intent mirrors the displacement");
}

void test_look_yaw_and_pitch() {
    const camera::FreeCameraPose pose = level_pose();
    auto right = advance(pose, {0.0F, 0.0F, 0.0F, 20.0F, 0.0F}, 0.0F, "look right");
    expect_close(right.yaw_degrees, -10.0F, "positive look yaw turns right (yaw decreases)");
    expect(right.eye == pose.eye, "look never translates");
    auto forward = camera::free_camera_forward(right);
    expect(forward.has_value() && forward.value()[0] > 0.0F,
           "after turning right the view leans toward +X");
    auto down = advance(pose, {0.0F, 0.0F, 0.0F, 0.0F, 20.0F}, 0.0F, "look down");
    expect_close(down.pitch_degrees, 10.0F, "positive look pitch tilts down");

    // Pitch clamps to the bounds; yaw wraps in [-180, 180).
    auto floor = advance(pose, {0.0F, 0.0F, 0.0F, 0.0F, 1000.0F}, 0.0F, "look far down");
    expect(floor.pitch_degrees == 80.0F, "pitch clamps at pitch_max_degrees");
    auto ceiling = advance(pose, {0.0F, 0.0F, 0.0F, 0.0F, -1000.0F}, 0.0F, "look far up");
    expect(ceiling.pitch_degrees == -80.0F, "pitch clamps at pitch_min_degrees");
    camera::FreeCameraPose near_seam = pose;
    near_seam.yaw_degrees = 175.0F;
    auto wrapped = advance(near_seam, {0.0F, 0.0F, 0.0F, -20.0F, 0.0F}, 0.0F, "wrap across 180");
    expect_close(wrapped.yaw_degrees, -175.0F, "yaw wraps across +180 to -175");
    auto spun = advance(pose, {0.0F, 0.0F, 0.0F, 7200.0F + 40.0F, 0.0F}, 0.0F, "many turns");
    expect_close(spun.yaw_degrees, -20.0F, "ten full turns wrap back into range", 1e-2F);
    expect(spun.yaw_degrees >= -180.0F && spun.yaw_degrees < 180.0F, "wrapped yaw stays in range");
    expect(camera::wrap_yaw_degrees(180.0F) == -180.0F, "+180 wraps to -180");
    expect(camera::wrap_yaw_degrees(-180.0F) == -180.0F, "-180 is in range");
    expect(camera::wrap_yaw_degrees(540.0F) == -180.0F, "540 wraps to -180");
    expect(camera::wrap_yaw_degrees(-190.0F) == 170.0F, "-190 wraps to 170");

    // Look is applied before translation, so the step flies along the new view.
    auto turn_and_fly = advance(pose, {0.0F, 1.0F, 0.0F, -180.0F, 0.0F}, 1.0F, "turn and fly");
    expect_close(turn_and_fly.yaw_degrees, 90.0F, "look yaw applied");
    expect_close(turn_and_fly.eye[0], 10.0F - 100.0F, "then flight uses the turned basis", 1e-2F);
}

void test_zero_duration() {
    const camera::FreeCameraPose pose = level_pose();
    auto still = advance(pose, {1.0F, 1.0F, 1.0F, 0.0F, 0.0F}, 0.0F, "zero duration step");
    expect(still == pose, "zero duration never translates");
    auto look = advance(pose, {1.0F, 1.0F, 1.0F, 4.0F, 4.0F}, 0.0F, "zero duration look");
    expect(look.eye == pose.eye && look.yaw_degrees != pose.yaw_degrees
               && look.pitch_degrees != pose.pitch_degrees,
           "zero duration still applies look deltas");
}

void test_malformed_and_overflow_inputs_are_atomic() {
    const auto code = camera::diagnostic_codes::invalid_free_request;
    const camera::FreeCameraPose pose = level_pose();
    const camera::FreeCameraIntent move{0.0F, 1.0F, 0.0F, 0.0F, 0.0F};
    expect_rejected(settings(), pose, move, -0.1F, code, "negative duration is rejected");
    expect_rejected(settings(), pose, move, nan, code, "NaN duration is rejected");
    expect_rejected(settings(), pose, move, infinity, code, "infinite duration is rejected");
    expect_rejected(settings(), pose, {nan, 0.0F, 0.0F, 0.0F, 0.0F}, 0.1F, code,
                    "NaN strafe is rejected");
    expect_rejected(settings(), pose, {0.0F, 0.0F, 0.0F, infinity, 0.0F}, 0.1F, code,
                    "infinite look is rejected");
    expect_rejected(settings(), pose, {0.0F, 1.5F, 0.0F, 0.0F, 0.0F}, 0.1F, code,
                    "an out-of-range translation axis is rejected, not clamped");
    expect_rejected(settings(), pose, {0.0F, 0.0F, -2.0F, 0.0F, 0.0F}, 0.1F, code,
                    "an out-of-range rise axis is rejected");
    // Finite inputs whose products overflow.
    auto steep_look = settings();
    steep_look.look_degrees_per_unit = 4.0F;
    expect_rejected(steep_look, pose, {0.0F, 0.0F, 0.0F, largest, 0.0F}, 0.1F, code,
                    "a look delta that overflows is rejected");
    expect_rejected(steep_look, pose, {0.0F, 0.0F, 0.0F, 0.0F, -largest}, 0.1F, code,
                    "a pitch delta that overflows is rejected");
    expect_rejected(settings(), pose, move, largest, code,
                    "a duration whose travel overflows is rejected");
    camera::FreeCameraPose far = pose;
    far.eye[2] = -largest;
    expect_rejected(settings(), far, move, 1.0e30F, code,
                    "a translation that overflows the eye is rejected");
    camera::FreeCameraPose broken = pose;
    broken.eye[0] = nan;
    expect_rejected(settings(), broken, {}, 0.1F, code, "a non-finite pose is rejected");
    broken = pose;
    broken.pitch_degrees = 85.0F;
    expect_rejected(settings(), broken, {}, 0.1F, code, "an out-of-bounds pose is rejected");
    auto bad_settings = settings();
    bad_settings.look_degrees_per_unit = -1.0F;
    expect_rejected(bad_settings, pose, {}, 0.1F, camera::diagnostic_codes::invalid_free_settings,
                    "invalid settings are rejected on every step");

    // The controller commits nothing from a rejected step.
    const std::array<float, 3> eye = pose.eye;
    auto created = camera::FreeCameraController::create(
        settings(), std::span<const float, 3>{eye}, 390.0F, 10.0F);
    expect(created.has_value(), "controller creates from a valid pose");
    if (!created) return;
    camera::FreeCameraController controller = created.value();
    expect_close(controller.pose().yaw_degrees, 30.0F, "controller wraps the entry yaw");
    expect(controller.settings() == settings(), "controller keeps its settings");
    expect(controller.advance({0.0F, 1.0F, 0.0F, 10.0F, 0.0F}, 0.25F).has_value(),
           "controller advances");
    const camera::FreeCameraPose committed = controller.pose();
    expect(committed != pose, "the accepted step moved the controller");
    const camera::FreeCameraIntent hostile[] = {
        {0.0F, 1.0F, 0.0F, 10.0F, nan},
        {0.0F, 1.0F, 1.01F, 10.0F, 0.0F},
        {0.0F, 1.0F, 0.0F, infinity, 0.0F},
    };
    for (const camera::FreeCameraIntent& intent : hostile) {
        expect(!controller.advance(intent, 0.25F).has_value(), "hostile step is rejected");
        expect(controller.pose() == committed, "a rejected step leaves no partial mutation");
    }
    expect(!controller.advance({0.0F, 1.0F, 0.0F, 10.0F, 0.0F}, -1.0F).has_value(),
           "negative duration rejected by the controller");
    expect(controller.pose() == committed, "a rejected duration leaves no partial mutation");
    expect(!camera::FreeCameraController::create(
                bad_settings, std::span<const float, 3>{eye}, 0.0F, 0.0F)
                .has_value(),
           "controller refuses invalid settings");
}

void test_determinism() {
    const auto replay = []() {
        camera::FreeCameraPose pose = level_pose();
        const camera::FreeCameraIntent script[] = {
            {0.0F, 1.0F, 0.0F, 3.0F, 1.0F},
            {1.0F, 1.0F, 0.0F, -2.0F, 0.5F},
            {0.0F, 0.0F, 1.0F, 0.0F, -4.0F},
            {-1.0F, 0.0F, -1.0F, 11.0F, 0.0F},
        };
        for (int frame = 0; frame < 32; ++frame) {
            pose = camera::advance_free_camera(settings(), pose, script[frame % 4], 1.0F / 60.0F)
                       .value();
        }
        return pose;
    };
    expect(replay() == replay(), "one scripted sequence reproduces the same pose");
}

} // namespace

int main() {
    test_settings_validation();
    test_entry_matches_the_tactical_view();
    test_translation_axes();
    test_diagonal_normalisation();
    test_pitched_vertical_cap();
    test_look_yaw_and_pitch();
    test_zero_duration();
    test_malformed_and_overflow_inputs_are_atomic();
    test_determinism();
    if (failures != 0) {
        std::cerr << failures << " free camera contract failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "free camera contracts passed\n";
    return EXIT_SUCCESS;
}
