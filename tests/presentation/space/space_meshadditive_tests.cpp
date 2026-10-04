#include "space_effect_support.hpp"

namespace eawr_space_test {

// -- MeshAdditive sky route (opt-in, synthetic) --------------------------------

constexpr const char* meshadditive_shader = "MeshAdditive.fx";
constexpr Vec4f authored_color{0.5F, 0.75F, 1.0F, 0.2F};
constexpr Vec4f authored_scroll{0.25F, -0.5F, 7.0F, 9.0F};

bool near4(const Vec4f& a, const Vec4f& b, const float tolerance = 1.0e-5F) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance)
        && near(a.w, b.w, tolerance);
}

// Converts a synthetic surface to MeshAdditive: Color, UVScrollRate, then
// BaseTexture. Normals are left zero on purpose; the pass ignores them.
void make_meshadditive(eawr::assets::Submesh& submesh) {
    const std::string texture = std::get<std::string>(submesh.parameters.front().value);
    submesh.shader = meshadditive_shader;
    submesh.parameters = {
        {"Color", ParameterKind::vector4, authored_color},
        {"UVScrollRate", ParameterKind::vector4, authored_scroll},
        {"BaseTexture", ParameterKind::texture, texture},
    };
    for (auto& vertex : submesh.vertices) vertex.normal = {0.0F, 0.0F, 0.0F};
}

struct AdditiveHarness final {
    Harness harness;
    AdditiveHarness() {
        for (auto& mesh : harness.model.meshes) make_meshadditive(mesh.submeshes.front());
        harness.routes = space::meshadditive_material_routes();
    }
    eawr::assets::Submesh& first() { return harness.model.meshes[0].submeshes[0]; }
};

void expect_additive_rejected(AdditiveHarness& additive, const space::SurfaceStatus cause, const std::string& label) {
    const auto plan = additive.harness.plan();
    expect(plan.status == space::PlanStatus::surface_rejected, label + ": the plan is not ready");
    if (plan.surfaces.empty()) return;
    const auto& surface = plan.surfaces.front();
    expect(surface.status == cause, label + ": first cause is " + std::string(space::to_string(cause))
        + ", got " + std::string(space::to_string(surface.status)));
    expect(!surface.texture && !surface.model, label + ": a rejected surface has no upload payload");
    expect(surface.material == space::SkyMaterial::meshadditive, label + ": the route still names the matched material");
}

