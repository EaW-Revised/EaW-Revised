#include "eawr/presentation/terrain/terrain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace eawr::presentation::terrain {
namespace {

core::Diagnostic diagnostic(
    const assets::Source& source, const std::string_view code, std::string message) {
    return core::Diagnostic{
        .code = std::string(code),
        .severity = core::Severity::error,
        .message = std::move(message),
        .logical_path = source.logical_path.empty()
            ? std::nullopt : std::optional(source.logical_path),
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = source.source_id.empty()
            ? std::nullopt : std::optional(source.source_id),
    };
}

template <typename T>
core::Result<T> fail(
    const assets::Source& source, const std::string_view code, std::string message) {
    return core::Result<T>::failure(diagnostic(source, code, std::move(message)));
}

// Source height of a clamped grid sample.  Clamping at the border keeps the
// central difference defined without inventing a sample outside the map.
double source_height(
    const assets::Terrain& source, const std::int64_t column, const std::int64_t row) {
    const auto clamp = [](const std::int64_t value, const std::uint32_t extent) {
        const auto limit = static_cast<std::int64_t>(extent) - 1;
        return value < 0 ? std::int64_t{0} : (value > limit ? limit : value);
    };
    const std::int64_t x = clamp(column, source.width);
    const std::int64_t y = clamp(row, source.height);
    const auto index = static_cast<std::size_t>(y) * source.width + static_cast<std::size_t>(x);
    return static_cast<double>(source.samples[index].height_sample) * source.height_scale;
}

// Central differences over the source heightfield.  The surface is
// z = f(x, y) in source space, so its normal is (-dz/dx, -dz/dy, 1) before the
// one basis conversion.
assets::Vec3f source_normal(
    const assets::Terrain& source, const std::uint32_t column, const std::uint32_t row) {
    const auto x = static_cast<std::int64_t>(column);
    const auto y = static_cast<std::int64_t>(row);
    const double span = 2.0 * source.cell_spacing;
    const double slope_x = (source_height(source, x + 1, y) - source_height(source, x - 1, y)) / span;
    const double slope_y = (source_height(source, x, y + 1) - source_height(source, x, y - 1)) / span;
    const double length = std::hypot(slope_x, slope_y, 1.0);
    return {static_cast<float>(-slope_x / length),
            static_cast<float>(-slope_y / length),
            static_cast<float>(1.0 / length)};
}

void expand(assets::Vec3f& minimum, assets::Vec3f& maximum, const assets::Vec3f point) {
    minimum.x = std::min(minimum.x, point.x);
    minimum.y = std::min(minimum.y, point.y);
    minimum.z = std::min(minimum.z, point.z);
    maximum.x = std::max(maximum.x, point.x);
    maximum.y = std::max(maximum.y, point.y);
    maximum.z = std::max(maximum.z, point.z);
}

constexpr assets::Vec3f lowest_bound{
    std::numeric_limits<float>::max(),
    std::numeric_limits<float>::max(),
    std::numeric_limits<float>::max(),
};
constexpr assets::Vec3f highest_bound{
    std::numeric_limits<float>::lowest(),
    std::numeric_limits<float>::lowest(),
    std::numeric_limits<float>::lowest(),
};

} // namespace

bool declared(const std::optional<std::string>& name) {
    if (!name) return false;
    for (const unsigned char character : *name) {
        if (character > ' ') return true;
    }
    return false;
}

