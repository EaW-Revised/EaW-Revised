// Synthetic controls for the CPU vertex-deformation probe.  Every expected
// position below is derived by hand from the bone transforms, not from the
// probe or the Player.

#include "deformation_probe.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace eawr;
namespace playback = eawr::presentation::animation;
namespace probe = eawr::tests::animation_deformation;
using assets::Vec3f;
using assets::Vec4f;

int failures{};
void expect(const bool condition, const std::string_view message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; ++failures; }
}
bool close(const float left, const float right, const float tolerance = 1.0e-5F) {
    return std::abs(left - right) <= tolerance;
}
bool close(const Vec3f left, const Vec3f right, const float tolerance = 1.0e-5F) {
    return close(left.x, right.x, tolerance) && close(left.y, right.y, tolerance) && close(left.z, right.z, tolerance);
}
bool at(const probe::Deformed& deformed, const std::uint32_t index, const Vec3f expected) {
    for (const auto& vertex : deformed.vertices)
        if (vertex.index == index) return vertex.position && close(*vertex.position, expected);
    return false;
}
bool fails_with(const probe::Checked<probe::Deformed>& result, const std::string_view needle) {
    return !result && result.error.find(needle) != std::string::npos;
}

constexpr float half_root_two = 0.70710678F;
const Vec4f z_quarter_turn{0.0F, 0.0F, half_root_two, half_root_two};  // +90 degrees about +Z

assets::Bone bone(const std::string_view name, const std::int32_t parent, const Vec3f translation = {}) {
    assets::Bone value;
    value.name = name;
    value.parent = parent;
    value.relative_transform = {1.0F, 0.0F, 0.0F, translation.x,
        0.0F, 1.0F, 0.0F, translation.y,
        0.0F, 0.0F, 1.0F, translation.z};
    return value;
}

struct Key final { Vec3f translation; Vec4f rotation{0.0F, 0.0F, 0.0F, 1.0F}; };

// Three stored frames, two playable, one frame per second: duration 2 s.
assets::AnimationTrack track(const std::uint32_t index, const std::string_view name, const std::vector<Key>& keys) {
    assets::AnimationTrack value;
    value.bone_index = index;
    value.bone_name = name;
    for (const Key& key : keys) value.samples.push_back({key.translation, {1.0F, 1.0F, 1.0F}, key.rotation, true});
    return value;
}
assets::Animation clip(std::vector<assets::AnimationTrack> tracks) {
    assets::Animation value;
    value.source.logical_path = "synthetic.ala";
    value.stored_frame_count = 3;
    value.playable_frame_count = 2;
    value.frames_per_second = 1.0F;
    value.duration_seconds = 2.0F;
    value.tracks = std::move(tracks);
    return value;
}

assets::Vertex vertex(const Vec3f position, const std::array<std::uint32_t, 4> indices = {},
    const std::array<float, 4> weights = {}) {
    assets::Vertex value;
    value.position = position;
    value.bone_indices = indices;
    value.bone_weights = weights;
    return value;
}
assets::Submesh submesh(std::vector<assets::Vertex> vertices, std::vector<std::uint16_t> indices,
    std::vector<std::uint32_t> skin_bones = {}) {
    assets::Submesh value;
    value.shader = "Synthetic.fx";
    value.vertices = std::move(vertices);
    value.indices = std::move(indices);
    value.skin_bones = std::move(skin_bones);
    return value;
}
assets::Mesh mesh(const std::string_view name, const std::int32_t bone_index, assets::Submesh part) {
    assets::Mesh value;
    value.name = name;
    value.bone = bone_index;
    value.submeshes.push_back(std::move(part));
    return value;
}

playback::Pose bind_pose(const assets::Model& model) {
    const auto player = playback::Player::create(model);
    if (!player) { expect(false, "bind player creates"); return {}; }
    const auto pose = player.value().sample({0.0F, playback::PlaybackMode::clamp, 0.0F});
    if (!pose) { expect(false, "bind pose samples"); return {}; }
    return pose.value();
}
playback::Pose animated_pose(const assets::Model& model, const assets::Animation& animation, const float time,
    const playback::PlaybackMode mode = playback::PlaybackMode::loop) {
    const auto player = playback::Player::create(model, &animation);
    if (!player) { expect(false, "animated player creates"); return {}; }
    const auto pose = player.value().sample({time, mode, 0.0F});
    if (!pose) { expect(false, "animated pose samples"); return {}; }
    return pose.value();
}

