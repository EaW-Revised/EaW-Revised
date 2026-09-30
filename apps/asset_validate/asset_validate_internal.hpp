#pragma once
#include "eawr/assets/assets.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/presentation/lighting/lighting.hpp"
#include "eawr/presentation/terrain/terrain.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>


namespace asset_validate_internal {

struct Arguments final {
    std::string profile;
    std::string view{"effective"};
    std::string inspect_model;
    std::string inspect_texture;
    std::string inspect_map;
    std::string only_format;
    std::filesystem::path game_root;
    std::filesystem::path mod_root;
    std::filesystem::path report;
    std::filesystem::path summary;
    std::filesystem::path failures;
};

struct Layer final {
    std::string id;
    std::filesystem::path root;
    eawr::vfs::ManifestResolution manifest;
};

struct AssetCounts final {
    std::uint64_t meshes{};
    std::uint64_t bones{};
    std::uint64_t materials{};
    std::uint64_t animations{};
    std::uint64_t vertices{};
    std::uint64_t indices{};
    std::uint64_t mips{};
    std::uint64_t samples{};
    std::uint64_t maps{};
    std::uint64_t terrain_samples{};
    std::uint64_t placements{};
    std::uint64_t semantic_maps{};
    // TED placement CRC resolution against the active XML catalog.  The four
    // CRC buckets always sum to `placements`; a record is never dropped.
    std::uint64_t placements_crc_absent{};
    std::uint64_t placements_crc_missing{};
    std::uint64_t placements_crc_collision{};
    std::uint64_t placements_crc_unique{};
    // Model-chain renderability of the uniquely typed placements.  These four
    // buckets also sum to `placements`.
    std::uint64_t placements_model_renderable{};
    std::uint64_t placements_model_unresolved{};
    std::uint64_t placements_model_undeclared{};
    std::uint64_t placements_model_unknown{};
    std::uint64_t texture_references{};
    std::uint64_t textures_resolved{};
    std::uint64_t texture_slots_untextured{};
    std::uint64_t sky_references{};
    std::uint64_t skies_resolved{};
    std::uint64_t skies_model_renderable{};
};

struct Aggregate final {
    std::uint64_t discovered{};
    std::uint64_t loaded{};
    std::uint64_t failed{};
    std::uint64_t notices{};
    AssetCounts counts;
};

// Distinct unresolved TED references, with the number of maps each one appears
// in.  It is a ledger of names, not a counter: it never feeds an aggregate.
struct UnresolvedLedger final {
    std::map<std::uint32_t, std::uint64_t> type_crcs;
    std::map<std::string, std::uint64_t> model_names;
    std::map<std::string, std::uint64_t> texture_names;
    std::map<std::string, std::uint64_t> sky_ids;
};

// Which TERRAIN-family surface effect and which skydome model the corpus's
// maps actually reference, counted by map. Like the unresolved ledger this is
// a record of names and never feeds an aggregate count.
struct FamilyLedger final {
    // Effect program -> maps that select it for at least one used slot.
    std::map<std::string, std::uint64_t> terrain_effects;
    // Effect program -> declared slots that select it, whether or not used.
    std::map<std::string, std::uint64_t> terrain_declared_slots;
    // Resolved skydome model logical path -> maps that reference it.
    std::map<std::string, std::uint64_t> skydome_models;
    // Skydome object id -> maps, for ids the catalog does resolve.
    std::map<std::string, std::uint64_t> skydome_objects;
    // Typed TED map notice -> maps that carry it, and its total occurrences.
    std::map<std::string, std::uint64_t> map_issue_maps;
    std::map<std::string, std::uint64_t> map_issue_occurrences;
    std::uint64_t maps_with_terrain{};
    std::uint64_t maps_with_water_record{};
    std::uint64_t space_maps{};
    std::uint64_t maps_with_sky_reference{};
};

struct AssetOutcome final {
    eawr::vfs::AssetRecord source;
    std::string format;
    std::string content_sha256;
    bool loaded{};
    AssetCounts counts;
    std::uint64_t notices{};
    std::optional<eawr::core::Diagnostic> diagnostic;
    std::string affected_feature;
    std::string follow_up;
};

std::string utf8(const std::filesystem::path& path);
std::string json(std::string_view value);
void write_environment_reference_ledger(std::ostream&, const eawr::assets::MapResolution&, bool);
void write_source_bounds_ledger(std::ostream&, const eawr::assets::Map&);
void write_environment_candidate_ledger(std::ostream&, const eawr::assets::Map&);
std::string format_of(const std::string&);
std::string sha256_hex(std::span<const std::byte>);
AssetCounts model_counts(const eawr::assets::Model&);
AssetCounts animation_counts(const eawr::assets::Animation&);
AssetCounts texture_counts(const eawr::assets::Texture&);
std::string map_semantics(const eawr::assets::Map&);
void record_families(const eawr::assets::Map&, const eawr::assets::ObjectTypeCatalog&,
                     const eawr::assets::AssetProbe&, FamilyLedger&);
AssetCounts map_counts(const eawr::assets::Map&, const eawr::assets::ObjectTypeCatalog&,
                       const eawr::assets::AssetProbe&, UnresolvedLedger&);
void add_counts(AssetCounts&, const AssetCounts&);
std::string affected_feature(std::string_view, const eawr::core::Diagnostic&);
std::string follow_up(std::string_view, const eawr::core::Diagnostic&);
std::map<std::string, Aggregate> aggregate(const std::vector<AssetOutcome>&);
std::string complete_report(const Arguments&, const std::vector<AssetOutcome>&,
                            const std::map<std::string, Aggregate>&, const UnresolvedLedger&,
                            const FamilyLedger&);
std::string legacy_summary(const Arguments&, const std::map<std::string, Aggregate>&);
std::string legacy_failures(const Arguments&, const std::vector<AssetOutcome>&);
bool write_file(const std::filesystem::path&, const std::string&);
} // namespace asset_validate_internal