namespace {

assets::Vertex grid_vertex(
    const assets::Terrain& source, const std::uint32_t column, const std::uint32_t row) {
    const auto index = static_cast<std::size_t>(row) * source.width + column;
    const assets::TerrainSample& sample = source.samples[index];
    const assets::Vec3f position{
        static_cast<float>(static_cast<double>(column) * source.cell_spacing),
        static_cast<float>(static_cast<double>(row) * source.cell_spacing),
        static_cast<float>(static_cast<double>(sample.height_sample) * source.height_scale),
    };
    // `vertex_intensity` is retained as a monochrome vertex term.  The TED
    // contract names it an intensity and explicitly not a blend weight, but its
    // exact role is unproven, so it is carried losslessly rather than folded
    // into geometry or a material constant here.
    const float intensity = static_cast<float>(sample.vertex_intensity) / 255.0F;
    assets::Vertex vertex;
    // Source basis, like every other `assets::Model`.  The renderer applies the
    // one documented conversion when it uploads.
    vertex.position = position;
    vertex.normal = source_normal(source, column, row);
    // One texture tile per cell.  The authored tile rate is not derivable from
    // the fields this loader retains, so it is stated here rather than implied.
    vertex.texcoord[0] = {static_cast<float>(column), static_cast<float>(row)};
    vertex.color = {intensity, intensity, intensity, 1.0F};
    return vertex;
}

SurfaceEffect select_effect(const assets::TerrainMaterial& material) {
    if (!declared(material.primary_texture)) return SurfaceEffect::undeclared;
    if (declared(material.secondary_texture)) return SurfaceEffect::terrain_render_bump_dual;
    return SurfaceEffect::terrain_render_bump_nobump;
}

} // namespace

