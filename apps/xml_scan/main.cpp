#include "eawr/core/diagnostic.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Arguments {
    eawr::data::Profile profile{eawr::data::Profile::eaw};
    std::filesystem::path game_root;
    std::optional<std::filesystem::path> mod_root;
    std::filesystem::path report;
    std::optional<std::filesystem::path> input_receipt;
    std::vector<std::string> samples;
    bool profile_set{false};
};

void usage(std::ostream& out) {
    out << "Usage: xml_scan --profile <eaw|foc|remake> --game-root <dir> "
           "[--mod-root <leaf;parent;...>] --report <json> [--input-receipt <json>] [--sample <object-id>]...\n";
}

std::optional<Arguments> arguments(const int argc, char** argv) {
    Arguments result;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option(argv[i]);
        if (option == "--help") { usage(std::cout); return std::nullopt; }
        if (i + 1 >= argc) return std::nullopt;
        const std::string value(argv[++i]);
        if (option == "--profile") {
            result.profile_set = true;
            if (value == "eaw") result.profile = eawr::data::Profile::eaw;
            else if (value == "foc") result.profile = eawr::data::Profile::foc;
            else if (value == "remake") result.profile = eawr::data::Profile::remake;
            else return std::nullopt;
        } else if (option == "--game-root") result.game_root = value;
        else if (option == "--mod-root") result.mod_root = std::filesystem::path(value);
        else if (option == "--report") result.report = value;
        else if (option == "--input-receipt") result.input_receipt = std::filesystem::path(value);
        else if (option == "--sample") result.samples.push_back(value);
        else return std::nullopt;
    }
    if (!result.profile_set || result.game_root.empty() || result.report.empty() ||
        (result.profile == eawr::data::Profile::remake && !result.mod_root)) return std::nullopt;
    return result;
}

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string lower(std::string value) {
    for (char& ch : value) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    return value;
}

bool iequals(const std::string_view left, const std::string_view right) {
    return lower(std::string(left)) == lower(std::string(right));
}

std::filesystem::path data_root(const std::filesystem::path& value) {
    if (iequals(utf8(value.filename()), "data")) return value;
    if (std::filesystem::is_directory(value / "Data")) return value / "Data";
    return value;
}

std::optional<std::vector<std::pair<std::string, std::filesystem::path>>> roots(const Arguments& args) {
    const bool install = std::filesystem::is_directory(args.game_root / "GameData" / "Data") &&
                         std::filesystem::is_directory(args.game_root / "corruption" / "Data");
    std::vector<std::pair<std::string, std::filesystem::path>> result;
    if (args.profile == eawr::data::Profile::eaw) {
        result.emplace_back("base", install ? args.game_root / "GameData" / "Data" : data_root(args.game_root));
    } else {
        if (!install) return std::nullopt;
        if (args.profile == eawr::data::Profile::remake) {
            for (const auto& layer : eawr::vfs::mod_chain_roots(*args.mod_root)) result.push_back(layer);
        }
        result.emplace_back("expansion", args.game_root / "corruption" / "Data");
        result.emplace_back("base", args.game_root / "GameData" / "Data");
    }
    return result;
}

std::string json(const std::string_view value) {
    std::ostringstream out;
    out << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20U) out << "\\u00" << hex[ch >> 4U] << hex[ch & 15U];
            else out << static_cast<char>(ch);
        }
    }
    out << '"';
    return out.str();
}

constexpr std::string_view schema_revision = "3e1b825a124fbc13b2293665f34a36dd4d4be80f";