// Byte fingerprint of every field the probe reads, for the unchanged-input check.
template <class T> void append(std::string& out, const T& value) {
    char bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    out.append(bytes, sizeof(T));
}
std::string fingerprint(const assets::Model& model) {
    std::string out;
    for (const auto& b : model.bones) { out += b.name; append(out, b.parent); append(out, b.relative_transform); }
    for (const auto& m : model.meshes) {
        out += m.name; append(out, m.bone); append(out, m.visible);
        for (const auto& s : m.submeshes) {
            for (const auto& v : s.vertices) { append(out, v.position); append(out, v.bone_indices); append(out, v.bone_weights); }
            for (const auto i : s.indices) append(out, i);
            for (const auto b : s.skin_bones) append(out, b);
        }
    }
    return out;
}
std::string fingerprint(const assets::Animation& animation) {
    std::string out;
    append(out, animation.duration_seconds);
    for (const auto& t : animation.tracks) {
        out += t.bone_name; append(out, t.bone_index);
        for (const auto& s : t.samples) { append(out, s.translation); append(out, s.scale); append(out, s.rotation); }
    }
    return out;
}

// --- Controls ---------------------------------------------------------------

// Bind translation: root bound at (2,0,0), animated to (5,0,0) at t = 1 s.  A
// rigid vertex is stored in its bone's space, so it draws at the bone's
// transform: bind (2,1,0) -> (4,1,0), animated -> (7,1,0).
void test_bind_translation() {
    assets::Model model;
    model.bones = {bone("root", -1, {2.0F, 0.0F, 0.0F})};
    model.meshes.push_back(mesh("Rigid", 0, submesh({vertex({2.0F, 1.0F, 0.0F})}, {0, 0, 0})));
    const auto animation = clip({track(0, "root", {{{2.0F, 0.0F, 0.0F}}, {{5.0F, 0.0F, 0.0F}}, {{2.0F, 0.0F, 0.0F}}})});
    const auto bind = probe::deform(model, {"Rigid", 0}, bind_pose(model));
    const auto moved = probe::deform(model, {"Rigid", 0}, animated_pose(model, animation, 1.0F));
    expect(bind && at(*bind.value, 0, {4.0F, 1.0F, 0.0F}), "bind translation: bind pose places the vertex at its bone");
    expect(moved && at(*moved.value, 0, {7.0F, 1.0F, 0.0F}), "bind translation: vertex follows the animated bone");
    expect(moved && moved.value->route == probe::Route::rigid, "bind translation: rigid route");
}

// Rotated, translated child: root bound at the origin, child bound 2 up Y.
// At t = 1 s the root is at (1,0,0) turned +90 about Z; the untracked child
// follows.  Child skin = T(1,0,0) Rz90, so (1,2,0) -> (-2,1,0) + (1,0,0).
void test_rotated_translated_child() {
    assets::Model model;
    model.bones = {bone("root", -1), bone("child", 0, {0.0F, 2.0F, 0.0F})};
    model.meshes.push_back(mesh("Skin", -1, submesh({vertex({1.0F, 2.0F, 0.0F}, {0, 0, 0, 0}, {1.0F, 0.0F, 0.0F, 0.0F})},
        {0, 0, 0}, {1})));
    const auto animation = clip({track(0, "root", {{{0.0F, 0.0F, 0.0F}}, {{1.0F, 0.0F, 0.0F}, z_quarter_turn},
        {{0.0F, 0.0F, 0.0F}}})});
    const auto pose = animated_pose(model, animation, 1.0F);
    expect(pose.bones.size() == 2 && close(pose.bones[1].model_asset[12], -1.0F) && close(pose.bones[1].model_asset[13], 0.0F),
        "rotated child: child origin follows the rotated, translated parent to (-1,0,0)");
    const auto moved = probe::deform(model, {"Skin", 0}, pose);
    expect(moved && at(*moved.value, 0, {-1.0F, 1.0F, 0.0F}), "rotated child: palette vertex (1,2,0) -> (-1,1,0)");
    const auto bind = probe::deform(model, {"Skin", 0}, bind_pose(model));
    expect(bind && at(*bind.value, 0, {1.0F, 2.0F, 0.0F}), "rotated child: bind pose leaves the vertex at its source");
}

