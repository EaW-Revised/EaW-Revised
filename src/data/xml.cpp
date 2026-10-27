#include "eawr/core/load_profile.hpp"
#include "xml_internal.hpp"
#include "eawr/diagnostics/owner_census.hpp"
#include <atomic>
#include <optional>
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

namespace {
using Census = diagnostics::OwnerCensus;
void census_source(Census& output, const Census::Group group, const SourceLocation& source) noexcept {
    output.string(group, source.logical_path);
    output.string(group, source.source_id);
    output.string(group, source.layer_id);
}
void census_node(Census& output, const Census::Group group, const XmlNode& node, unsigned depth = 0) noexcept {
    if (!output.step(group)) return;
    if (depth == 64) { output.incomplete(group); return; }
    output.string(group, node.name);
    output.string(group, node.raw_text);
    census_source(output, group, node.source);
    if (node.schema_ref) output.string(group, *node.schema_ref);
    output.vector(group, node.attributes);
    for (const auto& attribute : node.attributes) {
        if (!output.step(group)) break;
        output.string(group, attribute.name);
        output.string(group, attribute.value);
        census_source(output, group, attribute.source);
        if (attribute.schema_ref) output.string(group, *attribute.schema_ref);
    }
    output.vector(group, node.children);
    for (const auto& child : node.children) {
        if (!output.step(group)) break;
        census_node(output, group, child, depth + 1);
    }
}
void census_definition(Census& output, const Definition& definition) noexcept {
    output.string(Census::xml_definitions, definition.id);
    output.string(Census::xml_definitions, definition.type_name);
    output.string(Census::xml_definitions, definition.schema_type_name);
    census_node(output, Census::xml_definitions, definition.root);
}
void census_resolved(Census& output, const std::unordered_map<std::string, EffectiveObject>& cache) noexcept {
    for (const auto& entry : cache) {
        const auto& [key, object] = entry;
        if (!output.step(Census::xml_resolved)) break;
        output.range(Census::xml_resolved, &entry, sizeof(entry));
        output.string(Census::xml_resolved, key);
        output.string(Census::xml_resolved, object.object_id);
        output.string(Census::xml_resolved, object.type_name);
        output.vector(Census::xml_resolved, object.chain);
        for (const auto& name : object.chain) {
            if (!output.step(Census::xml_resolved)) break;
            output.string(Census::xml_resolved, name);
        }
        output.vector(Census::xml_resolved, object.values);
        for (const auto& value : object.values) {
            if (!output.step(Census::xml_resolved)) break;
            census_node(output, Census::xml_resolved, value.value);
            output.string(Census::xml_resolved, value.source_object_id);
            if (value.displaced_value) census_node(output, Census::xml_resolved, *value.displaced_value);
            if (value.inherited_real) {
                census_node(output, Census::xml_resolved, value.inherited_real->value);
                output.string(Census::xml_resolved, value.inherited_real->source_object_id);
            }
        }
    }
}
}

void Catalog::owner_census(diagnostics::OwnerCensus& output) const {
    if (!impl_) return;
    if (output.range(Census::xml_definitions, impl_.get(), sizeof(Impl))) {
        output.vector(Census::xml_definitions, impl_->definitions);
        for (const auto& definition : impl_->definitions) {
            if (!output.step(Census::xml_definitions)) break;
            census_definition(output, definition);
        }
        for (const auto& entry : impl_->by_id) {
            const auto& [key, indexes] = entry;
            if (!output.step(Census::xml_definitions)) break;
            output.range(Census::xml_definitions, &entry, sizeof(entry));
            output.string(Census::xml_definitions, key);
            output.vector(Census::xml_definitions, indexes);
        }
        for (const auto& entry : impl_->winners) {
            const auto& [key, index] = entry;
            if (!output.step(Census::xml_definitions)) break;
            static_cast<void>(index);
            output.range(Census::xml_definitions, &entry, sizeof(entry));
            output.string(Census::xml_definitions, key);
        }
        std::lock_guard lock(impl_->cache_mutex);
        census_resolved(output, impl_->resolve_cache);
    }
    if (overlay_ && output.range(Census::xml_definitions, overlay_.get(), sizeof(Overlay))) {
        for (const auto& [index, definition] : overlay_->replaced) {
            if (!output.step(Census::xml_definitions)) break;
            static_cast<void>(index);
            output.range(Census::xml_definitions, &definition, sizeof(Definition));
            census_definition(output, definition);
        }
        std::lock_guard lock(overlay_->cache_mutex);
        census_resolved(output, overlay_->resolve_cache);
    }
}

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

