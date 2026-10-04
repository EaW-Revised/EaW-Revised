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
    check(registry.size() == 12,
        "the registry has the four WP-43 families, Tree, Grass, the bump colorize pair and the three DX8 mesh families");
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
        const bool stored_product = source.find("ALBEDO = eawr_stored_albedo(stored_rgb);") != std::string::npos;
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
        const std::string_view cull = family.program == "Grass.fx" ? "cull_disabled" : "cull_back";
        check(source.find(cull) != std::string::npos && source.find("depth_test_default") != std::string::npos,
            name + " keeps the renderer-baseline cull (Grass: none) and depth test");
        const bool lit = source.find("eawr_sph_r") != std::string::npos
            || source.find("eawr_sph_fill_r") != std::string::npos;
        check(lit == family.receives_shadows, name + " receives shadows exactly when it reads engine light");
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
    check(additive.find("eawr_scrolled_uv = UV + eawr_time * eawr_uv_scroll_rate;") != std::string_view::npos
            && additive.find("instance uniform vec3 eawr_unit_light_scale = vec3(1.0)") != std::string::npos
            && additive.find("clamp(eawr_color * eawr_light_scale.rgb * eawr_unit_light_scale * eawr_light_scale.a, 0.0, 1.0)")
                != std::string_view::npos,
        "MeshAdditive scrolls the first UV by TIME and saturates the per-vertex colour");
    const auto& offset = legacy::mesh_additive_offset::shader_source;
    check(offset.find("render_mode unshaded, fog_disabled, blend_add, depth_draw_never,") != std::string_view::npos
            && offset.find("eawr_offset_uv = UV + eawr_uv_offset;") != std::string_view::npos
            && offset.find("eawr_time") == std::string_view::npos,
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
        const MaterialDescription exact = admissible(family, pass);
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