namespace {

void meshadditive_route_table() {
    // The shipped route table is untouched; the opt-in table adds one exact row.
    expect(space::material_routes().size() == 1 && space::material_routes().front().shader == "MeshGloss.fx",
           "material_routes() still holds only MeshGloss.fx");
    const auto routes = space::meshadditive_material_routes();
    expect(routes.size() == 2 && routes[0].shader == "MeshGloss.fx" && routes[0].material == space::SkyMaterial::meshgloss
           && routes[1].shader == "MeshAdditive.fx" && routes[1].material == space::SkyMaterial::meshadditive
           && !routes[1].provenance.empty(), "the opt-in table is MeshGloss.fx plus exact MeshAdditive.fx");
    expect(space::to_string(space::SkyMaterial::meshadditive) == "meshadditive", "material name");
    const auto fields = space::meshadditive_fields();
    const std::vector<std::pair<std::string, std::string>> expected_fields{
        {"BaseTexture", "consumed"}, {"Color", "consumed_rgb"}, {"UVScrollRate", "consumed_xy"}};
    expect(fields.size() == expected_fields.size(), "three MeshAdditive field dispositions");
    for (std::size_t index = 0; index < std::min(fields.size(), expected_fields.size()); ++index) {
        expect(fields[index].name == expected_fields[index].first
               && fields[index].disposition == expected_fields[index].second && !fields[index].rule.empty(),
               "field disposition " + expected_fields[index].first);
    }
    const auto inputs = space::meshadditive_default_inputs();
    expect(inputs.id == "space-sky-meshadditive-inputs-v1" && inputs.time == 0.0F
           && same(inputs.light_scale, {1, 1, 1, 1}) && !inputs.cause.empty() && space::finite_inputs(inputs),
           "the default inputs freeze TIME at 0 and declare a unit LIGHT_SCALE");
    auto bad_inputs = inputs;
    bad_inputs.time = std::numeric_limits<float>::infinity();
    expect(!space::finite_inputs(bad_inputs), "a non-finite TIME is not a valid input");
    bad_inputs = inputs;
    bad_inputs.light_scale.w = std::numeric_limits<float>::quiet_NaN();
    expect(!space::finite_inputs(bad_inputs), "a non-finite LIGHT_SCALE is not a valid input");
    const std::vector<std::pair<std::string, std::string>> expected_states{
        {"blend", "one_one_add"}, {"depth_write", "off"}, {"depth_test", "on_engine_default"}, {"fog", "disabled"},
        {"cull", "disabled"}, {"srgb", "stored_values_on_srgb_output"}, {"alpha", "rgb_only"}, {"pass", "transparent"}};
    const auto states = space::meshadditive_render_policy();
    expect(states.size() == expected_states.size(), "eight declared render states");
    for (std::size_t index = 0; index < std::min(states.size(), expected_states.size()); ++index) {
        expect(states[index].state == expected_states[index].first
               && states[index].value == expected_states[index].second && !states[index].rule.empty(),
               "render state " + expected_states[index].first);
    }
}

void meshadditive_admission_cases() {
    // Opt-in: under the shipped routes a MeshAdditive surface keeps the ledger's verdict.
    for (const bool shipped : {true, false}) {
        AdditiveHarness additive;
        additive.harness.routes = shipped ? space::material_routes() : std::span<const space::MaterialRouteRow>{};
        const auto plan = additive.harness.plan();
        const auto& surface = plan.surfaces.front();
        expect(plan.status == space::PlanStatus::surface_rejected
               && surface.causes == std::vector<space::SurfaceStatus>{space::SurfaceStatus::unconsumed_parameter,
                                                                      space::SurfaceStatus::shader_not_qualified}
               && !surface.material && !surface.meshadditive && surface.field_dispositions.empty(),
               std::string(shipped ? "shipped routes" : "no routes") + ": MeshAdditive keeps the ledger's causes");
        const auto ledger = space::plan_surfaces(additive.harness.model, space::qualifications());
        expect(ledger.front().causes == surface.causes, "the two-argument planner agrees");
    }

    // Accepted: authored values exactly as parsed, normals not required.
    {
        AdditiveHarness additive;
        const auto plan = additive.harness.plan();
        expect(plan.status == space::PlanStatus::ready && plan.accepted_count() == 2,
               "both MeshAdditive surfaces are accepted: " + plan.detail);
        for (const auto& surface : plan.surfaces) {
            expect(surface.material == space::SkyMaterial::meshadditive && surface.meshadditive.has_value(),
                   "surface material is meshadditive with values");
            if (surface.meshadditive) {
                expect(same(surface.meshadditive->color, authored_color)
                       && surface.meshadditive->color_kind == ParameterKind::vector4
                       && same(surface.meshadditive->uv_scroll_rate, authored_scroll),
                       "Color and UVScrollRate are the parsed values bit for bit, unused components included");
            }
            expect(!surface.meshgloss && surface.unconsumed_parameters.empty(), "nothing is left unconsumed");
            expect(surface.field_dispositions.size() == 3, "every field carries its disposition");
            expect(surface.parameters == std::vector<std::string>{"Color:vector4", "UVScrollRate:vector4",
                                                                  "BaseTexture:texture"},
                   "parameters keep stored order");
            expect(surface.texture && surface.model, "an accepted surface carries texture and geometry");
        }
        expect(additive.harness.texture_requests == std::vector<std::string>{"eawr_space_a.tga", "eawr_space_b.dds"},
               "each MeshAdditive surface binds its own BaseTexture");
    }
    {
        // Color is declared float3: a float3 chunk is accepted and w is recorded as 0.
        AdditiveHarness additive;
        additive.first().parameters[0] = {"Color", ParameterKind::vector3, Vec3f{0.25F, 0.5F, 2.0F}};
        const auto plan = additive.harness.plan();
        const auto& surface = plan.surfaces.front();
        expect(plan.status == space::PlanStatus::ready && surface.meshadditive
               && surface.meshadditive->color_kind == ParameterKind::vector3
               && same(surface.meshadditive->color, {0.25F, 0.5F, 2.0F, 0.0F}),
               "a float3 Color is accepted as authored, unclamped");
    }
    {
        // A MeshGloss sibling keeps its own route under the opt-in table.
        AdditiveHarness additive;
        auto& sibling = additive.harness.model.meshes[1].submeshes[0];
        sibling.parameters = {{"BaseTexture", ParameterKind::texture, std::string("eawr_space_b.dds")}};
        make_meshgloss(sibling);
        const auto plan = additive.harness.plan();
        expect(plan.status == space::PlanStatus::ready && plan.surfaces[0].material == space::SkyMaterial::meshadditive
               && plan.surfaces[1].material == space::SkyMaterial::meshgloss && plan.surfaces[1].meshgloss,
               "MeshGloss and MeshAdditive surfaces plan side by side");
        Harness diffuse;
        diffuse.routes = space::meshadditive_material_routes();
        const auto kept = diffuse.plan();
        expect(kept.status == space::PlanStatus::ready && kept.surfaces[0].material == space::SkyMaterial::opaque_diffuse,
               "a qualified diffuse shader keeps the opaque diffuse contract");
    }
}

void meshadditive_identity_cases() {
    // Identity: case-insensitive exact name only.
    {
        AdditiveHarness additive;
        additive.first().shader = "meshadditive.FX";
        expect(additive.harness.plan().surfaces.front().status == space::SurfaceStatus::accepted,
               "the identity compare is case-insensitive like every row");
        for (const std::string shader : {"MeshAdditive.fxo", "MeshAdditiveVColor.fx", "MeshAdditiveOffset.fx",
                                         "MeshAdditive", "Dev/MeshAdditiveBloom.fx"}) {
            AdditiveHarness other;
            other.first().shader = shader;
            const auto plan = other.harness.plan();
            const auto& surface = plan.surfaces.front();
            expect(!surface.material && std::find(surface.causes.begin(), surface.causes.end(),
                                                   space::SurfaceStatus::shader_not_qualified) != surface.causes.end(),
                   shader + " is not admitted by the MeshAdditive route");
        }
    }
}

void meshadditive_bone_and_texture_cases() {
    // Non-billboard only: every billboard mode, sun (7) included, is rejected.
    for (std::uint32_t mode = 1; mode <= 7; ++mode) {
        AdditiveHarness additive;
        additive.harness.model.bones.front().billboard = mode;
        expect_additive_rejected(additive, space::SurfaceStatus::hierarchy_unsupported,
                                 "MeshAdditive on billboard mode " + std::to_string(mode));
    }
    {
        // A mode-7 sun under a visible identity root, as the ledger's sun bones are.
        AdditiveHarness additive;
        auto& model = additive.harness.model;
        eawr::assets::Bone sun;
        sun.name = "Sun";
        sun.parent = 0;
        sun.billboard = 7;
        sun.relative_transform = {1, 0, 0, 1000, 0, 1, 0, 0, 0, 0, 1, 0};
        model.bones.push_back(sun);
        model.meshes[1].bone = 1;
        const auto plan = additive.harness.plan();
        expect(plan.status == space::PlanStatus::surface_rejected
               && plan.surfaces[0].status == space::SurfaceStatus::accepted
               && plan.surfaces[1].status == space::SurfaceStatus::hierarchy_unsupported && !plan.surfaces[1].model,
               "a mode-7 MeshAdditive sun stays hierarchy_unsupported and blocks the plan");
    }
    {
        AdditiveHarness additive;
        additive.harness.model.bones.front().relative_transform = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0};
        expect_additive_rejected(additive, space::SurfaceStatus::hierarchy_unsupported, "MeshAdditive on a scaled bone");
    }
    {
        AdditiveHarness additive;
        additive.first().skin_bones = {0};
        expect_additive_rejected(additive, space::SurfaceStatus::skinning_unsupported, "skinned MeshAdditive");
    }
    {
        AdditiveHarness additive;
        additive.harness.model.bones.front().visible = false;
        expect_additive_rejected(additive, space::SurfaceStatus::bone_visibility_unsupported,
                                 "MeshAdditive on a hidden bone");
    }
    {
        AdditiveHarness additive;
        additive.harness.missing_texture = "eawr_space_a.tga";
        expect_additive_rejected(additive, space::SurfaceStatus::texture_not_in_vfs, "MeshAdditive texture not in VFS");
    }
    {
        // A proper rigid chain is baked into positions exactly as for every route.
        AdditiveHarness additive;
        auto& model = additive.harness.model;
        model.bones.front().relative_transform = {0, -1, 0, 17, 1, 0, 0, 11, 0, 0, 1, 7};
        const Vec3f position = model.meshes[0].submeshes[0].vertices[0].position;
        const auto plan = additive.harness.plan();
        expect(plan.status == space::PlanStatus::ready, "a rigid MeshAdditive chain is accepted: " + plan.detail);
        if (!plan.surfaces.empty() && plan.surfaces.front().model) {
            const auto& vertex = plan.surfaces.front().model->meshes.front().submeshes.front().vertices.front();
            expect(near(vertex.position.x, -position.y + 17.0F) && near(vertex.position.y, position.x + 11.0F)
                   && near(vertex.position.z, position.z + 7.0F), "positions carry the rigid chain");
        }
    }
}

