// Retreat cinematic camera contracts (FS-R07..FS-R09, FSC-R07).
//
// Exercises include/eawr/presentation/camera/retreat_camera.hpp through its public API. Geometry values
// are test data; the frame numbers and the station offset are the debug-build rules the header cites.
// The Unlocked clip and fade values come from the shipped tactical camera XML through load_constants.

#include "../../support/check.hpp"
#include "eawr/presentation/camera/camera.hpp"
#include "eawr/presentation/camera/constants_source.hpp"
#include "eawr/presentation/camera/retreat_camera.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace {
namespace camera = eawr::presentation::camera;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAILED: ");
}

void expect_close(const float actual, const float expected, const std::string_view message, const float tolerance = 1e-3F) {
    if (!(std::abs(actual - expected) <= tolerance)) {
        std::cerr << "FAILED: " << message << " (expected " << expected << ", got " << actual << ")\n";
        ++failures;
    }
}

void expect_point(const std::array<float, 3>& actual, const std::array<float, 3>& expected, const std::string_view message) {
    for (std::size_t axis = 0; axis < 3; ++axis) expect_close(actual[axis], expected[axis], message);
}

std::span<const std::byte> bytes(const std::string& text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// A minimal data pair carrying the Unlocked definition the way the shipped data authors it, including the 'f' suffix.
camera::Constants unlocked_constants() {
    const std::string cameras =
        "<TacticalCameras><TacticalCamera Name=\"Unlocked\">"
        "<Pitch_Default>34.0</Pitch_Default><Pitch_Min>0.0</Pitch_Min><Pitch_Max>80.0</Pitch_Max>"
        "<Distance_Default>900.0</Distance_Default><Distance_Max>60000.0</Distance_Max><Distance_Min>0.0</Distance_Min>"
        "<Distance_Per_Mouse_Unit>50.0</Distance_Per_Mouse_Unit>"
        "<Near_Clip>1.0</Near_Clip><Far_Clip>60000.0</Far_Clip>"
        "<Object_Fade_Begin>10000.0f</Object_Fade_Begin><Object_Fade_End>11000.0f</Object_Fade_End>"
        "</TacticalCamera></TacticalCameras>";
    const std::string global =
        "<GameConstants><Tactical_Min_Scroll_Speed>1</Tactical_Min_Scroll_Speed>"
        "<Tactical_Max_Scroll_Speed>10</Tactical_Max_Scroll_Speed>"
        "<Tactical_Edge_Scroll_Region>2</Tactical_Edge_Scroll_Region>"
        "<Tactical_Offscreen_Scroll_Region>3</Tactical_Offscreen_Scroll_Region>"
        "<Push_Scroll_Speed_Modifier>1.5</Push_Scroll_Speed_Modifier>"
        "<Scroll_Acceleration_Factor>4</Scroll_Acceleration_Factor>"
        "<Scroll_Deceleration_Factor>5</Scroll_Deceleration_Factor></GameConstants>";
    auto loaded = camera::load_constants({bytes(cameras), "data/xml/tacticalcameras.xml", "camera-sha"},
        {bytes(global), "data/xml/gameconstants.xml", "global-sha"}, camera::Mode::unlocked);
    if (!loaded) {
        std::cerr << "FAILED: the Unlocked definition did not load\n";
        std::exit(EXIT_FAILURE);
    }
    return loaded.value().constants;
}

// Entry marker at the origin, ships fly 4000 units along +X.
camera::RetreatCameraInput ship_input() {
    camera::RetreatCameraInput input;
    input.entry = {0.0F, 0.0F, 0.0F};
    input.destination = {4000.0F, 0.0F, 0.0F};
    input.departure_direction = {1.0F, 0.0F, 0.0F};
    return input;
}

camera::RetreatCameraInput station_input() {
    auto input = ship_input();
    input.station = std::array<float, 3>{1000.0F, 2000.0F, 50.0F};
    return input;
}

camera::RetreatCameraPose pose_at(const camera::RetreatCameraInput& input, const std::uint32_t k, const camera::Constants& unlocked) {
    auto pose = camera::retreat_camera(input, k, unlocked);
    if (!pose) {
        std::cerr << "FAILED: retreat_camera(k=" << k << ") was rejected\n";
        std::exit(EXIT_FAILURE);
    }
    return pose.value();
}

void test_unlocked_clip_and_fade_come_from_data() {
    const auto unlocked = unlocked_constants();
    expect_close(unlocked.object_fade_begin, 10000.0F, "Object_Fade_Begin is read with its 'f' suffix");
    expect_close(unlocked.object_fade_end, 11000.0F, "Object_Fade_End is read with its 'f' suffix");
    const auto pose = pose_at(ship_input(), 0, unlocked);
    expect_close(pose.near_clip, 1.0F, "near clip from data");
    expect_close(pose.far_clip, 60000.0F, "far clip from data");
    expect_close(pose.object_fade_begin, 10000.0F, "fade begin from data");
    expect_close(pose.object_fade_end, 11000.0F, "fade end from data");
    auto modded = unlocked;
    modded.far_clip = 90000.0F;
    modded.object_fade_end = 12345.0F;
    const auto changed = pose_at(ship_input(), 0, modded);
    expect_close(changed.far_clip, 90000.0F, "a modded far clip reaches the pose");
    expect_close(changed.object_fade_end, 12345.0F, "a modded fade end reaches the pose");
}

void test_ship_shot_geometry() {
    const auto unlocked = unlocked_constants();
    const auto pose = pose_at(ship_input(), 0, unlocked);
    expect(pose.shot == camera::RetreatShot::ship, "k=0 plays the ship shot");
    // Span 4000, quarter 1000: camera beyond the destination (+1000 along travel), 150 to the left, 70 up; target the
    // same quarter span before the destination.
    expect_point(pose.eye, {5000.0F, 150.0F, 70.0F}, "ship shot camera");
    expect_point(pose.target, {3000.0F, 0.0F, 0.0F}, "ship shot target");
}

void test_ship_shot_is_fixed_and_follows_the_marker() {
    const auto unlocked = unlocked_constants();
    const auto first = pose_at(ship_input(), 0, unlocked);
    const auto later = pose_at(ship_input(), 149, unlocked);
    expect(first == later, "the ship shot holds its fixed camera and target through k=149");
    auto rotated = ship_input();
    rotated.destination = {0.0F, 4000.0F, 0.0F};
    rotated.departure_direction = {0.0F, 1.0F, 0.0F};
    const auto turned = pose_at(rotated, 0, unlocked);
    expect_point(turned.eye, {-150.0F, 5000.0F, 70.0F}, "a +Y departure turns the sideways offset with it");
    expect_point(turned.target, {0.0F, 3000.0F, 0.0F}, "a +Y departure moves the target with it");
    auto zero_span = ship_input();
    zero_span.destination = zero_span.entry;
    const auto flat = pose_at(zero_span, 0, unlocked);
    expect_point(flat.eye, {0.0F, 150.0F, 70.0F}, "zero retained span: camera at the marker plus the offsets");
    expect_point(flat.target, {0.0F, 0.0F, 0.0F}, "zero retained span: target at the marker");
}

void test_ship_only_never_cuts() {
    const auto unlocked = unlocked_constants();
    for (const std::uint32_t k : {0U, 149U, 150U, 210U, 211U, 449U})
        expect(pose_at(ship_input(), k, unlocked).shot == camera::RetreatShot::ship, "no station: the ship shot at every frame");
}

void test_station_cut_hold_and_orbit() {
    const auto unlocked = unlocked_constants();
    const auto input = station_input();
    const auto at_149 = pose_at(input, 149, unlocked);
    expect(at_149.shot == camera::RetreatShot::ship, "k=149 still the ship shot");
    expect_point(at_149.eye, pose_at(input, 0, unlocked).eye, "k=149 equals the k=0 ship camera");
    for (const std::uint32_t k : {150U, 209U, 210U}) {
        const auto held = pose_at(input, k, unlocked);
        expect(held.shot == camera::RetreatShot::station_hold, "k=150..210 holds the station shot");
        expect_point(held.target, {1000.0F, 2000.0F, 50.0F}, "the station shot targets the station");
        expect_point(held.eye, {1000.0F, 0.0F, 2050.0F}, "the station camera offset is (0,-2000,2000) from the station");
    }
    const auto first_orbit = pose_at(input, 211, unlocked);
    expect(first_orbit.shot == camera::RetreatShot::station_orbit, "k=211 begins the orbit");
    expect_point(first_orbit.target, {1000.0F, 2000.0F, 50.0F}, "the orbit keeps the station target");
    // One frame into an eased orbit has barely turned: still within a hair of the held camera.
    expect_close(first_orbit.eye[0], 1000.0F, "k=211 eye is nearly the held eye", 0.05F);
    expect_close(first_orbit.eye[2], 2050.0F, "the orbit keeps the camera height");
    const auto later = pose_at(input, 450, unlocked);
    const float radius = std::hypot(later.eye[0] - later.target[0], later.eye[1] - later.target[1]);
    expect_close(radius, 2000.0F, "the orbit keeps its radius about the target", 0.05F);
    expect(!(later.eye[0] == first_orbit.eye[0] && later.eye[1] == first_orbit.eye[1]), "the orbit moves the camera");
}

void test_orbit_fraction() {
    const auto unlocked = unlocked_constants();
    const auto input = station_input();
    const auto at_300 = pose_at(input, 300, unlocked);
    auto half = camera::retreat_camera(input, 300, unlocked, 0.5F);
    auto next = camera::retreat_camera(input, 301, unlocked);
    expect(half && next, "the fraction is accepted");
    if (!half || !next) return;
    const float whole = std::atan2(next.value().eye[1] - 2000.0F, next.value().eye[0] - 1000.0F)
        - std::atan2(at_300.eye[1] - 2000.0F, at_300.eye[0] - 1000.0F);
    const float part = std::atan2(half.value().eye[1] - 2000.0F, half.value().eye[0] - 1000.0F)
        - std::atan2(at_300.eye[1] - 2000.0F, at_300.eye[0] - 1000.0F);
    expect(whole > 0.0F && part > 0.0F && part < whole, "a half frame turns less than a whole frame");
    expect_close(part / whole, 0.5F, "a half frame turns about half a frame", 0.02F);
    auto ship = camera::retreat_camera(input, 100, unlocked, 0.9F);
    expect(ship && ship.value() == pose_at(input, 100, unlocked), "the fraction never moves a held ship shot");
}

void test_station_only_keys() {
    const auto unlocked = unlocked_constants();
    auto input = station_input();
    input.ship_shot = false;
    const auto at_0 = pose_at(input, 0, unlocked);
    expect(at_0.shot == camera::RetreatShot::station_hold, "station-only starts on the attached shot at k=0");
    for (const std::uint32_t k : {59U, 60U})
        expect(pose_at(input, k, unlocked).shot == camera::RetreatShot::station_hold, "station-only holds through k=60");
    expect_point(pose_at(input, 59, unlocked).eye, {1000.0F, 0.0F, 2050.0F}, "station-only hold offset at k=59");
    const auto orbit = pose_at(input, 61, unlocked);
    expect(orbit.shot == camera::RetreatShot::station_orbit, "station-only begins the orbit at k=61");
    expect_close(orbit.eye[0], 1000.0F, "k=61 eye is nearly the held eye", 0.05F);
    // The orbit clock restarts from its own last key: frame 211 of a ship+station cinematic and frame 61 of a
    // station-only one are both one frame into the orbit.
    const auto reference = pose_at(station_input(), 211, unlocked);
    expect_point(orbit.eye, reference.eye, "one frame into the orbit is the same camera in both cinematics");
}

void test_orbit_curve() {
    expect_close(camera::retreat_orbit_degrees(0.0F), 0.0F, "no turn at the last key");
    expect_close(camera::retreat_orbit_degrees(-5.0F), 0.0F, "no turn before the last key");
    expect_close(camera::retreat_orbit_degrees(500.0F), 180.0F, "half a turn at half the period", 0.01F);
    expect_close(camera::retreat_orbit_degrees(1000.0F), 360.0F, "one turn per period", 0.01F);
    expect_close(camera::retreat_orbit_degrees(5000.0F), 360.0F, "the curve stops at one turn", 0.01F);
    const float early = camera::retreat_orbit_degrees(10.0F);
    const float middle = camera::retreat_orbit_degrees(510.0F) - camera::retreat_orbit_degrees(500.0F);
    expect(early < 0.01F * 360.0F && middle > early * 3.0F, "eased in: it starts slowly and is fastest mid-period");
    float previous = 0.0F;
    bool monotone = true;
    for (std::uint32_t frame = 1; frame <= 1000; ++frame) {
        const float now = camera::retreat_orbit_degrees(static_cast<float>(frame));
        monotone = monotone && now >= previous;
        previous = now;
    }
    expect(monotone, "the orbit never turns back");
}

void test_departure_direction() {
    expect_point(camera::retreat_departure_direction(0.0F, 0.0F), {-1.0F, 0.0F, 0.0F}, "yaw 0: local -X is world -X");
    expect_point(camera::retreat_departure_direction(0.0F, 90.0F), {0.0F, -1.0F, 0.0F}, "yaw 90 turns -X to -Y");
    expect_point(camera::retreat_departure_direction(0.0F, 180.0F), {1.0F, 0.0F, 0.0F}, "yaw 180 turns -X to +X");
    const auto tilted = camera::retreat_departure_direction(30.0F, 45.0F);
    expect_close(std::sqrt(tilted[0] * tilted[0] + tilted[1] * tilted[1] + tilted[2] * tilted[2]), 1.0F, "unit length");
}

void test_rejections() {
    const auto unlocked = unlocked_constants();
    auto bad = ship_input();
    bad.entry[1] = std::numeric_limits<float>::quiet_NaN();
    expect(!camera::retreat_camera(bad, 0, unlocked), "a non-finite entry is rejected");
    auto no_roster = ship_input();
    no_roster.ship_shot = false;
    expect(!camera::retreat_camera(no_roster, 0, unlocked), "no ship shot and no station is rejected");
    auto clipped = unlocked;
    clipped.far_clip = clipped.near_clip;
    expect(!camera::retreat_camera(ship_input(), 0, clipped), "an Unlocked camera with no clip range is rejected");
    auto fade = unlocked;
    fade.object_fade_end = fade.object_fade_begin - 1.0F;
    expect(!camera::retreat_camera(ship_input(), 0, fade), "an inverted fade range is rejected");
}

void test_determinism() {
    const auto unlocked = unlocked_constants();
    const auto input = station_input();
    for (std::uint32_t k = 0; k < 450; k += 7)
        expect(pose_at(input, k, unlocked) == pose_at(input, k, unlocked), "same inputs, same pose");
}

} // namespace

int main() {
    test_unlocked_clip_and_fade_come_from_data();
    test_ship_shot_geometry();
    test_ship_shot_is_fixed_and_follows_the_marker();
    test_ship_only_never_cuts();
    test_station_cut_hold_and_orbit();
    test_station_only_keys();
    test_orbit_fraction();
    test_orbit_curve();
    test_departure_direction();
    test_rejections();
    test_determinism();
    if (failures != 0) {
        std::cerr << failures << " retreat camera contract failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "retreat camera contracts passed\n";
    return EXIT_SUCCESS;
}
