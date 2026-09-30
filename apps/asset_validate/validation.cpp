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

std::string format_of(const std::string& path) {
    if (path.ends_with(".alo")) return "alo";
    if (path.ends_with(".ala")) return "ala";
    if (path.ends_with(".dds")) return "dds";
    if (path.ends_with(".tga")) return "tga";
    if (path.ends_with(".ted")) return "ted";
    return {};
}

constexpr std::array<std::uint32_t, 64> sha_k{
    0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
    0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
    0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
    0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
    0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
    0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
    0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
    0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U,
};

void sha_transform(std::array<std::uint32_t, 8>& state, const std::uint8_t* block) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        const auto byte = index * 4;
        words[index] = (std::uint32_t{block[byte]} << 24U) | (std::uint32_t{block[byte + 1]} << 16U)
            | (std::uint32_t{block[byte + 2]} << 8U) | std::uint32_t{block[byte + 3]};
    }
    for (std::size_t index = 16; index < 64; ++index) {
        const auto s0 = std::rotr(words[index - 15], 7) ^ std::rotr(words[index - 15], 18)
            ^ (words[index - 15] >> 3U);
        const auto s1 = std::rotr(words[index - 2], 17) ^ std::rotr(words[index - 2], 19)
            ^ (words[index - 2] >> 10U);
        words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }
    auto a = state[0]; auto b = state[1]; auto c = state[2]; auto d = state[3];
    auto e = state[4]; auto f = state[5]; auto g = state[6]; auto h = state[7];
    for (std::size_t index = 0; index < 64; ++index) {
        const auto t1 = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25))
            + ((e & f) ^ ((~e) & g)) + sha_k[index] + words[index];
        const auto t2 = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22))
            + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

std::string sha256_hex(const std::span<const std::byte> input) {
    std::array<std::uint32_t, 8> state{
        0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,
        0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U,
    };
    std::size_t offset = 0;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(input.data());
    while (input.size() - offset >= 64) {
        sha_transform(state, bytes + offset);
        offset += 64;
    }
    std::array<std::uint8_t, 128> tail{};
    const auto remainder = input.size() - offset;
    if (remainder != 0) std::copy_n(bytes + offset, remainder, tail.begin());
    tail[remainder] = 0x80U;
    const std::size_t padded = remainder < 56 ? 64 : 128;
    const auto bit_count = static_cast<std::uint64_t>(input.size()) * 8U;
    for (unsigned index = 0; index < 8; ++index) {
        tail[padded - 1U - index] = static_cast<std::uint8_t>(bit_count >> (8U * index));
    }
    sha_transform(state, tail.data());
    if (padded == 128) sha_transform(state, tail.data() + 64);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto word : state) {
        for (int shift = 24; shift >= 0; shift -= 8) output << std::setw(2) << ((word >> shift) & 0xffU);
    }
    return output.str();
}

AssetCounts model_counts(const eawr::assets::Model& model) {
    AssetCounts result;
    result.meshes = model.meshes.size();
    result.bones = model.bones.size();
    for (const auto& mesh : model.meshes) {
        result.materials += mesh.submeshes.size();
        for (const auto& submesh : mesh.submeshes) {
            result.vertices += submesh.vertices.size();
            result.indices += submesh.indices.size();
        }
    }
    return result;
}

AssetCounts animation_counts(const eawr::assets::Animation& animation) {
    AssetCounts result;
    result.animations = 1;
    result.bones = animation.tracks.size();
    for (const auto& track : animation.tracks) result.samples += track.samples.size();
    return result;
}

AssetCounts texture_counts(const eawr::assets::Texture& texture) {
    AssetCounts result;
    result.mips = texture.mips.size();
    return result;
}

