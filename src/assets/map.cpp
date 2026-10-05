#include "eawr/core/load_profile.hpp"
#include "asset_internal.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/data/tag_trace.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace eawr::assets {
std::uint32_t object_type_crc(const std::string_view logical_name) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const unsigned char raw : logical_name) {
        const std::uint8_t byte = raw >= 'a' && raw <= 'z' ? static_cast<std::uint8_t>(raw - ('a' - 'A')) : raw;
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1U) ^ ((crc & 1U) ? 0xedb88320U : 0U);
    }
    return ~crc;
}

ObjectTypeCatalog object_type_catalog(const data::Catalog& catalog) {
    // #628: a name index over every object, not objects the scene uses.
    const data::tag_trace::Unrecorded unrecorded;
    ObjectTypeCatalog result;
    for (const auto& definition : catalog.definitions()) {
        if (!definition.namespace_winner || definition.id.empty() || definition.category != data::Category::game_object) continue;
        ObjectTypeRef entry;
        entry.logical_name = definition.id;
        entry.source = definition.root.source;
        // The model chain is read from the resolved effective object so an
        // inherited model is found.  A failed resolution leaves every tag
        // absent rather than substituting the definition's own partial view.
        if (auto effective = catalog.resolve(definition.id, data::Category::game_object)) {
            const auto declared = [&](const std::string_view tag) -> std::optional<std::string> {
                const auto* value = effective.value().value(tag);
                if (value == nullptr || value->value.raw_text.empty()) return std::nullopt;
                return value->value.raw_text;
            };
            entry.model_name = declared("Model_Name");
            entry.land_model_name = declared("Land_Model_Name");
            entry.space_model_name = declared("Space_Model_Name");
        }
        result.entries.push_back(std::move(entry));
    }
    return result;
}

std::string_view to_string(const MapIssue issue) noexcept {
    switch (issue) {
    case MapIssue::unknown_root_field: return "unknown_root_field";
    case MapIssue::duplicate_root_field: return "duplicate_root_field";
    case MapIssue::unknown_chunk: return "unknown_chunk";
    case MapIssue::optional_section_absent: return "optional_section_absent";
    case MapIssue::preview_not_decoded: return "preview_not_decoded";
    case MapIssue::main_group_absent: return "main_group_absent";
    case MapIssue::kind_structure_mismatch: return "kind_structure_mismatch";
    case MapIssue::declared_extents_invalid: return "declared_extents_invalid";
    case MapIssue::environment_stream_invalid: return "environment_stream_invalid";
    case MapIssue::volume_stream_invalid: return "volume_stream_invalid";
    case MapIssue::terrain_absent: return "terrain_absent";
    case MapIssue::terrain_legacy_plane: return "terrain_legacy_plane";
    case MapIssue::terrain_header_invalid: return "terrain_header_invalid";
    case MapIssue::terrain_dimensions: return "terrain_dimensions";
    case MapIssue::terrain_cell_count: return "terrain_cell_count";
    case MapIssue::terrain_plane_size: return "terrain_plane_size";
    case MapIssue::terrain_material_stream_invalid: return "terrain_material_stream_invalid";
    case MapIssue::terrain_material_slot: return "terrain_material_slot";
    case MapIssue::passability_size: return "passability_size";
    case MapIssue::water_stream_invalid: return "water_stream_invalid";
    case MapIssue::water_texture_invalid: return "water_texture_invalid";
    case MapIssue::placement_record_shape: return "placement_record_shape";
    case MapIssue::placement_stream_invalid: return "placement_stream_invalid";
    case MapIssue::placement_crc_absent: return "placement_crc_absent";
    case MapIssue::placement_type_missing: return "placement_type_missing";
    case MapIssue::placement_type_collision: return "placement_type_collision";
    case MapIssue::orientation_absent: return "orientation_absent";
    case MapIssue::orientation_nonfinite: return "orientation_nonfinite";
    case MapIssue::orientation_three_axis: return "orientation_three_axis";
    }
    return "unknown";
}

core::Result<Map> load_map(const vfs::Vfs& filesystem, const std::string_view path, const ObjectTypeCatalog& catalog) {
    core::load_profile::Scope load_scope(core::load_profile::Phase::map);
    auto record = filesystem.stat(path); if (!record) return core::Result<Map>::failure(record.error());
    // Reject an oversize record from its stat size, before open() allocates.
    if (record.value().size > detail::max_file_size) {
        return core::Result<Map>::failure(detail::error(source_from(record.value()), diagnostic_codes::limit, "TED record exceeds 512 MiB safety limit before read"));
    }
    auto bytes = filesystem.open(path); if (!bytes) return core::Result<Map>::failure(bytes.error());
    return load_map(bytes.value(), source_from(record.value()), catalog);
}

} // namespace eawr::assets
