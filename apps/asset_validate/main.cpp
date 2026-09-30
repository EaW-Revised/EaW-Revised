#include "asset_validate_internal.hpp"
#include "eawr/assets/assets.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/presentation/lighting/lighting.hpp"
#include "eawr/presentation/terrain/terrain.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace asset_validate_internal {

void usage() {
    std::cerr
        << "Usage: asset_validate --profile <eaw|foc|remake> [--view <effective|raw>] "
           "--game-root <dir> [--mod-root <leaf;parent;...>] "
           "[--only-format <alo|ala|dds|tga|ted>] "
           "(--report <json> | --summary <json> --failures <json> | "
           "--inspect-model <logical-path> | --inspect-texture <logical-path> | "
           "--inspect-map <logical-path>)\n";
}

std::optional<Arguments> arguments(const int argc, char** argv) {
    Arguments result;
    for (int index = 1; index < argc; ++index) {
        if (index + 1 >= argc) return std::nullopt;
        const std::string_view key(argv[index]);
        const std::filesystem::path value(argv[++index]);
        if (key == "--profile") result.profile = value.string();
        else if (key == "--view") result.view = value.string();
        else if (key == "--game-root") result.game_root = value;
        else if (key == "--mod-root") result.mod_root = value;
        else if (key == "--report") result.report = value;
        else if (key == "--summary") result.summary = value;
        else if (key == "--failures") result.failures = value;
        else if (key == "--inspect-model") result.inspect_model = value.generic_string();
        else if (key == "--inspect-texture") result.inspect_texture = value.generic_string();
        else if (key == "--inspect-map") result.inspect_map = value.generic_string();
        else if (key == "--only-format") result.only_format = value.string();
        else return std::nullopt;
    }

    const bool profile_ok = result.profile == "eaw" || result.profile == "foc" || result.profile == "remake";
    const bool view_ok = result.view == "effective" || result.view == "raw";
    const bool no_inspect = result.inspect_model.empty() && result.inspect_texture.empty()
        && result.inspect_map.empty();
    const bool no_output = result.report.empty() && result.summary.empty() && result.failures.empty();
    const bool report_mode = !result.report.empty() && result.summary.empty() && result.failures.empty()
        && no_inspect;
    const bool legacy_mode = result.report.empty() && !result.summary.empty() && !result.failures.empty()
        && no_inspect;
    const bool inspect_model_mode = no_output
        && !result.inspect_model.empty() && result.inspect_texture.empty() && result.inspect_map.empty();
    const bool inspect_texture_mode = no_output
        && result.inspect_model.empty() && !result.inspect_texture.empty() && result.inspect_map.empty();
    const bool inspect_map_mode = no_output
        && result.inspect_model.empty() && result.inspect_texture.empty() && !result.inspect_map.empty();
    const bool format_ok = result.only_format.empty() || result.only_format == "alo" || result.only_format == "ala" ||
        result.only_format == "dds" || result.only_format == "tga" || result.only_format == "ted";
    if (!profile_ok || !view_ok || !format_ok || result.game_root.empty()
        || !(report_mode || legacy_mode || inspect_model_mode || inspect_texture_mode || inspect_map_mode)
        || (result.profile == "remake" && result.mod_root.empty())) {
        return std::nullopt;
    }
    return result;
}

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string lower(std::string value) {
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
    }
    return value;
}

bool ieq(const std::string_view left, const std::string_view right) {
    return lower(std::string(left)) == lower(std::string(right));
}

std::filesystem::path data_root(const std::filesystem::path& root) {
    if (ieq(utf8(root.filename()), "data")) return root;
    if (std::filesystem::is_directory(root / "Data")) return root / "Data";
    return root;
}

