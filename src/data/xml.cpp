#include "xml_internal.hpp"
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace eawr::data {

Catalog::Catalog() = default;
Catalog::~Catalog() = default;
Catalog::Catalog(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
Catalog::Catalog(Catalog&&) noexcept = default;
Catalog& Catalog::operator=(Catalog&&) noexcept = default;
Catalog::Catalog(const Catalog&) = default;
Catalog& Catalog::operator=(const Catalog&) = default;

std::uint64_t Catalog::generation() const noexcept {
    if (overlay_) return overlay_->generation;
    return impl_ ? impl_->generation : 0;
}

const Definition& Catalog::definition_at(const std::size_t index) const noexcept {
    if (overlay_) {
        const auto replaced = overlay_->replaced.find(index);
        if (replaced != overlay_->replaced.end()) return replaced->second;
    }
    return impl_->definitions[index];
}
Profile Catalog::profile() const noexcept { return impl_ ? impl_->profile : Profile::eaw; }

const Definition* Catalog::find(const std::string_view object_id) const noexcept {
    if (!impl_) return nullptr;
    const auto found = impl_->winners.find(ascii_lower(object_id));
    return found == impl_->winners.end() ? nullptr : &definition_at(found->second);
}

std::vector<const Definition*> Catalog::find_all(const std::string_view object_id) const {
    std::vector<const Definition*> result;
    if (!impl_) return result;
    const auto found = impl_->by_id.find(ascii_lower(object_id));
    if (found == impl_->by_id.end()) return result;
    result.reserve(found->second.size());
    for (const auto index : found->second) result.push_back(&definition_at(index));
    return result;
}

const std::vector<Definition>& Catalog::definitions() const noexcept {
    static const std::vector<Definition> empty;
    return impl_ ? impl_->definitions : empty;
}

const std::vector<InventoryFile>& Catalog::physical_inventory() const noexcept {
    static const std::vector<InventoryFile> empty;
    return impl_ ? impl_->physical_inventory : empty;
}

const std::vector<RegistryFile>& Catalog::registry_files() const noexcept {
    static const std::vector<RegistryFile> empty;
    return impl_ ? impl_->registry_files : empty;
}

const std::vector<RegistryRoot>& Catalog::registry_roots() const noexcept {
    static const std::vector<RegistryRoot> empty;
    return impl_ ? impl_->registry_roots : empty;
}


core::Result<LoadResult> load_catalog(const vfs::Vfs& vfs, const Profile profile) {
    return load_catalog(vfs, profile, LoadOptions{});
}

core::Result<LoadResult> load_catalog(
    const vfs::Vfs& vfs,
    const Profile profile,
    const LoadOptions& options
) {
    struct RegistrySpec {
        Category category;
        std::string_view path;
        std::string_view root;
        std::string_view schema_type;
    };
    static constexpr RegistrySpec registries[] = {
        {Category::game_object, "data/xml/gameobjectfiles.xml", "Game_Object_Files", "GameObjectType"},
        {Category::hardpoint, "data/xml/hardpointdatafiles.xml", "Hard_Point_Files", "HardPoint"},
        {Category::faction, "data/xml/factionfiles.xml", "Faction_Files", "Faction"},
        {Category::campaign, "data/xml/campaignfiles.xml", "Campaign_Files", "Campaign"},
        {Category::sfx, "data/xml/sfxeventfiles.xml", "SFXEvent_Files", "SFXEvent"},
    };

    auto impl = std::make_shared<Catalog::Impl>();
    impl->profile = profile;
    impl->generation = next_generation.fetch_add(1, std::memory_order_relaxed);
    std::vector<core::Diagnostic> diagnostics;
    std::unordered_map<std::string, std::pair<bool, std::string>> parse_outcomes;

    // Inventory coverage is deliberately wider than the active XML registries:
    // retain every physical XML record mounted by the accepted VFS, including
    // manifests outside Data/XML and shadowed/disabled layer records.
    auto raw = vfs.enumerate_raw({}, ".xml");
    if (!raw) return core::Result<LoadResult>::failure(raw.error());
    for (auto& record : raw.value()) {
        impl->physical_inventory.push_back(InventoryFile{
            .record = std::move(record),
            .active_registry_file = false,
            .parsed = false,
            .outcome = std::nullopt,
        });
    }

    std::unordered_set<std::string> active_records;
    std::size_t global_registry_order = 0;
    for (const auto& registry : registries) {
        impl->registry_roots.push_back(RegistryRoot{
            .category = registry.category,
            .registry_path = std::string(registry.path),
            .outcome = "missing",
            .source = std::nullopt,
            .input_sha256 = std::nullopt,
        });
        auto& root_receipt = impl->registry_roots.back();
        auto bytes = vfs.open(registry.path);
        auto record = vfs.stat(registry.path);
        if (!bytes || !record) {
            root_receipt.outcome = unavailable_outcome(!bytes ? bytes.error() : record.error());
            if (record) root_receipt.source = SourceLocation{record.value().canonical_path,
                record.value().source_id, record.value().layer_id, 1, 1};
            diagnostics.push_back((!bytes ? bytes.error() : record.error()));
            diagnostics.back().code = std::string(diagnostic_codes::registry_invalid);
            continue;
        }
        root_receipt.source = SourceLocation{record.value().canonical_path,
            record.value().source_id, record.value().layer_id, 1, 1};
        root_receipt.input_sha256 = input_hash(bytes.value());
        active_records.insert(record_identity(record.value()));
        auto parsed = parse_document(bytes.value(), record.value());
        if (!parsed) {
            root_receipt.outcome = "parse_error";
            parse_outcomes[record.value().canonical_path] = {false, parsed.error().message};
            diagnostics.push_back(parsed.error());
            diagnostics.back().code = std::string(diagnostic_codes::registry_invalid);
            continue;
        }
        parse_outcomes[record.value().canonical_path] = {true, "parsed active registry XML"};
        if (!ascii_iequals(parsed.value().root.name(), registry.root)) {
            root_receipt.outcome = "parse_error";
            const auto source = SourceLocation{record.value().canonical_path, record.value().source_id,
                                               record.value().layer_id, 1, 1};
            diagnostics.push_back(diagnostic(
                diagnostic_codes::registry_invalid,
                "registry root is '" + std::string(parsed.value().root.name()) + "', expected '" +
                    std::string(registry.root) + "'",
                &source));
            continue;
        }
        root_receipt.outcome = "loaded";
        std::size_t local_order = 0;
        for (const auto file : parsed.value().root.children()) {
            if (file.type() != pugi::node_element || !ascii_iequals(file.name(), "File")) continue;
            const auto path = included_path(registry.path, file.child_value());
            RegistryFile registry_file{
                .category = registry.category,
                .registry_path = std::string(registry.path),
                .included_path = path,
                .include_order = local_order++,
                .loaded = false,
                .source = std::nullopt,
                .outcome = "missing",
                .input_sha256 = std::nullopt,
            };
            auto included_bytes = vfs.open(path);
            auto included_record = vfs.stat(path);
            if (!included_bytes || !included_record) {
                registry_file.outcome = unavailable_outcome(!included_bytes ? included_bytes.error() : included_record.error());
                if (included_record) registry_file.source = SourceLocation{included_record.value().canonical_path,
                    included_record.value().source_id, included_record.value().layer_id, 1, 1};
                auto error = !included_bytes ? included_bytes.error() : included_record.error();
                error.code = std::string(diagnostic_codes::registry_include_missing);
                error.message = "active registry include is unavailable: " + path;
                diagnostics.push_back(std::move(error));
                impl->registry_files.push_back(std::move(registry_file));
                ++global_registry_order;
                continue;
            }
            active_records.insert(record_identity(included_record.value()));
            registry_file.source = SourceLocation{included_record.value().canonical_path,
                included_record.value().source_id, included_record.value().layer_id, 1, 1};
            registry_file.input_sha256 = input_hash(included_bytes.value());
            auto included = parse_document(included_bytes.value(), included_record.value());
            if (!included) {
                registry_file.outcome = "parse_error";
                parse_outcomes[included_record.value().canonical_path] = {false, included.error().message};
                diagnostics.push_back(included.error());
                impl->registry_files.push_back(std::move(registry_file));
                ++global_registry_order;
                continue;
            }
            parse_outcomes[included_record.value().canonical_path] = {true, "parsed active registry include"};
            registry_file.loaded = true;
            registry_file.outcome = "loaded";
            std::size_t definition_order = 0;
            for (const auto definition : included.value().root.children()) {
                if (definition.type() != pugi::node_element) continue;
                auto category = registry.category;
                std::string schema_type(registry.schema_type);
                if (registry.category == Category::game_object) {
                    const auto concrete = canonical_object_type(definition.name());
                    if (concrete && is_ability_type(*concrete)) {
                        category = Category::ability;
                        schema_type = *concrete;
                    }
                }
                add_definition(*impl, definition, included_bytes.value(), included_record.value(), profile,
                               category, schema_type, global_registry_order, definition_order++, diagnostics, options);
                if (registry.category == Category::game_object) {
                    add_nested_abilities(*impl, definition, included_bytes.value(), included_record.value(), profile,
                                         global_registry_order, definition_order, diagnostics, options);
                }
            }
            impl->registry_files.push_back(std::move(registry_file));
            ++global_registry_order;
        }
    }

    auto effective = vfs.enumerate({}, ".xml");
    if (!effective) return core::Result<LoadResult>::failure(effective.error());
    std::unordered_map<std::string, std::string> effective_sources;
    for (const auto& record : effective.value()) {
        effective_sources.insert_or_assign(record.canonical_path, record.source_id);
        if (parse_outcomes.contains(record.canonical_path)) continue;
        auto bytes = vfs.open(record.canonical_path);
        if (!bytes) {
            parse_outcomes[record.canonical_path] = {false, bytes.error().message};
            diagnostics.push_back(bytes.error());
            continue;
        }
        auto parsed = parse_document(bytes.value(), record);
        if (!parsed) {
            parse_outcomes[record.canonical_path] = {false, parsed.error().message};
            diagnostics.push_back(parsed.error());
        } else {
            parse_outcomes[record.canonical_path] = {true, "parsed effective physical XML"};
        }
    }
    for (auto& file : impl->physical_inventory) {
        // Raw records share a canonical path; only the effective source has readable bytes.
        file.active_registry_file = active_records.contains(record_identity(file.record));
        const auto outcome = parse_outcomes.find(file.record.canonical_path);
        const auto effective_source = effective_sources.find(file.record.canonical_path);
        if (outcome != parse_outcomes.end() && effective_source != effective_sources.end() &&
            effective_source->second == file.record.source_id) {
            file.parsed = outcome->second.first;
            file.outcome = outcome->second.second;
        } else {
            file.outcome = "shadowed/inactive physical record retained; public VFS exposes metadata but not shadow bytes";
        }
    }

    index_winners(*impl, diagnostics);
    return core::Result<LoadResult>::success(LoadResult{Catalog(std::move(impl)), std::move(diagnostics)});
}



core::Result<sim::math::Fixed> fixed_value(const XmlNode& node) {
    return sim::math::Fixed::from_decimal(trim(node.raw_text));
}

std::string registry_include_path(const std::string_view registry_path, const std::string_view entry) {
    return included_path(registry_path, std::string(entry));
}

core::Result<XmlDocument> load_document(const vfs::Vfs& vfs, const std::string_view logical_path) {
    auto record = vfs.stat(logical_path);
    if (!record) return core::Result<XmlDocument>::failure(record.error());
    auto bytes = vfs.open(logical_path);
    if (!bytes) return core::Result<XmlDocument>::failure(bytes.error());
    auto parsed = parse_document(bytes.value(), record.value());
    if (!parsed) return core::Result<XmlDocument>::failure(parsed.error());
    const auto& found = record.value();
    auto root = build_node(parsed.value().root, bytes.value(), found);
    apply_document_overrides(root, found.canonical_path);
    return core::Result<XmlDocument>::success(XmlDocument{
        .root = std::move(root),
        .source = SourceLocation{found.canonical_path, found.source_id, found.layer_id, 1, 1},
        .input_sha256 = input_hash(bytes.value()),
    });
}

} // namespace eawr::data
