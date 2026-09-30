#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/data/xml.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace eawr::assets {

// TED source space is right-handed, X-right/Y-forward/Z-up.  These values are
// deliberately not converted at this asset boundary.
using SourceVec3 = Vec3f;

enum class MapKind : std::uint8_t { land = 1, space = 2 };
// `absent` means mini 5 is missing or malformed; `nonfinite` means a stored
// component is NaN or infinite.  Neither is ever presented as an identity or
// yaw-only rotation.
enum class OrientationStatus : std::uint8_t {
    yaw_only, unsupported_three_axis_order, absent, nonfinite,
};
enum class TypeResolution : std::uint8_t { missing, unique, collision };

struct RawField final {
    std::uint32_t id{};
    std::uint64_t byte_offset{};
    std::vector<std::byte> bytes;
};

struct RawChunk final {
    std::uint32_t id{};
    bool group{};
    std::uint64_t header_offset{};
    std::vector<std::byte> payload{};
    std::vector<RawChunk> children{};
};

struct TerrainSample final {
    std::int16_t height_sample{};
    std::uint8_t material_slot{};
    std::uint8_t vertex_intensity{};
};

// A named texture reference that keeps the mini id it was declared under.
struct TextureReference final {
    std::uint8_t field_id{};
    std::string logical_name;
};

// A 1/259 source volume: two adjacent vector3 minis with ids k and k+1 whose
// components satisfy minimum <= maximum, kept exactly as stored.  No
// recentering; the meaning of each volume id is not pinned.  Every 1/259 mini,
// paired or not, stays in Map::volume_fields.
struct SourceVolume final {
    std::uint8_t min_field_id{};
    std::uint8_t max_field_id{};
    std::uint64_t min_byte_offset{};
    std::uint64_t max_byte_offset{};
    SourceVec3 minimum;
    SourceVec3 maximum;
};

// The newer-header 0x10/0x11 binary32 pair.  The field ids are kept because
// which of the two is width is not a renderer invariant yet.
struct DeclaredExtents final {
    std::uint8_t first_field_id{0x10};
    float first{};
    std::uint8_t second_field_id{0x11};
    float second{};
    std::uint64_t first_byte_offset{};
    std::uint64_t second_byte_offset{};
};

struct TerrainMaterial final {
    std::vector<RawField> fields;
    std::optional<std::string> primary_texture;
    std::optional<std::string> secondary_texture;
};

struct Terrain final {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t cell_count{};
    std::uint32_t slot_count{};
    float cell_spacing{20.0F};
    float height_scale{25.0F / 512.0F};
    // Every 1/257/0 mini, including the grid minis 0, 1, 4 and 5.
    std::vector<RawField> header_fields;
    std::vector<TerrainSample> samples;
    std::vector<TerrainMaterial> materials;
    std::vector<std::byte> raw_plane;
};

// Water data only: the non-grid minis of 1/257/0, then every mini-stream
// leaf under 1/257/9 (wave records nest one group deep), each kept losslessly
// by mini id with the chunk path it came from.
struct WaterRecord final {
    std::string chunk_path;
    std::uint64_t byte_offset{};
    std::vector<RawField> fields;
};

struct EnvironmentDescriptor final {
    // Ordinal among sibling 1/256/4/6 records, including invalid records.
    std::uint32_t record_ordinal{};
    std::vector<RawField> fields;
    std::optional<std::string> name;
    std::optional<std::string> primary_sky;
    std::optional<std::string> secondary_sky;
    std::optional<std::string> cloud_texture;
    // Offset of the effective (last valid) mini for each decoded value.
    std::optional<std::uint64_t> primary_sky_offset;
    std::optional<std::uint64_t> secondary_sky_offset;
    std::optional<std::uint64_t> cloud_texture_offset;
};

struct PlacementKey final {
    Source map;
    std::uint32_t record_ordinal{};
};

