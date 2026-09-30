#include "xml_internal.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/core/sha256.hpp"

#include "pugixml.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace eawr::data {
struct PackedSchemaRow {
    std::uint32_t object_type;
    std::uint32_t tag_path;
    std::uint32_t tag_name;
    std::uint32_t schema_ref;
    std::uint8_t profile;
    std::uint8_t node_kind;
    std::uint8_t status;
};

#include "xml_schema_contract.inc"

std::string_view schema_string(const std::uint32_t locator) {
    return schema_string_blobs[locator >> schema_string_chunk_shift] +
           (locator & schema_string_chunk_offset_mask);
}

struct SchemaValue {
    SchemaStatus status{SchemaStatus::unknown};
    std::string_view schema_ref;
};

std::string ascii_lower(std::string_view value) {
    std::string result(value);
    for (char& ch : result) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    }
    return result;
}

bool ascii_iequals(const std::string_view left, const std::string_view right) {
    return ascii_lower(left) == ascii_lower(right);
}

std::string alnum_key(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            result.push_back(static_cast<char>((ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch)));
        }
    }
    return result;
}

std::string trim(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size() && xml_space(static_cast<unsigned char>(value[first]))) ++first;
    std::size_t last = value.size();
    while (last > first && xml_space(static_cast<unsigned char>(value[last - 1]))) --last;
    return std::string(value.substr(first, last - first));
}

