// scene_scan: builds the P1-11 static scene for one map or for every map of a
// profile's effective view, read-only, and reports resolution in JSON. The
// report names logical VFS paths, content hashes and counts only; the host
// installation path is an input and is never written.

#include "eawr/assets/map.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

struct Arguments final {
    std::string profile{"eaw"};
    std::filesystem::path game_root;
    std::string map;
    bool all{};
    std::filesystem::path report;
};

void usage() {
    std::cerr << "Usage: scene_scan [--profile <eaw|foc>] --game-root <GAME_ROOT> "
                 "(--map <logical path> | --all) [--report <file>]\n";
}

std::optional<Arguments> arguments(const int argc, char** argv) {
    Arguments result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view flag = argv[index];
        const auto next = [&]() -> std::optional<std::string> {
            if (index + 1 >= argc) return std::nullopt;
            return std::string(argv[++index]);
        };
        if (flag == "--profile") {
            auto value = next(); if (!value) return std::nullopt; result.profile = *value;
        } else if (flag == "--game-root") {
            auto value = next(); if (!value) return std::nullopt; result.game_root = *value;
        } else if (flag == "--map") {
            auto value = next(); if (!value) return std::nullopt; result.map = *value;
        } else if (flag == "--report") {
            auto value = next(); if (!value) return std::nullopt; result.report = *value;
        } else if (flag == "--all") {
            result.all = true;
        } else {
            return std::nullopt;
        }
    }
    // EaW and FoC are the same builder over a different mount. Remake needs a
    // mod root and is left to a later pass rather than half-supported here.
    if ((result.profile != "eaw" && result.profile != "foc") || result.game_root.empty() ||
        (result.all == !result.map.empty())) {
        return std::nullopt;
    }
    return result;
}

// The effective view each profile reads, highest priority first, with the
// layer IDs xml_scan and asset_validate use. FoC needs both halves of a shared
// install: the expansion shadows base, and base supplies every logical path
// the expansion does not replace.
std::optional<std::vector<std::pair<std::string, std::filesystem::path>>> profile_roots(
    const Arguments& args) {
    const std::filesystem::path base = args.game_root / "GameData" / "Data";
    if (args.profile == "eaw") return std::vector<std::pair<std::string, std::filesystem::path>>{{"base", base}};
    const std::filesystem::path expansion = args.game_root / "corruption" / "Data";
    std::error_code error;
    if (!std::filesystem::is_directory(base, error) || !std::filesystem::is_directory(expansion, error)) {
        return std::nullopt;
    }
    return std::vector<std::pair<std::string, std::filesystem::path>>{{"expansion", expansion}, {"base", base}};
}

std::string json(const std::string_view value) {
    std::ostringstream output;
    output << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        default:
            if (character < 0x20U || character >= 0x7fU) {
                output << "\\u00" << hex[character >> 4U] << hex[character & 15U];
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    output << '"';
    return output.str();
}

std::string sha256_text(const std::string& text) {
    return eawr::sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

// An identity for the effective catalog that names no host path: every active
// game-object winner's id, type and logical source location, in catalog order.
std::string catalog_fingerprint(const eawr::data::Catalog& catalog) {
    std::ostringstream text;
    for (const auto& definition : catalog.definitions()) {
        if (!definition.namespace_winner || definition.category != eawr::data::Category::game_object) continue;
        text << definition.id << '\t' << definition.type_name << '\t'
             << definition.root.source.logical_path << '\t' << definition.root.source.line << '\n';
    }
    return sha256_text(text.str());
}

// The winner fingerprint above deliberately describes definition locations,
// not their values. Bind this inventory separately to the exact active XML
// inputs observed by the catalog loader. Length framing keeps distinct paths,
// outcomes and hashes unambiguous without writing any source bytes or host path.
void identity_field(std::string& bytes, const std::string_view value) {
    const auto length = static_cast<std::uint64_t>(value.size());
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<char>((length >> shift) & 0xffU));
    }
    bytes.append(value);
}

