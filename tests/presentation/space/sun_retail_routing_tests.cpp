#include "sun_retail_test_support.hpp"

namespace eawr_sun_retail_test {

// -- degenerate guards and precedence ------------------------------------------------------

void test_degenerate_guards() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    for (const Vec3f light : {Vec3f{nan, 0, 1}, Vec3f{1, inf, 0}, Vec3f{0, 0, -inf}}) {
        RetailCase sun;
        sun.toward_light = light;
        expect_status(sun, SunRetailStatus::sun_direction_nonfinite, "non-finite L");
    }
    for (const Vec3f light : {Vec3f{0, 0, 0}, Vec3f{-0.0F, -0.0F, -0.0F}, Vec3f{0, -0.0F, 0}}) {
        RetailCase sun;
        sun.toward_light = light;
        expect_status(sun, SunRetailStatus::sun_direction_zero, "zero L");
    }
    for (const Vec3f light : {Vec3f{0, 1.0002F, 0}, Vec3f{0, 0.99985F, 0}, Vec3f{1.0e-30F, 0, 0},
                              Vec3f{3.0e38F, 0, 3.0e38F}, Vec3f{0, 0.5F, 0.5F}}) {
        RetailCase sun;
        sun.toward_light = light;
        expect_status(sun, SunRetailStatus::sun_direction_not_unit, "non-unit L " + show(widen(light)));
    }
    // Within the unit tolerance L is used as given for the distance.
    {
        RetailCase long_light;
        long_light.toward_light = {0, 1.00005F, 0};
        const auto placement = long_light.place();
        expect_near3(placement.origin, {0, 990.04995, 0}, "|L| = 1.00005 places at E + d*L, not E + d*L/|L|", 1.0e-4);
        expect_near3(placement.axes[1], {0, -1, 0}, "|L| = 1.00005 still faces along -L/|L|", 1.0e-12);
        RetailCase short_light;
        short_light.toward_light = {0, 0.99992F, 0};
        expect_near3(short_light.place().origin, {0, 989.92001, 0}, "|L| = 0.99992 places at E + d*L", 1.0e-4);
    }

    // Camera, at validate_camera's thresholds.
    for (int field = 0; field < 3; ++field) {
        RetailCase sun;
        (field == 0 ? sun.eye : field == 1 ? sun.target : sun.up).y = field == 1 ? inf : nan;
        expect_status(sun, SunRetailStatus::camera_nonfinite, "non-finite camera field " + std::to_string(field));
    }
    {
        RetailCase sun;
        sun.target = sun.eye;
        expect_status(sun, SunRetailStatus::camera_direction_degenerate, "eye = target");
        sun.eye = {0, 0, 0};
        sun.target = {5.0e-7F, 0, 0};
        expect_status(sun, SunRetailStatus::camera_direction_degenerate, "eye 5e-7 from target");
        RetailCase overhead;
        overhead.eye = {0, 0, 10};
        expect_status(overhead, SunRetailStatus::camera_up_collinear, "looking straight down with up +Z");
        RetailCase zero_up;
        zero_up.up = {0, 0, 0};
        expect_status(zero_up, SunRetailStatus::camera_up_collinear, "zero up");
        zero_up.up = {0, 0, 5.0e-7F};
        expect_status(zero_up, SunRetailStatus::camera_up_collinear, "up shorter than 1e-6");
        RetailCase grazing;
        grazing.up = {0, 1, 1.0e-5F};
        expect_status(grazing, SunRetailStatus::camera_up_collinear, "up within 1e-4 of forward");
        grazing.up = {0, 1, 1.0e-3F};
        expect_status(grazing, SunRetailStatus::placed, "up 1e-3 off forward is placed");
        RetailCase scaled_up;
        scaled_up.up = {0, 1000, 0.05F};
        expect_status(scaled_up, SunRetailStatus::camera_up_collinear, "the up test uses a normalised sine");
    }

    // A sun behind the camera is still placed: culling is not this evaluator's job (G-02, G-06).
    {
        RetailCase behind;
        behind.toward_light = {0, -1, 0};
        expect_near3(behind.place().origin, {0, -1010, 0}, "a sun behind the eye is placed behind it");
        expect_frame(behind.place(), {-1, 0, 0}, {0, 1, 0}, {0, 0, 1}, "a sun behind the eye still faces it");
    }