// The surface effect a declared slot selects, using exactly the presentation
// terrain module's rule so the inventory and the renderer cannot disagree.
std::string terrain_effect_name(const eawr::assets::TerrainMaterial& material) {
    namespace terrain = eawr::presentation::terrain;
    if (!terrain::declared(material.primary_texture)) return "undeclared";
    return std::string(terrain::effect_program(terrain::declared(material.secondary_texture)
        ? terrain::SurfaceEffect::terrain_render_bump_dual
        : terrain::SurfaceEffect::terrain_render_bump_nobump));
}

// Decoded compatibility scalars for --inspect-map: counts, typed notices, the
// six terrain checkpoints and the gated three-axis records.  No file bytes.
std::string map_semantics(const eawr::assets::Map& map) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"volumes\":" << map.volumes.size()
        << ",\"water_textures\":" << map.water_textures.size()
        << ",\"declared_extents\":" << (map.declared_extents ? "true" : "false")
        << ",\"issues\":{";
    std::map<std::string, std::uint64_t> issues;
    for (const eawr::assets::MapNotice& notice : map.issues) {
        ++issues[std::string(eawr::assets::to_string(notice.issue))];
    }
    bool first = true;
    for (const auto& [name, count] : issues) {
        if (!first) out << ',';
        first = false;
        out << json(name) << ':' << count;
    }
    out << '}';
    if (map.terrain && !map.terrain->samples.empty()) {
        const auto& terrain = *map.terrain;
        const auto [low, high] = std::minmax_element(
            terrain.samples.begin(), terrain.samples.end(),
            [](const eawr::assets::TerrainSample& left, const eawr::assets::TerrainSample& right) {
                return left.height_sample < right.height_sample;
            });
        out << ",\"terrain_height_range\":[" << low->height_sample << ',' << high->height_sample << ']';
        const std::size_t width = terrain.width;
        const std::size_t height = terrain.height;
        const std::array<std::size_t, 6> indices{0, 1, width - 1, (height / 2) * width + width / 2,
                                                 (height - 1) * width, height * width - 1};
        out << ",\"terrain_checkpoints\":[";
        for (std::size_t index = 0; index < indices.size(); ++index) {
            const auto& sample = terrain.samples[std::min(indices[index], terrain.samples.size() - 1)];
            if (index != 0) out << ',';
            out << "{\"index\":" << indices[index] << ",\"height\":" << sample.height_sample
                << ",\"material\":" << static_cast<unsigned>(sample.material_slot)
                << ",\"intensity\":" << static_cast<unsigned>(sample.vertex_intensity) << '}';
        }
        out << ']';
    }
    out << ",\"three_axis_records\":[";
    first = true;
    for (const eawr::assets::Placement& placement : map.placements) {
        if (placement.orientation_status != eawr::assets::OrientationStatus::unsupported_three_axis_order
            || !placement.orientation_degrees) continue;
        if (!first) out << ',';
        first = false;
        std::ostringstream crc;
        if (placement.type_crc) {
            crc << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << *placement.type_crc;
        }
        const bool unique = placement.type_resolution == eawr::assets::TypeResolution::unique;
        const auto& degrees = *placement.orientation_degrees;
        out << "{\"ordinal\":" << placement.key.record_ordinal
            << ",\"object_id\":" << placement.serialized_object_id.value_or(0)
            << ",\"type_crc\":" << json(crc.str())
            << ",\"type\":" << json(unique ? placement.type_candidates.front().logical_name : std::string())
            << ",\"position\":[";
        if (placement.position) {
            out << placement.position->x << ',' << placement.position->y << ',' << placement.position->z;
        }
        out << "],\"euler_degrees\":[" << degrees.x << ',' << degrees.y << ',' << degrees.z << "]}";
    }
    out << "]}";
    return out.str();
}