void identity_optional(std::string& bytes, const std::optional<std::string>& value) {
    identity_field(bytes, value ? "present" : "absent");
    if (value) identity_field(bytes, *value);
}

std::string report_logical_path(const std::string_view value) {
    // An authored registry File can be a native path even after the loader adds
    // data/xml/. Preserve that attempt in the input hash, but not in public JSON.
    if (value.starts_with("data/xml/") && value.find(':') == std::string_view::npos &&
        value.find('\\') == std::string_view::npos &&
        value.find("//") == std::string_view::npos &&
        value.find("..") == std::string_view::npos) return std::string(value);
    return "invalid-logical-path:" + sha256_text(std::string(value));
}

std::string catalog_input_fingerprint(const eawr::data::Catalog& catalog) {
    std::string bytes;
    identity_field(bytes, "eawr-scene-catalog-inputs-v1");
    identity_field(bytes, eawr::data::to_string(catalog.profile()));
    for (const auto& root : catalog.registry_roots()) {
        identity_field(bytes, "root");
        identity_field(bytes, eawr::data::to_string(root.category));
        identity_field(bytes, root.registry_path);
        identity_field(bytes, root.outcome);
        identity_optional(bytes, root.input_sha256);
        identity_field(bytes, root.source ? root.source->source_id : "");
        identity_field(bytes, root.source ? root.source->layer_id : "");
    }
    for (const auto& file : catalog.registry_files()) {
        identity_field(bytes, "include");
        identity_field(bytes, eawr::data::to_string(file.category));
        identity_field(bytes, file.registry_path);
        identity_field(bytes, file.included_path);
        identity_field(bytes, std::to_string(file.include_order));
        identity_field(bytes, file.loaded ? "loaded" : "unloaded");
        identity_field(bytes, file.outcome);
        identity_optional(bytes, file.input_sha256);
        identity_field(bytes, file.source ? file.source->source_id : "");
        identity_field(bytes, file.source ? file.source->layer_id : "");
    }
    return sha256_text(bytes);
}

struct MapResult final {
    std::string logical_path;
    std::string sha256;
    std::string failure;
    std::optional<eawr::scene::Scene> scene;
    // Source-basis height range of the terrain samples, for comparing with
    // placement heights in single-map mode.
    std::optional<std::pair<double, double>> terrain_z;
};

MapResult scan_map(const eawr::vfs::Vfs& filesystem, const std::string& logical_path,
                   const eawr::assets::ObjectTypeCatalog& types, const eawr::data::Catalog& catalog,
                   eawr::scene::VfsAssetCache& cache) {
    MapResult result;
    result.logical_path = logical_path;
    auto bytes = filesystem.open(logical_path);
    if (!bytes) {
        result.failure = bytes.error().message;
        return result;
    }
    result.sha256 = eawr::sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.value().data()), bytes.value().size()));
    auto map = eawr::assets::load_map(filesystem, logical_path, types);
    if (!map) {
        result.failure = map.error().message;
        return result;
    }
    eawr::scene::BuildInput input;
    input.map = &map.value();
    input.map_sha256 = result.sha256;
    input.catalog = &catalog;
    input.access = cache.access();
    result.scene = eawr::scene::build(input);
    if (map.value().terrain && !map.value().terrain->samples.empty()) {
        const auto& terrain = *map.value().terrain;
        double low = 1e300, high = -1e300;
        for (const auto& sample : terrain.samples) {
            const double z = static_cast<double>(sample.height_sample) * terrain.height_scale;
            low = std::min(low, z);
            high = std::max(high, z);
        }
        result.terrain_z = std::make_pair(low, high);
    }
    return result;
}