void meshadditive_parameter_cases() {
    // BaseTexture rules are the contract's own.
    {
        AdditiveHarness additive;
        additive.first().parameters.pop_back();
        expect_additive_rejected(additive, space::SurfaceStatus::base_texture_missing, "MeshAdditive without BaseTexture");
    }

    // Each value field: missing, duplicated, wrong kind, kind/value mismatch, non-finite.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (std::size_t slot = 0; slot < 2; ++slot) {
        const std::string name = slot == 0 ? "Color" : "UVScrollRate";
        {
            AdditiveHarness additive;
            auto& parameters = additive.first().parameters;
            parameters.erase(parameters.begin() + static_cast<std::ptrdiff_t>(slot));
            expect_additive_rejected(additive, space::SurfaceStatus::material_parameter_missing, name + " missing");
            const auto plan = additive.harness.plan();
            expect(!plan.surfaces.front().meshadditive, name + " missing: no default is substituted");
            expect(plan.surfaces.front().detail.find(name) != std::string::npos, name + " missing: detail names it");
        }
        {
            AdditiveHarness additive;
            auto& parameters = additive.first().parameters;
            parameters.push_back(parameters[slot]);
            expect_additive_rejected(additive, space::SurfaceStatus::material_parameter_invalid, name + " duplicated");
        }
        {
            AdditiveHarness additive;
            additive.first().parameters[slot] = {name, ParameterKind::scalar, 1.0F};
            expect_additive_rejected(additive, space::SurfaceStatus::material_parameter_invalid, name + " as a scalar");
        }
        {
            AdditiveHarness additive;
            additive.first().parameters[slot].value = std::int32_t{1};
            expect_additive_rejected(additive, space::SurfaceStatus::material_parameter_invalid,
                                     name + " kind/value mismatch");
        }
        for (const float bad : {nan, inf, -inf}) {
            AdditiveHarness additive;
            auto& parameter = additive.first().parameters[slot];
            Vec4f value = std::get<Vec4f>(parameter.value);
            // The unread components must still be finite.
            value.w = bad;
            parameter.value = value;
            expect_additive_rejected(additive, space::SurfaceStatus::material_value_nonfinite, name + " non-finite w");
        }
    }
    {
        // UVScrollRate is only ever a float4 chunk.
        AdditiveHarness additive;
        additive.first().parameters[1] = {"UVScrollRate", ParameterKind::vector3, Vec3f{0.25F, 0.0F, 0.0F}};
        expect_additive_rejected(additive, space::SurfaceStatus::material_parameter_invalid, "UVScrollRate as vector3");
    }
    {
        AdditiveHarness additive;
        additive.first().parameters[0] = {"Color", ParameterKind::vector3, Vec3f{nan, 0.0F, 0.0F}};
        expect_additive_rejected(additive, space::SurfaceStatus::material_value_nonfinite, "float3 Color non-finite");
    }
    {
        AdditiveHarness additive;
        additive.first().parameters.push_back({"Emissive", ParameterKind::vector4, authored_color});
        expect_additive_rejected(additive, space::SurfaceStatus::unconsumed_parameter, "MeshAdditive with Emissive");
    }
    {
        AdditiveHarness additive;
        additive.first().parameters.push_back({"CloudTexture", ParameterKind::texture, std::string("cloud.tga")});
        expect_additive_rejected(additive, space::SurfaceStatus::multitexture_required, "MeshAdditive with a second texture");
    }
}

} // namespace