void record_families(const eawr::assets::Map& map,
                     const eawr::assets::ObjectTypeCatalog& catalog,
                     const eawr::assets::AssetProbe& probe,
                     FamilyLedger& families) {
    if (map.kind == eawr::assets::MapKind::space) ++families.space_maps;
    if (!map.water_records.empty()) ++families.maps_with_water_record;
    std::map<std::string, bool> issues;
    for (const eawr::assets::MapNotice& notice : map.issues) {
        const std::string name(eawr::assets::to_string(notice.issue));
        ++families.map_issue_occurrences[name];
        issues[name] = true;
    }
    for (const auto& [name, seen] : issues) if (seen) ++families.map_issue_maps[name];
    if (map.terrain) {
        ++families.maps_with_terrain;
        // A slot that no sample uses is declared but not exercised, so the two
        // are counted separately rather than conflated.
        std::vector<std::uint64_t> used(map.terrain->materials.size(), 0);
        for (const eawr::assets::TerrainSample& sample : map.terrain->samples) {
            if (sample.material_slot < used.size()) ++used[sample.material_slot];
        }
        std::map<std::string, bool> present;
        for (std::size_t index = 0; index < map.terrain->materials.size(); ++index) {
            const std::string effect = terrain_effect_name(map.terrain->materials[index]);
            ++families.terrain_declared_slots[effect];
            if (used[index] != 0) present[effect] = true;
        }
        for (const auto& [effect, seen] : present) {
            if (seen) ++families.terrain_effects[effect];
        }
    }
    std::map<std::string, bool> skies;
    std::map<std::string, bool> models;
    for (const eawr::assets::EnvironmentDescriptor& environment : map.environments) {
        for (const std::optional<std::string>* field :
             {&environment.primary_sky, &environment.secondary_sky}) {
            if (!field->has_value() || (*field)->empty()) continue;
            const eawr::assets::ObjectTypeRef* type =
                eawr::assets::find_object_type(catalog, **field);
            if (type == nullptr) continue;
            skies[**field] = true;
            const std::optional<std::string>& declared =
                map.kind == eawr::assets::MapKind::space && type->space_model_name
                    ? type->space_model_name
                    : (type->land_model_name ? type->land_model_name : type->model_name);
            if (!declared || declared->empty()) continue;
            // Record the model the sky object actually resolves to, so the
            // inventory names art that exists rather than an authored string.
            if (eawr::assets::resolve_reference(
                    declared, eawr::assets::ReferenceKind::model, probe)
                != eawr::assets::ReferenceStatus::resolved) continue;
            models[*declared] = true;
        }
    }
    if (!skies.empty()) ++families.maps_with_sky_reference;
    for (const auto& [name, seen] : skies) if (seen) ++families.skydome_objects[name];
    for (const auto& [name, seen] : models) if (seen) ++families.skydome_models[name];
}

AssetCounts map_counts(const eawr::assets::Map& map,
                       const eawr::assets::ObjectTypeCatalog& catalog,
                       const eawr::assets::AssetProbe& probe,
                       UnresolvedLedger& ledger) {
    AssetCounts result;
    result.maps = 1;
    result.terrain_samples = map.terrain ? map.terrain->samples.size() : 0;
    result.placements = map.placements.size();
    result.semantic_maps = map.semantic_complete ? 1 : 0;
    const auto resolution = eawr::assets::resolve_map(map, catalog, probe);
    result.placements_crc_absent = resolution.crc_absent;
    result.placements_crc_missing = resolution.crc_missing;
    result.placements_crc_collision = resolution.crc_collision;
    result.placements_crc_unique = resolution.crc_unique;
    result.placements_model_renderable = resolution.model_chain_renderable;
    result.placements_model_unresolved = resolution.model_chain_unresolved;
    result.placements_model_undeclared = resolution.model_chain_undeclared;
    result.placements_model_unknown = resolution.model_chain_unknown;
    result.texture_references = resolution.texture_references;
    result.textures_resolved = resolution.textures_resolved;
    result.texture_slots_untextured = resolution.texture_slots_untextured;
    result.sky_references = resolution.sky_references;
    result.skies_resolved = resolution.skies_resolved;
    result.skies_model_renderable = resolution.skies_model_renderable;
    for (const auto crc : resolution.unresolved_type_crcs) ++ledger.type_crcs[crc];
    for (const auto& name : resolution.unresolved_model_names) ++ledger.model_names[name];
    for (const auto& name : resolution.unresolved_texture_names) ++ledger.texture_names[name];
    for (const auto& name : resolution.unresolved_sky_ids) ++ledger.sky_ids[name];
    return result;
}

