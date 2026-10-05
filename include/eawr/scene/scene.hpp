#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/core/result.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/sim/commands.hpp"
#include "eawr/sim/math/fixed.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/snapshot.hpp"
#include "eawr/sim/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Static scene builder (P1-11). Application/data side: it reads a loaded TED
// map and the effective XML catalog and produces a deterministic static scene
// of fixed transforms and stable asset IDs. The simulation never sees XML, VFS
// paths or engine resources; it is handed only what `Scene::instances()`
// returns. Nothing here depends on an engine.
namespace eawr::scene {

inline constexpr std::uint32_t contract_version = 1;

namespace diagnostic_codes {
inline constexpr std::string_view nonfinite = "EAWR-SCENE-0001";
inline constexpr std::string_view overflow = "EAWR-SCENE-0002";
inline constexpr std::string_view invalid_workers = "EAWR-SCENE-0003";
} // namespace diagnostic_codes

// Why a placement does not fully resolve. Every cause is counted; none is a
// reason to drop the placement from the scene or from a denominator.
enum class Cause : std::uint8_t {
    crc_absent,              // record carries no valid type CRC
    crc_missing,             // CRC matches no active catalog winner
    crc_collision,           // CRC matches several winners
    object_unresolvable,     // catalog could not resolve the effective object
    model_undeclared,        // no model tag anywhere in the derive chain
    model_not_in_vfs,        // declared model name probes to no file
    model_particle_system,   // model is a particle ALO, not a mesh model
    model_failed_to_load,    // model file exists but did not decode
    model_has_no_surface,    // model decodes with no visible submesh
    texture_unresolved,      // a declared texture parameter probes to no file
    shader_unsupported,      // a visible submesh selects an unsupported program
    effect_unresolved,       // an attached proxy effect probes to no file
    scale_invalid,           // Scale_Factor is malformed or not positive
    position_absent,         // record carries no position mini
    orientation_three_axis,  // historical evidence cause; sourced finite triples no longer emit it
    transform_nonfinite,     // a TED float is NaN or infinite
    transform_overflow,      // a converted value leaves the Q24 range
    orientation_absent,      // record carries no valid orientation mini
};

[[nodiscard]] std::string_view to_string(Cause cause) noexcept;
[[nodiscard]] std::span<const Cause> all_causes() noexcept;

// Whether a cause means the placement cannot be drawn at all, as opposed to
// being drawn with some of its surfaces or effects missing.
[[nodiscard]] bool blocks_drawing(Cause cause) noexcept;

struct Issue final {
    Cause cause{};
    // The unresolved name (a model, texture, shader or effect name, or the
    // CRC in hex). Never a host path.
    std::string detail;

