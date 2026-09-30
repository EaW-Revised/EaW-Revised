// Synthetic controls for the shot-readiness census, palette, attachment and
// digest helpers. Every expectation is derived by hand from the fixture.

#include "eawr/presentation/animation/shot_readiness.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace eawr;
namespace playback = eawr::presentation::animation;
using playback::Matrix;

int failures{};
void expect(const bool condition, const std::string_view message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; ++failures; }
}
bool close(const double left, const double right, const double tolerance = 1.0e-5) {
    return std::abs(left - right) <= tolerance;
}

assets::Bone bone(const std::string_view name, const std::int32_t parent, const float x = 0.0F, const float y = 0.0F) {
    assets::Bone value;
    value.name = name; value.parent = parent; value.visible = true;
    value.relative_transform = {1.0F, 0.0F, 0.0F, x, 0.0F, 1.0F, 0.0F, y, 0.0F, 0.0F, 1.0F, 0.0F};
    return value;
}

assets::Vertex vertex(const std::array<std::uint32_t, 4> indices, const std::array<float, 4> weights) {
    assets::Vertex value;
    value.position = {1.0F, 0.0F, 0.0F};
    value.bone_indices = indices;
    value.bone_weights = weights;
    return value;
}

assets::Submesh submesh(std::vector<std::uint32_t> palette, std::vector<assets::Vertex> vertices) {
    assets::Submesh value;
    value.shader = "RSkinBumpColorize.fx";
    value.skin_bones = std::move(palette);
    value.vertices = std::move(vertices);
    value.indices = {0, 0, 0};
    return value;
}

assets::Mesh mesh(const std::string_view name, const std::int32_t bone_index, const bool visible, assets::Submesh part) {
    assets::Mesh value;
    value.name = name; value.bone = bone_index; value.visible = visible;
    value.submeshes.push_back(std::move(part));
    return value;
}

// Bones: 0 root (tracked), 1 child of root, 2 static, 3 listed-only.
assets::Model fixture() {
    assets::Model model;
    model.source.logical_path = "synthetic.alo";
    model.bones = {bone("Root", -1), bone("Child", 0, 0.0F, 2.0F), bone("Static", -1, 5.0F), bone("Listed", -1)};
    // Palette {1,2,3}: slot 0 (Child) and slot 1 (Static) carry weight; slot 2
    // (Listed) only appears with weight 0 and -0.0 on an out-of-range index.
    model.meshes.push_back(mesh("Skin", -1, true, submesh({1, 2, 3}, {
        vertex({0, 1, 0, 0}, {0.25F, 0.75F, 0.0F, 0.0F}),
        vertex({0, 2, 9, 0}, {1.0F, 0.0F, -0.0F, 0.0F}),
        vertex({0, 0, 0, 0}, {1.0F, 0.0F, 0.0F, 0.0F})})));
    model.meshes.push_back(mesh("Rigid", 0, true, submesh({}, {vertex({0, 0, 0, 0}, {0, 0, 0, 0}),
        vertex({0, 0, 0, 0}, {0, 0, 0, 0})})));
    model.meshes.push_back(mesh("HiddenRigid", 2, false, submesh({}, {vertex({0, 0, 0, 0}, {0, 0, 0, 0})})));
    model.meshes.push_back(mesh("Loose", -1, true, submesh({}, {vertex({0, 0, 0, 0}, {0, 0, 0, 0})})));
    model.proxies.push_back({"p_effect", 1, true, false});
    assets::Light light; light.name = "L"; light.bone = 3; model.lights.push_back(light);
    assets::Dazzle dazzle; dazzle.name = "D"; dazzle.bone = 0; model.dazzles.push_back(dazzle);
    return model;
}

