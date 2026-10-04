#pragma once

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

namespace presentation = eawr::presentation;
namespace godot_backend = eawr::presentation::godot_backend;
namespace legacy = eawr::presentation::godot_backend::legacy;
using presentation::MaterialBinding;
using presentation::MaterialDescription;
using presentation::RenderPass;
extern int failures;



void check(const bool condition, const std::string_view message);
[[nodiscard]] bool near(const float left, const float right, const float tolerance = 1.0e-6F);
[[nodiscard]] MaterialDescription selector(const legacy::Family& family, const RenderPass pass);
[[nodiscard]] MaterialDescription admissible(const legacy::Family& family, const RenderPass pass);
[[nodiscard]] RenderPass admitted_pass(const legacy::Family& family);
[[nodiscard]] std::string rejection(const MaterialDescription& material);
[[nodiscard]] std::string_view glsl_type(const legacy::UniformValue& value);
[[nodiscard]] const legacy::Uniform* uniform(const std::vector<legacy::Uniform>& uniforms, const std::string_view name);
void registry_contracts();
void source_contracts();
void selector_contracts();
void binding_contracts();
void gloss_colorize_contracts();
void vegetation_contracts();
void bump_colorize_contracts();
void ownership_colorization_contracts();
void dx8_mesh_contracts();
void reference_contracts();
void texture_placeholder_contracts();
void derived_fog_contracts();

template <typename T>
[[nodiscard]] T value_of(const std::vector<legacy::Uniform>& uniforms, const std::string_view name) {
    const legacy::Uniform* item = uniform(uniforms, name);
    if (item == nullptr || !std::holds_alternative<T>(item->value)) {
        check(false, std::string("missing typed uniform ") + std::string(name));
        return T{};
    }
    return std::get<T>(item->value);
}


} // namespace legacy_family_test_support
