#include "land_look.hpp"
#include "land_look_shaders.hpp"

#include "eawr/presentation/camera/constants_source.hpp"
#include "eawr/presentation/camera/controller.hpp"
#include "eawr/sim/replay.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <variant>

namespace eawr::presentation::godot_backend::land_look {
namespace {

[[nodiscard]] core::Result<void> fail(const std::string_view code, std::string message) {
    return core::Result<void>::failure(core::Diagnostic{
        .code = std::string(code), .severity = core::Severity::error, .message = std::move(message),
        .logical_path = std::nullopt, .line = std::nullopt, .column = std::nullopt, .source_id = std::nullopt});
}

[[nodiscard]] bool ieq(const std::string_view left, const std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](const char value) {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
        };
        if (fold(left[index]) != fold(right[index])) return false;
    }
    return true;
}

[[nodiscard]] sim::math::Mat3x4 identity() {
    using Fixed = sim::math::Fixed;
    const Fixed zero = Fixed::from_raw(0);
    const Fixed one = Fixed::from_raw(Fixed::scale);
    sim::math::Mat3x4 result{};
    result.rows[0] = {one, zero, zero, zero};
    result.rows[1] = {zero, one, zero, zero};
    result.rows[2] = {zero, zero, one, zero};
    return result;
}

[[nodiscard]] assets::Texture flat_texture(const std::uint8_t r, const std::uint8_t g, const std::uint8_t b) {
    assets::MipLevel mip;
    mip.width = 1;
    mip.height = 1;
    mip.row_pitch = 4;
    mip.bytes = {std::byte{r}, std::byte{g}, std::byte{b}, std::byte{255}};
    assets::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.format = assets::PixelFormat::rgba8;
    texture.mips.push_back(std::move(mip));
    return texture;
}

[[nodiscard]] std::uint32_t power_of_two_at_least(const std::uint32_t value) {
    std::uint32_t result = 1;
    while (result < value && result < (1U << 30U)) result <<= 1U;
    return result;
}

void put_u16(std::vector<std::byte>& bytes, const std::size_t offset, const double value) {
    const auto clamped = static_cast<std::uint32_t>(std::clamp(std::lround(value), 0L, 65535L));
    bytes[offset] = std::byte{static_cast<std::uint8_t>(clamped >> 8U)};
    bytes[offset + 1] = std::byte{static_cast<std::uint8_t>(clamped & 0xffU)};
}

[[nodiscard]] const assets::MaterialParameter* parameter(const assets::Submesh& submesh, const std::string_view name) {
    for (const assets::MaterialParameter& entry : submesh.parameters) {
        if (ieq(entry.name, name)) return &entry;
    }
    return nullptr;
}

[[nodiscard]] std::optional<std::string> texture_parameter(const assets::Submesh& submesh, const std::string_view name) {
    const auto* found = parameter(submesh, name);
    if (found == nullptr) return std::nullopt;
    const auto* value = std::get_if<std::string>(&found->value);
    if (value == nullptr || value->empty()) return std::nullopt;
    return *value;
}

// Bone transforms are three float4 columns for vector-left multiplication:
// element 3, 7 and 11 are the translation (assets::Bone).
using Rigid = std::array<float, 12>;
constexpr Rigid identity_rigid{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

[[nodiscard]] assets::Vec3f rigid_point(const Rigid& m, const assets::Vec3f v) {
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3],
            m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7],
            m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11]};
}

[[nodiscard]] Rigid compose(const Rigid& parent, const Rigid& child) {
    Rigid result{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            float value = 0.0F;
            for (int k = 0; k < 3; ++k) value += parent[row * 4 + k] * child[k * 4 + column];
            if (column == 3) value += parent[row * 4 + 3];
            result[row * 4 + column] = value;
        }
    }
    return result;
}

// Composes the bind chain from `bone` to the root; nullopt for a malformed
// chain.
[[nodiscard]] std::optional<Rigid> chain(const assets::Model& model, const std::int32_t bone) {
    Rigid result = identity_rigid;
    std::int32_t current = bone;
    for (std::size_t steps = 0; current >= 0; ++steps) {
        if (steps > model.bones.size() || static_cast<std::size_t>(current) >= model.bones.size()) {
            return std::nullopt;
        }
        const assets::Bone& record = model.bones[static_cast<std::size_t>(current)];
        result = compose(record.relative_transform, result);
        current = record.parent;
    }
    return result;
}

