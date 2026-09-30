#include "eawr/presentation/animation/animation.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace eawr;
namespace playback = eawr::presentation::animation;
using playback::Matrix;

int failures{};
void expect(const bool condition, const std::string_view message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; ++failures; }
}
bool close(const float left, const float right) { return std::abs(left - right) < 0.0001F; }

assets::Bone bone(const std::string_view name, const std::int32_t parent, const float x = 0.0F) {
    assets::Bone value;
    value.name = name; value.parent = parent; value.visible = true;
    value.relative_transform = {1.0F, 0.0F, 0.0F, x,
        0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F};
    return value;
}
assets::Bone bone_with_transform(const std::string_view name, const std::int32_t parent,
    const Matrix& local, const bool visible) {
    assets::Bone value;
    value.name = name; value.parent = parent; value.visible = visible;
    value.relative_transform = {local[0], local[4], local[8], local[12],
        local[1], local[5], local[9], local[13],
        local[2], local[6], local[10], local[14]};
    return value;
}
void expect_matrix_close(const Matrix& actual, const Matrix& expected, const std::string_view message) {
    bool matches = true;
    for (std::size_t index = 0; index < actual.size(); ++index)
        matches = matches && close(actual[index], expected[index]);
    expect(matches, message);
}
assets::AnimationTrack track(const std::uint32_t index, const std::string_view name) {
    assets::AnimationTrack value;
    value.bone_index = index; value.bone_name = name;
    value.translation_interpolation = assets::Interpolation::linear;
    value.scale_interpolation = assets::Interpolation::linear;
    value.rotation_interpolation = assets::Interpolation::spherical;
    value.samples = {
        {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, true},
        {{10.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 1.0F, 0.0F}, false},
        {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, true},
    };
    return value;
}
assets::Animation animation_data() {
    assets::Animation value;
    value.source.logical_path = "synthetic.ala";
    value.stored_frame_count = 3; value.playable_frame_count = 2;
    value.frames_per_second = 1.0F; value.duration_seconds = 2.0F;
    value.tracks.push_back(track(0, "root"));
    return value;
}

void test_bind_identity_and_missing_tracks() {
    assets::Model model; model.bones = {bone("root", -1), bone("hand", 0, 2.0F)};
    const auto player = playback::Player::create(model);
    expect(static_cast<bool>(player), "bind-only player creates");
    if (!player) return;
    const auto pose = player.value().sample({});
    expect(static_cast<bool>(pose), "bind-only pose samples");
    if (!pose) return;
    expect(close(pose.value().bones[0].model_asset[0], 1.0F) && close(pose.value().bones[0].model_asset[12], 0.0F), "bind root is identity");
    expect(close(pose.value().bones[1].model_asset[12], 2.0F), "missing track keeps child bind pose");
    expect(close(pose.value().bones[1].skin_asset[12], 0.0F), "bind skin palette is identity");
}

void test_rotation_interpolation_hierarchy_loop_and_attachments() {
    assets::Model model; model.bones = {bone("root", -1), bone("hand", 0, 2.0F)};
    const assets::Animation animation = animation_data();
    const auto player = playback::Player::create(model, &animation);
    expect(static_cast<bool>(player), "animation player accepts matched track");
    if (!player) return;
    const auto pose = player.value().sample({0.5F, playback::PlaybackMode::loop, 0.0F});
    expect(static_cast<bool>(pose), "interpolated pose samples");
    if (!pose) return;
    expect(close(pose.value().sampled_time_seconds, 0.5F), "sample time retained");
    expect(close(pose.value().bones[0].model_asset[12], 5.0F), "translation linearly interpolates");
    expect(close(pose.value().bones[0].model_asset[0], 0.0F) && close(pose.value().bones[0].model_asset[1], 1.0F), "rotation spherically interpolates");
    expect(close(pose.value().bones[1].model_asset[12], 5.0F) && close(pose.value().bones[1].model_asset[13], 2.0F), "untracked child follows animated hierarchy");
    expect(pose.value().bones[0].visible, "visibility uses step interpolation");
    expect(close(pose.value().bones[1].skin_asset[12], 5.0F), "skin palette is animated model times inverse bind");
    const auto looped = player.value().sample({2.5F, playback::PlaybackMode::loop, 0.0F});
    expect(looped && close(looped.value().sampled_time_seconds, 0.5F), "loop uses playable duration not stored terminal count");
    const auto attachment = player.value().attachment(pose.value(), "hand", playback::AttachmentSpace::model);
    expect(attachment && close(attachment.value().column_major[12], 5.0F) && close(attachment.value().column_major[14], -2.0F), "model attachment applies one Z-up to Y-up conversion");
    expect(attachment && close(attachment.value().column_major[2], -1.0F)
        && close(attachment.value().column_major[8], 1.0F),
        "model attachment conjugates animated orientation into render basis");
    Matrix world = playback::Player::identity_matrix(); world[13] = 7.0F;
    const auto world_attachment = player.value().attachment(pose.value(), "hand", playback::AttachmentSpace::world, world);
    expect(world_attachment && close(world_attachment.value().column_major[13], 7.0F), "world attachment prepends render world without another conversion");
    const auto missing = player.value().attachment(pose.value(), "absent", playback::AttachmentSpace::model);
    expect(!missing && missing.error().code == playback::diagnostic_codes::missing_attachment, "missing attachment is diagnostic, not identity fallback");
}