core::Result<Mesh> build(const assets::Map& map, const BuildOptions& options) {
    if (options.chunk_cells == 0 || options.chunk_cells > max_chunk_cells) {
        return fail<Mesh>(map.source, diagnostic_codes::invalid_options,
            "terrain chunk size must be between 1 and 128 cells");
    }
    if (!map.terrain) {
        return fail<Mesh>(map.source, diagnostic_codes::no_terrain,
            "map has no semantic terrain view; a space or raw-only map is not an empty heightfield");
    }
    const assets::Terrain& source = *map.terrain;
    if (source.width < 2 || source.height < 2) {
        return fail<Mesh>(map.source, diagnostic_codes::invalid_terrain,
            "terrain grid needs at least two samples on each axis to form a cell");
    }
    const auto size_limit = std::numeric_limits<std::size_t>::max();
    if (source.width > size_limit / source.height) {
        return fail<Mesh>(map.source, diagnostic_codes::invalid_terrain,
            "terrain grid sample count exceeds the supported size");
    }
    const auto expected = static_cast<std::size_t>(source.width) * source.height;
    if (source.samples.size() != expected) {
        return fail<Mesh>(map.source, diagnostic_codes::invalid_terrain,
            "terrain sample count disagrees with the declared grid");
    }
    if (!std::isfinite(source.cell_spacing) || source.cell_spacing <= 0.0F) {
        return fail<Mesh>(map.source, diagnostic_codes::invalid_terrain,
            "terrain cell spacing must be finite and positive");
    }
    if (!std::isfinite(source.height_scale)) {
        return fail<Mesh>(map.source, diagnostic_codes::invalid_terrain,
            "terrain height scale must be finite");
    }
    const std::uint32_t cells_x = source.width - 1;
    const std::uint32_t cells_y = source.height - 1;
    const double float_limit = std::numeric_limits<float>::max();
    if (static_cast<double>(cells_x) * source.cell_spacing > float_limit
        || static_cast<double>(cells_y) * source.cell_spacing > float_limit) {
        return fail<Mesh>(map.source, diagnostic_codes::invalid_terrain,
            "terrain grid extent exceeds the finite position range");
    }
    if (source.materials.size() > 256) {
        return fail<Mesh>(map.source, diagnostic_codes::invalid_terrain,
            "terrain material descriptor count exceeds the 8-bit slot range");
    }
    const auto cell_count = static_cast<std::uint64_t>(cells_x) * cells_y;
    if (cell_count > std::numeric_limits<std::uint64_t>::max() / 4) {
        return fail<Mesh>(map.source, diagnostic_codes::invalid_terrain,
            "terrain geometry count exceeds the supported size");
    }
    const auto chunks_x = cells_x / options.chunk_cells + (cells_x % options.chunk_cells != 0);
    const auto chunks_y = cells_y / options.chunk_cells + (cells_y % options.chunk_cells != 0);
    if (chunks_x > size_limit / chunks_y) {
        return fail<Mesh>(map.source, diagnostic_codes::invalid_terrain,
            "terrain chunk count exceeds the supported size");
    }
    for (std::uint32_t row = 0; row < source.height; ++row) {
        for (std::uint32_t column = 0; column < source.width; ++column) {
            const auto index = static_cast<std::size_t>(row) * source.width + column;
            const auto& sample = source.samples[index];
            if (std::abs(static_cast<double>(sample.height_sample) * source.height_scale)
                > float_limit) {
                return fail<Mesh>(map.source, diagnostic_codes::invalid_terrain,
                    "terrain sample height exceeds the finite position range");
            }
            if (!source.materials.empty() && column < cells_x && row < cells_y
                && sample.material_slot >= source.materials.size()) {
                return fail<Mesh>(map.source, diagnostic_codes::invalid_terrain,
                    "terrain sample names a material slot outside the descriptor list");
            }
        }
    }

    Mesh mesh;
    mesh.source = map.source;
    mesh.chunk_cells = options.chunk_cells;
    mesh.grid_width = source.width;
    mesh.grid_height = source.height;
    mesh.cell_spacing = source.cell_spacing;
    mesh.height_scale = source.height_scale;
    mesh.bounds_min = lowest_bound;
    mesh.bounds_max = highest_bound;
    mesh.notices = map.notices;

    if (source.materials.empty()) {
        mesh.notices.push_back({0, 0, 0,
            "terrain declares no material descriptors; geometry is built untextured"});
    }
    mesh.slots.reserve(source.materials.size());
    for (std::size_t index = 0; index < source.materials.size(); ++index) {
        const assets::TerrainMaterial& material = source.materials[index];
        MaterialSlot slot;
        slot.slot = static_cast<std::uint8_t>(index);
        slot.effect = select_effect(material);
        slot.primary_texture = material.primary_texture;
        slot.secondary_texture = material.secondary_texture;
        mesh.slots.push_back(std::move(slot));
    }

    // Cells, not samples: the last row and column of samples close the final
    // cell and start no new one.
    mesh.chunks_x = chunks_x;
    mesh.chunks_y = chunks_y;
    mesh.chunks.reserve(static_cast<std::size_t>(mesh.chunks_x) * mesh.chunks_y);

    for (std::uint32_t chunk_y = 0; chunk_y < mesh.chunks_y; ++chunk_y) {
        for (std::uint32_t chunk_x = 0; chunk_x < mesh.chunks_x; ++chunk_x) {
            Chunk chunk;
            chunk.chunk_x = chunk_x;
            chunk.chunk_y = chunk_y;
            chunk.cell_origin_x = chunk_x * options.chunk_cells;
            chunk.cell_origin_y = chunk_y * options.chunk_cells;
            chunk.cells_x = std::min(options.chunk_cells, cells_x - chunk.cell_origin_x);
            chunk.cells_y = std::min(options.chunk_cells, cells_y - chunk.cell_origin_y);
            chunk.bounds_min = lowest_bound;
            chunk.bounds_max = highest_bound;

            // One surface per material slot present in this chunk, keyed by
            // slot so the output order is the slot order and not the order the
            // cells happened to be visited in.
            std::vector<Surface> by_slot(source.materials.empty() ? 1U : source.materials.size());
            for (std::size_t index = 0; index < by_slot.size(); ++index) {
                by_slot[index].material_slot = static_cast<std::uint8_t>(index);
            }

            for (std::uint32_t row = 0; row < chunk.cells_y; ++row) {
                for (std::uint32_t column = 0; column < chunk.cells_x; ++column) {
                    const std::uint32_t x = chunk.cell_origin_x + column;
                    const std::uint32_t y = chunk.cell_origin_y + row;
                    const auto anchor = static_cast<std::size_t>(y) * source.width + x;
                    // The cell takes the slot of its anchor sample.  Averaging
                    // or blending four slots would be a guess at the blend-map
                    // semantics this slice does not have.
                    // A map with no material descriptors declares no slots
                    // at all, so its samples cannot name one; the geometry is
                    // still built, untextured, under a single noticed surface.
                    const std::uint8_t slot = source.materials.empty()
                        ? std::uint8_t{0} : source.samples[anchor].material_slot;
                    Surface& surface = by_slot[slot];
                    if (surface.vertices.size() + 4
                        > static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max())) {
                        return fail<Mesh>(map.source, diagnostic_codes::invalid_options,
                            "chunk surface exceeds the 16-bit index bound; use a smaller chunk size");
                    }
                    const auto base = static_cast<std::uint16_t>(surface.vertices.size());
                    const std::array<assets::Vertex, 4> corners{
                        grid_vertex(source, x, y),
                        grid_vertex(source, x + 1, y),
                        grid_vertex(source, x + 1, y + 1),
                        grid_vertex(source, x, y + 1),
                    };
                    for (const assets::Vertex& corner : corners) {
                        expand(chunk.bounds_min, chunk.bounds_max, corner.position);
                        expand(mesh.bounds_min, mesh.bounds_max, corner.position);
                        surface.vertices.push_back(corner);
                    }
                    // Counter-clockwise about the source up axis (+Z).  The
                    // asset-to-render rotation is proper, so this is still
                    // counter-clockwise about render up after upload.
                    const std::array<std::uint16_t, 6> order{0, 1, 2, 0, 2, 3};
                    for (const std::uint16_t offset : order) {
                        surface.indices.push_back(static_cast<std::uint16_t>(base + offset));
                    }
                }
            }

            for (Surface& surface : by_slot) {
                if (surface.indices.empty()) continue;
                if (surface.material_slot < mesh.slots.size()) {
                    mesh.slots[surface.material_slot].cells += surface.indices.size() / 6;
                }
                mesh.vertex_count += surface.vertices.size();
                mesh.triangle_count += surface.indices.size() / 3;
                chunk.surfaces.push_back(std::move(surface));
            }
            if (!chunk.surfaces.empty()) mesh.chunks.push_back(std::move(chunk));
        }
    }
    if (mesh.chunks.empty()) {
        return fail<Mesh>(map.source, diagnostic_codes::invalid_terrain,
            "terrain produced no drawable chunk");
    }
    return core::Result<Mesh>::success(std::move(mesh));
}

