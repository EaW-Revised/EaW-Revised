// Arrival cinematic camera contracts (docs/behaviour/gc-arrival-camera.md, SK-ARR-01..SK-ARR-14).
//
// Exercises include/eawr/presentation/camera/arrival_camera.hpp through its public API. Geometry values are test
// data; the key frames, offsets and ease values are the debug-build rules the header cites, checked here against
// independent arithmetic (the closed-form distance table of WR-38 and hand-worked key positions).

#include "../../support/check.hpp"
#include "eawr/presentation/camera/arrival_camera.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <set>
#include <string_view>

namespace {
namespace camera = eawr::presentation::camera;
using Vec3 = std::array<float, 3>;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAILED: ");
}

void expect_close(const float actual, const float expected, const std::string_view message, const float tolerance = 1e-2F) {
    if (!(std::abs(actual - expected) <= tolerance)) {
        std::cerr << "FAILED: " << message << " (expected " << expected << ", got " << actual << ")\n";
        ++failures;
    }
}

void expect_point(const Vec3& actual, const Vec3& expected, const std::string_view message, const float tolerance = 1e-2F) {
    for (std::size_t axis = 0; axis < 3; ++axis) expect_close(actual[axis], expected[axis], message, tolerance);
}

// The destination is the origin and the fleet departs along +X, so the entry point is (D, 0, 0).
camera::ArrivalCameraInput attacker_input(const std::uint8_t shot) {
    camera::ArrivalCameraInput input;
    input.role = camera::ArrivalRole::attacker;
    input.shot_choice = shot;
    input.destination = {0.0F, 0.0F, 0.0F};
    input.departure_direction = {1.0F, 0.0F, 0.0F};
    input.rest_eye = {-1000.0F, 500.0F, 800.0F};
    input.look_at = {0.0F, 0.0F, 0.0F};
    return input;
}

camera::ArrivalCameraInput defender_input() {
    auto input = attacker_input(0);
    input.role = camera::ArrivalRole::defender;
    return input;
}

camera::ArrivalCameraPose pose_at(const camera::ArrivalCameraInput& input, const std::uint32_t k, const float fraction = 0.0F) {
    auto pose = camera::arrival_camera(input, k, fraction);
    if (!pose) {
        std::cerr << "FAILED: arrival_camera(k=" << k << ") was rejected\n";
        std::exit(EXIT_FAILURE);
    }
    return pose.value();
}

// WR-38, closed form: the retail ease with no ease-in and an ease-out of one half.
double reference_ease(const double t) { return t <= 0.5 ? 4.0 * t / 3.0 : 2.0 / 3.0 + (8.0 / 3.0) * (t - t * t / 2.0 - 3.0 / 8.0); }

double reference_entry_distance() {
    double total = 75 * 8.125; // frames 45..119
    for (int k = 120; k <= 149; ++k) total += 8.125 - 8.125 * reference_ease((k - 119) / 30.0);
    return total;
}

void test_entry_distance_matches_the_table() {
    expect_close(camera::arrival_entry_distance(), static_cast<float>(reference_entry_distance()), "entry distance = full-speed span + slow-to-target span", 0.05F);
    expect(camera::arrival_entry_distance() > 609.375F + 60.0F && camera::arrival_entry_distance() < 609.375F + 130.0F,
        "entry distance is the full-speed span plus a slow-to-target span of under 130");
    auto input = attacker_input(0);
    expect_point(camera::arrival_entry_point(input), {camera::arrival_entry_distance(), 0.0F, 0.0F}, "entry point lies behind the destination along the direction");
}

void test_ease() {
    expect_close(camera::arrival_ease(0.0F, 0.5F, 0.5F), 0.0F, "ease starts at 0");
    expect_close(camera::arrival_ease(1.0F, 0.5F, 0.5F), 1.0F, "ease ends at 1");
    expect_close(camera::arrival_ease(0.5F, 0.5F, 0.5F), 0.5F, "symmetric ease passes half at half");
    expect_close(camera::arrival_ease(0.25F, 0.5F, 0.5F), 0.125F, "symmetric ease: quarter point");
    expect_close(camera::arrival_ease(1.0F, 0.0F, 0.0F), 1.0F, "no ease is linear (end)");
    expect_close(camera::arrival_ease(0.3F, 0.0F, 0.0F), 0.3F, "no ease is linear");
    for (const float t : {0.1F, 0.4F, 0.5F, 0.9F}) expect_close(camera::arrival_ease(t, 0.0F, 0.5F), static_cast<float>(reference_ease(t)), "ease(0, 1/2) matches the WR-38 curve");
    float previous = -1.0F;
    for (int i = 0; i <= 100; ++i) {
        const float value = camera::arrival_ease(static_cast<float>(i) / 100.0F, 0.5F, 0.5F);
        expect(value >= previous, "ease never goes backwards");
        previous = value;
    }
}

