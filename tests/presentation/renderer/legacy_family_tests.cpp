// CPU contracts for the legacy/ family adapters (WP-43): exact selectors,
// fail-closed diagnostics, binding validation and defaults, derived uniforms,
// adapter source render states and the engine-free reference arithmetic.
#include "eawr/presentation/renderer.hpp"

#include "legacy/registry.hpp"
#include "shader_adapter.hpp"
#include "../../../apps/viewer/src/family_textures.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <regex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace presentation = eawr::presentation;
namespace godot_backend = eawr::presentation::godot_backend;
namespace legacy = eawr::presentation::godot_backend::legacy;
using presentation::MaterialBinding;
using presentation::MaterialDescription;
using presentation::RenderPass;

int failures = 0;

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

[[nodiscard]] bool near(const float left, const float right, const float tolerance = 1.0e-6F) {
    return std::fabs(left - right) <= tolerance;
}

[[nodiscard]] MaterialDescription selector(const legacy::Family& family, const RenderPass pass) {
    return MaterialDescription{
        .schema_version = MaterialDescription::current_schema_version,
        .route = presentation::MaterialRoute::legacy_effect,
        .pass = pass,
        .program = std::string(family.program),
        .technique = std::string(family.technique),
        .pass_name = std::string(family.pass_name),
        .bindings = {},
    };
}

// The selector with the fewest bindings the family admits: every family
// keeps the effect initializers for unauthored parameters, except that
// MeshGlossColorize and MeshAlphaGloss need a GlossTexture and the bump
// colorize pair a NormalTexture.
[[nodiscard]] MaterialDescription admissible(const legacy::Family& family, const RenderPass pass) {
    MaterialDescription material = selector(family, pass);
    if (family.program == "MeshGlossColorize.fx") {
        material.bindings = {{"BaseTexture", std::string("base.tga")}, {"GlossTexture", std::string("base.tga")}};
    }
    if (family.authored_binormals) {
        material.bindings = {{"BaseTexture", std::string("hull.dds")}, {"NormalTexture", std::string("hull_b.dds")}};
    }
    if (family.program == "MeshAlphaGloss.fx") {
        material.bindings = {{"BaseTexture", std::string("rock.tga")}, {"GlossTexture", std::string("rock_gloss.tga")}};
    }
    return material;
}

[[nodiscard]] RenderPass admitted_pass(const legacy::Family& family) {
    return family.opaque ? RenderPass::opaque : RenderPass::transparent;
}

[[nodiscard]] std::string rejection(const MaterialDescription& material) {
    const auto result = presentation::validate_material(material);
    return result ? std::string{} : result.error().code + " " + result.error().message;
}

[[nodiscard]] std::string_view glsl_type(const legacy::UniformValue& value) {
    switch (value.index()) {
    case 0: return "float";
    case 1: return "vec2";
    case 2: return "vec3";
    default: return "vec4";
    }
}

[[nodiscard]] const legacy::Uniform* uniform(const std::vector<legacy::Uniform>& uniforms, const std::string_view name) {
    for (const legacy::Uniform& item : uniforms) {
        if (item.name == name) return &item;
    }
    return nullptr;
}

template <typename T>
[[nodiscard]] T value_of(const std::vector<legacy::Uniform>& uniforms, const std::string_view name) {
    const legacy::Uniform* item = uniform(uniforms, name);
    if (item == nullptr || !std::holds_alternative<T>(item->value)) {
        check(false, std::string("missing typed uniform ") + std::string(name));
        return T{};
    }
    return std::get<T>(item->value);
}

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
    check(registry.size() == 11,
        "the registry has the four WP-43 families, Tree, Grass, the bump colorize pair and the three DX8 mesh families");
    const std::vector<std::tuple<std::string_view, std::string_view, std::string_view, bool, bool, bool>> expected{
        {"MeshAdditive.fx", "t0", "t0_p0", false, true, false},
        {"MeshAdditiveOffset.fx", "t0", "t0_p0", false, true, false},
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
             "MeshAdditiveVColor.fx", "alDefault.fx", "Planet.fx", "Nebula.fx", "TerrainMeshBump.fx"}) {
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
            && additive.find("clamp(eawr_color * eawr_light_scale.rgb * eawr_light_scale.a, 0.0, 1.0)")
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
            && colorize.find("mix(base_linear_rgb, eawr_colorization * base_linear_rgb, base_sample.a)")
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
    for (const std::string_view unknown : {"alDefault.fx", "MeshAdditiveVColor.fx", "meshadditive.fx",
             "Planet.fx", "Nebula.fx", "TerrainMeshBump.fx", "TerrainMeshGloss.fx", "MeshShadowVolume.fx"}) {
        MaterialDescription material = selector(legacy::registry()[0], RenderPass::transparent);
        material.program = std::string(unknown);
        check(rejection(material) == "EAWR-RENDER-0001 unknown legacy material family '" + std::string(unknown)
                + "' has no implemented Godot adapter",
            std::string(unknown) + " stays an unknown legacy family");
    }
}