namespace {

assets::Submesh build_submesh(const Mesh& mesh, const Surface& surface) {
    assets::Submesh submesh;
    submesh.vertices = surface.vertices;
    submesh.indices = surface.indices;
    if (surface.material_slot < mesh.slots.size()) {
        const MaterialSlot& slot = mesh.slots[surface.material_slot];
        submesh.shader = std::string(effect_program(slot.effect));
        if (slot.primary_texture) {
            submesh.parameters.push_back(
                {"DiffuseTexture0", assets::ParameterKind::texture, *slot.primary_texture});
        }
        if (slot.secondary_texture) {
            submesh.parameters.push_back(
                {"DiffuseTexture1", assets::ParameterKind::texture, *slot.secondary_texture});
        }
    }
    return submesh;
}

assets::Mesh build_mesh(const Chunk& chunk) {
    assets::Mesh output;
    output.name = "terrain-chunk-" + std::to_string(chunk.chunk_x)
        + "-" + std::to_string(chunk.chunk_y);
    output.bounds_min = chunk.bounds_min;
    output.bounds_max = chunk.bounds_max;
    output.visible = true;
    return output;
}

} // namespace

assets::Model surface_model(const Mesh& mesh, const Chunk& chunk, const Surface& surface) {
    assets::Model model;
    model.source = mesh.source;
    assets::Mesh output = build_mesh(chunk);
    output.name += "-slot-" + std::to_string(static_cast<unsigned>(surface.material_slot));
    output.submeshes.push_back(build_submesh(mesh, surface));
    model.meshes.push_back(std::move(output));
    return model;
}

assets::Model chunk_model(const Mesh& mesh, const Chunk& chunk) {
    assets::Model model;
    model.source = mesh.source;
    assets::Mesh output = build_mesh(chunk);
    output.submeshes.reserve(chunk.surfaces.size());
    for (const Surface& surface : chunk.surfaces) {
        output.submeshes.push_back(build_submesh(mesh, surface));
    }
    model.meshes.push_back(std::move(output));
    return model;
}

