#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/math/fixed.hpp"
#include "eawr/vfs/vfs.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::data {

enum class Profile : std::uint8_t { eaw, foc, remake };
enum class Category : std::uint8_t { game_object, hardpoint, ability, faction, campaign, sfx };
enum class SchemaStatus : std::uint8_t { known, deprecated, unknown };
enum class MergeMode : std::uint8_t { replace, merge, ignored };
enum class ValueProvenance : std::uint8_t { own, inherited, added, overridden, merged };

struct SourceLocation {
    std::string logical_path;
    std::string source_id;
    std::string layer_id;
    std::uint64_t line{1};
    std::uint64_t column{1};
};

struct XmlAttribute {
    std::string name;
    std::string value;
    SourceLocation source;
    SchemaStatus schema_status{SchemaStatus::unknown};
    std::optional<std::string> schema_ref;
};

struct XmlNode {
    std::string name;
    std::string raw_text;
    std::vector<XmlAttribute> attributes;
    std::vector<XmlNode> children;
    SourceLocation source;
    SchemaStatus schema_status{SchemaStatus::unknown};
    std::optional<std::string> schema_ref;
    MergeMode merge_mode{MergeMode::replace};
    bool multiple_allowed{false};
};

struct Definition {
    std::string id;
    std::string type_name;
    std::string schema_type_name;
    Category category{Category::game_object};
    XmlNode root;
    std::size_t registry_order{0};
    std::size_t definition_order{0};
    bool active{true};
    bool winner{false};
};

struct EffectiveValue {
    XmlNode value;
    ValueProvenance provenance{ValueProvenance::own};
    std::string source_object_id;
    std::optional<XmlNode> displaced_value;
};

struct EffectiveObject {
    std::string object_id;
    std::string type_name;
    Category category{Category::game_object};
    std::vector<std::string> chain; // derived to root base
    std::vector<EffectiveValue> values;
    std::uint64_t catalog_generation{0};

    [[nodiscard]] const EffectiveValue* value(std::string_view tag_name) const noexcept;
};

struct InventoryFile {
    vfs::AssetRecord record;
    bool active_registry_file{false};
    bool parsed{false};
    std::optional<std::string> outcome;
};

struct RegistryFile {
    Category category{Category::game_object};
    std::string registry_path;
    std::string included_path;
    std::size_t include_order{0};
    bool loaded{false};
    std::optional<SourceLocation> source;
    std::string outcome{"missing"};
    std::optional<std::string> input_sha256;
};

struct RegistryRoot {
    Category category{Category::game_object};
    std::string registry_path;
    std::string outcome{"missing"};
    std::optional<SourceLocation> source;
    std::optional<std::string> input_sha256;
};

struct MergeRule {
    std::string object_type;
    std::string tag_name;
    MergeMode mode{MergeMode::replace};
    bool multiple_allowed{false};
};

struct LoadOptions {
    std::vector<MergeRule> merge_rules;
};

// One changed value for with_overrides (the tag perturbation check, docs/tag-applied-check.md).
// `tag` is a tag of the object's root element, or a path below one (`Abilities/Some_Ability/Tag`).
// The object's own definition then authors exactly `values` for that tag, one element each, in
// order; an empty list removes the tag from its own definition (an inherited value then shows
// through). A value the object inherits is overridden by its own copy, as a variant's own tag is.
struct ValueOverride {
    std::string object_id;
    std::string tag;
    std::vector<std::string> values;
};

class Catalog final {
public:
    struct Impl;

    Catalog();
    ~Catalog();
    Catalog(Catalog&&) noexcept;
    Catalog& operator=(Catalog&&) noexcept;
    Catalog(const Catalog&);
    Catalog& operator=(const Catalog&);

    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] Profile profile() const noexcept;
    [[nodiscard]] const Definition* find(std::string_view object_id) const noexcept;
    [[nodiscard]] std::vector<const Definition*> find_all(std::string_view object_id) const;
    [[nodiscard]] core::Result<EffectiveObject> resolve(std::string_view object_id) const;
    [[nodiscard]] const std::vector<Definition>& definitions() const noexcept;
    [[nodiscard]] const std::vector<InventoryFile>& physical_inventory() const noexcept;
    [[nodiscard]] const std::vector<RegistryFile>& registry_files() const noexcept;
    [[nodiscard]] const std::vector<RegistryRoot>& registry_roots() const noexcept;

