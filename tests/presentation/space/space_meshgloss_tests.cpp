#include "space_effect_support.hpp"

namespace eawr_space_test {

// -- MeshGloss sky route -------------------------------------------------------

using eawr::assets::ParameterKind;
using eawr::assets::Vec4f;

constexpr const char* meshgloss_shader = "MeshGloss.fx";
constexpr Vec4f authored_emissive{0.5F, 0.375F, 0.25F, 1.0F};
constexpr Vec4f authored_diffuse{0.75F, 0.5F, 0.25F, 0.5F};
constexpr Vec4f authored_specular{0.125F, 0.25F, 0.625F, 1.0F};
constexpr float authored_shininess = 24.0F;

// Converts a synthetic surface to MeshGloss in the authored field order the
// ledger records (Emissive, Diffuse, Specular, Shininess, BaseTexture), with
// the quad's face normal on every vertex.
void make_meshgloss(eawr::assets::Submesh& submesh) {
    const std::string texture = std::get<std::string>(submesh.parameters.front().value);
    submesh.shader = meshgloss_shader;
    submesh.parameters = {
        {"Emissive", ParameterKind::vector4, authored_emissive},
        {"Diffuse", ParameterKind::vector4, authored_diffuse},
        {"Specular", ParameterKind::vector4, authored_specular},
        {"Shininess", ParameterKind::scalar, authored_shininess},
        {"BaseTexture", ParameterKind::texture, texture},
    };
    const Vec3f a = submesh.vertices[0].position;
    const Vec3f b = submesh.vertices[2].position;
    const Vec3f c = submesh.vertices[6].position;
    const Vec3f u{b.x - a.x, b.y - a.y, b.z - a.z};
    const Vec3f v{c.x - a.x, c.y - a.y, c.z - a.z};
    Vec3f n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    n = {n.x / length, n.y / length, n.z / length};
    for (auto& vertex : submesh.vertices) vertex.normal = n;
}

struct GlossHarness final {
    Harness harness;
    GlossHarness() {
        for (auto& mesh : harness.model.meshes) make_meshgloss(mesh.submeshes.front());
        harness.routes = space::material_routes();
    }
    eawr::assets::Submesh& first() { return harness.model.meshes[0].submeshes[0]; }
};

void expect_gloss_rejected(GlossHarness& gloss, const space::SurfaceStatus cause, const std::string& label) {
    const auto plan = gloss.harness.plan();
    expect(plan.status == space::PlanStatus::surface_rejected, label + ": the plan is not ready");
    if (plan.surfaces.empty()) return;
    const auto& surface = plan.surfaces.front();
    expect(surface.status == cause, label + ": first cause is " + std::string(space::to_string(cause))
        + ", got " + std::string(space::to_string(surface.status)));
    expect(!surface.texture && !surface.model, label + ": a rejected surface has no upload payload");
    expect(surface.material == space::SkyMaterial::meshgloss, label + ": the route still names the matched material");
}

namespace {

void meshgloss_route_table() {
    // The route table: one exact identity, nothing else admitted by name.
    const auto routes = space::material_routes();
    expect(routes.size() == 1 && routes.front().shader == "MeshGloss.fx"
           && routes.front().material == space::SkyMaterial::meshgloss, "the only route row is exact MeshGloss.fx");
    const auto fields = space::meshgloss_fields();
    const std::vector<std::pair<std::string, std::string>> expected_fields{
        {"BaseTexture", "consumed"}, {"Emissive", "consumed_rgb"}, {"Diffuse", "consumed_rgb"},
        {"Specular", "consumed_rgb"}, {"Shininess", "recorded_not_consumed"}};
    expect(fields.size() == expected_fields.size(), "five MeshGloss field dispositions");
    for (std::size_t index = 0; index < std::min(fields.size(), expected_fields.size()); ++index) {
        expect(fields[index].name == expected_fields[index].first
               && fields[index].disposition == expected_fields[index].second && !fields[index].rule.empty(),
               "field disposition " + expected_fields[index].first);
    }
    const auto unlit = space::unlit_sky_policy();
    bool zero = unlit.light_specular.x == 0.0F && unlit.light_specular.y == 0.0F && unlit.light_specular.z == 0.0F;
    for (const auto& matrix : unlit.sph) for (const float value : matrix) zero = zero && value == 0.0F;
    expect(zero && same(unlit.light_scale, {1, 1, 1, 1}) && unlit.id == "space-sky-unlit-v1" && !unlit.cause.empty(),
           "the unlit sky policy composes no light and a unit light scale");
}

void meshgloss_admission_cases() {
    // Opt-in: without routes the planner is exactly the diffuse contract, so
    // the sky ledger's two-argument verdict is unchanged.
    {
        GlossHarness gloss;
        gloss.harness.routes = {};
        const auto plan = gloss.harness.plan();
        expect(plan.status == space::PlanStatus::surface_rejected, "no route: MeshGloss stays rejected");
        const auto& surface = plan.surfaces.front();
        expect(surface.causes == std::vector<space::SurfaceStatus>{space::SurfaceStatus::unconsumed_parameter,
                                                                   space::SurfaceStatus::shader_not_qualified},
               "no route: the ledger's causes (unconsumed_parameter, shader_not_qualified)");
        expect(!surface.material && !surface.meshgloss && surface.field_dispositions.empty(), "no route: no material");
        const auto ledger = space::plan_surfaces(gloss.harness.model, space::qualifications());
        expect(ledger.front().causes == surface.causes, "the two-argument planner agrees");
    }

    // Accepted: the authored values are carried exactly, nothing defaulted.
    {
        GlossHarness gloss;
        const auto plan = gloss.harness.plan();
        expect(plan.status == space::PlanStatus::ready && plan.accepted_count() == 2,
               "both MeshGloss surfaces are accepted: " + plan.detail);
        for (const auto& surface : plan.surfaces) {
            expect(surface.material == space::SkyMaterial::meshgloss, "surface material is meshgloss");
            expect(surface.meshgloss.has_value(), "authored values are present");
            if (surface.meshgloss) {
                expect(same(surface.meshgloss->emissive, authored_emissive)
                       && same(surface.meshgloss->diffuse, authored_diffuse)
                       && same(surface.meshgloss->specular, authored_specular)
                       && surface.meshgloss->shininess == authored_shininess,
                       "Emissive, Diffuse, Specular and Shininess are the parsed values bit for bit");
            }
            expect(surface.unconsumed_parameters == std::vector<std::string>{"Shininess"},
                   "only Shininess is recorded as not consumed");
            expect(surface.field_dispositions.size() == 5, "every field carries its disposition");
            expect(surface.parameters.size() == 5 && surface.parameters[0] == "Emissive:vector4"
                   && surface.parameters[3] == "Shininess:scalar" && surface.parameters[4] == "BaseTexture:texture",
                   "parameters keep stored order");
            expect(surface.texture && surface.model, "an accepted MeshGloss surface carries texture and geometry");
        }
        expect(gloss.harness.texture_requests == std::vector<std::string>{"eawr_space_a.tga", "eawr_space_b.dds"},
               "each MeshGloss surface binds its own BaseTexture");
    }

    // A qualification row wins over a route row; the diffuse fixture is unchanged.
    {
        Harness diffuse;
        diffuse.routes = space::material_routes();
        const auto plan = diffuse.plan();
        expect(plan.status == space::PlanStatus::ready && plan.accepted_count() == 2, "diffuse fixture with routes");
        for (const auto& surface : plan.surfaces) {
            expect(surface.material == space::SkyMaterial::opaque_diffuse && !surface.meshgloss
                   && surface.field_dispositions.empty(), "a qualified shader keeps the opaque diffuse contract");
        }
    }

    // Identity: case-insensitive exact name only; `.fxo` and MeshAdditive are not MeshGloss.
    {
        GlossHarness gloss;
        gloss.first().shader = "meshgloss.FX";
        expect(gloss.harness.plan().surfaces.front().status == space::SurfaceStatus::accepted,
               "the identity compare is case-insensitive like every row");
        for (const std::string shader : {"MeshGloss.fxo", "MeshGlossForStarfields.fxo", "MeshAdditive.fx", "MeshGloss"}) {
            GlossHarness other;
            other.first().shader = shader;
            const auto plan = other.harness.plan();
            const auto& surface = plan.surfaces.front();
            expect(!surface.material && surface.status == space::SurfaceStatus::unconsumed_parameter
                   && std::find(surface.causes.begin(), surface.causes.end(),
                                space::SurfaceStatus::shader_not_qualified) != surface.causes.end(),
                   shader + " is not admitted by the MeshGloss route");
        }
    }
}

void meshgloss_base_texture_cases() {
    // BaseTexture rules are the contract's own.
    {
        GlossHarness gloss;
        gloss.first().parameters.pop_back();
        expect_gloss_rejected(gloss, space::SurfaceStatus::base_texture_missing, "MeshGloss without BaseTexture");
    }
    {
        GlossHarness gloss;
        gloss.first().parameters.back() = {"BaseTexture", ParameterKind::vector4, authored_emissive};
        expect_gloss_rejected(gloss, space::SurfaceStatus::base_texture_wrong_type, "MeshGloss BaseTexture as vector4");
    }
}

void meshgloss_value_field_cases() {
    // Every value field: missing, ill-typed, duplicated and non-finite.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (std::size_t slot = 0; slot < 4; ++slot) {
        const std::string name = std::string(space::meshgloss_fields()[slot + 1].name);
        {
            GlossHarness gloss;
            auto& parameters = gloss.first().parameters;
            parameters.erase(parameters.begin() + static_cast<std::ptrdiff_t>(slot));
            expect_gloss_rejected(gloss, space::SurfaceStatus::material_parameter_missing, name + " missing");
            const auto plan = gloss.harness.plan();
            expect(!plan.surfaces.front().meshgloss, name + " missing: no partial material");
            expect(plan.surfaces.front().detail.find(name) != std::string::npos, name + " missing: detail names it");
        }
        {
            GlossHarness gloss;
            auto& parameters = gloss.first().parameters;
            parameters.push_back(parameters[slot]);
            expect_gloss_rejected(gloss, space::SurfaceStatus::material_parameter_invalid, name + " duplicated");
        }
        {
            GlossHarness gloss;
            auto& parameter = gloss.first().parameters[slot];
            parameter = slot == 3 ? eawr::assets::MaterialParameter{name, ParameterKind::vector4, authored_emissive}
                                  : eawr::assets::MaterialParameter{name, ParameterKind::scalar, 1.0F};
            expect_gloss_rejected(gloss, space::SurfaceStatus::material_parameter_invalid, name + " wrong kind");
        }
        {
            // The declared kind agrees but the stored value does not.
            GlossHarness gloss;
            gloss.first().parameters[slot].value = std::int32_t{1};
            expect_gloss_rejected(gloss, space::SurfaceStatus::material_parameter_invalid, name + " kind/value mismatch");
        }
        for (const float bad : {nan, inf, -inf}) {
            GlossHarness gloss;
            auto& parameter = gloss.first().parameters[slot];
            if (slot == 3) {
                parameter.value = bad;
            } else {
                Vec4f value = std::get<Vec4f>(parameter.value);
                // w is not read by the arithmetic but must still be finite.
                value.w = bad;
                parameter.value = value;
            }
            expect_gloss_rejected(gloss, space::SurfaceStatus::material_value_nonfinite, name + " non-finite");
        }
    }
    {
        // Finite authored values are consumed as authored, even when unusual.
        GlossHarness gloss;
        gloss.first().parameters[0].value = Vec4f{-0.25F, 4.0F, 0.0F, 0.0F};
        gloss.first().parameters[3].value = 0.0F;
        const auto plan = gloss.harness.plan();
        expect(plan.status == space::PlanStatus::ready && plan.surfaces.front().meshgloss
               && same(plan.surfaces.front().meshgloss->emissive, {-0.25F, 4.0F, 0.0F, 0.0F}),
               "finite values are neither clamped nor rejected");
    }
}

void meshgloss_extra_parameter_cases() {
    // Other authored parameters remain unreviewed; another texture needs multitexture.
    {
        GlossHarness gloss;
        gloss.first().parameters.push_back({"UVScrollRate", ParameterKind::vector4, authored_emissive});
        expect_gloss_rejected(gloss, space::SurfaceStatus::unconsumed_parameter, "MeshGloss with UVScrollRate");
    }
    {
        GlossHarness gloss;
        gloss.first().parameters.push_back({"CloudTexture", ParameterKind::texture, std::string("cloud.tga")});
        expect_gloss_rejected(gloss, space::SurfaceStatus::multitexture_required, "MeshGloss with a second texture");
    }
}

void meshgloss_geometry_cases() {
    // Geometry: MeshGloss needs a defined normal direction per vertex.
    {
        GlossHarness gloss;
        gloss.first().vertices[4].normal = {0.0F, 0.0F, 0.0F};
        expect_gloss_rejected(gloss, space::SurfaceStatus::geometry_invalid, "MeshGloss zero normal");
        Harness diffuse;
        diffuse.model.meshes[0].submeshes[0].vertices[4].normal = {0.0F, 0.0F, 0.0F};
        expect(diffuse.plan().status == space::PlanStatus::ready, "the diffuse contract does not read normals");
    }

    // Rigid hierarchy: the chain is baked into positions and normals.
    {
        GlossHarness gloss;
        auto& model = gloss.harness.model;
        model.bones.front().relative_transform = {1, 0, 0, 17, 0, 1, 0, 11, 0, 0, 1, 7};
        eawr::assets::Bone child;
        child.name = "Sky";
        child.parent = 0;
        // 90 degrees about source z, then translated.
        child.relative_transform = {0, -1, 0, -9, 1, 0, 0, 13, 0, 0, 1, -5};
        model.bones.push_back(child);
        for (auto& mesh : model.meshes) mesh.bone = 1;
        const Vec3f position = model.meshes[0].submeshes[0].vertices[0].position;
        const Vec3f normal = model.meshes[0].submeshes[0].vertices[0].normal;
        const auto plan = gloss.harness.plan();
        expect(plan.status == space::PlanStatus::ready && plan.accepted_count() == 2,
               "a rigid rotated MeshGloss chain is accepted: " + plan.detail);
        if (!plan.surfaces.empty() && plan.surfaces.front().model) {
            const auto& baked = plan.surfaces.front().model->meshes.front();
            const auto& vertex = baked.submeshes.front().vertices.front();
            expect(baked.bone == -1, "the rigid chain is baked, not kept");
            expect(near(vertex.position.x, -position.y - 9.0F + 17.0F) && near(vertex.position.y, position.x + 13.0F + 11.0F)
                   && near(vertex.position.z, position.z - 5.0F + 7.0F), "positions carry the composed rigid chain");
            expect(near(vertex.normal.x, -normal.y) && near(vertex.normal.y, normal.x) && near(vertex.normal.z, normal.z),
                   "normals carry the chain's rotation only");
        }
    }
}

void meshgloss_bone_and_texture_cases() {
    {
        GlossHarness gloss;
        gloss.harness.model.bones.front().billboard = 7;
        expect_gloss_rejected(gloss, space::SurfaceStatus::hierarchy_unsupported, "MeshGloss on a billboard bone");
    }
    {
        GlossHarness gloss;
        gloss.harness.model.bones.front().relative_transform = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0};
        expect_gloss_rejected(gloss, space::SurfaceStatus::hierarchy_unsupported, "MeshGloss on a scaled bone");
    }
    {
        GlossHarness gloss;
        gloss.harness.model.bones.front().visible = false;
        expect_gloss_rejected(gloss, space::SurfaceStatus::bone_visibility_unsupported, "MeshGloss on a hidden bone");
    }
    {
        GlossHarness gloss;
        gloss.first().skin_bones = {0};
        expect_gloss_rejected(gloss, space::SurfaceStatus::skinning_unsupported, "skinned MeshGloss");
    }
    {
        // A texture failure rejects exactly as the contract does; no grey pass.
        GlossHarness gloss;
        gloss.harness.missing_texture = "eawr_space_a.tga";
        expect_gloss_rejected(gloss, space::SurfaceStatus::texture_not_in_vfs, "MeshGloss texture not in the VFS");
    }
    {
        // A mixed model: one MeshGloss, one sun-like billboard. The MeshGloss
        // surface is accepted on its own, the plan is still blocked.
        GlossHarness gloss;
        auto& model = gloss.harness.model;
        eawr::assets::Bone sun;
        sun.name = "Sun";
        sun.parent = 0;
        sun.billboard = 7;
        sun.relative_transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        model.bones.push_back(sun);
        model.meshes[1].bone = 1;
        model.meshes[1].submeshes[0].shader = "MeshAdditive.fx";
        const auto plan = gloss.harness.plan();
        expect(plan.status == space::PlanStatus::surface_rejected, "a rejected sibling still blocks the plan");
        expect(plan.surfaces[0].status == space::SurfaceStatus::accepted
               && plan.surfaces[1].status == space::SurfaceStatus::hierarchy_unsupported,
               "surface verdicts are independent: MeshGloss accepted, billboard sibling rejected");
    }
}

} // namespace

