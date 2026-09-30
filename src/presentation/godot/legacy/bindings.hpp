#pragma once

#include "eawr/presentation/renderer.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

// Shared reading of authored material bindings for the legacy/ family files.
// Effect parameters are bound by exact name. A parameter the ALO does not
// author keeps the effect's declared initializer (the MIT alo-viewer reference
// sets only authored parameters), so a family supplies that default itself.
namespace eawr::presentation::godot_backend::legacy::bindings {

// Caller-supplied text is echoed as at most 48 printable ASCII bytes.
[[nodiscard]] inline std::string echoed(const std::string_view text) {
    constexpr std::size_t limit = 48;
    std::string result("'");
    for (std::size_t index = 0; index < text.size() && index < limit; ++index) {
        const auto character = static_cast<unsigned char>(text[index]);
        result += character >= 0x20 && character < 0x7F ? static_cast<char>(character) : '?';
    }
    if (text.size() > limit) result += "...";
    return result + "'";
}

[[nodiscard]] inline std::size_t count(const MaterialDescription& material, const std::string_view name) noexcept {
    std::size_t result{};
    for (const MaterialBinding& binding : material.bindings) {
        if (binding.name == name) ++result;
    }
    return result;
}

[[nodiscard]] inline const MaterialBinding* find(
    const MaterialDescription& material, const std::string_view name) noexcept {
    for (const MaterialBinding& binding : material.bindings) {
        if (binding.name == name) return &binding;
    }
    return nullptr;
}

[[nodiscard]] inline std::string_view kind_name(const assets::ParameterValue& value) noexcept {
    switch (value.index()) {
    case 0: return "an integer";
    case 1: return "a scalar";
    case 2: return "a float3";
    case 3: return "a float4";
    default: return "a texture";
    }
}

// Why a colour or vector parameter cannot be read, or nullopt. Only a float3
// or float4 chunk can carry it, at most once, and every authored component
// must be finite, including one the effect does not read.
[[nodiscard]] inline std::optional<std::string> vector_problem(
    const MaterialDescription& material, const std::string_view name) {
    const std::size_t bound = count(material, name);
    if (bound == 0) return std::nullopt;
    if (bound > 1) {
        return "binds parameter " + echoed(name) + " " + std::to_string(bound) + " times";
    }
    const assets::ParameterValue& value = find(material, name)->value;
    std::array<float, 4> components{};
    std::size_t size{};
    if (const auto* three = std::get_if<assets::Vec3f>(&value)) {
        components = {three->x, three->y, three->z, 0.0F};
        size = 3;
    } else if (const auto* four = std::get_if<assets::Vec4f>(&value)) {
        components = {four->x, four->y, four->z, four->w};
        size = 4;
    } else {
        return "binds parameter " + echoed(name) + " as " + std::string(kind_name(value))
            + ", not a float3 or float4";
    }
    for (std::size_t index = 0; index < size; ++index) {
        if (!std::isfinite(components[index])) {
            return "binds a non-finite component in parameter " + echoed(name);
        }
    }
    return std::nullopt;
}

// The authored components, or `fallback` when the parameter is unbound. A
// float3 keeps the fallback's fourth component. Read after vector_problem.
[[nodiscard]] inline std::array<float, 4> vector_or(
    const MaterialDescription& material, const std::string_view name, const std::array<float, 4>& fallback) {
    const MaterialBinding* binding = find(material, name);
    if (binding == nullptr) return fallback;
    if (const auto* three = std::get_if<assets::Vec3f>(&binding->value)) {
        return {three->x, three->y, three->z, fallback[3]};
    }
    if (const auto* four = std::get_if<assets::Vec4f>(&binding->value)) {
        return {four->x, four->y, four->z, four->w};
    }
    return fallback;
}

// Why a float parameter cannot be read, or nullopt. A scalar or an integer
// chunk can carry it (an integer converts, as a D3DX integer set of a float
// parameter does), at most once, and the value must be finite.
[[nodiscard]] inline std::optional<std::string> scalar_problem(
    const MaterialDescription& material, const std::string_view name) {
    const std::size_t bound = count(material, name);
    if (bound == 0) return std::nullopt;
    if (bound > 1) {
        return "binds parameter " + echoed(name) + " " + std::to_string(bound) + " times";
    }
    const assets::ParameterValue& value = find(material, name)->value;
    if (const auto* scalar = std::get_if<float>(&value)) {
        if (!std::isfinite(*scalar)) return "binds a non-finite value in parameter " + echoed(name);
        return std::nullopt;
    }
    if (std::holds_alternative<std::int32_t>(value)) return std::nullopt;
    return "binds parameter " + echoed(name) + " as " + std::string(kind_name(value))
        + ", not a scalar or an integer";
}

// The authored value, or `fallback` when the parameter is unbound. Read after
// scalar_problem.
[[nodiscard]] inline float scalar_or(
    const MaterialDescription& material, const std::string_view name, const float fallback) {
    const MaterialBinding* binding = find(material, name);
    if (binding == nullptr) return fallback;
    if (const auto* scalar = std::get_if<float>(&binding->value)) return *scalar;
    if (const auto* integer = std::get_if<std::int32_t>(&binding->value)) return static_cast<float>(*integer);
    return fallback;
}

// ASCII case-insensitive equality: texture references resolve through the
// case-insensitive game file system.
[[nodiscard]] inline bool same_texture(const std::string_view left, const std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](const char value) {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
        };
        if (fold(left[index]) != fold(right[index])) return false;
    }
    return true;
}

[[nodiscard]] constexpr float saturate(const float value) noexcept {
    return value < 0.0F ? 0.0F : (value > 1.0F ? 1.0F : value);
}

} // namespace eawr::presentation::godot_backend::legacy::bindings
