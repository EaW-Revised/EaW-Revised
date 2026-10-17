#include "scene_test_support.hpp"

namespace eawr::tests::scene_tests {

namespace {
void missing_material_effect_contracts() {
    using eawr::scene::missing_material_effect;
    Assets assets;
    const auto access = assets.access();
    expect(!missing_material_effect(access, "MeshGloss.fx"), "a compiled effect prevents the missing-file route");
    expect(!missing_material_effect(access, "EawrUnlisted.fx"), "an existing unsupported effect is not missing");
    expect(missing_material_effect(access, "InventedMissing.fx"), "absence of both files selects the built-in material");
    expect(missing_material_effect(access, "MeshAdditive.fx"), "a known adapter cannot replace a missing file");
    expect(!missing_material_effect({}, "InventedMissing.fx"), "no existence provider is not evidence of absence");
    assets.files.insert("data/art/shaders/inventedmissing.fx");
    expect(!missing_material_effect(access, "INVENTEDMISSING.FXO"), "source presence also prevents the missing-file route");
    assets.files.insert("effects/with.dots/present.fxo");
    expect(!missing_material_effect(access, "Effects\\With.Dots\\Present.fx"), "lookup preserves directory dots");
    auto fixture = model({"MeshGloss.fx", "InventedAbsent.fx", "EawrUnlisted.fx"}, "absent_texture.dds", {});
    assets.models.emplace("data/art/models/missing_effect.alo", std::move(fixture));
    const auto surfaces = eawr::scene::construction_surfaces(access, "data/art/models/missing_effect.alo");
    expect(surfaces.size() == 3, "missing effects retain surface provenance");
    if (surfaces.size() != 3) return;
    expect(surfaces[0].supported && !surfaces[0].effect_missing, "an existing admitted material stays drawable");
    expect(surfaces[1].supported && surfaces[1].effect_missing && surfaces[1].textures.empty()
               && eawr::scene::material_selector(surfaces[1]) == &eawr::scene::missing_effect_selector(),
        "a missing effect selects the built-in material without unused texture inputs");
    expect(!surfaces[2].supported && !surfaces[2].effect_missing && !surfaces[2].textures.empty(),
        "an existing unsupported effect retains its unsupported status and texture evidence");
}
} // namespace

void selector_contracts() {
    expect(eawr::scene::legacy_selectors().size() == 22, "the selector table matches the accepted legacy list");
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
    const auto* glow = eawr::scene::find_legacy_selector("meshbumpspecglowcolorize.FX");
    expect(glow != nullptr && !glow->transparent && glow->technique == "sph_t2"
               && glow->pass == "sph_t2_p0" && glow->stored_values
               && eawr::scene::colorizes("MeshBumpSpecGlowColorize.fx"),
           "specular/glow colorize uses the stored-value opaque DX9 scene route");
    const auto* skin_additive = eawr::scene::find_legacy_selector("rskinadditive.FX");
    const auto* skin_alpha = eawr::scene::find_legacy_selector("RSkinAlpha.fx");
    expect(skin_additive != nullptr && skin_additive->transparent && skin_additive->technique == "t0"
               && skin_additive->pass == "t0_p0", "skinned additive selects its transparent programmable pass");
    expect(skin_alpha != nullptr && skin_alpha->transparent && skin_alpha->technique == "sph_t0"
               && skin_alpha->pass == "sph_t0_p0" && skin_alpha->stored_values,
           "skinned alpha selects its stored-value programmable transparent pass");
    const auto* shield = eawr::scene::find_legacy_selector("meshshield.FXO");
    const auto* farseer = eawr::scene::find_legacy_selector("MeshShieldFarseer.fxo");
    expect(shield != nullptr && shield->transparent && shield->technique == "t0"
               && farseer != nullptr && farseer->transparent && farseer->technique == "sph_t3",
           "qualified compiled shields select their own additive passes");
    expect(eawr::scene::find_legacy_selector("MeshShield.fx") == nullptr
               && eawr::scene::find_legacy_selector("MeshShieldFarseer.fx") == nullptr,
           "generic compiled shield admission does not change existing uncompiled selectors");
    const auto* deathstar = eawr::scene::find_legacy_selector("planet_deathstar.FX");
    expect(deathstar != nullptr && !deathstar->transparent && deathstar->stored_values
               && deathstar->technique == "sph_t3", "Death Star is the stored-value opaque highest-detail family");
    const auto* vertex_additive = eawr::scene::find_legacy_selector("MeshAdditiveVColor.fx");
    expect(vertex_additive != nullptr && vertex_additive->transparent && vertex_additive->technique == "t0"
               && vertex_additive->pass == "t0_p0",
           "vertex-colour additive is scene-admitted as its sourced t0 pass");
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
    for (const std::string_view alias : {"MeshGloss.fxo", "meshgloss.FXO"}) {
        expect(eawr::scene::find_legacy_selector(alias) == gloss,
               "compiled gloss uses the existing canonical selector and pass");
    }
    for (const std::string_view alias : {"MeshAdditive.fxo", "meshadditive.FXO"}) {
        expect(eawr::scene::find_legacy_selector(alias) == additive,
               "compiled additive uses the existing canonical selector and pass");
    }
    const auto* doubled = eawr::scene::find_legacy_selector("MeshAdditive2x.fx");
    expect(doubled != nullptr && doubled->transparent && doubled->technique == "t0"
               && doubled->pass == "t0_p0", "doubled additive has its own transparent selector");
    for (const std::string_view alias : {"MeshAdditive2x.fxo", "meshadditive2x.FXO"}) {
        expect(eawr::scene::find_legacy_selector(alias) == doubled,
               "compiled doubled additive selects the qualified family");
    }
    for (const std::string_view unsupported : {"MeshGloss.fxoo", "MeshAdditive.fxo.extra",
             "MeshGlossForStarfields.fxo", "MeshAdditive2x.fxoo"}) {
        expect(eawr::scene::find_legacy_selector(unsupported) == nullptr,
               "unqualified generic selectors are not admitted by suffix or environment support");
    }
    expect(eawr::scene::find_legacy_selector("TerrainRenderBump.fx") == nullptr, "terrain stays outside the legacy list");
    std::set<std::string_view> names;
    for (const Cause cause : eawr::scene::all_causes()) names.insert(eawr::scene::to_string(cause));
    expect(names.size() == eawr::scene::all_causes().size() && !names.contains("unknown"), "every cause has a unique name");
}

void static_mesh_state_contracts() {
    missing_material_effect_contracts();
    {
        eawr::assets::Model attached;
        attached.proxies = {{"Glow", 2, true, false}, {"Glow_ALT12", 3, true, false},
                            {"Glow_ALT1x", 4, true, false}, {"Glow_ALT0", 5, true, false}};
        eawr::scene::AssetAccess access;
        access.exists = [](const std::string_view path) {
            return path == "data/art/models/glow.alo" || path == "data/art/models/glow_alt0.alo";
        };
        const auto effects = eawr::scene::model_proxy_effects(access, attached);
        expect(effects.size() == 4 && effects[0].bone == 2 && effects[1].bone == 3,
               "runtime attachment references preserve ordinal and bone identity");
        expect(effects[0].resolved == "data/art/models/glow.alo"
            && effects[1].resolved == effects[0].resolved && effects[1].alternate_suffix_removed,
               "runtime attached models use the scene's trailing ALT-number fallback");
        expect(effects[2].resolved.empty() && !effects[2].alternate_suffix_removed,
               "malformed ALT suffix stays unresolved");
        expect(effects[3].resolved == "data/art/models/glow_alt0.alo" && !effects[3].alternate_suffix_removed,
               "literal attached effect wins over fallback");
    }
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
    for (auto& mesh : construction.meshes) {
        eawr::assets::Submesh submesh;
        submesh.shader = "MeshGloss.fx";
        mesh.submeshes.push_back(std::move(submesh));
    }
    Assets assets;
    assets.models.emplace("data/art/models/construction.alo", construction);
    const auto surfaces = eawr::scene::construction_surfaces(assets.access(), "data/art/models/construction.alo");
    expect(surfaces.size() == 5, "WBP-17 runtime surfaces retain scaffold and all visible damage alternates");
    expect(std::any_of(surfaces.begin(), surfaces.end(), [](const auto& surface) {
        return eawr::scene::mesh_alternate(surface.mesh_name) == 3U;
    }), "the damaged construction state survives scene preparation");
    expect(!eawr::scene::mesh_alternate("Trim") && !eawr::scene::mesh_alternate("Body_ALTx"),
        "unmarked and malformed surfaces do not invent alternates");
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