// Non-identity local palette with asymmetric four-bone weights.  Bones 0-3 are
// animated to pure translations; bone 4 is a far decoy.  skin_bones maps local
// {0,1,2,3,4} -> global {3,1,4,0,2}.  The vertex names local {4,1,0,3}, i.e.
// global {2,1,3,0}, weights {0.1,0.2,0.3,0.4}:
//   (1,1,1) + 0.1(0,0,3) + 0.2(0,2,0) + 0.3(4,4,4) + 0.4(1,0,0) = (2.6,2.6,2.5).
// Reading local indices as global would pick the decoy.
void test_local_palette_four_influences() {
    assets::Model model;
    model.bones = {bone("b0", -1), bone("b1", -1), bone("b2", -1), bone("b3", -1), bone("decoy", -1)};
    model.meshes.push_back(mesh("Skin", -1, submesh({
        vertex({1.0F, 1.0F, 1.0F}, {4, 1, 0, 3}, {0.1F, 0.2F, 0.3F, 0.4F}),
        vertex({0.0F, 0.0F, 0.0F}, {3, 0, 0, 0}, {0.25F, 0.75F, 0.0F, 0.0F})},
        {0, 1, 1}, {3, 1, 4, 0, 2})));
    const auto animation = clip({
        track(0, "b0", {{{}}, {{1.0F, 0.0F, 0.0F}}, {{}}}),
        track(1, "b1", {{{}}, {{0.0F, 2.0F, 0.0F}}, {{}}}),
        track(2, "b2", {{{}}, {{0.0F, 0.0F, 3.0F}}, {{}}}),
        track(3, "b3", {{{}}, {{4.0F, 4.0F, 4.0F}}, {{}}}),
        track(4, "decoy", {{{}}, {{100.0F, 100.0F, 100.0F}}, {{}}})});
    const auto moved = probe::deform(model, {"Skin", 0}, animated_pose(model, animation, 1.0F));
    expect(moved && at(*moved.value, 0, {2.6F, 2.6F, 2.5F}), "local palette: four asymmetric influences blend through skin_bones");
    // Second vertex: local {3,0} -> global {0,3}; 0.25(1,0,0) + 0.75(4,4,4) = (3.25,3,3).
    expect(moved && at(*moved.value, 1, {3.25F, 3.0F, 3.0F}), "local palette: asymmetric two-influence weights");
    expect(moved && moved.value->vertices[0].active_influences == 4
        && moved.value->vertices[0].weight_class == probe::WeightClass::unit, "local palette: four active influences, unit sum");
    expect(moved && moved.value->vertices[0].influences[0].global == 2 && moved.value->vertices[0].influences[3].global == 0,
        "local palette: global bones recorded through the palette");
}

// Rigid meshes are stored in the mesh bone's space and follow its model
// transform; unskinned meshes pass through even while every bone moves.  The
// child model is T(1,0,0) Rz90 T(0,2,0): (1,2,0) -> (1,4,0) -> (-4,1,0) -> (-3,1,0).
void test_rigid_and_unskinned() {
    assets::Model model;
    model.bones = {bone("root", -1), bone("child", 0, {0.0F, 2.0F, 0.0F})};
    model.meshes.push_back(mesh("Rigid", 1, submesh({vertex({1.0F, 2.0F, 0.0F}, {9, 9, 9, 9}, {0.3F, 0.3F, 0.3F, 0.3F})}, {0, 0, 0})));
    model.meshes.push_back(mesh("Static", -1, submesh({vertex({1.0F, 2.0F, 3.0F})}, {0, 0, 0})));
    const auto animation = clip({track(0, "root", {{{}}, {{1.0F, 0.0F, 0.0F}, z_quarter_turn}, {{}}})});
    const auto pose = animated_pose(model, animation, 1.0F);
    const auto rigid = probe::deform(model, {"Rigid", 0}, pose);
    expect(rigid && rigid.value->route == probe::Route::rigid && at(*rigid.value, 0, {-3.0F, 1.0F, 0.0F}),
        "rigid: synthesized one-bone weight on the mesh bone, stored weights ignored");
    expect(rigid && rigid.value->vertices[0].weight_class == probe::WeightClass::synthesized
        && rigid.value->vertices[0].influences[0].global == 1, "rigid: synthesized influence recorded");
    const auto still = probe::deform(model, {"Static", 0}, pose);
    expect(still && still.value->route == probe::Route::unskinned && at(*still.value, 0, {1.0F, 2.0F, 3.0F}),
        "unskinned: position passes through under an animated pose");
    const auto evaluation = probe::evaluate(model, {"Static", 0}, bind_pose(model), pose);
    expect(evaluation && evaluation.value->changed == 0 && evaluation.value->none == 1, "unskinned: no change, weight class none");
}

// An animated bone that no vertex weights moves nothing.  An inactive
// influence with an invalid local index is ignored; the same index with a
// non-zero weight is refused.
void test_zero_influence_bone_and_inactive_index() {
    assets::Model model;
    model.bones = {bone("still", -1), bone("moving", -1)};
    model.meshes.push_back(mesh("Skin", -1, submesh({vertex({1.0F, 1.0F, 1.0F}, {0, 1, 1, 7}, {1.0F, 0.0F, 0.0F, 0.0F})},
        {0, 0, 0}, {0, 1})));
    const auto animation = clip({track(1, "moving", {{{}}, {{50.0F, 0.0F, 0.0F}, z_quarter_turn}, {{}}})});
    const auto pose = animated_pose(model, animation, 1.0F);
    expect(pose.bones.size() == 2 && close(pose.bones[1].skin_asset[12], 50.0F), "zero influence: the unweighted bone really moves");
    const auto evaluation = probe::evaluate(model, {"Skin", 0}, bind_pose(model), pose);
    expect(evaluation && evaluation.value->changed == 0 && evaluation.value->predicted == 1,
        "zero influence: vertex unchanged although an animated bone is in its palette");
    expect(evaluation && evaluation.value->influencing_bones == std::vector<std::uint32_t>{0},
        "zero influence: only the weighted bone influences");
    expect(evaluation && evaluation.value->influencing_detail.size() == 1
        && evaluation.value->influencing_detail[0].name == "still"
        && evaluation.value->influencing_detail[0].animated_skin_deviation == 0.0F,
        "zero influence: the influencing bone's palette is exactly identity");
    // -0.0 is zero for the renderer's `weight != 0` test.
    model.meshes[0].submeshes[0].vertices[0].bone_weights = {1.0F, 0.0F, 0.0F, -0.0F};
    expect(static_cast<bool>(probe::deform(model, {"Skin", 0}, pose)), "zero influence: -0.0 weight with invalid index is inactive");
    model.meshes[0].submeshes[0].vertices[0].bone_weights = {0.5F, 0.0F, 0.0F, 0.5F};
    expect(fails_with(probe::deform(model, {"Skin", 0}, pose), "active local bone index"),
        "zero influence: an active weight on an invalid local index is refused");
}