[[nodiscard]] assets::Vec3f normalized(const assets::Vec3f v) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!(length > 0.0F) || !std::isfinite(length)) return {0.0F, 0.0F, 1.0F};
    return {v.x / length, v.y / length, v.z / length};
}

} // namespace

std::string with_map_fog(std::string shader) {
    const bool has_vertex = shader.find("void vertex() {") != std::string::npos;
    constexpr std::string_view declarations = R"GODOT(
uniform sampler2D eawr_fog_texture : filter_nearest, repeat_disable;
uniform vec2 eawr_fog_origin = vec2(0.0);
uniform vec2 eawr_fog_extent = vec2(1.0);
uniform vec2 eawr_fog_size = vec2(1.0);
uniform bool eawr_fog_bound = false;
varying vec3 eawr_fog_world_position;
float eawr_fog_attenuation() {
    vec2 source = vec2(eawr_fog_world_position.x, -eawr_fog_world_position.z);
    vec2 uv = (source - eawr_fog_origin) / eawr_fog_extent;
    if (!eawr_fog_bound || uv.x < 0.0 || uv.y < 0.0 || uv.x >= 1.0 || uv.y >= 1.0) return 0.0;
    vec2 texel = min(floor(uv * eawr_fog_size), eawr_fog_size - vec2(1.0));
    return texture(eawr_fog_texture, (texel + vec2(0.5)) / eawr_fog_size).r;
}
)GODOT";
    const std::size_t declaration_point = shader.find(has_vertex ? "void vertex() {" : "void fragment() {");
    shader.insert(declaration_point, declarations);
    if (has_vertex) {
        const std::size_t vertex = shader.find("void vertex() {") + std::string_view("void vertex() {\n").size();
        shader.insert(vertex, "    eawr_fog_world_position = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;\n");
    } else {
        const std::size_t vertex = shader.find("void fragment() {");
        shader.insert(vertex, "void vertex() {\n    eawr_fog_world_position = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;\n}\n");
    }
    const std::size_t albedo = shader.find("    ALBEDO = ");
    const std::size_t statement_end = shader.find(';', albedo);
    shader.insert(statement_end, " * eawr_fog_attenuation()");
    return shader;
}

std::string terrain_shader(const bool lit) {
    std::string shader;
    if (lit) {
        shader = "shader_type spatial;\nrender_mode ambient_light_disabled, cull_disabled;\n";
        shader += blend_declarations;
        shader += R"GODOT(uniform mat4 eawr_sph_r;
uniform mat4 eawr_sph_g;
uniform mat4 eawr_sph_b;
uniform vec3 eawr_shadow_floor = vec3(0.5);
varying vec3 eawr_irradiance;
void vertex() {
    vec3 world_position = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    eawr_source_position = vec3(world_position.x, -world_position.z, world_position.y);
    vec3 normal_world = normalize(mat3(MODEL_MATRIX) * NORMAL);
    vec4 normal_h = vec4(normal_world, 1.0);
    eawr_irradiance = vec3(
        dot(normal_h, eawr_sph_r * normal_h),
        dot(normal_h, eawr_sph_g * normal_h),
        dot(normal_h, eawr_sph_b * normal_h));
}
void fragment() {
    ALBEDO = 2.0 * eawr_irradiance * eawr_terrain_albedo();
}
void light() {
    DIFFUSE_LIGHT += mix(eawr_shadow_floor, vec3(1.0), ATTENUATION);
}
)GODOT";
    } else {
        shader = "shader_type spatial;\nrender_mode unshaded, cull_disabled;\n";
        shader += blend_declarations;
        shader += R"GODOT(void vertex() {
    vec3 world_position = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    eawr_source_position = vec3(world_position.x, -world_position.z, world_position.y);
}
void fragment() {
    ALBEDO = eawr_terrain_albedo();
}
)GODOT";
    }
    return shader;
}

