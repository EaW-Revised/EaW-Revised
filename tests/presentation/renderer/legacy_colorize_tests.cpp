#include "legacy_family_support.hpp"

namespace legacy_family_test_support {

void gloss_colorize_contracts() {
    MaterialDescription material = selector(legacy::mesh_gloss_colorize::family, RenderPass::opaque);
    const std::string prefix = "EAWR-RENDER-0001 legacy material family 'MeshGlossColorize.fx' ";
    check(rejection(material) == prefix
            + "binds no GlossTexture; an unbound gloss sampler's result is not established",
        "MeshGlossColorize without GlossTexture fails closed");
    material.bindings = {
        {"Emissive", eawr::assets::Vec4f{0.1F, 0.2F, 0.3F, 1.0F}},
        {"Diffuse", eawr::assets::Vec4f{0.9F, 0.8F, 0.7F, 1.0F}},
        {"Specular", eawr::assets::Vec4f{0.5F, 0.5F, 0.5F, 1.0F}},
        {"Shininess", 12.0F},
        {"Colorization", eawr::assets::Vec4f{1.0F, 0.25F, 0.0F, 1.0F}},
        {"BaseTexture", std::string("ev_craft.tga")},
        {"GlossTexture", std::string("ev_craft_gloss.tga")},
    };
    check(rejection(material).empty() && legacy::shader_source(material).has_value(),
        "a distinct GlossTexture is admitted (the laser pads, #80)");
    const auto sampled = legacy::binding_textures(material);
    check(sampled.size() == 1 && sampled[0].name == "GlossTexture",
        "MeshGlossColorize's GlossTexture reaches the upload as its own texture (gate MULTITEX-01)");
    material.bindings[6].value = std::string("EV_CRAFT.TGA");
    check(rejection(material).empty() && legacy::shader_source(material).has_value(),
        "GlossTexture naming BaseTexture (case-insensitively) is admitted");
    const auto uniforms = legacy::uniforms(material);
    check(value_of<std::array<float, 3>>(uniforms, "eawr_emissive") == std::array<float, 3>{0.1F, 0.2F, 0.3F}
            && value_of<std::array<float, 3>>(uniforms, "eawr_diffuse") == std::array<float, 3>{0.9F, 0.8F, 0.7F}
            && value_of<std::array<float, 3>>(uniforms, "eawr_specular") == std::array<float, 3>{0.5F, 0.5F, 0.5F}
            && value_of<std::array<float, 3>>(uniforms, "eawr_colorization") == std::array<float, 3>{1.0F, 0.25F, 0.0F},
        "MeshGlossColorize binds Emissive, Diffuse, Specular and Colorization rgb");
    MaterialDescription changed = material;
    changed.bindings[6].value = 1.0F;
    check(rejection(changed) == prefix + "binds GlossTexture as a value that is not a texture",
        "a non-texture GlossTexture fails closed");
    changed = material;
    changed.bindings.push_back({"GlossTexture", std::string("ev_craft.tga")});
    check(rejection(changed) == prefix + "binds GlossTexture or BaseTexture more than once",
        "a duplicated GlossTexture fails closed");
    changed = material;
    changed.bindings[4].value = std::int32_t{3};
    check(rejection(changed) == prefix + "binds parameter 'Colorization' as an integer, not a float3 or float4",
        "a malformed Colorization (team colour) fails closed");
    changed = material;
    changed.pass = RenderPass::transparent;
    check(rejection(changed) == prefix + "cannot draw in the transparent render pass",
        "MeshGlossColorize has no transparent LIGHT_SCALE source and is opaque only");
}

void vegetation_contracts() {
    const auto& tree = legacy::tree::shader_source;
    check(tree.find("render_mode unshaded, fog_disabled, depth_draw_opaque, depth_test_default, cull_back;")
                != std::string_view::npos
            && tree.find("blend_") == std::string_view::npos
            && tree.find("ALPHA_SCISSOR_THRESHOLD = 128.5 / 255.0;") != std::string_view::npos
            && tree.find("filter_nearest_mipmap") != std::string_view::npos
            && tree.find("2.0 * eawr_vertex_diffuse.rgb * base_linear_rgb\n        + 2.0 * eawr_vertex_specular * base_sample.a")
                != std::string_view::npos,
        "Tree is alpha-tested at 128 without blending, point-filtered, 2x diffuse plus 2x alpha-masked specular");
    check(tree.find("uniform vec3 eawr_wind = vec3(0.0);") != std::string_view::npos
            && tree.find("uniform float eawr_scene_time = 0.0;") != std::string_view::npos
            && tree.find("sin(6.2831855 * fract(eawr_scene_time / 3.0) + phase)") != std::string_view::npos
            && tree.find("eawr_bend_scale * bend * (mesh_z * mesh_z / (box_height * box_height))")
                != std::string_view::npos
            && tree.find("VERTEX += inverse(mat3(MODEL_MATRIX)) * offset;") != std::string_view::npos
            && tree.find("TIME") == std::string_view::npos,
        "Tree bends by BendScale x bend x z^2 / H^2 on the bound scene clock, never Godot's TIME");
    MaterialDescription leaves = selector(legacy::tree::family, RenderPass::opaque);
    auto uniforms = legacy::uniforms(leaves);
    check(value_of<std::array<float, 3>>(uniforms, "eawr_diffuse") == std::array<float, 3>{1.0F, 1.0F, 1.0F}
            && value_of<std::array<float, 3>>(uniforms, "eawr_specular") == std::array<float, 3>{1.0F, 1.0F, 1.0F}
            && value_of<std::array<float, 3>>(uniforms, "eawr_emissive") == std::array<float, 3>{0.0F, 0.0F, 0.0F}
            && value_of<float>(uniforms, "eawr_bend_scale") == 1.0F,
        "unauthored Tree parameters keep the effect initializers");
    check(legacy::reads_wind(leaves), "Tree reads the scene wind");
    leaves.bindings = {
        {"Emissive", eawr::assets::Vec4f{0.0F, 0.0F, 0.0F, 0.0F}},
        {"Diffuse", eawr::assets::Vec4f{0.75F, 0.5F, 0.25F, 0.0F}},
        {"Specular", eawr::assets::Vec4f{0.0F, 0.0F, 0.0F, 0.0F}},
        {"Shininess", 32.0F},
        {"BendScale", 0.6F},
        {"BaseTexture", std::string("leaves.tga")},
        {"NormalTexture", std::string("None")},
    };
    check(rejection(leaves).empty() && legacy::shader_source(leaves).has_value(),
        "an authored Tree material (unread Shininess and NormalTexture) is admitted");
    check(value_of<std::array<float, 3>>(legacy::uniforms(leaves), "eawr_diffuse")
            == std::array<float, 3>{0.75F, 0.5F, 0.25F}
            && value_of<float>(legacy::uniforms(leaves), "eawr_bend_scale") == 0.6F,
        "Tree binds Diffuse.rgb and BendScale");
    leaves.bindings[4].value = std::int32_t{2};
    check(rejection(leaves).empty() && value_of<float>(legacy::uniforms(leaves), "eawr_bend_scale") == 2.0F,
        "an integer BendScale converts to a float");
    leaves.bindings[4].value = eawr::assets::Vec3f{1.0F, 1.0F, 1.0F};
    check(rejection(leaves).find("'Tree.fx' binds parameter 'BendScale' as a float3") != std::string::npos,
        "a vector BendScale fails closed");
    leaves.bindings[4].value = std::numeric_limits<float>::infinity();
    check(rejection(leaves).find("'Tree.fx' binds a non-finite value in parameter 'BendScale'") != std::string::npos,
        "a non-finite BendScale fails closed");
    leaves.bindings[4].value = 0.6F;
    leaves.bindings[1].value = 1.0F;
    check(rejection(leaves).find("'Tree.fx' binds parameter 'Diffuse' as a scalar") != std::string::npos,
        "a scalar Tree Diffuse fails closed");
    leaves.pass = RenderPass::transparent;
    leaves.bindings[1].value = eawr::assets::Vec4f{1.0F, 1.0F, 1.0F, 0.0F};
    check(rejection(leaves) == "EAWR-RENDER-0001 legacy material family 'Tree.fx' cannot draw in the transparent render pass",
        "Tree is admitted only as the alpha-tested opaque pass");

    const auto& grass = legacy::grass::shader_source;
    check(grass.find("render_mode unshaded, fog_disabled, blend_mix, depth_draw_always, depth_test_default, cull_disabled;")
                != std::string_view::npos
            && grass.find("if (alpha <= 8.0 / 255.0) {\n        discard;") != std::string_view::npos
            && grass.find("vec3(0.0, 1.0, 0.0)") != std::string_view::npos
            && grass.find("mix(eawr_diffuse.rgb, eawr_diffuse1, wave)") != std::string_view::npos,
        "Grass blends with depth writes, alpha-tests at 8, lights an up normal and lerps Diffuse to Diffuse1");
    check(grass.find("float time_scale = 0.125 + 0.875 * normalized_speed;") != std::string_view::npos
            && grass.find("float bend = 10.0 * normalized_speed + (10.0 + 10.5 * normalized_speed) * wave;")
                != std::string_view::npos
            && grass.find("(1.0 - UV.y) * bend / wind_speed * eawr_wind") != std::string_view::npos
            && grass.find("TIME") == std::string_view::npos,
        "Grass waves at 0.125 + 0.875 w and moves its tip (1 - v) x (10w + (10 + 10.5w) a) along the wind");
    MaterialDescription cover = selector(legacy::grass::family, RenderPass::transparent);
    cover.bindings = {
        {"Emissive", eawr::assets::Vec4f{0.0F, 0.0F, 0.0F, 0.0F}},
        {"Diffuse0", eawr::assets::Vec4f{9.0F, 9.0F, 9.0F, 9.0F}},
        {"Diffuse", eawr::assets::Vec4f{0.5F, 0.75F, 1.0F, 0.5F}},
        {"Diffuse1", eawr::assets::Vec3f{0.25F, 0.25F, 0.25F}},
        {"BendScale", 0.25F},
        {"BaseTexture", std::string("cover.tga")},
    };
    check(rejection(cover).empty() && legacy::shader_source(cover).has_value(),
        "an authored Grass material is admitted; a parameter the effect does not declare is ignored");
    uniforms = legacy::uniforms(cover);
    check(value_of<std::array<float, 4>>(uniforms, "eawr_diffuse") == std::array<float, 4>{0.5F, 0.75F, 1.0F, 0.5F}
            && value_of<std::array<float, 3>>(uniforms, "eawr_diffuse1") == std::array<float, 3>{0.25F, 0.25F, 0.25F}
            && value_of<float>(uniforms, "eawr_bend_scale") == 0.25F
            && uniform(uniforms, "eawr_time") == nullptr && uniform(uniforms, "eawr_wave_rate") == nullptr,
        "Grass binds Diffuse with its opacity, Diffuse1.rgb and BendScale; the clock is the renderer's");
    check(legacy::reads_wind(cover), "Grass reads the scene wind");
    cover.bindings[3].value = std::string("x.tga");
    check(rejection(cover).find("'Grass.fx' binds parameter 'Diffuse1' as a texture") != std::string::npos,
        "a texture Diffuse1 fails closed");
    cover.bindings[3].value = eawr::assets::Vec3f{1.0F, 1.0F, 1.0F};
    cover.pass = RenderPass::opaque;
    check(rejection(cover) == "EAWR-RENDER-0001 legacy material family 'Grass.fx' cannot draw in the opaque render pass",
        "Grass is a blended transparent-pass family");

    const auto lit = legacy::tree::reference_pixel({0.5F, 0.25F, 0.125F, 1.0F}, {0.5F, 1.0F, 0.25F},
        {0.25F, 0.0F, 0.0F}, 1.0F);
    check(near(lit.rgb[0], 1.0F) && near(lit.rgb[1], 0.5F) && near(lit.rgb[2], 0.0625F) && lit.drawn,
        "Tree RGB is 2 x texel x diffuse plus 2 x specular x texel alpha");
    const auto cut = legacy::tree::reference_pixel({0.5F, 0.5F, 0.5F, 128.0F / 255.0F}, {1.0F, 1.0F, 1.0F},
        {0.0F, 0.0F, 0.0F}, 1.0F);
    const auto kept = legacy::tree::reference_pixel({0.5F, 0.5F, 0.5F, 129.0F / 255.0F}, {1.0F, 1.0F, 1.0F},
        {0.0F, 0.0F, 0.0F}, 1.0F);
    check(!cut.drawn && kept.drawn, "Tree alpha test is GREATER 128: 128 is cut, 129 is kept");
    const auto faint = legacy::grass::reference_pixel({0.5F, 0.5F, 0.5F, 8.0F / 255.0F}, {1.0F, 1.0F, 1.0F, 1.0F});
    const auto blade = legacy::grass::reference_pixel({0.5F, 0.25F, 0.0F, 0.5F}, {0.5F, 1.0F, 1.0F, 0.5F});
    check(!faint.drawn && blade.drawn && near(blade.rgb[0], 0.5F) && near(blade.rgb[1], 0.5F)
            && near(blade.alpha, 0.25F),
        "Grass is 2 x texel x colour with alpha texel x colour, cut at or below 8/255");
    check(!legacy::reads_wind(selector(legacy::mesh_additive::family, RenderPass::transparent)),
        "MeshAdditive does not read the scene wind");
}

// MeshBumpColorize / RSkinBumpColorize sph_t2 (#199).
void bump_colorize_contracts() {
    const legacy::Family& mesh = legacy::bump_colorize::mesh_family;
    const legacy::Family& rskin = legacy::bump_colorize::rskin_family;
    for (const legacy::Family* family : {&mesh, &rskin}) {
        const std::string name(family->program);
        check(family->authored_binormals && family->binding_textures.size() == 1
                && family->binding_textures[0].name == "NormalTexture",
            name + " samples NormalTexture as its own per-binding texture and reads authored binormals");
        const MaterialDescription exact = admissible(*family, RenderPass::opaque);
        check(legacy::binding_textures(exact).size() == 1 && legacy::authored_binormals(exact),
            name + " registry queries report the per-binding texture and the binormal stream");
        MaterialDescription fixed = exact;
        fixed.technique = family == &mesh ? "t0" : "sph_t0";
        fixed.pass_name = family == &mesh ? "t0_p0" : "sph_t0_p0";
        check(rejection(fixed).empty() && legacy::binding_textures(fixed).empty() && !legacy::authored_binormals(fixed)
                && !legacy::shader_source(fixed) && legacy::uniforms(fixed).empty(),
            name + " fixed-function row stays the original adapter with no family inputs");

        MaterialDescription changed = exact;
        changed.bindings = {{"BaseTexture", std::string("hull.dds")}};
        check(rejection(changed) == "EAWR-RENDER-0001 legacy material family '" + name
                + "' binds no NormalTexture; an unbound normal sampler's result is not established",
            name + " without a NormalTexture fails closed");
        changed.bindings.push_back({"NormalTexture", eawr::assets::Vec3f{0.5F, 0.5F, 1.0F}});
        check(rejection(changed).ends_with("binds NormalTexture as a value that is not a texture"),
            name + " with a non-texture NormalTexture fails closed");
        changed = exact;
        changed.bindings.push_back({"NormalTexture", std::string("other_b.dds")});
        check(rejection(changed).ends_with("binds NormalTexture or BaseTexture more than once"),
            name + " with a duplicated NormalTexture fails closed");
        changed = exact;
        changed.bindings.push_back({"UVOffset", eawr::assets::Vec4f{0.0F, std::numeric_limits<float>::quiet_NaN(),
            0.0F, 0.0F}});
        check(rejection(changed).ends_with("binds a non-finite component in parameter 'UVOffset'"),
            name + " with a non-finite UVOffset fails closed");

        const auto defaults = legacy::uniforms(exact);
        check(value_of<std::array<float, 3>>(defaults, "eawr_emissive") == std::array<float, 3>{0.0F, 0.0F, 0.0F}
                && value_of<std::array<float, 3>>(defaults, "eawr_diffuse") == std::array<float, 3>{1.0F, 1.0F, 1.0F}
                && value_of<std::array<float, 3>>(defaults, "eawr_specular") == std::array<float, 3>{1.0F, 1.0F, 1.0F}
                && value_of<std::array<float, 3>>(defaults, "eawr_colorization")
                    == std::array<float, 3>{0.0F, 1.0F, 0.0F}
                && value_of<std::array<float, 2>>(defaults, "eawr_uv_offset") == std::array<float, 2>{0.0F, 0.0F},
            name + " unauthored parameters keep the effect initializers");
        MaterialDescription authored = exact;
        authored.bindings.push_back({"Emissive", eawr::assets::Vec4f{0.125F, 0.25F, 0.5F, 1.0F}});
        authored.bindings.push_back({"Diffuse", eawr::assets::Vec3f{0.5F, 0.75F, 1.0F}});
        authored.bindings.push_back({"Specular", eawr::assets::Vec4f{0.25F, 0.5F, 0.75F, 0.0F}});
        authored.bindings.push_back({"Colorization", eawr::assets::Vec4f{1.0F, 0.5F, 0.25F, 1.0F}});
        authored.bindings.push_back({"UVOffset", eawr::assets::Vec4f{0.125F, -0.25F, 3.0F, 4.0F}});
        authored.bindings.push_back({"Shininess", 64.0F});
        check(rejection(authored).empty(), name + " authored parameters validate; Shininess is unread");
        const auto bound = legacy::uniforms(authored);
        check(value_of<std::array<float, 3>>(bound, "eawr_emissive") == std::array<float, 3>{0.125F, 0.25F, 0.5F}
                && value_of<std::array<float, 3>>(bound, "eawr_diffuse") == std::array<float, 3>{0.5F, 0.75F, 1.0F}
                && value_of<std::array<float, 3>>(bound, "eawr_specular") == std::array<float, 3>{0.25F, 0.5F, 0.75F}
                && value_of<std::array<float, 3>>(bound, "eawr_colorization") == std::array<float, 3>{1.0F, 0.5F, 0.25F}
                && value_of<std::array<float, 2>>(bound, "eawr_uv_offset") == std::array<float, 2>{0.125F, -0.25F},
            name + " binds the authored rgb and UVOffset.xy exactly");
    }

    const std::string mesh_source(mesh.shader(RenderPass::opaque));
    const std::string rskin_source(rskin.shader(RenderPass::opaque));
    for (const std::string& source : {mesh_source, rskin_source}) {
        check(source.find("uniform sampler2D NormalTexture") != std::string::npos
                && source.find("eawr_sph_fill_r * normal_h") != std::string::npos
                && source.find("eawr_sph_r") == std::string::npos
                && source.find("CAMERA_POSITION_WORLD") != std::string::npos
                && source.find("CUSTOM0.x * tangent_model + CUSTOM0.y * normal_model") != std::string::npos
                && source.find("BINORMAL") == std::string::npos
                && source.find("clamp(eawr_diffuse * fill * eawr_light_scale.rgb * eawr_unit_light_scale + eawr_emissive,")
                    != std::string::npos
                && source.find("instance uniform vec3 eawr_unit_light_scale = vec3(1.0);") != std::string::npos
                && source.find("UV += eawr_uv_offset;") != std::string::npos
                && source.find("pow(n_dot_h, 16.0) * normal_texel.a") != std::string::npos
                && source.find("mix(base.rgb, colorization * base.rgb, base.a)") != std::string::npos
                && source.find("eawr_srgb_to_linear") == std::string::npos,
            "bump colorize fills per vertex, lights the sun per pixel on the authored frame, on stored values");
    }
    check(mesh_source.find("vec3 frame_normal = model_basis * normal_model;") != std::string::npos
            && rskin_source.find("vec3 frame_normal = normal_world;") != std::string::npos,
        "the mesh program keeps the raw object-space frame, the RSKIN program normalizes the skinned normal");

    // Pixel stage on stored values: colorize by base alpha, n.L and n.H
    // saturated, 2 x surface x (sun + fill) + specular x gloss.
    legacy::bump_colorize::PixelInputs in;
    in.base = {0.5F, 0.25F, 1.0F, 1.0F};
    in.normal = {0.5F, 0.5F, 1.0F, 0.5F};
    in.colorization = {1.0F, 0.5F, 0.25F};
    in.diffuse = {1.0F, 1.0F, 1.0F};
    in.specular = {1.0F, 1.0F, 1.0F};
    in.light_diffuse = {0.5F, 0.5F, 0.5F};
    in.light_specular = {0.25F, 0.25F, 0.25F};
    in.vertex_diffuse = {0.125F, 0.125F, 0.125F};
    in.tangent_light = {0.0F, 0.0F, 1.0F};
    in.tangent_half = {0.0F, 0.0F, 1.0F};
    const auto facing = legacy::bump_colorize::reference_pixel(in);
    // surface = (0.5, 0.125, 0.25); light = 0.5 + 0.125; specular = 0.25 x 1 x 0.5.
    check(near(facing[0], 2.0F * 0.5F * 0.625F + 0.125F) && near(facing[1], 2.0F * 0.125F * 0.625F + 0.125F)
            && near(facing[2], 2.0F * 0.25F * 0.625F + 0.125F),
        "bump colorize: colorized surface x 2 (sun + fill) plus specular x gloss alpha");
    in.tangent_light = {0.0F, 0.0F, -1.0F};
    in.tangent_half = {0.6F, 0.0F, 0.8F};
    const auto away = legacy::bump_colorize::reference_pixel(in);
    const float highlight = std::pow(0.8F, 16.0F) * 0.25F * 0.5F;
    check(near(away[0], 2.0F * 0.5F * 0.125F + highlight, 1.0e-5F),
        "a sun behind the perturbed normal leaves the fill; the highlight is saturate(n.H)^16");

    // Authored binormal coefficients reproduce the binormal on the stored
    // unit tangent and normal, also when the frame is not orthogonal.
    const auto rebuilt = [](const std::array<float, 3>& n, const std::array<float, 3>& t,
                             const std::array<float, 3>& c) {
        const std::array<float, 3> side{n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0]};
        return std::array<float, 3>{c[0] * t[0] + c[1] * n[0] + c[2] * side[0],
            c[0] * t[1] + c[1] * n[1] + c[2] * side[1], c[0] * t[2] + c[1] * n[2] + c[2] * side[2]};
    };
    const float root_half = std::sqrt(0.5F);
    const std::array<float, 3> normal{0.0F, 0.0F, 1.0F};
    const std::array<float, 3> tangent{root_half, 0.0F, root_half};
    const std::array<float, 3> binormal{0.3F, 0.8F, -0.52F};
    const auto coefficients = legacy::binormal_coefficients(normal, tangent, binormal);
    const auto back = rebuilt(normal, tangent, coefficients);
    check(near(back[0], binormal[0], 1.0e-5F) && near(back[1], binormal[1], 1.0e-5F)
            && near(back[2], binormal[2], 1.0e-5F),
        "binormal coefficients rebuild a non-orthogonal authored binormal");
    const auto orthogonal = legacy::binormal_coefficients({0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, -1.0F, 0.0F});
    check(near(orthogonal[0], 0.0F) && near(orthogonal[1], 0.0F) && near(orthogonal[2], -1.0F),
        "an orthogonal mirrored frame is -cross(n, t)");
    const auto parallel = legacy::binormal_coefficients({0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F, 0.5F});
    check(near(parallel[0], 0.5F) && near(parallel[1], 0.5F) && near(parallel[2], 0.0F),
        "a tangent parallel to the normal keeps the binormal's projection");
}