void test_meshadditive_route() {
    meshadditive_route_table();
    meshadditive_admission_cases();
    meshadditive_identity_cases();
    meshadditive_bone_and_texture_cases();
    meshadditive_parameter_cases();
}

// The behaviour note's synthetic expected-outcome cases P-01..P-04, U-01, U-02.
void test_meshadditive_arithmetic() {
    const auto unit = space::meshadditive_default_inputs();
    const Vec4f texel{0.8F, 0.4F, 0.6F, 0.3F};
    space::MeshAdditiveMaterial material;
    material.color = {0.5F, 0.25F, 1.0F, 0.2F};

    // P-01: fragment = texel * Color.rgb, alpha = texel alpha; Color.w is inert.
    const auto p01 = space::meshadditive_fragment(space::meshadditive_vertex_color(material, unit), texel);
    expect(near4(p01, {0.4F, 0.1F, 0.6F, 0.3F}), "P-01 fragment (0.4, 0.1, 0.6, 0.3)");
    auto opaque_color = material;
    opaque_color.color.w = 0.9F;
    expect(same(space::meshadditive_fragment(space::meshadditive_vertex_color(opaque_color, unit), texel), p01),
           "P-01 Color alpha 0.2 versus 0.9 changes nothing");

    // P-02: LIGHT_SCALE.rgb * LIGHT_SCALE.a multiplies Color.rgb; alpha stays 1.
    auto scaled = unit;
    scaled.light_scale = {0.5F, 1.0F, 2.0F, 0.5F};
    space::MeshAdditiveMaterial p02_material;
    p02_material.color = {0.5F, 0.25F, 0.8F, 1.0F};
    const auto p02_vertex = space::meshadditive_vertex_color(p02_material, scaled);
    expect(near4(p02_vertex, {0.125F, 0.125F, 0.8F, 1.0F}), "P-02 vertex colour (0.125, 0.125, 0.8), alpha 1");
    expect(near4(space::meshadditive_fragment(p02_vertex, texel), {0.1F, 0.05F, 0.48F, 0.3F}),
           "P-02 fragment (0.1, 0.05, 0.48, 0.3)");
    scaled.light_scale.w = 0.0F;
    expect(near4(space::meshadditive_fragment(space::meshadditive_vertex_color(p02_material, scaled), texel),
                 {0.0F, 0.0F, 0.0F, 0.3F}), "P-02 light-scale alpha 0 gives rgb 0 and texel alpha");

    // P-03: saturation per vertex before the texel multiply.
    auto doubled = unit;
    doubled.light_scale = {2.0F, 2.0F, 2.0F, 1.0F};
    space::MeshAdditiveMaterial white;
    white.color = {1.0F, 1.0F, 1.0F, 0.0F};
    expect(near4(space::meshadditive_fragment(space::meshadditive_vertex_color(white, doubled), {0.5F, 0.5F, 0.5F, 1.0F}),
                 {0.5F, 0.5F, 0.5F, 1.0F}), "P-03 an over-unit product contributes 1, so the fragment is 0.5");
    space::MeshAdditiveMaterial negative;
    negative.color = {-0.5F, 0.5F, 3.0F, 0.0F};
    expect(near4(space::meshadditive_vertex_color(negative, unit), {0.0F, 0.5F, 1.0F, 1.0F}),
           "P-03 a negative channel contributes 0 and an over-unit one 1");

    // P-04: ONE/ONE ADD into an RGBA UNORM target, separate alpha off.
    expect(near4(space::additive_blend({0.1F, 0.2F, 0.3F, 0.4F}, p01), {0.5F, 0.3F, 0.9F, 0.7F}),
           "P-04 destination (0.1, 0.2, 0.3, 0.4) gives (0.5, 0.3, 0.9, 0.7)");
    expect(near4(space::additive_blend({0.7F, 0.95F, 0.2F, 0.9F}, p01), {1.0F, 1.0F, 0.8F, 1.0F}),
           "P-04 destination (0.7, 0.95, 0.2, 0.9) saturates to (1, 1, 0.8, 1)");
    const auto clear_alpha = space::meshadditive_fragment(space::meshadditive_vertex_color(material, unit),
                                                          {0.8F, 0.4F, 0.6F, 0.0F});
    const auto blended = space::additive_blend({0.1F, 0.2F, 0.3F, 0.4F}, clear_alpha);
    expect(near(blended.x, 0.5F) && near(blended.y, 0.3F) && near(blended.z, 0.9F),
           "P-04 texel alpha 0 gives the same rgb");

    // U-01: scroll at two times; z and w never matter; wrap per sample.
    const Vec4f rate{0.25F, -0.5F, 7.0F, 9.0F};
    const Vec4f rate_xy{0.25F, -0.5F, 0.0F, 0.0F};
    const eawr::assets::Vec2f uv{0.1F, 0.8F};
    const auto at = [&](const Vec4f& r, const float t) { return space::meshadditive_uv(uv, r, t); };
    expect(near(at(rate, 0).x, 0.1F) && near(at(rate, 0).y, 0.8F), "U-01 t = 0 gives (0.1, 0.8)");
    expect(near(at(rate, 2).x, 0.6F) && near(at(rate, 2).y, -0.2F), "U-01 t = 2 unwrapped (0.6, -0.2)");
    expect(near(space::wrap_uv(at(rate, 2)).x, 0.6F) && near(space::wrap_uv(at(rate, 2)).y, 0.8F),
           "U-01 t = 2 sampled (0.6, 0.8)");
    expect(near(at(rate, 3).x, 0.85F) && near(at(rate, 3).y, -0.7F), "U-01 t = 3 unwrapped (0.85, -0.7)");
    expect(near(space::wrap_uv(at(rate, 3)).x, 0.85F) && near(space::wrap_uv(at(rate, 3)).y, 0.3F),
           "U-01 t = 3 sampled (0.85, 0.3)");
    for (const float t : {0.0F, 2.0F, 3.0F}) {
        expect(at(rate, t).x == at(rate_xy, t).x && at(rate, t).y == at(rate_xy, t).y, "U-01 z and w have no effect");
    }

    // U-02: the point at local (-0.6h, 0, 0.6h) on the (+-h, 0, +-h) quad.
    const float h = 1.0F;
    const eawr::assets::Vec2f point{(-0.6F * h / h + 1.0F) * 0.5F, (1.0F - 0.6F * h / h) * 0.5F};
    const auto quadrant = [](const eawr::assets::Vec2f& value) {
        const auto wrapped = space::wrap_uv(value);
        return (wrapped.x >= 0.5F ? 1 : 0) + (wrapped.y >= 0.5F ? 2 : 0);
    };
    const Vec4f u_rate{0.25F, 0.0F, 0.0F, 0.0F};
    expect(quadrant(space::meshadditive_uv(point, u_rate, 0.0F)) == 0
           && quadrant(space::meshadditive_uv(point, u_rate, 1.0F)) == 0
           && quadrant(space::meshadditive_uv(point, u_rate, 2.0F)) == 1,
           "U-02 top-left at t = 0 and t = 1, top-right at t = 2");
}


} // namespace eawr_space_test