std::string receipt_path(const std::string_view value) {
    // An authored File value may resemble a native path after the loader prefixes
    // data/xml/. Preserve the loader attempt, but redact that value in the receipt.
    if (value.find(':') == std::string_view::npos && value.find("//") == std::string_view::npos &&
        value.find("..") == std::string_view::npos) return std::string(value);
    const auto digest = eawr::core::sha256_hex({
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
    return "invalid-logical-path:" + digest;
}

// Length framing keeps every ordered field boundary unambiguous, including absent fields.
void identity_field(std::string& canonical, const std::string_view value) {
    canonical += std::to_string(value.size());
    canonical += ':';
    canonical += value;
}

void identity_row(std::string& canonical, const std::string_view kind,
                  const eawr::data::Category category, const std::string_view path,
                  const std::string_view order, const std::string_view outcome,
                  const std::optional<eawr::data::SourceLocation>& source,
                  const std::optional<std::string>& digest) {
    identity_field(canonical, kind);
    identity_field(canonical, eawr::data::to_string(category));
    identity_field(canonical, path);
    identity_field(canonical, order);
    identity_field(canonical, outcome);
    identity_field(canonical, source ? "1" : "0");
    identity_field(canonical, source ? source->source_id : "");
    identity_field(canonical, source ? source->layer_id : "");
    identity_field(canonical, digest ? "1" : "0");
    identity_field(canonical, digest.value_or(""));
}

void write_receipt_row(std::ostream& out, const std::string_view category,
                       const std::string_view path, const std::string_view outcome,
                       const std::optional<eawr::data::SourceLocation>& source,
                       const std::optional<std::string>& digest) {
    out << "{\"category\":" << json(category)
        << ",\"logical_path\":" << json(path)
        << ",\"outcome\":" << json(outcome)
        << ",\"source_id\":" << (source ? json(source->source_id) : "null")
        << ",\"layer\":" << (source ? json(source->layer_id) : "null")
        << ",\"sha256\":" << (digest ? json(*digest) : "null");
}

bool write_input_receipt(const std::filesystem::path& path, const eawr::data::Catalog& catalog) {
    const auto parent = path.parent_path();
    std::filesystem::create_directories(parent.empty() ? std::filesystem::path(".") : parent);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    std::string canonical;
    identity_field(canonical, "eawr-xml-input-receipt-v1");
    identity_field(canonical, eawr::data::to_string(catalog.profile()));
    identity_field(canonical, schema_revision);
    for (const auto& root : catalog.registry_roots()) {
        identity_row(canonical, "root", root.category, root.registry_path, "", root.outcome,
                     root.source, root.input_sha256);
    }
    for (const auto& file : catalog.registry_files()) {
        identity_row(canonical, "include", file.category, receipt_path(file.included_path),
                     std::to_string(file.include_order), file.outcome, file.source, file.input_sha256);
        identity_field(canonical, file.registry_path);
    }
    const auto digest = eawr::core::sha256_hex({
        reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()});
    out << "{\"schema_version\":1,\"profile\":" << json(eawr::data::to_string(catalog.profile()))
        << ",\"schema_revision\":" << json(schema_revision)
        << ",\"scope\":\"project scanner observed XML inputs in enumerated VFS mounts; not complete retail mounted content or runtime observation\""
        << ",\"identity_algorithm\":\"sha256-length-framed-v1\",\"identity_sha256\":" << json(digest)
        << ",\"roots\":[";
    for (std::size_t i = 0; i < catalog.registry_roots().size(); ++i) {
        if (i) out << ',';
        const auto& root = catalog.registry_roots()[i];
        write_receipt_row(out, eawr::data::to_string(root.category), root.registry_path,
                          root.outcome, root.source, root.input_sha256);
        out << '}';
    }
    out << "],\"includes\":[";
    for (std::size_t i = 0; i < catalog.registry_files().size(); ++i) {
        if (i) out << ',';
        const auto& file = catalog.registry_files()[i];
        write_receipt_row(out, eawr::data::to_string(file.category), receipt_path(file.included_path),
                          file.outcome, file.source, file.input_sha256);
        out << ",\"registry_path\":" << json(file.registry_path)
            << ",\"include_order\":" << file.include_order << '}';
    }
    out << "]}\n";
    out.close();
    return static_cast<bool>(out);
}

void write_diagnostic(std::ostream& out, const eawr::core::Diagnostic& value) {
    out << "{\"code\":" << json(value.code)
        << ",\"severity\":" << json(eawr::core::to_string(value.severity))
        << ",\"message\":" << json(value.message)
        << ",\"logical_path\":" << (value.logical_path ? json(*value.logical_path) : "null")
        << ",\"line\":" << (value.line ? std::to_string(*value.line) : "null")
        << ",\"column\":" << (value.column ? std::to_string(*value.column) : "null")
        << ",\"source_id\":" << (value.source_id ? json(*value.source_id) : "null") << '}';
}

std::string_view provenance(const eawr::data::ValueProvenance value) {
    switch (value) {
    case eawr::data::ValueProvenance::own: return "own";
    case eawr::data::ValueProvenance::inherited: return "inherited";
    case eawr::data::ValueProvenance::added: return "added";
    case eawr::data::ValueProvenance::overridden: return "overridden";
    case eawr::data::ValueProvenance::merged: return "merged";
    }
    return "own";
}

void write_source(std::ostream& out, const eawr::data::SourceLocation& value) {
    out << "{\"logical_path\":" << json(value.logical_path)
        << ",\"source_id\":" << json(value.source_id)
        << ",\"layer\":" << json(value.layer_id)
        << ",\"line\":" << value.line
        << ",\"column\":" << value.column << '}';
}

// Ordered attributes and nested elements of one effective occurrence, recursively, so a sample
// can be compared structurally rather than through its direct text alone.
void write_nested(std::ostream& out, const eawr::data::XmlNode& value) {
    out << "\"attributes\":[";
    for (std::size_t i = 0; i < value.attributes.size(); ++i) {
        if (i) out << ',';
        out << "{\"name\":" << json(value.attributes[i].name)
            << ",\"value\":" << json(value.attributes[i].value) << '}';
    }
    out << "],\"children\":[";
    for (std::size_t i = 0; i < value.children.size(); ++i) {
        if (i) out << ',';
        const auto& child = value.children[i];
        out << "{\"name\":" << json(child.name)
            << ",\"raw_text\":" << json(child.raw_text)
            << ",\"line\":" << child.source.line << ',';
        write_nested(out, child);
        out << '}';
    }
    out << ']';
}

void write_raw_tag(std::ostream& out, const eawr::data::XmlNode& value) {
    out << "{\"name\":" << json(value.name)
        << ",\"raw_text\":" << json(value.raw_text)
        << ",\"source\":";
    write_source(out, value.source);
    out << '}';
}

std::string source_hash(const eawr::vfs::Vfs& vfs, const std::string_view path) {
    const auto bytes = vfs.open(path);
    if (!bytes) return {};
    const auto input = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.value().data()), bytes.value().size());
    return eawr::sim::sha256_hex(input);
}