// A +90 degree Z turn about the origin: (0,0,5) is on the axis and stays;
// (1,0,0) moves to (0,1,0).  Substituting the bind pose for the animated pose
// must find no change at all.
void test_rotation_axis_moving_vertex_and_bind_negative() {
    assets::Model model;
    model.bones = {bone("spinner", -1)};
    model.meshes.push_back(mesh("Skin", -1, submesh({
        vertex({0.0F, 0.0F, 5.0F}, {0, 0, 0, 0}, {1.0F, 0.0F, 0.0F, 0.0F}),
        vertex({1.0F, 0.0F, 0.0F}, {0, 0, 0, 0}, {1.0F, 0.0F, 0.0F, 0.0F})}, {0, 1, 1}, {0})));
    const auto animation = clip({track(0, "spinner", {{{}}, {{}, z_quarter_turn}, {{}}})});
    const auto bind = bind_pose(model);
    const auto pose = animated_pose(model, animation, 1.0F);
    const auto evaluation = probe::evaluate(model, {"Skin", 0}, bind, pose);
    expect(evaluation && evaluation.value->changed == 1, "rotation axis: exactly the off-axis vertex changes");
    expect(evaluation && !evaluation.value->vertices[0].changed && evaluation.value->vertices[0].displacement <= probe::change_epsilon,
        "rotation axis: the on-axis vertex is unchanged");
    expect(evaluation && evaluation.value->vertices[1].animated && close(*evaluation.value->vertices[1].animated, {0.0F, 1.0F, 0.0F}),
        "moving vertex: (1,0,0) -> (0,1,0)");
    expect(evaluation && close(evaluation.value->max_displacement, std::sqrt(2.0F))
        && evaluation.value->max_displacement_vertex == 1U, "moving vertex: displacement sqrt(2) at vertex 1");
    const auto negative = probe::evaluate(model, {"Skin", 0}, bind, bind);
    expect(negative && negative.value->changed == 0 && negative.value->max_displacement == 0.0F,
        "substituted bind: no vertex changes when the bind pose replaces the animated pose");
}

// Loop and clamp endpoints on a clip whose stored terminal frame (7,0,0)
// deliberately differs from frame 0.
void test_loop_and_clamp_endpoints() {
    assets::Model model;
    model.bones = {bone("root", -1)};
    model.meshes.push_back(mesh("Rigid", 0, submesh({vertex({0.0F, 0.0F, 0.0F})}, {0, 0, 0})));
    const auto animation = clip({track(0, "root", {{{0.0F, 0.0F, 0.0F}}, {{1.0F, 0.0F, 0.0F}}, {{7.0F, 0.0F, 0.0F}}})});
    struct Case { float time; playback::PlaybackMode mode; float x; std::string_view name; };
    for (const Case& value : {
             Case{0.0F, playback::PlaybackMode::loop, 0.0F, "loop t=0 is frame 0"},
             Case{2.0F, playback::PlaybackMode::loop, 0.0F, "loop t=duration wraps to frame 0"},
             Case{2.0F, playback::PlaybackMode::clamp, 7.0F, "clamp t=duration is the stored terminal frame"},
             Case{100.0F, playback::PlaybackMode::clamp, 7.0F, "clamp past the end holds the terminal frame"},
             Case{3.5F, playback::PlaybackMode::loop, 4.0F, "loop t=3.5 wraps to 1.5, halfway from 1 to 7"}}) {
        const auto moved = probe::deform(model, {"Rigid", 0}, animated_pose(model, animation, value.time, value.mode));
        expect(moved && at(*moved.value, 0, {value.x, 0.0F, 0.0F}), value.name);
    }
}

