#pragma once

// The shader program names the space environment draws itself, and which environment route each one takes. One table:
// the environment scene's route selection and the load check's shader audit both read it, so they cannot drift apart.

#include "eawr/core/text.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace eawr::assets {

enum class EnvironmentShaderKind : std::uint8_t {
    none,
    meshgloss,
    meshadditive,
    meshadditive2x,
    meshadditive_vcolor,
    skydome,
    planet,
    nebula,
    legacy_mesh,
};

struct EnvironmentShaderName final {
    std::string_view name;
    EnvironmentShaderKind kind;
    // True when only the environment draws this program: a surface placed on a unit or prop is not drawn, and the model
    // is normally the map's own skydome or nebula. The load check does not list these (it cannot tell the two uses apart).
    bool environment_only;
};

inline constexpr std::array<EnvironmentShaderName, 12> environment_shader_names{{
    {"MeshGloss.fx", EnvironmentShaderKind::meshgloss, false},
    {"MeshGloss.fxo", EnvironmentShaderKind::meshgloss, false},
    {"MeshGlossForStarfields.fxo", EnvironmentShaderKind::meshgloss, true},
    {"MeshAdditive.fx", EnvironmentShaderKind::meshadditive, false},
    {"MeshAdditive.fxo", EnvironmentShaderKind::meshadditive, false},
    {"MeshAdditive2x.fxo", EnvironmentShaderKind::meshadditive2x, false},
    {"MeshAdditiveVColor.fx", EnvironmentShaderKind::meshadditive_vcolor, false},
    {"Skydome.fx", EnvironmentShaderKind::skydome, true},
    {"Planet.fx", EnvironmentShaderKind::planet, false},
    {"Nebula.fx", EnvironmentShaderKind::nebula, true},
    // The alpha companion of the planet model stays with that model's owner.
    {"MeshAlpha.fx", EnvironmentShaderKind::legacy_mesh, false},
    {"Planet_Deathstar.fx", EnvironmentShaderKind::legacy_mesh, false},
}};

[[nodiscard]] constexpr EnvironmentShaderKind environment_shader_kind(const std::string_view shader) noexcept {
    for (const EnvironmentShaderName& entry : environment_shader_names) {
        if (core::iequals(shader, entry.name)) return entry.kind;
    }
    return EnvironmentShaderKind::none;
}

[[nodiscard]] constexpr bool is_environment_only_shader(const std::string_view shader) noexcept {
    for (const EnvironmentShaderName& entry : environment_shader_names) {
        if (entry.environment_only && core::iequals(shader, entry.name)) return true;
    }
    return false;
}

} // namespace eawr::assets
