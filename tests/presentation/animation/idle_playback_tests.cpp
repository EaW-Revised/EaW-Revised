// Idle clip playback contracts (#145, #157): the Idle_Anim_00_Rate_Mod parse,
// the per-placement start frame, the clip position a 30 Hz clock reaches and
// the pose sampled there. The clip lengths below match the Coruscant asteroid
// (300 frames at 5 fps) and space junk (590 frames at 30 fps) idle clips; no
// asset is read.

#include "eawr/presentation/animation/idle_playback.hpp"
#include "eawr/presentation/skin_pose.hpp"
#include "../../../src/presentation/godot/submission_plan.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>

namespace {
std::atomic<std::size_t> global_allocations{0};
}

// Match the render-stream allocation budget: measure work, never wall time.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(const std::size_t size) {
    global_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* const memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* const memory) noexcept { std::free(memory); }
void operator delete(void* const memory, std::size_t) noexcept { std::free(memory); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

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
std::optional<idle::Player> synthetic_player(const float fps, const std::size_t bones = 1) {
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
    for (std::size_t index = 1; index < bones; ++index) {
        auto child = root;
        child.name = "child" + std::to_string(index);
        child.parent = static_cast<std::int32_t>(index - 1);
        child.relative_transform[3] = 2.0F;
        model.bones.push_back(child);
        auto child_track = track;
        child_track.bone_index = static_cast<std::uint32_t>(index);
        child_track.bone_name = child.name;
        child_track.samples[1].visible = false;
        child_track.samples[1].scale = {2.0F, 1.0F, 0.5F};
        child_track.samples[1].rotation = {0.0F, 0.0F, 1.0F, 0.0F};
        animation.tracks.push_back(std::move(child_track));
    }
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

void test_environment_idle_budget() {
    constexpr std::size_t bone_count = 8;
    const auto player = synthetic_player(5.0F, bone_count);
    expect(player.has_value(), "the hierarchy budget fixture binds");
    if (!player) return;
    // Sphere, ring and glow reuse the same sampled pose and own their palettes.
    const std::array instances{std::pair{1U, 10U}, std::pair{2U, 11U}, std::pair{3U, 12U}};
    struct Pending final {
        unsigned asset_id{};
        std::vector<idle::Matrix> palette;
        std::vector<idle::Matrix> model_transforms;
    };
    std::unordered_map<unsigned, Pending> pending;
    std::size_t converted = 0;
    std::size_t billboard_converted = 0;
    std::size_t descendant_steps = 0;
    std::size_t descendants = 0;
    bool billboard_correct = true;
    eawr::presentation::BillboardPoseScratch<idle::Matrix> scratch;
    scratch.prepare(bone_count);
    const std::array<std::int32_t, bone_count> parents{-1, 0, 1, 2, 3, 4, 5, 6};
    const auto bind = [&](const unsigned entity, const unsigned asset, const auto& bones) {
        if (!eawr::presentation::cache_skin_pose(pending, entity, asset, bones,
                [&](const idle::Matrix& matrix) { ++converted; return matrix; }))
            return eawr::core::Result<void>::failure({});
        const auto& stored = pending.at(entity);
        scratch.reset(stored.model_transforms, stored.palette, idle::Player::identity_matrix(),
            [&](const idle::Matrix& matrix) { ++billboard_converted; return matrix; });
        // A billboard bone's delta propagates in source order to its children.
        descendant_steps += eawr::presentation::visit_skin_descendants(parents, 3,
            [&](const std::size_t child) {
                ++descendants;
                scratch.models[child][12] += 1.0F;
                scratch.palettes[child][12] += 1.0F;
            });
        for (std::size_t bone = 0; bone < bone_count; ++bone) {
            auto model = stored.model_transforms[bone];
            auto palette = stored.palette[bone];
            if (bone >= 3) { model[12] += 1.0F; palette[12] += 1.0F; }
            billboard_correct = billboard_correct && scratch.models[bone] == model
                && scratch.palettes[bone] == palette;
        }
        return eawr::core::Result<void>::success();
    };
    for (const idle::IdlePlayback playback : {
            idle::IdlePlayback{.loop = true, .rate = {1, 4}},
            idle::IdlePlayback{.restarts = true, .rate = {8, 1}},
            idle::IdlePlayback{}, idle::IdlePlayback{.loop = true, .rate = {0, 1}}}) {
        idle::IdlePose state;
        expect(bool(state.advance(*player, playback, 1, 0, 30, instances, bind)), "load initializes idle buffers");
        namespace submit = eawr::presentation::godot_backend::detail;
        std::array<submit::PlacedPiece, 3> placed{};
        const eawr::sim::math::Mat3x4 fixed{};
        for (auto& piece : placed) static_cast<void>(submit::place_piece(piece, false, fixed));
        std::size_t static_transforms = 0;
        const auto* const storage = state.pose.bones.data();
        bool correct = true;
        std::size_t allocations = 0;
        for (std::uint64_t tick = 1; tick <= 4096; ++tick) {
            // Build the independently owned reference outside the measured path.
            const auto reference = idle::sample_idle(*player, playback, 1, tick, 30);
            const auto before = global_allocations.load(std::memory_order_relaxed);
            const std::size_t conversions_before = converted;
            const std::size_t billboard_before = billboard_converted;
            const std::size_t steps_before = descendant_steps;
            const std::size_t descendants_before = descendants;
            const auto advanced = state.advance(*player, playback, 1, tick, 30, instances, bind);
            for (auto& piece : placed) static_transforms += submit::place_piece(piece, false, fixed);
            allocations += global_allocations.load(std::memory_order_relaxed) - before;
            correct = correct && bool(advanced) && bool(reference) && state.pose.bones.data() == storage
                && state.work.samples == 1 && state.work.bones == bone_count && state.work.bindings == 3
                && converted - conversions_before == 3 * bone_count && player->sampled(state.pose)
                && billboard_converted - billboard_before == 3 * bone_count
                && descendant_steps - steps_before == 3 * 16
                && descendants - descendants_before == 3 * 5;
            if (reference) {
                correct = correct && state.pose.sampled_time_seconds == reference.value().sampled_time_seconds;
                for (std::size_t bone = 0; bone < bone_count; ++bone) {
                    const auto& actual = state.pose.bones[bone];
                    const auto& expected = reference.value().bones[bone];
                    correct = correct && actual.local_asset == expected.local_asset
                        && actual.model_asset == expected.model_asset && actual.skin_asset == expected.skin_asset
                        && actual.visible == expected.visible;
                    for (const auto& [entity, asset] : instances) {
                        const auto& stored = pending.at(entity);
                        auto skin = expected.skin_asset;
                        auto model = expected.model_asset;
                        if (!expected.visible) {
                            for (std::size_t component = 0; component < 12; ++component) {
                                skin[component] = 0.0F;
                                model[component] = 0.0F;
                            }
                        }
                        correct = correct && stored.asset_id == asset
                            && stored.palette[bone] == idle::Player::asset_to_render_transform(skin)
                            && stored.model_transforms[bone] == idle::Player::asset_to_render_transform(model);
                    }
                }
            }
            const auto held_before = global_allocations.load(std::memory_order_relaxed);
            const auto held = state.advance(*player, playback, 1, tick, 30, instances, bind);
            allocations += global_allocations.load(std::memory_order_relaxed) - held_before;
            correct = correct && bool(held) && state.work.samples == 0 && state.work.bones == 0
                && state.work.bindings == 0 && converted - conversions_before == 3 * bone_count
                && billboard_converted - billboard_before == 3 * bone_count
                && descendant_steps - steps_before == 3 * 16 && descendants - descendants_before == 3 * 5;
        }
        expect(allocations == 0, "advancing and held steady-state idle ticks allocate nothing");
        expect(static_transforms == 0, "animated palettes send no static placement transforms after load");
        expect(correct && billboard_correct,
            "one sample, three palette/billboard updates and bounded parent walks; held ticks do no work");
        const auto previous = state.pose;
        const auto previous_tick = state.tick;
        expect(!state.advance(*player, playback, 1, 5000, 0, instances, bind), "invalid tick rate is rejected");
        expect(state.tick == previous_tick && state.pose.bones[0].model_asset == previous.bones[0].model_asset,
            "invalid sampling retains the last valid pose and tick");
        auto invalid = state.pose;
        invalid.bones.back().model_asset[15] = std::numeric_limits<float>::infinity();
        const auto palette = pending.at(1).palette;
        expect(!bind(1, 10, invalid.bones) && pending.at(1).palette == palette,
            "a late non-finite bone preserves the whole previous renderer palette");
        invalid = state.pose;
        invalid.bones.back().skin_asset[0] = std::numeric_limits<float>::quiet_NaN();
        expect(!bind(1, 10, invalid.bones) && pending.at(1).palette == palette,
            "a non-finite skin matrix preserves the whole previous palette");
    }
    // Restore every scratch element, including null poses and smaller/larger
    // skeleton switches, after a previous instance changed the working arrays.
    const auto& stored = pending.at(1);
    const auto before = global_allocations.load(std::memory_order_relaxed);
    scratch.reset(std::span<const idle::Matrix>(stored.model_transforms).first(2), {},
        idle::Player::identity_matrix(), [](const idle::Matrix& matrix) { return matrix; });
    bool reset = scratch.models[0] == stored.model_transforms[0]
        && scratch.palettes[0] == idle::Player::identity_matrix();
    scratch.reset(stored.model_transforms, stored.palette, idle::Player::identity_matrix(),
        [](const idle::Matrix& matrix) { return matrix; });
    const auto allocated = global_allocations.load(std::memory_order_relaxed) - before;
    reset = reset && scratch.models == stored.model_transforms && scratch.palettes == stored.palette;
    expect(allocated == 0 && reset, "billboard scratch resets fully without allocation across pose and skeleton switches");
}

void test_skin_palette_changes() {
    struct Pending final {
        unsigned asset_id{};
        std::vector<idle::Matrix> palette;
        std::vector<idle::Matrix> model_transforms;
    };
    std::unordered_map<unsigned, Pending> pending;
    std::vector<idle::BonePose> bones(256);
    for (auto& bone : bones) {
        bone.skin_asset = bone.model_asset = idle::Player::identity_matrix();
    }
    std::vector<std::uint8_t> changes;
    const auto cache = [&](const unsigned asset) {
        return eawr::presentation::cache_skin_pose(pending, 1U, asset, bones,
            [](const idle::Matrix& matrix) { return matrix; }, &changes);
    };
    const auto count = [&] { return std::count(changes.begin(), changes.end(), 1); };
    expect(cache(10) && count() == 256, "a new skeleton receives its complete palette");
    expect(cache(10) && count() == 0, "repeating a palette requires no bone writes");
    bones[17].skin_asset[12] = 4.0F;
    expect(cache(10) && count() == 1 && changes[17] == 1, "only the changed bone requires a write");
    bones[5].model_asset[12] = 7.0F;
    expect(cache(10) && count() == 0 && pending.at(1).model_transforms[5][12] == 7.0F,
        "unchanged skin matrices still retain new model transforms for billboard refresh");
    const auto visible_palette = pending.at(1).palette[17];
    const auto source_skin = bones[17].skin_asset;
    bones[17].visible = false;
    expect(cache(10) && count() == 1 && changes[17] == 1,
        "visibility alone changes the rendered palette");
    expect(std::all_of(pending.at(1).palette[17].begin(), pending.at(1).palette[17].begin() + 12,
            [](const float value) { return value == 0.0F; })
        && std::all_of(pending.at(1).model_transforms[17].begin(),
            pending.at(1).model_transforms[17].begin() + 12, [](const float value) { return value == 0.0F; })
        && bones[17].skin_asset == source_skin,
        "a hidden bind bone collapses geometry and billboard basis without modifying its source pose");
    expect(cache(10) && count() == 0, "a retained hidden bone requires no repeated palette write");
    bones[17].visible = true;
    expect(cache(10) && count() == 1 && changes[17] == 1
        && pending.at(1).palette[17] == visible_palette,
        "a visible animation sample restores the authored geometry");
    // Bit identity includes signed zero, even when float equality considers it equal.
    auto& zero = pending.at(1).palette[4][1];
    zero = std::copysign(0.0F, std::signbit(zero) ? 1.0F : -1.0F);
    expect(cache(10) && count() == 1 && changes[4] == 1, "signed-zero palette changes cross the engine boundary");
    expect(cache(11) && count() == 256, "changing the asset restores the complete palette");
    const auto retained = pending.at(1).palette;
    const auto retained_changes = changes;
    bones[0].skin_asset[0] = std::numeric_limits<float>::quiet_NaN();
    expect(!cache(12) && pending.at(1).asset_id == 11 && pending.at(1).palette == retained
        && changes == retained_changes, "a rejected pose leaves palette, asset and change flags intact");
    bones[0].skin_asset = idle::Player::identity_matrix();
    pending.erase(1);
    expect(cache(11) && count() == 256, "clearing the cached pose forces a complete next palette");
    bones.resize(32);
    expect(cache(11) && count() == 32, "a changed skeleton size forces every retained bone");
    expect(cache(11) && count() == 0, "the smaller skeleton then suppresses repeated matrices");
    std::vector<std::uint8_t> used(bones.size(), 0);
    expect(eawr::presentation::mark_skin_bone_usage(used, 17, 1.0F)
        && eawr::presentation::mark_skin_bone_usage(used, 5, 0.125F)
        && eawr::presentation::mark_skin_bone_usage(used, 0, 0.0F)
        && std::count(used.begin(), used.end(), 1) == 2, "usage includes every positive influence and no zero weights");
    expect(!eawr::presentation::mark_skin_bone_usage(used, 99, 1.0F)
        && !eawr::presentation::mark_skin_bone_usage(used, 1, -1.0F)
        && !eawr::presentation::mark_skin_bone_usage(used, 1, std::numeric_limits<float>::quiet_NaN()),
        "invalid palette influences fail closed");
    changes.assign(bones.size(), 1);
    expect(eawr::presentation::filter_skin_palette_changes(changes, used)
        && count() == 2 && changes[17] == 1 && changes[5] == 1,
        "a single-surface asset writes all its used bones and omits sibling palette entries");
    changes.assign(bones.size(), 0);
    changes[17] = 1;
    changes[2] = 1;
    expect(eawr::presentation::filter_skin_palette_changes(changes, used) && count() == 1 && changes[17] == 1,
        "an unchanged used bone is omitted while a changed used bone always crosses the boundary");
    const auto selected = changes;
    expect(!eawr::presentation::filter_skin_palette_changes(changes, std::span<const std::uint8_t>(used).first(1))
        && changes == selected, "a mismatched usage mask rejects without altering changes");
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
    test_environment_idle_budget();
    test_skin_palette_changes();
    if (failures != 0) return EXIT_FAILURE;
    std::cout << "idle playback contracts passed\n";
    return EXIT_SUCCESS;
}