    friend bool operator==(const Issue&, const Issue&) = default;
};

// Where a resolved value came from. For an XML value this is the tag, the
// object in the derive chain that supplied it and that object's source file;
// for a model-derived value the tag names the model field.
struct Provenance final {
    std::string tag;
    std::string source_object_id;
    std::string logical_path;
    std::uint64_t line{};
};

// The legacy-effect selectors the versioned material interface accepts. This
// table is the one shared copy on the data side; a structural test holds it
// equal to plan/inventories/godot-material-coverage.json.
struct LegacySelector final {
    std::string_view program;
    std::string_view technique;
    std::string_view pass;
    // The descriptor's render phase: a transparent-phase effect is drawn in
    // the transparent pass, everything else in the opaque pass.
    bool transparent{};
    // The selected adapter computes on stored 8-bit values as retail does
    // (docs/rendering.md colour policy), so colour constants such as the team
    // colour bind as stored values, not decoded to linear light.
    bool stored_values{};
};
[[nodiscard]] std::span<const LegacySelector> legacy_selectors() noexcept;
[[nodiscard]] const LegacySelector* find_legacy_selector(std::string_view program) noexcept;

// Static placements select ALT0 and hide girder meshes when a model has an
// intact ALT0 state; models without one keep their authored visible meshes.
[[nodiscard]] bool static_mesh_visible(const assets::Model& model, const assets::Mesh& mesh) noexcept;

// Capture-point XML marks objects whose idle clips select different visible
// bones for each owner. The bones whose first-frame visibility differs among
// those clips are ownership art; an uncaptured point draws only its invariant
// meshes (and follows the ALO bind visibility for the others).
[[nodiscard]] std::vector<std::uint8_t> capture_variant_bones(
    const assets::Model& model, std::span<const assets::Animation> idle_clips);
[[nodiscard]] bool uncaptured_mesh_visible(const assets::Model& model,
    const assets::Mesh& mesh, std::span<const std::uint8_t> variant_bones) noexcept;

// Team colour (P1-11 #32). A placement's owner is the signed 32-bit TED
// 1100/1113/1200 mini 2 (last valid occurrence). On an editor-authored map
// player index i is the i-th faction the catalog loads, in registry order
// (docs/asset-formats.md), and the team colour is that faction's <Color>
// (its first three 0..255 components). Status of a placement's colour:
//   faction_colour     resolved
//   owner_absent       no valid mini 2
//   owner_unmapped     the index names no loaded faction
//   colour_undeclared  the faction declares no <Color>
//   colour_invalid     <Color> is not three or four integers in 0..255
[[nodiscard]] std::span<const std::string_view> team_colour_statuses() noexcept;

// Whether an effect declares the engine-driven Colorization parameter (the
// P1-02 descriptor corpus), so a consumer binds the owner's team colour to it.
// Exact program names, ASCII case-insensitive like find_legacy_selector.
[[nodiscard]] bool colorizes(std::string_view program) noexcept;

// The Colorization binding for a team colour: each 0..255 sRGB-encoded
// channel decoded to linear light (the colorize adapters compute in linear
// light, so the colour then reads back as authored), alpha 1.
[[nodiscard]] assets::Vec4f colorization_binding(const std::array<std::uint8_t, 3>& rgb) noexcept;

// The Colorization binding for a team colour on `program`'s selected
// technique: each channel as the stored value 0..255 / 255 when that adapter
// computes on stored values, decoded to linear light otherwise; alpha 1.
[[nodiscard]] assets::Vec4f colorization_binding(
    std::string_view program, const std::array<std::uint8_t, 3>& rgb) noexcept;

// The factions in the order player indices name them: active faction
// definitions by registry then definition order, each id once.
[[nodiscard]] std::vector<std::string> faction_order(const data::Catalog& catalog);

struct TextureBinding final {
    std::string parameter;
    std::string declared;
    std::string resolved;  // logical path, empty when unresolved
};

struct Surface final {
    std::uint32_t mesh_index{};
    std::uint32_t submesh_index{};
    std::string mesh_name;
    std::string shader;
    bool supported{};
    std::string technique;
    std::string pass;
    std::vector<TextureBinding> textures;
};

struct AttachedEffect final {
    std::string proxy_name;
    std::string resolved;  // logical path, empty when unresolved
    std::uint32_t bone{};
    // True when the file was found only after removing a trailing
    // `_ALT<digits>` from the proxy name.
    bool alternate_suffix_removed{};
};

// A model as the scene sees it: resolved once per distinct logical path.
struct ModelFacts final {
    std::string logical_path;
    std::string sha256;
    std::vector<Surface> surfaces;
    std::vector<AttachedEffect> effects;
    std::string idle_animation;  // logical path, empty when none was found
    bool loaded{};
    bool particle_system{};
};

// Asset access is injected so the builder never opens a host path and tests
// can supply in-memory assets. `exists` probes a canonical logical path;
// `model` loads one, returning nullptr when the path does not decode, and
// `particle_system` says whether a failed load was a particle ALO. Callers own
// any caching.
struct AssetAccess final {
    std::function<bool(std::string_view logical_path)> exists;
    std::function<const assets::Model*(std::string_view logical_path)> model;
    std::function<std::string(std::string_view logical_path)> sha256;
    std::function<bool(std::string_view logical_path)> particle_system;
};

// Resolve each model proxy in ordinal order, including the documented ALT-number fallback.
// Shared by scene construction and runtime hardpoint attachments (BP-67).
[[nodiscard]] std::vector<AttachedEffect> model_proxy_effects(const AssetAccess& access, const assets::Model& model);

// WBP-17: runtime construction keeps authored alternate meshes available.
[[nodiscard]] std::vector<Surface> construction_surfaces(const AssetAccess& access, const std::string& path);
[[nodiscard]] std::optional<std::uint32_t> mesh_alternate(std::string_view name) noexcept;

// Fixed transform in the TED source basis (right-handed, X-right, Y-forward,
// Z-up), already in the simulation's Q24 representation.
struct Transform final {
    std::array<std::int64_t, 3> position_raw{};
    std::int64_t yaw_degrees_raw{};
    std::int64_t scale_raw{sim::math::Fixed::scale};
    sim::math::Mat3x4 matrix{};
};

struct Placement final {
    std::uint64_t scene_ordinal{};
    sim::EntityId entity_id{};
    std::string map_logical_path;
    std::uint32_t record_ordinal{};
    std::optional<std::uint32_t> serialized_object_id;
    std::optional<std::uint32_t> type_crc;