    // Vertex validation.
    const auto placement = RetailCase().place();
    expect(!space::sun_retail_vertex(placement, {nan, 0, 0}), "NaN local x places no vertex");
    expect(!space::sun_retail_vertex(placement, {0, 0, inf}), "infinite local z places no vertex");
    expect(space::sun_retail_vertex(placement, {0, 0, 0}).has_value(), "a finite local vertex is placed");
}

void test_precedence() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    RetailCase sun({0, 0, 0});
    sun.bones[1].billboard = 0;
    sun.bones[0].parent = 9;
    sun.eye.x = nan;
    sun.toward_light = {0, 0, 0};
    expect_status(sun, SunRetailStatus::mode_not_sun, "precedence: mode before everything");
    sun.mesh_bone = 4;
    expect_status(sun, SunRetailStatus::chain_invalid, "precedence: an unreadable mesh bone first");
    sun.mesh_bone = 1;
    sun.bones[1].billboard = 23;
    expect_status(sun, SunRetailStatus::mode_sun_alias, "precedence: an alias before the chain");
    sun.bones[1].billboard = 7;
    sun.bones[0].billboard = 1;
    expect_status(sun, SunRetailStatus::chain_invalid, "precedence: structure before ancestor mode");
    sun.bones[0].parent = -1;
    sun.bones[1].relative_transform[0] = 2.0F;
    expect_status(sun, SunRetailStatus::chain_billboard_ancestor, "precedence: ancestor mode before a scaled bone");
    sun.bones[0].billboard = 0;
    expect_status(sun, SunRetailStatus::chain_not_proper_rigid, "precedence: rigidity before d");
    sun.bones[1].relative_transform[0] = 1.0F;
    expect_status(sun, SunRetailStatus::sun_distance_zero, "precedence: d before the camera");
    sun.bones[1].relative_transform[3] = 1000.0F;
    expect_status(sun, SunRetailStatus::camera_nonfinite, "precedence: camera before L");
    sun.eye = {0, 0, 0};
    sun.toward_light = {nan, 0, 0};
    expect_status(sun, SunRetailStatus::camera_direction_degenerate, "precedence: degenerate camera before L");
    sun.eye = {0, 0, 10};
    sun.toward_light = {0, 0, 1};
    expect_status(sun, SunRetailStatus::camera_up_collinear, "precedence: collinear camera up before L along up");
    sun.eye = {0, -10, 0};
    sun.toward_light = {0, 0, nan};
    expect_status(sun, SunRetailStatus::sun_direction_nonfinite, "precedence: non-finite L before zero");
    sun.toward_light = {0, 0, 2};
    expect_status(sun, SunRetailStatus::sun_direction_not_unit, "precedence: non-unit L before L along up");
}

// -- statelessness, immutability, render basis --------------------------------------------

void test_statelessness() {
    RetailCase sun = general_case();
    const auto before = sun.bones;
    const auto first = sun.place();
    // A degenerate pass in between leaves no collapsed s behind (R-M7-05 retention is not reproduced).
    RetailCase degenerate = sun;
    degenerate.toward_light = {0, 0, 1};
    degenerate.up = {0, 0, 1};
    degenerate.eye = {0, -10, 0};
    degenerate.target = {0, 0, 0};
    expect(degenerate.place().status == SunRetailStatus::sun_along_camera_up, "the in-between pass is degenerate");
    const auto second = sun.place();
    expect(same(first, second) && std::abs(second.scale - 0.9999999865388264) <= 1.0e-12,
           "evaluation after a degenerate pass is bit-identical, with RT-07's measured s");
    bool unchanged = before.size() == sun.bones.size();
    for (std::size_t index = 0; unchanged && index < before.size(); ++index) {
        const auto& a = before[index];
        const auto& b = sun.bones[index];
        unchanged = a.name == b.name && a.parent == b.parent && a.visible == b.visible && a.billboard == b.billboard
            && a.relative_transform == b.relative_transform;
    }
    expect(unchanged, "the bind records are not modified");
    RetailCase hidden = sun;
    hidden.bones[0].visible = false;
    hidden.bones[1].visible = false;
    expect(same(first, hidden.place()), "bone visibility is not read by the evaluator");

    // The RT-01 camera given in the render basis.
    RetailCase rendered;
    rendered.eye = space::source_from_render({0, 0, 10});
    rendered.target = space::source_from_render({0, 0, 0});
    rendered.up = space::source_from_render({0, 1, 0});
    expect_vertex(rendered, {1, 0, 1}, {1, 990, 1}, "the RT-01 camera given in the render basis");
}