void test_validation_and_blend() {
    assets::Model model; model.bones = {bone("root", -1)};
    assets::Animation invalid = animation_data(); invalid.tracks.front().bone_name = "wrong";
    const auto rejected = playback::Player::create(model, &invalid);
    expect(!rejected && rejected.error().code == playback::diagnostic_codes::invalid_animation, "track name/index mismatch is rejected");
    assets::Animation animation = animation_data();
    assets::Animation bad_duration = animation;
    bad_duration.duration_seconds = 3.0F;
    const auto duration_rejected = playback::Player::create(model, &bad_duration);
    expect(!duration_rejected
        && duration_rejected.error().code == playback::diagnostic_codes::invalid_animation,
        "inconsistent duration metadata is rejected");
    const auto player = playback::Player::create(model, &animation);
    if (!player) { expect(false, "blend player creates"); return; }
    const auto blended = player.value().sample({1.0F, playback::PlaybackMode::clamp, 1.0F});
    expect(blended && close(blended.value().bones[0].model_asset[12], 0.0F) && close(blended.value().bones[0].model_asset[0], 1.0F), "blend-to-bind produces idle pose");
    const auto unblended = player.value().sample({1.0F, playback::PlaybackMode::clamp, 0.0F});
    expect(unblended && close(unblended.value().bones[0].model_asset[12], 10.0F)
        && !unblended.value().bones[0].visible, "zero blend retains sampled transform and visibility");
    const auto halfway = player.value().sample({1.0F, playback::PlaybackMode::clamp, 0.5F});
    expect(halfway && close(halfway.value().bones[0].model_asset[12], 5.0F)
        && !halfway.value().bones[0].visible, "midpoint blend retains transform blend and sampled visibility");
    const auto bad_request = player.value().sample({0.0F, playback::PlaybackMode::loop, 1.1F});
    expect(!bad_request && bad_request.error().code == playback::diagnostic_codes::invalid_request, "invalid blend is rejected");
    const auto bad_endpoint_time = player.value().sample({std::numeric_limits<float>::quiet_NaN(),
        playback::PlaybackMode::loop, 1.0F});
    expect(!bad_endpoint_time && bad_endpoint_time.error().code == playback::diagnostic_codes::invalid_request,
        "invalid time remains rejected at the bind endpoint");

    assets::Animation stepped = animation;
    stepped.tracks.front().translation_interpolation = assets::Interpolation::step;
    const auto stepped_player = playback::Player::create(model, &stepped);
    const auto stepped_pose = stepped_player ? stepped_player.value().sample({0.5F})
        : eawr::core::Result<playback::Pose>::failure(stepped_player.error());
    expect(stepped_pose && close(stepped_pose.value().bones[0].model_asset[12], 0.0F),
        "step translation retains the preceding key");

    assets::Model ambiguous; ambiguous.bones = {bone("socket", -1), bone("socket", 0)};
    const auto ambiguous_player = playback::Player::create(ambiguous);
    const auto ambiguous_pose = ambiguous_player ? ambiguous_player.value().sample({})
        : eawr::core::Result<playback::Pose>::failure(ambiguous_player.error());
    const auto ambiguous_attachment = ambiguous_pose
        ? ambiguous_player.value().attachment(
            ambiguous_pose.value(), "socket", playback::AttachmentSpace::model)
        : eawr::core::Result<playback::AttachmentTransform>::failure(ambiguous_pose.error());
    expect(!ambiguous_attachment
        && ambiguous_attachment.error().code == playback::diagnostic_codes::invalid_request,
        "duplicate attachment names are rejected as ambiguous");

    assets::Model singular; singular.bones = {bone("root", -1)};
    singular.bones[0].relative_transform[0] = 0.0F;
    const auto singular_player = playback::Player::create(singular);
    const auto singular_pose = singular_player ? singular_player.value().sample({})
        : eawr::core::Result<playback::Pose>::failure(singular_player.error());
    expect(singular_pose && close(singular_pose.value().bones[0].skin_asset[0], 0.0F)
        && close(singular_pose.value().bones[0].skin_asset[5], 1.0F),
        "zero-scale bind axes use a finite affine pseudoinverse");
}

