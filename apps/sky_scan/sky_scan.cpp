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

ShaderIdentity shader_identity(const std::string_view shader) {
    std::string_view stem = shader;
    if (stem.size() >= 3 && ieq(stem.substr(stem.size() - 3), ".fx")) stem.remove_suffix(3);
    for (const auto& effect : sky_scan_descriptors::effects) {
        if (!stem.empty() && ieq(effect.name, stem)) {
            return {std::string(effect.name), std::string(effect.name), std::string(effect.family)};
        }
    }
    return {lower(std::string(stem)), std::nullopt, std::string(not_in_bundle)};
}

Chain chain_class(const assets::Model& model, const std::int32_t start) {
    if (start < -1) return {"invalid", std::nullopt};
    bool identity = true;
    bool hidden = false;
    bool rigid = true;
    std::optional<std::uint32_t> billboard;
    std::int32_t current = start;
    for (std::size_t steps = 0; current >= 0; ++steps) {
        if (steps > model.bones.size() || static_cast<std::size_t>(current) >= model.bones.size()) {
            return {"invalid", std::nullopt};
        }
        const assets::Bone& bone = model.bones[static_cast<std::size_t>(current)];
        if (bone.billboard != 0 && !billboard) billboard = bone.billboard;
        if (!bone.visible) hidden = true;
        if (!proper_rigid(bone.relative_transform)) rigid = false;
        if (!identity_transform(bone.relative_transform)) identity = false;
        if (bone.parent < -1) return {"invalid", std::nullopt};
        current = bone.parent;
    }
    if (billboard) return {"billboard:" + std::string(billboard_label(*billboard)), billboard};
    if (hidden) return {"hidden_bone", std::nullopt};
    if (!rigid) return {"non_rigid", std::nullopt};
    return {identity ? "identity" : "rigid_static", std::nullopt};
}

namespace assets = eawr::assets;
namespace space = eawr::presentation::space;

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

char fold(const char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

std::string lower(std::string value) {
    for (char& character : value) character = fold(character);
    return value;
}

bool ieq(const std::string_view left, const std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (fold(left[index]) != fold(right[index])) return false;
    }
    return true;
}

// The same data-root rule and profile layer order as asset_validate: the
// Remake mod leaf, then the FoC expansion, then the EaW base.
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

// Authored names are arbitrary bytes. Every byte outside printable ASCII is
// escaped as its Latin-1 code point so the report is always valid UTF-8 JSON.
std::string json(const std::string_view value) {
    std::string output;
    output.reserve(value.size() + 2);
    output.push_back('"');
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20U || character >= 0x7fU) {
                output += "\\u00";
                output.push_back(hex[character >> 4U]);
                output.push_back(hex[character & 15U]);
            } else {
                output.push_back(static_cast<char>(character));
            }
        }
    }
    output.push_back('"');
    return output;
}

