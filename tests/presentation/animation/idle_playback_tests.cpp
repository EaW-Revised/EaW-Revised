// Idle clip playback contracts (#145, #157): the Idle_Anim_00_Rate_Mod parse,
// the per-placement start frame, the clip position a 30 Hz clock reaches and
// the pose sampled there. The clip lengths below match the Coruscant asteroid
// (300 frames at 5 fps) and space junk (590 frames at 30 fps) idle clips; no
// asset is read.

#include "eawr/presentation/animation/idle_playback.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace idle = eawr::presentation::animation;

int failures{};
void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool rate_is(const std::string_view text, const std::uint32_t numerator, const std::uint32_t denominator) {
    const auto rate = idle::parse_rate_mod(text);
    return rate && *rate == idle::RateMod{numerator, denominator};
}

void test_rate_mod_parse() {
    expect(rate_is("1.0", 1, 1), "1.0 is rate 1");
    expect(rate_is(" 0.25 ", 1, 4), "0.25 reduces to 1/4, whitespace trimmed");
    expect(rate_is("8.0", 8, 1) && rate_is("8", 8, 1), "8.0 and 8 are rate 8");
    expect(rate_is("0.00", 0, 1), "0.00 is a zero rate");
    expect(rate_is(".5", 1, 2) && rate_is("2.", 2, 1), "a bare fraction or trailing point parses");
    expect(rate_is("0.000001", 1, 1000000), "six fraction digits are exact");
    expect(rate_is("4294967295", 4294967295U, 1), "the uint32 range is accepted");
    for (const std::string_view bad : {"", " ", ".", "-1", "+1", "1e2", "1.2.3", "1.2345678", "4294967296", "one"}) {
        expect(!idle::parse_rate_mod(bad), "rejects \"" + std::string(bad) + "\"");
    }
}

void test_start_frame() {
    expect(idle::idle_start_frame("any", 0) == 0 && idle::idle_start_frame("any", 1) == 0,
           "no or one frame starts at frame 0");
    const std::string map = "data/art/maps/_mp_space_coruscant.ted#";
    expect(idle::idle_start_frame(map + "2", 300) == idle::idle_start_frame(map + "2", 300),
           "the start frame of a placement repeats");
    std::set<std::uint32_t> seen;
    bool in_range = true;
    for (int record = 0; record < 300; ++record) {
        const std::uint32_t frame = idle::idle_start_frame(map + std::to_string(record), 300);
        in_range = in_range && frame < 300;
        seen.insert(frame);
    }
    expect(in_range, "start frames lie in [0, frames - 1]");
    expect(seen.size() >= 150, "neighbouring records spread over the clip");
}

std::optional<idle::ClipPosition> at(const idle::IdlePlayback& playback, const std::uint32_t start,
                                     const std::uint32_t playable, const std::uint32_t fps, const std::uint64_t tick) {
    return idle::idle_position(playback, start, playable, fps, tick, 30);
}

bool is(const std::optional<idle::ClipPosition>& position, const std::uint64_t value, const std::uint32_t subdivisions) {
    return position && *position == idle::ClipPosition{value, subdivisions};
}

void test_looping_position() {
    const idle::IdlePlayback loop{.loop = true, .restarts = true};
    // Asteroid clip: 300 frames at 5 fps; one frame is six 30 Hz ticks.
    expect(is(at(loop, 100, 300, 5, 0), 3000, 30), "tick 0 sits on the start frame");
    expect(is(at(loop, 100, 300, 5, 6), 3030, 30), "six ticks advance one 5 fps frame");
    expect(is(at(loop, 100, 300, 5, 1200), 0, 30), "the loop wraps to frame 0");
    expect(is(at(loop, 100, 300, 5, 1800), 3000, 30), "a full 60 s loop returns to the start frame");
    expect(is(at(loop, 400, 300, 5, 0), 3000, 30), "the start frame is reduced modulo the playable frames");
    const std::uint64_t far = 1800ULL * 1000000000000000ULL + 6U;
    expect(is(at(loop, 0, 300, 5, far), 30, 30), "a large tick reduces exactly");
    // Space junk clip: 590 frames at 30 fps, one frame per tick.
    expect(is(at(loop, 589, 590, 30, 1), 0, 30), "the junk clip wraps after its last frame");
    idle::IdlePlayback fixed = loop;
    fixed.random_start = false;
    expect(is(at(fixed, 100, 300, 5, 0), 0, 30), "a DUMMY_STARSHIP ignores the start frame");
}

void test_rate_mod_position() {
    idle::IdlePlayback quarter{.loop = true, .restarts = true, .rate = {1, 4}};
    expect(is(at(quarter, 2, 300, 5, 0), 240, 120), "a quarter rate subdivides frames by four");
    expect(is(at(quarter, 2, 300, 5, 24), 360, 120), "24 ticks at a quarter rate advance one 5 fps frame");
    const idle::IdlePlayback eight{.loop = true, .restarts = true, .rate = {8, 1}};
    expect(is(at(eight, 0, 300, 5, 3), 120, 30), "rate 8 advances eight times as far");
    const idle::IdlePlayback frozen{.loop = true, .restarts = true, .rate = {0, 1}};
    expect(is(at(frozen, 7, 300, 5, 12345), 210, 30), "a zero rate freezes the start frame");
}

