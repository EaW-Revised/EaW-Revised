// Story cinematic camera contracts (docs/behaviour/tactical-story-cinematic.md, TSC-01..TSC-10).
//
// Exercises include/eawr/presentation/camera/story_cinematic.hpp through its public API with independent arithmetic: the
// offsets a key call gives, the ease of a transition, the attached-object cases and the return to the player's camera.

#include "../../support/check.hpp"
#include "eawr/presentation/camera/story_cinematic.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>

namespace {
namespace camera = eawr::presentation::camera;
using Vec = std::array<double, 3>;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAILED: ");
}

bool near(const double a, const double b, const double tolerance = 1e-3) { return std::abs(a - b) <= tolerance; }

bool near(const std::array<float, 3>& a, const Vec& b, const double tolerance = 1e-2) {
    return near(a[0], b[0], tolerance) && near(a[1], b[1], tolerance) && near(a[2], b[2], tolerance);
}

const camera::StoryAttachmentReader no_objects{}; // an empty reference finds no object

camera::StoryKeyInput key(const Vec& position, const double distance, const double pitch, const double yaw, const bool euler) {
    camera::StoryKeyInput input;
    input.position = position;
    input.distance = distance;
    input.pitch = pitch;
    input.yaw = yaw;
    input.euler = euler;
    return input;
}

void test_the_ease() {
    using camera::StoryCinematic;
    for (const double t : {0.0, 0.1, 0.5, 0.9, 1.0}) expect(near(StoryCinematic::ease(t, 0.0, 0.0), t), "no ease is the straight line");
    expect(near(StoryCinematic::ease(0.0, 0.5, 0.5), 0.0), "the ease starts at 0");
    expect(near(StoryCinematic::ease(1.0, 0.5, 0.5), 1.0), "the ease ends at 1");
    expect(near(StoryCinematic::ease(0.5, 0.5, 0.5), 0.5), "an ease symmetric about the middle gives half at the middle");
    // Speed 2 over a quarter of the segment's first half ramp: 2 * t^2 / (2 * 0.5) = 0.125 at t = 0.25.
    expect(near(StoryCinematic::ease(0.25, 0.5, 0.5), 0.125), "the ramp is a parabola");
    double previous = -1.0;
    bool monotone = true;
    for (int i = 0; i <= 100; ++i) {
        const double value = StoryCinematic::ease(i / 100.0, 0.25, 0.25);
        monotone = monotone && value >= previous;
        previous = value;
    }
    expect(monotone, "the 0.25 ease never runs backwards");
    expect(near(StoryCinematic::ease(1.0, 0.25, 0.25), 1.0), "the 0.25 ease ends at 1");
}

void test_an_idle_cinematic() {
    camera::StoryCinematic cinematic;
    expect(!cinematic.active(), "a cinematic is idle until started");
    cinematic.set_camera_key(0.0, key({1, 2, 3}, 0, 0, 0, false));
    cinematic.transition_to_tactical(0.0, 1.0, {0, 0, 0}, {1, 1, 1});
    expect(!cinematic.pose(1.0, no_objects), "an idle cinematic has no pose");
    cinematic.start(10.0, {0, 0, 100}, {0, 50, 0});
    expect(cinematic.active(), "start runs it");
    const auto held = cinematic.pose(12.0, no_objects);
    expect(held && near(held->eye, {0, 0, 100}) && near(held->target, {0, 50, 0}), "a started cinematic holds the player's pose");
    cinematic.end();
    expect(!cinematic.active() && !cinematic.pose(13.0, no_objects), "end drops the cinematic");
}

void test_the_offset_of_a_key() {
    camera::StoryCinematic cinematic;
    cinematic.start(0.0, {0, 0, 0}, {0, 0, 0});
    // Without the euler flag the three numbers are the offset.
    cinematic.set_camera_key(1.0, key({100, 200, 300}, 5, 6, 7, false));
    auto pose = cinematic.pose(2.0, no_objects);
    expect(pose && near(pose->eye, {105, 206, 307}), "a plain offset adds to the position");
    // Yaw 90 turns the -Y backward offset to +X; pitch 0 keeps it level.
    cinematic.set_camera_key(3.0, key({0, 0, 0}, 1940, 0, 90, true));
    pose = cinematic.pose(4.0, no_objects);
    expect(pose && near(pose->eye, {1940, 0, 0}, 0.1), "yaw 90 places the camera on +X");
    // Pitch 30 lifts the camera: half the distance up, cos 30 of it out.
    cinematic.set_camera_key(5.0, key({0, 0, 0}, 100, 30, 0, true));
    pose = cinematic.pose(6.0, no_objects);
    expect(pose && near(pose->eye, {0, -100 * std::cos(30.0 * 3.14159265358979 / 180.0), 50.0}, 0.1), "pitch 30 lifts the camera");
    // Yaw 0 puts it behind the position along -Y.
    cinematic.set_camera_key(7.0, key({10, 20, 30}, 100, 0, 0, true));
    pose = cinematic.pose(8.0, no_objects);
    expect(pose && near(pose->eye, {10, -80, 30}, 0.1), "yaw 0 puts the camera behind along -Y");
}