std::string input_hash(const std::span<const std::byte> bytes) {
    return core::sha256_hex({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

std::string unavailable_outcome(const core::Diagnostic& error) {
    return error.code == vfs::diagnostic_codes::not_found ? "missing" : "read_error";
}

std::string join_chain(const std::vector<std::string>& chain) {
    std::ostringstream out;
    for (std::size_t i = 0; i < chain.size(); ++i) {
        if (i) out << " -> ";
        out << chain[i];
    }
    return out.str();
}

core::Diagnostic diagnostic(
    const std::string_view code,
    std::string message,
    const SourceLocation* source,
    const core::Severity severity
) {
    core::Diagnostic result{
        .code = std::string(code),
        .severity = severity,
        .message = std::move(message),
        .logical_path = std::nullopt,
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = std::nullopt,
    };
    if (source != nullptr) {
        result.logical_path = source->logical_path;
        result.line = source->line;
        result.column = source->column;
        result.source_id = source->source_id;
    }
    return result;
}

std::string schema_key(
    const Profile profile,
    const std::string_view object_type,
    const std::string_view tag_path,
    const std::string_view node_kind,
    const std::string_view name
) {
    std::string result(to_string(profile));
    result.push_back('\x1f');
    result += ascii_lower(object_type);
    result.push_back('\x1f');
    result += ascii_lower(tag_path);
    result.push_back('\x1f');
    result += node_kind;
    result.push_back('\x1f');
    result += ascii_lower(name);
    return result;
}

const std::unordered_map<std::string, SchemaValue>& schema_lookup() {
    static const auto lookup = [] {
        std::unordered_map<std::string, SchemaValue> result;
        result.reserve(std::size(schema_rows));
        for (const auto& row : schema_rows) {
            const auto status = row.status == 1U ? SchemaStatus::known :
                                row.status == 2U ? SchemaStatus::deprecated : SchemaStatus::unknown;
            const auto profile = row.profile == 0U ? Profile::eaw : row.profile == 1U ? Profile::foc : Profile::remake;
            const auto node_kind = row.node_kind == 0U ? std::string_view{"element"} : std::string_view{"attribute"};
            result.insert_or_assign(
                schema_key(profile, schema_string(row.object_type), schema_string(row.tag_path),
                           node_kind, schema_string(row.tag_name)),
                SchemaValue{status, schema_string(row.schema_ref)});
        }
        return result;
    }();
    return lookup;
}

SchemaValue schema_value(
    const Profile profile,
    const std::string_view object_type,
    const std::string_view tag_path,
    const std::string_view node_kind,
    const std::string_view name
) {
    const auto found = schema_lookup().find(schema_key(profile, object_type, tag_path, node_kind, name));
    return found == schema_lookup().end() ? SchemaValue{} : found->second;
}

const std::unordered_map<std::string, std::string>& normalized_object_types() {
    static const auto values = [] {
        std::unordered_map<std::string, std::string> result;
        for (const auto locator : schema_object_type_locators) {
            const auto name = schema_string(locator);
            result.try_emplace(alnum_key(name), name);
        }
        return result;
    }();
    return values;
}

std::optional<std::string> canonical_object_type(const std::string_view element_name) {
    const auto found = normalized_object_types().find(alnum_key(element_name));
    if (found == normalized_object_types().end()) return std::nullopt;
    return found->second;
}

bool is_ability_type(const std::string_view type_name) {
    const auto key = alnum_key(type_name);
    return key.size() >= 7U && key.ends_with("ability");
}

std::pair<std::uint64_t, std::uint64_t> line_column(
    const std::span<const std::byte> bytes,
    const std::ptrdiff_t raw_offset
) {
    if (raw_offset <= 0) return {1, 1};
    const auto offset = std::min<std::size_t>(static_cast<std::size_t>(raw_offset), bytes.size());
    std::uint64_t line = 1;
    std::uint64_t column = 1;
    for (std::size_t i = 0; i < offset; ++i) {
        const auto ch = std::to_integer<unsigned char>(bytes[i]);
        if (ch == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return {line, column};
}

std::vector<std::ptrdiff_t> attribute_offsets(
    const std::span<const std::byte> bytes,
    const std::ptrdiff_t element_offset
) {
    std::vector<std::ptrdiff_t> result;
    if (element_offset < 0 || static_cast<std::size_t>(element_offset) >= bytes.size()) return result;
    std::size_t cursor = static_cast<std::size_t>(element_offset);
    const auto character = [&](const std::size_t index) {
        return static_cast<char>(std::to_integer<unsigned char>(bytes[index]));
    };
    if (character(cursor) != '<') {
        if (cursor == 0 || character(cursor - 1) != '<') return result;
        --cursor;
    }
    ++cursor;
    while (cursor < bytes.size() && !xml_space(static_cast<unsigned char>(character(cursor))) &&
           character(cursor) != '>' && character(cursor) != '/') ++cursor;
    while (cursor < bytes.size()) {
        while (cursor < bytes.size() && xml_space(static_cast<unsigned char>(character(cursor)))) ++cursor;
        if (cursor >= bytes.size() || character(cursor) == '>' || character(cursor) == '/') break;
        const auto name_offset = cursor;
        while (cursor < bytes.size() && !xml_space(static_cast<unsigned char>(character(cursor))) &&
               character(cursor) != '=' && character(cursor) != '>' && character(cursor) != '/') ++cursor;
        result.push_back(static_cast<std::ptrdiff_t>(name_offset));
        while (cursor < bytes.size() && xml_space(static_cast<unsigned char>(character(cursor)))) ++cursor;
        if (cursor >= bytes.size() || character(cursor) != '=') break;
        ++cursor;
        while (cursor < bytes.size() && xml_space(static_cast<unsigned char>(character(cursor)))) ++cursor;
        if (cursor >= bytes.size() || (character(cursor) != '\'' && character(cursor) != '"')) break;
        const auto quote = character(cursor++);
        while (cursor < bytes.size() && character(cursor) != quote) ++cursor;
        if (cursor < bytes.size()) ++cursor;
    }
    return result;
}

struct NumericRootCompatibility {
    std::vector<std::byte> bytes;
    std::string authored_name;
};

std::optional<NumericRootCompatibility> numeric_root_compatibility(
    const std::span<const std::byte> bytes
) {
    const std::string source(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::size_t cursor = source.starts_with("\xef\xbb\xbf") ? 3U : 0U;
    const auto skip_space = [&] {
        while (cursor < source.size() &&
               xml_space(static_cast<unsigned char>(source[cursor]))) ++cursor;
    };
    while (true) {
        skip_space();
        if (source.compare(cursor, 2, "<?") == 0) {
            const auto end = source.find("?>", cursor + 2);
            if (end == std::string::npos) return std::nullopt;
            const auto instruction = ascii_lower(std::string_view(source).substr(cursor, end + 2 - cursor));
            if (instruction.starts_with("<?xml")) {
                const auto encoding = instruction.find("encoding");
                if (encoding != std::string::npos) {
                    const auto equals = instruction.find('=', encoding + 8);
                    if (equals == std::string::npos) return std::nullopt;
                    const auto quote = instruction.find_first_of("\"'", equals + 1);
                    if (quote == std::string::npos) return std::nullopt;
                    const auto close = instruction.find(instruction[quote], quote + 1);
                    if (close == std::string::npos || instruction.substr(quote + 1, close - quote - 1) != "utf-8") {
                        return std::nullopt;
                    }
                }
            }
            cursor = end + 2;
        } else if (source.compare(cursor, 4, "<!--") == 0) {
            const auto end = source.find("-->", cursor + 4);
            if (end == std::string::npos) return std::nullopt;
            cursor = end + 3;
        } else {
            break;
        }
    }
    skip_space();
    if (cursor + 2 >= source.size() || source[cursor] != '<' ||
        source[cursor + 1] < '0' || source[cursor + 1] > '9') return std::nullopt;
    const auto name_start = cursor + 1;
    auto name_end = name_start;
    const auto name_character = [](const unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
               (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' || ch == ':';
    };
    while (name_end < source.size() && name_character(static_cast<unsigned char>(source[name_end]))) ++name_end;
    if (name_end == name_start || name_end >= source.size()) return std::nullopt;
    const auto delimiter = source[name_end];
    if (!xml_space(static_cast<unsigned char>(delimiter)) && delimiter != '>' && delimiter != '/') {
        return std::nullopt;
    }
    const auto name = source.substr(name_start, name_end - name_start);
    const auto closing = "</" + name + ">";
    const auto closing_start = source.rfind(closing);
    if (closing_start == std::string::npos || closing_start <= name_end) return std::nullopt;
    for (auto trailing = closing_start + closing.size(); trailing < source.size(); ++trailing) {
        if (!xml_space(static_cast<unsigned char>(source[trailing]))) return std::nullopt;
    }
    NumericRootCompatibility result{std::vector<std::byte>(bytes.begin(), bytes.end()), name};
    result.bytes[name_start] = std::byte{'_'};
    result.bytes[closing_start + 2] = std::byte{'_'};
    return result;
}

core::Result<ParsedDocument> parse_document(
    const std::span<const std::byte> bytes,
    const vfs::AssetRecord& record
) {
    ParsedDocument parsed;
    auto result = parsed.document.load_buffer(
        bytes.data(), bytes.size(), pugi::parse_full | pugi::parse_ws_pcdata, pugi::encoding_auto);
    std::optional<std::string> numeric_root_name;
    if (!result && (result.status == pugi::status_bad_start_element ||
                    result.status == pugi::status_unrecognized_tag)) {
        if (auto compatible = numeric_root_compatibility(bytes)) {
            parsed.document.reset();
            result = parsed.document.load_buffer(compatible->bytes.data(), compatible->bytes.size(),
                pugi::parse_full | pugi::parse_ws_pcdata, pugi::encoding_utf8);
            if (result) numeric_root_name = std::move(compatible->authored_name);
        }
    }
    if (!result) {
        const auto [line, column] = line_column(bytes, result.offset);
        SourceLocation source{record.canonical_path, record.source_id, record.layer_id, line, column};
        return core::Result<ParsedDocument>::failure(diagnostic(
            diagnostic_codes::malformed_xml,
            std::string("malformed XML: ") + result.description(),
            &source));
    }
    for (const auto child : parsed.document.children()) {
        if (child.type() == pugi::node_doctype) {
            const auto [line, column] = line_column(bytes, child.offset_debug());
            SourceLocation source{record.canonical_path, record.source_id, record.layer_id, line, column};
            return core::Result<ParsedDocument>::failure(diagnostic(
                diagnostic_codes::forbidden_doctype,
                "DOCTYPE declarations are forbidden",
                &source));
        }
        if (child.type() == pugi::node_element) {
            if (parsed.root) {
                SourceLocation source{record.canonical_path, record.source_id, record.layer_id, 1, 1};
                return core::Result<ParsedDocument>::failure(diagnostic(
                    diagnostic_codes::malformed_xml,
                    "XML document contains more than one root element",
                    &source));
            }
            parsed.root = child;
        }
    }
    if (!parsed.root) {
        SourceLocation source{record.canonical_path, record.source_id, record.layer_id, 1, 1};
        return core::Result<ParsedDocument>::failure(diagnostic(
            diagnostic_codes::malformed_xml, "XML document has no root element", &source));
    }
    if (numeric_root_name) parsed.root.set_name(numeric_root_name->c_str());
    return core::Result<ParsedDocument>::success(std::move(parsed));
}

const MergeRule* merge_rule(
    const LoadOptions& options,
    const std::string_view object_type,
    const std::string_view tag_name
) {
    for (auto rule = options.merge_rules.rbegin(); rule != options.merge_rules.rend(); ++rule) {
        if (ascii_iequals(rule->object_type, object_type) && ascii_iequals(rule->tag_name, tag_name)) {
            return &*rule;
        }
    }
    return nullptr;
}

MergeMode merge_mode(
    const LoadOptions& options,
    const std::string_view object_type,
    const std::string_view tag_name
) {
    if (const auto* rule = merge_rule(options, object_type, tag_name)) return rule->mode;
    // The pinned eaw-schema revision has one explicit variant merge declaration.
    if (ascii_iequals(object_type, "GameObjectType") && ascii_iequals(tag_name, "Death_Clone")) {
        return MergeMode::merge;
    }
    return MergeMode::replace;
}

bool multiple_allowed(
    const LoadOptions& options,
    const std::string_view object_type,
    const std::string_view tag_name
) {
    if (const auto* rule = merge_rule(options, object_type, tag_name)) return rule->multiple_allowed;
    return ascii_iequals(object_type, "GameObjectType") && ascii_iequals(tag_name, "Death_Clone");
}

void classify(
    XmlNode& node,
    const Profile profile,
    std::string object_type,
    const std::span<const std::byte> bytes,
    const pugi::xml_node source_node,
    std::vector<core::Diagnostic>& diagnostics,
    const std::string& path,
    const LoadOptions& options
) {
    std::string schema_path = path;
    if (const auto nested_type = canonical_object_type(node.name); nested_type && !ascii_iequals(node.name, path)) {
        object_type = *nested_type;
        schema_path = node.name;
    }
    const auto schema = schema_value(profile, object_type, schema_path, "element", node.name);
    node.schema_status = schema.status;
    if (!schema.schema_ref.empty()) node.schema_ref = std::string(schema.schema_ref);
    node.merge_mode = merge_mode(options, object_type, node.name);
    node.multiple_allowed = multiple_allowed(options, object_type, node.name);
    if (node.schema_status == SchemaStatus::unknown) {
        diagnostics.push_back(diagnostic(
            diagnostic_codes::unknown_field,
            std::string(to_string(profile)) + "/" + object_type + "/" + schema_path + "/element/" + node.name,
            &node.source,
            core::Severity::warning));
    } else if (node.schema_status == SchemaStatus::deprecated) {
        diagnostics.push_back(diagnostic(
            diagnostic_codes::deprecated_field,
            std::string(to_string(profile)) + "/" + object_type + "/" + schema_path + "/element/" + node.name,
            &node.source,
            core::Severity::warning));
    }
    for (auto& attribute : node.attributes) {
        const auto attribute_path = schema_path + "/@" + attribute.name;
        const auto value = schema_value(profile, object_type, attribute_path, "attribute", attribute.name);
        attribute.schema_status = value.status;
        if (!value.schema_ref.empty()) attribute.schema_ref = std::string(value.schema_ref);
        if (attribute.schema_status == SchemaStatus::unknown) {
            diagnostics.push_back(diagnostic(
                diagnostic_codes::unknown_field,
                std::string(to_string(profile)) + "/" + object_type + "/" + attribute_path +
                    "/attribute/" + attribute.name,
                &attribute.source,
                core::Severity::warning));
        } else if (attribute.schema_status == SchemaStatus::deprecated) {
            diagnostics.push_back(diagnostic(
                diagnostic_codes::deprecated_field,
                std::string(to_string(profile)) + "/" + object_type + "/" + attribute_path +
                    "/attribute/" + attribute.name,
                &attribute.source,
                core::Severity::warning));
        }
    }
    std::size_t child_index = 0;
    for (auto child = source_node.first_child(); child; child = child.next_sibling()) {
        if (child.type() != pugi::node_element) continue;
        auto& target = node.children[child_index++];
        classify(target, profile, object_type, bytes, child, diagnostics,
                 schema_path + "/" + target.name, options);
    }
}

XmlNode build_node(
    const pugi::xml_node source,
    const std::span<const std::byte> bytes,
    const vfs::AssetRecord& record
) {
    const auto [line, column] = line_column(bytes, source.offset_debug());
    XmlNode result{
        .name = source.name(),
        .raw_text = {},
        .attributes = {},
        .children = {},
        .source = SourceLocation{record.canonical_path, record.source_id, record.layer_id, line, column},
        .schema_status = SchemaStatus::unknown,
        .schema_ref = std::nullopt,
        .merge_mode = MergeMode::replace,
        .multiple_allowed = false,
    };
    const auto offsets = attribute_offsets(bytes, source.offset_debug());
    std::size_t attribute_index = 0;
    for (const auto attribute : source.attributes()) {
        const auto attribute_offset = attribute_index < offsets.size()
            ? offsets[attribute_index] : source.offset_debug();
        const auto [attribute_line, attribute_column] = line_column(bytes, attribute_offset);
        result.attributes.push_back(XmlAttribute{
            .name = attribute.name(),
            .value = attribute.value(),
            .source = SourceLocation{record.canonical_path, record.source_id, record.layer_id,
                                     attribute_line, attribute_column},
            .schema_status = SchemaStatus::unknown,
            .schema_ref = std::nullopt,
        });
        ++attribute_index;
    }
    for (const auto child : source.children()) {
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
            result.raw_text += child.value();
        } else if (child.type() == pugi::node_element) {
            result.children.push_back(build_node(child, bytes, record));
        }
    }
    return result;
}

std::optional<std::string> attribute_value(const XmlNode& node, const std::string_view name) {
    for (const auto& attribute : node.attributes) {
        if (ascii_iequals(attribute.name, name)) return attribute.value;
    }
    return std::nullopt;
}

std::string included_path(const std::string_view registry_path, std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    value = trim(value);
    if (ascii_lower(value).starts_with("data/")) return value;
    const auto slash = registry_path.rfind('/');
    return std::string(registry_path.substr(0, slash + 1)) + value;
}

int layer_rank(const std::string_view layer) {
    const auto key = ascii_lower(layer);
    if (key == "mod") return 100'000;
    if (key.starts_with("mod[") && key.ends_with(']')) {
        int index = 0;
        bool valid = key.size() > 5;
        for (std::size_t i = 4; valid && i + 1 < key.size(); ++i) {
            valid = key[i] >= '0' && key[i] <= '9';
            if (valid) index = index * 10 + (key[i] - '0');
        }
        if (valid) return 99'999 - index;
    }
    if (key == "expansion") return 1'000;
    if (key == "base") return 0;
    return 0;
}

std::string record_identity(const vfs::AssetRecord& record) {
    std::string result = record.canonical_path;
    result.push_back('\x1f');
    result += record.source_id;
    return result;
}

std::string token_merge(const std::string_view base, const std::string_view derived) {
    auto tokens = [](const std::string_view value) {
        std::vector<std::string> result;
        std::size_t start = 0;
        auto separator = [](const char ch) {
            return ch == ',' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
        };
        while (start < value.size()) {
            while (start < value.size() && separator(value[start])) ++start;
            std::size_t end = start;
            while (end < value.size() && !separator(value[end])) ++end;
            if (end > start) result.emplace_back(value.substr(start, end - start));
            start = end;
        }
        return result;
    };
    auto values = tokens(base);
    auto added = tokens(derived);
    values.insert(values.end(), added.begin(), added.end());
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ", ";
        out << values[i];
    }
    return out.str();
}

const XmlNode* child_named(const XmlNode& node, const std::string_view name) {
    const XmlNode* result = nullptr;
    for (const auto& child : node.children) {
        if (ascii_iequals(child.name, name)) result = &child;
    }
    return result;
}

std::atomic<std::uint64_t> next_generation{1};

void add_definition(
    Catalog::Impl& catalog,
    const pugi::xml_node source,
    const std::span<const std::byte> bytes,
    const vfs::AssetRecord& record,
    const Profile profile,
    Category category,
    std::string schema_type,
    const std::size_t registry_order,
    const std::size_t definition_order,
    std::vector<core::Diagnostic>& diagnostics,
    const LoadOptions& options
) {
    auto root = build_node(source, bytes, record);
    classify(root, profile, schema_type, bytes, source, diagnostics, root.name, options);
    const auto id_value = attribute_value(root, "Name");
    Definition definition{
        .id = id_value ? trim(*id_value) : std::string{},
        .type_name = root.name,
        .schema_type_name = std::move(schema_type),
        .category = category,
        .root = std::move(root),
        .registry_order = registry_order,
        .definition_order = definition_order,
    };
    if (definition.id.empty()) {
        diagnostics.push_back(diagnostic(
            diagnostic_codes::definition_missing_id,
            "active definition has no non-empty Name attribute",
            &definition.root.source));
    }
    catalog.definitions.push_back(std::move(definition));
}

void add_nested_abilities(
    Catalog::Impl& catalog,
    const pugi::xml_node node,
    const std::span<const std::byte> bytes,
    const vfs::AssetRecord& record,
    const Profile profile,
    const std::size_t registry_order,
    std::size_t& definition_order,
    std::vector<core::Diagnostic>& diagnostics,
    const LoadOptions& options,
    const bool inside_abilities
) {
    const bool in_list = inside_abilities || ascii_iequals(node.name(), "Abilities");
    for (const auto child : node.children()) {
        if (child.type() != pugi::node_element) continue;
        const auto type = canonical_object_type(child.name());
        const bool ability = in_list && child.attribute("Name") &&
            (is_ability_type(child.name()) || (type && is_ability_type(*type)));
        if (ability) {
            std::vector<core::Diagnostic> already_reported_by_owner;
            add_definition(catalog, child, bytes, record, profile, Category::ability,
                           type.value_or(std::string(child.name())), registry_order,
                           definition_order++, already_reported_by_owner, options);
        }
        add_nested_abilities(catalog, child, bytes, record, profile, registry_order,
                             definition_order, diagnostics, options, in_list);
    }
}

void index_winners(Catalog::Impl& catalog, std::vector<core::Diagnostic>& diagnostics) {
    for (std::size_t i = 0; i < catalog.definitions.size(); ++i) {
        if (!catalog.definitions[i].id.empty()) catalog.by_id[ascii_lower(catalog.definitions[i].id)].push_back(i);
    }
    for (auto& [id, indexes] : catalog.by_id) {
        std::stable_sort(indexes.begin(), indexes.end(), [&](const std::size_t left, const std::size_t right) {
            const auto& a = catalog.definitions[left];
            const auto& b = catalog.definitions[right];
            const auto ar = layer_rank(a.root.source.layer_id);
            const auto br = layer_rank(b.root.source.layer_id);
            if (ar != br) return ar > br;
            if (a.registry_order != b.registry_order) return a.registry_order > b.registry_order;
            return a.definition_order > b.definition_order;
        });
        catalog.winners[id] = indexes.front();
        catalog.definitions[indexes.front()].winner = true;
        if (indexes.size() > 1) {
            const auto& winner = catalog.definitions[indexes.front()];
            std::ostringstream message;
            message << "duplicate object ID '" << winner.id << "' has " << indexes.size()
                    << " active definitions; strongest layer then later registry/definition order wins";
            diagnostics.push_back(diagnostic(diagnostic_codes::duplicate_id, message.str(),
                                             &winner.root.source, core::Severity::warning));
        }
    }
}


} // namespace eawr::data
