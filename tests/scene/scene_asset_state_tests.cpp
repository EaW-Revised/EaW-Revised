#include "scene_test_support.hpp"

namespace eawr::tests::scene_tests {

void selector_contracts() {
    expect(eawr::scene::legacy_selectors().size() == 14, "the selector table matches the accepted legacy list");
    const auto* alpha = eawr::scene::find_legacy_selector("MeshAlpha.fx");
    expect(alpha != nullptr && alpha->transparent && alpha->technique == "sph_t1",
           "MeshAlpha is the fixed transparent-phase pass");
    // The DX8 technique retail draws at the Highest shader detail, on stored values (#200).
    const auto* batch = eawr::scene::find_legacy_selector("BatchMeshGloss.fx");
    expect(batch != nullptr && !batch->transparent && batch->technique == "sph_t0" && batch->pass == "sph_t0_p0"
               && batch->stored_values, "BatchMeshGloss is the DX8 opaque-phase pass");
    const auto* batch_alpha = eawr::scene::find_legacy_selector("BatchMeshAlpha.fx");
    expect(batch_alpha != nullptr && batch_alpha->transparent && batch_alpha->technique == "sph_t0"
               && batch_alpha->pass == "sph_t0_p0" && batch_alpha->stored_values,
           "BatchMeshAlpha is the DX8 transparent-phase pass");
    const auto* alpha_gloss = eawr::scene::find_legacy_selector("MeshAlphaGloss.fx");
    expect(alpha_gloss != nullptr && alpha_gloss->transparent && alpha_gloss->technique == "sph_t0"
               && alpha_gloss->pass == "sph_t0_p0" && alpha_gloss->stored_values,
           "MeshAlphaGloss is the DX8 transparent-phase pass");
    const auto* additive = eawr::scene::find_legacy_selector("MeshAdditive.fx");
    expect(additive != nullptr && additive->transparent && additive->technique == "t0",
           "MeshAdditive is scene-admitted as its t0 transparent pass");
    const auto* tree = eawr::scene::find_legacy_selector("Tree.fx");
    expect(tree != nullptr && !tree->transparent && tree->technique == "sph_t1" && tree->pass == "sph_t1_p0",
           "Tree is the alpha-tested sph_t1 pass, drawn with opaque work");
    const auto* grass = eawr::scene::find_legacy_selector("Grass.fx");
    expect(grass != nullptr && grass->transparent && grass->technique == "sph_t0",
           "Grass is the blended sph_t0 pass");
    expect(eawr::scene::find_legacy_selector("MeshSolidColor.fx") == nullptr,
           "the editor-marker family stays outside the scene route");
    const auto* gloss_colorize = eawr::scene::find_legacy_selector("MeshGlossColorize.fx");
    expect(gloss_colorize != nullptr && !gloss_colorize->transparent && gloss_colorize->technique == "sph_t0"
               && gloss_colorize->pass == "sph_t0_p0" && !gloss_colorize->stored_values,
           "MeshGlossColorize (the laser pads, #80) is scene-admitted as its opaque sph_t0 pass");
    expect(eawr::scene::colorizes("MeshBumpColorize.fx") && eawr::scene::colorizes("rskinglosscolorize.FX")
               && eawr::scene::colorizes("MeshAlphaGloss.fx") && !eawr::scene::colorizes("MeshGloss.fx")
               && !eawr::scene::colorizes("Tree.fx"), "colorizing effects are the ones declaring Colorization");
    const auto statuses = eawr::scene::team_colour_statuses();
    expect(statuses.size() == 5 && statuses.front() == "faction_colour"
               && std::set<std::string_view>(statuses.begin(), statuses.end()).size() == 5,
           "team colour statuses are distinct");
    const auto binding = eawr::scene::colorization_binding({0, 255, 128});
    expect(binding.x == 0.0F && binding.y == 1.0F && std::fabs(binding.z - 0.2158605F) < 1.0e-6F && binding.w == 1.0F,
           "the Colorization binding is the team colour decoded to linear light, alpha 1");
    const auto* bump = eawr::scene::find_legacy_selector("MeshBumpColorize.fx");
    const auto* skin_bump = eawr::scene::find_legacy_selector("rskinbumpcolorize.fx");
    expect(bump != nullptr && bump->technique == "sph_t2" && bump->pass == "sph_t2_p0" && bump->stored_values
               && skin_bump != nullptr && skin_bump->technique == "sph_t2" && skin_bump->stored_values,
           "the bump colorize pair selects the Highest DX9 technique, which computes on stored values (#199)");
    const auto stored = eawr::scene::colorization_binding("MeshBumpColorize.fx", {0, 255, 128});
    expect(stored.x == 0.0F && stored.y == 1.0F && stored.z == 128.0F / 255.0F && stored.w == 1.0F,
           "a stored-value selector binds the team colour as the stored value");
    const auto linear = eawr::scene::colorization_binding("RSkinGlossColorize.fx", {0, 255, 128});
    expect(linear.x == binding.x && linear.y == binding.y && linear.z == binding.z
               && eawr::scene::colorization_binding("NoSuchEffect.fx", {0, 255, 128}).z == binding.z,
           "linear adapters and unknown programs keep the linear binding");
    const auto* gloss = eawr::scene::find_legacy_selector("meshgloss.FX");
    expect(gloss != nullptr && gloss->technique == "sph_t0", "selector lookup folds case");
    expect(eawr::scene::find_legacy_selector("TerrainRenderBump.fx") == nullptr, "terrain stays outside the legacy list");
    std::set<std::string_view> names;
    for (const Cause cause : eawr::scene::all_causes()) names.insert(eawr::scene::to_string(cause));
    expect(names.size() == eawr::scene::all_causes().size() && !names.contains("unknown"), "every cause has a unique name");
}

void static_mesh_state_contracts() {
    eawr::assets::Model construction;
    for (const std::string_view name : {
             "Body_ALT0", "Body_ALT1", "Body_ALT2", "Body_ALT3", "girder1x06", "Trim"}) {
        eawr::assets::Mesh mesh;
        mesh.name = name;
        mesh.visible = true;
        construction.meshes.push_back(std::move(mesh));
    }
    const auto named = [&](const std::string_view name) -> const eawr::assets::Mesh& {
        const auto found = std::find_if(construction.meshes.begin(), construction.meshes.end(),
            [name](const eawr::assets::Mesh& mesh) { return mesh.name == name; });
        return *found;
    };
    expect(eawr::scene::static_mesh_visible(construction, named("Body_ALT0")), "intact ALT0 is visible");
    for (const std::string_view state : {"Body_ALT1", "Body_ALT2", "Body_ALT3"}) {
        expect(!eawr::scene::static_mesh_visible(construction, named(state)), "only intact ALT0 state is visible");
    }
    expect(!eawr::scene::static_mesh_visible(construction, named("girder1x06")), "construction girder is hidden");
    expect(eawr::scene::static_mesh_visible(construction, named("Trim")), "unmarked trim remains visible");
    construction.meshes.front().visible = false;
    expect(!eawr::scene::static_mesh_visible(construction, construction.meshes.front()), "ALO hidden flag wins");
    construction.meshes.erase(std::find_if(construction.meshes.begin(), construction.meshes.end(),
        [](const eawr::assets::Mesh& mesh) { return mesh.name == "Body_ALT0"; }));
    expect(eawr::scene::static_mesh_visible(construction, named("girder1x06")),
           "a girder without an intact ALT state is not guessed to be construction scaffolding");
}

void uncaptured_capture_point_contracts() {
    eawr::assets::Model model;
    for (const auto& [name, visible] : {
             std::pair{"Root", true}, {"NeutralLamp", true}, {"RebelEmblem", true},
             {"EmpireEmblem", false}, {"Collision", false}}) {
        eawr::assets::Bone bone;
        bone.name = name;
        bone.visible = visible;
        bone.parent = -1;
        model.bones.push_back(std::move(bone));
    }
    for (std::int32_t index = 0; index < static_cast<std::int32_t>(model.bones.size()); ++index) {
        eawr::assets::Mesh mesh;
        mesh.name = model.bones[static_cast<std::size_t>(index)].name;
        mesh.bone = index;
        mesh.visible = true;
        model.meshes.push_back(std::move(mesh));
    }
    const auto clip = [&](const bool rebel, const bool empire) {
        eawr::assets::Animation result;
        for (const auto& [index, visible] : {std::pair{2U, rebel}, {3U, empire}}) {
            eawr::assets::AnimationTrack track;
            track.bone_index = index;
            track.bone_name = model.bones[index].name;
            eawr::assets::AnimationSample sample;
            sample.visible = visible;
            track.samples.push_back(sample);
            result.tracks.push_back(std::move(track));
        }
        return result;
    };
    const std::array clips{clip(true, false), clip(false, true)};
    const auto variant = eawr::scene::capture_variant_bones(model, clips);
    expect(variant.size() == model.bones.size() && variant[2] && variant[3]
               && !variant[0] && !variant[1] && !variant[4],
           "idle-clip visibility changes identify owner art without mesh names");
    expect(eawr::scene::uncaptured_mesh_visible(model, model.meshes[0], variant)
               && eawr::scene::uncaptured_mesh_visible(model, model.meshes[1], variant)
               && !eawr::scene::uncaptured_mesh_visible(model, model.meshes[2], variant)
               && !eawr::scene::uncaptured_mesh_visible(model, model.meshes[3], variant)
               && !eawr::scene::uncaptured_mesh_visible(model, model.meshes[4], variant),
           "uncaptured selection keeps neutral art and hides both owner variants and bind-hidden bones");
}

} // namespace eawr::tests::scene_tests