    std::string object_id;
    Provenance object_provenance;

    std::string model_declared;
    Provenance model_provenance;
    std::string model_path;
    sim::AssetId asset_id{};  // 0 when no model resolved

    std::int64_t scale_raw{sim::math::Fixed::scale};
    bool scale_declared{};
    Provenance scale_provenance;
    // LZ-01: presentation-only height of authored space props.
    std::int64_t layer_z_adjust_raw{};

    // Team colour (see team_colour_statuses). The owner is the TED record's
    // player index; the colour is its faction's <Color>, with provenance.
    std::optional<std::int32_t> owner_player;
    std::string owner_faction;
    enum class CaptureState : std::uint8_t { uncaptured, controlled };
    // A gameplay owner can later select the corresponding idle clip. TED's
    // editor owner_player remains separate from this runtime capture state.
    bool capture_point{};
    CaptureState capture_state{CaptureState::uncaptured};
    std::string capture_owner_faction;
    std::optional<std::array<std::uint8_t, 3>> team_colour;
    std::string team_colour_status{"owner_absent"};
    Provenance team_colour_provenance;
    std::string idle_animation;
    std::string idle_animation_status{"none"};
    Provenance idle_animation_provenance;

    std::vector<Surface> surfaces;
    std::vector<AttachedEffect> effects;
    std::optional<Transform> transform;
    std::vector<Issue> issues;  // sorted by cause then detail, unique

    [[nodiscard]] bool resolved() const noexcept { return issues.empty(); }
    [[nodiscard]] bool drawable() const noexcept;
};

struct SceneAsset final {
    sim::AssetId asset_id{};
    std::string logical_path;
    std::string sha256;
};

// Compact diagnostic grouping. A placement contributes once to each distinct
// issue (cause/detail); the model path stays empty when resolution stopped
// before model lookup. Shadow-volume issues remain visible here even when a
// different surface makes the placement drawable.
struct IssueGroup final {
    Cause cause{};
    std::string object_id;
    std::string model_path;
    std::string detail;
    std::uint64_t count{};

    friend bool operator==(const IssueGroup&, const IssueGroup&) = default;
};

struct Scene final {
    std::uint32_t contract_version{scene::contract_version};
    std::string map_logical_path;
    std::string map_sha256;
    std::string map_kind;
    std::vector<SceneAsset> assets;  // sorted by logical path; ids 1..N
    std::vector<Placement> placements;  // scene order
    std::string scene_sha256;

