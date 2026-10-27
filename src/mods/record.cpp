#include "eawr/mods/record.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <utility>

namespace eawr::mods {

namespace {

using steam::json_quote;
using steam::JsonValue;

constexpr std::array<std::pair<ModKind, std::string_view>, 3> kKinds{{
    {ModKind::workshop, "workshop"}, {ModKind::mods_folder, "mods-folder"}, {ModKind::manual, "manual"}}};
constexpr std::array<std::pair<ModRole, std::string_view>, 3> kRoles{{
    {ModRole::playable, "playable"}, {ModRole::parent_only, "parent-only"}, {ModRole::unknown, "unknown"}}};
constexpr std::array<std::pair<ModState, std::string_view>, 4> kStates{{
    {ModState::installed, "installed"}, {ModState::needs_update, "needs-update"}, {ModState::downloading, "downloading"},
    {ModState::missing, "missing"}}};

template <typename Enum, std::size_t N>
std::string_view name_of(const std::array<std::pair<Enum, std::string_view>, N>& table, Enum value) noexcept {
    for (const auto& [entry, text] : table) {
        if (entry == value) return text;
    }
    return {};
}

template <typename Enum, std::size_t N>
std::optional<Enum> parse_name(const std::array<std::pair<Enum, std::string_view>, N>& table, std::string_view text) noexcept {
    for (const auto& [entry, name] : table) {
        if (name == text) return entry;
    }
    return std::nullopt;
}

// Whole numbers print exactly; others with round-trip precision and a '.' whatever the C locale says.
std::string number_text(double value) {
    if (!std::isfinite(value)) return "null";
    if (std::floor(value) == value && std::fabs(value) <= 9007199254740992.0) {
        return std::to_string(static_cast<long long>(value));
    }
    std::array<char, 64> buffer{};
    const int length = std::snprintf(buffer.data(), buffer.size(), "%.17g", value);
    std::string text(buffer.data(), length > 0 ? static_cast<std::size_t>(length) : 0u);
    for (char& c : text) {
        if (c == ',') c = '.';
    }
    return text;
}

void write_json(std::string& out, const JsonValue& value) {
    switch (value.kind) {
    case JsonValue::Kind::null: out += "null"; break;
    case JsonValue::Kind::boolean: out += value.boolean ? "true" : "false"; break;
    case JsonValue::Kind::number: out += number_text(value.number); break;
    case JsonValue::Kind::string: out += json_quote(value.string); break;
    case JsonValue::Kind::array:
        out += '[';
        for (std::size_t index = 0; index < value.array.size(); ++index) {
            if (index != 0) out += ", ";
            write_json(out, value.array[index]);
        }
        out += ']';
        break;
    case JsonValue::Kind::object:
        out += '{';
        for (std::size_t index = 0; index < value.keys.size(); ++index) {
            if (index != 0) out += ", ";
            out += json_quote(value.keys[index]);
            out += ": ";
            write_json(out, value.values[index]);
        }
        out += '}';
        break;
    }
}

void write_warnings(std::string& out, const std::vector<Warning>& warnings) {
    out += '[';
    for (std::size_t index = 0; index < warnings.size(); ++index) {
        if (index != 0) out += ", ";
        out += "{\"code\": " + json_quote(warnings[index].code) + ", \"message\": " + json_quote(warnings[index].message) + "}";
    }
    out += ']';
}

const JsonValue* typed(const JsonValue& object, std::string_view key, JsonValue::Kind kind) {
    const JsonValue* member = object.member(key);
    return member != nullptr && member->kind == kind ? member : nullptr;
}

// A non-negative whole number that a double holds exactly.
template <typename Integer>
bool read_integer(const JsonValue& object, std::string_view key, Integer& out) {
    const JsonValue* member = typed(object, key, JsonValue::Kind::number);
    if (member == nullptr) return false;
    const double value = member->number;
    if (!(value >= 0) || value > 9007199254740992.0 || std::floor(value) != value) return false;
    out = static_cast<Integer>(value);
    return true;
}

std::optional<std::vector<Warning>> read_warnings(const JsonValue* list) {
    if (list == nullptr || list->kind != JsonValue::Kind::array) return std::nullopt;
    std::vector<Warning> warnings;
    for (const JsonValue& item : list->array) {
        const std::string* code = item.string_member("code");
        const std::string* message = item.string_member("message");
        if (code == nullptr || message == nullptr) return std::nullopt;
        warnings.push_back({*code, *message});
    }
    return warnings;
}

} // namespace

std::string_view to_string(ModKind kind) noexcept { return name_of(kKinds, kind); }
std::string_view to_string(ModRole role) noexcept { return name_of(kRoles, role); }
std::string_view to_string(ModState state) noexcept { return name_of(kStates, state); }
std::optional<ModKind> parse_mod_kind(std::string_view text) noexcept { return parse_name(kKinds, text); }
std::optional<ModRole> parse_mod_role(std::string_view text) noexcept { return parse_name(kRoles, text); }
std::optional<ModState> parse_mod_state(std::string_view text) noexcept { return parse_name(kStates, text); }

bool ModRecord::has_warning(std::string_view code) const {
    for (const Warning& warning : warnings) {
        if (warning.code == code) return true;
    }
    return false;
}

std::string record_to_json(const ModRecord& record) {
    std::string out = "{\"key\": " + json_quote(record.key);
    out += ", \"kind\": " + json_quote(to_string(record.kind));
    out += ", \"name\": " + json_quote(record.name);
    out += ", \"alias\": " + (record.alias ? json_quote(*record.alias) : std::string("null"));
    out += ", \"root\": " + json_quote(record.root);
    out += ", \"has_data\": ";
    out += record.has_data ? "true" : "false";
    out += ", \"metadata\": ";
    if (record.metadata.kind == JsonValue::Kind::object) write_json(out, record.metadata);
    else out += "{}";
    out += ", \"role\": " + json_quote(to_string(record.role));
    out += ", \"state\": " + json_quote(to_string(record.state));
    const Fingerprint& print = record.fingerprint;
    out += ", \"fingerprint\": {\"acf_manifest\": " + json_quote(print.acf_manifest);
    out += ", \"time_updated\": " + std::to_string(print.time_updated);
    out += ", \"acf_size\": " + std::to_string(print.acf_size);
    out += ", \"tree\": " + json_quote(print.tree);
    out += ", \"tree_files\": " + std::to_string(print.tree_files);
    out += ", \"tree_capped\": ";
    out += print.tree_capped ? "true" : "false";
    out += "}, \"warnings\": ";
    write_warnings(out, record.warnings);
    out += '}';
    return out;
}

std::optional<ModRecord> record_from_json(const JsonValue& value) {
    if (value.kind != JsonValue::Kind::object) return std::nullopt;
    ModRecord record;
    const std::string* key = value.string_member("key");
    const std::string* kind = value.string_member("kind");
    const std::string* name = value.string_member("name");
    const std::string* root = value.string_member("root");
    const std::string* role = value.string_member("role");
    const std::string* state = value.string_member("state");
    const JsonValue* has_data = typed(value, "has_data", JsonValue::Kind::boolean);
    const JsonValue* metadata = typed(value, "metadata", JsonValue::Kind::object);
    const JsonValue* print = typed(value, "fingerprint", JsonValue::Kind::object);
    if (key == nullptr || key->empty() || kind == nullptr || name == nullptr || root == nullptr || role == nullptr || state == nullptr
        || has_data == nullptr || metadata == nullptr || print == nullptr) {
        return std::nullopt;
    }
    const auto parsed_kind = parse_mod_kind(*kind);
    const auto parsed_role = parse_mod_role(*role);
    const auto parsed_state = parse_mod_state(*state);
    if (!parsed_kind || !parsed_role || !parsed_state) return std::nullopt;
    record.key = *key;
    record.kind = *parsed_kind;
    record.name = *name;
    record.root = *root;
    record.role = *parsed_role;
    record.state = *parsed_state;
    record.has_data = has_data->boolean;
    record.metadata = *metadata;
    if (const JsonValue* alias = value.member("alias"); alias != nullptr && alias->kind != JsonValue::Kind::null) {
        if (alias->kind != JsonValue::Kind::string) return std::nullopt;
        record.alias = alias->string;
    }

    const std::string* manifest = print->string_member("acf_manifest");
    const std::string* tree = print->string_member("tree");
    const JsonValue* capped = typed(*print, "tree_capped", JsonValue::Kind::boolean);
    if (manifest == nullptr || tree == nullptr || capped == nullptr || !read_integer(*print, "time_updated", record.fingerprint.time_updated)
        || !read_integer(*print, "acf_size", record.fingerprint.acf_size) || !read_integer(*print, "tree_files", record.fingerprint.tree_files)) {
        return std::nullopt;
    }
    record.fingerprint.acf_manifest = *manifest;
    record.fingerprint.tree = *tree;
    record.fingerprint.tree_capped = capped->boolean;

    auto warnings = read_warnings(value.member("warnings"));
    if (!warnings) return std::nullopt;
    record.warnings = std::move(*warnings);
    return record;
}

std::string catalog_to_json(const Catalog& catalog) {
    std::string out = "{\"format\": " + json_quote(kCatalogFormat) + ", \"version\": " + std::to_string(kCatalogVersion);
    out += ", \"records\": [";
    for (std::size_t index = 0; index < catalog.records.size(); ++index) {
        out += index == 0 ? "\n  " : ",\n  ";
        out += record_to_json(catalog.records[index]);
    }
    out += catalog.records.empty() ? "]" : "\n]";
    out += ", \"warnings\": ";
    write_warnings(out, catalog.warnings);
    out += "}\n";
    return out;
}

std::optional<Catalog> catalog_from_json(std::string_view text) {
    const std::optional<JsonValue> root = steam::parse_json(text);
    if (!root || root->kind != JsonValue::Kind::object) return std::nullopt;
    const std::string* format = root->string_member("format");
    int version = 0;
    if (format == nullptr || *format != kCatalogFormat || !read_integer(*root, "version", version) || version != kCatalogVersion) {
        return std::nullopt;
    }
    const JsonValue* records = typed(*root, "records", JsonValue::Kind::array);
    if (records == nullptr) return std::nullopt;
    Catalog catalog;
    for (const JsonValue& item : records->array) {
        auto record = record_from_json(item);
        if (!record) return std::nullopt;
        catalog.records.push_back(std::move(*record));
    }
    auto warnings = read_warnings(root->member("warnings"));
    if (!warnings) return std::nullopt;
    catalog.warnings = std::move(*warnings);
    return catalog;
}

} // namespace eawr::mods
