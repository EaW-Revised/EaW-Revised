#include "space_test_support.hpp"

namespace eawr_space_test {

// -- MeshGloss sky route -------------------------------------------------------

using eawr::assets::ParameterKind;
using eawr::assets::Vec4f;

constexpr const char* meshgloss_shader = "MeshGloss.fx";
constexpr Vec4f authored_emissive{0.5F, 0.375F, 0.25F, 1.0F};
constexpr Vec4f authored_diffuse{0.75F, 0.5F, 0.25F, 0.5F};
constexpr Vec4f authored_specular{0.125F, 0.25F, 0.625F, 1.0F};
constexpr float authored_shininess = 24.0F;

bool same(const Vec4f& a, const Vec4f& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
bool near(const float a, const float b, const float tolerance = 1.0e-5F) { return std::abs(a - b) <= tolerance; }

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

void test_environment_effect_routes() {
    using eawr::assets::MaterialParameter;
    using eawr::assets::ParameterKind;
    using eawr::assets::Vec4f;
    const auto binding_names = [](const space::EnvironmentEffectPlan& plan) {
        std::vector<std::string> names;
        for (const auto& binding : plan.material.bindings) names.push_back(binding.name);
        return names;
    };
    space::EnvironmentEffectInputs inputs;
    inputs.id = "synthetic-explicit-inputs";
    inputs.time_seconds = 0.25F;
    inputs.light_scale = {1, 1, 1, 1};
    inputs.light_direction = {1, 0, 0};
    inputs.ambient_light = {0.2F, 0.2F, 0.2F};
    inputs.diffuse_light = {0.8F, 0.8F, 0.8F};
    inputs.specular_light = {1, 1, 1};

    auto model = sky_model();
    auto& surface = model.meshes[0].submeshes[0];
    for (auto& vertex : surface.vertices) {
        vertex.normal = {1, 0, 0};
        vertex.color = {0.5F, 0.25F, 0.75F, 1};
    }
    surface.shader = "Planet.fx";
    surface.vertex_format = "alD3dVertNU2U3U3";
    surface.parameters.push_back({"CloudTexture", ParameterKind::texture, std::string("cloud.tga")});
    surface.parameters.push_back({"Emissive", ParameterKind::vector3, Vec3f{0.1F, 0.2F, 0.3F}});
    surface.parameters.push_back({"Diffuse", ParameterKind::vector3, Vec3f{0.6F, 0.7F, 0.8F}});
    surface.parameters.push_back({"Specular", ParameterKind::vector3, Vec3f{0.3F, 0.2F, 0.1F}});
    surface.parameters.push_back({"CloudScrollRate", ParameterKind::scalar, 0.4F});
    const auto planet = space::plan_environment_effect(model, 0, 0, "t0", inputs);
    expect(planet.status == space::EnvironmentEffectStatus::ready && planet.route_id == space::planet_route_id
               && planet.pass_name == "t0_p0" && planet.textures.size() == 2
               && planet.material.pass == eawr::presentation::RenderPass::opaque
               && planet.fields.size() == 6,
           "Planet t0 binds two textures and three authored colors under explicit lighting");
    expect(std::any_of(planet.fields.begin(), planet.fields.end(), [](const auto& field) {
        return field.name == "CloudScrollRate" && field.disposition == "recorded_not_consumed";
    }), "Planet t0 records its unused cloud scroll field");
    auto mixed_planet = model;
    auto& mixed_planet_parameters = mixed_planet.meshes[0].submeshes[0].parameters;
    mixed_planet_parameters[0].name = "basetexture";
    mixed_planet_parameters[1].name = "cLoUdTeXtUrE";
    mixed_planet_parameters[2].name = "emissive";
    mixed_planet_parameters[3].name = "DIFFUSE";
    mixed_planet_parameters[4].name = "sPeCuLaR";
    mixed_planet_parameters[5].name = "cloudscrollrate";
    const auto canonical_planet = space::plan_environment_effect(mixed_planet, 0, 0, "t0", inputs);
    expect(canonical_planet.status == space::EnvironmentEffectStatus::ready
               && canonical_planet.textures == std::vector<std::pair<std::string, std::string>>{
                   {"BaseTexture", "eawr_space_a.tga"}, {"CloudTexture", "cloud.tga"}}
               && binding_names(canonical_planet) == std::vector<std::string>{
                   "BaseTexture", "CloudTexture", "Emissive", "Diffuse", "Specular",
                   "eawr_effect_time", "eawr_effect_light_scale", "eawr_effect_light_direction",
                   "eawr_effect_ambient", "eawr_effect_diffuse", "eawr_effect_specular"}
               && canonical_planet.fields[5].name == "CloudScrollRate",
           "Planet mixed-case fields bind the canonical shader uniform names");
    expect(space::plan_environment_effect(model, 0, 0, "sph_t2", inputs).status
               == space::EnvironmentEffectStatus::technique_unsupported,
           "Planet programmable technique stays closed");
    inputs.light_direction = {0, 0, 0};
    expect(space::plan_environment_effect(model, 0, 0, "t0", inputs).status
               == space::EnvironmentEffectStatus::input_invalid,
           "Planet rejects a zero light direction");
    inputs.light_direction = {1, 0, 0};
    inputs.light_scale.w = 0.5F;
    const auto blended = space::plan_environment_effect(model, 0, 0, "t0", inputs);
    expect(blended.status == space::EnvironmentEffectStatus::ready
               && blended.material.pass == eawr::presentation::RenderPass::transparent
               && blended.material.program.find("ALPHA =") != std::string::npos,
           "Planet alpha branch selects explicit SRCALPHA blend path");
    inputs.light_scale.w = 1;
    surface.parameters.push_back({"CloudTexture", ParameterKind::texture, std::string("duplicate.tga")});
    expect(space::plan_environment_effect(model, 0, 0, "t0", inputs).status
               == space::EnvironmentEffectStatus::field_invalid, "duplicate Planet texture blocks");
    surface.parameters.pop_back();
    surface.parameters.erase(surface.parameters.begin() + 1);
    expect(space::plan_environment_effect(model, 0, 0, "t0", inputs).status
               == space::EnvironmentEffectStatus::field_missing, "missing Planet cloud texture blocks");

    surface.shader = "Nebula.fx";
    surface.vertex_format = "alD3dVertNU2C";
    surface.parameters.clear();
    surface.parameters.push_back({"BaseTexture", ParameterKind::texture, std::string("nebula.tga")});
    surface.parameters.push_back({"UVScrollRate", ParameterKind::vector4, Vec4f{0.25F, -0.5F, 0, 0}});
    surface.parameters.push_back({"DistortionScale", ParameterKind::scalar, 2.0F});
    surface.parameters.push_back({"SFreq", ParameterKind::scalar, 0.1F});
    surface.parameters.push_back({"TFreq", ParameterKind::scalar, 0.2F});
    const auto nebula = space::plan_environment_effect(model, 0, 0, "t0", inputs);
    expect(nebula.status == space::EnvironmentEffectStatus::ready && nebula.route_id == space::nebula_route_id
               && nebula.textures.size() == 1
               && nebula.material.pass == eawr::presentation::RenderPass::transparent
               && nebula.material.program.find("blend_add") != std::string::npos,
           "Nebula t0 binds authored scroll, distortion, and additive state");
    auto mixed_nebula = model;
    auto& mixed_nebula_parameters = mixed_nebula.meshes[0].submeshes[0].parameters;
    mixed_nebula_parameters[0].name = "basetexture";
    mixed_nebula_parameters[1].name = "uvscrollrate";
    mixed_nebula_parameters[2].name = "DISTORTIONSCALE";
    mixed_nebula_parameters[3].name = "sfreq";
    mixed_nebula_parameters[4].name = "tFrEq";
    const auto canonical_nebula = space::plan_environment_effect(mixed_nebula, 0, 0, "t0", inputs);
    expect(canonical_nebula.status == space::EnvironmentEffectStatus::ready
               && canonical_nebula.textures == std::vector<std::pair<std::string, std::string>>{
                   {"BaseTexture", "nebula.tga"}}
               && binding_names(canonical_nebula) == std::vector<std::string>{
                   "BaseTexture", "UVScrollRate", "DistortionScale", "SFreq", "TFreq",
                   "eawr_effect_time", "eawr_effect_light_scale"},
           "Nebula mixed-case fields bind the canonical shader uniform names");
    surface.parameters[2].value = std::numeric_limits<float>::infinity();
    expect(space::plan_environment_effect(model, 0, 0, "t0", inputs).status
               == space::EnvironmentEffectStatus::field_nonfinite, "nonfinite Nebula distortion blocks");
    surface.parameters[2].value = 2.0F;
    surface.parameters.push_back({"OtherTexture", ParameterKind::texture, std::string("other.tga")});
    expect(space::plan_environment_effect(model, 0, 0, "t0", inputs).status
               == space::EnvironmentEffectStatus::field_invalid, "unreviewed Nebula texture blocks");
    surface.parameters.pop_back();
    surface.vertex_format = "alD3dVertNU2";
    expect(space::plan_environment_effect(model, 0, 0, "t0", inputs).status
               == space::EnvironmentEffectStatus::geometry_invalid, "Nebula requires NU2C vertex colour");
    surface.vertex_format = "alD3dVertNU2C";
    model.bones[0].billboard = 7;
    expect(space::plan_environment_effect(model, 0, 0, "t0", inputs).status
               == space::EnvironmentEffectStatus::hierarchy_unsupported, "placed Nebula billboard stays closed");
    model.bones[0].billboard = 0;
    surface.shader = "Nebula.fxo";
    expect(space::plan_environment_effect(model, 0, 0, "t0", inputs).status
               == space::EnvironmentEffectStatus::shader_unsupported, "Nebula fxo stays closed");
    surface.shader = "Nebula.fx";
    inputs.id.clear();
    expect(space::plan_environment_effect(model, 0, 0, "t0", inputs).status
               == space::EnvironmentEffectStatus::input_invalid, "unnamed external clock and light state blocks");
}


void test_environment_effect_clock() {
    // #185: retail's scene clock gains LogicalFPS / 1000 = 0.03 s per 30 Hz
    // sim frame and wraps at 28800 s; one presentation tick stands for one.
    expect(space::environment_effect_time(0) == 0.0F, "the effect clock starts at zero");
    expect(near(space::environment_effect_time(1), 0.03F), "one tick is 0.03 s of effect time");
    expect(near(space::environment_effect_time(30), 0.9F), "a second of 30 Hz ticks is 0.9 s of effect time");
    expect(near(space::environment_effect_time(59), 1.77F), "the default held capture tick");
    expect(near(space::environment_effect_time(space::effect_clock_wrap_ticks - 1), 28799.97F, 1.0e-2F),
           "the clock runs up to its wrap");
    expect(space::environment_effect_time(space::effect_clock_wrap_ticks) == 0.0F, "the clock wraps at 28800 s");
    expect(space::environment_effect_time(space::effect_clock_wrap_ticks + 30)
               == space::environment_effect_time(30), "a wrapped clock repeats");
    expect(near(static_cast<float>(space::effect_clock_wrap_ticks * space::effect_clock_seconds_per_tick),
                28800.0F), "the wrap is 28800 s");
}

} // namespace eawr_space_test