// Zero and non-unit weight sums get no prediction and are never normalized.
void test_nonunit_and_zero_weights() {
    assets::Model model;
    model.bones = {bone("root", -1)};
    model.meshes.push_back(mesh("Skin", -1, submesh({
        vertex({1.0F, 0.0F, 0.0F}, {0, 0, 0, 0}, {0.5F, 0.0F, 0.0F, 0.0F}),
        vertex({2.0F, 0.0F, 0.0F}, {0, 0, 0, 0}, {0.0F, 0.0F, 0.0F, 0.0F}),
        vertex({3.0F, 0.0F, 0.0F}, {0, 0, 0, 0}, {0.6F, 0.6F, 0.0F, 0.0F}),
        vertex({4.0F, 0.0F, 0.0F}, {0, 0, 0, 0}, {1.0F, 0.0F, 0.0F, 0.0F})}, {0, 1, 2, 3, 3, 3}, {0})));
    const auto animation = clip({track(0, "root", {{{}}, {{0.0F, 5.0F, 0.0F}}, {{}}})});
    const auto evaluation = probe::evaluate(model, {"Skin", 0}, bind_pose(model), animated_pose(model, animation, 1.0F));
    expect(static_cast<bool>(evaluation), "weights: zero and non-unit sums are accepted, not refused");
    if (!evaluation) return;
    const auto& result = *evaluation.value;
    expect(result.nonunit == 2 && result.zero == 1 && result.unit == 1, "weights: classes counted");
    expect(result.predicted == 1 && result.changed == 1, "weights: only the unit-sum vertex is predicted and counted");
    expect(!result.vertices[0].vertex.position && !result.vertices[0].animated, "weights: non-unit sum has no position");
    expect(!result.vertices[1].vertex.position && !result.vertices[1].animated, "weights: zero sum has no position");
    expect(result.vertices[0].vertex.influences[0].weight == 0.5F && result.vertices[0].vertex.weight_sum == 0.5F,
        "weights: raw weight kept unchanged, not normalized to 1");
    expect(result.vertices[1].vertex.active_influences == 0, "weights: zero sum has no active influence");
    const std::string block = probe::vertex_block(result);
    expect(block.find("\"weight\":0.5,") != std::string::npos && block.find("\"bind\":null") != std::string::npos,
        "weights: receipt records the raw weight and a null prediction");
}

std::string summary(const probe::Evaluation& evaluation) {
    std::ostringstream out;
    probe::write_summary(out, evaluation);
    return out.str();
}

// A zero changed count claims "unchanged at this sample" only when every indexed
// vertex is predicted.  With no prediction it is indeterminate; with some
// unpredicted vertices it covers the predicted subset only.
void test_unchanged_scope() {
    assets::Model model;
    model.bones = {bone("still", -1), bone("moving", -1)};
    // All unpredicted: a non-unit and a zero sum, both on the animated bone.
    model.meshes.push_back(mesh("Unpredicted", -1, submesh({
        vertex({1.0F, 0.0F, 0.0F}, {1, 0, 0, 0}, {0.5F, 0.0F, 0.0F, 0.0F}),
        vertex({2.0F, 0.0F, 0.0F}, {1, 0, 0, 0}, {0.0F, 0.0F, 0.0F, 0.0F})}, {0, 1, 1}, {0, 1})));
    // Mixed: a unit vertex on the still bone is predicted and unchanged; a
    // non-unit vertex on the moving bone has no prediction.
    model.meshes.push_back(mesh("Mixed", -1, submesh({
        vertex({1.0F, 0.0F, 0.0F}, {0, 0, 0, 0}, {1.0F, 0.0F, 0.0F, 0.0F}),
        vertex({2.0F, 0.0F, 0.0F}, {1, 0, 0, 0}, {0.6F, 0.6F, 0.0F, 0.0F})}, {0, 1, 1}, {0, 1})));
    // Fully predicted and unchanged: unit weight on the still bone only.
    model.meshes.push_back(mesh("Still", -1, submesh({
        vertex({1.0F, 0.0F, 0.0F}, {0, 0, 0, 0}, {1.0F, 0.0F, 0.0F, 0.0F})}, {0, 0, 0}, {0, 1})));
    // Fully predicted and moving.
    model.meshes.push_back(mesh("Moving", -1, submesh({
        vertex({1.0F, 0.0F, 0.0F}, {1, 0, 0, 0}, {1.0F, 0.0F, 0.0F, 0.0F})}, {0, 0, 0}, {0, 1})));
    const auto animation = clip({track(1, "moving", {{{}}, {{0.0F, 5.0F, 0.0F}}, {{}}})});
    const auto bind = bind_pose(model);
    const auto pose = animated_pose(model, animation, 1.0F);

    const auto unpredicted = probe::evaluate(model, {"Unpredicted", 0}, bind, pose);
    expect(unpredicted && unpredicted.value->predicted == 0 && unpredicted.value->changed == 0,
        "scope: all-unpredicted control has no prediction and no changed vertex");
    expect(unpredicted && probe::unchanged_scope(*unpredicted.value) == "indeterminate_no_prediction",
        "scope: all-unpredicted is indeterminate, not unchanged");
    expect(unpredicted && summary(*unpredicted.value).find("\"unchanged_scope\":\"indeterminate_no_prediction\"") != std::string::npos
        && summary(*unpredicted.value).find("unchanged_at_this_sample_only") == std::string::npos,
        "scope: all-unpredicted receipt never claims unchanged");

    const auto mixed = probe::evaluate(model, {"Mixed", 0}, bind, pose);
    expect(mixed && mixed.value->predicted == 1 && mixed.value->changed == 0 && mixed.value->vertices.size() == 2,
        "scope: mixed control predicts one of two vertices, none changed");
    expect(mixed && probe::unchanged_scope(*mixed.value) == "unchanged_predicted_subset_only",
        "scope: mixed unchanged covers the predicted subset only");
    expect(mixed && summary(*mixed.value).find("\"unchanged_scope\":\"unchanged_predicted_subset_only\"") != std::string::npos
        && summary(*mixed.value).find("unchanged_at_this_sample_only") == std::string::npos,
        "scope: mixed receipt never claims the whole sample unchanged");

    const auto still = probe::evaluate(model, {"Still", 0}, bind, pose);
    expect(still && probe::unchanged_scope(*still.value) == "unchanged_at_this_sample_only",
        "scope: fully predicted and unchanged is unchanged at this sample only");
    const auto moving = probe::evaluate(model, {"Moving", 0}, bind, pose);
    expect(moving && moving.value->changed == 1 && probe::unchanged_scope(*moving.value) == "not_applicable",
        "scope: a changed sample is not_applicable");
}