void test_attacker_shot_zero_keys() {
    const auto input = attacker_input(0);
    const float distance = camera::arrival_entry_distance();
    const Vec3 entry{distance, 0.0F, 0.0F};
    // Camera key 0: a twentieth of the span from the destination, 300 to the side (span along +X: toward -Y), 70 up.
    const Vec3 start{0.05F * distance, -300.0F, 70.0F};
    const auto first = pose_at(input, 0);
    expect(first.shot == camera::ArrivalShot::attacker_near_destination, "choice 0 plays the near-destination shot");
    expect_point(first.eye, start, "shot 0 camera at k=0");
    expect_point(first.target, entry, "shot 0 target at the entry point at k=0");
    expect_point(pose_at(input, 34).eye, start, "shot 0 camera holds through k=34");
    expect_point(pose_at(input, 35).eye, start, "shot 0 camera is still at the start pose at k=35");
    expect(pose_at(input, 36).eye != start, "shot 0 camera moves after k=35");
    expect_point(pose_at(input, 22).target, entry, "the target holds on the entry point to k=22");
    expect_point(pose_at(input, 23).target, entry, "the target leaves the entry point at k=23");
    expect(pose_at(input, 24).target != entry, "the target has moved by k=24");
    // Segment 35..120 eases 1/2 in and out: the half-way presented time is the half-way point.
    const auto half = pose_at(input, 77, 0.5F);
    expect_point(half.eye, {(start[0] + input.rest_eye[0]) / 2.0F, (start[1] + input.rest_eye[1]) / 2.0F, (start[2] + input.rest_eye[2]) / 2.0F}, "shot 0 camera is linear between the keys (half way at 77.5)");
    expect_point(pose_at(input, 120).eye, input.rest_eye, "shot 0 ends on the rest pose at k=120");
    expect_point(pose_at(input, 120).target, input.destination, "shot 0 target ends on the destination at k=120");
    expect_point(pose_at(input, 149).eye, input.rest_eye, "shot 0 holds the rest pose to the end of the timer");
    expect_point(pose_at(input, 119).eye, input.rest_eye, "shot 0 is within a hair of the rest pose at k=119", 5.0F);
}

void test_attacker_shot_one_keys() {
    const auto input = attacker_input(1);
    const float distance = camera::arrival_entry_distance();
    const Vec3 entry{distance, 0.0F, 0.0F};
    // Beyond the entry point by the span (so 2D), 70 below the entry's height.
    const Vec3 start{2.0F * distance, 0.0F, -70.0F};
    const auto first = pose_at(input, 0);
    expect(first.shot == camera::ArrivalShot::attacker_behind_entry, "choice 1 plays the behind-entry shot");
    expect_point(first.eye, start, "shot 1 camera at k=0");
    expect_point(first.target, entry, "shot 1 target at the entry point at k=0");
    expect_point(pose_at(input, 35).eye, start, "shot 1 camera still at the start pose at k=35");
    expect(pose_at(input, 36).eye != start, "shot 1 camera moves after k=35");
    // The spline has equal end tangents, so the symmetric ease passes the straight midpoint at 77.5.
    expect_point(pose_at(input, 77, 0.5F).eye, {(start[0] + input.rest_eye[0]) / 2.0F, (start[1] + input.rest_eye[1]) / 2.0F, (start[2] + input.rest_eye[2]) / 2.0F}, "shot 1 spline passes the midpoint at 77.5");
    expect_point(pose_at(input, 120).eye, input.rest_eye, "shot 1 ends on the rest pose at k=120");
    // The target eases out 0 / in 1/2 here: 0.6667 of the way at the half of 23..120.
    const float e = camera::arrival_ease(0.5F, 0.0F, 0.5F);
    expect_close(pose_at(input, 71, 0.5F).target[0], entry[0] + e * (input.destination[0] - entry[0]), "shot 1 target eases with (0, 1/2)");
    expect_point(pose_at(input, 120).target, input.destination, "shot 1 target ends on the destination");
}

