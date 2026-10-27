#include "../../support/check.hpp"

#include "eawr/assets/geometry_stripped.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/particles/attachment_lifecycle.hpp"
#include "eawr/presentation/particles/map_attachment_owner.hpp"
#include "eawr/presentation/particles/map_effect_plan.hpp"
#include "eawr/presentation/particles/particles.hpp"
#include "eawr/presentation/particles/render.hpp"
#include "eawr/presentation/particles/proxy_binding.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Contracts for the UnitEmitters emitter metadata (#4645): the geometry-stripped Model copy answers the
// emitter planners exactly as the decoded Model does, and carries no geometry. Wholly synthetic.
namespace particles = eawr::presentation::particles;
namespace playback = eawr::presentation::animation;
namespace assets = eawr::assets;
namespace scene = eawr::scene;
namespace math = eawr::sim::math;

namespace {

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(const bool condition, const char* message) { local_checker.check(condition, message, std::cerr); }

eawr::core::Result<math::Fixed> test_fixed(const float value) {
    return eawr::core::Result<math::Fixed>::success(
        math::Fixed::from_raw(std::llround(static_cast<double>(value) * static_cast<double>(math::Fixed::scale))));
}

assets::Model make_model() {
    assets::Model model;
    model.source.logical_path = "data/art/models/eawr_metadata_host.alo";
    model.source.source_id = "synthetic:emitter-metadata";
    assets::Bone root; root.name = "Root"; root.parent = -1;
    root.relative_transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    assets::Bone hull = root; hull.name = "Hull"; hull.parent = 0; hull.relative_transform[3] = 2.0F;
    assets::Bone engine = root; engine.name = "Engine"; engine.parent = 1; engine.relative_transform[7] = -3.5F;
    assets::Bone latent = root; latent.name = "Latent"; latent.parent = 0; latent.visible = false;
    model.bones = {root, hull, engine, latent};
    assets::Mesh mesh;
    mesh.name = "Hull_Mesh"; mesh.bone = 1;
    mesh.bounds_min = {-1, -1, -1}; mesh.bounds_max = {1, 1, 1};
    assets::Submesh submesh;
    submesh.vertices.resize(3);
    submesh.vertices[1].position = {1, 0, 0};
    submesh.vertices[2].position = {0, 1, 0};
    submesh.indices = {0, 1, 2};
    mesh.submeshes.push_back(submesh);
    assets::Mesh charged;
    charged.name = "Charged"; charged.bone = 1;
    model.meshes = {mesh, charged};
    model.proxies = {{"eawr_metadata_engine", 2, true, false}, {"eawr_metadata_latent", 3, true, false}};
    return model;
}

} // namespace

