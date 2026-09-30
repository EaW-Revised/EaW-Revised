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

std::string optional_json(const std::optional<std::string>& value) {
    return value ? json(*value) : std::string("null");
}

template <typename Container, typename Write>
void list(std::ostream& out, const Container& values, Write write) {
    out << '[';
    bool first = true;
    for (const auto& value : values) {
        if (!first) out << ',';
        first = false;
        write(value);
    }
    out << ']';
}

void write_tally(std::ostream& out, const Tally& tally) {
    out << "{\"surfaces\":" << tally.surfaces << ",\"visible_surfaces\":" << tally.visible_surfaces
        << ",\"models\":" << tally.models.size() << ",\"maps_primary\":" << tally.maps_primary.size()
        << ",\"maps_secondary\":" << tally.maps_secondary.size() << ",\"land_maps\":" << tally.land_maps.size()
        << ",\"space_maps\":" << tally.space_maps.size() << '}';
}

void write_resolved(std::ostream& out, const std::optional<Resolved>& value, const bool with_source_id) {
    if (!value) {
        out << "null";
        return;
    }
    out << "{\"logical_path\":" << json(value->logical_path) << ",\"layer\":" << json(value->layer_id)
        << ",\"origin\":" << json(value->origin);
    if (with_source_id) out << ",\"source_id\":" << json(value->source_id);
    out << '}';
}

void write_surface(std::ostream& out, const SurfaceRow& surface, const bool full) {
    out << "{\"mesh_index\":" << surface.mesh_index << ",\"submesh_index\":" << surface.submesh_index
        << ",\"mesh_name\":" << json(surface.mesh_name)
        << ",\"mesh_visible\":" << (surface.mesh_visible ? "true" : "false")
        << ",\"bone\":" << surface.bone << ",\"chain_class\":" << json(surface.chain.label)
        << ",\"billboard_mode\":";
    if (surface.chain.billboard_mode) out << *surface.chain.billboard_mode;
    else out << "null";
    out << ",\"skinned\":" << (surface.skinned ? "true" : "false") << ",\"skin_bones\":" << surface.skin_bones
        << ",\"vertex_format\":" << json(surface.vertex_format) << ",\"shader\":" << json(surface.shader)
        << ",\"descriptor_effect\":" << optional_json(surface.identity.effect)
        << ",\"family\":" << json(surface.identity.family) << ",\"parameters\":";
    list(out, surface.parameters, [&](const std::string& value) { out << json(value); });
    out << ",\"textures\":";
    list(out, surface.textures, [&](const TextureRow& texture) {
        out << "{\"parameter\":" << json(texture.parameter) << ",\"name\":" << json(texture.name)
            << ",\"status\":" << json(texture.status) << ",\"resolved\":";
        write_resolved(out, texture.resolved, full);
        out << '}';
    });
    out << ",\"vertices\":" << surface.vertices << ",\"triangles\":" << surface.triangles
        << ",\"space_primary_plan\":";
    if (surface.plan_status) {
        out << "{\"status\":" << json(*surface.plan_status) << ",\"causes\":";
        list(out, surface.plan_causes, [&](const std::string& value) { out << json(value); });
        out << '}';
    } else {
        out << "null";
    }
    out << '}';
}

void write_model(std::ostream& out, const ModelRow& model, const bool full) {
    out << "{\"logical_path\":" << json(model.source.logical_path) << ",\"layer\":" << json(model.source.layer_id)
        << ",\"origin\":" << json(model.source.origin);
    if (full) out << ",\"source_id\":" << json(model.source.source_id);
    out << ",\"sha256\":" << (model.sha256.empty() ? std::string("null") : json(model.sha256))
        << ",\"declared_names\":";
    list(out, model.declared_names, [&](const std::string& value) { out << json(value); });
    out << ",\"status\":" << json(model.status);
    if (model.status != "loaded") {
        out << ",\"failure\":{\"code\":" << json(model.failure_code);
        if (full) out << ",\"message\":" << json(model.failure_message);
        out << '}';
    }
    out << ",\"usage\":{\"maps_primary\":" << model.usage.primary_maps.size()
        << ",\"maps_secondary\":" << model.usage.secondary_maps.size()
        << ",\"land_maps\":" << model.usage.land_maps.size()
        << ",\"space_maps\":" << model.usage.space_maps.size()
        << ",\"space_primary_maps\":" << model.usage.space_primary_maps.size() << '}'
        << ",\"bones\":" << model.bones << ",\"meshes\":" << model.meshes
        << ",\"distinct_base_textures\":" << model.base_textures.size() << ",\"surfaces\":";
    list(out, model.surfaces, [&](const SurfaceRow& surface) {
        out << "\n      ";
        write_surface(out, surface, full);
    });
    out << '}';
}