// Finite float positions can lie further apart than the largest float.  Such
// displacement and bind-residual distances are refused, never reported as
// infinity alongside success.
void test_distance_overflow() {
    assets::Model model;
    model.bones = {bone("root", -1)};
    model.meshes.push_back(mesh("Rigid", 0, submesh({vertex({3.0e38F, 0.0F, 0.0F})}, {0, 0, 0})));
    const auto bind = bind_pose(model);
    playback::Pose mirrored = bind;
    mirrored.bones[0].model_asset[0] = -1.0F;  // rigid x -> -x: finite (-3e38,0,0)
    const auto baseline = probe::evaluate(model, {"Rigid", 0}, bind, bind);
    expect(baseline && baseline.value->max_displacement == 0.0F && baseline.value->max_bind_residual == 0.0F,
        "distance overflow baseline: identity poses accepted");
    const auto displaced = probe::evaluate(model, {"Rigid", 0}, bind, mirrored);
    expect(!displaced && displaced.error.find("displacement overflows") != std::string::npos,
        "distance overflow: animated displacement beyond float max is refused");
    // Bind and animated agree (displacement 0) but bind is 6e38 from the source.
    const auto residual = probe::evaluate(model, {"Rigid", 0}, mirrored, mirrored);
    expect(!residual && residual.error.find("bind residual overflows") != std::string::npos,
        "distance overflow: bind residual beyond float max is refused");
}

// Only vertices referenced by the index buffer are evaluated, but every vertex
// is validated, as the renderer uploads the whole buffer.
void test_indexed_vertices_only() {
    assets::Model model;
    model.bones = {bone("root", -1)};
    model.meshes.push_back(mesh("Skin", -1, submesh({
        vertex({1.0F, 0.0F, 0.0F}, {0, 0, 0, 0}, {1.0F, 0.0F, 0.0F, 0.0F}),
        vertex({2.0F, 0.0F, 0.0F}, {0, 0, 0, 0}, {1.0F, 0.0F, 0.0F, 0.0F}),
        vertex({3.0F, 0.0F, 0.0F}, {0, 0, 0, 0}, {1.0F, 0.0F, 0.0F, 0.0F})}, {2, 0, 2}, {0})));
    const auto pose = bind_pose(model);
    const auto deformed = probe::deform(model, {"Skin", 0}, pose);
    expect(deformed && deformed.value->vertices.size() == 2 && deformed.value->vertices[0].index == 0
        && deformed.value->vertices[1].index == 2, "indexed: unreferenced vertex 1 excluded, ascending order");
    model.meshes[0].submeshes[0].vertices[1].bone_weights = {-1.0F, 0.0F, 0.0F, 0.0F};
    expect(fails_with(probe::deform(model, {"Skin", 0}, pose), "negative"), "indexed: an unreferenced malformed vertex still refuses");
}

