#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/scene/scene.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace eawr::presentation::particles {

// Evidence is aligned by proxy ordinal. A resolved path alone does not prove
// that the referenced ALO is a particle system or establish its CPU budget.
enum class EffectKind : std::uint8_t { unknown, particle, non_particle };
struct EffectEvidence final {
    EffectKind kind{EffectKind::unknown};
    std::size_t requested_capacity{};
};

enum class VisibilityEvidence : std::uint8_t { unknown, bind_pose };
struct MapEffectPlacementInput final {
    const assets::Model* model{};
    const scene::Placement* placement{};
    // Each supplied matrix is an independently validated, source Z-up model
    // frame for that bone. Missing entries remain unsupported; no frame is
    // inferred from an animation or from a proxy name.
    std::span<const std::optional<sim::math::Mat3x4>> bone_model_frames;
    std::span<const EffectEvidence> effects;
    VisibilityEvidence visibility{VisibilityEvidence::unknown};
    std::optional<std::uint32_t> selected_alt;
    std::optional<std::uint32_t> selected_lod;
    // Per proxy ordinal: 1 when the state of the owning hardpoint hides it.
    // Empty (the land path) hides nothing.
    std::span<const std::uint8_t> hardpoint_hidden_proxies{};
    // Per proxy ordinal: 1 when the unit's code shows its emitter type (an ion
    // stun's, BP-43, space-damage IS-09). The authored hidden flag is the same
    // flag the type switch clears, so such a proxy runs although authored
    // hidden. Empty shows nothing.
    std::span<const std::uint8_t> code_shown_proxies{};
};

enum class MapEffectStatus : std::uint8_t { admitted, hidden, unresolved, unsupported };
enum class MapEffectCause : std::uint8_t {
    none, missing_placement, missing_model, model_mismatch, missing_transform,
    missing_reference, reference_mismatch, unresolved_reference, unknown_effect_kind,
    non_particle_reference, unknown_visibility, hidden_proxy, hidden_bone,
    malformed_variant_tag, missing_alt_selection, missing_lod_selection,
    alt_mismatch, lod_mismatch, missing_bone, missing_frame,
    zero_capacity, capacity_exhausted, frame_overflow,
    // A hardpoint's damage emitter while the hardpoint's state hides it
    // (scene::hidden_hardpoint_proxies).
    hardpoint_state
};

struct MapEffectRecord final {
    std::string map_logical_path;
    std::uint64_t scene_ordinal{};
    std::uint32_t record_ordinal{};
    std::size_t proxy_ordinal{};
    std::string proxy_name;
    std::string model_logical_path;
    assets::Source model_source;
    scene::Provenance model_provenance;
    std::string effect_logical_path;
    bool alternate_suffix_removed{};
    MapEffectStatus status{MapEffectStatus::unsupported};
    MapEffectCause cause{MapEffectCause::none};
    std::string detail;
    std::optional<sim::math::Mat3x4> emitter_frame;
    std::int64_t scale_raw{};
    std::uint32_t seed{};
    std::size_t capacity{};
};

struct MapEffectPlan final {
    std::vector<MapEffectRecord> records;
    std::size_t allocated_capacity{};
    std::size_t aggregate_capacity{};
};

// Pure admission snapshot. Inputs are already resolved and in scene source
// order; neither assets nor runtime state are opened or updated here.
[[nodiscard]] MapEffectPlan plan_map_effects(
    std::span<const MapEffectPlacementInput> placements,
    std::uint64_t seed, std::size_t aggregate_capacity);

} // namespace eawr::presentation::particles