std::string render(const Scan& scan, const bool full) {
    std::ostringstream out;
    const Totals sums = totals(scan.models);
    std::uint64_t land = 0, space_count = 0, unknown = 0, failed = 0, references = 0;
    std::map<std::string, std::uint64_t> statuses;
    std::map<GroupKey, std::uint64_t> groups;
    for (const MapRow& map : scan.maps) {
        if (map.status != "loaded") ++failed;
        if (map.kind == "land") ++land;
        else if (map.kind == "space") ++space_count;
        else ++unknown;
        for (const SkyRow& sky : map.skies) {
            ++statuses[sky.status];
            if (sky.status != "undeclared") ++references;
            ++groups[GroupKey{sky.environment_ordinal, sky.field, sky.reference_name, sky.model_name,
                              sky.status, map.kind}];
        }
    }
    std::uint64_t models_loaded = 0;
    for (const auto& [path, model] : scan.models) models_loaded += model.status == "loaded" ? 1U : 0U;

    out << "{\n  \"schema_version\":1,\n  \"tool\":\"sky_scan\",\n  \"profile\":" << json(scan.profile)
        << ",\n  \"scope\":\"CPU-only sky model surface/shader ledger; measures material families, "
           "renders nothing, qualifies no shader and claims no visual fidelity\""
        << ",\n  \"catalog_status\":" << json(scan.catalog_loaded ? "loaded" : "unavailable")
        << ",\n  \"descriptor_bundle\":{\"source\":\"plan/inventories/shader-corpus.json\",\"bundle_id\":"
        << json(sky_scan_descriptors::bundle_id) << ",\"effects\":" << sky_scan_descriptors::effects.size()
        << ",\"match\":\"authored shader stem with a case-insensitive .fx removed, compared case-insensitively "
           "to effects[].name; no match is not_in_descriptor_bundle\"}"
        << ",\n  \"billboard_labels\":{\"attribution\":" << json(billboard_attribution) << ",\"labels\":";
    list(out, billboard_labels, [&](const std::string_view value) { out << json(value); });
    out << ",\"other_values\":\"unknown\"}"
        << ",\n  \"chain_class_rule\":\"attach bone to root: invalid, then nearest billboard bone, then any "
           "stored-hidden bone, then any non-proper-rigid transform, then identity if every transform is "
           "exactly identity (or no bone) else rigid_static\""
        << ",\n  \"model_selection_rule\":\"assets::resolve_map: land map Land_Model_Name else Model_Name; "
           "space map Space_Model_Name else Model_Name\""
        << ",\n  \"maps\":{\"scanned\":" << scan.maps.size() << ",\"failed\":" << failed << ",\"land\":" << land
        << ",\"space\":" << space_count << ",\"unknown_kind\":" << unknown
        << ",\"sky_references\":" << references << ",\"reference_rows_by_status\":{";
    bool first = true;
    for (const auto& [status, count] : statuses) {
        if (!first) out << ',';
        first = false;
        out << json(status) << ':' << count;
    }
    out << "}},\n  \"reference_groups\":";
    list(out, groups, [&](const auto& entry) {
        const GroupKey& key = entry.first;
        out << "\n    {\"environment_ordinal\":" << key.ordinal << ",\"field\":" << json(key.field)
            << ",\"reference_name\":" << optional_json(key.reference) << ",\"model_name\":"
            << optional_json(key.model) << ",\"status\":" << json(key.status) << ",\"map_kind\":"
            << json(key.kind) << ",\"maps\":" << entry.second << '}';
    });

    out << ",\n  \"aggregates\":{\n    \"models\":" << scan.models.size() << ",\"models_loaded\":" << models_loaded
        << ",\"surfaces\":" << sums.surfaces << ",\"hidden_mesh_surfaces\":" << sums.hidden_mesh_surfaces
        << ",\"billboard_surfaces\":" << sums.billboard_surfaces << ",\"skinned_surfaces\":"
        << sums.skinned_surfaces << ",\"vertex_colour_format_surfaces\":" << sums.vertex_colour_format_surfaces
        << ",\"vertex_colour_format_rule\":\"format-name observation: stored vertex format name ends in C\""
        << ",\"textures_declared\":" << sums.textures_declared << ",\"textures_resolved\":" << sums.textures_resolved
        << ",\"textures_not_in_vfs\":" << sums.textures_not_in_vfs
        << ",\"multi_texture_surfaces\":" << sums.multi_texture_surfaces
        << ",\n    \"models_without_surfaces\":";
    list(out, sums.models_without_surfaces, [&](const std::string& value) { out << json(value); });
    out << ",\n    \"land_sky_models_with_multiple_base_textures\":";
    list(out, sums.land_multi_base_texture_models, [&](const std::string& value) { out << json(value); });
    out << ",\"land_maps_with_multiple_base_texture_sky\":" << sums.land_multi_base_texture_maps.size()
        << ",\n    \"shaders\":";
    list(out, sums.shaders, [&](const auto& entry) {
        out << "\n      {\"shader\":" << json(entry.first) << ",\"family\":"
            << json(*entry.second.families.begin()) << ',';
        std::ostringstream tally;
        write_tally(tally, entry.second);
        out << tally.str().substr(1);
    });
    out << ",\n    \"families\":";
    list(out, sums.families, [&](const auto& entry) {
        out << "\n      {\"family\":" << json(entry.first) << ',';
        std::ostringstream tally;
        write_tally(tally, entry.second);
        out << tally.str().substr(1);
    });
    out << ",\n    \"family_usage\":{";
    std::set<std::string> other;
    for (const auto& [family, tally] : sums.families) {
        if (family != "SKYDOME" && family != "NEBULA" && family != "PLANET" && family != "MESH"
            && family != not_in_bundle) other.insert(family);
    }
    std::set<std::string> descriptor_other;
    for (const auto& effect : sky_scan_descriptors::effects) {
        const std::string family(effect.family);
        if (family != "SKYDOME" && family != "NEBULA" && family != "PLANET" && family != "MESH") {
            descriptor_other.insert(family);
        }
    }
    first = true;
    for (const std::string_view family : {"SKYDOME", "NEBULA", "PLANET", "MESH"}) {
        if (!first) out << ',';
        first = false;
        const auto found = sums.families.find(std::string(family));
        out << "\n      " << json(family) << ":{\"status\":"
            << json(found == sums.families.end() ? "unused_in_corpus" : "used") << ",\"tally\":";
        write_tally(out, found == sums.families.end() ? Tally{} : found->second);
        out << '}';
    }
    Tally other_tally;
    for (const auto& family : other) {
        const Tally& tally = sums.families.at(family);
        other_tally.surfaces += tally.surfaces;
        other_tally.visible_surfaces += tally.visible_surfaces;
        other_tally.models.insert(tally.models.begin(), tally.models.end());
        other_tally.maps_primary.insert(tally.maps_primary.begin(), tally.maps_primary.end());
        other_tally.maps_secondary.insert(tally.maps_secondary.begin(), tally.maps_secondary.end());
        other_tally.land_maps.insert(tally.land_maps.begin(), tally.land_maps.end());
        other_tally.space_maps.insert(tally.space_maps.begin(), tally.space_maps.end());
    }
    out << ",\n      \"other\":{\"status\":" << json(other.empty() ? "unused_in_corpus" : "used")
        << ",\"descriptor_families\":";
    list(out, descriptor_other, [&](const std::string& value) { out << json(value); });
    out << ",\"families_used\":";
    list(out, other, [&](const std::string& value) { out << json(value); });
    out << ",\"tally\":";
    write_tally(out, other_tally);
    out << '}';
    const auto outside = sums.families.find(std::string(not_in_bundle));
    out << ",\n      " << json(not_in_bundle) << ":{\"status\":"
        << json(outside == sums.families.end() ? "unused_in_corpus" : "used") << ",\"tally\":";
    write_tally(out, outside == sums.families.end() ? Tally{} : outside->second);
    out << "}\n    },\n    \"vertex_formats\":{";
    first = true;
    for (const auto& [name, count] : sums.vertex_formats) {
        if (!first) out << ',';
        first = false;
        out << json(name) << ':' << count;
    }
    out << "},\n    \"chain_classes\":{";
    first = true;
    for (const auto& [name, count] : sums.chain_classes) {
        if (!first) out << ',';
        first = false;
        out << json(name) << ':' << count;
    }
    out << "}\n  },\n  \"models\":";
    list(out, scan.models, [&](const auto& entry) {
        out << "\n    ";
        write_model(out, entry.second, full);
    });
    if (full) {
        out << ",\n  \"map_records\":";
        list(out, scan.maps, [&](const MapRow& map) {
            out << "\n    {\"logical_path\":" << json(map.logical_path) << ",\"layer\":" << json(map.layer_id)
                << ",\"sha256\":" << (map.sha256.empty() ? std::string("null") : json(map.sha256))
                << ",\"kind\":" << json(map.kind) << ",\"status\":" << json(map.status)
                << ",\"failure_code\":" << (map.failure_code.empty() ? std::string("null") : json(map.failure_code))
                << ",\"environments\":" << map.environments << ",\"skies\":";
            list(out, map.skies, [&](const SkyRow& sky) {
                out << "{\"environment_ordinal\":" << sky.environment_ordinal << ",\"field\":" << json(sky.field)
                    << ",\"field_id\":" << sky.field_id << ",\"reference_name\":" << optional_json(sky.reference_name)
                    << ",\"model_name\":" << optional_json(sky.model_name) << ",\"status\":" << json(sky.status)
                    << ",\"model_path\":" << optional_json(sky.model_path) << '}';
            });
            out << '}';
        });
    }
    out << "\n}\n";
    return out.str();
}

bool write_file(const std::filesystem::path& path, const std::string& contents) {
    std::error_code error;
    const auto parent = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    std::filesystem::create_directories(parent, error);
    if (error) {
        std::cerr << "cannot create output directory\n";
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) {
        std::cerr << "cannot write output file\n";
        return false;
    }
    return true;
}

} // namespace sky_scan_internal
