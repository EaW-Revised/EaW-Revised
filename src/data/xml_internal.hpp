#pragma once
#include "eawr/data/xml.hpp"
#include "pugixml.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace eawr::data {

// XML 1.0 S: independent of the embedding process LC_CTYPE.
constexpr bool xml_space(const unsigned char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}


struct Catalog::Impl {
    Profile profile{Profile::eaw};
    std::uint64_t generation{0};
    std::vector<Definition> definitions;
    std::vector<InventoryFile> physical_inventory;
    std::vector<RegistryFile> registry_files;
    std::vector<RegistryRoot> registry_roots;
    std::unordered_map<std::string, std::vector<std::size_t>> by_id;
    std::unordered_map<std::string, std::size_t> winners;
    mutable std::mutex cache_mutex;
    mutable std::unordered_map<std::string, EffectiveObject> resolve_cache;
};

// with_overrides: the definitions it replaced, by index into the shared load's definitions, and
// its own resolve cache (the load's cache holds the unchanged objects).
struct Catalog::Overlay {
    std::uint64_t generation{0};
    std::unordered_map<std::size_t, Definition> replaced;
    mutable std::mutex cache_mutex;
    mutable std::unordered_map<std::string, EffectiveObject> resolve_cache;
};

extern std::atomic<std::uint64_t> next_generation;
// Original byte offsets are retained even when the parser accepts a compatibility form.
class SourceOffsets {
public:
    explicit SourceOffsets(std::span<const std::byte> bytes = {});
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> line_column(std::ptrdiff_t offset) const;
private:
    std::size_t size_{};
    std::vector<std::size_t> line_starts_{0};
};
struct ParsedDocument {
    pugi::xml_document document;
    pugi::xml_node root;
    SourceOffsets offsets;
};

std::string input_hash(std::span<const std::byte> bytes);
std::string unavailable_outcome(const core::Diagnostic& error);
std::optional<std::string> canonical_object_type(std::string_view element_name);
bool is_ability_type(std::string_view type_name);
core::Result<ParsedDocument> parse_document(std::span<const std::byte> bytes, const vfs::AssetRecord& record);
XmlNode build_node(pugi::xml_node source, std::span<const std::byte> bytes, const vfs::AssetRecord& record,
                  const SourceOffsets& offsets);
std::string included_path(std::string_view registry_path, std::string value);
// DocumentOverrides of this thread for the document at `canonical_path`, applied to its root.
void apply_document_overrides(XmlNode& root, std::string_view canonical_path);
std::string record_identity(const vfs::AssetRecord& record);
void add_definition(Catalog::Impl& catalog, pugi::xml_node source, std::span<const std::byte> bytes,
    const vfs::AssetRecord& record, Profile profile, Category category, std::string schema_type,
    std::size_t registry_order, std::size_t definition_order,
    std::vector<core::Diagnostic>& diagnostics, const LoadOptions& options, const SourceOffsets& offsets);
void add_nested_abilities(Catalog::Impl& catalog, pugi::xml_node source,
    std::span<const std::byte> bytes, const vfs::AssetRecord& record, Profile profile,
    std::size_t registry_order, std::size_t& definition_order,
    std::vector<core::Diagnostic>& diagnostics, const LoadOptions& options, const SourceOffsets& offsets,
    bool inside_abilities = false);
void index_winners(Catalog::Impl& catalog, std::vector<core::Diagnostic>& diagnostics);
std::string ascii_lower(std::string_view value);
bool ascii_iequals(std::string_view left, std::string_view right);
std::string trim(std::string_view value);
std::string join_chain(const std::vector<std::string>& chain);
core::Diagnostic diagnostic(std::string_view code, std::string message,
    const SourceLocation* source = nullptr, core::Severity severity = core::Severity::error);
std::string token_merge(std::string_view base, std::string_view derived);
const XmlNode* child_named(const XmlNode& node, std::string_view name);

} // namespace eawr::data
