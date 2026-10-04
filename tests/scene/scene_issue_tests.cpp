#include "scene_test_support.hpp"

namespace eawr::tests::scene_tests {

void scene_issue_contracts(const TempTree& tree, const eawr::data::Catalog& catalog, const std::vector<Record>& records,
                           const eawr::core::Result<eawr::assets::Map>& map, const eawr::scene::BuildInput& input,
                           const eawr::scene::Scene& scene, const std::string& serial_bytes) {
    expect(scene.placements.size() == records.size(), "the scene keeps every placement in its denominator");
    if (scene.placements.size() != records.size()) return;
    const auto& p = scene.placements;

    for (std::size_t index = 0; index < p.size(); ++index) {
        expect(p[index].record_ordinal == index && p[index].scene_ordinal == index, "scene order is record order");
        expect(p[index].entity_id == index + 1U, "entity ids follow scene order");
    }

    // Stable asset IDs: the distinct loaded models in byte order.
    expect(scene.assets.size() == 3, "three distinct models load");
    if (scene.assets.size() == 3) {
        expect(scene.assets[0].logical_path == "data/art/models/eawr_scene_odd.alo" && scene.assets[0].asset_id == 1,
               "asset 1 is the lexically first model");
        expect(scene.assets[1].logical_path == "data/art/models/eawr_scene_plain.alo" && scene.assets[1].asset_id == 2,
               "asset 2 is the second model");
        expect(scene.assets[2].logical_path == "data/art/models/eawr_scene_prop.alo" && scene.assets[2].asset_id == 3,
               "asset 3 is the third model; the authored mixed-case name folds");
    }

    expect(p[0].resolved() && p[0].drawable(), "a plain placement fully resolves");
    expect(p[0].asset_id == 2 && p[0].surfaces.size() == 2, "the collision mesh is not a surface");
    expect(p[0].model_provenance.tag == "Model_Name", "the model records the tag it came from");
    expect(p[0].transform && p[0].transform->position_raw[0] == 40 * Fixed::scale
               && p[0].transform->yaw_degrees_raw == 90 * Fixed::scale, "position and yaw convert to Q24");
    expect(!p[0].scale_declared && p[0].scale_raw == Fixed::scale, "an undeclared scale is unit and marked undeclared");
    expect(p[0].idle_animation_status == "none_found", "no idle clip is invented");
    expect(!p[0].capture_point && p[1].capture_point && p[2].capture_point
               && p[1].capture_state == eawr::scene::Placement::CaptureState::uncaptured
               && p[1].capture_owner_faction.empty(),
           "effective CAPTURE_POINT behavior marks inherited objects without treating TED editor owner as capture state");
    expect(p[0].owner_player == 1 && p[0].owner_faction == "EAWR_BLUE" && p[0].team_colour_status == "faction_colour"
               && p[0].team_colour == std::array<std::uint8_t, 3>{10, 20, 200},
           "player 1 is the second loaded faction and takes its Color");
    expect(p[0].team_colour_provenance.tag == "Color" && p[0].team_colour_provenance.source_object_id == "EAWR_BLUE"
               && p[0].team_colour_provenance.logical_path == "data/xml/eawr_scene_factions.xml"
               && p[0].team_colour_provenance.line > 0, "the team colour records its XML provenance");
    expect(p[1].owner_faction == "EAWR_RED" && p[1].team_colour == std::array<std::uint8_t, 3>{200, 10, 20},
           "a four-component Color with spaces gives its first three components");
    expect(p[2].owner_faction == "EAWR_BARE" && p[2].team_colour_status == "colour_undeclared" && !p[2].team_colour,
           "a faction without Color is an explicit status, not a guessed colour");
    expect(p[7].owner_faction == "EAWR_BROKEN" && p[7].team_colour_status == "colour_invalid" && !p[7].team_colour,
           "an out-of-range Color component is invalid");
    expect(p[5].owner_player == 9 && p[5].owner_faction.empty() && p[5].team_colour_status == "owner_unmapped",
           "an index past the loaded factions is unmapped");
    expect(p[4].owner_player == -1 && p[4].team_colour_status == "owner_unmapped", "a negative owner is unmapped");
    expect(!p[3].owner_player && p[3].team_colour_status == "owner_absent" && !p[3].team_colour,
           "a record without mini 2 has no owner");
    expect(eawr::scene::faction_order(catalog)
               == std::vector<std::string>{"EAWR_RED", "EAWR_BLUE", "EAWR_BARE", "EAWR_BROKEN"},
           "factions are ordered as the catalog loads them");

    expect(p[1].resolved() && p[1].drawable(), "a placement with a resolved proxy effect resolves");
    expect(p[1].model_provenance.tag == "Land_Model_Name", "a land map prefers Land_Model_Name");
    expect(p[1].scale_declared && p[1].scale_raw == 3 * Fixed::scale / 2, "Scale_Factor reaches the transform");
    expect(p[1].scale_provenance.logical_path == "data/xml/eawr_scene_objects.xml"
               && p[1].scale_provenance.source_object_id == "EAWR_SCENE_PROP", "scale records its XML provenance");
    expect(p[1].transform && p[1].transform->matrix.rows[2][2].raw() == 3 * Fixed::scale / 2
               && p[1].transform->matrix.rows[1][3].raw() == 21 * (Fixed::scale / 2), "scale and translation land in the matrix");
    expect(p[1].effects.size() == 2 && p[1].effects[0].resolved == "data/art/models/eawr_scene_smoke.alo"
               && !p[1].effects[0].alternate_suffix_removed, "an attached proxy effect resolves literally first");
    expect(p[1].effects.size() == 2 && p[1].effects[1].resolved == "data/art/models/eawr_scene_smoke.alo"
               && p[1].effects[1].alternate_suffix_removed, "an _ALT<n> proxy resolves by its effect name and says so");
    expect(p[1].idle_animation == "data/art/models/eawr_scene_prop_idle_00.ala"
               && p[1].idle_animation_status == "corpus_naming_observed", "an idle clip is observed by corpus naming");
    expect(p[1].surfaces.size() == 1 && p[1].surfaces[0].textures.size() == 1
               && p[1].surfaces[0].textures[0].resolved == "data/art/textures/eawr_scene_prop.dds",
           "a .tga-authored texture resolves by stem");

    expect(p[2].resolved() && p[2].asset_id == 3 && p[2].scale_raw == 3 * Fixed::scale / 2
               && p[2].scale_provenance.source_object_id == "EAWR_SCENE_PROP",
           "a variant inherits model and scale with the base as provenance");

    expect(has(p[3], Cause::crc_missing) && !p[3].drawable() && !p[3].transform, "a missing CRC blocks drawing");
    expect(has(p[4], Cause::crc_absent), "an absent CRC is its own cause");
    expect(has(p[5], Cause::model_undeclared, "EAWR_SCENE_NO_MODEL"), "an undeclared model is its own cause");
    expect(has(p[6], Cause::model_not_in_vfs, "eawr_scene_lost.alo"), "a model that probes to nothing is named");
    expect(has(p[7], Cause::shader_unsupported, "EawrUnlisted.fx") && has(p[7], Cause::texture_unresolved)
               && has(p[7], Cause::effect_unresolved, "eawr_scene_gone"), "partial causes are all counted");
    expect(has(p[7], Cause::effect_unresolved, "eawr_scene_smoke_ALTx"), "only a numeric _ALT suffix is removed");
    expect(!has(p[7], Cause::shader_unsupported, "NeverDrawn.fx"), "an invisible mesh's shader is not counted");
    expect(p[7].drawable() && !p[7].resolved(), "a partially unsupported model is drawn but unresolved");
    expect(has(p[8], Cause::position_absent) && !p[8].drawable(), "an absent position blocks drawing");
    expect(!has(p[9], Cause::orientation_three_axis) && p[9].resolved() && p[9].drawable(),
           "a finite three-axis placement draws (R-ROT-01..03)");
    if (p[9].transform) {
        // Independent point rotations: fixed quarter turn, X roll, Y pitch, Z yaw.
        // This checks the decoder-to-scene path, not only the matrix helper.
        const double radians = std::acos(-1.0) / 180.0;
        const double cr = std::cos(10.0 * radians), sr = std::sin(10.0 * radians);
        const double cp = std::cos(20.0 * radians), sp = std::sin(20.0 * radians);
        const double cy = std::cos(30.0 * radians), sy = std::sin(30.0 * radians);
        for (std::size_t column = 0; column < 3; ++column) {
            const std::array<double, 3> v{column == 1 ? -1.0 : 0.0, column == 0 ? 1.0 : 0.0,
                column == 2 ? 1.0 : 0.0};
            const std::array<double, 3> rolled{v[0], cr * v[1] - sr * v[2], sr * v[1] + cr * v[2]};
            const std::array<double, 3> pitched{cp * rolled[0] + sp * rolled[2], rolled[1],
                -sp * rolled[0] + cp * rolled[2]};
            const std::array<double, 3> expected{cy * pitched[0] - sy * pitched[1],
                sy * pitched[0] + cy * pitched[1], pitched[2]};
            for (std::size_t row = 0; row < 3; ++row) {
                const double actual = static_cast<double>(p[9].transform->matrix.rows[row][column].raw()) / Fixed::scale;
                expect(std::abs(actual - expected[row]) < 1e-5, "placement matrix numerically matches R-ROT-01");
            }
        }
        expect(p[9].transform->yaw_degrees_raw == 30 * Fixed::scale, "presentation retains the unchanged yaw (R-ROT-04)");
    }
    expect(has(p[10], Cause::transform_nonfinite), "a NaN position is rejected, not repaired");
    expect(has(p[11], Cause::transform_overflow), "an out-of-range position is rejected");
    expect(has(p[12], Cause::scale_invalid, "-1") && !p[12].drawable(), "a non-positive scale is rejected");
    expect(has(p[13], Cause::orientation_absent) && !p[13].transform && !p[13].drawable(),
           "an absent orientation is a cause, never an identity rotation");
    expect(has(p[14], Cause::transform_nonfinite) && !p[14].transform && !p[14].drawable(),
           "a NaN roll is rejected, never presented as yaw-only");

    expect(scene.resolved_count() == 4, "four placements fully resolve");
    expect(scene.drawable_count() == 5, "five placements are drawable");
    expect(scene.count(Cause::shader_unsupported) == 1, "per-cause counts count placements");
    const auto groups = eawr::scene::issue_groups(scene);
    const auto unsupported = std::find_if(groups.begin(), groups.end(), [](const eawr::scene::IssueGroup& group) {
        return group.cause == Cause::shader_unsupported && group.detail == "EawrUnlisted.fx";
    });
    expect(unsupported != groups.end() && unsupported->object_id == p[7].object_id
               && unsupported->model_path == p[7].model_path && unsupported->count == 1,
           "issue groups retain object, model, shader and placement count without a renderer");
    expect(std::is_sorted(groups.begin(), groups.end(), [](const auto& left, const auto& right) {
        return std::tie(left.cause, left.object_id, left.model_path, left.detail)
            < std::tie(right.cause, right.object_id, right.model_path, right.detail);
    }), "issue groups have stable cause/object/model/detail order");
    const auto instances = scene.instances();
    expect(instances.size() == 5 && instances.front().entity_id == 1 && instances.front().asset_id == 2,
           "the simulation receives fixed transforms and stable ids for drawable placements only");

    // Determinism: rebuilding gives the same hash; permuting the records
    // handed to the builder, or the order assets are first touched, does not
    // change it; a changed transform does.
    const eawr::scene::Scene again = eawr::scene::build(input);
    expect(again.scene_sha256 == scene.scene_sha256 && scene.scene_sha256.size() == 64, "the scene hash is repeatable");
    eawr::assets::Map permuted = map.value();
    std::reverse(permuted.placements.begin(), permuted.placements.end());
    eawr::scene::BuildInput reordered = input;
    reordered.map = &permuted;
    expect(eawr::scene::build(reordered).scene_sha256 == scene.scene_sha256, "record order is restored by the builder");
    eawr::platform::ThreadWorkerAdapter parallel_four(4);
    auto reordered_parallel = eawr::scene::build(reordered, parallel_four);
    expect(reordered_parallel && eawr::scene::canonical_text(reordered_parallel.value().scene) == serial_bytes,
           "parallel build restores reversed source order");
    eawr::assets::Map moved = map.value();
    moved.placements.front().position->x = 41.0F;
    eawr::scene::BuildInput changed = input;
    changed.map = &moved;
    expect(eawr::scene::build(changed).scene_sha256 != scene.scene_sha256, "the hash covers transforms");
    eawr::assets::Map recoloured = map.value();
    for (auto& field : recoloured.placements.front().fields) {
        if (field.id == 2) field.bytes[0] = std::byte{0};
    }
    changed.map = &recoloured;
    const auto recoloured_scene = eawr::scene::build(changed);
    expect(recoloured_scene.placements.front().owner_faction == "EAWR_RED"
               && recoloured_scene.scene_sha256 != scene.scene_sha256, "the hash covers the owner");

    const std::string text = eawr::scene::canonical_text(scene);
    expect(text.find(tree.root.filename().string()) == std::string::npos, "the canonical scene names no host path");
    expect(text.find(":\\") == std::string::npos && text.find(":/") == std::string::npos, "no drive path in the scene");
    expect(text.rfind("eawr-static-scene 1\n", 0) == 0, "the canonical text is versioned");
    expect(text.find("\n team_colour faction_colour 1 EAWR_BLUE 10 20 200 Color EAWR_BLUE data/xml/eawr_scene_factions.xml ")
               != std::string::npos, "the canonical text carries owner, faction, colour and provenance");
    expect(text.find("\n team_colour owner_absent - - - - - - - - 0\n") != std::string::npos,
           "an absent owner is written explicitly");
}

} // namespace eawr::tests::scene_tests
