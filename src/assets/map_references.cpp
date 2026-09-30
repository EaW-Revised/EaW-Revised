#include "asset_internal.hpp"
#include "eawr/assets/map.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>


namespace eawr::assets {

namespace {
char ascii_lower(const char character) {
    return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A')) : character;
}

bool ascii_iequals(const std::string_view left, const std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) return false;
    }
    return true;
}

std::string ascii_lower_copy(const std::string_view value) {
    std::string result(value);
    for (char& character : result) character = ascii_lower(character);
    return result;
}

// Authored references use backslashes and mixed case; the VFS canonical form is
// lowercase with forward slashes.  Nothing else about the name is rewritten.
std::string canonical_reference(const std::string_view name) {
    std::string result = ascii_lower_copy(name);
    for (char& character : result) {
        if (character == '\\') character = '/';
    }
    const auto first = result.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    result.erase(0, first);
    result.erase(result.find_last_not_of(" \t\r\n") + 1);
    return result;
}

bool has_suffix(const std::string_view name) {
    const auto dot = name.rfind('.');
    return dot != std::string_view::npos && dot + 1 < name.size()
        && name.find('/', dot) == std::string_view::npos;
}

std::string_view reference_root(const ReferenceKind kind) {
    return kind == ReferenceKind::model ? "data/art/models/" : "data/art/textures/";
}

std::span<const std::string_view> reference_suffixes(const ReferenceKind kind) {
    static constexpr std::array<std::string_view, 1> model_suffixes{".alo"};
    static constexpr std::array<std::string_view, 2> texture_suffixes{".tga", ".dds"};
    if (kind == ReferenceKind::model) return model_suffixes;
    return texture_suffixes;
}

// The stem is the name without a suffix this kind knows about.  An unknown
// suffix is left alone so an authored `my.model.name` is not truncated.
std::string_view reference_stem(const std::string_view name, const ReferenceKind kind) {
    for (const auto suffix : reference_suffixes(kind)) {
        if (name.size() > suffix.size() && name.ends_with(suffix)) {
            return name.substr(0, name.size() - suffix.size());
        }
    }
    return name;
}

// A uniquely typed placement uses the map kind's model tag when the chain
// declares one, and otherwise the generic tag.  No tag is invented.
const std::optional<std::string>& chosen_model(
    const ObjectTypeRef& type, const std::optional<MapKind> kind) {
    if (kind == MapKind::land && type.land_model_name) return type.land_model_name;
    if (kind == MapKind::space && type.space_model_name) return type.space_model_name;
    return type.model_name;
}

template <typename T>
void remember(std::vector<T>& values, T value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

void count_reference(const std::optional<std::string>& name, const ReferenceKind kind,
                     const AssetProbe& probe, std::uint64_t& declared, std::uint64_t& resolved,
                     std::vector<std::string>& unresolved) {
    const auto status = resolve_reference(name, kind, probe);
    if (status == ReferenceStatus::absent) return;
    ++declared;
    if (status == ReferenceStatus::resolved) ++resolved;
    else remember(unresolved, *name);
}
} // namespace

ReferenceStatus resolve_reference(
    const std::optional<std::string>& name, const ReferenceKind kind, const AssetProbe& probe) {
    if (!name) return ReferenceStatus::absent;
    const auto canonical = canonical_reference(*name);
    if (canonical.empty()) return ReferenceStatus::absent;
    if (!probe) return ReferenceStatus::unresolved;
    const std::string root(reference_root(kind));
    if (has_suffix(canonical) && probe(root + canonical)) return ReferenceStatus::resolved;
    const std::string stem = root + std::string(reference_stem(canonical, kind));
    for (const auto suffix : reference_suffixes(kind)) {
        if (probe(stem + std::string(suffix))) return ReferenceStatus::resolved;
    }
    return ReferenceStatus::unresolved;
}

const ObjectTypeRef* find_object_type(
    const ObjectTypeCatalog& catalog, const std::string_view logical_name) {
    if (logical_name.empty()) return nullptr;
    for (const auto& entry : catalog.entries) {
        if (ascii_iequals(entry.logical_name, logical_name)) return &entry;
    }
    return nullptr;
}