void test_a_timed_transition() {
    camera::StoryCinematic cinematic;
    cinematic.start(100.0, {0, 0, 0}, {0, 0, 0});
    cinematic.transition_camera_key(100.0, key({300, 0, 0}, 0, 0, 0, false), 4.0); // 120 ticks
    expect(near(cinematic.pose(100.0, no_objects)->eye, {0, 0, 0}), "the transition starts where the camera was");
    const auto quarter = cinematic.pose(130.0, no_objects); // the clock never runs backwards: read the earlier time first
    expect(quarter && near(quarter->eye, {300 * 0.125, 0, 0}, 0.5), "a quarter of the way in time is an eighth in space");
    expect(near(cinematic.pose(160.0, no_objects)->eye, {150, 0, 0}), "halfway in time is halfway in space (symmetric ease)");
    expect(near(cinematic.pose(220.0, no_objects)->eye, {300, 0, 0}), "the transition ends on the key");
    expect(near(cinematic.pose(900.0, no_objects)->eye, {300, 0, 0}), "the camera holds the last key");
    // A one-tick time is the Set_ call.
    cinematic.transition_camera_key(950.0, key({7, 8, 9}, 0, 0, 0, false), 1.0 / 30.0);
    expect(near(cinematic.pose(951.0, no_objects)->eye, {7, 8, 9}), "a time of one tick or less cuts");
    // A transition begun mid-way starts from the pose shown.
    cinematic.start(0.0, {0, 0, 0}, {0, 0, 0});
    cinematic.transition_camera_key(0.0, key({100, 0, 0}, 0, 0, 0, false), 2.0);
    const auto mid = cinematic.pose(30.0, no_objects);
    cinematic.transition_camera_key(30.0, key({0, 100, 0}, 0, 0, 0, false), 2.0);
    expect(mid && near(cinematic.pose(30.0, no_objects)->eye, {mid->eye[0], mid->eye[1], mid->eye[2]}), "a new transition starts at the shown pose");
    expect(near(cinematic.pose(90.0, no_objects)->eye, {0, 100, 0}), "and ends on its own key");
}

void test_the_target_track_is_separate() {
    camera::StoryCinematic cinematic;
    cinematic.start(0.0, {0, 0, 500}, {0, 0, 0});
    cinematic.set_target_key(0.0, key({10, 20, 30}, 0, 0, 0, false));
    auto pose = cinematic.pose(1.0, no_objects);
    expect(pose && near(pose->eye, {0, 0, 500}) && near(pose->target, {10, 20, 30}), "a target key moves only the target");
    cinematic.transition_target_key(1.0, key({70, 20, 30}, 0, 0, 0, false), 2.0);
    pose = cinematic.pose(31.0, no_objects);
    expect(pose && near(pose->target, {40, 20, 30}) && near(pose->eye, {0, 0, 500}), "a target transition moves the target alone");
}

