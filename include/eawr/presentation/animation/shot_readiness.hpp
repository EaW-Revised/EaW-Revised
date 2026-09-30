#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/core/result.hpp"
#include "eawr/presentation/animation/animation.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Shot-readiness evidence for one explicitly named model and optional clip.
// Presentation-only and read-only: nothing here selects, associates, approves
// or retargets a clip, and nothing enters simulation state. Every function
// fails closed with an EAWR-ANIMATION diagnostic rather than guessing.
namespace eawr::presentation::animation {

// Per-bone classification. "Draw-bound" means a visible mesh draws through the
// bone under the renderer's skinning rules: an active palette influence
// (weight != 0; -0.0 is inactive) or a rigid mesh binding (empty palette,
// mesh.bone >= 0).
struct BoneBindCensus final {
    std::string name;
    std::int32_t parent{-1};
    bool tracked{};           // a clip track drives this bone's local transform
    bool inherits_tracked{};  // untracked, but a tracked ancestor moves it
    std::uint32_t palette_listed{};    // visible submeshes whose skin palette lists it
    std::uint32_t palette_weighted{};  // of those, submeshes with an active influence on it
    std::uint32_t rigid_meshes{};      // visible meshes rigidly bound to it
    std::uint32_t hidden_bindings{};   // weighted/rigid bindings on invisible meshes
    std::uint32_t proxies{};
    std::uint32_t lights{};
    std::uint32_t dazzles{};
    [[nodiscard]] bool draw_bound() const noexcept { return palette_weighted != 0 || rigid_meshes != 0; }
};

struct TrackedBindCensus final {
    std::size_t bone_count{};
    std::size_t track_count{};
    std::vector<BoneBindCensus> bones;  // Model::bones order
    std::size_t draw_bound{};
    std::size_t draw_bound_tracked{};    // draw-bound and tracked
    std::size_t draw_bound_inherited{};  // draw-bound, untracked, tracked ancestor
    std::size_t draw_bound_static{};     // draw-bound and unaffected by the clip
    std::size_t tracked_not_draw_bound{};
    std::size_t palette_listed_unweighted{};  // listed somewhere, weighted nowhere
};

// Validates the pair exactly as Player::create does (the census is refused for
// any pair the player refuses), then applies the renderer's weight/palette
// rules to every submesh: an active local index outside the palette, a palette
// bone outside the model, a negative or non-finite weight, a rigid bone out of
// range, mesh.bone < -1, or a skinned mesh on a boneless model is refused.
[[nodiscard]] core::Result<TrackedBindCensus> tracked_bind_census(
    const assets::Model& model, const assets::Animation* animation = nullptr);

enum class SkinRoute : std::uint8_t { palette, rigid, unskinned };
[[nodiscard]] constexpr std::string_view to_string(const SkinRoute route) noexcept {
    return route == SkinRoute::palette ? "palette" : route == SkinRoute::rigid ? "rigid" : "unskinned";
}

struct PaletteSlot final {
    std::uint32_t local{};  // palette slot; 0 for the synthesized rigid slot
    std::uint32_t bone{};   // global model bone
    std::string name;
    bool tracked{};
    bool inherits_tracked{};
    std::uint64_t active_influences{};  // vertex influences with weight != 0 (rigid: vertex count)
    Matrix skin_asset{};                // the pose's skin matrix, ALO asset basis
};

struct SubmeshPalette final {
    SkinRoute route{SkinRoute::unskinned};
    std::string mesh;
    std::size_t submesh{};
    std::string shader;
    std::size_t vertex_count{};
    std::vector<PaletteSlot> slots;  // palette: one per skin_bones entry; rigid: one; unskinned: none
};

// The actual palette a draw of one exact (unique, case-sensitive, visible)
// mesh/submesh would upload for `pose`. The pose must have been sampled by
// `player` and `census` must come from the same model.
[[nodiscard]] core::Result<SubmeshPalette> submesh_palette(const Player& player, const Pose& pose,
    const assets::Model& model, const TrackedBindCensus& census, std::string_view mesh, std::size_t submesh);

struct AttachmentProbe final {
    std::string bone;
    std::size_t index{};
    bool tracked{};
    bool inherits_tracked{};
    AttachmentTransform reference;  // model space, render basis
    AttachmentTransform sampled;
    double translation_delta{};  // render-basis origin distance, sampled vs reference
    double basis_delta{};        // largest absolute 3x3 element difference
};

// Named attachment transform of one bone at two poses sampled by `player`
// (normally its bind output and a fixed clip time), with the documented
// asset-to-render conversion applied once by Player::attachment.
[[nodiscard]] core::Result<AttachmentProbe> attachment_probe(const Player& player, const Pose& reference,
    const Pose& sampled, const TrackedBindCensus& census, std::string_view bone);

// Bones whose model or skin matrix differs by more than `epsilon` in any
// element between two poses of the same player.
[[nodiscard]] core::Result<std::vector<std::size_t>> moved_bones(
    const Player& player, const Pose& reference, const Pose& sampled, float epsilon);

// SHA-256 (lowercase hex) of the pose's bone order, visibility and the exact
// IEEE-754 bits (little-endian) of its local, model and skin matrices.
[[nodiscard]] std::string pose_digest(const Pose& pose);
// SHA-256 of the route, mesh/submesh, slot mapping, active counts and the exact
// skin-matrix bits of every slot.
[[nodiscard]] std::string palette_digest(const SubmeshPalette& palette);

} // namespace eawr::presentation::animation