std::optional<std::vector<std::pair<std::string, std::filesystem::path>>> roots(const Arguments& args) {
    std::vector<std::pair<std::string, std::filesystem::path>> result;
    const bool install = std::filesystem::is_directory(args.game_root / "GameData" / "Data")
        && std::filesystem::is_directory(args.game_root / "corruption" / "Data");
    if (args.profile == "eaw") {
        result.emplace_back("base", install ? args.game_root / "GameData" / "Data" : data_root(args.game_root));
    } else {
        if (!install) return std::nullopt;
        if (args.profile == "remake") {
            for (const auto& layer : eawr::vfs::mod_chain_roots(args.mod_root)) result.push_back(layer);
        }
        result.emplace_back("expansion", args.game_root / "corruption" / "Data");
        result.emplace_back("base", args.game_root / "GameData" / "Data");
    }
    return result;
}

// Authored asset names are arbitrary bytes, not guaranteed UTF-8: a TED
// mini-record whose payload only looks like a string decodes to whatever was
// there.  Emitting such a byte raw would make the whole report invalid JSON, so
// every byte outside printable ASCII is escaped as its Latin-1 code point,
// which keeps the report valid UTF-8 and round-trips the byte exactly.
} // namespace asset_validate_internal

using namespace asset_validate_internal;

namespace {

using RawSources = std::map<std::string, std::unique_ptr<eawr::vfs::Vfs>>;
using ProbeCache = std::map<std::string, bool, std::less<>>;

eawr::data::Profile data_profile(const std::string& profile) {
    return profile == "eaw" ? eawr::data::Profile::eaw :
        profile == "foc" ? eawr::data::Profile::foc : eawr::data::Profile::remake;
}

std::string json_names(const std::vector<std::string>& values) {
    std::ostringstream listed;
    listed << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) listed << ',';
        listed << json(values[index]);
    }
    listed << ']';
    return listed.str();
}

std::string json_type_crcs(const std::vector<std::uint32_t>& values) {
    std::ostringstream crcs;
    crcs << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) crcs << ',';
        std::ostringstream hex;
        hex << "0x" << std::hex << std::setw(8) << std::setfill('0') << values[index];
        crcs << json(hex.str());
    }
    crcs << ']';
    return crcs.str();
}

int inspect_model(const eawr::vfs::Vfs& mounted, const std::string& logical_path) {
    auto model = eawr::assets::load_model(mounted, logical_path);
    if (!model) {
        std::cerr << eawr::core::format_diagnostic(model.error()) << '\n';
        return 1;
    }
    std::cout << "{\"path\":" << json(model.value().source.logical_path) << ",\"meshes\":[";
    bool first_mesh = true;
    for (const auto& mesh : model.value().meshes) {
        if (!first_mesh) std::cout << ',';
        first_mesh = false;
        std::cout << "{\"name\":" << json(mesh.name) << ",\"submeshes\":[";
        bool first_submesh = true;
        for (const auto& submesh : mesh.submeshes) {
            if (!first_submesh) std::cout << ',';
            first_submesh = false;
            std::cout << "{\"shader\":" << json(submesh.shader) << ",\"parameters\":[";
            bool first_parameter = true;
            for (const auto& parameter : submesh.parameters) {
                if (!first_parameter) std::cout << ',';
                first_parameter = false;
                std::cout << "{\"name\":" << json(parameter.name)
                    << ",\"kind\":" << static_cast<unsigned>(parameter.kind);
                if (const auto* texture = std::get_if<std::string>(&parameter.value)) {
                    std::cout << ",\"texture\":" << json(*texture);
                }
                std::cout << '}';
            }
            std::cout << "]}";
        }
        std::cout << "]}";
    }
    std::cout << "]}\n";
    return 0;
}