void test_meshgloss_route() {
    meshgloss_route_table();
    meshgloss_admission_cases();
    meshgloss_base_texture_cases();
    meshgloss_value_field_cases();
    meshgloss_extra_parameter_cases();
    meshgloss_geometry_cases();
    meshgloss_bone_and_texture_cases();
}

void test_meshgloss_arithmetic() {
    space::MeshGlossMaterial material;
    material.diffuse = {0.5F, 0.25F, 1.0F, 1.0F};
    material.emissive = {0.1F, 0.0F, 0.05F, 1.0F};
    material.specular = {0.6F, 0.4F, 0.2F, 1.0F};
    material.shininess = 4.0F;

    // Case 1: irradiance is the quadratic form, with no clamp.
    std::array<float, 16> constant{};
    constant[15] = 0.3F;
    for (const Vec3f normal : {Vec3f{1, 0, 0}, Vec3f{0, -1, 0}, Vec3f{0, 0, 1}}) {
        expect(near(space::sph_irradiance(constant, normal), 0.3F), "bottom-right only gives a constant 0.3");
    }
    std::array<float, 16> skew{};
    skew[0 * 4 + 3] = 0.2F;
    skew[3 * 4 + 0] = 0.2F;
    expect(near(space::sph_irradiance(skew, {1, 0, 0}), 0.4F) && near(space::sph_irradiance(skew, {-1, 0, 0}), -0.4F),
           "the symmetric x/homogeneous pair gives +0.4 and -0.4, unclamped");

    // Case 3: aligned normal, eye and light give a unit cosine; Shininess is inert.
    space::SkyLightPolicy aligned = space::unlit_sky_policy();
    aligned.light_direction = {0, 0, 1};
    aligned.light_specular = {1.0F, 0.5F, 0.25F};
    const Vec3f n{0, 0, 1};
    const Vec3f origin{0, 0, 0};
    const auto lit = space::meshgloss_vertex(material, aligned, n, origin, {0, 0, 10});
    expect(near(lit.specular.x, 0.6F) && near(lit.specular.y, 0.2F) && near(lit.specular.z, 0.05F),
           "vertex specular is Specular times the light-zero specular at unit cosine");
    auto glossy = material;
    glossy.shininess = 64.0F;
    const auto same_lit = space::meshgloss_vertex(glossy, aligned, n, origin, {0, 0, 10});
    expect(same_lit.specular.x == lit.specular.x && same_lit.specular.y == lit.specular.y
           && same_lit.specular.z == lit.specular.z && same_lit.diffuse.x == lit.diffuse.x,
           "changing Shininess from 4 to 64 changes nothing");
    const float c = std::sqrt(3.0F) * 0.5F;
    space::SkyLightPolicy half = aligned;
    half.light_direction = {0, c, 0.5F};
    const auto grazing = space::meshgloss_vertex(material, half, n, origin, {0, c * 10.0F, 5.0F});
    expect(near(grazing.specular.x, 0.6F / 65536.0F, 1.0e-8F), "a half cosine gives the factor 1/65536");

    // Case 4: the full per-vertex and fragment equation.
    space::SkyLightPolicy scene = space::unlit_sky_policy();
    scene.sph[0][15] = 0.4F;
    scene.sph[1][15] = 0.8F;
    scene.sph[2][15] = 0.2F;
    scene.light_scale = {0.5F, 1.0F, 2.0F, 0.3F};
    scene.light_direction = {0, 0, 1};
    scene.light_specular = {1.0F, 0.5F, 0.25F};
    const auto vertex = space::meshgloss_vertex(material, scene, n, origin, {0, 0, 10});
    expect(near(vertex.diffuse.x, 0.2F) && near(vertex.diffuse.y, 0.2F) && near(vertex.diffuse.z, 0.45F)
           && near(vertex.diffuse.w, 0.3F), "vertex diffuse is Diffuse * irradiance * light scale + Emissive");
    const auto out = space::meshgloss_fragment(vertex, {0.25F, 0.5F, 1.0F, 0.4F});
    expect(near(out.x, 0.34F) && near(out.y, 0.28F) && near(out.z, 0.92F) && near(out.w, 0.3F),
           "fragment is 2 * diffuse * texel + specular * texel alpha, alpha from light scale");
    const auto matte = space::meshgloss_fragment(vertex, {0.25F, 0.5F, 1.0F, 0.0F});
    expect(near(matte.x, 0.1F) && near(matte.y, 0.2F) && near(matte.z, 0.9F) && near(matte.w, 0.3F),
           "texture alpha zero removes only the specular term");

    // The shipped sky policy: 2 * Emissive * texel whatever Diffuse, Specular or the normal.
    const auto unlit = space::unlit_sky_policy();
    for (const Vec3f normal : {Vec3f{0, 0, 1}, Vec3f{1, 0, 0}, Vec3f{0, -1, 0}}) {
        const auto sky = space::meshgloss_fragment(space::meshgloss_vertex(material, unlit, normal, {3, 4, 5}, origin),
                                                   {0.25F, 0.5F, 1.0F, 1.0F});
        expect(near(sky.x, 0.05F) && near(sky.y, 0.0F) && near(sky.z, 0.1F) && sky.w == 1.0F,
               "unlit sky output is 2 * Emissive.rgb * texel, opaque");
    }
}



} // namespace eawr_space_test