namespace {

// Little-endian binary32 and u32 readers over a mini payload. A payload of
// the wrong size is not a value, so the caller keeps its default.
std::optional<float> field_float(const std::vector<std::byte>& bytes, const std::size_t offset = 0) {
    if (bytes.size() < offset + 4) return std::nullopt;
    std::uint32_t raw{};
    for (std::size_t index = 0; index < 4; ++index) {
        raw |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (8U * index);
    }
    float value{};
    static_assert(sizeof(value) == sizeof(raw));
    std::memcpy(&value, &raw, sizeof(value));
    if (!std::isfinite(value)) return std::nullopt;
    return value;
}

std::optional<std::uint32_t> field_u32(const std::vector<std::byte>& bytes) {
    if (bytes.size() != 4) return std::nullopt;
    std::uint32_t raw{};
    for (std::size_t index = 0; index < 4; ++index) {
        raw |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[index])) << (8U * index);
    }
    return raw;
}

std::optional<assets::Vec3f> field_vec3(const std::vector<std::byte>& bytes) {
    if (bytes.size() != 12) return std::nullopt;
    const auto x = field_float(bytes, 0);
    const auto y = field_float(bytes, 4);
    const auto z = field_float(bytes, 8);
    if (!x || !y || !z) return std::nullopt;
    return assets::Vec3f{*x, *y, *z};
}

struct Mini final {
    std::uint8_t id{};
    std::vector<std::byte> bytes;
};

// 8-bit id, 8-bit length, payload. A truncated tail ends the stream.
std::vector<Mini> parse_minis(const std::vector<std::byte>& payload) {
    std::vector<Mini> result;
    std::size_t offset = 0;
    while (offset + 2 <= payload.size()) {
        const auto id = std::to_integer<std::uint8_t>(payload[offset]);
        const auto size = std::to_integer<std::uint8_t>(payload[offset + 1]);
        if (offset + 2 + size > payload.size()) break;
        result.push_back({id, std::vector<std::byte>(payload.begin() + static_cast<std::ptrdiff_t>(offset + 2),
            payload.begin() + static_cast<std::ptrdiff_t>(offset + 2 + size))});
        offset += 2 + size;
    }
    return result;
}

const assets::RawChunk* child(const std::vector<assets::RawChunk>& chunks, const std::uint32_t id) {
    for (const assets::RawChunk& chunk : chunks) {
        if (chunk.id == id) return &chunk;
    }
    return nullptr;
}

} // namespace

LayerMapping decode_mapping(const assets::TerrainMaterial& material) {
    LayerMapping mapping;
    for (const assets::RawField& field : material.fields) {
        if (field.id == 0x07) {
            if (const auto value = field.bytes.size() == 4 ? field_float(field.bytes) : std::nullopt;
                value && *value > 0.0F) {
                mapping.tile_size = *value;
            }
        } else if (field.id == 0x08) {
            if (const auto value = field.bytes.size() == 4 ? field_float(field.bytes) : std::nullopt) {
                mapping.rotation = *value;
            }
        } else if (field.id == 0x09) {
            if (const auto value = field.bytes.size() == 4 ? field_float(field.bytes) : std::nullopt) {
                mapping.tilt = *value;
            }
        } else if (field.id == 0x0a) {
            if (const auto value = field_vec3(field.bytes)) mapping.tint = *value;
        }
    }
    return mapping;
}

TexGen retail_texgen(const LayerMapping& mapping) {
    const float scale = 1.0F / mapping.tile_size;
    const float c = std::cos(mapping.rotation);
    const float s = std::sin(mapping.rotation);
    const float lean = std::cos(mapping.tilt);
    const float rise = std::sin(mapping.tilt);
    return {
        {c * scale, -s * scale, 0.0F},
        {s * lean * scale, c * lean * scale, -rise * scale},
    };
}