int inspect_texture(const eawr::vfs::Vfs& mounted, const std::string& logical_path) {
    auto texture = eawr::assets::load_texture(mounted, logical_path);
    if (!texture) {
        std::cerr << eawr::core::format_diagnostic(texture.error()) << '\n';
        return 1;
    }
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto& mip : texture.value().mips) {
        for (const auto byte : mip.bytes) {
            hash ^= std::to_integer<std::uint8_t>(byte);
            hash *= 1099511628211ULL;
        }
    }
    std::cout << "{\"path\":" << json(texture.value().source.logical_path)
        << ",\"width\":" << texture.value().width
        << ",\"height\":" << texture.value().height
        << ",\"format\":" << json(eawr::assets::to_string(texture.value().format))
        << ",\"mips\":" << texture.value().mips.size()
        << ",\"alpha\":" << (texture.value().has_alpha ? "true" : "false")
        << ",\"payload_fnv1a64\":" << hash
        << ",\"notices\":" << texture.value().notices.size() << "}\n";
    return 0;
}

int inspect_map(const eawr::vfs::Vfs& mounted, const Arguments& args) {
    eawr::assets::ObjectTypeCatalog map_catalog;
    bool catalog_loaded = false;
    if (auto catalog = eawr::data::load_catalog(mounted, data_profile(args.profile))) {
        map_catalog = eawr::assets::object_type_catalog(catalog.value().catalog);
        for (const auto& root : catalog.value().catalog.registry_roots()) {
            if (root.category == eawr::data::Category::game_object && root.outcome == "loaded") {
                catalog_loaded = true;
                break;
            }
        }
    }
    auto record = mounted.stat(args.inspect_map);
    if (!record) {
        std::cerr << eawr::core::format_diagnostic(record.error()) << '\n';
        return 1;
    }
    auto bytes = mounted.open(args.inspect_map);
    if (!bytes) {
        std::cerr << eawr::core::format_diagnostic(bytes.error()) << '\n';
        return 1;
    }
    auto loaded = eawr::assets::load_map(
        bytes.value(), eawr::assets::source_from(record.value()), map_catalog);
    if (!loaded) {
        std::cerr << eawr::core::format_diagnostic(loaded.error()) << '\n';
        return 1;
    }
    const eawr::assets::AssetProbe inspect_probe = [&mounted](const std::string_view logical_path) {
        return mounted.stat(logical_path).operator bool();
    };
    const auto& map = loaded.value();
    const auto resolution = eawr::assets::resolve_map(map, map_catalog, inspect_probe);
    std::cout << "{\"logical_path\":" << json(map.source.logical_path)
        << ",\"sha256\":" << json(sha256_hex(bytes.value()))
        << ",\"stored_size\":" << record.value().size
        << ",\"kind\":" << json(!map.kind ? "unknown"
            : *map.kind == eawr::assets::MapKind::land ? "land" : "space")
        << ",\"semantic_complete\":" << (map.semantic_complete ? "true" : "false")
        << ",\"terrain_width\":" << (map.terrain ? map.terrain->width : 0U)
        << ",\"terrain_height\":" << (map.terrain ? map.terrain->height : 0U)
        << ",\"terrain_samples\":" << (map.terrain ? map.terrain->samples.size() : 0U)
        << ",\"terrain_materials\":" << (map.terrain ? map.terrain->materials.size() : 0U)
        << ",\"environments\":" << map.environments.size()
        << ",\"water_records\":" << map.water_records.size()
        << ",\"placements\":" << resolution.placements
        << ",\"placements_crc_absent\":" << resolution.crc_absent
        << ",\"placements_crc_missing\":" << resolution.crc_missing
        << ",\"placements_crc_collision\":" << resolution.crc_collision
        << ",\"placements_crc_unique\":" << resolution.crc_unique
        << ",\"placements_model_renderable\":" << resolution.model_chain_renderable
        << ",\"placements_model_unresolved\":" << resolution.model_chain_unresolved
        << ",\"placements_model_undeclared\":" << resolution.model_chain_undeclared
        << ",\"placements_model_unknown\":" << resolution.model_chain_unknown
        << ",\"texture_references\":" << resolution.texture_references
        << ",\"textures_resolved\":" << resolution.textures_resolved
        << ",\"sky_references\":" << resolution.sky_references
        << ",\"skies_resolved\":" << resolution.skies_resolved
        << ",\"skies_model_renderable\":" << resolution.skies_model_renderable
        << ",\"notices\":" << map.notices.size()
        << ",\"semantics\":" << map_semantics(map)
        << ",\"unresolved_references\":{";
    std::cout << "\"placement_type_crcs\":" << json_type_crcs(resolution.unresolved_type_crcs)
        << ",\"model_names\":" << json_names(resolution.unresolved_model_names)
        << ",\"texture_names\":" << json_names(resolution.unresolved_texture_names)
        << ",\"sky_object_ids\":" << json_names(resolution.unresolved_sky_ids)
        << "},\"environment_reference_ledger\":";
    write_environment_reference_ledger(std::cout, resolution, catalog_loaded);
    std::cout << ",\"source_bounds_ledger\":";
    write_source_bounds_ledger(std::cout, map);
    std::cout << ",\"environment_candidate_ledger\":";
    write_environment_candidate_ledger(std::cout, map);
    std::cout << "}\n";
    return 0;
}