void binding_contracts() {
    const legacy::Family& additive = legacy::mesh_additive::family;
    MaterialDescription material = selector(additive, RenderPass::transparent);

    const auto defaults = legacy::uniforms(material);
    check(value_of<std::array<float, 3>>(defaults, "eawr_color") == std::array<float, 3>{1.0F, 1.0F, 1.0F}
            && value_of<std::array<float, 2>>(defaults, "eawr_uv_scroll_rate") == std::array<float, 2>{0.0F, 0.0F}
            && value_of<float>(defaults, "eawr_time") == 0.0F,
        "unauthored MeshAdditive parameters keep the effect initializers and TIME is fixed at 0");

    material.bindings = {
        {"BaseTexture", std::string("w_glow.tga")},
        {"UVScrollRate", eawr::assets::Vec4f{0.25F, -0.5F, 7.0F, 9.0F}},
        {"Color", eawr::assets::Vec4f{0.5F, 0.75F, 1.5F, 0.2F}},
        {"Emissive", eawr::assets::Vec4f{9.0F, 9.0F, 9.0F, 9.0F}},
    };
    check(rejection(material).empty(), "authored float4 MeshAdditive parameters validate; unread ones are ignored");
    const auto authored = legacy::uniforms(material);
    check(value_of<std::array<float, 3>>(authored, "eawr_color") == std::array<float, 3>{0.5F, 0.75F, 1.5F}
            && value_of<std::array<float, 2>>(authored, "eawr_uv_scroll_rate") == std::array<float, 2>{0.25F, -0.5F},
        "MeshAdditive binds Color.rgb unclamped and UVScrollRate.xy exactly");
    material.bindings[2].value = eawr::assets::Vec3f{0.25F, 0.375F, 0.5F};
    check(rejection(material).empty()
            && value_of<std::array<float, 3>>(legacy::uniforms(material), "eawr_color")
                == std::array<float, 3>{0.25F, 0.375F, 0.5F},
        "a float3 Color binds its three components");

    const auto rejected = [&](const MaterialDescription& changed, const std::string_view fragment, const std::string_view why) {
        const std::string message = rejection(changed);
        check(message.starts_with("EAWR-RENDER-0001 legacy material family 'MeshAdditive.fx' ")
                && message.find(fragment) != std::string::npos,
            why);
        check(!legacy::shader_source(changed), std::string(why) + " (upload selection)");
    };
    MaterialDescription changed = material;
    changed.bindings[2].value = 1.0F;
    rejected(changed, "binds parameter 'Color' as a scalar, not a float3 or float4", "a scalar Color fails closed");
    changed.bindings[2].value = std::int32_t{1};
    rejected(changed, "as an integer", "an integer Color fails closed");
    changed.bindings[2].value = std::string("red.tga");
    rejected(changed, "as a texture", "a texture Color fails closed");
    changed = material;
    changed.bindings[1].value = eawr::assets::Vec4f{0.0F, 0.0F, 0.0F, std::numeric_limits<float>::quiet_NaN()};
    rejected(changed, "binds a non-finite component in parameter 'UVScrollRate'",
        "a non-finite unread UVScrollRate component fails closed");
    changed = material;
    changed.bindings[2].value = eawr::assets::Vec4f{std::numeric_limits<float>::infinity(), 0.0F, 0.0F, 1.0F};
    rejected(changed, "non-finite component in parameter 'Color'", "an infinite Color fails closed");
    changed = material;
    changed.bindings.push_back({"Color", eawr::assets::Vec4f{1.0F, 1.0F, 1.0F, 1.0F}});
    rejected(changed, "binds parameter 'Color' 2 times", "a duplicated Color fails closed");
    changed = material;
    changed.bindings[1].value = eawr::assets::Vec3f{0.5F, 0.25F, 0.0F};
    check(rejection(changed).empty()
            && value_of<std::array<float, 2>>(legacy::uniforms(changed), "eawr_uv_scroll_rate")
                == std::array<float, 2>{0.5F, 0.25F},
        "a float3 UVScrollRate binds its first two components");

    MaterialDescription offset = selector(legacy::mesh_additive_offset::family, RenderPass::transparent);
    offset.bindings = {{"UVOffset", eawr::assets::Vec4f{0.5F, -0.25F, 3.0F, 4.0F}},
        {"Color", eawr::assets::Vec4f{0.2F, 0.4F, 0.6F, 1.0F}}};
    const auto offset_uniforms = legacy::uniforms(offset);
    check(rejection(offset).empty()
            && value_of<std::array<float, 2>>(offset_uniforms, "eawr_uv_offset") == std::array<float, 2>{0.5F, -0.25F}
            && value_of<std::array<float, 3>>(offset_uniforms, "eawr_color") == std::array<float, 3>{0.2F, 0.4F, 0.6F}
            && uniform(offset_uniforms, "eawr_time") == nullptr,
        "MeshAdditiveOffset binds UVOffset.xy and Color.rgb and no time");
    offset.bindings[0].value = 2.0F;
    check(rejection(offset).find("binds parameter 'UVOffset' as a scalar") != std::string::npos,
        "a scalar UVOffset fails closed");

    MaterialDescription solid = selector(legacy::mesh_solid_color::family, RenderPass::opaque);
    check(value_of<std::array<float, 3>>(legacy::uniforms(solid), "eawr_color") == std::array<float, 3>{0.0F, 0.0F, 1.0F},
        "an unauthored MeshSolidColor Color keeps the effect initializer");
    solid.bindings = {{"Color", eawr::assets::Vec4f{0.098F, 0.992F, 0.024F, 0.482F}}};
    check(rejection(solid).empty()
            && value_of<std::array<float, 3>>(legacy::uniforms(solid), "eawr_color")
                == std::array<float, 3>{0.098F, 0.992F, 0.024F},
        "MeshSolidColor binds Color.rgb");
    solid.bindings[0].value = std::string("marker.tga");
    check(rejection(solid).find("'MeshSolidColor.fx' binds parameter 'Color' as a texture") != std::string::npos,
        "a texture MeshSolidColor Color fails closed");
}

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
                && source.find("mix(base.rgb, eawr_colorization * base.rgb, base.a)") != std::string::npos
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
                && source.find("eawr_diffuse.rgb * irradiance * eawr_light_scale.rgb + eawr_emissive") != std::string::npos
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