// Root translates 0 -> 10 along x over frames 0..2 at 1 fps (loop of 2 s).
assets::Animation clip() {
    assets::Animation value;
    value.source.logical_path = "synthetic.ala";
    value.frames_per_second = 1.0F;
    value.stored_frame_count = 3;
    value.playable_frame_count = 2;
    value.duration_seconds = 2.0F;
    assets::AnimationTrack track;
    track.bone_index = 0; track.bone_name = "Root";
    track.samples = {
        {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, true},
        {{10.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, true},
        {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, true}};
    value.tracks.push_back(track);
    return value;
}

void census_controls() {
    const auto model = fixture();
    const auto animation = clip();
    const auto census = playback::tracked_bind_census(model, &animation);
    expect(census.has_value(), "census accepts the fixture");
    if (!census) return;
    const auto& c = census.value();
    expect(c.bone_count == 4 && c.track_count == 1 && c.bones.size() == 4, "census counts bones and tracks");
    expect(c.bones[0].tracked && !c.bones[0].inherits_tracked, "Root is tracked");
    expect(!c.bones[1].tracked && c.bones[1].inherits_tracked, "Child inherits its tracked parent");
    expect(!c.bones[2].tracked && !c.bones[2].inherits_tracked, "Static is unaffected");
    expect(c.bones[0].rigid_meshes == 1 && c.bones[0].palette_listed == 0 && c.bones[0].dazzles == 1, "Root: one rigid mesh, one dazzle");
    expect(c.bones[1].palette_listed == 1 && c.bones[1].palette_weighted == 1 && c.bones[1].proxies == 1, "Child: weighted palette slot, one proxy");
    expect(c.bones[2].palette_weighted == 1 && c.bones[2].hidden_bindings == 1 && c.bones[2].rigid_meshes == 0,
        "Static: weighted, and its invisible rigid mesh is a hidden binding only");
    expect(c.bones[3].palette_listed == 1 && c.bones[3].palette_weighted == 0 && c.bones[3].lights == 1,
        "Listed: zero and -0.0 weights leave it unweighted");
    expect(c.draw_bound == 3 && c.draw_bound_tracked == 1 && c.draw_bound_inherited == 1 && c.draw_bound_static == 1,
        "draw-bound split tracked / inherited / static");
    expect(c.tracked_not_draw_bound == 0 && c.palette_listed_unweighted == 1, "no tracked-only bone; one listed-only bone");

    const auto bind_only = playback::tracked_bind_census(model);
    expect(bind_only.has_value() && bind_only.value().track_count == 0 && bind_only.value().draw_bound_static == 3
        && !bind_only.value().bones[0].tracked, "bind-only census: every draw-bound bone is static");

    // A tracked bone that draws nothing is reported separately.
    auto unbound = fixture();
    unbound.meshes.erase(unbound.meshes.begin() + 1);  // drop the Root rigid mesh
    const auto unbound_census = playback::tracked_bind_census(unbound, &animation);
    expect(unbound_census.has_value() && unbound_census.value().tracked_not_draw_bound == 1
        && unbound_census.value().draw_bound_tracked == 0, "tracked but not draw-bound is counted");
}

void census_refusals() {
    const auto animation = clip();
    const auto refused = [&](assets::Model model, const std::string_view why, const std::string_view code) {
        const auto result = playback::tracked_bind_census(model, &animation);
        expect(!result.has_value() && result.error().code == code, why);
    };
    { auto m = fixture(); m.meshes[0].submeshes[0].vertices[0].bone_indices[0] = 3; refused(m, "active index outside the palette", playback::diagnostic_codes::invalid_model); }
    { auto m = fixture(); m.meshes[0].submeshes[0].skin_bones[0] = 4; refused(m, "palette bone outside the model", playback::diagnostic_codes::invalid_model); }
    { auto m = fixture(); m.meshes[0].submeshes[0].vertices[1].bone_weights[2] = -0.5F; refused(m, "negative weight, even on an invalid index", playback::diagnostic_codes::invalid_model); }
    { auto m = fixture(); m.meshes[0].submeshes[0].vertices[0].bone_weights[0] = std::numeric_limits<float>::quiet_NaN(); refused(m, "NaN weight", playback::diagnostic_codes::invalid_model); }
    { auto m = fixture(); m.meshes[0].submeshes[0].vertices[0].bone_weights[0] = std::numeric_limits<float>::infinity(); refused(m, "infinite weight", playback::diagnostic_codes::invalid_model); }
    { auto m = fixture(); m.meshes[3].bone = -2; refused(m, "mesh bone below -1", playback::diagnostic_codes::invalid_model); }
    { auto m = fixture(); m.meshes[1].bone = 4; refused(m, "rigid bone outside the model", playback::diagnostic_codes::invalid_model); }
    { auto m = fixture(); m.proxies[0].bone = 4; refused(m, "proxy bone outside the model", playback::diagnostic_codes::invalid_model); }
    { auto m = fixture(); m.lights[0].bone = 7; refused(m, "light bone outside the model", playback::diagnostic_codes::invalid_model); }
    { auto m = fixture(); m.dazzles[0].bone = 9; refused(m, "dazzle bone outside the model", playback::diagnostic_codes::invalid_model); }
    {
        assets::Model m; m.source.logical_path = "boneless.alo";
        m.meshes.push_back(mesh("Skin", -1, true, submesh({0}, {vertex({0, 0, 0, 0}, {1, 0, 0, 0})})));
        const auto result = playback::tracked_bind_census(m);
        expect(!result.has_value() && result.error().code == playback::diagnostic_codes::invalid_model, "skinned mesh on a boneless model");
    }
    {
        auto wrong = clip(); wrong.tracks[0].bone_name = "root";
        const auto result = playback::tracked_bind_census(fixture(), &wrong);
        expect(!result.has_value() && result.error().code == playback::diagnostic_codes::invalid_animation,
            "a pair the player refuses is refused with the player's diagnostic");
    }
    {
        // An inactive index outside the palette is accepted (weight 0 and -0.0).
        auto m = fixture(); m.meshes[0].submeshes[0].vertices[2].bone_indices[3] = 200;
        expect(playback::tracked_bind_census(m, &animation).has_value(), "inactive out-of-range index is ignored");
    }
}

void palette_controls() {
    const auto model = fixture();
    const auto animation = clip();
    const auto census = playback::tracked_bind_census(model, &animation).value();
    const auto player = playback::Player::create(model, &animation).value();
    const auto pose = player.sample_tick(15, 30).value();  // 0.5 s: Root at x = 5
    const auto palette = playback::submesh_palette(player, pose, model, census, "Skin", 0);
    expect(palette.has_value(), "palette for Skin/0");
    if (palette) {
        const auto& p = palette.value();
        expect(p.route == playback::SkinRoute::palette && p.slots.size() == 3 && p.vertex_count == 3 && p.shader == "RSkinBumpColorize.fx",
            "palette route, three slots");
        expect(p.slots[0].bone == 1 && p.slots[0].name == "Child" && p.slots[0].inherits_tracked && p.slots[0].active_influences == 3,
            "slot 0 is global Child with three active influences");
        expect(p.slots[1].bone == 2 && p.slots[1].active_influences == 1 && !p.slots[1].tracked && !p.slots[1].inherits_tracked,
            "slot 1 is global Static with one active influence");
        expect(p.slots[2].bone == 3 && p.slots[2].active_influences == 0, "slot 2 is listed but carries no weight");
        expect(close(p.slots[0].skin_asset[12], 5.0) && close(p.slots[0].skin_asset[13], 0.0), "Child skin translation follows Root to x = 5");
        expect(close(p.slots[1].skin_asset[12], 0.0), "Static skin matrix is identity-translated");
        expect(p.slots[0].skin_asset == pose.bones[1].skin_asset, "slot matrix is the pose's own skin matrix");
    }
    const auto rigid = playback::submesh_palette(player, pose, model, census, "Rigid", 0);
    expect(rigid.has_value() && rigid.value().route == playback::SkinRoute::rigid && rigid.value().slots.size() == 1
        && rigid.value().slots[0].bone == 0 && rigid.value().slots[0].tracked && rigid.value().slots[0].active_influences == 2,
        "rigid route synthesizes one slot on mesh.bone with every vertex");
    const auto loose = playback::submesh_palette(player, pose, model, census, "Loose", 0);
    expect(loose.has_value() && loose.value().route == playback::SkinRoute::unskinned && loose.value().slots.empty(), "unskinned route has no slot");

    const auto refused = [&](const core::Result<playback::SubmeshPalette>& result, const std::string_view why) {
        expect(!result.has_value() && result.error().code == playback::diagnostic_codes::invalid_request, why);
    };
    refused(playback::submesh_palette(player, pose, model, census, "HiddenRigid", 0), "invisible mesh refused");
    refused(playback::submesh_palette(player, pose, model, census, "skin", 0), "case-mismatched mesh refused");
    refused(playback::submesh_palette(player, pose, model, census, "Skin", 1), "submesh out of range refused");
    refused(playback::submesh_palette(player, playback::Pose{}, model, census, "Skin", 0), "default pose refused");
    const auto other = playback::Player::create(model, &animation).value();
    refused(playback::submesh_palette(player, other.sample_tick(15, 30).value(), model, census, "Skin", 0),
        "a pose from an independently created player is refused");
    auto duplicate = model; duplicate.meshes.push_back(duplicate.meshes[0]);
    const auto duplicate_census = playback::tracked_bind_census(duplicate, &animation).value();
    const auto duplicate_player = playback::Player::create(duplicate, &animation).value();
    refused(playback::submesh_palette(duplicate_player, duplicate_player.sample_tick(0, 30).value(), duplicate, duplicate_census, "Skin", 0),
        "ambiguous mesh refused");
    auto renamed = census; renamed.bones[2].name = "Other";
    refused(playback::submesh_palette(player, pose, model, renamed, "Skin", 0), "census of another model refused");
    const auto copy = pose;
    expect(playback::submesh_palette(player, copy, model, census, "Skin", 0).has_value(), "a copy of a sampled pose is accepted");
}

void attachment_controls() {
    const auto model = fixture();
    const auto animation = clip();
    const auto census = playback::tracked_bind_census(model, &animation).value();
    const auto player = playback::Player::create(model, &animation).value();
    const auto bind = player.sample({0.0F, playback::PlaybackMode::loop, 1.0F}).value();
    const auto half = player.sample_tick(15, 30).value();
    const auto child = playback::attachment_probe(player, bind, half, census, "Child");
    expect(child.has_value(), "Child attachment probe");
    if (child) {
        const auto& c = child.value();
        expect(c.index == 1 && !c.tracked && c.inherits_tracked, "Child probe records inheritance");
        // Asset (5,2,0) -> render (x, z, -y) = (5, 0, -2); bind (0,2,0) -> (0, 0, -2).
        expect(close(c.sampled.column_major[12], 5.0) && close(c.sampled.column_major[13], 0.0) && close(c.sampled.column_major[14], -2.0),
            "Child sampled origin in the render basis");
        expect(close(c.reference.column_major[12], 0.0) && close(c.reference.column_major[14], -2.0), "Child bind origin in the render basis");
        expect(close(c.translation_delta, 5.0) && close(c.basis_delta, 0.0), "Child moves 5 units with no rotation");
    }
    const auto fixed = playback::attachment_probe(player, bind, half, census, "Static");
    expect(fixed.has_value() && fixed.value().translation_delta == 0.0 && fixed.value().basis_delta == 0.0
        && close(fixed.value().sampled.column_major[12], 5.0), "Static attachment does not move");
    const auto missing = playback::attachment_probe(player, bind, half, census, "Muzzle");
    expect(!missing.has_value() && missing.error().code == playback::diagnostic_codes::missing_attachment, "absent bone is missing_attachment");
    const auto other = playback::Player::create(model, &animation).value();
    const auto foreign = playback::attachment_probe(player, bind, other.sample_tick(15, 30).value(), census, "Child");
    expect(!foreign.has_value() && foreign.error().code == playback::diagnostic_codes::invalid_request, "foreign pose refused");

    const auto moved = playback::moved_bones(player, bind, half, 1.0e-4F);
    expect(moved.has_value() && moved.value() == std::vector<std::size_t>{0, 1}, "Root and Child moved; Static and Listed did not");
    const auto start = player.sample_tick(0, 30).value();
    const auto none = playback::moved_bones(player, bind, start, 1.0e-4F);
    expect(none.has_value() && none.value().empty(), "clip start equals bind for this fixture");
    expect(!playback::moved_bones(player, bind, half, -1.0F).has_value(), "negative epsilon refused");
    expect(!playback::moved_bones(player, bind, other.sample_tick(0, 30).value(), 0.0F).has_value(), "moved_bones refuses a foreign pose");
}

void digest_controls() {
    playback::Pose identity;
    identity.bones.resize(1);
    identity.bones[0].local_asset = identity.bones[0].model_asset = identity.bones[0].skin_asset = playback::Player::identity_matrix();
    // Known answers computed independently from the documented encoding.
    expect(playback::pose_digest(identity) == "57c3d338e14027404dc8ed1dcdf16ac97d6f567197511a2f896f071ef01c3b84",
        "pose digest known answer");
    playback::SubmeshPalette rigid;
    rigid.route = playback::SkinRoute::rigid; rigid.mesh = "M"; rigid.submesh = 0;
    playback::PaletteSlot slot; slot.local = 0; slot.bone = 0; slot.name = "root"; slot.active_influences = 3;
    slot.skin_asset = playback::Player::identity_matrix();
    rigid.slots.push_back(slot);
    expect(playback::palette_digest(rigid) == "2c73a44669e357d0f6d1e96abd6e5bc78417344871fe3de645d72b91094d797f",
        "palette digest known answer");

    auto flipped = identity;
    flipped.bones[0].skin_asset[0] = std::nextafter(1.0F, 2.0F);
    expect(playback::pose_digest(flipped) != playback::pose_digest(identity), "one-ulp skin change changes the pose digest");
    auto negative_zero = identity; negative_zero.bones[0].model_asset[1] = -0.0F;
    expect(playback::pose_digest(negative_zero) != playback::pose_digest(identity), "the digest is over exact bits (-0.0 differs)");
    auto hidden = identity; hidden.bones[0].visible = false;
    expect(playback::pose_digest(hidden) != playback::pose_digest(identity), "visibility enters the pose digest");
    auto counted = rigid; counted.slots[0].active_influences = 4;
    expect(playback::palette_digest(counted) != playback::palette_digest(rigid), "active count enters the palette digest");
    auto remapped = rigid; remapped.slots[0].bone = 1;
    expect(playback::palette_digest(remapped) != playback::palette_digest(rigid), "global bone enters the palette digest");

    const auto model = fixture();
    const auto animation = clip();
    const auto player = playback::Player::create(model, &animation).value();
    const auto a = player.sample_tick(9, 30).value();
    const auto b = player.sample_tick(9, 30).value();
    const auto again = playback::Player::create(model, &animation).value().sample_tick(9, 30).value();
    expect(playback::pose_digest(a) == playback::pose_digest(b) && playback::pose_digest(a) == playback::pose_digest(again),
        "fixed-tick digests repeat within and across players");
    expect(playback::pose_digest(a) != playback::pose_digest(player.sample_tick(10, 30).value()), "a different tick changes the digest");
}

void player_origin_controls() {
    const auto model = fixture();
    const auto animation = clip();
    auto player = playback::Player::create(model, &animation).value();
    const auto pose = player.sample_tick(3, 30).value();
    expect(player.sampled(pose) && player.bone_count() == 4, "sampled() accepts the player's own pose");
    expect(!player.sampled(playback::Pose{}), "sampled() rejects a default pose");
    const auto other = playback::Player::create(model, &animation).value();
    expect(!other.sampled(pose), "sampled() rejects another player's pose");
    auto moved = std::move(player);
    expect(moved.sampled(pose), "a moved player keeps its origin");
}

} // namespace

int main() {
    census_controls();
    census_refusals();
    palette_controls();
    attachment_controls();
    digest_controls();
    player_origin_controls();
    if (failures != 0) { std::cerr << failures << " shot-readiness control(s) failed\n"; return 1; }
    std::cout << "shot-readiness controls passed\n";
    return 0;
}