// BatchMeshGloss, BatchMeshAlpha and MeshAlphaGloss sph_t0 (#200).

void ownership_colorization_contracts() {
    // WBP-52: every ownership state changes only masked diffuse, leaving rock
    // texels and independent specular identical. Both mining hull adapters
    // share this contract; shaders without colourisation have no override.
    const std::array<std::array<float, 3>, 5> states{{
        {1, 1, 1}, {0.6F, 0.75F, 1}, {0.2F, 0.5F, 1},
        {0.6F, 0.75F, 1}, {0.2F, 0.5F, 1}}};
    legacy::bump_colorize::PixelInputs in;
    in.base = {0.4F, 0.3F, 0.2F, 0};
    in.normal = {0.5F, 0.5F, 1, 0.5F};
    in.diffuse = in.specular = in.light_diffuse = in.light_specular = {1, 1, 1};
    in.vertex_diffuse = {0.25F, 0.25F, 0.25F};
    in.tangent_light = in.tangent_half = {0, 0, 1};
    in.colorization = states[0];
    const auto rock = legacy::bump_colorize::reference_pixel(in);
    for (const auto& colour : states) {
        in.colorization = colour;
        in.base[3] = 0;
        check(legacy::bump_colorize::reference_pixel(in) == rock,
            "WBP-52: neutral, capturing, captured, neutralizing and built leave unmasked rock unchanged");
        in.base[3] = 1;
        const auto trim = legacy::bump_colorize::reference_pixel(in);
        check(near(trim[0], 2.5F * in.base[0] * colour[0] + 0.5F),
            "WBP-52: masked trim follows colourisation without changing highlight");
        in.base[3] = 0.5F;
        const auto edge = legacy::bump_colorize::reference_pixel(in);
        check(near(edge[0], (rock[0] + trim[0]) * 0.5F), "WBP-52: mask edges retain partial coverage");
    }
    for (const auto& family : {legacy::bump_colorize::mesh_family, legacy::bump_colorize::rskin_family,
                              legacy::mesh_gloss_colorize::family}) {
        const auto source = family.shader(RenderPass::opaque);
        check(source.find("instance uniform vec4 eawr_unit_colorization = vec4(0.0, 0.0, 0.0, -1.0)")
                  != std::string_view::npos
                && source.find("eawr_unit_colorization.a < 0.0 ? eawr_colorization") != std::string_view::npos,
            "WBP-52: ownership override preserves ordinary uploaded colours until explicitly bound");
    }
    check(godot_backend::rskin_shader_opaque.find("mix(base_linear_rgb, colorization, base_sample.a)")
              != std::string_view::npos,
        "WBP-52: RSKIN override remains inside its existing alpha mask");
    check(godot_backend::meshgloss_shader_opaque.find("eawr_unit_colorization") == std::string_view::npos,
        "WBP-52: noncolourising meshes ignore ownership overrides");
}


} // namespace legacy_family_test_support