// Record ordinals as ascending runs, "0-3,7,9-12": each group's ordinals are
// already ascending because placements are visited in scene order.
std::string ranges(const std::vector<std::uint32_t>& ordinals) {
    std::ostringstream out;
    for (std::size_t index = 0; index < ordinals.size();) {
        std::size_t end = index;
        while (end + 1 < ordinals.size() && ordinals[end + 1] == ordinals[end] + 1U) ++end;
        if (index != 0) out << ',';
        out << ordinals[index];
        if (end != index) out << '-' << ordinals[end];
        index = end + 1;
    }
    return out.str();
}

void write_counts(std::ostream& out, const std::map<std::string, std::uint64_t>& counts) {
    out << '{';
    bool first = true;
    for (const auto& [name, count] : counts) {
        out << (first ? "" : ", ") << json(name) << ": " << count;
        first = false;
    }
    out << '}';
}

std::map<std::string, std::uint64_t> cause_counts(const eawr::scene::Scene& scene) {
    std::map<std::string, std::uint64_t> counts;
    for (const eawr::scene::Cause cause : eawr::scene::all_causes()) {
        counts.emplace(std::string(eawr::scene::to_string(cause)), scene.count(cause));
    }
    return counts;
}

void write_map(std::ostream& out, const MapResult& result, const bool detail) {
    out << "{\"logical_path\": " << json(result.logical_path)
        << ", \"sha256\": " << json(result.sha256);
    if (!result.scene) {
        out << ", \"status\": \"failed\", \"failure\": " << json(result.failure) << '}';
        return;
    }
    const eawr::scene::Scene& scene = *result.scene;
    out << ", \"status\": \"built\", \"kind\": " << json(scene.map_kind)
        << ", \"scene_sha256\": " << json(scene.scene_sha256)
        << ", \"placements\": " << scene.placements.size()
        << ", \"resolved\": " << scene.resolved_count()
        << ", \"drawable\": " << scene.drawable_count()
        << ", \"unresolved\": " << (scene.placements.size() - scene.resolved_count())
        << ", \"assets\": " << scene.assets.size()
        << ", \"placements_by_cause\": ";
    write_counts(out, cause_counts(scene));
    // One group per distinct (object, cause, detail), with every record
    // ordinal it covers, so each unresolved placement is named without
    // repeating identical rows.
    std::map<std::tuple<std::string, std::string, std::string>, std::vector<std::uint32_t>> groups;
    for (const eawr::scene::Placement& placement : scene.placements) {
        for (const eawr::scene::Issue& issue : placement.issues) {
            std::string object = placement.object_id;
            if (object.empty() && placement.type_crc) {
                std::ostringstream hex;
                hex << "crc:0x" << std::hex;
                hex.width(8);
                hex.fill('0');
                hex << *placement.type_crc;
                object = hex.str();
            }
            groups[{object, std::string(eawr::scene::to_string(issue.cause)), issue.detail}]
                .push_back(placement.record_ordinal);
        }
    }
    out << ", \"unresolved_groups\": [";
    bool first = true;
    for (const auto& [key, ordinals] : groups) {
        out << (first ? "\n      " : ",\n      ") << "{\"object_id\": " << json(std::get<0>(key))
            << ", \"cause\": " << json(std::get<1>(key))
            << ", \"detail\": " << json(std::get<2>(key))
            << ", \"count\": " << ordinals.size()
            << ", \"record_ordinals\": " << json(ranges(ordinals)) << '}';
        first = false;
    }
    out << (groups.empty() ? "]" : "\n    ]");
    if (detail) {
        // Single-map mode also lists what each surface selects, which is what
        // decides the adapters a populated render needs.
        std::map<std::string, std::uint64_t> shaders;
        std::map<std::string, std::uint64_t> effects;
        std::map<std::string, std::uint64_t> idle;
        std::uint64_t scaled{};
        for (const eawr::scene::Placement& placement : scene.placements) {
            for (const auto& surface : placement.surfaces) {
                ++shaders[surface.shader + (surface.supported ? " (supported)" : " (unsupported)")];
            }
            for (const auto& effect : placement.effects) {
                ++effects[effect.resolved.empty() ? "unresolved" : "resolved"];
            }
            ++idle[placement.idle_animation_status];
            if (placement.scale_declared) ++scaled;
        }
        out << ", \"surface_shaders\": ";
        write_counts(out, shaders);
        out << ", \"effects\": ";
        write_counts(out, effects);
        out << ", \"idle_animation\": ";
        write_counts(out, idle);
        out << ", \"scale_declared\": " << scaled;
        double low = 1e300, high = -1e300;
        for (const eawr::scene::Placement& placement : scene.placements) {
            if (!placement.transform) continue;
            const double z = static_cast<double>(placement.transform->position_raw[2])
                / static_cast<double>(eawr::sim::math::Fixed::scale);
            low = std::min(low, z);
            high = std::max(high, z);
        }
        if (low <= high) out << ", \"placement_z\": [" << low << ", " << high << "]";
        if (result.terrain_z) {
            out << ", \"terrain_z\": [" << result.terrain_z->first << ", " << result.terrain_z->second << "]";
        }
    }
    out << '}';
}

} // namespace

