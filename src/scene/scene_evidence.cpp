#include "eawr/scene/scene.hpp"

#include "scene_internal.hpp"

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <tuple>

namespace eawr::scene {
namespace {

using sim::math::Fixed;

// Text escaping for the canonical form: every byte outside printable ASCII,
// plus space and '%', is percent-encoded so a field boundary is unambiguous.
[[nodiscard]] std::string escape(const std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    if (value.empty()) return "-";
    for (const unsigned char character : value) {
        if (character <= 0x20U || character >= 0x7fU || character == '%') {
            result.push_back('%');
            result.push_back(digits[character >> 4U]);
            result.push_back(digits[character & 15U]);
        } else {
            result.push_back(static_cast<char>(character));
        }
    }
    return result;
}

} // namespace

std::vector<IssueGroup> issue_groups(const Scene& scene) {
    using Key = std::tuple<Cause, std::string, std::string, std::string>;
    std::map<Key, std::uint64_t> counts;
    for (const Placement& placement : scene.placements) {
        for (const Issue& issue : placement.issues) {
            ++counts[{issue.cause, placement.object_id, placement.model_path, issue.detail}];
        }
    }
    std::vector<IssueGroup> result;
    result.reserve(counts.size());
    for (const auto& [key, count] : counts) {
        result.push_back({std::get<0>(key), std::get<1>(key), std::get<2>(key), std::get<3>(key), count});
    }
    return result;
}

std::string canonical_text(const Scene& scene) {
    std::ostringstream out;
    const auto provenance = [&](const Provenance& value) {
        out << ' ' << escape(value.tag) << ' ' << escape(value.source_object_id) << ' '
            << escape(value.logical_path) << ' ' << value.line;
    };
    out << "eawr-static-scene " << scene.contract_version << '\n'
        << "map " << escape(scene.map_logical_path) << ' ' << escape(scene.map_sha256) << ' '
        << escape(scene.map_kind) << '\n';
    for (const SceneAsset& asset : scene.assets) {
        out << "asset " << asset.asset_id << ' ' << escape(asset.logical_path) << ' '
            << escape(asset.sha256) << '\n';
    }
    for (const Placement& placement : scene.placements) {
        out << "placement " << placement.scene_ordinal << ' ' << placement.entity_id << ' '
            << escape(placement.map_logical_path) << ' ' << placement.record_ordinal << ' '
            << (placement.serialized_object_id ? std::to_string(*placement.serialized_object_id) : "-")
            << ' ' << (placement.type_crc ? hex32(*placement.type_crc) : "-") << '\n';
        out << " object " << escape(placement.object_id);
        provenance(placement.object_provenance);
        out << "\n model " << escape(placement.model_declared) << ' ' << escape(placement.model_path)
            << ' ' << placement.asset_id;
        provenance(placement.model_provenance);
        out << "\n scale " << placement.scale_raw << ' ' << (placement.scale_declared ? 1 : 0);
        provenance(placement.scale_provenance);
        out << "\n team_colour " << escape(placement.team_colour_status) << ' '
            << (placement.owner_player ? std::to_string(*placement.owner_player) : "-") << ' '
            << escape(placement.owner_faction);
        if (placement.team_colour) {
            for (const std::uint8_t channel : *placement.team_colour) out << ' ' << static_cast<unsigned>(channel);
        } else {
            out << " - - -";
        }
        provenance(placement.team_colour_provenance);
        out << "\n idle " << escape(placement.idle_animation) << ' '
            << escape(placement.idle_animation_status);
        provenance(placement.idle_animation_provenance);
        out << '\n';
        for (const Surface& surface : placement.surfaces) {
            out << " surface " << surface.mesh_index << ' ' << surface.submesh_index << ' '
                << escape(surface.mesh_name) << ' ' << escape(surface.shader) << ' '
                << (surface.supported ? 1 : 0) << ' ' << escape(surface.technique) << ' '
                << escape(surface.pass) << '\n';
            for (const TextureBinding& texture : surface.textures) {
                out << "  texture " << escape(texture.parameter) << ' ' << escape(texture.declared)
                    << ' ' << escape(texture.resolved) << '\n';
            }
        }
        for (const AttachedEffect& effect : placement.effects) {
            out << " effect " << escape(effect.proxy_name) << ' ' << effect.bone << ' '
                << escape(effect.resolved) << '\n';
        }
        if (placement.transform) {
            out << " transform";
            for (const auto& row : placement.transform->matrix.rows) {
                for (const Fixed value : row) out << ' ' << value.raw();
            }
            out << '\n';
        }
        for (const Issue& issue : placement.issues) {
            out << " issue " << to_string(issue.cause) << ' ' << escape(issue.detail) << '\n';
        }
    }
    return out.str();
}

} // namespace eawr::scene