struct ObjectTypeRef final {
    std::string logical_name;
    data::SourceLocation source;
    // Declared model chain, taken from the resolved effective object.  An
    // absent optional means the tag is not declared anywhere in the derive
    // chain; it is never defaulted to a guessed asset name.
    std::optional<std::string> model_name;
    std::optional<std::string> land_model_name;
    std::optional<std::string> space_model_name;
};

struct ObjectTypeCatalog final {
    std::vector<ObjectTypeRef> entries;
};

struct Placement final {
    // record_ordinal is the index of the 1100 record among its siblings, so an
    // 1100 whose shape is unresolved keeps its slot and its denominator row.
    PlacementKey key;
    std::uint64_t byte_offset{}; // 1100 header offset
    std::optional<std::uint32_t> serialized_object_id;
    std::optional<std::uint32_t> type_crc;
    TypeResolution type_resolution{TypeResolution::missing};
    std::vector<ObjectTypeRef> type_candidates;
    std::optional<SourceVec3> position;
    std::optional<SourceVec3> orientation_degrees; // roll X, pitch Y, yaw Z
    OrientationStatus orientation_status{OrientationStatus::absent};
    std::vector<RawField> fields;
};

// Typed map notice categories.  Every MapNotice is mirrored as a generic
// Notice in Map::notices, so notice totals stay one list.
enum class MapIssue : std::uint8_t {
    unknown_root_field,
    duplicate_root_field,
    unknown_chunk,
    optional_section_absent,
    preview_not_decoded,
    main_group_absent,
    kind_structure_mismatch,
    declared_extents_invalid,
    environment_stream_invalid,
    volume_stream_invalid,
    terrain_absent,
    terrain_legacy_plane,
    terrain_header_invalid,
    terrain_dimensions,
    terrain_cell_count,
    terrain_plane_size,
    terrain_material_stream_invalid,
    terrain_material_slot,
    passability_size,
    water_stream_invalid,
    water_texture_invalid,
    placement_record_shape,
    placement_stream_invalid,
    placement_crc_absent,
    placement_type_missing,
    placement_type_collision,
    orientation_absent,
    orientation_nonfinite,
    orientation_three_axis,
};

[[nodiscard]] std::string_view to_string(MapIssue issue) noexcept;

struct MapNotice final {
    MapIssue issue{};
    std::string chunk_path;   // slash-joined chunk ids; "root" for root minis
    std::uint64_t byte_offset{};
    std::uint64_t byte_size{};
    std::optional<std::uint32_t> record_ordinal;
    std::optional<std::uint8_t> field_id;
    std::string message;
};

struct Map final {
    Source source;
    std::uint32_t format_version{};
    std::optional<MapKind> kind;
    std::vector<RawField> root_fields;
    // Root 0x09, decoded from validated UTF-16LE to UTF-8.  Empty is valid;
    // absence (old headers) is valid too.  Not an asset path.
    std::optional<std::string> context_name;
    std::optional<DeclaredExtents> declared_extents;
    std::vector<RawChunk> chunks;
    std::vector<Notice> notices;
    std::vector<MapNotice> issues;
    std::vector<SourceVolume> volumes;
    std::vector<RawField> volume_fields;
    std::vector<EnvironmentDescriptor> environments;
    std::vector<WaterRecord> water_records;
    // 1/257/0 minis 0x1D and 0x1E.  They join the texture denominator.
    std::vector<TextureReference> water_textures;
    std::vector<std::byte> raw_passability;
    std::vector<Placement> placements;
    std::optional<Terrain> terrain;
    // True means every required semantic section for the declared map kind
    // decoded and the body agrees with the declared kind.  A structurally
    // retained, raw-only document is never complete.  A failed semantic view
    // leaves its optional empty and a MapNotice, never a failed load.
    bool semantic_complete{};
};

// Existence predicate over an already canonical logical VFS path.  The asset
// boundary stays free of a filesystem dependency; the caller supplies (and may
// memoise) the probe.
using AssetProbe = std::function<bool(std::string_view logical_path)>;

// Status of one declared reference.  `absent` means the document declares no
// such reference at all and is never conflated with a declared-but-missing one.
enum class ReferenceStatus : std::uint8_t { absent, unresolved, resolved };