core::Result<std::vector<MaterialBinding>> register_terrain_blend(
    GodotRenderer& renderer, const terrain::BlendMap& blend,
    const std::vector<assets::TerrainMaterial>& materials, const TextureLookup& lookup,
    TerrainBlendReport& report) {
    using Result = core::Result<std::vector<MaterialBinding>>;
    if (blend.layers.empty() || blend.width < 2 || blend.height < 2
        || blend.sample_layers.size() != static_cast<std::size_t>(blend.width) * blend.height) {
        return Result::failure(fail("EAWR-TERRAIN-0004", "terrain blend map is empty or inconsistent").error());
    }
    std::vector<assets::Texture> layers;
    layers.reserve(blend.layers.size());
    std::uint32_t largest = 64;
    for (const terrain::BlendLayer& layer : blend.layers) {
        std::optional<assets::Texture> texture;
        if (layer.slot < materials.size() && terrain::declared(materials[layer.slot].primary_texture)) {
            texture = lookup(*materials[layer.slot].primary_texture);
        }
        if (!texture) {
            ++report.unresolved_layers;
            texture = flat_texture(190, 190, 190);
        } else {
            largest = std::max({largest, texture->width, texture->height});
        }
        layers.push_back(std::move(*texture));
    }
    const std::uint32_t edge = std::min(power_of_two_at_least(largest), 1024U);

    assets::Texture index;
    index.width = blend.width;
    index.height = blend.height;
    index.format = assets::PixelFormat::l8;
    assets::MipLevel index_mip;
    index_mip.width = blend.width;
    index_mip.height = blend.height;
    index_mip.row_pitch = blend.width;
    index_mip.bytes.resize(blend.sample_layers.size());
    std::memcpy(index_mip.bytes.data(), blend.sample_layers.data(), blend.sample_layers.size());
    index.mips.push_back(std::move(index_mip));

    // Row 0: tile size x16 and rotation as two 16-bit values; row 1: tilt as
    // a 16-bit value; row 2: tint. Both angles are wrapped into [0, 2pi). The
    // adapter decodes them with texelFetch (eawr_u16) and rebuilds
    // terrain::retail_texgen from them.
    assets::Texture params;
    params.width = static_cast<std::uint32_t>(blend.layers.size());
    params.height = 3;
    params.format = assets::PixelFormat::rgba8;
    assets::MipLevel params_mip;
    params_mip.width = params.width;
    params_mip.height = 3;
    params_mip.row_pitch = params.width * 4;
    params_mip.bytes.assign(static_cast<std::size_t>(params.width) * 3 * 4, std::byte{0});
    constexpr double two_pi = 6.283185307179586;
    const auto wrapped = [](const float angle) {
        const double turn = std::isfinite(angle) ? std::fmod(static_cast<double>(angle), two_pi) : 0.0;
        return turn < 0.0 ? turn + two_pi : turn;
    };
    for (std::size_t layer = 0; layer < blend.layers.size(); ++layer) {
        const terrain::LayerMapping& mapping = blend.layers[layer].mapping;
        const std::size_t row0 = layer * 4;
        const std::size_t row1 = (params.width + layer) * 4;
        const std::size_t row2 = (static_cast<std::size_t>(params.width) * 2 + layer) * 4;
        put_u16(params_mip.bytes, row0, static_cast<double>(mapping.tile_size) * 16.0);
        put_u16(params_mip.bytes, row0 + 2, wrapped(mapping.rotation) / two_pi * 65535.0);
        put_u16(params_mip.bytes, row1, wrapped(mapping.tilt) / two_pi * 65535.0);
        params_mip.bytes[row1 + 3] = std::byte{255};
        const auto channel = [](const float value) {
            return std::byte{static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F))};
        };
        params_mip.bytes[row2] = channel(mapping.tint.x);
        params_mip.bytes[row2 + 1] = channel(mapping.tint.y);
        params_mip.bytes[row2 + 2] = channel(mapping.tint.z);
        params_mip.bytes[row2 + 3] = std::byte{255};
    }
    params.mips.push_back(std::move(params_mip));

    if (auto registered = renderer.register_shared_texture_array("terrain-layers", layers, edge); !registered) {
        return Result::failure(registered.error());
    }
    if (auto registered = renderer.register_shared_texture("terrain-layer-index", index); !registered) {
        return Result::failure(registered.error());
    }
    if (auto registered = renderer.register_shared_texture("terrain-layer-params", params); !registered) {
        return Result::failure(registered.error());
    }
    report.layers = blend.layers.size();
    report.layer_edge = edge;
    return Result::success({
        {"eawr_layers", std::string{"shared:terrain-layers"}},
        {"eawr_layer_index", std::string{"shared:terrain-layer-index"}},
        {"eawr_layer_params", std::string{"shared:terrain-layer-params"}},
        {"eawr_grid", assets::Vec4f{static_cast<float>(blend.width), static_cast<float>(blend.height),
                                    blend.cell_spacing, 0.0F}},
    });
}

