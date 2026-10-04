#include "space_fog_units.hpp"

#include "family_textures.hpp"

#include "eawr/core/diagnostic.hpp"
#include "eawr/presentation/animation/animation.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <map>
#include <utility>

namespace eawr::presentation::godot_backend {

namespace {

[[nodiscard]] bool ieq(const std::string_view left, const std::string_view right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; };
        return lower(a) == lower(b);
    });
}

// The documented conversion (x, y, z) -> (x, z, -y) is a signed permutation,
// so conjugating the source matrix by it moves raw Q24 values exactly.
[[nodiscard]] sim::math::Mat3x4 source_to_render(const sim::math::Mat3x4& source) {
    using Fixed = sim::math::Fixed;
    constexpr std::array<std::size_t, 3> axis{0, 2, 1};
    constexpr std::array<std::int64_t, 3> sign{1, 1, -1};
    sim::math::Mat3x4 result{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            result.rows[row][column] = Fixed::from_raw(
                sign[row] * sign[column] * source.rows[axis[row]][axis[column]].raw());
        }
        result.rows[row][3] = Fixed::from_raw(sign[row] * source.rows[axis[row]][3].raw());
    }
    return result;
}

[[nodiscard]] double to_double(const sim::math::Fixed value) {
    return static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale);
}

[[nodiscard]] std::array<double, 3> transform_point(const sim::math::Mat3x4& matrix, const std::array<double, 3>& point) {
    std::array<double, 3> result{};
    for (std::size_t row = 0; row < 3; ++row) {
        result[row] = to_double(matrix.rows[row][0]) * point[0] + to_double(matrix.rows[row][1]) * point[1]
            + to_double(matrix.rows[row][2]) * point[2] + to_double(matrix.rows[row][3]);
    }
    return result;
}

} // namespace