void texture_placeholder_contracts() {
    MaterialDescription material = admissible(legacy::bump_colorize::mesh_family, RenderPass::opaque);
    const auto missing = [](std::string_view) -> std::optional<eawr::assets::Texture> { return std::nullopt; };
    const auto normal = godot_backend::family_binding_textures(material, missing);
    check(normal.size() == 1 && normal[0].texture.mips[0].bytes == std::vector<std::byte>{
        std::byte{128}, std::byte{128}, std::byte{255}, std::byte{0}},
        "unresolved NormalTexture is flat with zero gloss alpha");
    // Exercise a gloss binding without admitting the separate #200 family here.
    constexpr legacy::TextureBinding gloss_bindings[]{{"GlossTexture", legacy::TexturePlaceholder::black}};
    material.bindings.push_back({"GlossTexture", std::string("missing-gloss.dds")});
    const auto gloss = godot_backend::family_binding_textures(material, gloss_bindings, missing);
    check(gloss.size() == 1 && gloss[0].texture.mips[0].bytes == std::vector<std::byte>(4, std::byte{0}),
        "unresolved GlossTexture has zero red gloss, not the flat normal's half gloss");
    const auto loaded = godot_backend::family_binding_textures(material, gloss_bindings,
        [](std::string_view) -> std::optional<eawr::assets::Texture> {
            auto texture = godot_backend::family_placeholder_texture(legacy::TexturePlaceholder::black);
            texture.mips[0].bytes[0] = std::byte{231};
            return texture;
        });
    check(loaded[0].texture.mips[0].bytes[0] == std::byte{231}, "resolved gloss is preserved");
}

} // namespace

void derived_fog_contracts() {
    // Land map fog (#28) derives a fog stage for every family it draws.
    for (const legacy::Family& family : legacy::registry()) {
        const auto derived = eawr::presentation::godot_backend::derived_legacy_fog_variant(
            family.shader(admitted_pass(family)));
        check(derived.has_value() && derived->find("eawr_fog_attenuation()") != std::string::npos,
              std::string(family.program) + " has a derived fog-stub-v1 variant");
    }
}

int main() {
    texture_placeholder_contracts();
    registry_contracts();
    derived_fog_contracts();
    source_contracts();
    selector_contracts();
    binding_contracts();
    gloss_colorize_contracts();
    vegetation_contracts();
    reference_contracts();
    bump_colorize_contracts();
    dx8_mesh_contracts();
    if (failures != 0) {
        std::cerr << failures << " legacy family contract(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "legacy family contracts passed\n";
    return EXIT_SUCCESS;
}