void test_attachment_finite_validation() {
    assets::Model model; model.bones = {bone("socket", -1)};
    const auto player = playback::Player::create(model);
    expect(static_cast<bool>(player), "attachment finite-validation player creates");
    if (!player) return;
    const auto pose = player.value().sample({});
    expect(static_cast<bool>(pose), "attachment finite-validation pose samples");
    if (!pose) return;

    playback::Pose wrong_count_pose = pose.value();
    wrong_count_pose.bones.clear();
    const auto count_mismatch = player.value().attachment(
        wrong_count_pose, "socket", playback::AttachmentSpace::model);
    expect(!count_mismatch && count_mismatch.error().code == playback::diagnostic_codes::invalid_request,
        "pose bone-count mismatch remains invalid request");

    const Matrix expected_model = playback::Player::asset_to_render_transform(pose.value().bones[0].model_asset);
    const auto model_attachment = player.value().attachment(
        pose.value(), "socket", playback::AttachmentSpace::model);
    expect(model_attachment && model_attachment.value().column_major == expected_model,
        "finite model attachment preserves its converted transform");
    Matrix world = playback::Player::identity_matrix();
    world[12] = 3.0F; world[13] = 5.0F; world[14] = 7.0F;
    const auto world_attachment = player.value().attachment(
        pose.value(), "socket", playback::AttachmentSpace::world, world);
    expect(world_attachment && world_attachment.value().column_major
        == playback::Player::multiply(world, expected_model),
        "finite world attachment prepends the render-world transform");
    Matrix nonfinite_world = world;
    nonfinite_world[12] = std::numeric_limits<float>::infinity();
    const auto invalid_world_attachment = player.value().attachment(
        pose.value(), "socket", playback::AttachmentSpace::world, nonfinite_world);
    expect(!invalid_world_attachment
        && invalid_world_attachment.error().code == playback::diagnostic_codes::invalid_request,
        "non-finite world matrix remains invalid request");

    playback::Pose nonfinite_pose = pose.value();
    nonfinite_pose.bones[0].model_asset[0] = std::numeric_limits<float>::quiet_NaN();
    const auto nan_attachment = player.value().attachment(
        nonfinite_pose, "socket", playback::AttachmentSpace::model);
    expect(!nan_attachment && nan_attachment.error().code == playback::diagnostic_codes::invalid_request,
        "NaN selected-bone model transform is rejected as invalid request");

    nonfinite_pose = pose.value();
    nonfinite_pose.bones[0].model_asset[5] = std::numeric_limits<float>::infinity();
    const auto infinity_attachment = player.value().attachment(
        nonfinite_pose, "socket", playback::AttachmentSpace::world, world);
    expect(!infinity_attachment
        && infinity_attachment.error().code == playback::diagnostic_codes::invalid_request,
        "infinite selected-bone model transform is rejected as invalid request");

    playback::Pose large_finite_pose = pose.value();
    large_finite_pose.bones[0].model_asset[0] = std::numeric_limits<float>::max();
    const auto large_model_attachment = player.value().attachment(
        large_finite_pose, "socket", playback::AttachmentSpace::model);
    expect(static_cast<bool>(large_model_attachment),
        "large finite selected-bone transform remains valid in model space");
    Matrix overflowing_world = playback::Player::identity_matrix();
    overflowing_world[0] = 2.0F;
    const auto overflow_attachment = player.value().attachment(
        large_finite_pose, "socket", playback::AttachmentSpace::world, overflowing_world);
    expect(!overflow_attachment
        && overflow_attachment.error().code == playback::diagnostic_codes::invalid_request,
        "finite operands that overflow during world multiplication are rejected");
}

void test_exact_bind_fade_endpoint() {
    const Matrix root_bind{
        0.0F, -2.0F, 0.0F, 0.0F,
        -3.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 4.0F, 0.0F,
        5.0F, 6.0F, 7.0F, 1.0F};
    const Matrix child_bind{
        0.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 2.0F, 0.0F, 0.0F,
        -3.0F, 0.0F, 0.0F, 0.0F,
        2.0F, 1.0F, -4.0F, 1.0F};
    assets::Model model;
    model.bones = {bone_with_transform("root", -1, root_bind, true),
        bone_with_transform("hand", 0, child_bind, false)};
    const assets::Animation animation = animation_data();
    const auto player = playback::Player::create(model, &animation);
    expect(static_cast<bool>(player), "exact bind endpoint player creates");
    if (!player) return;

    const auto pose = player.value().sample({1.0F, playback::PlaybackMode::clamp, 1.0F});
    expect(static_cast<bool>(pose), "exact bind endpoint samples");
    if (!pose) return;
    expect(close(pose.value().sampled_time_seconds, 1.0F), "bind endpoint retains clamped sample time");

    const Matrix child_model = playback::Player::multiply(root_bind, child_bind);
    expect(pose.value().bones[0].local_asset == root_bind,
        "mirrored, rotated nonuniform root local matrix is preserved exactly");
    expect(pose.value().bones[0].model_asset == root_bind,
        "mirrored, rotated nonuniform root model matrix is preserved exactly");
    expect_matrix_close(pose.value().bones[0].skin_asset, playback::Player::identity_matrix(),
        "mirrored root bind skin matrix is initialized");
    expect(pose.value().bones[1].local_asset == child_bind,
        "untracked collapsed child local matrix is preserved exactly");
    expect(pose.value().bones[1].model_asset == child_model,
        "untracked child model matrix follows exact parent and child bind matrices");
    Matrix collapsed_skin = playback::Player::identity_matrix();
    collapsed_skin[10] = 0.0F;
    collapsed_skin[14] = -9.0F;
    expect_matrix_close(pose.value().bones[1].skin_asset, collapsed_skin,
        "collapsed child bind skin matrix is initialized with its collapsed axis");
    expect(pose.value().bones[0].visible,
        "tracked root uses original bind visibility at full blend despite animated visibility");
    expect(!pose.value().bones[1].visible,
        "untracked child retains original bind visibility at full blend");

    const Matrix expected_attachment = playback::Player::asset_to_render_transform(child_model);
    const auto model_attachment = player.value().attachment(pose.value(), "hand", playback::AttachmentSpace::model);
    expect(model_attachment && model_attachment.value().column_major == expected_attachment,
        "model attachment uses the exact bind model matrix");
    Matrix render_world = playback::Player::identity_matrix();
    render_world[12] = 11.0F; render_world[13] = 13.0F; render_world[14] = 17.0F;
    const auto world_attachment = player.value().attachment(
        pose.value(), "hand", playback::AttachmentSpace::world, render_world);
    expect(world_attachment && world_attachment.value().column_major
        == playback::Player::multiply(render_world, expected_attachment),
        "world attachment prepends render world to the exact bind transform");

    const auto looped = player.value().sample({2.5F, playback::PlaybackMode::loop, 1.0F});
    expect(looped && close(looped.value().sampled_time_seconds, 0.5F),
        "bind endpoint retains looped sample time");
}