core::Result<BlendMap> blend_map(const assets::Map& map) {
    if (!map.terrain) {
        return fail<BlendMap>(map.source, diagnostic_codes::no_terrain,
            "map has no semantic terrain view; a blend map needs one");
    }
    const assets::Terrain& source = *map.terrain;
    if (source.width < 2 || source.height < 2
        || source.samples.size() != static_cast<std::size_t>(source.width) * source.height
        || !std::isfinite(source.cell_spacing) || source.cell_spacing <= 0.0F) {
        return fail<BlendMap>(map.source, diagnostic_codes::invalid_terrain,
            "terrain grid cannot carry a blend map");
    }
    BlendMap result;
    result.width = source.width;
    result.height = source.height;
    result.cell_spacing = source.cell_spacing;
    std::array<std::uint64_t, 256> counts{};
    for (const assets::TerrainSample& sample : source.samples) {
        const std::uint8_t slot = source.materials.empty() ? std::uint8_t{0} : sample.material_slot;
        if (!source.materials.empty() && slot >= source.materials.size()) {
            return fail<BlendMap>(map.source, diagnostic_codes::invalid_terrain,
                "terrain sample names a material slot outside the descriptor list");
        }
        ++counts[slot];
    }
    std::array<std::uint8_t, 256> layer_of{};
    for (std::size_t slot = 0; slot < counts.size(); ++slot) {
        if (counts[slot] == 0) continue;
        layer_of[slot] = static_cast<std::uint8_t>(result.layers.size());
        BlendLayer layer;
        layer.slot = static_cast<std::uint8_t>(slot);
        layer.samples = counts[slot];
        if (slot < source.materials.size()) layer.mapping = decode_mapping(source.materials[slot]);
        result.layers.push_back(layer);
    }
    result.sample_layers.reserve(source.samples.size());
    for (const assets::TerrainSample& sample : source.samples) {
        result.sample_layers.push_back(layer_of[source.materials.empty() ? 0 : sample.material_slot]);
    }
    return core::Result<BlendMap>::success(std::move(result));
}

std::vector<LayerWeight> blend_weights(const BlendMap& map, const float x, const float y) {
    std::vector<LayerWeight> result;
    if (map.width < 2 || map.height < 2 || map.sample_layers.size()
            != static_cast<std::size_t>(map.width) * map.height || !(map.cell_spacing > 0.0F)) {
        return result;
    }
    const float gx = x / map.cell_spacing;
    const float gy = y / map.cell_spacing;
    const float cell_x = std::clamp(std::floor(gx), 0.0F, static_cast<float>(map.width - 2));
    const float cell_y = std::clamp(std::floor(gy), 0.0F, static_cast<float>(map.height - 2));
    const float fx = std::clamp(gx - cell_x, 0.0F, 1.0F);
    const float fy = std::clamp(gy - cell_y, 0.0F, 1.0F);
    const auto column = static_cast<std::size_t>(cell_x);
    const auto row = static_cast<std::size_t>(cell_y);
    const std::array<std::pair<std::size_t, float>, 4> corners{{
        {row * map.width + column, (1.0F - fx) * (1.0F - fy)},
        {row * map.width + column + 1, fx * (1.0F - fy)},
        {(row + 1) * map.width + column, (1.0F - fx) * fy},
        {(row + 1) * map.width + column + 1, fx * fy},
    }};
    for (const auto& [index, weight] : corners) {
        const std::uint8_t layer = map.sample_layers[index];
        const auto found = std::find_if(result.begin(), result.end(),
            [layer](const LayerWeight& entry) { return entry.layer == layer; });
        if (found != result.end()) found->weight += weight;
        else result.push_back({layer, weight});
    }
    std::sort(result.begin(), result.end(),
        [](const LayerWeight& left, const LayerWeight& right) { return left.layer < right.layer; });
    return result;
}

std::optional<WaterPlane> water_header(const assets::Map& map) {
    const auto header = std::find_if(map.water_records.begin(), map.water_records.end(),
        [](const assets::WaterRecord& record) { return record.chunk_path == "1/257/0"; });
    if (header == map.water_records.end()) return std::nullopt;
    WaterPlane plane;
    bool family_found = false;
    for (const assets::RawField& field : header->fields) {
        if (field.id == 0x16) {
            if (const auto value = field_u32(field.bytes)) {
                plane.family = *value;
                family_found = true;
            }
        } else if (field.id == 0x15) {
            if (const auto value = field.bytes.size() == 4 ? field_float(field.bytes) : std::nullopt) {
                plane.height = *value;
            }
        } else if (field.id == 0x1a) {
            if (const auto value = field_vec3(field.bytes)) plane.color = *value;
        } else if (field.id == 0x20) {
            if (const auto value = field.bytes.size() == 4 ? field_float(field.bytes) : std::nullopt) {
                plane.alpha = std::clamp(*value, 0.0F, 1.0F);
            }
        }
    }
    if (!family_found) return std::nullopt;
    for (const assets::TextureReference& texture : map.water_textures) {
        if (texture.field_id == 0x1d) plane.bump_texture = texture.logical_name;
        else if (texture.field_id == 0x1e) plane.base_texture = texture.logical_name;
    }
    return plane;
}