void write_sample(
    std::ostream& out,
    const eawr::data::Catalog& catalog,
    const eawr::vfs::Vfs& vfs,
    const std::string_view object_id
) {
    out << "{\"requested_id\":" << json(object_id);
    auto effective = catalog.resolve(object_id);
    if (!effective) {
        out << ",\"error\":";
        write_diagnostic(out, effective.error());
        out << '}';
        return;
    }
    out << ",\"object_id\":" << json(effective.value().object_id)
        << ",\"type_name\":" << json(effective.value().type_name)
        << ",\"chain\":[";
    for (std::size_t i = 0; i < effective.value().chain.size(); ++i) {
        if (i) out << ',';
        out << json(effective.value().chain[i]);
    }
    out << "],\"raw_chain\":[";
    for (std::size_t i = 0; i < effective.value().chain.size(); ++i) {
        if (i) out << ',';
        const auto* definition = catalog.find(effective.value().chain[i]);
        if (definition == nullptr) { out << "null"; continue; }
        out << "{\"id\":" << json(definition->id)
            << ",\"type_name\":" << json(definition->type_name)
            << ",\"source\":";
        write_source(out, definition->root.source);
        out << ",\"input_sha256\":" << json(source_hash(vfs, definition->root.source.logical_path))
            << ",\"tags\":[";
        for (std::size_t tag = 0; tag < definition->root.children.size(); ++tag) {
            if (tag) out << ',';
            write_raw_tag(out, definition->root.children[tag]);
        }
        out << "]}";
    }
    out << "],\"effective_values\":[";
    for (std::size_t i = 0; i < effective.value().values.size(); ++i) {
        if (i) out << ',';
        const auto& value = effective.value().values[i];
        out << "{\"name\":" << json(value.value.name)
            << ",\"raw_text\":" << json(value.value.raw_text)
            << ",\"provenance\":" << json(provenance(value.provenance))
            << ",\"source_object_id\":" << json(value.source_object_id)
            << ",\"source\":";
        write_source(out, value.value.source);
        out << ",\"displaced_raw_text\":"
            << (value.displaced_value ? json(value.displaced_value->raw_text) : "null") << ',';
        write_nested(out, value.value);
        out << '}';
    }
    out << "]}";
}

} // namespace