std::optional<assets::Vec3f> toward_sun(const assets::EnvironmentDescriptor& environment) {
    std::optional<float> heading;
    std::optional<float> elevation;
    for (const assets::RawField& field : environment.fields) {
        if (field.bytes.size() != 4 || (field.id != 0x08 && field.id != 0x0b)) continue;
        float value{};
        std::uint32_t raw{};
        for (std::size_t index = 0; index < 4; ++index) {
            raw |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(field.bytes[index])) << (8U * index);
        }
        std::memcpy(&value, &raw, sizeof(value));
        if (!std::isfinite(value)) continue;
        if (field.id == 0x08) heading = value;
        else elevation = value;
    }
    if (!heading || !elevation) return std::nullopt;
    return assets::Vec3f{std::sin(*heading) * std::cos(*elevation),
                         -std::cos(*heading) * std::cos(*elevation), std::sin(*elevation)};
}

core::Result<SkyComposition> compose_sky(
    GodotRenderer& renderer, const assets::Model& model, const TextureLookup& lookup,
    const SkyInputs& inputs, sim::AssetId& next_asset, sim::EntityId& next_entity,
    std::vector<sim::RenderInstance>& instances) {
    using Result = core::Result<SkyComposition>;
    SkyComposition result;
    // Far enough that the instance's bounds always meet the view frustum; the
    // vertex stage keeps only the direction.
    constexpr float far_out = 100000.0F;
    std::map<std::string, std::string> cloud_names;
    for (std::size_t mesh_index = 0; mesh_index < model.meshes.size(); ++mesh_index) {
        const assets::Mesh& mesh = model.meshes[mesh_index];
        for (std::size_t submesh_index = 0; submesh_index < mesh.submeshes.size(); ++submesh_index) {
            const assets::Submesh& submesh = mesh.submeshes[submesh_index];
            const std::string label = mesh.name + "/" + std::to_string(submesh_index) + " (" + submesh.shader + ")";
            const auto skip = [&](const std::string& why) {
                ++result.skipped_surfaces;
                result.skipped.push_back(label + ": " + why);
            };
            if (!mesh.visible) {
                skip("mesh is stored hidden");
                continue;
            }
            const auto placed = chain(model, mesh.bone);
            if (!placed) {
                skip("bone chain is malformed");
                continue;
            }
            const bool sun_bone = mesh.bone >= 0
                && model.bones[static_cast<std::size_t>(mesh.bone)].billboard == 7U;
            assets::Model upload;
            upload.source = model.source;
            assets::Mesh out;
            out.name = mesh.name;
            out.visible = true;
            assets::Submesh surface;
            surface.shader = submesh.shader;
            surface.indices = submesh.indices;
            assets::Texture own = flat_texture(0, 0, 0);
            std::vector<MaterialBinding> bindings{{"eawr_diffuse", std::string{"asset"}}};
            std::string program;
            RenderPass pass = RenderPass::opaque;
            if (ieq(submesh.shader, "Skydome.fx") && !sun_bone) {
                const assets::Vec3f centre = rigid_point(*placed, {0.0F, 0.0F, 0.0F});
                for (const assets::Vertex& vertex : submesh.vertices) {
                    assets::Vertex baked = vertex;
                    const assets::Vec3f world = rigid_point(*placed, vertex.position);
                    const assets::Vec3f direction = normalized({world.x - centre.x, world.y - centre.y,
                                                                world.z - centre.z});
                    baked.position = {direction.x * far_out, direction.y * far_out, direction.z * far_out};
                    baked.normal = {-direction.x, -direction.y, -direction.z};
                    surface.vertices.push_back(baked);
                }
                // Authored ground skies are upper domes. An overview eye
                // looks down through their open underside; mirror the mesh so
                // the terrain's surroundings use the same sky instead of the
                // viewport clear colour. Keep the source UVs at the join.
                if (surface.vertices.size() * 2 <= std::numeric_limits<std::uint16_t>::max()) {
                    const auto count = static_cast<std::uint16_t>(surface.vertices.size());
                    for (std::uint16_t index = 0; index < count; ++index) {
                        assets::Vertex lower = surface.vertices[index];
                        lower.position.z = -lower.position.z;
                        lower.normal.z = -lower.normal.z;
                        surface.vertices.push_back(lower);
                    }
                    const auto triangles = surface.indices.size();
                    for (std::size_t index = 0; index + 2 < triangles; index += 3) {
                        surface.indices.push_back(static_cast<std::uint16_t>(surface.indices[index] + count));
                        surface.indices.push_back(static_cast<std::uint16_t>(surface.indices[index + 2] + count));
                        surface.indices.push_back(static_cast<std::uint16_t>(surface.indices[index + 1] + count));
                    }
                }
                if (const auto name = texture_parameter(submesh, "BaseTexture")) {
                    if (auto texture = lookup(*name)) own = std::move(*texture);
                }
                float cloud_scale = 1.0F;
                if (const auto* scale = parameter(submesh, "CloudScale")) {
                    if (const auto* value = std::get_if<float>(&scale->value); value && std::isfinite(*value)) {
                        cloud_scale = *value;
                    }
                }
                float has_cloud = 0.0F;
                if (const auto name = texture_parameter(submesh, "CloudTexture")) {
                    auto found = cloud_names.find(*name);
                    if (found == cloud_names.end()) {
                        if (auto texture = lookup(*name)) {
                            const std::string key = "sky-cloud-" + std::to_string(cloud_names.size());
                            if (auto registered = renderer.register_shared_texture(key, *texture); !registered) {
                                return Result::failure(registered.error());
                            }
                            found = cloud_names.emplace(*name, key).first;
                        }
                    }
                    if (found != cloud_names.end()) {
                        bindings.push_back({"eawr_clouds", "shared:" + found->second});
                        has_cloud = 1.0F;
                    }
                }
                bindings.push_back({"eawr_sky", assets::Vec4f{inputs.dome_fraction, cloud_scale, 0.0F, has_cloud}});
                program = std::string(dome_shader);
                pass = RenderPass::transparent;
                ++result.dome_surfaces;
            } else if (ieq(submesh.shader, "MeshAdditive.fx") && sun_bone) {
                if (!inputs.toward_sun) {
                    skip("no environment light 0 direction for the sun");
                    continue;
                }
                const auto& own_record = model.bones[static_cast<std::size_t>(mesh.bone)].relative_transform;
                const float distance = std::sqrt(own_record[3] * own_record[3] + own_record[7] * own_record[7]
                    + own_record[11] * own_record[11]);
                if (!(distance > 1.0e-3F) || !std::isfinite(distance)) {
                    skip("sun bone has no distance");
                    continue;
                }
                // The quad's two widest local axes become screen right and up.
                assets::Vec3f low{1e30F, 1e30F, 1e30F};
                assets::Vec3f high{-1e30F, -1e30F, -1e30F};
                for (const assets::Vertex& vertex : submesh.vertices) {
                    low = {std::min(low.x, vertex.position.x), std::min(low.y, vertex.position.y),
                           std::min(low.z, vertex.position.z)};
                    high = {std::max(high.x, vertex.position.x), std::max(high.y, vertex.position.y),
                            std::max(high.z, vertex.position.z)};
                }
                const std::array<float, 3> extent{high.x - low.x, high.y - low.y, high.z - low.z};
                std::array<int, 3> order{0, 2, 1};
                std::stable_sort(order.begin(), order.end(),
                    [&](const int left, const int right) { return extent[static_cast<std::size_t>(left)]
                        > extent[static_cast<std::size_t>(right)]; });
                const int first = std::min(order[0], order[1]);
                const int second = std::max(order[0], order[1]);
                const auto axis = [](const assets::Vec3f& v, const int which) {
                    return which == 0 ? v.x : (which == 1 ? v.y : v.z);
                };
                for (const assets::Vertex& vertex : submesh.vertices) {
                    assets::Vertex local = vertex;
                    // Source (a, 0, b) uploads as render (a, b, 0).
                    local.position = {axis(vertex.position, first), 0.0F, axis(vertex.position, second)};
                    surface.vertices.push_back(local);
                }
                // Two unreferenced far vertices keep the instance's bounds in view.
                for (const float sign : {-1.0F, 1.0F}) {
                    assets::Vertex bound;
                    bound.position = {sign * far_out, sign * far_out, sign * far_out};
                    surface.vertices.push_back(bound);
                }
                if (const auto name = texture_parameter(submesh, "BaseTexture")) {
                    if (auto texture = lookup(*name)) own = std::move(*texture);
                }
                assets::Vec4f color{1.0F, 1.0F, 1.0F, 1.0F};
                if (const auto* value = parameter(submesh, "Color")) {
                    if (const auto* v4 = std::get_if<assets::Vec4f>(&value->value)) color = *v4;
                    else if (const auto* v3 = std::get_if<assets::Vec3f>(&value->value)) color = {v3->x, v3->y, v3->z, 1.0F};
                }
                const assets::Vec3f toward = normalized(*inputs.toward_sun);
                bindings.push_back({"eawr_sun_color", color});
                bindings.push_back({"eawr_sun", assets::Vec4f{toward.x, toward.z, -toward.y, distance}});
                bindings.push_back({"eawr_sun_limit", inputs.dome_fraction * 0.95F});
                program = std::string(sun_shader);
                pass = RenderPass::transparent;
                ++result.sun_surfaces;
            } else {
                skip(sun_bone ? "mode-7 bone without a MeshAdditive surface" : "shader has no sky adapter");
                continue;
            }
            out.submeshes.push_back(std::move(surface));
            upload.meshes.push_back(std::move(out));
            const MaterialDescription material{
                .schema_version = MaterialDescription::current_schema_version,
                .route = MaterialRoute::modern_spatial,
                .pass = pass,
                .program = std::move(program),
                .technique = {},
                .pass_name = {},
                .bindings = std::move(bindings),
            };
            if (auto uploaded = renderer.upload(next_asset, upload, own, material); !uploaded) {
                return Result::failure(uploaded.error());
            }
            // A dome that encloses the scene must not shadow it.
            renderer.set_casts_shadows(next_asset, false);
            instances.push_back({next_entity++, next_asset++, identity()});
        }
    }
    return Result::success(std::move(result));
}

