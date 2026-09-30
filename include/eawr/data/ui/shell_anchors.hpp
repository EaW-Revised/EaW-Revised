#pragma once

// Command-bar shell anchors (design docs/ui/ui-layer.md section 1.3, rule UI-L2,
// ticket UI-03 #170). A shell model's mesh names are component names; each
// mesh rect encloses its vertices after the full bone-chain transform.
// Units are reference units (UI-L1), origin bottom-left and y up.

#include "eawr/assets/assets.hpp"
#include "eawr/core/result.hpp"
#include "eawr/data/ui/command_bar.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::data::ui {

struct ReferenceRect {
    float x{};
    float y{};
    float width{};
    float height{};

    [[nodiscard]] float right() const noexcept { return x + width; }
    [[nodiscard]] float top() const noexcept { return y + height; }

    friend bool operator==(const ReferenceRect&, const ReferenceRect&) = default;
};

// Column-major affine matrix in the asset basis, as used by the ALA player.
using ShellTransform = std::array<float, 16>;
struct ShellVertex {
    assets::Vec3f position;
    assets::Vec2f uv;
};
struct ShellTriangle {
    std::array<ShellVertex, 3> vertices;
    std::string base_texture;
};

struct ShellAnchor {
    std::string name;        // mesh name as authored
    AltName alt;             // `_ALT<n>` faction variant, if any
    ReferenceRect rect;
    float z_min{};           // layering depth in the shell
    float z_max{};
    bool visible{true};      // mesh visibility flag in the model
    std::string shader;      // first submesh shader, e.g. MeshAlpha.fx
    std::string base_texture; // first submesh BaseTexture, empty if none
    std::string bone;
    // The bone's model-space origin, where the engine centres the component's
    // texture quads (a button's state art); absent for a mesh without a bone.
    std::optional<assets::Vec2f> origin;
    std::vector<ShellTriangle> triangles; // transformed geometry, original wrap UVs
};

class ShellAnchors {
public:
    [[nodiscard]] const std::string& model_path() const noexcept { return model_path_; }
    [[nodiscard]] const std::vector<ShellAnchor>& anchors() const noexcept { return anchors_; }
    // ASCII case-insensitive, by full mesh name (with any ALT suffix).
    [[nodiscard]] const ShellAnchor* find(std::string_view name) const noexcept;
    // Anchors shown for a faction variant: unsuffixed meshes plus `_ALT<variant>`.
    [[nodiscard]] std::vector<const ShellAnchor*> for_variant(std::uint32_t variant) const;

    void set_model_path(std::string path) { model_path_ = std::move(path); }
    void add(ShellAnchor anchor) { anchors_.push_back(std::move(anchor)); }

private:
    std::string model_path_;
    std::vector<ShellAnchor> anchors_;
};

struct ShellAnchorLoad {
    ShellAnchors shell;
    std::vector<core::Diagnostic> diagnostics;
};

// Anchors of every mesh with at least one vertex, in model mesh order.
[[nodiscard]] ShellAnchorLoad shell_anchors(const assets::Model& model);
// One validated model-space transform per bone of this exact model.
[[nodiscard]] ShellAnchorLoad shell_anchors(const assets::Model& model,
    std::span<const ShellTransform> model_transforms);
// Loads `data/art/models/<model_name>` with the ALO reader.
[[nodiscard]] core::Result<ShellAnchorLoad> load_shell_anchors(const vfs::Vfs& filesystem, std::string_view model_name);

// Mesh names that the catalogue has no component for (decorative meshes such
// as faceplates), and components the shell has no mesh for are both normal;
// this binds the ones that match.
struct BoundAnchor {
    const ShellAnchor* anchor{};
    const CommandBarComponent* component{}; // null for a decorative mesh
};
[[nodiscard]] std::vector<BoundAnchor> bind_components(const ShellAnchors& shell, const CommandBarCatalog& catalog);

namespace diagnostic_codes {
inline constexpr std::string_view shell_mesh_empty = "EAWR-UI-0310";     // mesh without vertices; no anchor
inline constexpr std::string_view shell_mesh_unbound = "EAWR-UI-0311";   // mesh not connected to a bone
inline constexpr std::string_view shell_bone_transform = "EAWR-UI-0312"; // retired: full affine transforms are supported
inline constexpr std::string_view shell_mesh_duplicate = "EAWR-UI-0313"; // same mesh name twice
} // namespace diagnostic_codes

} // namespace eawr::data::ui
