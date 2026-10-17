#include "legacy_family_support.hpp"

namespace legacy_family_test_support {

// The renderer's original selector table (renderer_contract.cpp), as
// program and technique.
constexpr std::array<std::pair<std::string_view, std::string_view>, 9> original_selectors{{
    {"MeshGloss.fx", "sph_t0"}, {"MeshAlpha.fx", "sph_t1"}, {"MeshAlphaGloss.fx", "sph_t1"},
    {"BatchMeshAlpha.fx", "sph_t1"}, {"BatchMeshGloss.fx", "sph_t1"}, {"MeshBumpColorize.fx", "t0"},
    {"RSkinBumpColorize.fx", "sph_t0"}, {"RSkinGloss.fx", "sph_t1"}, {"RSkinGlossColorize.fx", "sph_t0"}}};

// The quoted techniques a program has, original row first, as a rejection
// names them.
[[nodiscard]] std::string supported_techniques(const std::string_view program) {
    std::string result;
    for (const auto& [original, technique] : original_selectors) {
        if (original == program) result += "'" + std::string(technique) + "'";
    }
    for (const legacy::Family& family : legacy::registry()) {
        if (family.program == program) result += (result.empty() ? "'" : ", '") + std::string(family.technique) + "'";
    }
    return result;
}

void registry_contracts() {
    const auto registry = legacy::registry();
    check(registry.size() == 20,
        "the registry includes skinned families and the missing-effect placeholder");
    const std::vector<std::tuple<std::string_view, std::string_view, std::string_view, bool, bool, bool>> expected{
        {"MeshAdditive.fx", "t0", "t0_p0", false, true, false},
        {"MeshAdditiveOffset.fx", "t0", "t0_p0", false, true, false},
        {"MeshAdditiveVColor.fx", "t0", "t0_p0", false, true, false},
        {"MeshSolidColor.fx", "t0", "t0_p0", true, false, false},
        {"MeshGlossColorize.fx", "sph_t0", "sph_t0_p0", true, false, true},
        {"Tree.fx", "sph_t1", "sph_t1_p0", true, false, true},
        {"Grass.fx", "sph_t0", "sph_t0_p0", false, true, true},
        {"MeshBumpColorize.fx", "sph_t2", "sph_t2_p0", true, false, true},
        {"RSkinBumpColorize.fx", "sph_t2", "sph_t2_p0", true, false, true},
        {"BatchMeshGloss.fx", "sph_t0", "sph_t0_p0", true, false, true},
        {"BatchMeshAlpha.fx", "sph_t0", "sph_t0_p0", false, true, true},
        {"MeshAlphaGloss.fx", "sph_t0", "sph_t0_p0", false, true, true},
        {"eawr_missing_effect", "fallback", "opaque", true, false, true},
        {"MeshAdditive2x.fx", "t0", "t0_p0", false, true, false},
        {"MeshBumpSpecGlowColorize.fx", "sph_t2", "sph_t2_p0", true, false, true},
        {"RSkinAdditive.fx", "t0", "t0_p0", false, true, false},
        {"RSkinAlpha.fx", "sph_t0", "sph_t0_p0", false, true, true},
        {"MeshShield.fxo", "t0", "t0_p0", false, true, false},
        {"MeshShieldFarseer.fxo", "sph_t3", "sph_t3_p0", false, true, false},
        {"Planet_Deathstar.fx", "sph_t3", "sph_t3_p0", true, false, true},
    };
    for (std::size_t index = 0; index < expected.size() && index < registry.size(); ++index) {
        const auto& [program, technique, pass_name, opaque, transparent, shadows] = expected[index];
        const legacy::Family& family = registry[index];
        check(family.program == program && family.technique == technique && family.pass_name == pass_name
                && family.opaque == opaque && family.transparent == transparent
                && family.receives_shadows == shadows,
            std::string("registry row ") + std::string(program) + " has its exact selector, passes and shadow policy");
        check(legacy::find_family(program, technique) == &family, "find_family returns the registry row");
        check(family.reads_wind == (program == "Tree.fx" || program == "Grass.fx"),
            std::string("registry row ") + std::string(program) + " reads the scene wind only for Tree and Grass");
    }
    for (std::size_t left = 0; left < registry.size(); ++left) {
        for (const auto& [program, technique] : original_selectors) {
            check(registry[left].program != program || registry[left].technique != technique,
                "a legacy/ family must not shadow an original selector row");
        }
        for (std::size_t right = left + 1; right < registry.size(); ++right) {
            check(registry[left].program != registry[right].program
                    || registry[left].technique != registry[right].technique,
                "registry selectors are unique");
        }
    }
    for (const std::string_view absent : {"meshadditive.fx", "MeshAdditive", "MeshAdditive.fxo",
             "alDefault.fx", "Planet.fx", "Nebula.fx", "TerrainMeshBump.fx"}) {
        for (const std::string_view technique : {"t0", "sph_t0", "sph_t1", "sph_t2"}) {
            check(legacy::find_family(absent, technique) == nullptr,
                std::string("no family is found for ") + std::string(absent));
        }
    }
    check(legacy::find_family("MeshBumpColorize.fx", "t0") == nullptr
            && legacy::find_family("RSkinBumpColorize.fx", "sph_t0") == nullptr
            && legacy::find_family("MeshBumpColorize.fx", "SPH_T2") == nullptr,
        "the fixed-function bump rows stay original selectors; techniques match exactly");
    check(legacy::receives_shadows(selector(registry[0], RenderPass::transparent)) == false,
        "an unlit legacy/ family is compiled unshaded under shadows");
    MaterialDescription original = selector(registry[0], RenderPass::opaque);
    original.program = "MeshGloss.fx";
    check(legacy::receives_shadows(original), "original selectors keep the shadow-receiving rule");
    check(legacy::uniforms(original).empty(), "original selectors get no legacy/ uniforms");
    auto missing = selector(legacy::missing_effect::family, RenderPass::opaque);
    missing.bindings.push_back({"Color", eawr::assets::Vec4f{1.0F, 0.0F, 1.0F, 0.0F}});
    missing.bindings.push_back({"BaseTexture", std::string("invented-not-loaded.dds")});
    const auto fallback_uniforms = legacy::uniforms(missing);
    check(fallback_uniforms.size() == 1
              && std::get<std::array<float, 3>>(fallback_uniforms[0].value) == std::array<float, 3>{0.0F, 1.0F, 0.0F},
          "ALDEFAULT-01 uses green independently of authored colour, alpha and texture names");
}

void source_contracts() {
    const std::regex time_builtin(R"(\bTIME\b)");
    for (const legacy::Family& family : legacy::registry()) {
        const std::string name(family.program);
        for (const RenderPass pass : {RenderPass::opaque, RenderPass::alpha_tested, RenderPass::transparent,
                 RenderPass::post}) {
            const std::string_view source = family.shader(pass);
            check(source.empty() != legacy::admits_pass(family, pass),
                name + " has a source for exactly its admitted render passes");
        }
        const std::string source(family.shader(admitted_pass(family)));
        // The renderer derives shadow variants by replacing this exact prefix.
        check(source.starts_with("\nshader_type spatial;\nrender_mode unshaded, fog_disabled, "),
            name + " source starts with the unshaded, fog-disabled spatial declaration");
        check(source.find("uniform sampler2D BaseTexture") != std::string::npos,
            name + " declares BaseTexture, the renderer's legacy compile probe");
        check(source.find("void fragment()") != std::string::npos, name + " declares fragment()");
        check(!std::regex_search(source, time_builtin), name + " never reads Godot's TIME");
        // Colour policy (docs/rendering.md): stored texels, stored ALBEDO.
        check(source.find("source_color") == std::string::npos
                && source.find("OUTPUT_IS_SRGB") == std::string::npos
                && source.find("eawr_compatibility_") == std::string::npos,
            name + " samples stored texels and writes a stored ALBEDO on every backend");
        const std::size_t writer = source.find(godot_backend::stored_albedo_writer);
        const bool stored_product = source.find("ALBEDO = eawr_stored_albedo(") != std::string::npos;  // MeshShieldFarseer scales the argument
        check(stored_product == (writer != std::string::npos)
                && (writer == std::string::npos
                    || source.find(godot_backend::stored_albedo_writer, writer + 1) == std::string::npos),
            name + " writes a stored product only through its one stored-value ALBEDO writer");
        const std::string fallback = godot_backend::compatibility_source(source);
        check(stored_product
                ? fallback.find(godot_backend::stored_albedo_writer) == std::string::npos
                    && fallback.find(godot_backend::compatibility_albedo_writer) != std::string::npos
                : fallback == source,
            name + " differs on the Compatibility fallback only by the compensated ALBEDO writer");
        // Grass draws its engine-billboarded cards as authored, so both sides.
        const std::string_view cull = (family.program == "Grass.fx" || family.program == "MeshShield.fxo")
            ? "cull_disabled" : "cull_back";
        check(source.find(cull) != std::string::npos && source.find("depth_test_default") != std::string::npos,
            name + " keeps the renderer-baseline cull (Grass: none) and depth test");
        const bool lit = source.find("eawr_sph_r") != std::string::npos
            || source.find("eawr_sph_fill_r") != std::string::npos;
        check((lit || family.program == legacy::missing_effect::family.program) == family.receives_shadows,
            name + " receives shadows for engine lighting or the sourced missing-effect placeholder");
        const auto uniforms = family.uniforms(selector(family, admitted_pass(family)));
        check(!uniforms.empty(), name + " binds typed uniforms");
        for (const legacy::Uniform& item : uniforms) {
            const std::string declaration = "uniform " + std::string(glsl_type(item.value)) + " "
                + std::string(item.name) + " ";
            check(source.find(declaration) != std::string::npos,
                name + " declares " + std::string(item.name) + " with the bound type");
            check(item.name.starts_with("eawr_"), name + " binds only eawr_* uniforms");
        }
    }
    const auto& additive = legacy::mesh_additive::shader_source;
    check(additive.find("render_mode unshaded, fog_disabled, blend_add, depth_draw_never,") != std::string_view::npos
            && additive.find("ALPHA") == std::string_view::npos
            && additive.find("source_color") == std::string_view::npos,
        "MeshAdditive is ONE/ONE (alpha 1), writes no depth and samples stored texels");
    check(additive.find("eawr_scrolled_uv = UV + eawr_effect_time * eawr_uv_scroll_rate;") != std::string_view::npos
            && additive.find("instance uniform vec3 eawr_unit_light_scale = vec3(1.0)") != std::string::npos
            && additive.find("clamp(eawr_color * eawr_light_scale.rgb * eawr_unit_light_scale * eawr_light_scale.a, 0.0, 1.0)")
                != std::string_view::npos,
        "MeshAdditive scrolls the first UV by TIME and saturates the per-vertex colour");
    const std::string_view doubled = legacy::mesh_additive_2x::shader(RenderPass::transparent);
    check(doubled.find("clamp(2.0 * eawr_vertex_color * texture(BaseTexture, eawr_scrolled_uv).rgb, 0.0, 1.0)")
              != std::string_view::npos,
        "doubled additive saturates the sampled product after vertex saturation");
    auto scrolling = selector(legacy::mesh_additive_2x::family, RenderPass::transparent);
    check(!legacy::reads_effect_time(scrolling), "zero-rate doubled additive needs no effect-clock writes");
    scrolling.bindings.push_back({"UVScrollRate", eawr::assets::Vec4f{0.5F, 0.0F, 0.0F, 0.0F}});
    check(legacy::reads_effect_time(scrolling), "scrolling doubled additive reads the existing scene clock");
    const auto& glow_family = legacy::mesh_bump_spec_glow_colorize::family;
    const auto glow = glow_family.shader(RenderPass::opaque);
    check(glow.find("32.0 * specular_texel.a") != std::string_view::npos
              && glow.find("eawr_emissive * normal_texel.a") != std::string_view::npos
              && glow.find(" + eawr_emissive,") == std::string_view::npos,
        "specular map alpha controls exponent, normal alpha controls pixel emission, fill has no emission");
    auto glow_material = selector(glow_family, RenderPass::opaque);
    check(legacy::binding_textures(glow_material).size() == 2,
        "glow samples its own normal and specular texture bindings");
    glow_material.bindings = {{"NormalTexture", std::string("normal.tga")},
        {"SpecularTexture", std::string("specular.tga")}};
    check(rejection(glow_material).empty(), "glow accepts both distinct sampler bindings");
    glow_material.bindings.back().value = 0.5F;
    check(!rejection(glow_material).empty(), "glow rejects a scalar specular texture");
    auto skin_additive = selector(legacy::rskin::additive_family, RenderPass::transparent);
    skin_additive.bindings = {{"UVScrollRate", eawr::assets::Vec4f{0.25F, 0.0F, 7.0F, 9.0F}},
        {"eawr_effect_time", 2.5F}};
    check(rejection(skin_additive).empty() && legacy::reads_effect_time(skin_additive)
              && value_of<float>(legacy::uniforms(skin_additive), "eawr_effect_time") == 2.5F,
        "skinned scrolling additive reads the supplied scene clock with the shared bindings");
    auto skin_alpha = selector(legacy::rskin::alpha_family, RenderPass::transparent);
    skin_alpha.bindings = {{"Diffuse", eawr::assets::Vec4f{0.25F, 0.5F, 0.75F, 0.375F}}};
    check(rejection(skin_alpha).empty()
              && value_of<std::array<float, 4>>(legacy::uniforms(skin_alpha), "eawr_diffuse")[3] == 0.375F
              && !legacy::reads_effect_time(skin_alpha), "skinned alpha preserves authored material alpha");
    auto shield = selector(legacy::mesh_shield::family, RenderPass::transparent);
    check(!rejection(shield).empty(), "compiled shield diagnoses absent required texture bindings");
    shield.bindings = {{"BaseTexture", std::string("energy.tga")},
        {"WaveTexture", std::string("wave.tga")}, {"DistortionTexture", std::string("distortion.tga")}};
    check(rejection(shield).empty() && legacy::reads_effect_time(shield),
        "compiled shield has its three samplers and default scrolling subscribes to the scene clock");
    for (const std::string_view rate : {"BaseUVScrollRate", "WaveUVScrollRate", "DistortUVScrollRate"}) {
        shield.bindings.push_back({std::string(rate), 0.0F});
    }
    check(!legacy::reads_effect_time(shield), "three zero scroll rates require no clock writes");
    shield.bindings[2].value = 0.5F;
    check(!rejection(shield).empty(), "compiled shield rejects a scalar distortion sampler");
    auto farseer = selector(legacy::mesh_shield::farseer_family, RenderPass::transparent);
    farseer.bindings = {{"Atmosphere", eawr::assets::Vec4f{0.1F, 0.2F, 0.3F, 0.5F}},
        {"AtmospherePower", 2.0F}};
    check(rejection(farseer).empty() && !legacy::reads_effect_time(farseer)
              && legacy::binding_textures(farseer).empty(), "texture-free rim has no scene-clock input");
    auto planet = selector(legacy::planet_deathstar::family, RenderPass::opaque);
    check(!rejection(planet).empty(), "Death Star requires all three auxiliary sampler bindings");
    planet.bindings = {{"NormalTexture", std::string("normal.tga")},
        {"CloudTexture", std::string("cloud.tga")}, {"CloudNormalTexture", std::string("cloud-normal.tga")}};
    check(rejection(planet).empty() && legacy::reads_effect_time(planet),
        "Death Star defaults to a scrolling cloud layer with distinct sampler bindings");
    planet.bindings.push_back({"CloudScrollRate", 0.0F});
    check(!legacy::reads_effect_time(planet), "zero cloud scroll requires no effect-clock writes");
    planet.bindings.back().value = std::string("invalid.tga");
    check(!rejection(planet).empty(), "cloud scroll is a finite scalar rather than a sampler");
    const auto& offset = legacy::mesh_additive_offset::shader_source;
    check(offset.find("render_mode unshaded, fog_disabled, blend_add, depth_draw_never,") != std::string_view::npos
            && offset.find("eawr_offset_uv = UV + eawr_uv_offset;") != std::string_view::npos
            && offset.find("eawr_effect_time") == std::string_view::npos,
        "MeshAdditiveOffset offsets the first UV with no time term");
    const auto& solid = legacy::mesh_solid_color::shader_source;
    check(solid.find("render_mode unshaded, fog_disabled, depth_draw_never, depth_test_default, cull_back;")
                != std::string_view::npos
            && solid.find("blend_") == std::string_view::npos
            && solid.find("texture(") == std::string_view::npos
            && solid.find("clamp(eawr_color, 0.0, 1.0)") != std::string_view::npos,
        "MeshSolidColor replaces the target with the saturated colour, samples nothing and writes no depth");
    const auto& colorize = legacy::mesh_gloss_colorize::shader_source;
    check(colorize.find("render_mode unshaded, fog_disabled, depth_draw_opaque, depth_test_default, cull_back;")
                != std::string_view::npos
            && colorize.find("pow(max(dot(normal_world, half_direction), 0.0), 16.0)") != std::string_view::npos
            && colorize.find("mix(base_linear_rgb, colorization * base_linear_rgb, base_sample.a)")
                != std::string_view::npos
            && colorize.find("2.0 * eawr_vertex_diffuse.rgb * surface + eawr_vertex_specular * gloss")
                != std::string_view::npos
            && colorize.find("uniform sampler2D GlossTexture") != std::string_view::npos,
        "MeshGlossColorize colorizes before MeshGloss lighting and gates specular by gloss red");
}

void selector_contracts() {
    for (const legacy::Family& family : legacy::registry()) {
        const std::string name(family.program);
        const RenderPass pass = admitted_pass(family);
        MaterialDescription exact = admissible(family, pass);
        if (family.program == "MeshShield.fxo") {
            exact.bindings = {{"BaseTexture", std::string("energy.tga")},
                {"WaveTexture", std::string("wave.tga")}, {"DistortionTexture", std::string("distortion.tga")}};
        }
        if (family.program == "MeshBumpSpecGlowColorize.fx") {
            exact.bindings.push_back({"SpecularTexture", std::string("specular.tga")});
        }
        if (family.program == "Planet_Deathstar.fx") {
            exact.bindings.push_back({"CloudTexture", std::string("cloud.tga")});
            exact.bindings.push_back({"CloudNormalTexture", std::string("cloud-normal.tga")});
        }
        check(rejection(exact).empty(), name + " exact selector validates with its minimal bindings");
        check(legacy::shader_source(exact).has_value(), name + " exact selector selects its adapter source");

        MaterialDescription changed = exact;
        // A technique the program has neither as an original row nor as a family.
        for (const std::string_view candidate : {"t1", "sph_t0", "sph_t1", "sph_t3"}) {
            if (supported_techniques(family.program).find("'" + std::string(candidate) + "'") == std::string::npos) {
                changed.technique = std::string(candidate);
                break;
            }
        }
        const std::string technique = rejection(changed);
        check(technique == "EAWR-RENDER-0001 legacy material family '" + name + "' has no Godot adapter for"
                " technique '" + changed.technique + "' (supported: " + supported_techniques(family.program) + ")",
            name + " fixed-function technique fails closed naming the supported technique");
        check(!legacy::shader_source(changed), name + " upload selection is independently exhaustive (technique)");

        changed = exact;
        changed.pass_name = std::string(family.technique) + "_p1";
        check(rejection(changed).find("has no Godot adapter for pass '" + changed.pass_name + "'") != std::string::npos,
            name + " other pass name fails closed");
        check(!legacy::shader_source(changed), name + " upload selection is independently exhaustive (pass name)");

        for (const RenderPass other : {RenderPass::opaque, RenderPass::alpha_tested, RenderPass::transparent,
                 RenderPass::post}) {
            if (legacy::admits_pass(family, other)) continue;
            changed = exact;
            changed.pass = other;
            check(rejection(changed) == "EAWR-RENDER-0001 legacy material family '" + name
                    + "' cannot draw in the " + std::string(presentation::to_string(other)) + " render pass",
                name + " rejects a render pass its adapter lacks");
            check(!legacy::shader_source(changed), name + " upload selection rejects that render pass too");
        }

        changed = exact;
        changed.route = presentation::MaterialRoute::modern_spatial;
        check(!presentation::validate_material(changed), name + " selectors are not a modern route");
        check(!legacy::shader_source(changed), name + " source is legacy-route only");
    }
    for (const std::string_view unknown : {"alDefault.fx", "meshadditive.fx",
             "Planet.fx", "Nebula.fx", "TerrainMeshBump.fx", "TerrainMeshGloss.fx", "MeshShadowVolume.fx"}) {
        MaterialDescription material = selector(legacy::registry()[0], RenderPass::transparent);
        material.program = std::string(unknown);
        check(rejection(material) == "EAWR-RENDER-0001 unknown legacy material family '" + std::string(unknown)
                + "' has no implemented Godot adapter",
            std::string(unknown) + " stays an unknown legacy family");
    }
}


} // namespace legacy_family_test_support