// P1-03 intermediate bind blend: the supported domain is a proper TRS bind on
// every tracked bone and a positive animated scale. Expected matrices are
// closed-form literals, not recomputed by the player's own compose().
constexpr std::string_view unsupported_bind_blend = "EAWR-ANIMATION-0005";

assets::Animation constant_root_animation(const assets::Vec3f translation, const assets::Vec3f scale) {
    assets::Animation value = animation_data();
    for (auto& sample : value.tracks.front().samples) {
        sample.translation = translation; sample.scale = scale;
        sample.rotation = {0.0F, 0.0F, 0.0F, 1.0F}; sample.visible = true;
    }
    return value;
}
Matrix trs_matrix(const float x0, const float x1, const float y0, const float y1, const float z2,
    const float tx, const float ty, const float tz) {
    return {x0, x1, 0.0F, 0.0F, y0, y1, 0.0F, 0.0F, 0.0F, 0.0F, z2, 0.0F, tx, ty, tz, 1.0F};
}

void test_supported_intermediate_bind_blend() {
    // Bind: translation (4, -2, 6), rotation 90 degrees about +Z, scale (2, 3, 0.5).
    const Matrix root_bind = trs_matrix(0.0F, 2.0F, -3.0F, 0.0F, 0.5F, 4.0F, -2.0F, 6.0F);
    // The untracked child has a collapsed Z axis; the contract leaves it alone.
    const Matrix child_bind{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F};
    assets::Model model;
    model.bones = {bone_with_transform("root", -1, root_bind, true),
        bone_with_transform("hand", 0, child_bind, true)};
    // Animated: translation (8, 0, -4), identity rotation, scale (1, 2, 1).
    const assets::Animation animation = constant_root_animation({8.0F, 0.0F, -4.0F}, {1.0F, 2.0F, 1.0F});
    const auto player = playback::Player::create(model, &animation);
    expect(static_cast<bool>(player), "supported intermediate blend player creates");
    if (!player) return;

    // blend t: T = lerp, S = lerp, R = Rz(90 t). Columns are S.x (cos, sin, 0),
    // S.y (-sin, cos, 0), S.z (0, 0, 1); the child translation is T + column 0.
    struct Expected final { float blend; Matrix root; Matrix hand_model; };
    const Expected cases[] = {
        {0.25F, trs_matrix(1.1548494F, 0.4783543F, -0.8610377F, 2.0787289F, 0.875F, 7.0F, -0.5F, -1.5F),
            {1.1548494F, 0.4783543F, 0.0F, 0.0F, -0.8610377F, 2.0787289F, 0.0F, 0.0F,
             0.0F, 0.0F, 0.0F, 0.0F, 8.1548494F, -0.0216457F, -1.5F, 1.0F}},
        {0.75F, trs_matrix(0.6696960F, 1.6167892F, -2.5406687F, 1.0523794F, 0.625F, 5.0F, -1.5F, 3.5F),
            {0.6696960F, 1.6167892F, 0.0F, 0.0F, -2.5406687F, 1.0523794F, 0.0F, 0.0F,
             0.0F, 0.0F, 0.0F, 0.0F, 5.6696960F, 0.1167892F, 3.5F, 1.0F}},
    };
    for (const Expected& item : cases) {
        const std::string name = "supported blend " + std::to_string(item.blend);
        const auto pose = player.value().sample({0.5F, playback::PlaybackMode::clamp, item.blend});
        expect(static_cast<bool>(pose), name + ": samples");
        if (!pose) continue;
        expect_matrix_close(pose.value().bones[0].local_asset, item.root, name + ": root local matches closed form");
        expect_matrix_close(pose.value().bones[0].model_asset, item.root, name + ": root model matches closed form");
        expect(pose.value().bones[1].local_asset == child_bind, name + ": untracked collapsed child keeps its exact bind");
        expect_matrix_close(pose.value().bones[1].model_asset, item.hand_model, name + ": child model matches closed form");
        const auto attachment = player.value().attachment(pose.value(), "hand", playback::AttachmentSpace::model);
        expect(attachment && attachment.value().column_major
            == playback::Player::asset_to_render_transform(pose.value().bones[1].model_asset),
            name + ": attachment converts the blended child model");
    }

    const auto zero = player.value().sample({0.5F, playback::PlaybackMode::clamp, 0.0F});
    expect(zero && zero.value().bones[0].local_asset
        == trs_matrix(1.0F, 0.0F, 0.0F, 2.0F, 1.0F, 8.0F, 0.0F, -4.0F),
        "supported blend 0 is the exact animated local");
    const auto one = player.value().sample({0.5F, playback::PlaybackMode::clamp, 1.0F});
    expect(one && one.value().bones[0].local_asset == root_bind, "supported blend 1 is the exact bind local");

    // A binary32-rounded 30 degree bind rotation is within the shear tolerance.
    const float c = 0.8660254F, s = 0.5F;
    assets::Model rounded; rounded.bones = {bone_with_transform("root", -1,
        trs_matrix(c, s, -s, c, 1.0F, 0.0F, 0.0F, 0.0F), true)};
    const auto rounded_player = playback::Player::create(rounded, &animation);
    const auto rounded_pose = rounded_player
        ? rounded_player.value().sample({0.5F, playback::PlaybackMode::clamp, 0.5F})
        : eawr::core::Result<playback::Pose>::failure(rounded_player.error());
    expect(static_cast<bool>(rounded_pose), "binary32-rounded proper bind rotation is supported");
}