std::optional<WaterPlane> water_plane(const assets::Map& map) {
    auto plane = water_header(map);
    if (plane && plane->family != 0) return plane;
    return std::nullopt;
}

Rivers rivers(const assets::Map& map) {
    Rivers result;
    const assets::RawChunk* main = child(map.chunks, 1);
    const assets::RawChunk* bundle = main ? child(main->children, 257) : nullptr;
    const assets::RawChunk* waves = bundle ? child(bundle->children, 9) : nullptr;
    if (!waves) return result;
    for (const assets::RawChunk& group : waves->children) {
        if (group.id != 256 || !group.group) continue;
        River river;
        if (const auto* parameters = child(group.children, 257)) {
            for (const Mini& mini : parse_minis(parameters->payload)) {
                if (mini.id == 0x01) {
                    if (const auto value = field_u32(mini.bytes)) river.mode = *value;
                } else if (mini.id == 0x03 && mini.bytes.size() == 4) {
                    if (const auto value = field_float(mini.bytes)) river.flow = *value;
                } else if (mini.id == 0x10) {
                    if (const auto value = field_u32(mini.bytes)) river.type = *value;
                }
            }
        }
        if (const auto* points = child(group.children, 258)) {
            for (const Mini& mini : parse_minis(points->payload)) {
                if (mini.id != 0x07) continue;
                if (const auto value = field_vec3(mini.bytes)) river.points.push_back(*value);
            }
        }
        if (const auto* name = child(group.children, 259)) {
            for (const std::byte value : name->payload) {
                const auto character = std::to_integer<unsigned char>(value);
                if (character == 0) break;
                if (character < 0x20 || character >= 0x7f) {
                    river.texture.clear();
                    break;
                }
                river.texture.push_back(static_cast<char>(character));
            }
        }
        if (const auto* widths = child(group.children, 260)) {
            for (const Mini& mini : parse_minis(widths->payload)) {
                if (mini.id != 0x08 || mini.bytes.size() != 4) continue;
                if (const auto value = field_float(mini.bytes)) river.widths.push_back(*value);
            }
        }
        if (river.points.size() < 2 || river.points.size() != river.widths.size()) {
            ++result.skipped;
            continue;
        }
        result.rivers.push_back(std::move(river));
    }
    return result;
}

Rivers visible_rivers(const assets::Map& map) {
    return rivers(map);
}