void test_malformed_inputs() {
    assets::Model model;
    model.bones = {bone("root", -1), bone("child", 0, {0.0F, 1.0F, 0.0F})};
    model.meshes.push_back(mesh("Skin", -1, submesh({vertex({1.0F, 0.0F, 0.0F}, {0, 1, 0, 0}, {0.5F, 0.5F, 0.0F, 0.0F})},
        {0, 0, 0}, {0, 1})));
    model.meshes.push_back(mesh("Rigid", 1, submesh({vertex({1.0F, 0.0F, 0.0F})}, {0, 0, 0})));
    const auto pose = bind_pose(model);
    expect(static_cast<bool>(probe::deform(model, {"Skin", 0}, pose)), "malformed baseline: valid skin accepted");
    expect(static_cast<bool>(probe::deform(model, {"Rigid", 0}, pose)), "malformed baseline: valid rigid accepted");

    const auto with_skin = [&](auto&& change) {
        assets::Model copy = model;
        change(copy.meshes[0].submeshes[0]);
        return probe::deform(copy, {"Skin", 0}, pose);
    };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    expect(fails_with(with_skin([&](auto& s) { s.vertices[0].bone_weights[0] = nan; }), "non-finite"), "malformed: NaN weight");
    expect(fails_with(with_skin([&](auto& s) { s.vertices[0].bone_weights[1] = inf; }), "non-finite"), "malformed: infinite weight");
    expect(fails_with(with_skin([&](auto& s) { s.vertices[0].bone_weights[1] = -0.5F; }), "negative"), "malformed: negative weight");
    expect(fails_with(with_skin([&](auto& s) { s.vertices[0].bone_weights = {-0.5F, 0.0F, 0.0F, 0.0F}; s.vertices[0].bone_indices = {99, 0, 0, 0}; }),
        "negative"), "malformed: negative weight on an invalid index");
    expect(fails_with(with_skin([&](auto& s) { s.vertices[0].bone_indices[1] = 2; }), "active local bone index"),
        "malformed: active local index outside the palette");
    expect(fails_with(with_skin([&](auto& s) { s.skin_bones[1] = 9; }), "skin palette bone"), "malformed: palette bone outside the model");
    expect(fails_with(with_skin([&](auto& s) { s.vertices[0].bone_weights = {3.0e38F, 3.0e38F, 0.0F, 0.0F}; }), "overflows"),
        "malformed: weight sum overflow");
    expect(fails_with(with_skin([&](auto& s) { s.vertices[0].position = {nan, 0.0F, 0.0F}; }), "non-finite"), "malformed: non-finite position");
    expect(fails_with(with_skin([&](auto& s) { s.indices = {0, 0, 1}; }), "index exceeds"), "malformed: index out of range");
    expect(fails_with(with_skin([&](auto& s) { s.indices = {0, 0}; }), "triangle list"), "malformed: index count not a triangle list");
    expect(fails_with(with_skin([&](auto& s) { s.indices.clear(); }), "triangle list"), "malformed: empty index buffer");
    expect(fails_with(with_skin([&](auto& s) { s.vertices.clear(); s.indices.clear(); }), "no vertices"), "malformed: no vertices");

    {
        assets::Model copy = model;
        copy.meshes[1].submeshes[0].vertices[0].position = {3.0e38F, 0.0F, 0.0F};
        playback::Pose far = pose;
        far.bones[1].model_asset[12] = 3.0e38F;
        expect(fails_with(probe::deform(copy, {"Rigid", 0}, far), "overflows"), "malformed: deformed position overflow");
    }
    {
        playback::Pose short_pose = pose;
        short_pose.bones.pop_back();
        expect(fails_with(probe::deform(model, {"Skin", 0}, short_pose), "bone count"), "malformed: pose bone count");
        playback::Pose bad = pose;
        bad.bones[0].skin_asset[5] = nan;
        expect(fails_with(probe::deform(model, {"Skin", 0}, bad), "non-finite skin"), "malformed: non-finite pose matrix");
        bad = pose;
        bad.bones[1].skin_asset[3] = 0.5F;
        expect(fails_with(probe::deform(model, {"Skin", 0}, bad), "non-affine"), "malformed: non-affine pose matrix");
    }
    {
        assets::Model copy = model;
        copy.meshes[1].bone = 5;
        expect(fails_with(probe::deform(copy, {"Rigid", 0}, pose), "rigid mesh bone"), "malformed: rigid bone out of range");
        copy.meshes[1].bone = -2;
        expect(fails_with(probe::deform(copy, {"Rigid", 0}, pose), "negative but not -1"), "malformed: mesh bone below -1");
    }
    {
        assets::Model boneless;
        boneless.meshes.push_back(model.meshes[0]);
        expect(fails_with(probe::deform(boneless, {"Skin", 0}, playback::Pose{}), "without bones"), "malformed: skinned mesh without bones");
    }
    expect(fails_with(probe::deform(model, {"skin", 0}, pose), "absent"), "selection: mesh name is exact and case-sensitive");
    expect(fails_with(probe::deform(model, {"Skin", 1}, pose), "submesh index"), "selection: submesh out of range");
    {
        assets::Model copy = model;
        copy.meshes.push_back(copy.meshes[0]);
        expect(fails_with(probe::deform(copy, {"Skin", 0}, pose), "ambiguous"), "selection: duplicate mesh name");
        copy = model;
        copy.meshes[0].visible = false;
        expect(fails_with(probe::deform(copy, {"Skin", 0}, pose), "not drawable"), "selection: invisible mesh is not drawable");
    }
}

