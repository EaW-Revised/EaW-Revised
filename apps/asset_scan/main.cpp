#include "eawr/core/diagnostic.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Arguments {
    std::string profile;
    std::filesystem::path game_root;
    std::optional<std::filesystem::path> mod_root;
    std::filesystem::path report;
};

struct Layer {
    std::string id;
    std::filesystem::path data_root;
    eawr::vfs::ManifestResolution manifest;
};

struct ProbedArchive {
    std::string layer_id;
    std::string source_id;
    bool active{false};
    std::optional<eawr::vfs::ArchiveProbe> probe;
    std::optional<eawr::core::Diagnostic> error;
};

void usage(std::ostream& output) {
    output << "Usage: asset_scan --profile <eaw|foc|remake> --game-root <dir> "
              "[--mod-root <leaf;parent;...>] --report <json>\n";
}

std::optional<Arguments> parse_arguments(const int argc, char** argv) {
    Arguments result;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option(argv[i]);
        if (option == "--help") {
            usage(std::cout);
            return std::nullopt;
        }
        if (i + 1 >= argc) return std::nullopt;
        const std::string value(argv[++i]);
        if (option == "--profile") result.profile = value;
        else if (option == "--game-root") result.game_root = std::filesystem::path(value);
        else if (option == "--mod-root") result.mod_root = std::filesystem::path(value);
        else if (option == "--report") result.report = std::filesystem::path(value);
        else return std::nullopt;
    }
    if ((result.profile != "eaw" && result.profile != "foc" && result.profile != "remake") ||
        result.game_root.empty() || result.report.empty() || (result.profile == "remake" && !result.mod_root)) {
        return std::nullopt;
    }
    return result;
}

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string lower_ascii(std::string value) {
    for (char& ch : value) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    return value;
}

bool iequals(const std::string_view left, const std::string_view right) {
    return lower_ascii(std::string(left)) == lower_ascii(std::string(right));
}

std::string native_key(const std::filesystem::path& path) {
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec) normalized = path.lexically_normal();
    return lower_ascii(utf8(normalized));
}

std::filesystem::path as_data_root(const std::filesystem::path& root) {
    if (iequals(utf8(root.filename()), "data")) return root;
    if (std::filesystem::is_directory(root / "Data")) return root / "Data";
    return root;
}

std::optional<std::vector<std::pair<std::string, std::filesystem::path>>> profile_roots(
    const Arguments& arguments
) {
    std::vector<std::pair<std::string, std::filesystem::path>> roots;
    const auto direct_data = as_data_root(arguments.game_root);
    const bool full_install = std::filesystem::is_directory(arguments.game_root / "GameData" / "Data") &&
                              std::filesystem::is_directory(arguments.game_root / "corruption" / "Data");
    if (arguments.profile == "eaw") {
        roots.emplace_back("base", full_install ? arguments.game_root / "GameData" / "Data" : direct_data);
    } else {
        if (!full_install) return std::nullopt;
        if (arguments.profile == "remake") {
            for (const auto& layer : eawr::vfs::mod_chain_roots(*arguments.mod_root)) roots.push_back(layer);
        }
        roots.emplace_back("expansion", arguments.game_root / "corruption" / "Data");
        roots.emplace_back("base", arguments.game_root / "GameData" / "Data");
    }
    return roots;
}

std::string json_string(const std::string_view value) {
    std::ostringstream out;
    out << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20U) out << "\\u00" << hex[ch >> 4U] << hex[ch & 0x0fU];
            else out << static_cast<char>(ch);
        }
    }
    out << '"';
    return out.str();
}

void write_record(std::ostream& out, const eawr::vfs::AssetRecord& record) {
    out << "{\"path\":" << json_string(record.canonical_path)
        << ",\"original_path\":" << json_string(record.original_path)
        << ",\"layer\":" << json_string(record.layer_id)
        << ",\"origin\":" << json_string(eawr::vfs::to_string(record.origin))
        << ",\"size\":" << record.size
        << ",\"source_id\":" << json_string(record.source_id) << '}';
}

void write_diagnostic(std::ostream& out, const eawr::core::Diagnostic& diagnostic) {
    out << "{\"code\":" << json_string(diagnostic.code)
        << ",\"message\":" << json_string(diagnostic.message);
    if (diagnostic.logical_path) out << ",\"logical_path\":" << json_string(*diagnostic.logical_path);
    if (diagnostic.source_id) out << ",\"source_id\":" << json_string(*diagnostic.source_id);
    out << '}';
}

} // namespace

