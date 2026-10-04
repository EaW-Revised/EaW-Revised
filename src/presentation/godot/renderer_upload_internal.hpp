#pragma once
#include "renderer_internal.hpp"

namespace eawr::presentation::godot_backend {

[[nodiscard]] std::optional<std::string> binding_problem(const MaterialDescription& source,
    const std::vector<ReflectedUniform>& uniforms, const std::vector<std::string>& tokens);

} // namespace eawr::presentation::godot_backend
