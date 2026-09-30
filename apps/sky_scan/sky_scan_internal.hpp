#pragma once
// sky_scan: a CPU-only ledger of the surfaces and shader families of every
// environment sky model the effective map corpus references (P1-06, #27).
//
// It measures; it does not render, qualify a shader, or decide how an
// original sky is drawn. Every declared primary/secondary sky reference is
// kept as a row whether or not it resolves. No material value other than a
// texture name is written, and no host path is written.

#include "eawr/assets/assets.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/presentation/space/space.hpp"
#include "eawr/vfs/vfs.hpp"

#include "sky_scan_descriptors.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>


namespace sky_scan_internal {

namespace assets = eawr::assets;
namespace space = eawr::presentation::space;

std::string lower(std::string);
bool ieq(std::string_view, std::string_view) noexcept;
std::string canonical_reference(std::string_view);
bool has_suffix(std::string_view);
std::string_view billboard_label(std::uint32_t);
bool proper_rigid(const std::array<float, 12>&);
bool identity_transform(const std::array<float, 12>&);

struct Arguments final {
    std::string profile;
    std::filesystem::path game_root;
    std::filesystem::path mod_root;
    std::filesystem::path report;
    std::filesystem::path inventory;
};

struct Resolved final {
    std::string logical_path;
    std::string layer_id;
    std::string origin;
    std::string source_id;
};

class Resolver final {
public:
    explicit Resolver(const eawr::vfs::Vfs& filesystem) : filesystem_(filesystem) {}

    std::optional<Resolved> stat(const std::string& logical_path) {
        const auto cached = cache_.find(logical_path);
        if (cached != cache_.end()) return cached->second;
        std::optional<Resolved> found;
        if (auto record = filesystem_.stat(logical_path)) {
            found = Resolved{record.value().canonical_path, record.value().layer_id,
                             std::string(eawr::vfs::to_string(record.value().origin)), record.value().source_id};
        }
        cache_.emplace(logical_path, found);
        return found;
    }

    std::optional<Resolved> resolve(const std::string_view name, const std::string_view root,
                                    const std::span<const std::string_view> suffixes) {
        const std::string canonical = canonical_reference(name);
        if (canonical.empty()) return std::nullopt;
        if (has_suffix(canonical)) {
            if (auto found = stat(std::string(root) + canonical)) return found;
        }
        std::string_view stem = canonical;
        for (const auto suffix : suffixes) {
            if (stem.size() > suffix.size() && stem.ends_with(suffix)) {
                stem = stem.substr(0, stem.size() - suffix.size());
                break;
            }
        }
        for (const auto suffix : suffixes) {
            if (auto found = stat(std::string(root) + std::string(stem) + std::string(suffix))) return found;
        }
        return std::nullopt;
    }

    std::optional<Resolved> model(const std::string_view name) {
        static constexpr std::array<std::string_view, 1> suffixes{".alo"};
        return resolve(name, "data/art/models/", suffixes);
    }