// A raw record is read from a VFS that mounts only its own source: the loose
// tree of its layer, or its one archive over an empty data root.
eawr::core::Result<eawr::vfs::Vfs> mount_raw_source(
    const std::vector<Layer>& layers, const eawr::vfs::AssetRecord& record) {
    const auto layer = std::find_if(layers.begin(), layers.end(), [&record](const Layer& value) {
        return value.id == record.layer_id;
    });
    if (layer == layers.end()) {
        return eawr::core::Result<eawr::vfs::Vfs>::failure(eawr::core::Diagnostic{
            .code="EAWR-ASSET-CLI-0001", .severity=eawr::core::Severity::error,
            .message="raw layer missing", .logical_path=record.canonical_path,
            .line=std::nullopt, .column=std::nullopt, .source_id=record.source_id});
    }
    eawr::vfs::MountSpec isolated;
    isolated.layer_id = record.layer_id;
    isolated.loose_logical_prefix = layer->manifest.mount.loose_logical_prefix;
    if (record.origin == eawr::vfs::AssetOrigin::loose) {
        isolated.data_root = layer->root;
    } else {
        // A raw archive member must not be hidden by a same-name loose
        // file. Vfs requires an existing data root, so use an empty
        // scratch directory rather than a deliberately missing path.
        std::error_code scratch_error;
        const auto scratch = std::filesystem::temp_directory_path(scratch_error)
            / "eawr-asset-validate-empty-vfs-root";
        if (scratch_error || !std::filesystem::create_directories(scratch, scratch_error)) {
            if (scratch_error) return eawr::core::Result<eawr::vfs::Vfs>::failure(eawr::core::Diagnostic{
                .code="EAWR-ASSET-CLI-0003", .severity=eawr::core::Severity::error,
                .message="cannot create isolated raw VFS root", .logical_path=record.canonical_path,
                .line=std::nullopt, .column=std::nullopt, .source_id=record.source_id});
        }
        isolated.data_root = scratch;
        const auto archive = std::find_if(
            layer->manifest.mount.active_archives.begin(),
            layer->manifest.mount.active_archives.end(),
            [&record](const eawr::vfs::ArchiveSpec& value) { return value.source_id == record.source_id; });
        if (archive == layer->manifest.mount.active_archives.end()) {
            return eawr::core::Result<eawr::vfs::Vfs>::failure(eawr::core::Diagnostic{
                .code="EAWR-ASSET-CLI-0002", .severity=eawr::core::Severity::error,
                .message="raw archive is not active", .logical_path=record.canonical_path,
                .line=std::nullopt, .column=std::nullopt, .source_id=record.source_id});
        }
        isolated.active_archives.push_back(*archive);
    }
    const std::array one{isolated};
    return eawr::vfs::Vfs::mount(one);
}