MapResolution resolve_map(
    const Map& map, const ObjectTypeCatalog& catalog, const AssetProbe& probe) {
    MapResolution result;
    result.placements = map.placements.size();
    for (const auto& placement : map.placements) {
        if (!placement.type_crc) {
            ++result.crc_absent;
            ++result.model_chain_unknown;
            continue;
        }
        if (placement.type_resolution == TypeResolution::missing) {
            ++result.crc_missing;
            ++result.model_chain_unknown;
            remember(result.unresolved_type_crcs, *placement.type_crc);
            continue;
        }
        if (placement.type_resolution == TypeResolution::collision) {
            ++result.crc_collision;
            ++result.model_chain_unknown;
            continue;
        }
        ++result.crc_unique;
        const auto& model = chosen_model(placement.type_candidates.front(), map.kind);
        switch (resolve_reference(model, ReferenceKind::model, probe)) {
        case ReferenceStatus::absent: ++result.model_chain_undeclared; break;
        case ReferenceStatus::unresolved:
            ++result.model_chain_unresolved;
            remember(result.unresolved_model_names, *model);
            break;
        case ReferenceStatus::resolved: ++result.model_chain_renderable; break;
        }
    }
    if (map.terrain) {
        for (const auto& material : map.terrain->materials) {
            if (!material.primary_texture && !material.secondary_texture) ++result.texture_slots_untextured;
            count_reference(material.primary_texture, ReferenceKind::texture, probe,
                            result.texture_references, result.textures_resolved,
                            result.unresolved_texture_names);
            count_reference(material.secondary_texture, ReferenceKind::texture, probe,
                            result.texture_references, result.textures_resolved,
                            result.unresolved_texture_names);
        }
    }
    // Water surface textures are declared whether or not the terrain view
    // decoded, so they stay in the same texture denominator either way.
    for (const auto& water : map.water_textures) {
        count_reference(water.logical_name, ReferenceKind::texture, probe,
                        result.texture_references, result.textures_resolved,
                        result.unresolved_texture_names);
    }
    const auto count_sky = [&](const EnvironmentDescriptor& environment,
                               const EnvironmentReferenceField field,
                               const std::optional<std::string>& name,
                               const std::optional<std::uint64_t> offset) {
        EnvironmentReferenceResolution row;
        row.environment_ordinal = environment.record_ordinal;
        row.field = field;
        row.field_id = field == EnvironmentReferenceField::primary_sky ? 0x19 : 0x1a;
        row.declaration_byte_offset = offset;
        row.reference_name = name;
        if (!name || canonical_reference(*name).empty()) {
            result.environment_references.push_back(std::move(row));
            return;
        }
        ++result.sky_references;
        const auto* type = find_object_type(catalog, *name);
        if (type == nullptr) {
            row.status = EnvironmentReferenceStatus::missing_catalog_object;
            remember(result.unresolved_sky_ids, *name);
            result.environment_references.push_back(std::move(row));
            return;
        }
        ++result.skies_resolved;
        row.catalog_source = type->source;
        const auto& sky_model = chosen_model(*type, map.kind);
        row.model_name = sky_model;
        const auto model_status = resolve_reference(sky_model, ReferenceKind::model, probe);
        if (model_status == ReferenceStatus::resolved) {
            row.status = EnvironmentReferenceStatus::resolved;
            ++result.skies_model_renderable;
        } else if (!sky_model) {
            row.status = EnvironmentReferenceStatus::undeclared_model;
        } else {
            row.status = EnvironmentReferenceStatus::unresolved_model;
            remember(result.unresolved_model_names, *sky_model);
        }
        result.environment_references.push_back(std::move(row));
    };
    for (const auto& environment : map.environments) {
        count_sky(environment, EnvironmentReferenceField::primary_sky,
                  environment.primary_sky, environment.primary_sky_offset);
        count_sky(environment, EnvironmentReferenceField::secondary_sky,
                  environment.secondary_sky, environment.secondary_sky_offset);
        EnvironmentReferenceResolution cloud;
        cloud.environment_ordinal = environment.record_ordinal;
        cloud.field = EnvironmentReferenceField::cloud_texture;
        cloud.field_id = 0x2f;
        cloud.declaration_byte_offset = environment.cloud_texture_offset;
        cloud.reference_name = environment.cloud_texture;
        switch (resolve_reference(environment.cloud_texture, ReferenceKind::texture, probe)) {
        case ReferenceStatus::absent: break;
        case ReferenceStatus::unresolved:
            cloud.status = EnvironmentReferenceStatus::unresolved_texture;
            ++result.texture_references;
            remember(result.unresolved_texture_names, *environment.cloud_texture);
            break;
        case ReferenceStatus::resolved:
            cloud.status = EnvironmentReferenceStatus::resolved;
            ++result.texture_references;
            ++result.textures_resolved;
            break;
        }
        result.environment_references.push_back(std::move(cloud));
    }
    return result;
}


} // namespace eawr::assets
