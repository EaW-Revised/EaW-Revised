#include "space_test_support.hpp"

namespace eawr_space_test {

void test_environment_selection() {
    const auto selected = space::select_environment(space_map());
    expect(selected.status == space::PlanStatus::ready, "a kind-2 map with a declared primary sky is selected");
    expect(selected.primary_sky == "EAWR_SPACE_PRIMARY_SKY", "the primary sky id is environment 0's 0x19 value");
    expect(selected.environment_index == 0, "environment 0 is the slice policy");
    bool secondary = false, cloud = false, planet = false, nebula = false;
    for (const auto& item : selected.not_rendered) {
        expect(!item.cause.empty(), "every not-rendered component carries a cause: " + item.component);
        expect(item.status != "unused", "no environment component is accepted as unused");
        if (item.component == "secondary_sky") secondary = item.declared == "EAWR_SPACE_SECONDARY_SKY"
            && item.status == "declared_not_rendered";
        if (item.component == "cloud_texture") cloud = item.status == "declared_not_rendered";
        if (item.component == "planet") planet = item.status == "unexamined_blocked";
        if (item.component == "nebula") nebula = item.status == "unexamined_blocked";
    }
    expect(secondary && cloud, "declared secondary sky and cloud texture are recorded as not rendered");
    expect(planet && nebula, "Planet and Nebula remain blocked, never unused");

    auto land = space_map();
    land.kind = eawr::assets::MapKind::land;
    expect(space::select_environment(land).status == space::PlanStatus::not_space_map, "a land map is not planned here");
    auto unknown = space_map();
    unknown.kind.reset();
    expect(space::select_environment(unknown).status == space::PlanStatus::map_kind_unknown, "an unknown kind fails");
    auto terrain = space_map();
    terrain.terrain = eawr::assets::Terrain{};
    expect(space::select_environment(terrain).status == space::PlanStatus::terrain_present,
           "a space map carrying terrain is a structure disagreement");
    auto incomplete = space_map();
    incomplete.semantic_complete = false;
    expect(space::select_environment(incomplete).status == space::PlanStatus::map_not_semantically_complete,
           "a raw-only space map is not planned");
    auto empty = space_map();
    empty.environments.clear();
    expect(space::select_environment(empty).status == space::PlanStatus::environment_absent, "no environment fails");
    auto undeclared = space_map();
    undeclared.environments.front().primary_sky = "  ";
    undeclared.environments.push_back(undeclared.environments.front());
    const auto blank = space::select_environment(undeclared);
    expect(blank.status == space::PlanStatus::primary_sky_undeclared, "a blank primary sky is undeclared");
    bool second = false;
    for (const auto& item : blank.not_rendered) second = second || (item.component == "environment_1" && item.status == "not_selected");
    expect(second, "an additional environment is reported as not selected");
}

void test_model_selection() {
    expect(space::select_model(nullptr, true).status == space::PlanStatus::object_not_in_catalog,
           "a sky id absent from a loaded catalog");
    expect(space::select_model(nullptr, false).status == space::PlanStatus::catalog_unavailable,
           "a sky id with no catalog is distinct from an absent declaration");
    eawr::assets::ObjectTypeRef type;
    type.model_name = "generic.alo";
    auto generic = space::select_model(&type, true);
    expect(generic.status == space::PlanStatus::ready && generic.declared_tag == "Model_Name", "generic model tag");
    type.land_model_name = "land.alo";
    type.space_model_name = "space.alo";
    auto spaced = space::select_model(&type, true);
    expect(spaced.declared_name == "space.alo" && spaced.declared_tag == "Space_Model_Name",
           "the existing fallback prefers the space model");
    eawr::assets::ObjectTypeRef none;
    expect(space::select_model(&none, true).status == space::PlanStatus::object_declares_no_model,
           "an object without a model tag");
}

void test_two_surface_plan() {
    Harness harness;
    const auto plan = harness.plan();
    expect(plan.status == space::PlanStatus::ready, "the synthetic two-surface sky plans: " + plan.detail);
    expect(plan.surfaces.size() == 2 && plan.accepted_count() == 2, "both surfaces are accepted");
    expect(plan.model.declared_tag == "Space_Model_Name", "the space model tag is used");
    expect(plan.model_logical_path == "data/art/models/eawr_space_sky.alo", "model identity is retained");
    expect(harness.texture_requests == std::vector<std::string>{"eawr_space_a.tga", "eawr_space_b.dds"},
           "each surface looks up its own BaseTexture in ordinal order");
    if (plan.surfaces.size() == 2) {
        const auto& a = plan.surfaces[0];
        const auto& b = plan.surfaces[1];
        expect(a.mesh_index == 0 && b.mesh_index == 1 && a.submesh_index == 0, "mesh/submesh ordinals are stable");
        expect(a.original_shader == qualified_shader, "original shader identity is kept");
        expect(a.texture_identity.logical_path != b.texture_identity.logical_path, "each surface has its own texture");
        expect(a.texture && b.texture && a.model && b.model, "accepted surfaces carry texture and geometry");
        if (a.model && b.model && a.texture && b.texture) {
            expect(a.model->meshes.size() == 1 && a.model->meshes.front().submeshes.size() == 1,
                   "one surface per upload model");
            expect(a.model->meshes.front().bone == -1 && a.model->bones.empty(),
                   "the verified identity chain is dropped, not flattened");
            const auto& vertex = a.model->meshes.front().submeshes.front().vertices.front();
            expect(vertex.position.x == 400.0F && vertex.position.y == -100.0F && vertex.position.z == 100.0F,
                   "surface geometry stays in the source basis (the renderer converts once)");
            expect(a.texture->mips.front().bytes[0] != b.texture->mips.front().bytes[0],
                   "the two uploads carry different texture pixels");
        }
    }
    expect(!plan.visibility, "no visibility provider means explicitly unbound");

    // Determinism: the same inputs give the same ordered plan.
    Harness again;
    const auto second = again.plan();
    expect(second.surfaces.size() == plan.surfaces.size() && second.status == plan.status, "the plan is deterministic");
}

void expect_rejected(Harness& harness, const space::SurfaceStatus cause, const std::string& label) {
    const auto plan = harness.plan();
    expect(plan.status == space::PlanStatus::surface_rejected, label + ": the plan is not ready");
    bool found = false;
    for (const auto& surface : plan.surfaces) {
        for (const auto item : surface.causes) found = found || item == cause;
        if (surface.status != space::SurfaceStatus::accepted) {
            expect(!surface.texture && !surface.model, label + ": a rejected surface has no upload payload");
        }
    }
    expect(found, label + ": expected cause " + std::string(space::to_string(cause)));
}

namespace {

void surface_parameter_rejections() {
    {
        Harness harness;
        harness.model.meshes[1].visible = false;
        const auto plan = harness.plan();
        expect(plan.status == space::PlanStatus::ready && plan.accepted_count() == 1,
               "a hidden mesh is skipped and the remaining surface still plans");
        expect(plan.surfaces[1].status == space::SurfaceStatus::mesh_invisible, "hidden mesh status");
        harness.model.meshes[0].visible = false;
        expect(harness.plan().status == space::PlanStatus::no_drawable_surface, "all hidden is not drawable");
    }
    {
        Harness harness;
        harness.model.meshes[0].submeshes[0].parameters.clear();
        expect_rejected(harness, space::SurfaceStatus::base_texture_missing, "missing BaseTexture");
    }
    {
        Harness harness;
        auto& parameters = harness.model.meshes[0].submeshes[0].parameters;
        parameters.push_back(parameters.front());
        expect_rejected(harness, space::SurfaceStatus::base_texture_duplicate, "duplicate BaseTexture");
    }
    {
        Harness harness;
        harness.model.meshes[0].submeshes[0].parameters.front() =
            {"BaseTexture", eawr::assets::ParameterKind::scalar, 1.0F};
        expect_rejected(harness, space::SurfaceStatus::base_texture_wrong_type, "wrong-type BaseTexture");
    }
    {
        Harness harness;
        harness.model.meshes[1].submeshes[0].parameters.push_back(
            {"CloudTexture", eawr::assets::ParameterKind::texture, std::string("eawr_space_c.tga")});
        expect_rejected(harness, space::SurfaceStatus::multitexture_required, "second texture");
    }
    {
        Harness harness;
        harness.model.meshes[1].submeshes[0].parameters.push_back(
            {"UVScrollRate", eawr::assets::ParameterKind::scalar, 0.25F});
        expect_rejected(harness, space::SurfaceStatus::unconsumed_parameter, "unreviewed scalar input");
    }
    {
        Harness harness;
        harness.model.meshes[0].submeshes[0].shader = "Skydome.fx";
        expect_rejected(harness, space::SurfaceStatus::shader_not_qualified, "an original name alone");
        harness.model.meshes[0].submeshes[0].shader = "Planet.fx";
        expect_rejected(harness, space::SurfaceStatus::shader_not_qualified, "Planet is never admitted by name");
    }
    {
        Harness harness;
        harness.model.meshes[0].submeshes[0].skin_bones = {0};
        expect_rejected(harness, space::SurfaceStatus::skinning_unsupported, "palette skinning");
    }
}

void surface_rigid_chain_cases() {
    {
        Harness harness;
        harness.model.bones[0].relative_transform[3] = 5.0F;
        const auto plan = harness.plan();
        expect(plan.status == space::PlanStatus::ready && plan.surfaces[0].model.has_value(),
               "translated bone is accepted");
        if (plan.surfaces[0].model) {
            const auto& before = harness.model.meshes[0].submeshes[0].vertices[0].position;
            const auto& after = plan.surfaces[0].model->meshes[0].submeshes[0].vertices[0].position;
            expect(std::abs(after.x - before.x - 5.0F) < 0.001F && after.y == before.y && after.z == before.z,
                   "translation is baked in source basis");
        }
    }
    {
        Harness harness;
        auto& root = harness.model.bones[0];
        root.relative_transform[3] = 10.0F;
        root.relative_transform[7] = -4.0F;
        root.relative_transform[11] = 2.0F;
        eawr::assets::Bone child;
        child.name = "Sky";
        child.parent = 0;
        child.relative_transform = {0, -1, 0, 3, 1, 0, 0, 6, 0, 0, 1, -5};
        harness.model.bones.push_back(child);
        for (auto& mesh : harness.model.meshes) {
            mesh.bone = 1;
            for (auto& vertex : mesh.submeshes[0].vertices) vertex.normal = {0.6F, 0.8F, 0};
        }
        const auto first = harness.plan();
        const auto second = harness.plan();
        expect(first.status == space::PlanStatus::ready && second.status == space::PlanStatus::ready,
               "two-bone proper rigid chain is accepted repeatably");
        for (std::size_t i = 0; i < first.surfaces.size(); ++i) {
            if (!first.surfaces[i].model || !second.surfaces[i].model) continue;
            const auto& original = harness.model.meshes[i].submeshes[0].vertices;
            const auto& baked = first.surfaces[i].model->meshes[0].submeshes[0].vertices;
            const auto& again = second.surfaces[i].model->meshes[0].submeshes[0].vertices;
            expect(first.surfaces[i].model->bones.empty() && first.surfaces[i].model->meshes[0].bone == -1,
                   "baked surface carries no live bone");
            for (std::size_t j = 0; j < original.size(); ++j) {
                // Animation's bind-pose convention: parent * child * local point.
                const Vec3f reference{13.0F - original[j].position.y,
                                      2.0F + original[j].position.x,
                                      -3.0F + original[j].position.z};
                const auto& got = baked[j].position;
                expect(std::abs(got.x - reference.x) < 0.001F
                           && std::abs(got.y - reference.y) < 0.001F
                           && std::abs(got.z - reference.z) < 0.001F,
                       "baked position matches bind-pose parent multiplication");
                expect(std::abs(baked[j].normal.x + 0.8F) < 0.001F
                           && std::abs(baked[j].normal.y - 0.6F) < 0.001F,
                       "baked normal receives rotation without translation");
                expect(got.x == again[j].position.x && got.y == again[j].position.y
                           && got.z == again[j].position.z,
                       "rigid bake is deterministic");
            }
        }
        expect(harness.model.meshes[0].bone == 1, "source model is unchanged");
    }
}

void surface_hierarchy_rejections() {
    for (const auto& [label, transform] : std::vector<std::pair<std::string, std::array<float, 12>>>{
             {"scale", {2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}},
             {"shear", {1, 0.2F, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}},
             {"reflection", {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}},
             {"nonfinite", {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, std::numeric_limits<float>::infinity()}}}) {
        Harness harness;
        harness.model.bones[0].relative_transform = transform;
        expect_rejected(harness, space::SurfaceStatus::hierarchy_unsupported, label);
    }
    {
        Harness harness;
        harness.model.bones[0].parent = 0;
        expect_rejected(harness, space::SurfaceStatus::hierarchy_unsupported, "cyclic bone chain");
        harness.model.bones[0].parent = -2;
        expect_rejected(harness, space::SurfaceStatus::hierarchy_unsupported, "invalid negative parent");
    }
    {
        Harness harness;
        harness.model.bones[0].billboard = 1;
        expect_rejected(harness, space::SurfaceStatus::hierarchy_unsupported, "billboard bone");
        expect(harness.plan().surfaces[0].detail.find("billboard") != std::string::npos,
               "the hierarchy cause names the offending bone");
    }
    {
        Harness harness;
        harness.model.meshes[0].bone = 7;
        expect_rejected(harness, space::SurfaceStatus::hierarchy_unsupported, "bone index out of range");
    }
}

void surface_bone_visibility_cases() {
    // Bone visibility is stored apart from the transform. With no reviewed
    // inheritance rule, a hidden bone anywhere on an identity chain must not
    // be dropped into an unconditionally visible boneless upload.
    {
        Harness harness;
        harness.model.bones[0].visible = false;
        const auto plan = harness.plan();
        expect(plan.status != space::PlanStatus::ready && plan.accepted_count() == 0,
               "a hidden attached identity bone is not an accepted visible upload");
        expect_rejected(harness, space::SurfaceStatus::bone_visibility_unsupported, "hidden attached bone");
        expect(plan.surfaces.size() == 2 && plan.surfaces[0].status == space::SurfaceStatus::bone_visibility_unsupported
                   && plan.surfaces[0].detail.find("'Root'") != std::string::npos,
               "the hidden-bone cause is the surface status and names the bone");
    }
    {
        Harness harness;
        eawr::assets::Bone child;
        child.name = "Sky";
        child.parent = 0;
        child.relative_transform = harness.model.bones[0].relative_transform;
        harness.model.bones.push_back(child);
        harness.model.meshes[0].bone = 1;
        harness.model.meshes[1].bone = 1;
        expect(harness.plan().status == space::PlanStatus::ready,
               "a visible two-bone identity chain still plans (control for the hidden-ancestor case)");
        harness.model.bones[0].visible = false;
        const auto plan = harness.plan();
        expect(plan.status != space::PlanStatus::ready && plan.accepted_count() == 0,
               "a hidden identity ancestor is not an accepted visible upload");
        expect_rejected(harness, space::SurfaceStatus::bone_visibility_unsupported, "hidden ancestor bone");
        expect(plan.surfaces.size() == 2 && plan.surfaces[1].detail.find("bone 0 'Root'") != std::string::npos,
               "the hidden-ancestor cause names the ancestor, not the visible attached bone");
        // A hidden transformed chain is still rejected for visibility.
        harness.model.bones[1].relative_transform[3] = 5.0F;
        const auto both = harness.plan();
        bool visibility = false;
        for (const auto cause : both.surfaces[0].causes) {
            visibility = visibility || cause == space::SurfaceStatus::bone_visibility_unsupported;
        }
        expect(visibility, "a hidden, translated chain remains rejected");
    }
}

void surface_input_failure_cases() {
    {
        Harness harness;
        harness.model.meshes[0].submeshes[0].indices.back() = 99;
        expect_rejected(harness, space::SurfaceStatus::geometry_invalid, "index out of range");
    }
    {
        Harness harness;
        harness.missing_texture = "eawr_space_b.dds";
        expect_rejected(harness, space::SurfaceStatus::texture_not_in_vfs, "missing texture is not grey-passed");
        const auto plan = harness.plan();
        expect(plan.surfaces[0].status == space::SurfaceStatus::accepted,
               "only the surface whose texture is missing is rejected");
    }
    {
        Harness harness;
        harness.undecodable_texture = "eawr_space_a.tga";
        expect_rejected(harness, space::SurfaceStatus::texture_failed_to_decode, "undecodable texture");
    }
    {
        Harness harness;
        harness.texture_format = eawr::assets::PixelFormat::a8;
        expect_rejected(harness, space::SurfaceStatus::texture_unsupported, "alpha-only texture");
    }
    {
        Harness harness;
        harness.model.meshes[0].submeshes[0].shader = "Nebula.fx";
        const auto plan = harness.plan();
        expect(plan.surfaces[0].texture_identity.logical_path == "data/art/textures/eawr_space_a.tga",
               "a rejected surface still pins its texture identity");
    }
    {
        Harness harness;
        harness.model_status = space::ModelLookup::Status::not_in_vfs;
        expect(harness.plan().status == space::PlanStatus::model_not_in_vfs, "missing model");
        harness.model_status = space::ModelLookup::Status::failed_to_load;
        expect(harness.plan().status == space::PlanStatus::model_failed_to_load, "undecodable model");
    }
    {
        Harness harness;
        space::PlanInput input;
        input.map = &harness.map;
        input.catalog_loaded = false;
        expect(space::build_plan(input).status == space::PlanStatus::catalog_unavailable,
               "no catalog is a distinct outcome");
    }
}

} // namespace

void test_surface_rejections() {
    surface_parameter_rejections();
    surface_rigid_chain_cases();
    surface_hierarchy_rejections();
    surface_bone_visibility_cases();
    surface_input_failure_cases();
}

std::vector<std::byte> bytes(std::initializer_list<int> values) {
    std::vector<std::byte> result;
    for (const int value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

namespace {

void expect_unsupported(const eawr::assets::Texture& texture, const std::string& label) {
    const auto result = space::normalize_texture(texture);
    expect(!result && result.error().code == "EAWR-SPACE-0001", label);
}

void expect_oversized(const eawr::assets::Texture& texture, const std::string& label) {
    const auto result = space::normalize_texture(texture);
    expect(!result && result.error().code == "EAWR-SPACE-0001"
               && result.error().message.find("signed 32-bit limit") != std::string::npos,
           label);
}

// A 1x2 bottom-left RGBA texture with alpha; every other fixture derives from it.
eawr::assets::Texture tga_fixture() {
    eawr::assets::Texture tga;
    tga.source = source("data/art/textures/t.tga");
    tga.width = 1;
    tga.height = 2;
    tga.format = eawr::assets::PixelFormat::rgba8;
    tga.source_origin = eawr::assets::ImageOrigin::bottom_left;
    tga.has_alpha = true;
    tga.mips.push_back({1, 2, 4, bytes({1, 2, 3, 4, 5, 6, 7, 8})});
    return tga;
}

eawr::assets::Texture bgra_fixture() {
    eawr::assets::Texture bgra = tga_fixture();
    bgra.format = eawr::assets::PixelFormat::bgra8;
    bgra.source_origin = eawr::assets::ImageOrigin::top_left;
    return bgra;
}

eawr::assets::Texture bgr_fixture() {
    eawr::assets::Texture bgr = tga_fixture();
    bgr.format = eawr::assets::PixelFormat::bgr8;
    bgr.source_origin = eawr::assets::ImageOrigin::top_left;
    bgr.mips = {{1, 2, 3, bytes({1, 2, 3, 4, 5, 6})}};
    return bgr;
}

eawr::assets::Texture bc1_fixture() {
    eawr::assets::Texture bc1 = tga_fixture();
    bc1.format = eawr::assets::PixelFormat::bc1;
    bc1.source_origin = eawr::assets::ImageOrigin::top_left;
    bc1.width = 5;
    bc1.height = 3;
    bc1.mips = {{5, 3, 16, bytes({1, 2, 3, 4, 5, 6, 7, 8,
                                  9, 10, 11, 12, 13, 14, 15, 16})},
                {2, 1, 8, bytes({17, 18, 19, 20, 21, 22, 23, 24})},
                {1, 1, 8, bytes({25, 26, 27, 28, 29, 30, 31, 32})}};
    return bc1;
}

eawr::assets::Texture bc3_fixture() {
    eawr::assets::Texture bc3 = bc1_fixture();
    bc3.format = eawr::assets::PixelFormat::bc3;
    bc3.width = 5;
    bc3.height = 5;
    bc3.mips = {{5, 5, 32, std::vector<std::byte>(64, std::byte{0x5a})},
                {2, 2, 16, std::vector<std::byte>(16, std::byte{0x6b})},
                {1, 1, 16, std::vector<std::byte>(16, std::byte{0x7c})}};
    return bc3;
}

void uncompressed_conversion_cases() {
    const eawr::assets::Texture tga = tga_fixture();
    auto flipped = space::normalize_texture(tga);
    expect(bool(flipped), "bottom-left RGBA normalises");
    if (flipped) {
        expect(flipped.value().mips.front().bytes == bytes({5, 6, 7, 8, 1, 2, 3, 4}),
               "bottom-left rows are flipped to top-left");
        expect(flipped.value().source_origin == eawr::assets::ImageOrigin::top_left, "origin becomes top-left");
        expect(flipped.value().has_alpha, "the alpha flag is retained, not invented away");
    }
    const eawr::assets::Texture bgra = bgra_fixture();
    auto swizzled = space::normalize_texture(bgra);
    expect(swizzled && swizzled.value().mips.front().bytes == bytes({3, 2, 1, 4, 7, 6, 5, 8}), "BGRA is swizzled");
    const eawr::assets::Texture bgr = bgr_fixture();
    auto expanded = space::normalize_texture(bgr);
    expect(expanded && expanded.value().mips.front().bytes == bytes({3, 2, 1, 255, 6, 5, 4, 255}),
           "BGR is swizzled and made opaque");
    eawr::assets::Texture luminance = tga;
    luminance.format = eawr::assets::PixelFormat::l8;
    luminance.has_alpha = false;
    luminance.mips = {{1, 2, 1, bytes({17, 200})}};
    auto grey = space::normalize_texture(luminance);
    expect(grey && grey.value().format == eawr::assets::PixelFormat::rgba8
               && grey.value().mips.front().bytes == bytes({200, 200, 200, 255, 17, 17, 17, 255}),
           "bottom-left L8 is flipped and expanded to opaque grey RGBA");
    eawr::assets::Texture short_mip = tga;
    short_mip.mips = {{1, 2, 4, bytes({1, 2, 3, 4})}};
    expect_unsupported(short_mip, "a short mip level is refused");
}

void odd_mip_chain_cases() {
    eawr::assets::Texture odd_rgba = tga_fixture();
    odd_rgba.width = 3;
    odd_rgba.height = 5;
    odd_rgba.mips = {{3, 5, 12, std::vector<std::byte>(60)},
                     {1, 2, 4, bytes({1, 2, 3, 4, 5, 6, 7, 8})},
                     {1, 1, 4, bytes({9, 10, 11, 12})}};
    for (std::size_t row = 0; row < 5; ++row) {
        for (std::size_t byte = 0; byte < 12; ++byte) {
            odd_rgba.mips[0].bytes[row * 12 + byte] = static_cast<std::byte>(row * 20 + byte);
        }
    }
    const auto original_odd_bytes = odd_rgba.mips[0].bytes;
    const auto odd_flipped = space::normalize_texture(odd_rgba);
    expect(odd_flipped && odd_flipped.value().mips.size() == 3,
           "odd RGBA mip chain normalises");
    if (odd_flipped) {
        const auto& mips = odd_flipped.value().mips;
        expect(mips[0].row_pitch == 12 && mips[0].bytes[0] == std::byte{80}
                   && mips[0].bytes[48] == std::byte{0}, "odd base mip reverses all five rows");
        expect(mips[1].bytes == bytes({5, 6, 7, 8, 1, 2, 3, 4})
                   && mips[2].bytes == bytes({9, 10, 11, 12}),
               "each smaller mip is oriented independently");
    }
    expect(odd_rgba.mips[0].bytes == original_odd_bytes
               && odd_rgba.source_origin == eawr::assets::ImageOrigin::bottom_left,
           "normalisation leaves source pixels and origin unchanged");
}

void padded_row_cases() {
    eawr::assets::Texture padded_bgr = bgr_fixture();
    padded_bgr.width = 2;
    padded_bgr.height = 2;
    padded_bgr.mips = {{2, 2, 8, bytes({1, 2, 3, 4, 5, 6, 99, 99,
                                      7, 8, 9, 10, 11, 12})}};
    const auto bgr_output = space::normalize_texture(padded_bgr);
    expect(bgr_output && bgr_output.value().mips[0].row_pitch == 8
               && bgr_output.value().mips[0].bytes
                   == bytes({3, 2, 1, 255, 6, 5, 4, 255,
                             9, 8, 7, 255, 12, 11, 10, 255}),
           "padded BGR rows expand without consuming padding");
    eawr::assets::Texture padded_bgra = bgra_fixture();
    padded_bgra.width = 2;
    padded_bgra.height = 2;
    padded_bgra.mips = {{2, 2, 12, bytes({1, 2, 3, 4, 5, 6, 7, 8, 99, 99, 99, 99,
                                         9, 10, 11, 12, 13, 14, 15, 16})}};
    const auto bgra_output = space::normalize_texture(padded_bgra);
    expect(bgra_output && bgra_output.value().mips[0].bytes
               == bytes({3, 2, 1, 4, 7, 6, 5, 8,
                         11, 10, 9, 12, 15, 14, 13, 16}),
           "padded BGRA rows swizzle without consuming padding");
}

void block_passthrough_cases() {
    const eawr::assets::Texture bc1 = bc1_fixture();
    const auto bc1_output = space::normalize_texture(bc1);
    expect(bc1_output && bc1_output.value().format == eawr::assets::PixelFormat::bc1
               && bc1_output.value().mips.size() == 3
               && bc1_output.value().mips[0].bytes == bc1.mips[0].bytes
               && bc1_output.value().mips[1].bytes == bc1.mips[1].bytes,
           "odd top-left BC1 mip bytes pass through unchanged");
    const eawr::assets::Texture bc3 = bc3_fixture();
    const auto bc3_output = space::normalize_texture(bc3);
    expect(bc3_output && bc3_output.value().mips[0].row_pitch == 32
               && bc3_output.value().mips[0].bytes == bc3.mips[0].bytes
               && bc3_output.value().mips[2].bytes == bc3.mips[2].bytes,
           "odd 16-byte BC blocks retain their pitch and payload");
}

void malformed_block_cases() {
    eawr::assets::Texture bc1 = bc1_fixture();
    const eawr::assets::Texture bc3 = bc3_fixture();
    bc1.source_origin = eawr::assets::ImageOrigin::bottom_left;
    expect_unsupported(bc1, "a bottom-left block format is refused");
    bc1.source_origin = eawr::assets::ImageOrigin::top_left;
    eawr::assets::Texture malformed = bc1;
    malformed.mips[0].row_pitch = 4;
    expect_unsupported(malformed, "BC1 top mip with a four-byte pitch is refused");
    malformed = bc1;
    malformed.mips[0].row_pitch = 24;
    malformed.mips[0].bytes.resize(24);
    expect_unsupported(malformed, "padded BC1 top mip row is refused");
    malformed = bc1;
    malformed.mips[0].bytes.resize(15);
    expect_unsupported(malformed, "short BC1 top mip is refused");
    malformed = bc1;
    malformed.mips[0].bytes.push_back(std::byte{0});
    expect_unsupported(malformed, "trailing BC1 top mip byte is refused");
    malformed = bc1;
    malformed.mips[1].row_pitch = 4;
    expect_unsupported(malformed, "malformed BC1 later mip is refused");
    malformed = bc1;
    malformed.mips[1].row_pitch = 16;
    malformed.mips[1].bytes.resize(16);
    expect_unsupported(malformed, "padded BC1 later mip row is refused");
    malformed = bc1;
    malformed.mips[1].bytes.push_back(std::byte{0});
    expect_unsupported(malformed, "trailing BC1 later mip byte is refused");
    malformed = bc3;
    malformed.mips[0].bytes.resize(63);
    expect_unsupported(malformed, "short 16-byte BC mip is refused");
    malformed = bc3;
    malformed.mips[0].row_pitch = 33;
    expect_unsupported(malformed, "unaligned BC pitch is refused");
    malformed = bc1;
    malformed.mips[0].width = 4;
    expect_unsupported(malformed, "base mip dimensions must match texture dimensions");
    malformed = bc1;
    malformed.mips[1].width = 3;
    expect_unsupported(malformed, "each mip must have the expected halved dimensions");
    malformed = bc1;
    malformed.mips.push_back(malformed.mips.back());
    expect_unsupported(malformed, "mip chain cannot continue beyond 1x1");
}

void malformed_format_and_size_cases() {
    const eawr::assets::Texture tga = tga_fixture();
    const eawr::assets::Texture bc1 = bc1_fixture();
    eawr::assets::Texture malformed = tga;
    malformed.mips[0].row_pitch = 3;
    expect_unsupported(malformed, "uncompressed pitch below pixel width is refused");
    malformed = tga;
    malformed.source_origin = static_cast<eawr::assets::ImageOrigin>(255);
    expect_unsupported(malformed, "invalid image origin is refused");
    malformed = tga;
    malformed.format = static_cast<eawr::assets::PixelFormat>(255);
    expect_unsupported(malformed, "unknown pixel format is refused");
    malformed = tga;
    malformed.format = eawr::assets::PixelFormat::a8;
    expect_unsupported(malformed, "alpha-only texture is explicitly refused");
    malformed = tga;
    malformed.width = std::numeric_limits<std::uint32_t>::max();
    malformed.mips[0].width = malformed.width;
    expect_oversized(malformed, "oversized texture width is refused before payload validation");
    malformed = tga;
    malformed.height = std::numeric_limits<std::uint32_t>::max();
    malformed.mips[0].height = malformed.height;
    expect_oversized(malformed, "oversized texture height is refused before payload validation");
    malformed = bc1;
    malformed.mips[1].width = std::numeric_limits<std::uint32_t>::max();
    malformed.mips[1].row_pitch = 0;
    malformed.mips[1].bytes.clear();
    expect_oversized(malformed, "oversized later mip width is refused before payload validation");
    malformed = bc1;
    malformed.mips[1].height = std::numeric_limits<std::uint32_t>::max();
    malformed.mips[1].row_pitch = 0;
    malformed.mips[1].bytes.clear();
    expect_oversized(malformed, "oversized later mip height is refused before payload validation");
}

void planner_texture_cases() {
    Harness harness;
    harness.supplied_texture = bc3_fixture();
    harness.supplied_texture->mips[1].bytes.resize(15);
    const auto plan = harness.plan();
    expect(!plan.surfaces.empty() && plan.surfaces[0].status == space::SurfaceStatus::texture_unsupported
               && !plan.surfaces[0].texture
               && plan.surfaces[0].texture_identity.mip_count == 3,
           "planner rejects a malformed later BC mip while retaining source identity");
}

} // namespace

void test_texture_normalisation() {
    uncompressed_conversion_cases();
    odd_mip_chain_cases();
    padded_row_cases();
    block_passthrough_cases();
    malformed_block_cases();
    malformed_format_and_size_cases();
    planner_texture_cases();
}

} // namespace eawr_space_test
