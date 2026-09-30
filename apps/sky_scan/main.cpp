#include "sky_scan_internal.hpp"
// sky_scan: a CPU-only ledger of the surfaces and shader families of every
// environment sky model the effective map corpus references (P1-06, #27).
//
// It measures; it does not render, qualify a shader, or decide how an
// original sky is drawn. Every declared primary/secondary sky reference is
// kept as a row whether or not it resolves. No material value other than a
// texture name is written, and no host path is written.

#include "eawr/assets/assets.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/presentation/space/space.hpp"
#include "eawr/vfs/vfs.hpp"

#include "sky_scan_descriptors.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace sky_scan_internal {

namespace assets = eawr::assets;
namespace space = eawr::presentation::space;

void usage() {
    std::cerr << "Usage: sky_scan --profile <eaw|foc|remake> --game-root <dir> [--mod-root <leaf;parent;...>] "
                 "--report <out.json> [--inventory <fragment.json>]\n"
                 "  --mod-root is required for remake and rejected otherwise.\n";
}

std::optional<Arguments> arguments(const int argc, char** argv) {
    Arguments result;
    std::set<std::string_view> seen;
    for (int index = 1; index < argc; ++index) {
        if (index + 1 >= argc) return std::nullopt;
        const std::string_view key(argv[index]);
        const std::string_view text(argv[++index]);
        if (text.empty() || !seen.insert(key).second) return std::nullopt;
        const std::filesystem::path value(text);
        if (key == "--profile") result.profile = std::string(text);
        else if (key == "--game-root") result.game_root = value;
        else if (key == "--mod-root") result.mod_root = value;
        else if (key == "--report") result.report = value;
        else if (key == "--inventory") result.inventory = value;
        else return std::nullopt;
    }
    const bool profile_ok = result.profile == "eaw" || result.profile == "foc" || result.profile == "remake";
    if (!profile_ok || result.game_root.empty() || result.report.empty()) return std::nullopt;
    if ((result.profile == "remake") != !result.mod_root.empty()) return std::nullopt;
    return result;
}

} // namespace sky_scan_internal

using namespace sky_scan_internal;