// -- admission stays closed ----------------------------------------------------------------

constexpr const char* qualified_shader = "EawrSyntheticOpaqueDiffuse.fx";

eawr::assets::Source source(const std::string& path) {
    return {path, "synthetic", "test", eawr::vfs::AssetOrigin::loose, 0};
}

// A 2x2-cell quad (3x3 vertices) in the source basis.
eawr::assets::Submesh quad(const Vec3f origin, const Vec3f u_axis, const Vec3f v_axis, const std::string& texture) {
    eawr::assets::Submesh submesh;
    submesh.shader = qualified_shader;
    submesh.vertex_format = "alD3dVertNU2";
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const float u = static_cast<float>(column) * 0.5F;
            const float v = static_cast<float>(row) * 0.5F;
            eawr::assets::Vertex vertex;
            vertex.position = {origin.x + u_axis.x * u + v_axis.x * v, origin.y + u_axis.y * u + v_axis.y * v,
                               origin.z + u_axis.z * u + v_axis.z * v};
            vertex.texcoord[0] = {u, v};
            submesh.vertices.push_back(vertex);
        }
    }
    for (std::uint16_t row = 0; row < 2; ++row) {
        for (std::uint16_t column = 0; column < 2; ++column) {
            const auto corner = static_cast<std::uint16_t>(row * 3 + column);
            submesh.indices.insert(submesh.indices.end(),
                {corner, static_cast<std::uint16_t>(corner + 1), static_cast<std::uint16_t>(corner + 4),
                 corner, static_cast<std::uint16_t>(corner + 4), static_cast<std::uint16_t>(corner + 3)});
        }
    }
    submesh.parameters.push_back({"BaseTexture", eawr::assets::ParameterKind::texture, texture});
    return submesh;
}

// Surface 0 on the identity root; surface 1 on a sun bone of `mode` whose own
// translation the retail evaluator places.
eawr::assets::Model sun_sky_model(const std::uint32_t mode) {
    eawr::assets::Model model;
    model.source = source("data/art/models/eawr_sun_retail_sky.alo");
    model.bones.push_back(record("Root", -1, 0, identity12));
    model.bones.push_back(record("Sun", 0, mode, translation(1000, 0, 0)));
    eawr::assets::Mesh stars;
    stars.name = "Stars";
    stars.bone = 0;
    stars.submeshes.push_back(quad({400, -100, 100}, {0, 200, 0}, {0, 0, -200}, "eawr_sun_retail_stars.tga"));
    eawr::assets::Mesh sun;
    sun.name = "Sun";
    sun.bone = 1;
    sun.submeshes.push_back(quad({-50, 0, -50}, {100, 0, 0}, {0, 0, 100}, "eawr_sun_retail_sun.tga"));
    model.meshes.push_back(stars);
    model.meshes.push_back(sun);
    return model;
}