int main() {
    const assets::Model full = make_model();
    const assets::Model stripped = assets::geometry_stripped_copy(full);

    expect(!assets::geometry_stripped(full), "a decoded model is not marked stripped");
    expect(assets::geometry_stripped(stripped), "the metadata copy is marked stripped (the renderer upload refuses it)");
    expect(stripped.source.logical_path == full.source.logical_path && stripped.source.source_id == full.source.source_id,
           "the metadata copy keeps the source identity the planners compare");
    bool no_geometry = stripped.meshes.size() == full.meshes.size() && stripped.lights.empty() && stripped.dazzles.empty();
    for (const assets::Mesh& mesh : stripped.meshes) no_geometry = no_geometry && mesh.submeshes.empty();
    expect(no_geometry, "the metadata copy holds no submeshes, lights or dazzles");
    bool same_shape = stripped.bones.size() == full.bones.size() && stripped.proxies.size() == full.proxies.size();
    for (std::size_t i = 0; same_shape && i < full.bones.size(); ++i) {
        same_shape = stripped.bones[i].name == full.bones[i].name && stripped.bones[i].parent == full.bones[i].parent
            && stripped.bones[i].visible == full.bones[i].visible
            && stripped.bones[i].relative_transform == full.bones[i].relative_transform;
    }
    for (std::size_t i = 0; same_shape && i < full.proxies.size(); ++i) {
        same_shape = stripped.proxies[i].name == full.proxies[i].name && stripped.proxies[i].bone == full.proxies[i].bone
            && stripped.proxies[i].visible == full.proxies[i].visible;
    }
    for (std::size_t i = 0; same_shape && i < full.meshes.size(); ++i) {
        same_shape = stripped.meshes[i].name == full.meshes[i].name && stripped.meshes[i].bone == full.meshes[i].bone;
    }
    expect(same_shape, "bones, mesh names/bones and proxies are copied unchanged");

    auto full_player = playback::Player::create(full);
    auto stripped_player = playback::Player::create(stripped);
    expect(bool(full_player) && bool(stripped_player), "both models build a player");
    if (!full_player || !stripped_player) return 1;

    // The map-effect plan reads the model and the bind-pose bone frames: records must match field for field.
    scene::Placement placement;
    placement.map_logical_path = "data/art/maps/eawr_metadata.ted";
    placement.scene_ordinal = 1;
    placement.record_ordinal = 1;
    placement.model_path = full.source.logical_path;
    scene::Transform transform;
    transform.matrix = math::identity_matrix();
    placement.transform = transform;
    placement.effects = {{"eawr_metadata_engine", "data/art/models/eawr_metadata_engine.alo", 2, false},
                         {"eawr_metadata_latent", "data/art/models/eawr_metadata_latent.alo", 3, false}};
    const std::vector<particles::EffectEvidence> effects{{particles::EffectKind::particle, 16},
                                                         {particles::EffectKind::particle, 16}};
    const std::vector<std::uint8_t> hidden(2, 0U), shown(2, 0U);
    std::vector<std::optional<math::Mat3x4>> full_frames(full.bones.size()), stripped_frames(full.bones.size());
    const auto full_pose = full_player.value().sample({});
    const auto stripped_pose = stripped_player.value().sample({});
    expect(bool(full_pose) && bool(stripped_pose), "both bind poses sample");
    if (!full_pose || !stripped_pose) return 1;
    bool same_frames = true;
    for (std::size_t i = 0; i < full_frames.size(); ++i) {
        full_frames[i] = particles::fixed_model_frame(full_pose.value().bones[i].model_asset, test_fixed);
        stripped_frames[i] = particles::fixed_model_frame(stripped_pose.value().bones[i].model_asset, test_fixed);
        same_frames = same_frames && full_frames[i].has_value() == stripped_frames[i].has_value()
            && (!full_frames[i] || full_frames[i]->rows == stripped_frames[i]->rows);
    }
    expect(same_frames, "bind-pose emitter bone frames are identical");
    const auto plan = [&](const assets::Model& model, const std::vector<std::optional<math::Mat3x4>>& frames) {
        const particles::MapEffectPlacementInput input{&model, &placement, frames, effects,
            particles::VisibilityEvidence::bind_pose, std::nullopt, std::nullopt, hidden, shown};
        return particles::plan_map_effects(std::span(&input, 1), 7U, 1000U);
    };
    const auto a = plan(full, full_frames);
    const auto b = plan(stripped, stripped_frames);
    bool same = a.records.size() == b.records.size() && !a.records.empty();
    for (std::size_t i = 0; same && i < a.records.size(); ++i) {
        const auto& x = a.records[i];
        const auto& y = b.records[i];
        same = x.status == y.status && x.cause == y.cause && x.detail == y.detail && x.proxy_name == y.proxy_name
            && x.proxy_ordinal == y.proxy_ordinal && x.model_logical_path == y.model_logical_path
            && x.effect_logical_path == y.effect_logical_path && x.seed == y.seed && x.capacity == y.capacity
            && x.emitter_frame.has_value() == y.emitter_frame.has_value()
            && (!x.emitter_frame || x.emitter_frame->rows == y.emitter_frame->rows);
    }
    expect(same, "plan_map_effects gives identical records from the metadata copy");

    // Mesh bindings need geometry: the metadata copy cannot supply it, the decoded model can. UnitEmitters binds
    // from a transient reload and keeps the result per (path, proxy).
    const auto from_full = particles::bind_proxy_mesh(full, std::size_t{0});
    expect(bool(from_full) && !from_full.value().binding.geometry.submeshes.empty(),
           "the decoded model binds the proxy mesh with geometry");
    const auto from_stripped = particles::bind_proxy_mesh(stripped, std::size_t{0});
    expect(bool(from_stripped) && from_stripped.value().binding.geometry.submeshes.empty(),
           "the metadata copy binds with no geometry");

    return failures == 0 ? 0 : 1;
}