int main(const int argc, char** argv) {
    const auto arguments = parse_arguments(argc, argv);
    if (!arguments) {
        if (argc == 2 && std::string_view(argv[1]) == "--help") return 0;
        usage(std::cerr);
        return 2;
    }
    const auto roots = profile_roots(*arguments);
    if (!roots) {
        std::cerr << "foc/remake profiles require --game-root to be the installation root containing GameData/Data and corruption/Data\n";
        return 2;
    }

    std::vector<Layer> layers;
    std::vector<eawr::core::Diagnostic> errors;
    auto manifest_chain = eawr::vfs::resolve_manifest_chain(*roots);
    if (!manifest_chain) {
        // Keep the scan's partial-report contract when one layer is invalid.
        for (const auto& [id, root] : *roots) {
            auto manifest = eawr::vfs::resolve_manifest_mount(id, root);
            if (!manifest) errors.push_back(manifest.error());
            else layers.push_back({id, root, std::move(manifest.value())});
        }
        if (errors.empty()) errors.push_back(manifest_chain.error());
    } else {
        for (auto& manifest : manifest_chain.value()) {
            layers.push_back({manifest.mount.layer_id, manifest.mount.data_root, std::move(manifest)});
        }
    }

    std::vector<ProbedArchive> probed;
    for (const auto& [layer_id, root] : *roots) {
        std::set<std::string> active;
        const auto layer = std::find_if(layers.begin(), layers.end(), [&](const Layer& item) { return item.id == layer_id; });
        if (layer != layers.end()) {
            for (const auto& archive : layer->manifest.mount.active_archives) active.insert(native_key(archive.path));
        }
        std::vector<std::filesystem::path> discovered;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(
                 root, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code type_error;
            if (it->is_regular_file(type_error) && !type_error && iequals(utf8(it->path().extension()), ".meg")) {
                discovered.push_back(it->path());
            }
        }
        if (ec) {
            errors.push_back(eawr::core::Diagnostic{
                .code = std::string(eawr::vfs::diagnostic_codes::native_io),
                .severity = eawr::core::Severity::error,
                .message = "cannot enumerate archive probe set",
                .logical_path = std::nullopt,
                .line = std::nullopt,
                .column = std::nullopt,
                .source_id = layer_id,
            });
        }
        std::sort(discovered.begin(), discovered.end(), [&](const auto& a, const auto& b) {
            return lower_ascii(utf8(a.lexically_relative(root))) < lower_ascii(utf8(b.lexically_relative(root)));
        });
        for (const auto& archive : discovered) {
            const auto relative = utf8(archive.lexically_relative(root));
            const auto source_id = layer_id + ":Data/" + relative;
            auto probe = eawr::vfs::probe_meg_archive(archive, source_id);
            ProbedArchive row{layer_id, source_id, active.contains(native_key(archive)), std::nullopt, std::nullopt};
            if (probe) row.probe = std::move(probe.value());
            else {
                row.error = probe.error();
                errors.push_back(probe.error());
            }
            probed.push_back(std::move(row));
        }
    }

    std::vector<eawr::vfs::MountSpec> mount_specs;
    for (const auto& layer : layers) mount_specs.push_back(layer.manifest.mount);
    auto mounted = eawr::vfs::Vfs::mount(mount_specs);
    std::vector<eawr::vfs::AssetRecord> raw;
    std::vector<eawr::vfs::AssetRecord> effective;
    if (!mounted) errors.push_back(mounted.error());
    else {
        auto raw_result = mounted.value().enumerate_raw();
        auto effective_result = mounted.value().enumerate();
        if (!raw_result) errors.push_back(raw_result.error()); else raw = std::move(raw_result.value());
        if (!effective_result) errors.push_back(effective_result.error()); else effective = std::move(effective_result.value());
    }

    std::map<std::string, std::uint64_t> raw_xml_by_layer;
    std::map<std::string, std::uint64_t> loose_xml_by_layer;
    for (const auto& record : raw) {
        if (record.canonical_path.ends_with(".xml")) {
            ++raw_xml_by_layer[record.layer_id];
            if (record.origin == eawr::vfs::AssetOrigin::loose) ++loose_xml_by_layer[record.layer_id];
        }
    }
    std::uint64_t effective_xml = 0;
    for (const auto& record : effective) if (record.canonical_path.ends_with(".xml")) ++effective_xml;

    std::filesystem::create_directories(arguments->report.parent_path().empty()
                                             ? std::filesystem::path(".")
                                             : arguments->report.parent_path());
    std::ofstream report(arguments->report, std::ios::binary);
    if (!report) {
        std::cerr << "cannot create report\n";
        return 2;
    }
    report << "{\n  \"schema_version\": 1,\n  \"profile\": " << json_string(arguments->profile)
           << ",\n  \"precedence\": \"layers are highest-first; loose precedes active archives per layer; active archives are manifest/SFX then Patch, Patch2, 64Patch with later archives winning\",\n";
    report << "  \"layers\": [";
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (i) report << ',';
        const auto& layer = layers[i];
        report << "{\"id\":" << json_string(layer.id)
               << ",\"root_alias\":" << json_string(layer.id + ":Data")
               << ",\"manifest\":" << json_string(layer.manifest.manifest_source_id)
               << ",\"declared_archives\":[";
        for (std::size_t j = 0; j < layer.manifest.declared_archives.size(); ++j) {
            if (j) report << ',';
            report << json_string(layer.manifest.declared_archives[j]);
        }
        report << "],\"missing_declared_archives\":[";
        for (std::size_t j = 0; j < layer.manifest.missing_archives.size(); ++j) {
            if (j) report << ',';
            report << json_string(layer.manifest.missing_archives[j]);
        }
        report << "],\"active_archives\":[";
        for (std::size_t j = 0; j < layer.manifest.mount.active_archives.size(); ++j) {
            if (j) report << ',';
            report << json_string(layer.manifest.mount.active_archives[j].source_id);
        }
        report << "]}";
    }
    report << "],\n  \"archive_probes\": [";
    for (std::size_t i = 0; i < probed.size(); ++i) {
        if (i) report << ',';
        const auto& row = probed[i];
        report << "{\"layer\":" << json_string(row.layer_id)
               << ",\"source_id\":" << json_string(row.source_id)
               << ",\"active\":" << (row.active ? "true" : "false");
        if (row.probe) report << ",\"format\":" << json_string(row.probe->format)
                              << ",\"archive_size\":" << row.probe->archive_size
                              << ",\"filename_count\":" << row.probe->filename_count
                              << ",\"entry_count\":" << row.probe->entry_count;
        if (row.error) {
            report << ",\"error\":";
            write_diagnostic(report, *row.error);
        }
        report << '}';
    }
    report << "],\n  \"counts\": {\"raw_records\":" << raw.size()
           << ",\"effective_records\":" << effective.size()
           << ",\"effective_xml\":" << effective_xml
           << ",\"raw_xml_by_layer\":{";
    bool first = true;
    for (const auto& [layer, count] : raw_xml_by_layer) {
        if (!first) report << ',';
        first = false;
        report << json_string(layer) << ':' << count;
    }
    report << "},\"loose_xml_by_layer\":{";
    first = true;
    for (const auto& [layer, count] : loose_xml_by_layer) {
        if (!first) report << ',';
        first = false;
        report << json_string(layer) << ':' << count;
    }
    report << "}},\n  \"winners\": [";
    for (std::size_t i = 0; i < effective.size(); ++i) {
        if (i) report << ',';
        write_record(report, effective[i]);
    }
    report << "],\n  \"shadowed\": [";
    bool first_shadow = true;
    if (mounted) {
        for (const auto& winner : effective) {
            auto chain = mounted.value().candidates(winner.canonical_path);
            if (!chain) continue;
            for (std::size_t i = 1; i < chain.value().size(); ++i) {
                if (!first_shadow) report << ',';
                first_shadow = false;
                report << "{\"winner_path\":" << json_string(winner.canonical_path) << ",\"record\":";
                write_record(report, chain.value()[i]);
                report << '}';
            }
        }
    }
    report << "],\n  \"errors\": [";
    for (std::size_t i = 0; i < errors.size(); ++i) {
        if (i) report << ',';
        write_diagnostic(report, errors[i]);
    }
    report << "]\n}\n";
    report.close();

    std::cout << "profile=" << arguments->profile << " archives=" << probed.size()
              << " raw=" << raw.size() << " effective=" << effective.size()
              << " effective_xml=" << effective_xml << " errors=" << errors.size() << '\n';
    return errors.empty() ? 0 : 1;
}