space::SkyPlan viewer_plan(const eawr::assets::Model& model, const std::span<const space::MaterialRouteRow> routes) {
    eawr::assets::Map map;
    map.source = source("data/art/maps/eawr_sun_retail_space.ted");
    map.kind = eawr::assets::MapKind::space;
    map.semantic_complete = true;
    eawr::assets::EnvironmentDescriptor environment;
    environment.name = "EAWR_SUN_RETAIL_ENVIRONMENT";
    environment.primary_sky = "EAWR_SUN_RETAIL_SKY";
    map.environments.push_back(environment);
    eawr::assets::ObjectTypeRef type;
    type.logical_name = "EAWR_SUN_RETAIL_SKY";
    type.source.logical_path = "data/xml/eawr_sun_retail_objects.xml";
    type.space_model_name = "eawr_sun_retail_sky.alo";

    space::PlanInput input;
    input.map = &map;
    input.sky_type = &type;
    input.catalog_loaded = true;
    input.qualifications = space::qualifications();
    input.material_routes = routes;
    input.model = [&model](const std::string_view name) {
        space::ModelLookup lookup;
        lookup.status = space::ModelLookup::Status::resolved;
        lookup.logical_path = "data/art/models/" + std::string(name);
        lookup.sha256 = std::string(64, 'a');
        lookup.model = model;
        return lookup;
    };
    input.texture = [](const std::string_view name) {
        space::TextureLookup lookup;
        lookup.status = space::TextureLookup::Status::resolved;
        lookup.logical_path = "data/art/textures/" + std::string(name);
        lookup.sha256 = std::string(64, 'c');
        eawr::assets::Texture texture;
        texture.source = source(lookup.logical_path);
        texture.width = 1;
        texture.height = 1;
        texture.format = eawr::assets::PixelFormat::rgba8;
        eawr::assets::MipLevel mip;
        mip.width = 1;
        mip.height = 1;
        mip.row_pitch = 4;
        mip.bytes = {std::byte{200}, std::byte{150}, std::byte{50}, std::byte{255}};
        texture.mips.push_back(mip);
        lookup.texture = texture;
        return lookup;
    };
    return space::build_plan(input);
}