    [[nodiscard]] std::uint64_t count(Cause cause) const noexcept;
    [[nodiscard]] std::uint64_t resolved_count() const noexcept;
    [[nodiscard]] std::uint64_t drawable_count() const noexcept;
    // What the simulation receives: one fixed transform and stable asset ID
    // per drawable placement, in scene order.
    [[nodiscard]] std::vector<sim::RenderInstance> instances() const;
};

[[nodiscard]] std::vector<IssueGroup> issue_groups(const Scene& scene);

struct BuildInput final {
    const assets::Map* map{};
    std::string map_sha256;
    const data::Catalog* catalog{};
    AssetAccess access;
};

struct ExecutionStats final {
    std::size_t workers_requested{};
    std::size_t partitions_completed{};
    std::size_t placement_count{};
    std::vector<std::size_t> partition_placement_counts;
    std::size_t observed_worker_threads{};
};

struct BuildResult final {
    Scene scene;
    ExecutionStats execution;
};

// Exact conversion of an IEEE-754 binary32 value to Q24, rounding once to
// nearest with ties to even. NaN and infinity fail with `nonfinite`; a value
// outside the signed 64-bit raw range fails with `overflow`. The conversion
// is integer-only, so it does not depend on the host floating-point mode.
[[nodiscard]] core::Result<sim::math::Fixed> fixed_from_binary32(float value);

// The one conversion from a placed object's TED/XML orientation to its model
// transform (R-ROT-01, yaw-only form): the model first turns a fixed +90
// degrees about its own +Z, then yaws by `yaw` degrees counter-clockwise about
// source +Z, scales uniformly by `scale` and translates. The quarter turn is
// exact, so it adds no rounding. FoC keeps the same triple verbatim as the
// object's facing, so the simulation's heading is `yaw` itself (0 along +X,
// R-ROT-04); a model whose nose is on model -Y is drawn heading that way.
[[nodiscard]] core::Result<sim::math::Mat3x4> placement_transform(
    sim::math::Fixed x, sim::math::Fixed y, sim::math::Fixed z,
    sim::math::Fixed yaw_degrees, sim::math::Fixed scale);

// The same with a roll of `roll_degrees` about the object's forward axis between
// the quarter turn and the yaw (R-ROT-01 with pitch zero: Rz(yaw) Rx(roll)
// Rz(+90)); a live ship banking in a turn (#351). A zero roll is exactly the
// yaw-only form.
[[nodiscard]] core::Result<sim::math::Mat3x4> placement_transform(
    sim::math::Fixed x, sim::math::Fixed y, sim::math::Fixed z,
    sim::math::Fixed yaw_degrees, sim::math::Fixed roll_degrees, sim::math::Fixed scale);

// The full R-ROT-01 form, Rz(yaw) Ry(pitch) Rx(roll) Rz(+90), for an object whose facing
// triple has all three angles, such as a tumbling breakoff prop (#391). A zero pitch is
// exactly the roll form above.
[[nodiscard]] core::Result<sim::math::Mat3x4> placement_transform(
    sim::math::Fixed x, sim::math::Fixed y, sim::math::Fixed z, sim::math::Fixed yaw_degrees,
    sim::math::Fixed pitch_degrees, sim::math::Fixed roll_degrees, sim::math::Fixed scale);

[[nodiscard]] Scene build(const BuildInput& input);
[[nodiscard]] core::Result<BuildResult> build(const BuildInput& input,
                                               const sim::PartitionExecutor& executor);

// Memoising AssetAccess over a mounted VFS, for callers that build many scenes
// against one mount. Lookups are keyed by canonical logical path in ordered
// maps; nothing about the result depends on lookup order.
class VfsAssetCache final {
public:
    explicit VfsAssetCache(const vfs::Vfs& filesystem);
    VfsAssetCache(const VfsAssetCache&) = delete;
    VfsAssetCache& operator=(const VfsAssetCache&) = delete;

    [[nodiscard]] AssetAccess access();
    [[nodiscard]] bool exists(std::string_view logical_path);
    [[nodiscard]] const assets::Model* model(std::string_view logical_path);
    [[nodiscard]] std::string sha256(std::string_view logical_path);
    [[nodiscard]] bool particle_system(std::string_view logical_path);

private:
    struct Loaded final {
        std::optional<assets::Model> model;
        bool particle_system{};
    };
    const vfs::Vfs* filesystem_;
    std::map<std::string, bool, std::less<>> exists_;
    std::map<std::string, Loaded, std::less<>> models_;
    std::map<std::string, std::string, std::less<>> hashes_;
};

// Canonical text the scene hash is computed over. Exposed so a test can show
// what the hash covers and that it names no host path.
[[nodiscard]] std::string canonical_text(const Scene& scene);

} // namespace eawr::scene
