#include "renderer_test_support.hpp"

namespace eawr_renderer_test {

// Removes each derived-fog block exactly once; true when all were present once.
bool strip_derived_fog(std::string& text) {
    using namespace eawr::presentation::godot_backend;
    const std::string declarations = "\n" + std::string(fog_stage1_declarations) + std::string(derived_fog_helpers);
    bool once = true;
    for (const std::string_view block : {std::string_view(declarations), fog_stage1_vertex, derived_fog_fragment}) {
        const std::size_t at = text.find(block);
        once = once && at != std::string::npos && text.find(block, at + 1) == std::string::npos;
        if (at != std::string::npos) text.erase(at, block.size());
    }
    return once;
}

void derived_fog_variant_contracts() {
    using namespace eawr::presentation::godot_backend;
    for (const auto& [name, source] : {std::pair{"MeshGloss opaque", meshgloss_shader_opaque},
             std::pair{"MeshGloss alpha", meshgloss_shader_alpha}, std::pair{"RSkin", rskin_shader_opaque},
             std::pair{"fixed opaque", fixed_mesh_shader_opaque}, std::pair{"fixed alpha", fixed_mesh_shader_alpha}}) {
        const auto derived = derived_legacy_fog_variant(source);
        check(derived.has_value(), std::string(name) + " has a derived fog variant");
        if (!derived) continue;
        std::string stripped = *derived;
        check(strip_derived_fog(stripped) && stripped == source,
              std::string(name) + " derived fog variant is its source plus exactly the three fog blocks");
        const std::size_t albedo = derived->find("ALBEDO = ");
        check(derived->find(derived_fog_fragment) == derived->find(';', albedo) + 1,
              std::string(name) + " derived fog multiply directly follows the single ALBEDO statement");
        check(derived->find(fog_stage1_vertex) > derived->find("void vertex() {")
                  && derived->find(fog_stage1_vertex) < derived->find("void fragment() {"),
              std::string(name) + " world position is written in vertex()");
    }
    constexpr std::string_view no_vertex =
        "shader_type spatial;\nrender_mode unshaded, cull_back;\nvoid fragment() {\n    ALBEDO = vec3(1.0);\n}\n";
    const auto added = derived_legacy_fog_variant(no_vertex);
    check(added && added->find("void vertex() {\n" + std::string(fog_stage1_vertex) + "}\n\nvoid fragment() {")
                  != std::string::npos,
          "a source without vertex() gains one that writes the world position");
    for (const auto& [name, source] : {
             std::pair{"two ALBEDO assignments", std::string_view(
                 "render_mode unshaded;\nvoid fragment() {\n    ALBEDO = vec3(1.0);\n    ALBEDO = vec3(0.5);\n}\n")},
             std::pair{"no render_mode", std::string_view("void fragment() {\n    ALBEDO = vec3(1.0);\n}\n")},
             std::pair{"no fragment()", std::string_view("render_mode unshaded;\nvoid vertex() {\n}\n")},
             std::pair{"ALBEDO outside fragment()", std::string_view(
                 "render_mode unshaded;\nvoid light() {\n    ALBEDO = vec3(1.0);\n}\nvoid fragment() {\n}\n")},
             std::pair{"existing fog uniform", std::string_view(
                 "render_mode unshaded;\nuniform bool eawr_fog_bound;\nvoid fragment() {\n    ALBEDO = vec3(1.0);\n}\n")},
             std::pair{"skip_vertex_transform", std::string_view(
                 "render_mode unshaded, skip_vertex_transform;\nvoid fragment() {\n    ALBEDO = vec3(1.0);\n}\n")},
             std::pair{"world_vertex_coords", std::string_view(
                 "render_mode unshaded, world_vertex_coords;\nvoid fragment() {\n    ALBEDO = vec3(1.0);\n}\n")}}) {
        check(!derived_legacy_fog_variant(source), std::string("derived fog variant refuses ") + name);
    }
}

namespace {

std::string rejection(const eawr::presentation::MaterialDescription& material) {
    const auto result = eawr::presentation::validate_material(material);
    return result ? std::string{} : result.error().code + " " + result.error().message;
}

// Sets the modern material's program to `source` and returns its rejection.
std::string compile_rejection(eawr::presentation::MaterialDescription& modern, const std::string_view source) {
    modern.program = std::string(source);
    const auto result = eawr::presentation::validate_material(modern);
    return result ? std::string{} : result.error().code + " " + result.error().message;
}

void legacy_adapter_contracts(const eawr::presentation::MaterialDescription& legacy) {
    using namespace eawr;
    presentation::MaterialDescription rskin = legacy;
    rskin.program = "RSkinBumpColorize.fx";
    check(static_cast<bool>(presentation::validate_material(rskin)),
        "accepted RSKIN fixed fallback selector should validate");
    rskin.pass = presentation::RenderPass::transparent;
    check(!presentation::validate_material(rskin),
        "opaque RSKIN adapter must reject transparent routing");
    presentation::MaterialDescription batch_alpha = legacy;
    batch_alpha.program = "BatchMeshAlpha.fx";
    batch_alpha.technique = "sph_t1";
    batch_alpha.pass_name = "sph_t1_p0";
    batch_alpha.pass = presentation::RenderPass::transparent;
    check(static_cast<bool>(presentation::validate_material(batch_alpha)),
        "BatchMeshAlpha fixed-function transparent draw pass should validate");
    batch_alpha.pass = presentation::RenderPass::opaque;
    check(!presentation::validate_material(batch_alpha),
        "BatchMeshAlpha must not draw in the opaque pass");
    batch_alpha.pass = presentation::RenderPass::transparent;
    batch_alpha.technique = "sph_t0";
    check(!presentation::validate_material(batch_alpha),
        "BatchMeshAlpha programmable technique is outside the bounded adapter");
    batch_alpha.technique = "sph_t1";
    batch_alpha.pass_name = "sph_t1_p1";
    check(!presentation::validate_material(batch_alpha),
        "BatchMeshAlpha cleanup pass must not be submitted as geometry");
    batch_alpha.pass_name = "sph_t1_p0";
    batch_alpha.program = "BatchMeshAlphaOther.fx";
    check(!presentation::validate_material(batch_alpha),
        "BatchMeshAlpha family names must not select the exact adapter by prefix");
    const std::string_view alpha_shader = presentation::godot_backend::fixed_mesh_shader_alpha;
    check(alpha_shader.find("blend_mix, depth_draw_never, depth_test_default, cull_back")
            != std::string_view::npos
            && alpha_shader.find("ALPHA = base_sample.a * eawr_vertex_diffuse.a;")
                != std::string_view::npos
            && alpha_shader.find("Diffuse.a * eawr_light_scale.a") != std::string_view::npos,
        "bounded fixed-function alpha path must blend texture and interpolated material alpha"
        " without depth writes or back-face draw");
    using namespace presentation::godot_backend;
    std::string alpha_derived(fixed_mesh_shader_alpha_fog);
    bool alpha_blocks_once = true;
    for (const std::string_view block : {fog_stage1_declarations, fog_stage1_vertex, fog_stage1_fragment}) {
        const std::size_t at = alpha_derived.find(block);
        alpha_blocks_once = alpha_blocks_once && at != std::string::npos
            && alpha_derived.find(block, at + 1) == std::string::npos;
        if (at != std::string::npos) alpha_derived.erase(at, block.size());
    }
    check(alpha_blocks_once && alpha_derived == alpha_shader,
        "BatchMeshAlpha fog variant must equal its baseline plus exactly one fog stage");
    check(fixed_mesh_shader_alpha_fog.find("eawr_linear_rgb *= eawr_fog_attenuation();")
            < fixed_mesh_shader_alpha_fog.find("ALBEDO = eawr_linear_to_srgb(eawr_linear_rgb);")
            && fixed_mesh_shader_alpha_fog.find("ALPHA = base_sample.a * eawr_vertex_diffuse.a;")
                != std::string_view::npos,
        "BatchMeshAlpha fog must attenuate linear RGB before transfer without changing alpha");
    presentation::MaterialDescription unsupported_legacy = legacy;
    unsupported_legacy.program = "OtherEffect.fx";
    check(!presentation::validate_material(unsupported_legacy),
        "legacy material must reject an effect without an implemented Godot adapter");
    unsupported_legacy = legacy;
    unsupported_legacy.technique = "other_technique";
    check(!presentation::validate_material(unsupported_legacy),
        "legacy material must reject an unsupported technique selector");
    unsupported_legacy = legacy;
    unsupported_legacy.pass_name = "other_pass";
    check(!presentation::validate_material(unsupported_legacy),
        "legacy material must reject an unsupported pass selector");
    unsupported_legacy = legacy;
    unsupported_legacy.pass = presentation::RenderPass::alpha_tested;
    check(!presentation::validate_material(unsupported_legacy),
        "legacy material must reject routing without a ported shader variant");
}

void legacy_rejection_contracts(const eawr::presentation::MaterialDescription& legacy) {
    using namespace eawr;
    // Each legacy rejection is one EAWR-RENDER-0001 naming the first selector
    // outside the adapter table, never a generic or compile diagnostic.
    presentation::MaterialDescription unsupported_legacy = legacy;
    unsupported_legacy.program = "UnknownEffect.fx";
    check(rejection(unsupported_legacy) == "EAWR-RENDER-0001 unknown legacy material family"
            " 'UnknownEffect.fx' has no implemented Godot adapter",
        "unknown legacy family must be named as the rejected selector");
    unsupported_legacy = legacy;
    unsupported_legacy.technique = "sph_t9";
    check(rejection(unsupported_legacy) == "EAWR-RENDER-0001 legacy material family 'MeshGloss.fx'"
            " has no Godot adapter for technique 'sph_t9' (supported: 'sph_t0')",
        "unsupported legacy technique must be named with the family's supported technique");
    unsupported_legacy = legacy;
    unsupported_legacy.pass_name = "sph_t0_p9";
    check(rejection(unsupported_legacy) == "EAWR-RENDER-0001 legacy material family 'MeshGloss.fx'"
            " has no Godot adapter for pass 'sph_t0_p9' (supported: 'sph_t0_p0')",
        "unsupported legacy pass selector must be named with the family's supported pass");
    unsupported_legacy = legacy;
    unsupported_legacy.program = "BatchMeshGloss.fx";
    unsupported_legacy.technique = "sph_t1";
    unsupported_legacy.pass_name = "sph_t1_p0";
    unsupported_legacy.pass = presentation::RenderPass::transparent;
    check(rejection(unsupported_legacy) == "EAWR-RENDER-0001 legacy material family"
            " 'BatchMeshGloss.fx' cannot draw in the transparent render pass",
        "a known selector in a render pass its adapter lacks must name that pass");
    unsupported_legacy.pass = presentation::RenderPass::post;
    check(rejection(unsupported_legacy).ends_with("cannot draw in the post render pass"),
        "no legacy adapter may enter the post pass");
    unsupported_legacy = legacy;
    unsupported_legacy.program = "meshgloss.fx";
    check(rejection(unsupported_legacy).find("unknown legacy material family") != std::string::npos,
        "legacy family names are exact, not case-folded");
    unsupported_legacy.program = std::string(200, 'x') + "\n\x7f";
    const std::string bounded = rejection(unsupported_legacy);
    check(bounded.find(std::string(48, 'x') + "...'") != std::string::npos
            && bounded.find(std::string(49, 'x')) == std::string::npos
            && bounded.find('\n') == std::string::npos && bounded.size() < 160,
        "caller-supplied selector text must be truncated and printable in the diagnostic");
    unsupported_legacy = legacy;
    unsupported_legacy.technique = "sph\t0";
    check(rejection(unsupported_legacy).find("technique 'sph?0'") != std::string::npos,
        "control characters in selectors must not reach the diagnostic");
    for (const auto& [program, technique, pass_name, pass] : {
             std::tuple{"MeshGloss.fx", "sph_t0", "sph_t0_p0", presentation::RenderPass::transparent},
             std::tuple{"MeshAlpha.fx", "sph_t1", "sph_t1_p0", presentation::RenderPass::transparent},
             std::tuple{"MeshAlphaGloss.fx", "sph_t1", "sph_t1_p0", presentation::RenderPass::transparent},
             std::tuple{"BatchMeshGloss.fx", "sph_t1", "sph_t1_p0", presentation::RenderPass::opaque},
             std::tuple{"MeshBumpColorize.fx", "t0", "t0_p0", presentation::RenderPass::opaque},
             std::tuple{"RSkinGloss.fx", "sph_t1", "sph_t1_p0", presentation::RenderPass::opaque},
             std::tuple{"RSkinGlossColorize.fx", "sph_t0", "sph_t0_p0", presentation::RenderPass::opaque}}) {
        presentation::MaterialDescription supported = legacy;
        supported.program = program;
        supported.technique = technique;
        supported.pass_name = pass_name;
        supported.pass = pass;
        check(rejection(supported).empty(), "every implemented legacy adapter selector must still validate");
    }
}

void modern_preflight_contracts(eawr::presentation::MaterialDescription& modern) {
    using namespace eawr;
    // A canvas_item shader that mentions the spatial declaration in a comment
    // compiles in Godot, so only the leading-declaration rule refuses it.
    constexpr std::string_view disguised =
        "shader_type canvas_item; // shader_type spatial;\nvoid fragment() { COLOR = vec4(1.0); }";
    check(compile_rejection(modern, disguised)
            == "EAWR-RENDER-0007 modern spatial shader must begin with 'shader_type spatial;'",
        "a non-spatial shader naming the spatial declaration in a comment must be rejected");
    check(!presentation::spatial_declaration_end(disguised),
        "no probe anchor may be found in a non-spatial shader");
    check(compile_rejection(modern, "render_mode unshaded;\nshader_type spatial;\nvoid fragment() {}")
            .ends_with("must begin with 'shader_type spatial;'"),
        "the spatial declaration must be the first statement, as Godot requires");
    check(compile_rejection(modern, "shader_type spatial_extra;\nvoid fragment() {}")
            .ends_with("must begin with 'shader_type spatial;'"),
        "a longer identifier is not the spatial shader type");
    check(compile_rejection(modern, "shader_type spatial;\n/* unterminated\nvoid fragment() {}")
            == "EAWR-RENDER-0007 modern spatial shader has an unterminated comment or string literal",
        "an unterminated block comment must fail before upload");
    // Spatial sources with only vertex() or only light() compile in the pinned
    // Godot, so omitting fragment() is a contract restriction, never reported
    // as a compile failure; one that would also fail to compile keeps 0007.
    constexpr std::string_view no_fragment = "EAWR-RENDER-0001 modern spatial material must declare"
        " a fragment() entry point (renderer contract; the source was not compiled)";
    check(compile_rejection(modern, "shader_type spatial;\nvoid vertex() { VERTEX *= 1.0; }\n") == no_fragment,
        "a vertex-only spatial shader is outside the contract, not a compile failure");
    check(compile_rejection(modern, "shader_type spatial;\nvoid light() { DIFFUSE_LIGHT += vec3(ATTENUATION); }\n")
            == no_fragment,
        "a light-only spatial shader is outside the contract, not a compile failure");
    check(compile_rejection(modern, "shader_type spatial;\n// void fragment() {}\nvoid vertex() {}") == no_fragment,
        "a fragment entry point inside a comment does not count");
    check(compile_rejection(modern, "shader_type spatial;\nvoid my_fragment() {}") == no_fragment,
        "a longer function name is not the fragment entry point");
    check(compile_rejection(modern, "shader_type spatial;\nvoid vertex() {")
            == "EAWR-RENDER-0007 modern spatial shader has unbalanced braces",
        "a source that cannot compile keeps the compile code even without fragment()");
    check(compile_rejection(modern, "shader_type spatial;\nvoid fragment() { } /* } */")
            .empty(),
        "braces inside comments must not unbalance an otherwise valid source");
    check(compile_rejection(modern, "shader_type spatial;\nvoid fragment() { /* { */ }\n}")
            == "EAWR-RENDER-0007 modern spatial shader has unbalanced braces",
        "a stray closing brace in code must still be rejected");
    constexpr std::string_view comment_led =
        "// Leading comment; its semicolon precedes the declaration.\n"
        "/* block; */ shader_type  spatial ;\nvoid\n  fragment ( ) { ALBEDO = vec3(1.0); }\n";
    check(compile_rejection(modern, comment_led).empty(),
        "leading comments and free whitespace must not reject a valid spatial source");
    const auto anchor = presentation::spatial_declaration_end(comment_led);
    check(anchor && comment_led.substr(0, *anchor).ends_with("spatial ;")
            && comment_led.substr(*anchor).starts_with("\nvoid"),
        "the compile probe anchor must sit directly after the leading declaration's semicolon");
    const std::string long_rejection = compile_rejection(modern,
        "shader_type canvas_item; void fragment() { COLOR = vec4(" + std::string(4096, 'z') + "); }");
    check(long_rejection.find("zzzz") == std::string::npos && long_rejection.size() < 128,
        "shader text must never enter a preflight diagnostic");
}

void modern_material_contracts() {
    using namespace eawr;
    presentation::MaterialDescription modern{
        .schema_version = 1,
        .route = presentation::MaterialRoute::modern_spatial,
        .pass = presentation::RenderPass::opaque,
        .program = "shader_type spatial; void fragment(){ALBEDO=vec3(1.0);}",
        .technique = {},
        .pass_name = {},
        .bindings = {},
    };
    check(static_cast<bool>(presentation::validate_material(modern)),
        "independent modern material should validate");
    modern.technique = "legacy-only";
    check(!presentation::validate_material(modern),
        "modern material must reject legacy selectors");
    modern.technique.clear();
    modern.program = "shader_type canvas_item; void fragment() { COLOR = vec4(1.0); }";
    const auto invalid_shader = presentation::validate_material(modern);
    check(!invalid_shader, "modern shader must fail before allocating a shader RID when it is not spatial");
    check(!invalid_shader && invalid_shader.error().code
            == presentation::diagnostic_codes::shader_compile_failed,
        "modern shader preflight failure must carry the bounded compile diagnostic code");
    modern.program = "shader_type spatial; void fragment() { ALBEDO = vec3(1.0);";
    check(!presentation::validate_material(modern),
        "modern shader with unmatched braces must fail before upload");
    modern_preflight_contracts(modern);
    modern.program = "shader_type spatial; void fragment(){ALBEDO=vec3(1.0);}";
    modern.route = static_cast<presentation::MaterialRoute>(255);
    check(!presentation::validate_material(modern),
        "unknown material route must fail closed instead of selecting modern spatial");
    modern.route = presentation::MaterialRoute::modern_spatial;
    modern.pass = static_cast<presentation::RenderPass>(255);
    check(!presentation::validate_material(modern),
        "unknown material pass must fail closed instead of entering a pass queue");
}

} // namespace

void surface_material_contracts() {
    using namespace eawr;

    presentation::MaterialDescription legacy{
        .schema_version = 1,
        .route = presentation::MaterialRoute::legacy_effect,
        .pass = presentation::RenderPass::opaque,
        .program = "MeshGloss.fx",
        .technique = "sph_t0",
        .pass_name = "sph_t0_p0",
        .bindings = {},
    };
    check(static_cast<bool>(presentation::validate_material(legacy)),
        "versioned legacy material should validate");
    legacy_adapter_contracts(legacy);
    legacy_rejection_contracts(legacy);
    modern_material_contracts();
    legacy.schema_version = 2;
    check(!presentation::validate_material(legacy),
        "unknown material schema version must fail closed");
}

} // namespace eawr_renderer_test