core::Result<WaterComposition> compose_water(
    GodotRenderer& renderer, const assets::Map& map, const terrain::Mesh& mesh,
    const TextureLookup& lookup, sim::AssetId& next_asset, sim::EntityId& next_entity,
    std::vector<sim::RenderInstance>& instances, const float flow_time, const bool flow_animate,
    const bool fog) {
    using Result = core::Result<WaterComposition>;
    WaterComposition result;
    result.plane = terrain::water_plane(map);
    const auto upload = [&](const assets::Model& model, assets::Texture texture, const assets::Vec4f color,
                            const float bank_fade, const float flow_rate,
                            const float texture_alpha, const float water_ribbon) -> core::Result<void> {
        const MaterialDescription material{
            .schema_version = MaterialDescription::current_schema_version,
            .route = MaterialRoute::modern_spatial,
            .pass = RenderPass::transparent,
            .program = fog ? with_map_fog(std::string(water_shader)) : std::string(water_shader),
            .technique = {},
            .pass_name = {},
            .bindings = {{"eawr_diffuse", std::string{"asset"}}, {"eawr_water_color", color},
                         {"eawr_bank_fade", bank_fade}, {"eawr_flow_rate", flow_rate},
                         {"eawr_texture_alpha", texture_alpha}, {"eawr_flow_time", flow_time},
                         {"eawr_flow_animate", flow_animate ? 1.0F : 0.0F},
                         {"eawr_water_ribbon", water_ribbon}},
        };
        if (auto uploaded = renderer.upload(next_asset, model, texture, material); !uploaded) return uploaded;
        if (fog) {
            if (auto declared = renderer.declare_fog_consumer(next_asset); !declared) return declared;
        }
        renderer.set_casts_shadows(next_asset, false);
        instances.push_back({next_entity++, next_asset++, identity()});
        return core::Result<void>::success();
    };
    if (result.plane) {
        const terrain::WaterPlane& plane = *result.plane;
        // One quad over the terrain extent plus a margin, at the authored
        // height; the base texture repeats every 256 units.
        const float margin = 0.25F * std::max(mesh.bounds_max.x - mesh.bounds_min.x,
                                              mesh.bounds_max.y - mesh.bounds_min.y);
        const std::array<assets::Vec2f, 4> corners{{
            {mesh.bounds_min.x - margin, mesh.bounds_min.y - margin},
            {mesh.bounds_max.x + margin, mesh.bounds_min.y - margin},
            {mesh.bounds_max.x + margin, mesh.bounds_max.y + margin},
            {mesh.bounds_min.x - margin, mesh.bounds_max.y + margin}}};
        assets::Submesh surface;
        surface.shader = "eawr-water-plane";
        for (const assets::Vec2f& corner : corners) {
            assets::Vertex vertex;
            vertex.position = {corner.x, corner.y, plane.height};
            vertex.normal = {0.0F, 0.0F, 1.0F};
            vertex.texcoord[0] = {corner.x / 256.0F, corner.y / 256.0F};
            surface.vertices.push_back(vertex);
        }
        surface.indices = {0, 1, 2, 0, 2, 3};
        assets::Model model;
        model.source = map.source;
        assets::Mesh water_mesh;
        water_mesh.name = "water-plane";
        water_mesh.visible = true;
        water_mesh.submeshes.push_back(std::move(surface));
        model.meshes.push_back(std::move(water_mesh));
        std::optional<assets::Texture> base;
        if (!plane.base_texture.empty()) base = lookup(plane.base_texture);
        // Lava's authored colour is black; its texture carries the look.
        const bool lava = plane.family == 2;
        const assets::Vec4f color = lava ? assets::Vec4f{1.0F, 1.0F, 1.0F, 1.0F}
            : assets::Vec4f{plane.color.x, plane.color.y, plane.color.z, plane.alpha};
        if (auto uploaded = upload(model, base ? std::move(*base) : flat_texture(40, 60, 80), color,
                0.0F, 0.0F, 0.0F, 0.0F);
            !uploaded) {
            return Result::failure(uploaded.error());
        }
        result.plane_drawn = true;
    }
    const terrain::Rivers authored = terrain::visible_rivers(map);
    const auto header = terrain::water_header(map);
    result.rivers_skipped = authored.skipped;
    for (const terrain::TrackPass pass : {terrain::TrackPass::track, terrain::TrackPass::water_decoration}) {
        for (const terrain::River& river : authored.rivers) {
            if (terrain::track_pass(river) != pass) continue;
            // A modest offset keeps the sheet in front of rough cliff samples at
            // steep drops; effect strips retain their established placement.
            const assets::Model model = terrain::river_model(river, 6, river.mode == 5 ? 4.0F : 1.0F);
            if (model.meshes.empty()) {
                ++result.rivers_skipped;
                continue;
            }
            std::optional<assets::Texture> texture;
            if (!river.texture.empty()) texture = lookup(river.texture);
            const bool water_ribbon = pass == terrain::TrackPass::water_decoration || river.mode == 5;
            const assets::Vec4f color = water_ribbon && header
                ? assets::Vec4f{header->color.x, header->color.y, header->color.z, header->alpha}
                : assets::Vec4f{1.0F, 1.0F, 1.0F, 0.85F};
            if (auto uploaded = upload(model, texture ? std::move(*texture) : flat_texture(70, 90, 100),
                    color, 1.0F, water_ribbon ? river.flow : 0.0F,
                    water_ribbon ? 1.0F : 0.0F, water_ribbon ? 1.0F : 0.0F); !uploaded) {
                return Result::failure(uploaded.error());
            }
            ++result.rivers_drawn;
            if (pass == terrain::TrackPass::water_decoration) ++result.water_decoration_tracks_drawn;
            else ++result.terrain_tracks_drawn;
        }
    }
    if (result.plane_drawn || result.rivers_drawn != 0) {
        result.status = "approximate";
        result.cause = "no TerrainWater technique is translated: the plane uses the header colour and alpha; "
                       "water decorations and mode-5 ribbons use the header tint, texture alpha and authored flow scroll";
    } else {
        result.status = "absent";
        result.cause = result.rivers_skipped != 0
            ? "the active river records could not be built"
            : "the map declares no water plane or terrain tracks";
    }
    return Result::success(std::move(result));
}