// Which art root a declared file reference is probed against.  Authored names
// carry the source-art suffix (`.tga`) even where the shipped asset uses
// another (`.dds`), so every known suffix for the kind is tried against the
// name's stem as well as the literal name.
enum class ReferenceKind : std::uint8_t { model, texture };

enum class EnvironmentReferenceField : std::uint8_t { primary_sky, secondary_sky, cloud_texture };
enum class EnvironmentReferenceStatus : std::uint8_t {
    undeclared, missing_catalog_object, undeclared_model,
    unresolved_model, unresolved_texture, resolved,
};

// One row for each known reference field of each decoded environment, in
// environment/field order. Repeated names remain separate rows. The TED mini
// offset identifies the effective declaration; an absent field has no offset.
struct EnvironmentReferenceResolution final {
    std::uint32_t environment_ordinal{};
    EnvironmentReferenceField field{};
    std::uint8_t field_id{};
    std::optional<std::uint64_t> declaration_byte_offset;
    std::optional<std::string> reference_name;
    // Effective XML object declaration and its selected model, for sky rows.
    std::optional<data::SourceLocation> catalog_source;
    std::optional<std::string> model_name;
    EnvironmentReferenceStatus status{EnvironmentReferenceStatus::undeclared};
};

// Per-map resolution accounting.  Every persisted-object record and every
// declared reference stays in its denominator; an unresolved row is reported,
// never dropped.
struct MapResolution final {
    std::vector<EnvironmentReferenceResolution> environment_references;
    std::uint64_t placements{};
    std::uint64_t crc_absent{};     // record carries no valid type CRC mini
    std::uint64_t crc_missing{};    // CRC present, no catalog winner matches
    std::uint64_t crc_collision{};  // CRC present, several winners match
    std::uint64_t crc_unique{};     // CRC present, exactly one winner matches
    std::uint64_t model_chain_renderable{};
    std::uint64_t model_chain_unresolved{};  // uniquely typed, model chain does not resolve
    std::uint64_t model_chain_undeclared{};  // uniquely typed, no model tag in the chain
    std::uint64_t model_chain_unknown{};     // type did not resolve uniquely
    std::uint64_t texture_references{};
    std::uint64_t textures_resolved{};
    std::uint64_t texture_slots_untextured{};
    // TED environment skydome fields name an XML skydome object by id, not an
    // art file, so they resolve against the catalog and only then against the
    // resolved object's own model chain.
    std::uint64_t sky_references{};
    std::uint64_t skies_resolved{};
    std::uint64_t skies_model_renderable{};
    // The distinct unresolved references this map is responsible for, so a
    // report can name every one instead of only counting it.  A type CRC has
    // no recoverable name once it misses the catalog, so the hash itself is
    // the identity; nothing is reverse-engineered into a guessed name.
    std::vector<std::uint32_t> unresolved_type_crcs;
    std::vector<std::string> unresolved_model_names;
    std::vector<std::string> unresolved_texture_names;
    std::vector<std::string> unresolved_sky_ids;
};

[[nodiscard]] ReferenceStatus resolve_reference(
    const std::optional<std::string>& name, ReferenceKind kind, const AssetProbe& probe);
[[nodiscard]] const ObjectTypeRef* find_object_type(
    const ObjectTypeCatalog& catalog, std::string_view logical_name);
[[nodiscard]] MapResolution resolve_map(
    const Map& map, const ObjectTypeCatalog& catalog, const AssetProbe& probe);

[[nodiscard]] std::uint32_t object_type_crc(std::string_view logical_name) noexcept;
[[nodiscard]] ObjectTypeCatalog object_type_catalog(const data::Catalog& catalog);
[[nodiscard]] core::Result<Map> load_map(
    std::span<const std::byte> bytes, Source source, const ObjectTypeCatalog& catalog = {});
[[nodiscard]] core::Result<Map> load_map(
    const vfs::Vfs& filesystem, std::string_view logical_path,
    const ObjectTypeCatalog& catalog = {});

} // namespace eawr::assets