// The probe works in asset space.  The renderer converts positions with
// (x,y,z) -> (x,z,-y) and palettes with C S C^-1; converting after deforming
// must equal deforming the converted position by the converted palette.
void test_asset_to_render_identity() {
    const Vec3f p{1.5F, -2.0F, 3.25F};
    const Vec3f converted = probe::asset_to_render_point(p);
    expect(close(converted, {p.x, p.z, -p.y}), "render conversion: point maps (x,y,z) -> (x,z,-y)");
    assets::Model model;
    model.bones = {bone("root", -1, {0.5F, 0.0F, 0.0F})};
    const auto animation = clip({track(0, "root", {{{}}, {{1.0F, 2.0F, 3.0F}, z_quarter_turn}, {{}}})});
    const auto pose = animated_pose(model, animation, 1.0F);
    const playback::Matrix skin = pose.bones[0].skin_asset;
    const Vec3f deformed_then_converted = probe::asset_to_render_point(probe::transform_point(skin, p));
    const Vec3f converted_then_deformed = probe::transform_point(playback::Player::asset_to_render_transform(skin), converted);
    expect(close(deformed_then_converted, converted_then_deformed), "render conversion: C(S p) == (C S C^-1)(C p)");
    expect(!close(deformed_then_converted, probe::transform_point(skin, p)), "render conversion: the conversion is not the identity here");
}

// Repeated evaluation is byte-identical and never modifies its inputs.
void test_determinism_and_unchanged_inputs() {
    assets::Model model;
    model.bones = {bone("b0", -1), bone("b1", 0, {0.0F, 1.0F, 0.0F})};
    model.meshes.push_back(mesh("Na\"me\n", -1, submesh({
        vertex({1.0F, 1.0F, 0.0F}, {0, 1, 0, 0}, {0.3F, 0.7F, 0.0F, 0.0F}),
        vertex({0.0F, 1.0F, 1.0F}, {1, 0, 0, 0}, {1.0F, 0.0F, 0.0F, 0.0F})}, {0, 1, 1}, {1, 0})));
    const auto animation = clip({track(1, "b1", {{{0.0F, 1.0F, 0.0F}}, {{0.5F, 1.0F, 0.0F}, z_quarter_turn}, {{0.0F, 1.0F, 0.0F}}})});
    const std::string model_before = fingerprint(model);
    const std::string animation_before = fingerprint(animation);
    const auto bind = bind_pose(model);
    const auto pose = animated_pose(model, animation, 0.3F);
    const auto receipt = [&]() {
        const auto evaluation = probe::evaluate(model, {"Na\"me\n", 0}, bind, pose);
        if (!evaluation) return std::string("refused: ") + evaluation.error;
        probe::Header header;
        header.mesh = "Na\"me\n";
        header.mode = "loop";
        header.requested_time = 0.3F;
        header.sampled_time = pose.sampled_time_seconds;
        const std::string vertices = probe::vertex_block(*evaluation.value);
        std::ostringstream out;
        probe::write_receipt(out, header, *evaluation.value, vertices, "digest");
        return out.str();
    };
    const std::string first = receipt();
    const std::string second = receipt();
    expect(first.rfind("refused", 0) != 0, "determinism: evaluation succeeds");
    expect(first == second, "determinism: repeated receipts are byte-identical");
    expect(first.find("Na\\\"me\\u000a") != std::string::npos, "determinism: mesh name escaped without raw control bytes");
    expect(first.find("\"cpu_prediction_only\":true") != std::string::npos
        && first.find("\"gpu_parity_claim\":false") != std::string::npos
        && first.find("\"association_promoted\":false") != std::string::npos, "receipt: claims are bounded");
    expect(fingerprint(model) == model_before && fingerprint(animation) == animation_before, "inputs: model and animation unchanged");
}

} // namespace

int main() {
    test_bind_translation();
    test_rotated_translated_child();
    test_local_palette_four_influences();
    test_rigid_and_unskinned();
    test_zero_influence_bone_and_inactive_index();
    test_rotation_axis_moving_vertex_and_bind_negative();
    test_loop_and_clamp_endpoints();
    test_nonunit_and_zero_weights();
    test_unchanged_scope();
    test_distance_overflow();
    test_indexed_vertices_only();
    test_malformed_inputs();
    test_asset_to_render_identity();
    test_determinism_and_unchanged_inputs();
    if (failures != 0) {
        std::cerr << failures << " deformation probe check(s) failed\n";
        return 1;
    }
    std::cout << "deformation probe controls passed\n";
    return 0;
}