void test_unsupported_intermediate_bind_blend() {
    const Matrix identity = playback::Player::identity_matrix();
    Matrix mirrored = identity; mirrored[0] = -1.0F; mirrored[12] = 1.0F; mirrored[13] = 2.0F; mirrored[14] = 3.0F;
    const Matrix mirrored_rotated{0.0F, -2.0F, 0.0F, 0.0F, -3.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 4.0F, 0.0F, 5.0F, 6.0F, 7.0F, 1.0F};
    Matrix sheared = identity; sheared[4] = 0.5F;
    Matrix collapsed = identity; collapsed[0] = 0.0F;
    Matrix overflowing = identity; overflowing[0] = 3.0e38F;
    Matrix distant = identity; distant[12] = 3.0e38F;
    struct Fixture final { std::string_view name; Matrix bind; assets::Vec3f translation; assets::Vec3f scale; std::string_view rule; };
    const Fixture fixtures[] = {
        {"mirrored bind", mirrored, {0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, "mirrored"},
        {"mirrored rotated nonuniform bind", mirrored_rotated, {0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, "mirrored"},
        {"sheared bind", sheared, {0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, "sheared"},
        {"collapsed bind", collapsed, {0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, "collapsed"},
        {"overflowing bind", overflowing, {0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, "non-finite"},
        {"mirrored animated scale", identity, {0.0F, 0.0F, 0.0F}, {-1.0F, 1.0F, 1.0F}, "animated scale"},
        {"collapsed animated scale", identity, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 1.0F}, "animated scale"},
        {"overflowing blended translation", distant, {-3.0e38F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, "non-finite"},
    };
    for (const Fixture& fixture : fixtures) {
        const std::string name(fixture.name);
        assets::Model model; model.bones = {bone_with_transform("root", -1, fixture.bind, true)};
        const assets::Animation animation = constant_root_animation(fixture.translation, fixture.scale);
        const auto player = playback::Player::create(model, &animation);
        expect(static_cast<bool>(player), name + ": player still creates");
        if (!player) continue;
        const auto zero = player.value().sample({0.5F, playback::PlaybackMode::clamp, 0.0F});
        expect(static_cast<bool>(zero), name + ": blend 0 is unaffected");
        const auto one = player.value().sample({0.5F, playback::PlaybackMode::clamp, 1.0F});
        expect(one && one.value().bones[0].local_asset == fixture.bind, name + ": blend 1 keeps the exact bind");
        expect(static_cast<bool>(player.value().sample_tick(1U, 2U)), name + ": sample_tick is unaffected");
        for (const float blend : {0.25F, 0.5F}) {
            const auto pose = player.value().sample({0.5F, playback::PlaybackMode::clamp, blend});
            const std::string label = name + " at blend " + std::to_string(blend);
            expect(!pose, label + ": intermediate blend fails closed");
            if (pose) continue;
            expect(pose.error().code == unsupported_bind_blend, label + ": uses the unsupported bind-blend code");
            expect(pose.error().message.find("'root'") != std::string::npos, label + ": names the bone");
            expect(pose.error().message.find(fixture.rule) != std::string::npos, label + ": names the failed rule");
        }
    }
}

bool rejects_origin(const playback::Player& player, const playback::Pose& pose, const std::string_view name) {
    const auto result = player.attachment(pose, name, playback::AttachmentSpace::model);
    return !result && result.error().code == playback::diagnostic_codes::invalid_request;
}
bool accepts_origin(const playback::Player& player, const playback::Pose& pose, const std::string_view name) {
    return static_cast<bool>(player.attachment(pose, name, playback::AttachmentSpace::model));
}

void test_attachment_pose_origin() {
    assets::Model model; model.bones = {bone("root", -1), bone("hand", 0, 2.0F)};
    const assets::Animation animation = animation_data();
    auto owner_result = playback::Player::create(model, &animation);
    auto twin_result = playback::Player::create(model, &animation);
    auto clipless_result = playback::Player::create(model);
    auto clipless_twin_result = playback::Player::create(model);
    expect(owner_result && twin_result && clipless_result && clipless_twin_result, "pose-origin players create");
    if (!owner_result || !twin_result || !clipless_result || !clipless_twin_result) return;
    const playback::Player& owner = owner_result.value();
    const playback::Player& twin = twin_result.value();
    const playback::Player& clipless = clipless_result.value();
    const playback::Player& clipless_twin = clipless_twin_result.value();

    // Every sampling route stamps the creating player's origin; an
    // independently created player of the same model rejects it.
    struct Route final { std::string_view name; eawr::core::Result<playback::Pose> pose; const playback::Player* own; const playback::Player* other; };
    Route routes[] = {
        {"sample interpolated", owner.sample({0.5F, playback::PlaybackMode::loop, 0.0F}), &owner, &twin},
        {"sample blend-to-bind endpoint", owner.sample({1.0F, playback::PlaybackMode::clamp, 1.0F}), &owner, &twin},
        {"sample_tick clip", owner.sample_tick(3U, 2U), &owner, &twin},
        {"sample clipless bind", clipless.sample({}), &clipless, &clipless_twin},
        {"sample_tick clipless bind", clipless.sample_tick(7U, 30U), &clipless, &clipless_twin},
    };
    for (const Route& route : routes) {
        const std::string name(route.name);
        expect(static_cast<bool>(route.pose), name + ": pose samples");
        if (!route.pose) continue;
        expect(accepts_origin(*route.own, route.pose.value(), "hand"), name + ": creating player accepts its pose");
        expect(rejects_origin(*route.other, route.pose.value(), "hand"), name + ": identical independent player rejects the pose");
    }

    const auto pose = owner.sample({0.5F, playback::PlaybackMode::loop, 0.0F});
    if (!pose) { expect(false, "pose-origin reference pose samples"); return; }

    const playback::Pose copied = pose.value();
    expect(accepts_origin(owner, copied, "hand"), "copied pose retains its origin");
    expect(rejects_origin(twin, copied, "hand"), "copied pose remains foreign to an identical twin");
    playback::Pose assigned; assigned = copied;
    expect(accepts_origin(owner, assigned, "hand"), "copy-assigned pose retains its origin");
    expect(copied.bones[1].model_asset == pose.value().bones[1].model_asset, "copied pose matrices remain public values");

    playback::Pose fabricated; fabricated.bones.resize(model.bones.size());
    for (auto& entry : fabricated.bones) entry.model_asset = playback::Player::identity_matrix();
    expect(rejects_origin(owner, fabricated, "hand"), "fabricated equal-size pose is rejected");
    expect(rejects_origin(clipless, fabricated, "hand"), "fabricated equal-size pose is rejected by a clipless player");
    playback::Pose defaulted;
    expect(rejects_origin(owner, defaulted, "hand"), "default pose is rejected");
    playback::Pose resized_default; resized_default.bones.resize(model.bones.size());
    expect(rejects_origin(owner, resized_default, "hand"), "default pose resized to the bone count is rejected");

    assets::Model reversed; reversed.bones = {bone("hand", -1), bone("root", 0, 2.0F)};
    const auto reversed_player = playback::Player::create(reversed);
    const auto reversed_pose = reversed_player ? reversed_player.value().sample({})
        : eawr::core::Result<playback::Pose>::failure(reversed_player.error());
    expect(static_cast<bool>(reversed_pose), "reversed-name skeleton samples");
    if (reversed_pose) {
        expect(accepts_origin(reversed_player.value(), reversed_pose.value(), "hand"), "reversed skeleton accepts its own pose");
        expect(rejects_origin(owner, reversed_pose.value(), "hand"), "same-size reversed skeleton pose is rejected");
        expect(rejects_origin(clipless, reversed_pose.value(), "hand"), "same-size reversed skeleton pose is rejected by a bind player");
    }

    // Move construction transfers the origin: the destination accepts the
    // source's earlier poses and the moved-from player no longer does.
    auto source_result = playback::Player::create(model, &animation);
    if (!source_result) { expect(false, "move-construction source creates"); return; }
    const auto source_pose = source_result.value().sample({0.5F, playback::PlaybackMode::loop, 0.0F});
    if (!source_pose) { expect(false, "move-construction source pose samples"); return; }
    playback::Player moved(std::move(source_result.value()));
    expect(accepts_origin(moved, source_pose.value(), "hand"), "move construction transfers the origin");
    expect(rejects_origin(twin, source_pose.value(), "hand"), "moved origin remains foreign to a twin");
    expect(rejects_origin(source_result.value(), source_pose.value(), "hand"), "moved-from player no longer accepts the pose");
    const auto moved_pose = moved.sample({0.5F, playback::PlaybackMode::loop, 0.0F});
    expect(moved_pose && accepts_origin(moved, moved_pose.value(), "hand")
        && rejects_origin(twin, moved_pose.value(), "hand"), "moved player stamps the transferred origin");

    // Move assignment replaces the destination origin with the source origin.
    auto target_result = playback::Player::create(model, &animation);
    auto assigned_source_result = playback::Player::create(model, &animation);
    if (!target_result || !assigned_source_result) { expect(false, "move-assignment players create"); return; }
    playback::Player& target = target_result.value();
    const auto old_target_pose = target.sample({0.5F, playback::PlaybackMode::loop, 0.0F});
    const auto assigned_source_pose = assigned_source_result.value().sample({1.0F, playback::PlaybackMode::clamp, 1.0F});
    if (!old_target_pose || !assigned_source_pose) { expect(false, "move-assignment poses sample"); return; }
    expect(accepts_origin(target, old_target_pose.value(), "hand"), "target accepts its own pose before assignment");
    target = std::move(assigned_source_result.value());
    expect(accepts_origin(target, assigned_source_pose.value(), "hand"), "move assignment transfers the source origin");
    expect(rejects_origin(target, old_target_pose.value(), "hand"), "move assignment retires the destination's old origin");

    // Returned values own their data after the Player is destroyed.
    playback::AttachmentTransform retained;
    playback::Pose retained_pose;
    Matrix expected{};
    {
        auto scoped = playback::Player::create(model, &animation);
        if (!scoped) { expect(false, "scoped player creates"); return; }
        const auto scoped_pose = scoped.value().sample({0.5F, playback::PlaybackMode::loop, 0.0F});
        if (!scoped_pose) { expect(false, "scoped pose samples"); return; }
        retained_pose = scoped_pose.value();
        expected = playback::Player::asset_to_render_transform(scoped_pose.value().bones[1].model_asset);
        const auto scoped_attachment = scoped.value().attachment(scoped_pose.value(), "hand", playback::AttachmentSpace::model);
        if (!scoped_attachment) { expect(false, "scoped attachment resolves"); return; }
        retained = scoped_attachment.value();
    }
    expect(retained.space == playback::AttachmentSpace::model && retained.column_major == expected,
        "attachment transform is retained after player destruction");
    expect(close(retained_pose.bones[1].model_asset[12], 5.0F), "pose matrices remain readable after player destruction");
    expect(rejects_origin(owner, retained_pose, "hand"), "pose from a destroyed player is foreign to a live player");
}

// A rigid ALO mesh stores its vertices in its bone's space; the palette maps
// bind model space. Moving the vertex to bind space first must place it by its
// bone's bind transform at rest and by the animated transform under a clip.
void test_rigid_vertex_to_bind() {
    // Bind = T(5,0,0) * Rz(90) * S(2,1,1): columns (0,2,0), (-1,0,0), (0,0,1).
    const Matrix bind{0.0F, 2.0F, 0.0F, 0.0F, -1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F, 5.0F, 0.0F, 0.0F, 1.0F};
    assets::Vertex vertex;
    vertex.position = {1.0F, 1.0F, 1.0F};
    vertex.normal = {1.0F, 1.0F, 0.0F};
    vertex.tangent = {1.0F, -1.0F, 0.0F};
    vertex.binormal = {0.0F, 0.0F, 1.0F};
    vertex.texcoord[0] = {0.25F, 0.75F};
    const assets::Vertex placed = playback::Player::rigid_vertex_to_bind(bind, vertex);
    expect(close(placed.position.x, 4.0F) && close(placed.position.y, 2.0F) && close(placed.position.z, 1.0F),
        "rigid vertex position takes the full bind transform");
    expect(close(placed.tangent.x, 1.0F) && close(placed.tangent.y, 2.0F) && close(placed.tangent.z, 0.0F),
        "rigid tangent takes the linear part only");
    const auto dot = [](const assets::Vec3f a, const assets::Vec3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    expect(close(dot(placed.normal, placed.tangent), 0.0F) && close(dot(placed.normal, placed.binormal), 0.0F),
        "rigid normal stays perpendicular to the transformed surface under non-uniform scale");
    expect(dot(placed.normal, {-1.0F, 2.0F, 0.0F}) > 0.0F, "rigid normal keeps its outward side");
    expect(close(placed.texcoord[0].x, 0.25F) && close(placed.texcoord[0].y, 0.75F), "non-geometric attributes pass through");

    const Matrix mirror{-1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    assets::Vertex facing;
    facing.normal = {1.0F, 0.0F, 0.0F};
    const assets::Vertex mirrored = playback::Player::rigid_vertex_to_bind(mirror, facing);
    expect(close(mirrored.normal.x, -1.0F) && close(mirrored.normal.y, 0.0F), "a mirrored bind keeps the normal outward");

    // Through the player: the hand bone binds at x = 2 and the clip moves the
    // root, so a rigid vertex near the hand's origin must stay on the hand.
    assets::Model model; model.bones = {bone("root", -1), bone("hand", 0, 2.0F)};
    const assets::Animation animation = animation_data();
    const auto player = playback::Player::create(model, &animation);
    expect(static_cast<bool>(player), "rigid placement player creates");
    if (!player) return;
    const auto rest = player.value().sample({});
    const auto moved = player.value().sample({0.5F, playback::PlaybackMode::loop, 0.0F});
    expect(rest && moved, "rigid placement poses sample");
    if (!rest || !moved) return;
    assets::Vertex local;
    local.position = {0.5F, 0.0F, 0.0F};
    const assets::Vertex bind_space = playback::Player::rigid_vertex_to_bind(rest.value().bones[1].model_asset, local);
    expect(close(bind_space.position.x, 2.5F), "rest: a rigid vertex draws at its bone, not at the model origin");
    const auto apply = [](const Matrix& m, const assets::Vec3f p) {
        return assets::Vec3f{m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                             m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
    };
    const assets::Vec3f through_palette = apply(moved.value().bones[1].skin_asset, bind_space.position);
    const assets::Vec3f through_bone = apply(moved.value().bones[1].model_asset, local.position);
    expect(close(through_palette.x, through_bone.x) && close(through_palette.y, through_bone.y)
        && close(through_palette.z, through_bone.z), "animated: the palette places a bind-space rigid vertex on the animated bone");
}

void test_rigid_instance_routes() {
    assets::Model model;
    model.bones = {bone("root", -1), bone("attached", 0, 5.0F)};
    assets::Mesh rigid;
    rigid.bone = 1;
    assets::Vertex local;
    local.position = {2.0F, 0.0F, 0.0F};
    rigid.submeshes.push_back({});
    rigid.submeshes.front().vertices.push_back(local);
    model.meshes.push_back(rigid);
    const auto rest = playback::Player::create(model).value().sample({});
    const float instance_x = 10.0F;
    const float rigid_draw_x = instance_x + playback::Player::rigid_vertex_to_bind(
        rest.value().bones[1].model_asset, local).position.x;
    expect(close(rigid_draw_x, 17.0F),
        "bone-space rigid mesh takes its non-identity bind once through a map instance");

    // A skin-mapped vertex is already in bind-model space. At rest its skin
    // matrix is identity, even when the same non-identity bone owns the mesh.
    model.meshes.front().submeshes.front().skin_bones = {1};
    assets::Vertex bind_model;
    bind_model.position = {7.0F, 0.0F, 0.0F};
    const float skinned_draw_x = instance_x + bind_model.position.x *
        rest.value().bones[1].skin_asset[0] + rest.value().bones[1].skin_asset[12];
    expect(close(skinned_draw_x, 17.0F),
        "model-space skinned mesh takes no extra bind through a map instance");
}
void test_sample_position() {
    assets::Model model; model.bones = {bone("root", -1), bone("hand", 0, 2.0F)};
    const assets::Animation animation = animation_data();
    const auto player = playback::Player::create(model, &animation);
    expect(static_cast<bool>(player), "position player creates");
    if (!player) return;
    const playback::Player& clip = player.value();
    expect(clip.playable_frames() == 2U && clip.frames_per_second() == 1.0F, "clip length and rate are exposed");
    const auto x_at = [&](const std::uint64_t position, const std::uint32_t subdivisions) -> std::optional<float> {
        const auto pose = clip.sample_position(position, subdivisions);
        if (!pose) return std::nullopt;
        return pose.value().bones[0].model_asset[12];
    };
    expect(x_at(0U, 4U) && close(*x_at(0U, 4U), 0.0F), "position 0 is frame 0");
    expect(x_at(2U, 4U) && close(*x_at(2U, 4U), 5.0F), "a quarter-frame subdivision interpolates");
    expect(x_at(4U, 4U) && close(*x_at(4U, 4U), 10.0F), "position 4/4 is frame 1");
    expect(x_at(8U, 4U) && close(*x_at(8U, 4U), 0.0F), "the end position is the last stored frame");
    expect(!clip.sample_position(9U, 4U), "a position past the end fails");
    expect(!clip.sample_position(1U, 0U), "zero subdivisions fail");
    const auto sampled = clip.sample_position(2U, 4U);
    expect(sampled && clip.sampled(sampled.value()) && close(sampled.value().sampled_time_seconds, 0.5F),
           "a position pose carries this player's origin and its clip time");
    bool matches = true;
    for (std::uint64_t tick = 0; tick < 130U; ++tick) {
        const auto by_tick = clip.sample_tick(tick, 30U);
        const auto by_position = x_at(tick % 60U, 30U);
        matches = matches && by_tick && by_position && close(by_tick.value().bones[0].model_asset[12], *by_position);
    }
    expect(matches, "a reduced tick position samples exactly as sample_tick");
    const auto bind_only = playback::Player::create(model);
    expect(bind_only && bind_only.value().sample_position(0U, 30U), "a clipless player samples its bind pose");
}
} // namespace

int main() {
    test_sample_position();
    test_rigid_vertex_to_bind();
    test_rigid_instance_routes();
    test_bind_identity_and_missing_tracks();
    test_rotation_interpolation_hierarchy_loop_and_attachments();
    test_validation_and_blend();
    test_attachment_finite_validation();
    test_exact_bind_fade_endpoint();
    test_supported_intermediate_bind_blend();
    test_unsupported_intermediate_bind_blend();
    test_attachment_pose_origin();
    if (failures != 0) return EXIT_FAILURE;
    std::cout << "animation CPU contracts passed\n";
    return EXIT_SUCCESS;
}
