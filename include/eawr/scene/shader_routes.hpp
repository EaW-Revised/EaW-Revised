#pragma once

// Which route draws a placed surface of a given shader program, and an audit of a mounted mod stack for the shader names
// that no route handles. The audit is the load check's answer to "which layers will a mod silently lose": a surface whose
// shader has no hand-written adapter, no alias, no planet/environment/missing-effect route and no native translation is
// reported shader_unsupported at populate time and its layer is not drawn (scene::Cause::shader_unsupported does not block
// the placement). The same classification runs here over model headers, before any map is loaded.

#include "eawr/core/load_note.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/vfs/vfs_fwd.hpp"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::scene {

enum class ShaderRoute : std::uint8_t {
    hand_adapter,     // a legacy selector (including its compiled-name and source-name aliases)
    planet,           // the stock planet material on a unit or prop
    missing_effect,   // the effect file is absent from the mounted view: the engine's built-in material
    environment,      // drawn by the space environment (skydome, nebula, starfield gloss), not by the placed-surface route
    intentional_skip, // stencil shadow volumes: geometry for a shadow pass, never drawn in colour
    translated,       // admitted by the native shader translation (experimental, off by default)
    unhandled,        // none of the above: the surface is reported shader_unsupported and its layer is dropped
};

[[nodiscard]] std::string_view to_string(ShaderRoute route) noexcept;

// The route of one surface. `effect_missing` is scene::missing_material_effect for the program; `translated` is whether the
// native translation admitted it (always false where no renderer runs).
[[nodiscard]] ShaderRoute shader_route(std::string_view shader, bool effect_missing, bool translated = false) noexcept;

// The names only the space environment draws (assets/environment_shaders.hpp, the table the environment scene's route
// selection also reads).
[[nodiscard]] bool is_environment_route_shader(std::string_view shader) noexcept;

struct ShaderAuditEntry final {
    std::string shader;        // first spelling seen
    std::string layer;         // the mounted layer that supplies the winning model files
    std::string example_model; // a logical path of one model that names it
    std::uint64_t meshes{};    // visible submeshes naming it
    std::uint64_t models{};    // winning model files naming it on a visible mesh
};

struct ShaderAudit final {
    std::uint64_t models_scanned{};
    std::uint64_t models_unreadable{};
    std::uint64_t visible_meshes{};
    // Visible submesh count per route (every route, so a reader sees how much of the stack each one carries).
    std::map<ShaderRoute, std::uint64_t> by_route;
    // The unhandled names, per layer, ordered by layer then descending mesh count then name.
    std::vector<ShaderAuditEntry> unhandled;
};

// Scans every winning model of data/art/models in the mounted view by its chunk headers only. `translated` may be empty.
[[nodiscard]] ShaderAudit audit_model_shaders(const vfs::Vfs& filesystem, bool (*translated)(std::string_view) = nullptr);

// What a populate actually dropped. A surface of a drawn placement that has no material (its shader is unsupported) is not
// drawn; this groups those surfaces by shader so a map's report says which layers of which models are missing. Stencil
// shadow volumes are listed with route intentional_skip and left out of `unhandled_surfaces`.
struct DroppedLayer final {
    std::string shader;       // first spelling seen
    ShaderRoute route{ShaderRoute::unhandled};
    std::uint64_t surfaces{}; // dropped surfaces over every drawn placement (one per placement and submesh)
    std::uint64_t placements{};
    std::string object_type;  // the first placement's object type
    std::string model_path;   // and its model
};

struct DroppedLayers final {
    std::uint64_t surfaces{};            // every dropped surface
    std::uint64_t unhandled_surfaces{};  // those whose route is unhandled (the real gap)
    std::vector<DroppedLayer> by_shader; // descending surface count, then name
};

// `drawn` are the placements the scene draws (a land placement that is drawable, a space placement whose role is drawn).
[[nodiscard]] DroppedLayers dropped_layers(std::span<const Placement* const> drawn);

// {"surfaces": n, "unhandled_surfaces": n, "by_shader": [{"shader", "route", "surfaces", "placements", "object_type",
// "model_path"}, ...]}
[[nodiscard]] std::string dropped_layers_json(const DroppedLayers& dropped);

// One log line: "N dropped surfaces (M unhandled): Shader xK, ..." or "no dropped surfaces".
[[nodiscard]] std::string dropped_layers_summary(const DroppedLayers& dropped);

// One class C note per unhandled (layer, shader), action skipped_object: the layer-relative file is the example model.
[[nodiscard]] std::vector<core::LoadNote> shader_audit_notes(const ShaderAudit& audit);

inline constexpr std::string_view shader_unrouted_code = "EAWR-LOAD-2000";

} // namespace eawr::scene
