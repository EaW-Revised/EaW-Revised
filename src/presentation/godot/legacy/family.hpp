#pragma once

#include "eawr/presentation/renderer.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Engine-free description of one legacy effect family whose Godot adapter
// lives in its own file under legacy/ (WP-43). The renderer consults the
// registry only for selectors (program and technique) outside its original
// selector table, so a family file never changes an existing adapter's source,
// bindings or pass. A family may add a technique to an original program
// (#199: the bump colorize DX9 technique beside its fixed-function row).
namespace eawr::presentation::godot_backend::legacy {

// A shader uniform a family binds after the consumer bindings and the shared
// legacy constants. Authored parameters reach the adapter through these typed
// eawr_* uniforms, whatever ALO chunk kind carried them.
using UniformValue = std::variant<float, std::array<float, 2>, std::array<float, 3>, std::array<float, 4>>;

struct Uniform final {
    std::string_view name;
    UniformValue value;
};

// Each sampled binding declares a neutral fallback for unresolved viewer assets.
enum class TexturePlaceholder { black, flat_normal };
struct TextureBinding final {
    std::string_view name;
    TexturePlaceholder placeholder{TexturePlaceholder::black};
};

struct Family final {
    std::string_view program;
    std::string_view technique;
    std::string_view pass_name;
    bool opaque{};
    bool transparent{};
    // False when the selected pass reads no engine light input. Such a pass
    // has no shadow-receiving form, so the renderer keeps its unshaded source
    // even when a lighting state requests shadows; that is not a variant
    // failure.
    bool receives_shadows{};
    // The vertex stage reads the scene wind and clock (#147), which the
    // renderer binds as eawr_wind and eawr_scene_time.
    bool reads_wind{};
    // Texture bindings besides BaseTexture that the adapter samples. Each
    // must reach the upload as its own per-binding texture; the renderer
    // never substitutes the base texture for one (gate MULTITEX-01).
    std::span<const TextureBinding> binding_textures{};
    // The vertex stage reads the authored binormal from CUSTOM0 as
    // coefficients of the unit tangent, the unit normal and their cross
    // product, so it survives skinning; Godot's own BINORMAL is always
    // cross(NORMAL, TANGENT), which authored frames often are not (#199).
    bool authored_binormals{};
    // The adapter source for a render pass the family admits.
    std::string_view (*shader)(RenderPass pass) noexcept {};
    // Why the authored bindings cannot reach this adapter, or nullopt. The
    // text completes "legacy material family '<program>' ...".
    std::optional<std::string> (*binding_problem)(const MaterialDescription& material) {};
    // Typed uniforms derived from bindings that passed binding_problem.
    std::vector<Uniform> (*uniforms)(const MaterialDescription& material) {};
};

// The CUSTOM0 value of an authored_binormals family: (a, b, c) with
// binormal = a t + b n + c cross(n, t) for the unit tangent t and unit normal
// n the upload stores (Cramer's rule on that basis). A vertex whose tangent is
// parallel to its normal has no such basis and keeps the binormal's
// projection on t and n.
[[nodiscard]] inline std::array<float, 3> binormal_coefficients(const std::array<float, 3>& normal,
    const std::array<float, 3>& tangent, const std::array<float, 3>& binormal) noexcept {
    const auto cross = [](const std::array<float, 3>& u, const std::array<float, 3>& v) {
        return std::array<float, 3>{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    };
    const auto dot = [](const std::array<float, 3>& u, const std::array<float, 3>& v) {
        return u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
    };
    const std::array<float, 3> side = cross(normal, tangent);
    const std::array<float, 3> normal_side = cross(normal, side);
    const float determinant = dot(tangent, normal_side);
    if (!(determinant < -1.0e-6F || determinant > 1.0e-6F)) {
        return {dot(binormal, tangent), dot(binormal, normal), 0.0F};
    }
    return {dot(binormal, normal_side) / determinant, dot(tangent, cross(binormal, side)) / determinant,
        dot(tangent, cross(normal, binormal)) / determinant};
}

} // namespace eawr::presentation::godot_backend::legacy
