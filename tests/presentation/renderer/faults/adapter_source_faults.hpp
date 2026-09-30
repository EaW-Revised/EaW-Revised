#pragma once

// Test-only fault seam for the WP-08 renderer fault probe (#22). The probe
// build force-includes this header into its own compile of the unchanged
// production src/presentation/godot/renderer.cpp (-include, see
// CMakeLists.txt); nothing else includes it and no shared file is edited.
//
// renderer.cpp selects its fixed legacy adapter sources, and the fixed fog
// variants, by naming the constants of shader_adapter.hpp. That header is
// `#pragma once`, so including it here first and then defining a macro per
// constant redirects exactly those names in renderer.cpp to pick(), which
// returns the production text unless the probe armed a replacement for that
// slot. A macro does not expand inside its own replacement list, so the name
// inside pick(...) is the real constant. pick() also counts every read, so
// the probe proves the renderer consumed the armed text rather than assuming
// it. If renderer.cpp stops naming a constant, the read count stays zero and
// the probe fails; a new constant it names is not redirected until listed
// here. This is not the production binary: it is the production logic with
// the source text of one slot replaced.

#include "shader_adapter.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace eawr::presentation::godot_backend::fault_injection {

enum class Slot : std::size_t {
    meshgloss_opaque,
    meshgloss_alpha,
    rskin_opaque,
    fixed_mesh_opaque,
    fixed_mesh_alpha,
    fixed_mesh_opaque_fog,
    fixed_mesh_alpha_fog,
    count,
};

inline constexpr std::array<std::string_view, static_cast<std::size_t>(Slot::count)> slot_names{
    "meshgloss_shader_opaque", "meshgloss_shader_alpha", "rskin_shader_opaque",
    "fixed_mesh_shader_opaque", "fixed_mesh_shader_alpha", "fixed_mesh_shader_opaque_fog",
    "fixed_mesh_shader_alpha_fog",
};

struct State final {
    std::array<std::optional<std::string>, static_cast<std::size_t>(Slot::count)> replacement{};
    std::array<std::size_t, static_cast<std::size_t>(Slot::count)> reads{};
};

// One instance per loaded probe library (inline function, static local).
[[nodiscard]] inline State& state() {
    static State instance;
    return instance;
}

[[nodiscard]] inline std::string_view pick(const Slot slot, const std::string_view production) {
    State& current = state();
    const auto index = static_cast<std::size_t>(slot);
    ++current.reads[index];
    return current.replacement[index] ? std::string_view(*current.replacement[index]) : production;
}

// The production text of each slot, read without counting.
[[nodiscard]] inline std::string_view production(const Slot slot) {
    switch (slot) {
    case Slot::meshgloss_opaque: return godot_backend::meshgloss_shader_opaque;
    case Slot::meshgloss_alpha: return godot_backend::meshgloss_shader_alpha;
    case Slot::rskin_opaque: return godot_backend::rskin_shader_opaque;
    case Slot::fixed_mesh_opaque: return godot_backend::fixed_mesh_shader_opaque;
    case Slot::fixed_mesh_alpha: return godot_backend::fixed_mesh_shader_alpha;
    case Slot::fixed_mesh_opaque_fog: return godot_backend::fixed_mesh_shader_opaque_fog;
    case Slot::fixed_mesh_alpha_fog: return godot_backend::fixed_mesh_shader_alpha_fog;
    case Slot::count: break;
    }
    return {};
}

} // namespace eawr::presentation::godot_backend::fault_injection

#define EAWR_WP08_FAULT_PICK(slot, name) \
    (::eawr::presentation::godot_backend::fault_injection::pick( \
        ::eawr::presentation::godot_backend::fault_injection::Slot::slot, \
        ::eawr::presentation::godot_backend::name))
#define meshgloss_shader_opaque EAWR_WP08_FAULT_PICK(meshgloss_opaque, meshgloss_shader_opaque)
#define meshgloss_shader_alpha EAWR_WP08_FAULT_PICK(meshgloss_alpha, meshgloss_shader_alpha)
#define rskin_shader_opaque EAWR_WP08_FAULT_PICK(rskin_opaque, rskin_shader_opaque)
#define fixed_mesh_shader_opaque EAWR_WP08_FAULT_PICK(fixed_mesh_opaque, fixed_mesh_shader_opaque)
#define fixed_mesh_shader_alpha EAWR_WP08_FAULT_PICK(fixed_mesh_alpha, fixed_mesh_shader_alpha)
#define fixed_mesh_shader_opaque_fog EAWR_WP08_FAULT_PICK(fixed_mesh_opaque_fog, fixed_mesh_shader_opaque_fog)
#define fixed_mesh_shader_alpha_fog EAWR_WP08_FAULT_PICK(fixed_mesh_alpha_fog, fixed_mesh_shader_alpha_fog)