const Definition* Catalog::find(const std::string_view object_id, const Category category) const noexcept {
    if (!impl_) return nullptr;
    const auto found = impl_->by_id.find(ascii_lower(object_id));
    if (found == impl_->by_id.end()) return nullptr;
    // by_id is already ordered by layer and registry precedence.
    for (const auto index : found->second) {
        const auto& definition = definition_at(index);
        if (definition.category == category) return &definition;
    }
    return nullptr;
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
    core::load_profile::Scope load_scope(core::load_profile::Phase::catalog);
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
    core::LoadNoteCollector notes;
    LoadHealth health;
    const auto fail_load = [&health](const core::Diagnostic& diagnostic) {
        if (!health.fatal) health.fatal = diagnostic;
    };
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
    std::optional<core::load_profile::Scope> registry_scope;
    registry_scope.emplace(core::load_profile::Phase::catalog_registry);
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
            ++health.registry_invalid;
            fail_load(diagnostics.back());
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
            ++health.registry_invalid;
            fail_load(diagnostics.back());
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
            ++health.registry_invalid;
            fail_load(diagnostics.back());
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
                const bool unreadable = registry_file.outcome == "read_error";
                diagnostics.push_back(std::move(error));
                if (unreadable) {
                    // The file exists and cannot be read: not an omitted include (R-10, R-13).
                    ++health.include_failed;
                    if (registry.category == Category::hardpoint) {
                        ++health.hardpoint_include_failed;
                        fail_load(diagnostics.back());
                    }
                } else {
                    ++health.include_missing;
                    // XML R-10: an initially missing include is omitted, as before.
                    const auto [line, column] = parsed.value().offsets.line_column(file.offset_debug());
                    (void)column;
                    core::LoadNote note;
                    note.code = core::load_note_codes::missing_include;
                    note.classification = core::LoadCategory::retail_accepts;
                    note.layer.mod_id = record.value().layer_id;
                    note.file = record.value().canonical_path;
                    note.line = line;
                    note.object = path;
                    note.tag = file.name();
                    note.authored_value = file.child_value();
                    note.action = core::LoadAction::accepted;
                    note.effective_value = "omitted";
                    note.retail = "XML R-10: initial missing includes are omitted";
                    note.diagnostic_code = diagnostics.back().code;
                    note.message = diagnostics.back().message;
                    notes.append(std::move(note));
                }
                impl->registry_files.push_back(std::move(registry_file));
                ++global_registry_order;
                continue;
            }
            active_records.insert(record_identity(included_record.value()));
            const core::load_profile::LayerScope layer_timer(included_record.value().layer_id,
                core::load_profile::LayerKind::registry, included_bytes.value().size());
            registry_file.source = SourceLocation{included_record.value().canonical_path,
                included_record.value().source_id, included_record.value().layer_id, 1, 1};
            registry_file.input_sha256 = input_hash(included_bytes.value());
            auto included = parse_document(included_bytes.value(), included_record.value());
            if (!included) {
                registry_file.outcome = "parse_error";
                parse_outcomes[included_record.value().canonical_path] = {false, included.error().message};
                diagnostics.push_back(included.error());
                ++health.include_failed;
                if (registry.category == Category::hardpoint) {
                    // C-06: R-10 and R-13 do not make an admitted hardpoint parse failure recoverable.
                    ++health.hardpoint_include_failed;
                    fail_load(diagnostics.back());
                }
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
                               category, schema_type, global_registry_order, definition_order++, diagnostics, options,
                               included.value().offsets);
                if (registry.category == Category::game_object) {
                    add_nested_abilities(*impl, definition, included_bytes.value(), included_record.value(), profile,
                                         global_registry_order, definition_order, diagnostics, options, included.value().offsets);
                }
            }
            impl->registry_files.push_back(std::move(registry_file));
            ++global_registry_order;
        }
    }

    registry_scope.reset();
    std::optional<core::load_profile::Scope> inventory_scope;
    inventory_scope.emplace(core::load_profile::Phase::catalog_inventory);
    auto effective = vfs.enumerate({}, ".xml");
    if (!effective) return core::Result<LoadResult>::failure(effective.error());
    std::unordered_map<std::string, std::string> effective_sources;
    for (const auto& record : effective.value()) {
        effective_sources.insert_or_assign(record.canonical_path, record.source_id);
        if (parse_outcomes.contains(record.canonical_path)) continue;
        if (!options.parse_unregistered_xml) {
            parse_outcomes[record.canonical_path] = {false, "not parsed: the inventory pass was not requested"};
            continue;
        }
        core::load_profile::LayerScope layer_timer(record.layer_id, core::load_profile::LayerKind::inventory);
        auto bytes = vfs.open(record.canonical_path);
        if (!bytes) {
            layer_timer.discard();
            parse_outcomes[record.canonical_path] = {false, bytes.error().message};
            diagnostics.push_back(bytes.error());
            continue;
        }
        auto parsed = parse_document(bytes.value(), record);
        layer_timer.add_bytes(bytes.value().size());
        if (!parsed) {
            parse_outcomes[record.canonical_path] = {false, parsed.error().message};
            diagnostics.push_back(parsed.error());
        } else {
            parse_outcomes[record.canonical_path] = {true, "parsed effective physical XML"};
        }
    }
    inventory_scope.reset();
    core::load_profile::Scope index_scope(core::load_profile::Phase::catalog_index);
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
    std::vector<const Definition*> member_definitions;
    member_definitions.reserve(impl->definitions.size());
    for (const auto& definition : impl->definitions) member_definitions.push_back(&definition);
    derive_member_flags(member_definitions, impl->member_flags, impl->member_flags_issue, impl->member_roster_notes);
    return core::Result<LoadResult>::success(LoadResult{Catalog(std::move(impl)), std::move(diagnostics), std::move(health), notes.snapshot()});
}

