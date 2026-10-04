#include "legacy_family_support.hpp"

namespace legacy_family_test_support {

void dx8_mesh_contracts() {
    namespace dx8 = legacy::dx8_mesh;
    const legacy::Family& gloss = dx8::batch_mesh_gloss_family;
    const legacy::Family& alpha = dx8::batch_mesh_alpha_family;
    const legacy::Family& alpha_gloss = dx8::mesh_alpha_gloss_family;
    check(alpha_gloss.binding_textures.size() == 1
            && alpha_gloss.binding_textures[0].name == "GlossTexture"
            && alpha_gloss.binding_textures[0].placeholder == legacy::TexturePlaceholder::black,
        "MeshAlphaGloss unresolved gloss uses a black, zero-specular mask");
    for (const legacy::Family* family : {&gloss, &alpha, &alpha_gloss}) {
        const std::string name(family->program);
        const MaterialDescription exact = admissible(*family, admitted_pass(*family));
        check(!family->authored_binormals && legacy::binding_textures(exact).size() == (family == &alpha_gloss ? 1U : 0U),
            name + " reads no binormals; only MeshAlphaGloss samples a per-binding texture");
        MaterialDescription fixed = exact;
        fixed.technique = "sph_t1";
        fixed.pass_name = "sph_t1_p0";
        check(rejection(fixed).empty() && legacy::binding_textures(fixed).empty() && !legacy::shader_source(fixed)
                && legacy::uniforms(fixed).empty(),
            name + " fixed-function sph_t1 row stays the original adapter with no family inputs");

        const std::string source(family->shader(admitted_pass(*family)));
        check(source.find("eawr_sph_r * normal_h") != std::string::npos
                && source.find("normalize(normalize(CAMERA_POSITION_WORLD - position_world) + eawr_light_direction)")
                    != std::string::npos
                && source.find("pow(max(dot(normal_world, half_direction), 0.0), 16.0)") != std::string::npos
                && source.find("clamp(eawr_specular * highlight * eawr_light_specular, 0.0, 1.0)") != std::string::npos
                && source.find("instance uniform vec3 eawr_unit_light_scale = vec3(1.0)") != std::string::npos
                && source.find("eawr_diffuse.rgb * irradiance * eawr_light_scale.rgb * eawr_unit_light_scale + eawr_emissive") != std::string::npos
                && source.find("eawr_srgb_to_linear") == std::string::npos
                && source.find("Shininess") == std::string::npos,
            name + " lights per vertex from SPH_LIGHT_ALL with a camera half vector, clamped, on stored values");

        MaterialDescription changed = exact;
        changed.bindings.push_back({"Specular", std::string("spec.tga")});
        check(rejection(changed).ends_with("binds parameter 'Specular' as a texture, not a float3 or float4"),
            name + " with a texture Specular fails closed");
        changed = exact;
        changed.bindings.push_back({"Diffuse", eawr::assets::Vec4f{1.0F, std::numeric_limits<float>::infinity(),
            1.0F, 1.0F}});
        check(rejection(changed).ends_with("binds a non-finite component in parameter 'Diffuse'"),
            name + " with a non-finite Diffuse fails closed");
        changed = exact;
        changed.bindings.push_back({"Shininess", 32.0F});
        changed.bindings.push_back({"Colorization", eawr::assets::Vec4f{1.0F, 0.0F, 0.0F, 1.0F}});
        check(rejection(changed).empty(), name + " ignores the unread Shininess and Colorization");
        const auto defaults = legacy::uniforms(exact);
        check(value_of<std::array<float, 3>>(defaults, "eawr_emissive") == std::array<float, 3>{0.0F, 0.0F, 0.0F}
                && value_of<std::array<float, 3>>(defaults, "eawr_specular") == std::array<float, 3>{1.0F, 1.0F, 1.0F},
            name + " unauthored Emissive and Specular keep the effect initializers");
    }

    const std::string gloss_source(gloss.shader(RenderPass::opaque));
    check(gloss_source.find("render_mode unshaded, fog_disabled, depth_draw_opaque, depth_test_default, cull_back;")
                != std::string::npos
            && gloss_source.find("uniform vec3 eawr_diffuse") != std::string::npos
            && gloss_source.find("        eawr_light_scale.a), 0.0, 1.0);") != std::string::npos
            && gloss_source.find("clamp(2.0 * eawr_vertex_diffuse.rgb * base.rgb + eawr_vertex_specular * base.a, 0.0, 1.0)")
                != std::string::npos
            && gloss_source.find("ALPHA") == std::string::npos,
        "BatchMeshGloss is opaque, 2 x D x base + S x base alpha");
    const std::string alpha_source(alpha.shader(RenderPass::transparent));
    check(alpha_source.find("render_mode unshaded, fog_disabled, blend_mix, depth_draw_never, depth_test_default,")
                != std::string::npos
            && alpha_source.find("eawr_diffuse.a * eawr_light_scale.a") != std::string::npos
            && alpha_source.find("clamp(2.0 * eawr_vertex_diffuse.rgb * base.rgb + eawr_vertex_specular, 0.0, 1.0)")
                != std::string::npos
            && alpha_source.find("ALPHA = base.a * eawr_vertex_diffuse.a;") != std::string::npos
            && alpha_source.find("GlossTexture") == std::string::npos,
        "BatchMeshAlpha blends without depth writes, 2 x D x base + S, alpha base x D");
    const std::string alpha_gloss_source(alpha_gloss.shader(RenderPass::transparent));
    check(alpha_gloss_source.find("uniform sampler2D GlossTexture : filter_linear_mipmap, repeat_enable;")
                != std::string::npos
            && alpha_gloss_source.find("clamp(2.0 * eawr_vertex_diffuse.rgb * base.rgb + eawr_vertex_specular * gloss, 0.0, 1.0)")
                != std::string::npos
            && alpha_gloss_source.find("float gloss = texture(GlossTexture, UV).r;") != std::string::npos
            && alpha_gloss_source.find("ALPHA = base.a * eawr_vertex_diffuse.a;") != std::string::npos,
        "MeshAlphaGloss adds S x gloss red and blends on base x D alpha");

    // Diffuse: a float3 on BatchMeshGloss, a float4 on the alpha programs.
    MaterialDescription rock = admissible(gloss, RenderPass::opaque);
    rock.bindings.push_back({"Diffuse", eawr::assets::Vec4f{0.5F, 0.75F, 1.0F, 0.0F}});
    check(value_of<std::array<float, 3>>(legacy::uniforms(rock), "eawr_diffuse") == std::array<float, 3>{0.5F, 0.75F, 1.0F},
        "BatchMeshGloss binds Diffuse.rgb; its authored alpha never reaches the float3");
    MaterialDescription bush = admissible(alpha, RenderPass::transparent);
    bush.bindings.push_back({"Diffuse", eawr::assets::Vec4f{0.933F, 0.933F, 0.933F, 0.734F}});
    check(value_of<std::array<float, 4>>(legacy::uniforms(bush), "eawr_diffuse")
            == std::array<float, 4>{0.933F, 0.933F, 0.933F, 0.734F},
        "BatchMeshAlpha binds Diffuse with its alpha");
    bush = admissible(alpha, RenderPass::transparent);
    bush.bindings.push_back({"Diffuse", eawr::assets::Vec3f{0.25F, 0.5F, 0.75F}});
    check(value_of<std::array<float, 4>>(legacy::uniforms(bush), "eawr_diffuse")
            == std::array<float, 4>{0.25F, 0.5F, 0.75F, 1.0F},
        "a float3 Diffuse on an alpha program keeps the initializer's alpha 1");

    // GlossTexture must be one texture binding.
    const std::string prefix = "EAWR-RENDER-0001 legacy material family 'MeshAlphaGloss.fx' ";
    MaterialDescription shell = admissible(alpha_gloss, RenderPass::transparent);
    shell.bindings = {{"BaseTexture", std::string("rock.tga")}};
    check(rejection(shell) == prefix + "binds no GlossTexture; an unbound gloss sampler's result is not established",
        "MeshAlphaGloss without a GlossTexture fails closed");
    shell.bindings.push_back({"GlossTexture", eawr::assets::Vec3f{1.0F, 1.0F, 1.0F}});
    check(rejection(shell) == prefix + "binds GlossTexture as a value that is not a texture",
        "MeshAlphaGloss with a non-texture GlossTexture fails closed");
    shell = admissible(alpha_gloss, RenderPass::transparent);
    shell.bindings.push_back({"GlossTexture", std::string("other.tga")});
    check(rejection(shell) == prefix + "binds GlossTexture or BaseTexture more than once",
        "MeshAlphaGloss with a duplicated GlossTexture fails closed");
    shell = admissible(alpha_gloss, RenderPass::transparent);
    shell.bindings[1].value = std::string("rock.tga");
    check(rejection(shell).empty() && legacy::binding_textures(shell).size() == 1,
        "a GlossTexture naming the base texture is still its own per-binding texture");

    // Vertex colours saturate like vs_1_1 outputs; the highlight is N.H^16.
    const auto lit = dx8::reference_vertex({1.0F, 1.0F, 1.0F, 0.5F}, {0.992F, 0.0F, 0.0F}, {1.0F, 0.5F, 0.0F},
        {0.25F, 0.5F, 2.0F}, {1.0F, 1.0F, 1.0F, 1.0F}, {2.0F, 2.0F, 2.0F}, 1.0F, true);
    check(near(lit.diffuse[0], 1.0F) && near(lit.diffuse[1], 0.5F) && near(lit.diffuse[2], 1.0F)
            && near(lit.diffuse[3], 0.5F) && near(lit.specular[0], 1.0F) && near(lit.specular[1], 1.0F)
            && near(lit.specular[2], 0.0F),
        "D and S clamp to [0, 1]; the alpha programs take Diffuse.a x LIGHT_SCALE.a");
    const auto grazing = dx8::reference_vertex({1.0F, 1.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F},
        {0.5F, 0.5F, 0.5F}, {1.0F, 1.0F, 1.0F, 1.0F}, {1.0F, 1.0F, 1.0F}, 0.5F, false);
    check(near(grazing.specular[0], std::pow(0.5F, 16.0F)) && near(grazing.diffuse[3], 1.0F),
        "the highlight is max(N.H, 0)^16 and BatchMeshGloss takes LIGHT_SCALE.a, not Diffuse.a");
    const auto behind = dx8::reference_vertex({1.0F, 1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F},
        {0.5F, 0.5F, 0.5F}, {1.0F, 1.0F, 1.0F, 1.0F}, {1.0F, 1.0F, 1.0F}, -0.5F, true);
    check(near(behind.specular[0], 0.0F), "a half vector behind the normal gives no highlight");

    const dx8::VertexColours vertex{{0.5F, 0.25F, 0.75F, 0.5F}, {0.25F, 0.25F, 0.25F}};
    const std::array<float, 4> base{0.5F, 0.5F, 1.0F, 0.5F};
    const auto rock_pixel = dx8::reference_pixel(dx8::Program::batch_mesh_gloss, base, 0.0F, vertex);
    check(near(rock_pixel.rgb[0], 0.625F) && near(rock_pixel.rgb[1], 0.375F) && near(rock_pixel.rgb[2], 1.0F)
            && near(rock_pixel.alpha, 0.5F),
        "BatchMeshGloss: 2 x D x base + S x base alpha, saturated; alpha D.a");
    const auto bush_pixel = dx8::reference_pixel(dx8::Program::batch_mesh_alpha, base, 0.0F, vertex);
    check(near(bush_pixel.rgb[0], 0.75F) && near(bush_pixel.rgb[1], 0.5F) && near(bush_pixel.alpha, 0.25F),
        "BatchMeshAlpha: 2 x D x base + S; alpha base x D");
    const auto shell_pixel = dx8::reference_pixel(dx8::Program::mesh_alpha_gloss, base, 0.5F, vertex);
    check(near(shell_pixel.rgb[0], 0.625F) && near(shell_pixel.rgb[1], 0.375F) && near(shell_pixel.alpha, 0.25F),
        "MeshAlphaGloss: 2 x D x base + S x gloss red; alpha base x D");
}

void reference_contracts() {
    using legacy::mesh_additive::reference_add;
    const auto added = reference_add({0.25F, 0.5F, 0.75F}, {0.5F, 0.5F, 0.5F}, {0.5F, 1.0F, 2.0F},
        {1.0F, 1.0F, 1.0F, 1.0F});
    check(near(added[0], 0.5F) && near(added[1], 1.0F) && near(added[2], 1.0F),
        "MeshAdditive adds texel x saturated colour and saturates at the 8-bit target");
    const auto negative = reference_add({0.25F, 0.25F, 0.25F}, {1.0F, 1.0F, 1.0F}, {-1.0F, 0.5F, 0.5F},
        {1.0F, 1.0F, 1.0F, 0.0F});
    check(near(negative[0], 0.25F) && near(negative[1], 0.25F) && near(negative[2], 0.25F),
        "a negative colour contributes 0 and LIGHT_SCALE.a = 0 removes the contribution");
    const auto scaled = reference_add({0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {1.0F, 1.0F, 1.0F},
        {0.5F, 1.0F, 2.0F, 0.5F});
    check(near(scaled[0], 0.25F) && near(scaled[1], 0.5F) && near(scaled[2], 1.0F),
        "LIGHT_SCALE.rgb and LIGHT_SCALE.a both scale the vertex colour before saturation");
    const auto uv = legacy::mesh_additive::reference_uv({0.25F, 0.5F}, {0.25F, -0.5F}, 3.0F);
    check(near(uv[0], 1.0F) && near(uv[1], -1.0F), "MeshAdditive scrolls UV by time x rate, unwrapped");
    const auto offset = legacy::mesh_additive_offset::reference_uv({0.25F, 0.5F}, {0.5F, -0.25F});
    check(near(offset[0], 0.75F) && near(offset[1], 0.25F), "MeshAdditiveOffset adds UVOffset.xy");
    const auto solid = legacy::mesh_solid_color::reference_color({1.5F, -0.5F, 0.25F});
    check(near(solid[0], 1.0F) && near(solid[1], 0.0F) && near(solid[2], 0.25F),
        "MeshSolidColor writes the saturated colour");
    using legacy::mesh_gloss_colorize::reference_pixel;
    const auto uncolored = reference_pixel({0.5F, 0.25F, 0.125F, 0.0F}, 0.0F, {0.0F, 0.0F, 0.0F},
        {0.5F, 0.5F, 0.5F}, {1.0F, 1.0F, 1.0F});
    check(near(uncolored[0], 0.5F) && near(uncolored[1], 0.25F) && near(uncolored[2], 0.125F),
        "base alpha 0 leaves the texel uncolorized and gloss 0 removes specular");
    const auto colored = reference_pixel({0.5F, 0.25F, 0.125F, 1.0F}, 0.5F, {1.0F, 0.0F, 0.5F},
        {0.5F, 0.5F, 0.5F}, {0.2F, 0.4F, 0.6F});
    check(near(colored[0], 0.6F) && near(colored[1], 0.2F) && near(colored[2], 0.3625F),
        "base alpha 1 multiplies by Colorization before the 2x diffuse, plus specular x gloss red");
}

void derived_fog_contracts() {
    // Land map fog (#28) derives a fog stage for every family it draws.
    for (const legacy::Family& family : legacy::registry()) {
        const auto derived = eawr::presentation::godot_backend::derived_legacy_fog_variant(
            family.shader(admitted_pass(family)));
        check(derived.has_value() && derived->find("eawr_fog_attenuation()") != std::string::npos,
              std::string(family.program) + " has a derived fog-stub-v1 variant");
    }
}

} // namespace legacy_family_test_support