assets::Model river_model(const River& river, const std::uint32_t subdivisions, const float lift) {
    assets::Model model;
    if (river.points.size() < 2 || river.points.size() != river.widths.size() || subdivisions == 0) return model;
    const auto at = [&](const std::ptrdiff_t index) {
        const auto last = static_cast<std::ptrdiff_t>(river.points.size()) - 1;
        return river.points[static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(index, 0, last))];
    };
    const auto catmull = [](const float p0, const float p1, const float p2, const float p3, const float t) {
        const float t2 = t * t;
        const float t3 = t2 * t;
        return 0.5F * (2.0F * p1 + (p2 - p0) * t + (2.0F * p0 - 5.0F * p1 + 4.0F * p2 - p3) * t2
            + (3.0F * p1 - p0 - 3.0F * p2 + p3) * t3);
    };
    struct Section final {
        assets::Vec3f centre;
        float width{};
    };
    std::vector<Section> sections;
    const auto spans = static_cast<std::ptrdiff_t>(river.points.size()) - 1;
    for (std::ptrdiff_t span = 0; span < spans; ++span) {
        const assets::Vec3f p0 = at(span - 1), p1 = at(span), p2 = at(span + 1), p3 = at(span + 2);
        const std::uint32_t steps = span + 1 == spans ? subdivisions + 1 : subdivisions;
        for (std::uint32_t step = 0; step < steps; ++step) {
            const float t = static_cast<float>(step) / static_cast<float>(subdivisions);
            const float w0 = river.widths[static_cast<std::size_t>(span)];
            const float w1 = river.widths[static_cast<std::size_t>(span + 1)];
            sections.push_back({{catmull(p0.x, p1.x, p2.x, p3.x, t), catmull(p0.y, p1.y, p2.y, p3.y, t),
                                 catmull(p0.z, p1.z, p2.z, p3.z, t) + lift},
                                w0 + (w1 - w0) * t});
        }
    }
    constexpr std::array<float, 4> across{0.0F, 0.25F, 0.75F, 1.0F};
    constexpr std::array<float, 4> fade{0.0F, 1.0F, 1.0F, 0.0F};
    if (sections.size() * across.size() > std::numeric_limits<std::uint16_t>::max()) return model;
    assets::Submesh submesh;
    submesh.shader = "eawr-river";
    if (!river.texture.empty()) {
        submesh.parameters.push_back({"BaseTexture", assets::ParameterKind::texture, river.texture});
    }
    float travelled = 0.0F;
    assets::Vec3f minimum = lowest_bound;
    assets::Vec3f maximum = highest_bound;
    for (std::size_t index = 0; index < sections.size(); ++index) {
        const assets::Vec3f& previous = sections[index == 0 ? 0 : index - 1].centre;
        const assets::Vec3f& next = sections[std::min(index + 1, sections.size() - 1)].centre;
        float tx = next.x - previous.x;
        float ty = next.y - previous.y;
        const float length = std::hypot(tx, ty);
        const float slope = std::clamp(
            std::abs(next.z - previous.z) / std::max(length, 1.0F), 0.0F, 1.0F);
        if (length > 1.0e-4F) {
            tx /= length;
            ty /= length;
        } else {
            tx = 1.0F;
            ty = 0.0F;
        }
        if (index > 0) {
            const assets::Vec3f& back = sections[index - 1].centre;
            const float step = std::sqrt((sections[index].centre.x - back.x) * (sections[index].centre.x - back.x)
                + (sections[index].centre.y - back.y) * (sections[index].centre.y - back.y)
                + (sections[index].centre.z - back.z) * (sections[index].centre.z - back.z));
            travelled += step / std::max(sections[index].width, 1.0F);
        }
        // Left of the direction of travel is (-ty, tx).
        const float half = sections[index].width * 0.5F;
        for (std::size_t lane = 0; lane < across.size(); ++lane) {
            const float offset = (across[lane] * 2.0F - 1.0F) * half;
            assets::Vertex vertex;
            vertex.position = {sections[index].centre.x + ty * offset,
                               sections[index].centre.y - tx * offset,
                               sections[index].centre.z};
            vertex.normal = {0.0F, 0.0F, 1.0F};
            vertex.texcoord[0] = {across[lane], travelled};
            // The red channel carries the local drop for the water material's
            // waterfall sheen; alpha retains the authored bank fade.
            vertex.color = {slope, 1.0F, 1.0F, fade[lane]};
            expand(minimum, maximum, vertex.position);
            submesh.vertices.push_back(vertex);
        }
        if (index == 0) continue;
        const auto base = static_cast<std::uint16_t>((index - 1) * across.size());
        const auto top = static_cast<std::uint16_t>(index * across.size());
        for (std::size_t lane = 0; lane + 1 < across.size(); ++lane) {
            const std::array<std::uint16_t, 6> quad{
                static_cast<std::uint16_t>(base + lane), static_cast<std::uint16_t>(base + lane + 1),
                static_cast<std::uint16_t>(top + lane + 1), static_cast<std::uint16_t>(base + lane),
                static_cast<std::uint16_t>(top + lane + 1), static_cast<std::uint16_t>(top + lane)};
            submesh.indices.insert(submesh.indices.end(), quad.begin(), quad.end());
        }
    }
    assets::Mesh mesh;
    mesh.name = "river";
    mesh.bounds_min = minimum;
    mesh.bounds_max = maximum;
    mesh.visible = true;
    mesh.submeshes.push_back(std::move(submesh));
    model.meshes.push_back(std::move(mesh));
    return model;
}

} // namespace eawr::presentation::terrain