bool mentions(const std::filesystem::path& file, const std::vector<std::string_view>& tokens) {
    std::ifstream stream(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return std::any_of(tokens.begin(), tokens.end(),
                       [&text](const std::string_view token) { return text.find(token) != std::string::npos; });
}

void test_admission_closed() {
    // The gate rows, and a placed result is still not admissible.
    std::set<std::string_view> gates;
    for (const auto& gate : space::sun_retail_open_gates()) {
        expect(!gate.blocks.empty(), std::string(gate.id) + " names what it blocks");
        gates.insert(gate.id);
    }
    expect(gates == std::set<std::string_view>{"G-01a", "G-02", "G-03", "G-06", "G-07", "G-08", "G-12", "WP16-PASS"},
           "the open admission gates are G-01a, G-02, G-03, G-06, G-07, G-08, G-12 and WP16-PASS");
    const auto placed = RetailCase().place();
    expect(placed.status == SunRetailStatus::placed && !space::sun_retail_admissible(placed),
           "a placed retail sun is not admissible while gates are open");

    // Planner, sky ledger (plan_surfaces with qualifications()) and viewer (build_plan):
    // a sun the retail evaluator places stays hierarchy_unsupported with no upload payload,
    // so the renderer is never handed it.
    for (const std::uint32_t mode : {7U, 23U}) {
        const auto model = sun_sky_model(mode);
        const std::string label = "mode " + std::to_string(mode) + " sun";
        if (mode == 7) {
            space::SunRetailInput input;
            input.bones = model.bones;
            input.mesh_bone = 1;
            input.eye = {0, -10, 0};
            input.target = {0, 0, 0};
            input.toward_light = {0, 1, 0};
            expect(space::sun_retail_placement(input).status == SunRetailStatus::placed,
                   "the planner fixture's sun is placed by the retail evaluator");
        }
        const auto ledger = space::plan_surfaces(model, space::qualifications());
        const auto routed = space::plan_surfaces(model, space::qualifications(), space::meshadditive_material_routes());
        for (const auto* surfaces : {&ledger, &routed}) {
            expect(surfaces->size() == 2 && (*surfaces)[0].status == space::SurfaceStatus::accepted
                       && (*surfaces)[1].status == space::SurfaceStatus::hierarchy_unsupported && !(*surfaces)[1].model
                       && !(*surfaces)[1].texture && (*surfaces)[1].detail.find("billboard") != std::string::npos,
                   label + ": the planner verdict stays hierarchy_unsupported");
        }
        const std::array<std::span<const space::MaterialRouteRow>, 3> route_sets{
            std::span<const space::MaterialRouteRow>{}, space::material_routes(), space::meshadditive_material_routes()};
        for (const auto routes : route_sets) {
            const auto plan = viewer_plan(model, routes);
            expect(plan.status == space::PlanStatus::surface_rejected && plan.surfaces.size() == 2
                       && plan.surfaces[1].status == space::SurfaceStatus::hierarchy_unsupported
                       && !plan.surfaces[1].model && !plan.surfaces[1].texture,
                   label + ": the viewer's plan is blocked and carries no sun payload");
        }
    }
    const auto control = viewer_plan(sun_sky_model(0), {});
    expect(control.status == space::PlanStatus::ready && control.accepted_count() == 2,
           "control: the same fixture with a non-billboard bone plans ready: " + control.detail);

    // The proper-rigid chain fact agrees with the planner on the same records.
    const std::array<Rigid12, 7> records{{
        identity12,
        {0, -1, 0, 5, 1, 0, 0, 6, 0, 0, 1, 7},
        {1.00004F, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},
        {1.0002F, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},
        {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0},
        {1, 0.5F, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},
        translation(0, std::numeric_limits<float>::infinity(), 0),
    }};
    for (std::size_t index = 0; index < records.size(); ++index) {
        auto model = sun_sky_model(0);
        model.bones[0].relative_transform = records[index];
        const bool planner_rigid = space::plan_surfaces(model, space::qualifications())[0].status
            == space::SurfaceStatus::accepted;
        RetailCase sun;
        sun.bones[0].relative_transform = records[index];
        const bool retail_rigid = sun.place().status != SunRetailStatus::chain_not_proper_rigid;
        expect(planner_rigid == retail_rigid, "record " + std::to_string(index)
            + ": the retail chain check agrees with the planner's proper-rigid test");
    }

    // Structural: nothing outside the owned pair, the build lists and the P1
    // environment view's approximate sun (environment_scene, rescope
    // 2026-09-24) references the retail policy.
    const auto root = std::filesystem::path{EAWR_SUN_RETAIL_SOURCE_ROOT}.lexically_normal();
    const std::set<std::filesystem::path> owned{
        (root / "include/eawr/presentation/space/sun_retail.hpp").lexically_normal(),
        (root / "src/presentation/space/sun_retail.cpp").lexically_normal(),
        (root / "include/eawr/presentation/space/environment_scene.hpp").lexically_normal(),
        (root / "src/presentation/space/environment_scene.cpp").lexically_normal(),
        (root / "src/presentation/sources.cmake").lexically_normal(),
        (root / "apps/viewer/CMakeLists.txt").lexically_normal()};
    for (const auto& file : owned) expect(std::filesystem::is_regular_file(file), "owned file exists: " + file.string());
    const std::vector<std::string_view> tokens{"sun_retail", "eawr-sun-mode7-retail"};
    std::size_t scanned = 0;
    std::set<std::string> required{"src/presentation/CMakeLists.txt", "src/presentation/space/space.cpp",
                                   "src/presentation/sources.cmake",
                                   "src/presentation/godot/renderer.cpp", "apps/viewer/CMakeLists.txt",
                                   "apps/viewer/src/space_environment.cpp", "apps/sky_scan/main.cpp"};
    // Build outputs copied into the tree (the viewer's GDExtension and its debug symbols under
    // apps/viewer/project/bin) carry the policy's symbol names; they are not source (#882).
    const std::set<std::string> build_outputs{".pdb", ".dll", ".exp", ".lib", ".ilk", ".obj", ".o", ".a", ".so",
                                              ".dylib", ".exe"};
    for (const char* directory : {"src", "apps", "include"}) {
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(root / directory, error), end; !error && it != end;
             it.increment(error)) {
            if (!it->is_regular_file()) continue;
            if (build_outputs.count(it->path().extension().string()) != 0) continue;
            const auto path = it->path().lexically_normal();
            ++scanned;
            required.erase(path.lexically_relative(root).generic_string());
            if (owned.count(path) != 0) continue;
            expect(!mentions(path, tokens), "production file does not reference the retail sun policy: "
                + path.lexically_relative(root).generic_string());
        }
        expect(!error, std::string("scanned ") + directory + " without error");
    }
    expect(scanned > 100 && required.empty(), "the admission scan covered the planner, renderer, viewer and ledger ("
        + std::to_string(scanned) + " files)");
}

} // namespace eawr_sun_retail_test