// The exact bytes of one listed record: through the effective mount in the
// effective view, through the record's own isolated source in the raw view.
eawr::core::Result<std::vector<std::byte>> open_exact(
    const Arguments& args, const std::vector<Layer>& layers, const eawr::vfs::Vfs& mounted,
    RawSources& raw_sources, const eawr::vfs::AssetRecord& record) {
    if (args.view != "raw") return mounted.open(record.canonical_path);
    const std::string source_key = record.origin == eawr::vfs::AssetOrigin::loose
        ? record.layer_id + ":loose" : record.source_id;
    auto found = raw_sources.find(source_key);
    if (found == raw_sources.end()) {
        auto filesystem = mount_raw_source(layers, record);
        if (!filesystem) return eawr::core::Result<std::vector<std::byte>>::failure(filesystem.error());
        found = raw_sources.emplace(
            source_key, std::make_unique<eawr::vfs::Vfs>(std::move(filesystem.value()))).first;
    }
    return found->second->open(record.canonical_path);
}

bool probe_effective(const eawr::vfs::Vfs& mounted, ProbeCache& probe_cache, const std::string_view logical_path) {
    const auto cached = probe_cache.find(logical_path);
    if (cached != probe_cache.end()) return cached->second;
    const bool present = mounted.stat(logical_path).operator bool();
    probe_cache.emplace(std::string(logical_path), present);
    return present;
}

AssetOutcome validate_record(
    const eawr::vfs::AssetRecord& record, const std::string& format,
    const eawr::core::Result<std::vector<std::byte>>& bytes, const eawr::assets::ObjectTypeCatalog& type_catalog,
    const eawr::assets::AssetProbe& probe, UnresolvedLedger& ledger, FamilyLedger& families) {
    AssetOutcome outcome;
    outcome.source = record;
    outcome.format = format;
    if (!bytes) {
        outcome.diagnostic = bytes.error();
        outcome.affected_feature = affected_feature({}, bytes.error());
        outcome.follow_up = follow_up({}, bytes.error());
        return outcome;
    }
    outcome.content_sha256 = sha256_hex(bytes.value());
    const auto source = eawr::assets::source_from(record);
    if (format == "alo") {
        auto loaded = eawr::assets::load_model(bytes.value(), source);
        if (loaded) {
            outcome.loaded = true;
            outcome.counts = model_counts(loaded.value());
            outcome.notices = loaded.value().notices.size();
        } else outcome.diagnostic = loaded.error();
    } else if (format == "ala") {
        auto loaded = eawr::assets::load_animation(bytes.value(), source);
        if (loaded) {
            outcome.loaded = true;
            outcome.counts = animation_counts(loaded.value());
            outcome.notices = loaded.value().notices.size();
        } else outcome.diagnostic = loaded.error();
    } else if (format == "ted") {
        auto loaded = eawr::assets::load_map(bytes.value(), source, type_catalog);
        if (loaded) {
            outcome.loaded = true;
            outcome.counts = map_counts(loaded.value(), type_catalog, probe, ledger);
            record_families(loaded.value(), type_catalog, probe, families);
            outcome.notices = loaded.value().notices.size();
        } else outcome.diagnostic = loaded.error();
    } else {
        auto loaded = eawr::assets::load_texture(bytes.value(), source);
        if (loaded) {
            outcome.loaded = true;
            outcome.counts = texture_counts(loaded.value());
            outcome.notices = loaded.value().notices.size();
        } else outcome.diagnostic = loaded.error();
    }
    if (outcome.diagnostic) {
        outcome.affected_feature = affected_feature(format, *outcome.diagnostic);
        outcome.follow_up = follow_up(format, *outcome.diagnostic);
    }
    return outcome;
}