void test_an_attached_key() {
    camera::StoryCinematic cinematic;
    cinematic.start(0.0, {0, 0, 0}, {0, 0, 0});
    Vec where{100, 100, 0};
    const auto read_where = [&where](const std::uint64_t id) -> std::optional<camera::StoryAttachment> {
        if (id != 7) return std::nullopt;
        camera::StoryAttachment attached;
        attached.position = where;
        return attached;
    };
    const camera::StoryAttachmentReader reader(read_where);
    auto input = key({0, 0, 0}, 5, 0, 0, false);
    input.object = 7;
    cinematic.set_target_key(0.0, input);
    expect(near(cinematic.pose(1.0, reader)->target, {105, 100, 0}), "an attached key follows the object, plus its offset");
    where = {200, 150, 10};
    expect(near(cinematic.pose(2.0, reader)->target, {205, 150, 10}), "and keeps following it");
    // The object is gone: with no model matrix cached the track holds where it last was.
    expect(near(cinematic.pose(3.0, no_objects)->target, {205, 150, 10}), "a vanished object leaves the target where it was");
    // A rotated frame turns the offset when the call asked for the object's rotation.
    camera::StoryCinematic rotated;
    rotated.start(0.0, {0, 0, 0}, {0, 0, 0});
    const auto read_turned = [](std::uint64_t) -> std::optional<camera::StoryAttachment> {
        camera::StoryAttachment attached;
        attached.position = {1000, 0, 0};
        attached.rotation = std::array<std::array<double, 3>, 3>{{{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}}; // 90 degrees about Z
        attached.z_adjust = 3.0;
        return attached;
    };
    const camera::StoryAttachmentReader turned(read_turned);
    auto with_rotation = key({0, 0, 0}, 10, 0, 0, false);
    with_rotation.object = 1;
    with_rotation.use_object_rotation = true;
    rotated.set_camera_key(0.0, with_rotation);
    expect(near(rotated.pose(1.0, turned)->eye, {1000, 10, 3}), "the object's rotation turns the offset, and its height adjustment lifts it");
    with_rotation.use_object_rotation = false;
    rotated.set_camera_key(2.0, with_rotation);
    expect(near(rotated.pose(3.0, turned)->eye, {1010, 0, 3}), "without the rotation flag the offset stays in world axes");
    // The cached matrix serves a vanished object.
    with_rotation.use_object_rotation = true;
    rotated.set_camera_key(4.0, with_rotation);
    (void)rotated.pose(5.0, turned);
    expect(near(rotated.pose(6.0, no_objects)->eye, {1000, 10, 3}), "a vanished object's last matrix and height still place the offset");
    // The drawn centre.
    camera::StoryCinematic centred;
    centred.start(0.0, {0, 0, 0}, {0, 0, 0});
    const auto read_centre = [](std::uint64_t) -> std::optional<camera::StoryAttachment> {
        camera::StoryAttachment attached;
        attached.position = {0, 0, 0};
        attached.centre = std::array<double, 3>{50, 60, 70};
        return attached;
    };
    const camera::StoryAttachmentReader with_centre(read_centre);
    auto aim = key({0, 0, 0}, 1, 2, 3, false);
    aim.object = 9;
    aim.centre = true;
    centred.set_target_key(0.0, aim);
    expect(near(centred.pose(1.0, with_centre)->target, {51, 62, 73}), "the centre flag aims at the drawn centre plus the offset");
}

void test_the_return_to_the_player() {
    camera::StoryCinematic cinematic;
    cinematic.start(0.0, {0, 0, 100}, {0, 0, 0});
    cinematic.set_camera_key(0.0, key({500, 0, 0}, 0, 0, 0, false));
    cinematic.set_target_key(0.0, key({500, 100, 0}, 0, 0, 0, false));
    (void)cinematic.pose(10.0, no_objects);
    cinematic.transition_to_tactical(50.0, 2.0, {0, 0, 100}, {0, 0, 0}); // 60 ticks
    const auto start = cinematic.pose(50.0, no_objects);
    expect(start && near(start->eye, {500, 0, 0}) && near(start->target, {500, 100, 0}), "the return starts where the cinematic is");
    const auto middle = cinematic.pose(80.0, no_objects);
    expect(middle && near(middle->eye, {250, 0, 50}, 0.5) && near(middle->target, {250, 50, 0}, 0.5), "halfway in time is halfway back");
    const auto done = cinematic.pose(110.0, no_objects);
    expect(done && near(done->eye, {0, 0, 100}) && near(done->target, {0, 0, 0}), "the return ends on the player's pose");
    expect(near(cinematic.pose(500.0, no_objects)->eye, {0, 0, 100}), "and holds it until the cinematic ends");
    // A time of zero still takes one tick, never dividing by zero.
    cinematic.set_camera_key(600.0, key({9, 9, 9}, 0, 0, 0, false));
    (void)cinematic.pose(600.0, no_objects);
    cinematic.transition_to_tactical(600.0, 0.0, {1, 2, 3}, {4, 5, 6});
    const auto instant = cinematic.pose(602.0, no_objects);
    expect(instant && near(instant->eye, {1, 2, 3}) && near(instant->target, {4, 5, 6}), "a zero-second return arrives within a tick or two");
}

void test_rejections() {
    camera::StoryCinematic cinematic;
    cinematic.start(0.0, {0, 0, 0}, {1, 1, 1});
    const double nan = std::numeric_limits<double>::quiet_NaN();
    cinematic.set_camera_key(1.0, key({nan, 0, 0}, 0, 0, 0, false));
    cinematic.transition_target_key(1.0, key({0, nan, 0}, 0, 0, 0, false), 2.0);
    cinematic.transition_to_tactical(1.0, 1.0, {nan, 0, 0}, {0, 0, 0});
    const auto pose = cinematic.pose(2.0, no_objects);
    expect(pose && near(pose->eye, {0, 0, 0}) && near(pose->target, {1, 1, 1}), "a non-finite key or return changes nothing");
}

} // namespace

int main() {
    test_the_ease();
    test_an_idle_cinematic();
    test_the_offset_of_a_key();
    test_a_timed_transition();
    test_the_target_track_is_separate();
    test_an_attached_key();
    test_the_return_to_the_player();
    test_rejections();
    if (failures != 0) {
        std::cerr << failures << " story cinematic contract failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "story cinematic contracts passed\n";
    return EXIT_SUCCESS;
}
