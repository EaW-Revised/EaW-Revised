#pragma once

#include "eawr/data/xml.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::data {

struct RecordLayer {
    std::string name;
    std::string kind;
    std::size_t priority{}; // zero is strongest
};

struct IndexedDefinition {
    const data::Definition* definition{};
    // SHA-256 of a length-framed representation of the parsed definition tree.
    std::string digest;
    // Position in VFS candidates for the definition's include path, zero is strongest.
    std::optional<std::size_t> candidate_rank;
    // Retains whichever store owns definition, even when this handle is copied out.
    std::shared_ptr<const data::Catalog> catalog_owner;
    std::shared_ptr<const data::ShadowedDefinitions> shadow_owner;
};

struct RecordEntry {
    std::string id;
    std::string type_name;
    data::Category category{data::Category::game_object};
    // Lowest priority first; unranked definitions follow all ranked definitions.
    std::vector<IndexedDefinition> definitions;
    const data::Definition* winner{};
    std::shared_ptr<const data::Catalog> winner_owner;
};

struct RecordTagColumn {
    data::SourceLocation definition_source;
    std::size_t registry_order{};
    std::size_t definition_order{};
    bool winner{};
    std::vector<data::XmlNode> values;
};

enum class RecordTagHow { own, overridden, inherited, merged, added, dropped };
struct RecordTagRow {
    std::string tag;
    std::vector<data::EffectiveValue> vanilla;
    std::vector<RecordTagColumn> authored;
    std::vector<data::EffectiveValue> effective;
    RecordTagHow how{RecordTagHow::own};
};

struct RecordIndex {
    std::vector<RecordEntry> records;
    std::vector<RecordLayer> layers;
    std::vector<data::RegistryFile> registry_dropped_vanilla_files;
    std::vector<core::Diagnostic> diagnostics;
    std::shared_ptr<const data::Catalog> vanilla_catalog;
    std::shared_ptr<const data::Catalog> profile_catalog;
    std::shared_ptr<const data::ShadowedDefinitions> shadowed_definitions;
    [[nodiscard]] const RecordEntry* find(std::string_view id, data::Category category) const;
    [[nodiscard]] core::Result<data::EffectiveObject> effective(const RecordEntry& record) const;
    [[nodiscard]] core::Result<std::vector<RecordTagRow>> tags(const RecordEntry& record) const;
};

// Catalogs must be the separately loaded FoC-only and profile catalogs. Shadowed
// definitions come from the loader's diagnostic pass for the profile catalog.
// The index retains copies of both catalogs and the shadow pass. Copies and moves
// share this storage. Copied or moved entries and indexed definitions retain the
// stores behind their pointers; a raw pointer alone does not retain its owner.
// Catalog copies share their implementation; resolve is only safe on one worker
// thread. The B2 service uses one worker thread and caches the index there.
[[nodiscard]] core::Result<RecordIndex> build_record_index(const data::Catalog& vanilla,
    const data::Catalog& profile, const data::ShadowedDefinitions& shadowed, const vfs::Vfs& profile_vfs,
    std::span<const RecordLayer> layers = {});

struct WholeFileCandidate {
    vfs::AssetRecord candidate;
    std::optional<data::XmlDocument> document;
    std::optional<core::Diagnostic> error;
    bool winner{};
};
struct WholeFileView {
    std::optional<data::XmlDocument> vanilla;
    std::vector<WholeFileCandidate> candidates; // VFS precedence order
    std::vector<RecordTagRow> tags;
};
// DocumentOverrides on the calling thread affect document loading here.
[[nodiscard]] core::Result<WholeFileView> compare_whole_file(const vfs::Vfs& vanilla,
    const vfs::Vfs& profile, std::string_view logical_path);

} // namespace eawr::data