int validate_corpus(const Arguments& args, const std::vector<Layer>& layers, const eawr::vfs::Vfs& mounted) {
    auto listed = args.view == "raw" ? mounted.enumerate_raw() : mounted.enumerate();
    if (!listed) {
        std::cerr << eawr::core::format_diagnostic(listed.error()) << '\n';
        return 1;
    }
    RawSources raw_sources;
    eawr::assets::ObjectTypeCatalog type_catalog;
    auto loaded_catalog = eawr::data::load_catalog(mounted, data_profile(args.profile));
    if (loaded_catalog) type_catalog = eawr::assets::object_type_catalog(loaded_catalog.value().catalog);

    // References are always probed against the effective mount, in both views:
    // a raw-view accounting question is still "would the running game find this
    // asset", not "is it inside this one archive".  Results are memoised
    // because the same type recurs across hundreds of maps.
    ProbeCache probe_cache;
    const eawr::assets::AssetProbe probe = [&mounted, &probe_cache](const std::string_view logical_path) {
        return probe_effective(mounted, probe_cache, logical_path);
    };

    UnresolvedLedger ledger;
    FamilyLedger families;
    std::vector<AssetOutcome> outcomes;
    for (const auto& record : listed.value()) {
        const auto format = format_of(record.canonical_path);
        if (format.empty()) continue;
        if (!args.only_format.empty() && format != args.only_format) continue;
        // The historical 4,140/5,603 baseline is the raw Remake mod layer's
        // model/animation population, not inherited base assets or textures.
        if (args.view == "raw" && args.profile == "remake" && args.only_format.empty()
            && (record.layer_id != "mod" || (format != "alo" && format != "ala"))) continue;

        const auto bytes = open_exact(args, layers, mounted, raw_sources, record);
        outcomes.push_back(validate_record(record, format, bytes, type_catalog, probe, ledger, families));
    }
    std::sort(outcomes.begin(), outcomes.end(), [](const AssetOutcome& left, const AssetOutcome& right) {
        return std::tie(left.source.canonical_path, left.source.source_id, left.format)
            < std::tie(right.source.canonical_path, right.source.source_id, right.format);
    });

    const auto aggregates = aggregate(outcomes);
    bool wrote = true;
    if (!args.report.empty()) {
        wrote = write_file(
            args.report, complete_report(args, outcomes, aggregates, ledger, families));
    } else {
        wrote = write_file(args.summary, legacy_summary(args, aggregates));
        wrote = write_file(args.failures, legacy_failures(args, outcomes)) && wrote;
    }
    std::uint64_t failure_count = 0;
    for (const auto& outcome : outcomes) failure_count += outcome.loaded ? 0U : 1U;
    std::cout << "validated " << outcomes.size() << " assets; " << failure_count << " failed\n";
    if (!wrote) return 1;
    return failure_count == 0 ? 0 : 1;
}

} // namespace

int main(const int argc, char** argv) {
    const auto args = arguments(argc, argv);
    if (!args) {
        usage();
        return 2;
    }
    const auto configured = roots(*args);
    if (!configured) {
        std::cerr << "foc/remake require an installation root\n";
        return 2;
    }

    std::vector<Layer> layers;
    auto chain = eawr::vfs::resolve_manifest_chain(*configured);
    if (!chain) {
        std::cerr << eawr::core::format_diagnostic(chain.error()) << '\n';
        return 1;
    }
    for (auto& manifest : chain.value()) {
        layers.push_back({manifest.mount.layer_id, manifest.mount.data_root, std::move(manifest)});
    }
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& layer : layers) specs.push_back(layer.manifest.mount);
    auto mounted = eawr::vfs::Vfs::mount(specs);
    if (!mounted) {
        std::cerr << eawr::core::format_diagnostic(mounted.error()) << '\n';
        return 1;
    }

    if (!args->inspect_model.empty()) return inspect_model(mounted.value(), args->inspect_model);
    if (!args->inspect_texture.empty()) return inspect_texture(mounted.value(), args->inspect_texture);
    if (!args->inspect_map.empty()) return inspect_map(mounted.value(), *args);
    return validate_corpus(*args, layers, mounted.value());
}