int main(const int argc, char** argv) {
    const auto args = arguments(argc, argv);
    if (!args) {
        if (argc == 2 && std::string_view(argv[1]) == "--help") return 0;
        usage(std::cerr);
        return 2;
    }
    if (args->input_receipt &&
        std::filesystem::absolute(args->report).lexically_normal() ==
            std::filesystem::absolute(*args->input_receipt).lexically_normal()) {
        std::cerr << "report and input receipt must have different paths\n";
        return 2;
    }
    const auto profile_roots = roots(*args);
    if (!profile_roots) {
        std::cerr << "foc/remake require an installation root containing GameData/Data and corruption/Data\n";
        return 2;
    }
    std::vector<eawr::vfs::MountSpec> specs;
    auto chain = eawr::vfs::resolve_manifest_chain(*profile_roots);
    if (!chain) {
        std::cerr << eawr::core::format_diagnostic(chain.error()) << '\n';
        return 1;
    }
    for (auto& manifest : chain.value()) {
        specs.push_back(std::move(manifest.mount));
    }
    auto mounted = eawr::vfs::Vfs::mount(specs);
    if (!mounted) { std::cerr << eawr::core::format_diagnostic(mounted.error()) << '\n'; return 2; }
    auto loaded = eawr::data::load_catalog(mounted.value(), args->profile);
    if (!loaded) { std::cerr << eawr::core::format_diagnostic(loaded.error()) << '\n'; return 2; }

    auto& catalog = loaded.value().catalog;
    if (args->input_receipt && !write_input_receipt(*args->input_receipt, catalog)) {
        std::cerr << "cannot create input receipt\n";
        return 2;
    }
    auto diagnostics = std::move(loaded.value().diagnostics);
    std::vector<eawr::core::Diagnostic> unresolved;
    std::map<std::string, std::size_t> category_counts;
    std::map<std::string, std::size_t> winner_counts;
    for (const auto& definition : catalog.definitions()) {
        ++category_counts[std::string(eawr::data::to_string(definition.category))];
        if (!definition.winner) continue;
        ++winner_counts[std::string(eawr::data::to_string(definition.category))];
        auto effective = catalog.resolve(definition.id);
        if (!effective) unresolved.push_back(effective.error());
    }

    std::filesystem::create_directories(args->report.parent_path().empty()
        ? std::filesystem::path(".") : args->report.parent_path());
    std::ofstream out(args->report, std::ios::binary);
    if (!out) { std::cerr << "cannot create report\n"; return 2; }
    out << "{\n  \"schema_version\":1,\n  \"schema_revision\":" << json(schema_revision) << ','
        << "\n  \"profile\":" << json(eawr::data::to_string(args->profile))
        << ",\n  \"catalog_generation\":" << catalog.generation()
        << ",\n  \"counts\":{\"definitions\":" << catalog.definitions().size()
        << ",\"physical_xml_records\":" << catalog.physical_inventory().size()
        << ",\"registry_includes\":" << catalog.registry_files().size()
        << ",\"diagnostics\":" << diagnostics.size()
        << ",\"unresolved\":" << unresolved.size() << ",\"by_category\":{";
    bool first = true;
    for (const auto& [name, count] : category_counts) {
        if (!first) out << ',';
        first = false;
        out << json(name) << ":{\"raw\":" << count << ",\"winners\":" << winner_counts[name] << '}';
    }
    out << "}},\n  \"registries\":[";
    for (std::size_t i = 0; i < catalog.registry_files().size(); ++i) {
        if (i) out << ',';
        const auto& value = catalog.registry_files()[i];
        out << "{\"category\":" << json(eawr::data::to_string(value.category))
            << ",\"registry_path\":" << json(value.registry_path)
            << ",\"included_path\":" << json(value.included_path)
            << ",\"include_order\":" << value.include_order
            << ",\"loaded\":" << (value.loaded ? "true" : "false")
            << ",\"source_id\":" << (value.source ? json(value.source->source_id) : "null") << '}';
    }
    out << "],\n  \"physical_inventory\":[";
    for (std::size_t i = 0; i < catalog.physical_inventory().size(); ++i) {
        if (i) out << ',';
        const auto& value = catalog.physical_inventory()[i];
        out << "{\"logical_path\":" << json(value.record.canonical_path)
            << ",\"source_id\":" << json(value.record.source_id)
            << ",\"layer\":" << json(value.record.layer_id)
            << ",\"origin\":" << json(eawr::vfs::to_string(value.record.origin))
            << ",\"active_registry_file\":" << (value.active_registry_file ? "true" : "false")
            << ",\"parsed\":" << (value.parsed ? "true" : "false")
            << ",\"outcome\":" << (value.outcome ? json(*value.outcome) : "null") << '}';
    }
    out << "],\n  \"diagnostics\":[";
    for (std::size_t i = 0; i < diagnostics.size(); ++i) {
        if (i) out << ',';
        write_diagnostic(out, diagnostics[i]);
    }
    out << "],\n  \"unresolved\":[";
    for (std::size_t i = 0; i < unresolved.size(); ++i) {
        if (i) out << ',';
        write_diagnostic(out, unresolved[i]);
    }
    out << "],\n  \"samples\":[";
    for (std::size_t i = 0; i < args->samples.size(); ++i) {
        if (i) out << ',';
        write_sample(out, catalog, mounted.value(), args->samples[i]);
    }
    out << "]\n}\n";
    out.close();

    const auto hard_errors = std::count_if(diagnostics.begin(), diagnostics.end(), [](const auto& value) {
        return value.severity == eawr::core::Severity::error;
    });
    std::cout << "profile=" << eawr::data::to_string(args->profile)
              << " definitions=" << catalog.definitions().size()
              << " diagnostics=" << diagnostics.size()
              << " unresolved=" << unresolved.size() << '\n';
    return hard_errors == 0 && unresolved.empty() ? 0 : 1;
}