    std::optional<Resolved> texture(const std::string_view name) {
        static constexpr std::array<std::string_view, 2> suffixes{".tga", ".dds"};
        return resolve(name, "data/art/textures/", suffixes);
    }

private:
    const eawr::vfs::Vfs& filesystem_;
    std::map<std::string, std::optional<Resolved>> cache_;
};

// -- descriptor families -------------------------------------------------------

inline constexpr std::string_view not_in_bundle = "not_in_descriptor_bundle";

struct ShaderIdentity final {
    // The descriptor effect name when the stem matches one; otherwise the
    // lower-cased authored stem. Never a guessed family.
    std::string key;
    std::optional<std::string> effect;
    std::string family;
};

ShaderIdentity shader_identity(std::string_view shader);

// -- bone chain ------------------------------------------------------------------

// Billboard mode labels as named by the MIT-licensed alo-viewer at commit
// 9bb0053919cc5df8377610d4f91b11d956d6c2f4. A stored value outside 0..7 is
// reported as its integer with the label "unknown"; no meaning is guessed.
inline constexpr std::array<std::string_view, 8> billboard_labels{
    "disable", "parallel", "face", "zaxis_view", "zaxis_light", "zaxis_wind", "sunlight_glow", "sun",
};
inline constexpr std::string_view billboard_attribution =
    "alo-viewer (MIT) commit 9bb0053919cc5df8377610d4f91b11d956d6c2f4 billboard mode order";

struct Chain final {
    std::string label;
    std::optional<std::uint32_t> billboard_mode;
};

// Class of the mesh's attach chain, walked from its bone to the root.
// Precedence: invalid, billboard (the nearest billboard bone), hidden_bone,
// non_rigid, then identity (every transform exactly identity, or no bone)
// versus rigid_static.
Chain chain_class(const assets::Model& model, std::int32_t start);

struct TextureRow final {
    std::string parameter;
    std::string name;
    std::string status; // resolved | not_in_vfs | undeclared
    std::optional<Resolved> resolved;
};

struct SurfaceRow final {
    std::size_t mesh_index{};
    std::size_t submesh_index{};
    std::string mesh_name;
    bool mesh_visible{};
    std::int32_t bone{};
    Chain chain;
    bool skinned{};
    std::size_t skin_bones{};
    std::string vertex_format;
    std::string shader;
    ShaderIdentity identity;
    std::vector<std::string> parameters;
    std::vector<TextureRow> textures;
    std::uint64_t vertices{};
    std::uint64_t triangles{};
    std::optional<std::string> plan_status;
    std::vector<std::string> plan_causes;
};

struct Usage final {
    std::set<std::size_t> primary_maps;
    std::set<std::size_t> secondary_maps;
    std::set<std::size_t> land_maps;
    std::set<std::size_t> space_maps;
    std::set<std::size_t> space_primary_maps;
};

struct ModelRow final {
    Resolved source;
    std::set<std::string> declared_names;
    std::string status; // loaded | failed_to_load
    std::string sha256;
    std::string failure_code;
    std::string failure_message;
    std::size_t bones{};
    std::size_t meshes{};
    std::vector<SurfaceRow> surfaces;
    std::set<std::string> base_textures;
    Usage usage;
    std::optional<assets::Model> model;
};

struct SkyRow final {
    std::uint32_t environment_ordinal{};
    std::string field;
    unsigned field_id{};
    std::optional<std::string> reference_name;
    std::optional<std::string> model_name;
    std::string status;
    std::optional<std::string> model_path;
};

struct MapRow final {
    std::string logical_path;
    std::string layer_id;
    std::string sha256;
    std::string kind; // land | space | unknown
    std::string status; // loaded | failed_to_load | unreadable
    std::string failure_code;
    std::size_t environments{};
    std::vector<SkyRow> skies;
};

struct Tally final {
    std::uint64_t surfaces{};
    std::uint64_t visible_surfaces{};
    std::set<std::string> models;
    std::set<std::size_t> maps_primary;
    std::set<std::size_t> maps_secondary;
    std::set<std::size_t> land_maps;
    std::set<std::size_t> space_maps;
    std::set<std::string> families;
};

struct Totals final {
    std::map<std::string, Tally> shaders;
    std::map<std::string, Tally> families;
    std::map<std::string, std::uint64_t> vertex_formats;
    std::map<std::string, std::uint64_t> chain_classes;
    std::uint64_t surfaces{};
    std::uint64_t billboard_surfaces{};
    std::uint64_t skinned_surfaces{};
    std::uint64_t hidden_mesh_surfaces{};
    std::uint64_t vertex_colour_format_surfaces{};
    std::uint64_t textures_declared{};
    std::uint64_t textures_resolved{};
    std::uint64_t textures_not_in_vfs{};
    // Surfaces declaring more than one texture parameter (e.g. BaseTexture
    // plus CloudTexture), and loaded models that carry no mesh surface.
    std::uint64_t multi_texture_surfaces{};
    std::set<std::string> models_without_surfaces;
    std::set<std::string> land_multi_base_texture_models;
    std::set<std::size_t> land_multi_base_texture_maps;
};

struct GroupKey final {
    std::uint32_t ordinal{};
    std::string field;
    std::optional<std::string> reference;
    std::optional<std::string> model;
    std::string status;
    std::string kind;

    auto tie() const { return std::tie(ordinal, field, reference, model, status, kind); }
    bool operator<(const GroupKey& other) const { return tie() < other.tie(); }
};

struct Scan final {
    std::string profile;
    bool catalog_loaded{};
    std::vector<MapRow> maps;
    std::map<std::string, ModelRow> models;
};


std::string utf8(const std::filesystem::path&);
std::string lower(std::string);
bool ieq(std::string_view, std::string_view) noexcept;
std::filesystem::path data_root(const std::filesystem::path&);
std::optional<std::vector<std::pair<std::string, std::filesystem::path>>> roots(const Arguments&);
std::string json(std::string_view);
std::string sha256_hex(std::span<const std::byte>);
std::string canonical_reference(std::string_view);
bool has_suffix(std::string_view);
std::string_view billboard_label(std::uint32_t);
bool proper_rigid(const std::array<float, 12>&);
bool identity_transform(const std::array<float, 12>&);
std::string_view kind_name(assets::ParameterKind);
std::string_view reference_status(assets::EnvironmentReferenceStatus);
void describe_model(ModelRow&, Resolver&);
void attach_plan(ModelRow&);
void add_usage(Tally&, const ModelRow&);
bool colour_format_name(std::string_view);
Totals totals(const std::map<std::string, ModelRow>&);
std::string render(const Scan&, bool);
bool write_file(const std::filesystem::path&, const std::string&);
} // namespace sky_scan_internal