std::string sha256_hex(const std::span<const std::byte> bytes) {
    return eawr::core::sha256_hex(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

// -- reference resolution ------------------------------------------------------
//
// These mirror assets::resolve_reference exactly (canonical lower-case
// forward-slash name; the literal suffixed name first, then the stem with each
// known suffix) so the path recorded here is the one that resolution found.

std::string canonical_reference(const std::string_view name) {
    std::string result = lower(std::string(name));
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

std::string_view billboard_label(const std::uint32_t mode) {
    return mode < billboard_labels.size() ? billboard_labels[mode] : std::string_view("unknown");
}

// The planner's proper-rigid test (orthonormal columns, determinant +1,
// tolerance 1e-4), repeated here so the class never disagrees with it.
bool proper_rigid(const std::array<float, 12>& m) {
    if (!std::all_of(m.begin(), m.end(), [](const float v) { return std::isfinite(v); })) return false;
    const std::array<std::array<float, 3>, 3> axes{{{m[0], m[4], m[8]}, {m[1], m[5], m[9]}, {m[2], m[6], m[10]}}};
    const auto dot = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    constexpr float tolerance = 1.0e-4F;
    for (std::size_t i = 0; i < 3; ++i) {
        if (std::abs(dot(axes[i], axes[i]) - 1.0F) > tolerance) return false;
        for (std::size_t j = 0; j < i; ++j) {
            if (std::abs(dot(axes[i], axes[j])) > tolerance) return false;
        }
    }
    const auto& a = axes[0];
    const auto& b = axes[1];
    const auto& c = axes[2];
    const float determinant = a[0] * (b[1] * c[2] - b[2] * c[1])
        + a[1] * (b[2] * c[0] - b[0] * c[2]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
    return std::abs(determinant - 1.0F) <= tolerance;
}

bool identity_transform(const std::array<float, 12>& m) {
    constexpr std::array<float, 12> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    for (std::size_t index = 0; index < m.size(); ++index) {
        if (m[index] != identity[index]) return false;
    }
    return true;
}

std::string_view kind_name(const assets::ParameterKind kind) {
    switch (kind) {
    case assets::ParameterKind::integer: return "integer";
    case assets::ParameterKind::scalar: return "scalar";
    case assets::ParameterKind::vector3: return "vector3";
    case assets::ParameterKind::vector4: return "vector4";
    case assets::ParameterKind::texture: return "texture";
    }
    return "unknown";
}

// -- records ---------------------------------------------------------------------

std::string_view reference_status(const assets::EnvironmentReferenceStatus status) {
    using S = assets::EnvironmentReferenceStatus;
    switch (status) {
    case S::undeclared: return "undeclared";
    case S::missing_catalog_object: return "missing_catalog_object";
    case S::undeclared_model: return "undeclared_model";
    case S::unresolved_model: return "unresolved_model";
    case S::unresolved_texture: return "unresolved_texture";
    case S::resolved: return "resolved";
    }
    return "unknown";
}

void describe_model(ModelRow& row, Resolver& resolver) {
    const assets::Model& model = *row.model;
    row.bones = model.bones.size();
    row.meshes = model.meshes.size();
    for (std::size_t mesh_index = 0; mesh_index < model.meshes.size(); ++mesh_index) {
        const assets::Mesh& mesh = model.meshes[mesh_index];
        const Chain chain = chain_class(model, mesh.bone);
        for (std::size_t submesh_index = 0; submesh_index < mesh.submeshes.size(); ++submesh_index) {
            const assets::Submesh& submesh = mesh.submeshes[submesh_index];
            SurfaceRow surface;
            surface.mesh_index = mesh_index;
            surface.submesh_index = submesh_index;
            surface.mesh_name = mesh.name;
            surface.mesh_visible = mesh.visible;
            surface.bone = mesh.bone;
            surface.chain = chain;
            surface.skinned = !submesh.skin_bones.empty();
            surface.skin_bones = submesh.skin_bones.size();
            surface.vertex_format = submesh.vertex_format;
            surface.shader = submesh.shader;
            surface.identity = shader_identity(submesh.shader);
            surface.vertices = submesh.vertices.size();
            surface.triangles = submesh.indices.size() / 3;
            for (const assets::MaterialParameter& parameter : submesh.parameters) {
                surface.parameters.push_back(parameter.name + ":" + std::string(kind_name(parameter.kind)));
                if (parameter.kind != assets::ParameterKind::texture) continue;
                const auto* name = std::get_if<std::string>(&parameter.value);
                TextureRow texture;
                texture.parameter = parameter.name;
                texture.name = name != nullptr ? *name : std::string();
                if (canonical_reference(texture.name).empty()) {
                    texture.status = "undeclared";
                } else if (auto found = resolver.texture(texture.name)) {
                    texture.status = "resolved";
                    texture.resolved = std::move(found);
                } else {
                    texture.status = "not_in_vfs";
                }
                if (ieq(parameter.name, "BaseTexture") && texture.status != "undeclared") {
                    row.base_textures.insert(canonical_reference(texture.name));
                }
                surface.textures.push_back(std::move(texture));
            }
            row.surfaces.push_back(std::move(surface));
        }
    }
}

void attach_plan(ModelRow& row) {
    const auto plans = space::plan_surfaces(*row.model, space::qualifications());
    for (const space::SurfacePlan& plan : plans) {
        for (SurfaceRow& surface : row.surfaces) {
            if (surface.mesh_index != plan.mesh_index || surface.submesh_index != plan.submesh_index) continue;
            surface.plan_status = std::string(space::to_string(plan.status));
            for (const auto cause : plan.causes) surface.plan_causes.emplace_back(space::to_string(cause));
        }
    }
}

// -- aggregates ------------------------------------------------------------------

void add_usage(Tally& tally, const ModelRow& model) {
    tally.models.insert(model.source.logical_path);
    tally.maps_primary.insert(model.usage.primary_maps.begin(), model.usage.primary_maps.end());
    tally.maps_secondary.insert(model.usage.secondary_maps.begin(), model.usage.secondary_maps.end());
    tally.land_maps.insert(model.usage.land_maps.begin(), model.usage.land_maps.end());
    tally.space_maps.insert(model.usage.space_maps.begin(), model.usage.space_maps.end());
}

// A format-name observation only: the stored vertex format name ends in a
// capital C (as in alD3dVertNU2C). No vertex-colour semantics are claimed.
bool colour_format_name(const std::string_view name) {
    return !name.empty() && name.back() == 'C';
}

Totals totals(const std::map<std::string, ModelRow>& models) {
    Totals result;
    for (const auto& [path, model] : models) {
        if (model.status != "loaded") continue;
        if (model.surfaces.empty()) result.models_without_surfaces.insert(path);
        std::set<std::string> shaders_here;
        std::set<std::string> families_here;
        for (const SurfaceRow& surface : model.surfaces) {
            ++result.surfaces;
            Tally& shader = result.shaders[surface.identity.key];
            Tally& family = result.families[surface.identity.family];
            shader.families.insert(surface.identity.family);
            ++shader.surfaces;
            ++family.surfaces;
            if (surface.mesh_visible) {
                ++shader.visible_surfaces;
                ++family.visible_surfaces;
            } else {
                ++result.hidden_mesh_surfaces;
            }
            shaders_here.insert(surface.identity.key);
            families_here.insert(surface.identity.family);
            ++result.vertex_formats[surface.vertex_format];
            ++result.chain_classes[surface.chain.label];
            if (surface.chain.billboard_mode) ++result.billboard_surfaces;
            if (surface.skinned) ++result.skinned_surfaces;
            if (colour_format_name(surface.vertex_format)) ++result.vertex_colour_format_surfaces;
            if (surface.textures.size() > 1) ++result.multi_texture_surfaces;
            for (const TextureRow& texture : surface.textures) {
                if (texture.status == "undeclared") continue;
                ++result.textures_declared;
                if (texture.status == "resolved") ++result.textures_resolved;
                else ++result.textures_not_in_vfs;
            }
        }
        for (const auto& key : shaders_here) add_usage(result.shaders[key], model);
        for (const auto& key : families_here) add_usage(result.families[key], model);
        if (!model.usage.land_maps.empty() && model.base_textures.size() > 1) {
            result.land_multi_base_texture_models.insert(path);
            result.land_multi_base_texture_maps.insert(
                model.usage.land_maps.begin(), model.usage.land_maps.end());
        }
    }
    return result;
}

// -- output ----------------------------------------------------------------------

} // namespace sky_scan_internal