core::Result<LoadResult> load_catalog_strict(const vfs::Vfs& vfs, const Profile profile) {
    return load_catalog_strict(vfs, profile, LoadOptions{});
}

core::Result<LoadResult> load_catalog_strict(
    const vfs::Vfs& vfs,
    const Profile profile,
    const LoadOptions& options
) {
    auto loaded = load_catalog(vfs, profile, options);
    if (loaded && loaded.value().health.failed()) {
        auto error = *loaded.value().health.fatal;
        error.severity = core::Severity::error;
        return core::Result<LoadResult>::failure(std::move(error));
    }
    return loaded;
}

std::string describe_load_health(const LoadHealth& health) {
    std::string text;
    const auto add = [&text](std::string_view name, const std::size_t count) {
        if (count == 0) return;
        if (!text.empty()) text += ' ';
        text += name;
        text += '=';
        text += std::to_string(count);
    };
    add("registry_invalid", health.registry_invalid);
    add("include_failed", health.include_failed);
    add("hardpoint_include_failed", health.hardpoint_include_failed);
    add("include_missing", health.include_missing);
    return text;
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
    auto root = build_node(parsed.value().root, bytes.value(), found, parsed.value().offsets);
    apply_document_overrides(root, found.canonical_path);
    return core::Result<XmlDocument>::success(XmlDocument{
        .root = std::move(root),
        .source = SourceLocation{found.canonical_path, found.source_id, found.layer_id, 1, 1},
        .input_sha256 = input_hash(bytes.value()),
    });
}

} // namespace eawr::data