bool SpaceFogUnits::compose(GodotRenderer& renderer, const assets::Map& map, const std::string& map_sha256,
                            const vfs::Vfs& filesystem, const data::Catalog& catalog,
                            const std::span<const std::string> admitted_types) {
    scene::VfsAssetCache cache(filesystem);
    scene::BuildInput input;
    input.map = &map;
    input.map_sha256 = map_sha256;
    input.catalog = &catalog;
    input.access = cache.access();
    scene_ = scene::build(input);
    decisions_ = space_fog::classify(*scene_, [&](const std::string_view object_id) -> std::optional<std::string> {
        const data::Definition* definition = catalog.find(object_id, data::Category::game_object);
        if (definition == nullptr) return std::nullopt;
        return definition->type_name;
    }, admitted_types);

    // A failed composition reports no unit: everything uploaded is released
    // and a partly composed unit set is not evidence of anything.
    const auto fail = [&](std::string message) {
        failure_ = std::move(message);
        release(renderer);
        units_.clear();
        return false;
    };
    for (const auto& decision : decisions_) {
        if (decision.decision != space_fog::Decision::admitted_blocked) continue;
        for (const std::string& reason : decision.reasons) {
            unsupported_.push_back("placement " + std::to_string(decision.scene_ordinal) + " " + decision.object_id
                                   + " (" + decision.type_name + "): " + reason);
        }
    }
    if (!unsupported_.empty()) {
        return fail("fog-enabled space capture has admitted placements it cannot compose: " + unsupported_.front());
    }

    sim::AssetId next_asset = first_asset;
    sim::EntityId next_entity = first_entity;
    std::map<std::string, assets::Texture> textures;
    for (std::size_t ordinal = 0; ordinal < decisions_.size(); ++ordinal) {
        if (decisions_[ordinal].decision != space_fog::Decision::admitted) continue;
        const scene::Placement& placement = scene_->placements[ordinal];
        const scene::SceneAsset* asset = nullptr;
        for (const scene::SceneAsset& candidate : scene_->assets) {
            if (candidate.asset_id == placement.asset_id) asset = &candidate;
        }
        const assets::Model* model = asset == nullptr ? nullptr : cache.model(asset->logical_path);
        const std::string identity = "placement " + std::to_string(placement.scene_ordinal) + " "
            + placement.object_id + " (" + decisions_[ordinal].type_name + ")";
        if (model == nullptr) {
            unsupported_.push_back(identity + ": model did not load");
            return fail("admitted space unit model did not load: " + identity);
        }
        Unit unit;
        unit.scene_ordinal = placement.scene_ordinal;
        unit.object_id = placement.object_id;
        unit.type_name = decisions_[ordinal].type_name;
        unit.bounds = {std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest(),
                       std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest()};
        const sim::math::Mat3x4 render_matrix = source_to_render(placement.transform->matrix);
        for (std::size_t index = 0; index < placement.surfaces.size(); ++index) {
            const scene::Surface& surface = placement.surfaces[index];
            const scene::LegacySelector* selector = scene::find_legacy_selector(surface.shader);
            std::optional<std::string> texture_path;
            for (const scene::TextureBinding& binding : surface.textures) {
                if (ieq(binding.parameter, "BaseTexture") && !binding.resolved.empty()) texture_path = binding.resolved;
            }
            const std::string where = identity + " surface " + std::to_string(index) + " shader " + surface.shader;
            if (selector == nullptr || !texture_path) {
                unsupported_.push_back(where + (selector == nullptr ? ": no legacy selector" : ": no resolved BaseTexture"));
                return fail("fog-enabled space capture has an admitted surface it cannot compose: " + unsupported_.back());
            }
            auto found = textures.find(*texture_path);
            if (found == textures.end()) {
                auto decoded = assets::load_texture(filesystem, *texture_path);
                if (!decoded) {
                    unsupported_.push_back(where + ": BaseTexture did not decode: "
                                           + core::format_diagnostic(decoded.error()));
                    return fail("fog-enabled space capture has an admitted surface it cannot compose: "
                                + unsupported_.back());
                }
                found = textures.emplace(*texture_path, std::move(decoded.value())).first;
            }
            const assets::Mesh& source_mesh = model->meshes[surface.mesh_index];
            assets::Model single;
            single.source = model->source;
            single.bones = model->bones;
            assets::Mesh mesh = source_mesh;
            mesh.submeshes = {source_mesh.submeshes[surface.submesh_index]};
            single.meshes.push_back(std::move(mesh));
            const assets::Submesh& submesh = single.meshes.front().submeshes.front();
            MaterialDescription material{
                .schema_version = MaterialDescription::current_schema_version,
                .route = MaterialRoute::legacy_effect,
                .pass = selector->transparent ? RenderPass::transparent : RenderPass::opaque,
                .program = std::string(selector->program),
                .technique = std::string(selector->technique),
                .pass_name = std::string(selector->pass),
                .bindings = {},
            };
            for (const assets::MaterialParameter& parameter : submesh.parameters) {
                material.bindings.push_back({parameter.name, parameter.value});
            }
            const std::vector<GodotRenderer::BindingTexture> family_textures = family_binding_textures(material,
                [&](const std::string_view declared) -> std::optional<assets::Texture> {
                    for (const scene::TextureBinding& binding : surface.textures) {
                        if (binding.declared != declared || binding.resolved.empty()) continue;
                        auto cached = textures.find(binding.resolved);
                        if (cached == textures.end()) {
                            auto decoded = assets::load_texture(filesystem, binding.resolved);
                            if (!decoded) return std::nullopt;
                            cached = textures.emplace(binding.resolved, std::move(decoded.value())).first;
                        }
                        return cached->second;
                    }
                    return std::nullopt;
                });
            const sim::AssetId renderer_asset = next_asset++;
            const auto uploaded = renderer.upload(renderer_asset, single, found->second, material, family_textures);
            if (!uploaded) {
                unsupported_.push_back(where + ": upload failed: " + core::format_diagnostic(uploaded.error()));
                return fail("fog-enabled space capture could not upload an admitted surface: " + unsupported_.back());
            }
            uploaded_.push_back(renderer_asset);
            const auto declared = renderer.declare_fog_consumer(renderer_asset);
            if (!declared) {
                unsupported_.push_back(where + " asset " + std::to_string(renderer_asset) + ": "
                                       + core::format_diagnostic(declared.error()));
                return fail("fog-enabled space capture has an admitted surface that is not a fog consumer: "
                            + unsupported_.back());
            }
            // A static scene draws the bind pose: each rigid mesh at its
            // bone's attachment, with no clip playing.
            std::optional<std::vector<animation::BonePose>> pose;
            const std::int32_t bone = single.meshes.front().bone;
            if (!single.bones.empty() && (bone >= 0 || !submesh.skin_bones.empty())) {
                auto player = animation::Player::create(single);
                if (!player) {
                    unsupported_.push_back(where + ": bind pose player: " + core::format_diagnostic(player.error()));
                    return fail("fog-enabled space capture could not pose an admitted surface: " + unsupported_.back());
                }
                auto sampled = player.value().sample({});
                if (!sampled) {
                    unsupported_.push_back(where + ": bind pose: " + core::format_diagnostic(sampled.error()));
                    return fail("fog-enabled space capture could not pose an admitted surface: " + unsupported_.back());
                }
                pose = std::move(sampled.value().bones);
            }
            const sim::EntityId entity = next_entity++;
            instances_.push_back({entity, renderer_asset, render_matrix});
            unit.assets.push_back(renderer_asset);
            unit.entities.push_back(entity);
            if (pose) {
                const auto posed = renderer.set_skin_pose(entity, renderer_asset, *pose);
                if (!posed) {
                    unsupported_.push_back(where + ": skin pose: " + core::format_diagnostic(posed.error()));
                    return fail("fog-enabled space capture could not pose an admitted surface: " + unsupported_.back());
                }
            }
            // Posed model point -> placed source point -> render basis: the
            // same chain the renderer applies to the snapshot instance.
            std::vector<std::array<double, 3>> placed;
            placed.reserve(submesh.vertices.size());
            for (const assets::Vertex& vertex : submesh.vertices) {
                std::array<double, 3> point{vertex.position.x, vertex.position.y, vertex.position.z};
                if (pose && bone >= 0 && static_cast<std::size_t>(bone) < pose->size()) {
                    // Rigid vertices are stored in their bone's space.
                    const animation::BonePose& posed = (*pose)[static_cast<std::size_t>(bone)];
                    const animation::Matrix& m = submesh.skin_bones.empty() ? posed.model_asset : posed.skin_asset;
                    point = {m[0] * point[0] + m[4] * point[1] + m[8] * point[2] + m[12],
                             m[1] * point[0] + m[5] * point[1] + m[9] * point[2] + m[13],
                             m[2] * point[0] + m[6] * point[1] + m[10] * point[2] + m[14]};
                }
                const std::array<double, 3> world = transform_point(placement.transform->matrix, point);
                unit.bounds.min_x = std::min(unit.bounds.min_x, world[0]);
                unit.bounds.max_x = std::max(unit.bounds.max_x, world[0]);
                unit.bounds.min_y = std::min(unit.bounds.min_y, world[1]);
                unit.bounds.max_y = std::max(unit.bounds.max_y, world[1]);
                placed.push_back(world);
            }
            for (std::size_t first = 0; first + 2 < submesh.indices.size(); first += 3) {
                space::Triangle triangle{};
                bool valid = true;
                for (std::size_t corner = 0; corner < 3; ++corner) {
                    const std::size_t vertex = submesh.indices[first + corner];
                    if (vertex >= placed.size()) {
                        valid = false;
                        break;
                    }
                    const auto& world = placed[vertex];
                    triangle[corner] = {static_cast<float>(world[0]), static_cast<float>(world[2]),
                                        static_cast<float>(-world[1])};
                }
                if (valid) unit.triangles.push_back(triangle);
            }
        }
        units_.push_back(std::move(unit));
    }
    if (units_.empty()) {
        return fail("space fog admitted no drawable placement: the declared --eawr-space-fog-admit types match none");
    }
    return true;
}
} // namespace eawr::presentation::godot_backend