private:
    struct Overlay;
    explicit Catalog(std::shared_ptr<Impl> impl);
    [[nodiscard]] const Definition& definition_at(std::size_t index) const noexcept;
    std::shared_ptr<Impl> impl_;
    std::shared_ptr<const Overlay> overlay_; // with_overrides: replaced definitions over a shared load
    friend struct LoadResult;
    friend core::Result<Catalog> with_overrides(const Catalog&, std::span<const ValueOverride>);
    friend core::Result<struct LoadResult> load_catalog(const vfs::Vfs&, Profile);
    friend core::Result<struct LoadResult> load_catalog(const vfs::Vfs&, Profile, const LoadOptions&);
};

struct LoadResult {
    Catalog catalog;
    std::vector<core::Diagnostic> diagnostics;
};

[[nodiscard]] core::Result<LoadResult> load_catalog(const vfs::Vfs& vfs, Profile profile);
[[nodiscard]] core::Result<LoadResult> load_catalog(
    const vfs::Vfs& vfs, Profile profile, const LoadOptions& options);
[[nodiscard]] core::Result<sim::math::Fixed> fixed_value(const XmlNode& node);

// A catalog whose objects read as `catalog`'s, except for the overridden values. It shares the
// parsed load (only the changed definitions are copied) and never touches a file. find, find_all
// and resolve see the changes; definitions() and the file receipts stay the load's. Fails when an
// object is not in the catalog, or a path's container is neither authored nor inherited.
[[nodiscard]] core::Result<Catalog> with_overrides(const Catalog& catalog, std::span<const ValueOverride> overrides);

// One XML file outside the five catalog registries (for example
// gameconstants.xml or a TargetingPrioritySetFiles include), read through the
// VFS winner and parsed like a registry include. Nodes carry source locations
// but no schema classification. `input_sha256` is taken from the bytes the
// parser received, as for RegistryFile.
struct XmlDocument {
    XmlNode root;
    SourceLocation source;
    std::string input_sha256;
};

[[nodiscard]] core::Result<XmlDocument> load_document(const vfs::Vfs& vfs, std::string_view logical_path);

// The perturbation check's change to a document (docs/tag-applied-check.md). While a
// DocumentOverrides is alive, load_document on the same thread returns `logical_path` with
// `tag` (a path below the root element, `MaxRotationsSpace` or `A/B`) authoring exactly
// `values`, as ValueOverride does for an object; the file and other threads are untouched. A
// path below an absent element changes nothing.
struct DocumentOverride {
    std::string logical_path;
    std::string tag;
    std::vector<std::string> values;
};

class DocumentOverrides final {
public:
    explicit DocumentOverrides(std::vector<DocumentOverride> overrides);
    ~DocumentOverrides();
    DocumentOverrides(const DocumentOverrides&) = delete;
    DocumentOverrides& operator=(const DocumentOverrides&) = delete;

private:
    std::vector<DocumentOverride> overrides_;
    const std::vector<DocumentOverride>* previous_{};
};
// The logical path a registry's `<File>` entry names: the registry's own
// folder unless the entry already starts with `Data/`, as load_catalog uses.
[[nodiscard]] std::string registry_include_path(std::string_view registry_path, std::string_view entry);

[[nodiscard]] constexpr std::string_view to_string(Profile value) noexcept {
    switch (value) {
    case Profile::eaw: return "eaw";
    case Profile::foc: return "foc";
    case Profile::remake: return "remake";
    }
    return "eaw";
}

[[nodiscard]] constexpr std::string_view to_string(Category value) noexcept {
    switch (value) {
    case Category::game_object: return "game_objects";
    case Category::hardpoint: return "hardpoints";
    case Category::ability: return "abilities";
    case Category::faction: return "factions";
    case Category::campaign: return "campaigns";
    case Category::sfx: return "sfx";
    }
    return "game_objects";
}

namespace diagnostic_codes {
inline constexpr std::string_view malformed_xml = "EAWR-XML-0001";
inline constexpr std::string_view forbidden_doctype = "EAWR-XML-0002";
inline constexpr std::string_view registry_invalid = "EAWR-XML-0003";
inline constexpr std::string_view registry_include_missing = "EAWR-XML-0004";
inline constexpr std::string_view definition_missing_id = "EAWR-XML-0005";
inline constexpr std::string_view duplicate_id = "EAWR-XML-0006";
inline constexpr std::string_view object_not_found = "EAWR-XML-0007";
inline constexpr std::string_view variant_missing_base = "EAWR-XML-0008";
inline constexpr std::string_view variant_cycle = "EAWR-XML-0009";
inline constexpr std::string_view unknown_field = "EAWR-XML-0010";
inline constexpr std::string_view deprecated_field = "EAWR-XML-0011";
inline constexpr std::string_view physical_unreadable = "EAWR-XML-0012";
} // namespace diagnostic_codes

} // namespace eawr::data
