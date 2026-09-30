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

std::string json(const std::string_view value) {
    std::ostringstream output;
    output << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
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

void write_environment_reference_ledger(
    std::ostream& output, const eawr::assets::MapResolution& resolution, const bool catalog_loaded) {
    using eawr::assets::EnvironmentReferenceField;
    using eawr::assets::EnvironmentReferenceStatus;
    const auto optional_name = [&](const std::optional<std::string>& value) {
        output << (value ? json(*value) : "null");
    };
    output << "{\"schema_version\":1,\"catalog_status\":"
        << json(catalog_loaded ? "loaded" : "unavailable") << ",\"references\":[";
    bool first = true;
    for (const auto& row : resolution.environment_references) {
        if (!first) output << ',';
        first = false;
        const auto field = [&]() -> std::string_view {
            switch (row.field) {
            case EnvironmentReferenceField::primary_sky: return "primary_sky";
            case EnvironmentReferenceField::secondary_sky: return "secondary_sky";
            case EnvironmentReferenceField::cloud_texture: return "cloud_texture";
            }
            return "unknown";
        }();
        const auto status = [&]() -> std::string_view {
            switch (row.status) {
            case EnvironmentReferenceStatus::undeclared: return "undeclared";
            case EnvironmentReferenceStatus::missing_catalog_object: return "missing_catalog_object";
            case EnvironmentReferenceStatus::undeclared_model: return "undeclared_model";
            case EnvironmentReferenceStatus::unresolved_model: return "unresolved_model";
            case EnvironmentReferenceStatus::unresolved_texture: return "unresolved_texture";
            case EnvironmentReferenceStatus::resolved: return "resolved";
            }
            return "unknown";
        }();
        output << "{\"environment_ordinal\":" << row.environment_ordinal
            << ",\"field\":" << json(field) << ",\"field_id\":" << unsigned(row.field_id)
            << ",\"declaration_byte_offset\":";
        if (row.declaration_byte_offset) output << *row.declaration_byte_offset;
        else output << "null";
        output << ",\"reference_name\":";
        optional_name(row.reference_name);
        output << ",\"catalog_source\":";
        if (row.catalog_source) {
            const auto& source = *row.catalog_source;
            output << "{\"logical_path\":" << json(source.logical_path)
                << ",\"source_id\":" << json(source.source_id)
                << ",\"layer_id\":" << json(source.layer_id)
                << ",\"line\":" << source.line
                << ",\"column\":" << source.column << '}';
        } else output << "null";
        output << ",\"model_name\":";
        optional_name(row.model_name);
        output << ",\"status\":" << json(status) << '}';
    }
    output << "]}";
}

// Source-space structural evidence only. The decoded pair rows refer back to
// mini headers; retained field metadata never exposes payload bytes.
void write_source_bounds_ledger(std::ostream& output, const eawr::assets::Map& map) {
    output << std::setprecision(9)
        << "{\"schema_version\":1,\"source_coordinates\":\"unchanged_ted\""
        << ",\"interpretation\":{\"kind\":\"structural_source_bounds\""
        << ",\"selected_camera\":null,\"playable_boundary\":null"
        << ",\"inferred_volume_meaning\":false},\"declared_extents\":";
    if (map.declared_extents) {
        const auto& extents = *map.declared_extents;
        output << "{\"first_field_id\":" << unsigned(extents.first_field_id)
            << ",\"first_byte_offset\":" << extents.first_byte_offset
            << ",\"first\":" << extents.first
            << ",\"second_field_id\":" << unsigned(extents.second_field_id)
            << ",\"second_byte_offset\":" << extents.second_byte_offset
            << ",\"second\":" << extents.second << '}';
    } else output << "null";
    output << ",\"volumes\":[";
    for (std::size_t index = 0; index < map.volumes.size(); ++index) {
        if (index != 0) output << ',';
        const auto& volume = map.volumes[index];
        output << "{\"pair_ordinal\":" << index
            << ",\"min_field_id\":" << unsigned(volume.min_field_id)
            << ",\"min_byte_offset\":" << volume.min_byte_offset
            << ",\"max_field_id\":" << unsigned(volume.max_field_id)
            << ",\"max_byte_offset\":" << volume.max_byte_offset
            << ",\"minimum\":[" << volume.minimum.x << ',' << volume.minimum.y << ',' << volume.minimum.z
            << "],\"maximum\":[" << volume.maximum.x << ',' << volume.maximum.y << ',' << volume.maximum.z << "]}";
    }
    output << "],\"volume_fields\":[";
    for (std::size_t index = 0; index < map.volume_fields.size(); ++index) {
        if (index != 0) output << ',';
        const auto& field = map.volume_fields[index];
        const bool paired = std::any_of(map.volumes.begin(), map.volumes.end(), [&](const auto& volume) {
            return field.byte_offset == volume.min_byte_offset || field.byte_offset == volume.max_byte_offset;
        });
        output << "{\"field_id\":" << field.id << ",\"byte_offset\":" << field.byte_offset
            << ",\"payload_length\":" << field.bytes.size()
            << ",\"participates_in_pair\":" << (paired ? "true" : "false") << '}';
    }
    output << "],\"notices\":[";
    bool first = true;
    for (const auto& notice : map.issues) {
        const bool relevant = notice.issue == eawr::assets::MapIssue::declared_extents_invalid
            || notice.issue == eawr::assets::MapIssue::volume_stream_invalid
            || (notice.issue == eawr::assets::MapIssue::optional_section_absent
                && notice.chunk_path == "1/259")
            || (notice.issue == eawr::assets::MapIssue::duplicate_root_field
                && notice.field_id && (*notice.field_id == 0x10 || *notice.field_id == 0x11));
        if (!relevant) continue;
        if (!first) output << ',';
        first = false;
        output << "{\"issue_code\":" << json(eawr::assets::to_string(notice.issue))
            << ",\"chunk_path\":" << json(notice.chunk_path)
            << ",\"byte_offset\":" << notice.byte_offset
            << ",\"byte_size\":" << notice.byte_size
            << ",\"field_id\":";
        if (notice.field_id) output << unsigned(*notice.field_id);
        else output << "null";
        output << '}';
    }
    output << "]}";
}

// Shape-only diagnostics of the unconfirmed TED candidate lighting mapping for
// every decoded environment record, in record order. The rows come from the
// lighting module's own reader, so the first occurrence of a repeated mini is
// the one judged; this ledger neither selects an environment nor claims that
// any candidate field drives effective lighting.
void write_environment_candidate_ledger(std::ostream& output, const eawr::assets::Map& map) {
    namespace lighting = eawr::presentation::lighting;
    output << "{\"schema_version\":1,\"mapping_status\":\"unconfirmed\""
        << ",\"selected_environment\":null,\"environments\":[";
    bool first_environment = true;
    for (const eawr::assets::EnvironmentDescriptor& environment : map.environments) {
        std::vector<lighting::RawField> fields;
        fields.reserve(environment.fields.size());
        for (const auto& field : environment.fields) fields.push_back({field.id, field.bytes});
        const auto diagnostics = lighting::diagnose_candidate_environment(fields);
        if (!first_environment) output << ',';
        first_environment = false;
        output << "{\"environment_ordinal\":" << environment.record_ordinal << ",\"fields\":[";
        for (std::size_t index = 0; index < diagnostics.fields.size(); ++index) {
            const lighting::CandidateFieldResult& row = diagnostics.fields[index];
            if (index != 0) output << ',';
            output << "{\"field_id\":" << row.id
                << ",\"status\":" << json(lighting::to_string(row.status))
                << ",\"expected_size\":" << row.expected_size
                << ",\"observed_size\":" << row.observed_size
                << ",\"occurrence_count\":" << row.occurrence_count
                << ",\"selected_field_ordinal\":";
            if (row.selected_ordinal) {
                output << *row.selected_ordinal << ",\"selected_byte_offset\":"
                    << environment.fields[*row.selected_ordinal].byte_offset;
            } else output << "null,\"selected_byte_offset\":null";
            output << '}';
        }
        output << "]}";
    }
    output << "]}";
}

void write_counts(std::ostream& output, const AssetCounts& counts) {
    output << "{\"meshes\":" << counts.meshes
        << ",\"bones\":" << counts.bones
        << ",\"materials\":" << counts.materials
        << ",\"animations\":" << counts.animations
        << ",\"vertices\":" << counts.vertices
        << ",\"indices\":" << counts.indices
        << ",\"mips\":" << counts.mips
        << ",\"samples\":" << counts.samples
        << ",\"maps\":" << counts.maps
        << ",\"terrain_samples\":" << counts.terrain_samples
        << ",\"placements\":" << counts.placements
        << ",\"semantic_maps\":" << counts.semantic_maps
        << ",\"placements_crc_absent\":" << counts.placements_crc_absent
        << ",\"placements_crc_missing\":" << counts.placements_crc_missing
        << ",\"placements_crc_collision\":" << counts.placements_crc_collision
        << ",\"placements_crc_unique\":" << counts.placements_crc_unique
        << ",\"placements_model_renderable\":" << counts.placements_model_renderable
        << ",\"placements_model_unresolved\":" << counts.placements_model_unresolved
        << ",\"placements_model_undeclared\":" << counts.placements_model_undeclared
        << ",\"placements_model_unknown\":" << counts.placements_model_unknown
        << ",\"texture_references\":" << counts.texture_references
        << ",\"textures_resolved\":" << counts.textures_resolved
        << ",\"texture_slots_untextured\":" << counts.texture_slots_untextured
        << ",\"sky_references\":" << counts.sky_references
        << ",\"skies_resolved\":" << counts.skies_resolved
        << ",\"skies_model_renderable\":" << counts.skies_model_renderable << '}';
}

void write_aggregate(std::ostream& output, const Aggregate& value) {
    output << "{\"discovered\":" << value.discovered
        << ",\"loaded\":" << value.loaded
        << ",\"failed\":" << value.failed
        << ",\"notices\":" << value.notices
        << ",\"counts\":";
    write_counts(output, value.counts);
    output << '}';
}

void write_outcome(std::ostream& output, const AssetOutcome& outcome) {
    output << "{\"logical_path\":" << json(outcome.source.canonical_path)
        << ",\"format\":" << json(outcome.format)
        << ",\"outcome\":" << json(outcome.loaded ? "loaded" : "failed")
        << ",\"content_sha256\":";
    if (outcome.content_sha256.empty()) output << "null";
    else output << json(outcome.content_sha256);
    output << ",\"provenance\":{\"layer\":" << json(outcome.source.layer_id)
        << ",\"origin\":" << json(eawr::vfs::to_string(outcome.source.origin))
        << ",\"source_id\":" << json(outcome.source.source_id)
        << ",\"stored_size\":" << outcome.source.size << "}"
        << ",\"counts\":";
    write_counts(output, outcome.counts);
    output << ",\"notices\":" << outcome.notices;
    if (outcome.diagnostic) {
        const auto& diagnostic = *outcome.diagnostic;
        output << ",\"failure\":{\"code\":" << json(diagnostic.code)
            << ",\"cause\":" << json(diagnostic.message)
            << ",\"affected_feature\":" << json(outcome.affected_feature)
            << ",\"follow_up\":" << json(outcome.follow_up);
        if (diagnostic.column) output << ",\"byte_offset\":" << (*diagnostic.column - 1U);
        else output << ",\"byte_offset\":null";
        output << '}';
    }
    output << '}';
}

// Rates are reported alongside their exact numerator and denominator so a
// reader never has to trust a rounded percentage on its own.  A zero
// denominator is reported as a null rate rather than an invented 100%.
void write_rate(std::ostream& output, const std::string_view name,
                const std::uint64_t numerator, const std::uint64_t denominator) {
    output << json(name) << ":{\"resolved\":" << numerator
        << ",\"total\":" << denominator << ",\"rate\":";
    if (denominator == 0) output << "null";
    else {
        std::ostringstream rate;
        rate << std::fixed << std::setprecision(6)
            << (static_cast<double>(numerator) / static_cast<double>(denominator));
        output << rate.str();
    }
    output << ",\"meets_99_percent\":"
        << (denominator != 0 && numerator * 100U >= denominator * 99U ? "true" : "false") << '}';
}

void write_named_ledger(std::ostream& output, const std::string_view name,
                        const std::map<std::string, std::uint64_t>& entries) {
    output << json(name) << ":[";
    bool first = true;
    for (const auto& [value, maps] : entries) {
        if (!first) output << ',';
        first = false;
        output << "{\"name\":" << json(value) << ",\"maps\":" << maps << '}';
    }
    output << ']';
}

void write_crc_ledger(std::ostream& output, const std::map<std::uint32_t, std::uint64_t>& entries) {
    output << "\"placement_type_crcs\":[";
    bool first = true;
    for (const auto& [value, maps] : entries) {
        if (!first) output << ',';
        first = false;
        std::ostringstream hex;
        hex << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
        output << "{\"crc32\":" << json(hex.str()) << ",\"maps\":" << maps << '}';
    }
    output << ']';
}

void write_map_resolution(std::ostream& output, const AssetCounts& counts) {
    output << "{\n    ";
    write_rate(output, "placement_type_resolution", counts.placements_crc_unique, counts.placements);
    output << ",\n    ";
    write_rate(output, "placement_model_renderability", counts.placements_model_renderable, counts.placements);
    output << ",\n    ";
    write_rate(output, "terrain_and_cloud_textures", counts.textures_resolved, counts.texture_references);
    output << ",\n    ";
    write_rate(output, "environment_skies", counts.skies_resolved, counts.sky_references);
    output << ",\n    ";
    write_rate(output, "environment_sky_models", counts.skies_model_renderable, counts.sky_references);
    output << "\n  }";
}

void write_family_ledger(std::ostream& output, const FamilyLedger& families) {
    const auto listed = [&](const std::string_view name,
                            const std::map<std::string, std::uint64_t>& entries) {
        output << json(name) << ":[";
        bool first = true;
        for (const auto& [value, maps] : entries) {
            if (!first) output << ',';
            first = false;
            output << "{\"name\":" << json(value) << ",\"maps\":" << maps << '}';
        }
        output << ']';
    };
    output << "{\n    ";
    listed("terrain_effects_used", families.terrain_effects);
    output << ",\n    ";
    listed("terrain_effects_declared_slots", families.terrain_declared_slots);
    output << ",\n    ";
    listed("skydome_objects", families.skydome_objects);
    output << ",\n    ";
    listed("skydome_models", families.skydome_models);
    output << ",\n    \"map_issues\":[";
    bool first_issue = true;
    for (const auto& [name, maps] : families.map_issue_maps) {
        if (!first_issue) output << ',';
        first_issue = false;
        const auto occurrences = families.map_issue_occurrences.find(name);
        output << "{\"name\":" << json(name) << ",\"maps\":" << maps << ",\"occurrences\":"
            << (occurrences == families.map_issue_occurrences.end() ? 0 : occurrences->second) << '}';
    }
    output << ']';
    output << ",\n    \"maps_with_terrain\":" << families.maps_with_terrain
        << ",\"maps_with_water_record\":" << families.maps_with_water_record
        << ",\"maps_with_sky_reference\":" << families.maps_with_sky_reference
        << ",\"space_maps\":" << families.space_maps << "\n  }";
}

std::string complete_report(
    const Arguments& args,
    const std::vector<AssetOutcome>& outcomes,
    const std::map<std::string, Aggregate>& aggregates,
    const UnresolvedLedger& ledger,
    const FamilyLedger& families) {
    Aggregate total;
    for (const auto& [format, value] : aggregates) {
        static_cast<void>(format);
        total.discovered += value.discovered;
        total.loaded += value.loaded;
        total.failed += value.failed;
        total.notices += value.notices;
        add_counts(total.counts, value.counts);
    }
    std::ostringstream output;
    output << "{\n  \"schema_version\":2,\n  \"profile\":" << json(args.profile)
        << ",\n  \"view\":" << json(args.view)
        << ",\n  \"count_semantics\":{"
           "\"materials\":\"ALO submesh material bindings\","
           "\"animations\":\"successfully decoded ALA resources\","
           "\"bones\":\"ALO skeleton bones plus ALA bone tracks\","
           "\"samples\":\"decoded ALA track samples\","
           "\"placements_crc_*\":\"TED persisted-object records bucketed by type-CRC resolution "
           "against the active XML catalog; the four buckets sum to placements\","
           "\"placements_model_*\":\"uniquely typed placements bucketed by model-chain probe outcome, "
           "plus the not-uniquely-typed remainder as unknown; the four buckets sum to placements\","
           "\"texture_references\":\"declared TED terrain-material and cloud texture names\","
           "\"sky_references\":\"declared TED environment skydome object ids resolved "
           "against the active XML catalog, not against art files\"},"
        << "\n  \"aggregates\":{\n";
    bool first = true;
    for (const auto& [format, value] : aggregates) {
        if (!first) output << ",\n";
        first = false;
        output << "    " << json(format) << ':';
        write_aggregate(output, value);
    }
    output << "\n  },\n  \"total\":";
    write_aggregate(output, total);
    output << ",\n  \"map_resolution\":";
    write_map_resolution(output, aggregates.at("ted").counts);
    output << ",\n  \"map_unresolved\":{\n    ";
    write_crc_ledger(output, ledger.type_crcs);
    output << ",\n    ";
    write_named_ledger(output, "model_names", ledger.model_names);
    output << ",\n    ";
    write_named_ledger(output, "texture_names", ledger.texture_names);
    output << ",\n    ";
    write_named_ledger(output, "sky_object_ids", ledger.sky_ids);
    output << "\n  }";
    output << ",\n  \"map_families\":";
    write_family_ledger(output, families);
    output << ",\n  \"records\":[";
    for (std::size_t index = 0; index < outcomes.size(); ++index) {
        output << (index == 0 ? "\n    " : ",\n    ");
        write_outcome(output, outcomes[index]);
    }
    output << "\n  ]\n}\n";
    return output.str();
}

std::string legacy_summary(
    const Arguments& args,
    const std::map<std::string, Aggregate>& aggregates) {
    std::ostringstream output;
    output << "{\n  \"schema_version\": 2,\n  \"profile\": " << json(args.profile)
        << ",\n  \"view\": " << json(args.view) << ",\n  \"assets\": {";
    bool first = true;
    std::uint64_t failures = 0;
    for (const auto& [format, value] : aggregates) {
        if (!first) output << ',';
        first = false;
        output << "\n    " << json(format) << ": ";
        write_aggregate(output, value);
        failures += value.failed;
    }
    output << "\n  },\n  \"failure_count\": " << failures << "\n}\n";
    return output.str();
}

std::string legacy_failures(const Arguments& args, const std::vector<AssetOutcome>& outcomes) {
    std::ostringstream output;
    output << "{\n  \"schema_version\": 2,\n  \"profile\": " << json(args.profile)
        << ",\n  \"view\": " << json(args.view) << ",\n  \"failures\": [";
    bool first = true;
    for (const auto& outcome : outcomes) {
        if (outcome.loaded) continue;
        if (!first) output << ',';
        first = false;
        const auto& diagnostic = *outcome.diagnostic;
        output << "\n    {\"path\": " << json(outcome.source.canonical_path)
            << ", \"format\": " << json(outcome.format)
            << ", \"layer\": " << json(outcome.source.layer_id)
            << ", \"origin\": " << json(eawr::vfs::to_string(outcome.source.origin))
            << ", \"source_id\": " << json(outcome.source.source_id)
            << ", \"size\": " << outcome.source.size
            << ", \"content_sha256\": ";
        if (outcome.content_sha256.empty()) output << "null";
        else output << json(outcome.content_sha256);
        output << ", \"code\": " << json(diagnostic.code)
            << ", \"message\": " << json(diagnostic.message)
            << ", \"cause\": " << json(diagnostic.message)
            << ", \"affected_feature\": " << json(outcome.affected_feature)
            << ", \"follow_up\": " << json(outcome.follow_up);
        if (diagnostic.column) output << ", \"byte_offset\": " << (*diagnostic.column - 1U);
        else output << ", \"byte_offset\": null";
        output << '}';
    }
    output << "\n  ]\n}\n";
    return output.str();
}

bool write_file(const std::filesystem::path& path, const std::string& contents) {
    std::error_code error;
    const auto parent = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    std::filesystem::create_directories(parent, error);
    if (error) {
        std::cerr << "cannot create report directory: " << error.message() << '\n';
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) {
        std::cerr << "cannot write report: " << utf8(path) << '\n';
        return false;
    }
    return true;
}
} // namespace asset_validate_internal
