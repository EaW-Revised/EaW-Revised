#include "legacy_family_support.hpp"
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

namespace legacy_family_test_support {


int failures = 0;

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

[[nodiscard]] bool near(const float left, const float right, const float tolerance) {
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


} // namespace


using namespace legacy_family_test_support;

int main() {
    texture_placeholder_contracts();
    ownership_colorization_contracts();
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