core::Result<TacticalDefault> tactical_default(
    const vfs::Vfs& filesystem, const assets::Map& map, const terrain::Mesh& mesh,
    const std::uint32_t width, const std::uint32_t height, const TacticalRequest& request) {
    using Result = core::Result<TacticalDefault>;
    constexpr std::string_view tactical_path = "data/xml/tacticalcameras.xml";
    constexpr std::string_view constants_path = "data/xml/gameconstants.xml";
    auto tactical_bytes = filesystem.open(tactical_path);
    if (!tactical_bytes) return Result::failure(tactical_bytes.error());
    auto constants_bytes = filesystem.open(constants_path);
    if (!constants_bytes) return Result::failure(constants_bytes.error());
    const auto hash = [](const std::vector<std::byte>& bytes) {
        return sim::sha256_hex(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
    };
    TacticalDefault result;
    result.tactical_xml_sha256 = hash(tactical_bytes.value());
    const std::string constants_hash = hash(constants_bytes.value());
    auto loaded = camera::load_constants(
        {tactical_bytes.value(), tactical_path, result.tactical_xml_sha256},
        {constants_bytes.value(), constants_path, constants_hash}, camera::Mode::land);
    if (!loaded) return Result::failure(loaded.error());
    const camera::Constants& constants = loaded.value().constants;

    // The zoom whose solved distance is Distance_Default (distance grows with
    // zoom), unless one is requested.
    float zoom = 1.0F;
    if (request.zoom) {
        zoom = std::clamp(*request.zoom, camera::min_zoom, camera::max_zoom);
    } else {
        float low = camera::min_zoom;
        float high = camera::max_zoom;
        for (int step = 0; step < 40; ++step) {
            const float middle = 0.5F * (low + high);
            auto solved = camera::solve(constants, middle);
            if (!solved) return Result::failure(solved.error());
            if (solved.value().distance < constants.distance_default) low = middle;
            else high = middle;
        }
        zoom = high;
    }
    const float yaw = request.yaw_degrees.value_or(constants.yaw_default);

    // Target: the requested point, else the first Player_0 spawn marker, else
    // the terrain centre.
    std::array<float, 2> target{(mesh.bounds_min.x + mesh.bounds_max.x) * 0.5F,
                                (mesh.bounds_min.y + mesh.bounds_max.y) * 0.5F};
    result.target_origin = "terrain_centre";
    if (request.target) {
        target = *request.target;
        result.target_origin = "requested";
    } else {
        const std::uint32_t marker = assets::object_type_crc("Player_0_Spawn_Point_Marker");
        for (const assets::Placement& placement : map.placements) {
            if (placement.type_crc && *placement.type_crc == marker && placement.position) {
                target = {placement.position->x, placement.position->y};
                result.target_origin = "player_0_spawn_marker";
                break;
            }
        }
    }
    // Terrain height under the target (Location_Follows_Terrain).
    float ground = 0.0F;
    if (map.terrain && map.terrain->width >= 2 && map.terrain->height >= 2
        && map.terrain->samples.size() == static_cast<std::size_t>(map.terrain->width) * map.terrain->height) {
        const assets::Terrain& source = *map.terrain;
        const float gx = std::clamp(target[0] / source.cell_spacing, 0.0F, static_cast<float>(source.width - 1));
        const float gy = std::clamp(target[1] / source.cell_spacing, 0.0F, static_cast<float>(source.height - 1));
        const auto x0 = std::min(static_cast<std::uint32_t>(gx), source.width - 2);
        const auto y0 = std::min(static_cast<std::uint32_t>(gy), source.height - 2);
        const float fx = gx - static_cast<float>(x0);
        const float fy = gy - static_cast<float>(y0);
        const auto at = [&](const std::uint32_t x, const std::uint32_t y) {
            return static_cast<float>(source.samples[static_cast<std::size_t>(y) * source.width + x].height_sample)
                * source.height_scale;
        };
        ground = (at(x0, y0) * (1.0F - fx) + at(x0 + 1, y0) * fx) * (1.0F - fy)
            + (at(x0, y0 + 1) * (1.0F - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
    }
    result.target_source = {target[0], target[1], ground};
    camera::SourceTargetBounds bounds;
    bounds.min_x = mesh.bounds_min.x;
    bounds.max_x = mesh.bounds_max.x;
    bounds.min_y = mesh.bounds_min.y;
    bounds.max_y = mesh.bounds_max.y;
    bounds.logical_path = map.source.logical_path;
    bounds.source_id = "terrain-extent";
    bounds.authority = "project-authored";
    auto controller = camera::BoundedTacticalController::create(constants, bounds,
        {target[0], ground, -target[1]}, zoom, yaw, width, height);
    if (!controller) return Result::failure(controller.error());
    const camera::TacticalFrame& frame = controller.value().frame();
    result.camera.width = frame.width;
    result.camera.height = frame.height;
    result.camera.vertical_fov_degrees = frame.vertical_fov_degrees;
    result.camera.near_plane = frame.near_plane;
    result.camera.far_plane = frame.far_plane;
    result.camera.eye = frame.eye;
    result.camera.target = frame.target;
    result.camera.up = frame.up;
    result.zoom = zoom;
    result.yaw_degrees = yaw;
    result.distance = controller.value().state().distance;
    result.pitch_degrees = controller.value().state().pitch_degrees;
    return Result::success(std::move(result));
}

} // namespace eawr::presentation::godot_backend::land_look