void add_counts(AssetCounts& target, const AssetCounts& source) {
    target.meshes += source.meshes;
    target.bones += source.bones;
    target.materials += source.materials;
    target.animations += source.animations;
    target.vertices += source.vertices;
    target.indices += source.indices;
    target.mips += source.mips;
    target.samples += source.samples;
    target.maps += source.maps;
    target.terrain_samples += source.terrain_samples;
    target.placements += source.placements;
    target.semantic_maps += source.semantic_maps;
    target.placements_crc_absent += source.placements_crc_absent;
    target.placements_crc_missing += source.placements_crc_missing;
    target.placements_crc_collision += source.placements_crc_collision;
    target.placements_crc_unique += source.placements_crc_unique;
    target.placements_model_renderable += source.placements_model_renderable;
    target.placements_model_unresolved += source.placements_model_unresolved;
    target.placements_model_undeclared += source.placements_model_undeclared;
    target.placements_model_unknown += source.placements_model_unknown;
    target.texture_references += source.texture_references;
    target.textures_resolved += source.textures_resolved;
    target.texture_slots_untextured += source.texture_slots_untextured;
    target.sky_references += source.sky_references;
    target.skies_resolved += source.skies_resolved;
    target.skies_model_renderable += source.skies_model_renderable;
}

std::string affected_feature(const std::string_view format, const eawr::core::Diagnostic& diagnostic) {
    if (format == "alo" && diagnostic.code == eawr::assets::diagnostic_codes::unsupported
        && diagnostic.message.find("particle-system") != std::string::npos) return "particle_system";
    if ((format == "dds" || format == "tga") && diagnostic.code == eawr::assets::diagnostic_codes::limit
        && diagnostic.message.find("volume") != std::string::npos) return "volume_texture";
    if (format == "dds" || format == "tga") return "texture_2d";
    if (format == "ala") return "animation";
    if (format == "alo") return "model";
    if (format == "ted") return "map";
    return "asset_acquisition";
}

std::string follow_up(const std::string_view format, const eawr::core::Diagnostic& diagnostic) {
    if (format == "alo" && diagnostic.code == eawr::assets::diagnostic_codes::unsupported
        && diagnostic.message.find("particle-system") != std::string::npos) {
        return "Add a separately specified ParticleSystem CPU API; do not coerce this payload into Model.";
    }
    if ((format == "dds" || format == "tga") && diagnostic.code == eawr::assets::diagnostic_codes::limit
        && diagnostic.message.find("volume") != std::string::npos) {
        return "Extend Texture with explicit volume depth and slice semantics before accepting this resource.";
    }
    if ((format == "dds" || format == "tga")
        && diagnostic.code == eawr::assets::diagnostic_codes::texture_format) {
        return "Correct the source suffix/content mismatch or add a documented representable texture layout.";
    }
    if (format.empty()) {
        return "Repair the declared VFS source so the exact enumerated record can be read, then rerun validation.";
    }
    return "Repair or replace the malformed source asset, then rerun validation without suppressing this record.";
}

std::map<std::string, Aggregate> aggregate(const std::vector<AssetOutcome>& outcomes) {
    std::map<std::string, Aggregate> result;
    for (const std::string_view format : {"alo", "ala", "dds", "tga", "ted"}) {
        result.emplace(std::string(format), Aggregate{});
    }
    for (const auto& outcome : outcomes) {
        auto& current = result[outcome.format];
        ++current.discovered;
        if (outcome.loaded) ++current.loaded;
        else ++current.failed;
        current.notices += outcome.notices;
        add_counts(current.counts, outcome.counts);
    }
    return result;
}

} // namespace asset_validate_internal