int main(const int argc, char** argv) {
    const auto args = arguments(argc, argv);
    if (!args) {
        usage();
        return 2;
    }
    const auto roots = profile_roots(*args);
    if (!roots) {
        std::cerr << "foc requires an installation root containing GameData/Data and corruption/Data\n";
        return 2;
    }
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, root] : *roots) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, root);
        if (!manifest) {
            std::cerr << eawr::core::format_diagnostic(manifest.error()) << '\n';
            return 1;
        }
        specs.push_back(std::move(manifest).value().mount);
    }
    const bool foc = args->profile == "foc";
    const std::string_view view_name = foc ? "FoC" : "EaW";
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    if (!filesystem) {
        std::cerr << eawr::core::format_diagnostic(filesystem.error()) << '\n';
        return 1;
    }
    auto loaded = eawr::data::load_catalog(filesystem.value(), foc ? eawr::data::Profile::foc : eawr::data::Profile::eaw);
    if (!loaded) {
        std::cerr << eawr::core::format_diagnostic(loaded.error()) << '\n';
        return 1;
    }
    const eawr::data::Catalog& catalog = loaded.value().catalog;
    const eawr::assets::ObjectTypeCatalog types = eawr::assets::object_type_catalog(catalog);
    eawr::scene::VfsAssetCache cache(filesystem.value());

    std::vector<std::string> paths;
    if (args->all) {
        auto listed = filesystem.value().enumerate({}, ".ted");
        if (!listed) {
            std::cerr << eawr::core::format_diagnostic(listed.error()) << '\n';
            return 1;
        }
        for (const auto& record : listed.value()) paths.push_back(record.canonical_path);
        std::sort(paths.begin(), paths.end());
    } else {
        paths.push_back(args->map);
    }

    std::vector<MapResult> results;
    results.reserve(paths.size());
    for (const std::string& path : paths) {
        results.push_back(scan_map(filesystem.value(), path, types, catalog, cache));
    }

    std::ostringstream out;
    if (!args->all) {
        write_map(out, results.front(), true);
        out << '\n';
    } else {
        std::uint64_t built{}, placements{}, resolved{}, drawable{};
        std::vector<std::pair<std::string, std::string>> unloaded_roots;
        for (const auto& root : catalog.registry_roots()) {
            if (root.outcome != "loaded") unloaded_roots.emplace_back(root.registry_path, root.outcome);
        }
        std::vector<std::string> unloaded_includes;
        for (const auto& file : catalog.registry_files()) {
            if (!file.loaded) unloaded_includes.push_back(report_logical_path(file.included_path));
        }
        std::map<std::string, std::uint64_t> totals;
        std::map<std::string, std::uint64_t> maps_with;
        for (const MapResult& result : results) {
            if (!result.scene) continue;
            ++built;
            placements += result.scene->placements.size();
            resolved += result.scene->resolved_count();
            drawable += result.scene->drawable_count();
            for (const auto& [name, count] : cause_counts(*result.scene)) {
                totals[name] += count;
                if (count != 0) ++maps_with[name];
                else maps_with.try_emplace(name, 0);
            }
        }
        out << "{\n  \"schema_version\": 2,\n"
            << "  \"generator\": \"scene_scan --profile " << args->profile << " --game-root <GAME_ROOT> --all\",\n"
            << "  \"scene_contract_version\": " << eawr::scene::contract_version << ",\n"
            << "  \"profile\": " << json(args->profile) << ",\n  \"view\": \"effective\",\n"
            << "  \"provenance\": \"Read-only scan of every TED map in the " << view_name << " effective VFS view"
            << (foc ? " (corruption/Data expansion layer over GameData/Data base layer)" : "") << ". "
               "Logical paths, content hashes and counts only; no original bytes and no host paths. "
               "A placement is unresolved when any cause applies; blocking causes prevent drawing, "
               "texture/shader/effect causes leave the placement drawn with that part missing.\",\n"
            << "  \"inputs\": {\"catalog_sha256\": " << json(catalog_fingerprint(catalog))
            << ", \"catalog_input_sha256\": " << json(catalog_input_fingerprint(catalog))
            << ", \"catalog_input_scope\": \"active registry roots and ordered includes observed in the effective "
            << view_name << " VFS; not a complete retail runtime mount proof\""
            << ", \"catalog_game_object_winners\": " << types.entries.size()
            << ", \"legacy_selectors\": " << eawr::scene::legacy_selectors().size()
            << ", \"catalog_provisional\": " << (unloaded_roots.empty() && unloaded_includes.empty() ? "false" : "true")
            << ", \"unloaded_registry_roots\": [";
        for (std::size_t index = 0; index < unloaded_roots.size(); ++index) {
            out << (index == 0 ? "" : ", ") << "{\"logical_path\": " << json(unloaded_roots[index].first)
                << ", \"outcome\": " << json(unloaded_roots[index].second) << '}';
        }
        out << "]"
            << ", \"unloaded_registry_includes\": [";
        for (std::size_t index = 0; index < unloaded_includes.size(); ++index) {
            out << (index == 0 ? "" : ", ") << json(unloaded_includes[index]);
        }
        out << "]},\n"
            << "  \"blocking_causes\": [";
        bool first = true;
        for (const eawr::scene::Cause cause : eawr::scene::all_causes()) {
            if (!eawr::scene::blocks_drawing(cause)) continue;
            out << (first ? "" : ", ") << json(eawr::scene::to_string(cause));
            first = false;
        }
        out << "],\n  \"totals\": {\"maps_attempted\": " << results.size()
            << ", \"maps_built\": " << built << ", \"placements\": " << placements
            << ", \"resolved\": " << resolved << ", \"drawable\": " << drawable
            << ", \"unresolved\": " << (placements - resolved)
            << ",\n    \"placements_by_cause\": ";
        write_counts(out, totals);
        out << ",\n    \"maps_with_cause\": ";
        write_counts(out, maps_with);
        out << "},\n  \"maps\": [";
        for (std::size_t index = 0; index < results.size(); ++index) {
            out << (index == 0 ? "\n    " : ",\n    ");
            write_map(out, results[index], false);
        }
        out << "\n  ]\n}\n";
    }

    if (args->report.empty()) {
        std::cout << out.str();
    } else {
        std::ofstream file(args->report, std::ios::binary | std::ios::trunc);
        file << out.str();
        if (!file) {
            std::cerr << "report could not be written\n";
            return 1;
        }
    }
    return 0;
}