void test_non_looping_position() {
    // 10 frames at 30 fps from frame 7: three ticks reach the end.
    const idle::IdlePlayback restart{.loop = false, .restarts = true};
    expect(is(at(restart, 7, 10, 30, 2), 270, 30), "the first pass runs from the start frame");
    expect(is(at(restart, 7, 10, 30, 3), 0, 30), "the IDLE behaviour restarts a finished clip at frame 0");
    expect(is(at(restart, 7, 10, 30, 4), 30, 30), "the restarted clip plays on");
    expect(is(at(restart, 7, 10, 30, 13), 0, 30), "the restarted clip loops");
    const idle::IdlePlayback hold{.loop = false, .restarts = false};
    expect(is(at(hold, 7, 10, 30, 3), 300, 30) && is(at(hold, 7, 10, 30, 100), 300, 30),
           "without the IDLE behaviour the clip holds its last frame");
    const idle::IdlePlayback half{.loop = false, .restarts = true, .rate = {1, 2}};
    expect(is(at(half, 7, 10, 30, 5), 570, 60), "the first pass keeps Rate_Mod");
    expect(is(at(half, 7, 10, 30, 6), 0, 60) && is(at(half, 7, 10, 30, 7), 60, 60),
           "the restart plays at rate 1");
    const idle::IdlePlayback stopped{.loop = false, .restarts = true, .rate = {0, 1}};
    expect(is(at(stopped, 7, 10, 30, 1000), 210, 30), "a zero rate never finishes its first pass");
}

void test_position_limits() {
    const idle::IdlePlayback loop{.loop = true};
    expect(!idle::idle_position(loop, 0, 300, 0, 1, 30), "a zero frame rate is refused");
    expect(!idle::idle_position(loop, 0, 300, 5, 1, 0), "a zero tick rate is refused");
    expect(is(idle::idle_position(loop, 5, 0, 5, 9, 30), 0, 30), "a clipless player sits at position 0");
    constexpr std::uint32_t most = std::numeric_limits<std::uint32_t>::max();
    const idle::IdlePlayback fast{.loop = true, .rate = {most, 1}};
    expect(!idle::idle_position(fast, 0, most, most, 5, 30), "a speed past 64 bits is refused");
    const idle::IdlePlayback fine{.loop = true, .rate = {1, 2}};
    expect(!idle::idle_position(fine, 0, 300, 5, 1, most), "subdivisions past 32 bits are refused");
    // The largest accepted loop: start + advance exceeds 64 bits before the modulo.
    const std::uint64_t period = static_cast<std::uint64_t>(most) * most;
    expect(is(idle::idle_position(loop, most - 1, most, 1, period - 1, most),
              (static_cast<std::uint64_t>(most) - 1) * most - 1, most),
           "a loop position near 64 bits reduces without wrapping");
}

// One bone moving along x through 0, 10 and back to 0: two playable frames
// at `fps`, the third stored frame closing the loop.
std::optional<idle::Player> synthetic_player(const float fps) {
    eawr::assets::Bone root;
    root.name = "root";
    root.parent = -1;
    root.visible = true;
    root.relative_transform = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    eawr::assets::Model model;
    model.bones = {root};
    eawr::assets::AnimationTrack track;
    track.bone_index = 0;
    track.bone_name = "root";
    track.translation_interpolation = eawr::assets::Interpolation::linear;
    track.scale_interpolation = eawr::assets::Interpolation::linear;
    track.rotation_interpolation = eawr::assets::Interpolation::spherical;
    track.samples = {
        {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, true},
        {{10.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, true},
        {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, true},
    };
    eawr::assets::Animation animation;
    animation.source.logical_path = "synthetic_idle_00.ala";
    animation.stored_frame_count = 3;
    animation.playable_frame_count = 2;
    animation.frames_per_second = fps;
    animation.duration_seconds = 2.0F / fps;
    animation.tracks.push_back(track);
    auto player = idle::Player::create(model, &animation);
    if (!player) return std::nullopt;
    return std::move(player.value());
}

void test_sample_idle() {
    const auto player = synthetic_player(1.0F);
    expect(player.has_value(), "the synthetic idle clip binds");
    if (!player) return;
    const auto x_at = [&](const idle::IdlePlayback& playback, const std::uint32_t start,
                          const std::uint64_t tick) -> std::optional<float> {
        const auto pose = idle::sample_idle(*player, playback, start, tick, 30);
        if (!pose) return std::nullopt;
        return pose.value().bones[0].model_asset[12];
    };
    const auto near = [](const std::optional<float> value, const float expected) {
        return value && std::abs(*value - expected) < 0.0001F;
    };
    const idle::IdlePlayback loop{.loop = true};
    expect(near(x_at(loop, 1, 0), 10.0F), "sampling starts on the start frame");
    expect(near(x_at(loop, 1, 15), 5.0F), "half a 1 fps frame interpolates");
    expect(near(x_at(loop, 1, 30), 0.0F) && near(x_at(loop, 1, 60), 10.0F), "the loop wraps on its exact frame");
    const idle::IdlePlayback hold{};
    expect(near(x_at(hold, 1, 30), 0.0F) && near(x_at(hold, 1, 3000), 0.0F),
           "a clip without loop or IDLE holds its last stored frame");
    const idle::IdlePlayback restart{.restarts = true};
    expect(near(x_at(restart, 1, 45), 5.0F), "an IDLE clip restarts from frame 0");
    for (const float bad : {0.0F, 1.5F, -1.0F}) {
        const auto odd = synthetic_player(bad);
        expect(!odd || !idle::sample_idle(*odd, loop, 0, 1, 30), "a non-integral frame rate is refused");
    }
    expect(!idle::sample_idle(*player, loop, 0, 1, 0), "a zero tick rate is refused");
}

} // namespace

int main() {
    test_rate_mod_parse();
    test_start_frame();
    test_looping_position();
    test_rate_mod_position();
    test_non_looping_position();
    test_position_limits();
    test_sample_idle();
    if (failures != 0) return EXIT_FAILURE;
    std::cout << "idle playback contracts passed\n";
    return EXIT_SUCCESS;
}