void test_defender_sweep() {
    const auto input = defender_input();
    // look-at at the origin, rest eye at (-1000, 500, 800): the horizontal offset (1000, -500) swaps to (-500, 1000).
    const auto first = pose_at(input, 0);
    expect(first.shot == camera::ArrivalShot::defender_base_sweep, "the defender plays the base sweep");
    expect_point(first.eye, {-500.0F, 1000.0F, 20.0F}, "defender camera at k=0: swapped offset, 20 above the look-at");
    expect_point(first.target, input.look_at, "the defender target is the look-at point");
    expect_point(pose_at(input, 120).eye, input.rest_eye, "the defender sweep ends on the rest pose at k=120");
    expect_point(pose_at(input, 149).eye, input.rest_eye, "and holds it to the end of the timer");
    // Reverse rotation (the long way, clockwise): half way the horizontal offset has turned by 360 - 323.13/2.
    const auto halfway = pose_at(input, 60, 0.0F); // u = 0.5 of 0..120, ease(0.5, 0.5, 0.5) = 0.5
    const float radius = std::hypot(500.0F, 1000.0F);
    expect_close(std::hypot(halfway.eye[0], halfway.eye[1]), radius, "equal radii stay equal through the sweep", 0.5F);
    expect_point(halfway.eye, {radius * std::cos(315.0F * 0.017453292F), radius * std::sin(315.0F * 0.017453292F), 410.0F}, "defender sweep at k=60: angle 198.43 from the start, height half way", 1.0F);
    for (const std::uint32_t k : {1U, 30U, 60U, 90U, 119U}) expect_point(pose_at(input, k).target, input.look_at, "the defender target never moves");
}

void test_rejections() {
    auto input = attacker_input(0);
    input.destination[1] = std::numeric_limits<float>::quiet_NaN();
    expect(!camera::arrival_camera(input, 0), "a non-finite destination is refused");
    input = attacker_input(0);
    input.rest_eye[2] = std::numeric_limits<float>::infinity();
    expect(!camera::arrival_camera(input, 0), "a non-finite rest eye is refused");
    input = attacker_input(2);
    expect(!camera::arrival_camera(input, 0), "a shot choice above 1 is refused");
    input = attacker_input(0);
    input.departure_direction = {0.0F, 0.0F, 0.0F};
    expect(!camera::arrival_camera(input, 0), "a zero departure direction is refused for the attacker");
    auto defender = defender_input();
    defender.departure_direction = {0.0F, 0.0F, 0.0F};
    expect(static_cast<bool>(camera::arrival_camera(defender, 0)), "the defender shot does not use the departure direction");
}

void test_clip_planes() {
    for (const auto& input : {attacker_input(0), attacker_input(1), defender_input()}) {
        const auto pose = pose_at(input, 10);
        expect_close(pose.near_clip, 1.0F, "near clip 1 while the cinematic plays");
        expect_close(pose.far_clip, 10000.0F, "far clip 10000 while the cinematic plays");
    }
}

void test_fresh_battle_gate() {
    // SK-ARR-01: only a battle ready at tick 0 plays the cinematic; a battle restored mid-way plays none.
    expect(camera::arrival_admits_battle_ready_at(0), "a fresh battle (ready at tick 0) is admitted");
    expect(!camera::arrival_admits_battle_ready_at(1), "a battle restored at tick 1 is refused");
    expect(!camera::arrival_admits_battle_ready_at(1234), "a battle restored mid-way is refused");
}

void test_timer_and_choice() {
    expect(camera::arrival_cinematic_running(0), "the cinematic runs at frame 0");
    expect(camera::arrival_cinematic_running(149), "the cinematic runs at frame 149");
    expect(!camera::arrival_cinematic_running(150), "the 5 s timer has ended at frame 150");
    std::set<std::uint8_t> seen;
    for (std::uint64_t seed = 0; seed < 64; ++seed) {
        const auto choice = camera::arrival_shot_from_seed(seed);
        expect(choice <= 1, "the shot choice is 0 or 1");
        expect(choice == camera::arrival_shot_from_seed(seed), "the shot choice is a pure function of the seed");
        seen.insert(choice);
    }
    expect(seen.size() == 2, "both shots occur across seeds");
}

void test_fraction_is_continuous() {
    const auto input = attacker_input(0);
    const auto a = pose_at(input, 60, 0.0F);
    const auto b = pose_at(input, 60, 0.999F);
    const auto c = pose_at(input, 61, 0.0F);
    expect(std::abs(b.eye[0] - c.eye[0]) < 3.0F, "the frame fraction runs into the next frame without a jump");
    expect(a.eye != b.eye, "the camera moves within a frame");
}

} // namespace

int main() {
    test_entry_distance_matches_the_table();
    test_ease();
    test_attacker_shot_zero_keys();
    test_attacker_shot_one_keys();
    test_defender_sweep();
    test_rejections();
    test_clip_planes();
    test_fresh_battle_gate();
    test_timer_and_choice();
    test_fraction_is_continuous();
    if (failures != 0) {
        std::cerr << failures << " arrival camera contract failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "arrival camera contracts passed\n";
    return EXIT_SUCCESS;
}