int main(const int argc, char** argv) {
    const auto args = arguments(argc, argv);
    if (!args) {
        usage();
        return 2;
    }
    if (!std::filesystem::is_directory(args->game_root)
        ) {
        std::cerr << "sky_scan: --game-root and --mod-root must be existing directories\n";
        return 2;
    }
    const auto configured = roots(*args);
    if (!configured) {
        std::cerr << "sky_scan: foc/remake require an installation root with GameData/Data and corruption/Data\n";
        return 2;
    }
    std::vector<eawr::vfs::MountSpec> specs;
    auto chain = eawr::vfs::resolve_manifest_chain(*configured);
    if (!chain) {
        std::cerr << eawr::core::format_diagnostic(chain.error()) << '\n';
        return 1;
    }
    for (auto& manifest : chain.value()) {
        specs.push_back(std::move(manifest.mount));
    }
    auto mounted = eawr::vfs::Vfs::mount(specs);
    if (!mounted) {
        std::cerr << "sky_scan: cannot mount VFS: " << mounted.error().code << '\n';
        return 1;
    }
    const eawr::vfs::Vfs& filesystem = mounted.value();

    Scan scan;
    scan.profile = args->profile;
    const auto profile = args->profile == "eaw" ? eawr::data::Profile::eaw
        : args->profile == "foc" ? eawr::data::Profile::foc : eawr::data::Profile::remake;
    assets::ObjectTypeCatalog catalog;
    if (auto loaded = eawr::data::load_catalog(filesystem, profile)) {
        catalog = assets::object_type_catalog(loaded.value().catalog);
        for (const auto& root : loaded.value().catalog.registry_roots()) {
            if (root.category == eawr::data::Category::game_object && root.outcome == "loaded") {
                scan.catalog_loaded = true;
                break;
            }
        }
    }

    Resolver resolver(filesystem);
    const assets::AssetProbe probe = [&](const std::string_view logical_path) {
        return resolver.stat(std::string(logical_path)).has_value();
    };

    auto listed = filesystem.enumerate("data/art/maps", ".ted");
    if (!listed) {
        std::cerr << "sky_scan: cannot enumerate maps: " << listed.error().code << '\n';
        return 1;
    }
    for (const eawr::vfs::AssetRecord& record : listed.value()) {
        MapRow map;
        map.logical_path = record.canonical_path;
        map.layer_id = record.layer_id;
        map.kind = "unknown";
        auto bytes = filesystem.open(record.canonical_path);
        if (!bytes) {
            map.status = "unreadable";
            map.failure_code = bytes.error().code;
            scan.maps.push_back(std::move(map));
            continue;
        }
        map.sha256 = sha256_hex(bytes.value());
        auto loaded = assets::load_map(bytes.value(), assets::source_from(record), catalog);
        if (!loaded) {
            map.status = "failed_to_load";
            map.failure_code = loaded.error().code;
            scan.maps.push_back(std::move(map));
            continue;
        }
        map.status = "loaded";
        const assets::Map& decoded = loaded.value();
        if (decoded.kind) map.kind = *decoded.kind == assets::MapKind::land ? "land" : "space";
        map.environments = decoded.environments.size();
        const std::size_t map_index = scan.maps.size();
        const auto resolution = assets::resolve_map(decoded, catalog, probe);
        for (const auto& reference : resolution.environment_references) {
            if (reference.field == assets::EnvironmentReferenceField::cloud_texture) continue;
            SkyRow sky;
            const bool primary = reference.field == assets::EnvironmentReferenceField::primary_sky;
            sky.environment_ordinal = reference.environment_ordinal;
            sky.field = primary ? "primary_sky" : "secondary_sky";
            sky.field_id = reference.field_id;
            sky.reference_name = reference.reference_name;
            sky.model_name = reference.model_name;
            sky.status = std::string(reference_status(reference.status));
            if (reference.status == assets::EnvironmentReferenceStatus::resolved && reference.model_name) {
                if (auto found = resolver.model(*reference.model_name)) {
                    sky.model_path = found->logical_path;
                    auto [entry, inserted] = scan.models.try_emplace(found->logical_path);
                    ModelRow& model = entry->second;
                    if (inserted) {
                        model.source = *found;
                        auto model_bytes = filesystem.open(found->logical_path);
                        auto model_record = filesystem.stat(found->logical_path);
                        if (!model_bytes || !model_record) {
                            model.status = "failed_to_load";
                            model.failure_code = !model_bytes ? model_bytes.error().code : model_record.error().code;
                            model.failure_message = "model bytes could not be read";
                        } else {
                            model.sha256 = sha256_hex(model_bytes.value());
                            auto parsed = assets::load_model(model_bytes.value(), assets::source_from(model_record.value()));
                            if (parsed) {
                                model.status = "loaded";
                                model.model = std::move(parsed.value());
                                describe_model(model, resolver);
                            } else {
                                model.status = "failed_to_load";
                                model.failure_code = parsed.error().code;
                                model.failure_message = parsed.error().message;
                            }
                        }
                    }
                    model.declared_names.insert(*reference.model_name);
                    (primary ? model.usage.primary_maps : model.usage.secondary_maps).insert(map_index);
                    if (map.kind == "land") model.usage.land_maps.insert(map_index);
                    if (map.kind == "space") model.usage.space_maps.insert(map_index);
                    if (primary && map.kind == "space") model.usage.space_primary_maps.insert(map_index);
                } else {
                    // resolve_map and this resolver share one rule; a
                    // disagreement is reported, never silently dropped.
                    sky.status = "resolution_mismatch";
                }
            }
            map.skies.push_back(std::move(sky));
        }
        scan.maps.push_back(std::move(map));
    }
    for (auto& [path, model] : scan.models) {
        if (model.model && !model.usage.space_primary_maps.empty()) attach_plan(model);
    }

    if (!write_file(args->report, render(scan, true))) return 1;
    if (!args->inventory.empty() && !write_file(args->inventory, render(scan, false))) return 1;
    std::uint64_t references = 0;
    for (const MapRow& map : scan.maps) {
        for (const SkyRow& sky : map.skies) references += sky.status != "undeclared" ? 1U : 0U;
    }
    std::cout << "sky_scan: " << scan.maps.size() << " maps, " << references << " sky references, "
              << scan.models.size() << " sky models\n";
    return 0;
}
