#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/core/result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::terrain {

// TED sample data is right-handed X-right/Y-forward/Z-up with 20-unit cells and
// a 25/512 height scale (see the TED coordinate behaviour note in
// docs/asset-formats.md).  Geometry produced here stays in that source basis,
// exactly like `assets::Model` does, because the single documented
// asset-to-render conversion `(x, y, z) -> (x, z, -y)` belongs at the one
// existing Godot upload boundary.  Converting here as well would rotate the
// terrain twice.  `asset_to_render` is provided for a consumer that needs a
// render-basis value of its own, such as a camera framed on the map bounds.
[[nodiscard]] constexpr assets::Vec3f asset_to_render(const assets::Vec3f value) noexcept {
    return {value.x, value.z, -value.y};
}

// Index buffers are 16-bit, so a chunk must stay under 65,536 vertices.  A
// 32x32-cell chunk is 33x33 = 1,089 vertices and 2,048 triangles, which leaves
// the bound uncontested rather than merely satisfied.
inline constexpr std::uint32_t default_chunk_cells = 32;
inline constexpr std::uint32_t max_chunk_cells = 128;

struct BuildOptions final {
    std::uint32_t chunk_cells{default_chunk_cells};
};

// One drawable run inside a chunk, split by the material slot of each cell's
// anchor sample. The split only groups geometry: the map mode draws every
// surface with one blended material (see BlendMap below).
struct Surface final {
    std::uint8_t material_slot{};
    std::vector<assets::Vertex> vertices;
    std::vector<std::uint16_t> indices;
};

struct Chunk final {
    std::uint32_t chunk_x{};
    std::uint32_t chunk_y{};
    std::uint32_t cell_origin_x{};
    std::uint32_t cell_origin_y{};
    std::uint32_t cells_x{};
    std::uint32_t cells_y{};
    // Source-basis bounds of this chunk's own geometry.
    assets::Vec3f bounds_min{};
    assets::Vec3f bounds_max{};
    // Ordered by ascending material slot, so chunk contents are deterministic.
    std::vector<Surface> surfaces;
};

// Which TERRAIN-family effect a material slot's declared textures select, and
// what each texture resolves to.  The selection rule is recorded with the
// result so a reader never has to infer it from the numbers.
enum class SurfaceEffect : std::uint8_t {
    // No primary texture is declared; nothing is selected and nothing is
    // guessed.
    undeclared,
    // One declared diffuse texture and no declared normal map.
    terrain_render_bump_nobump,
    // Two declared diffuse textures, which is what the dual effect's
    // DiffuseTexture0/DiffuseTexture1/BlendTexture samplers take.
    terrain_render_bump_dual,
};

[[nodiscard]] constexpr std::string_view effect_program(const SurfaceEffect effect) noexcept {
    switch (effect) {
    case SurfaceEffect::terrain_render_bump_nobump: return "TerrainRenderBump.fx";
    case SurfaceEffect::terrain_render_bump_dual: return "TerrainRenderBumpDual.fx";
    case SurfaceEffect::undeclared: break;
    }
    return {};
}

[[nodiscard]] constexpr std::string_view effect_technique(const SurfaceEffect effect) noexcept {
    switch (effect) {
    case SurfaceEffect::terrain_render_bump_nobump: return "nobump";
    case SurfaceEffect::terrain_render_bump_dual: return "bump";
    case SurfaceEffect::undeclared: break;
    }
    return {};
}

[[nodiscard]] constexpr std::string_view effect_pass(const SurfaceEffect effect) noexcept {
    switch (effect) {
    case SurfaceEffect::terrain_render_bump_nobump: return "nobump_p0";
    case SurfaceEffect::terrain_render_bump_dual: return "bump_p0";
    case SurfaceEffect::undeclared: break;
    }
    return {};
}

// A declared reference is one that actually names something.  A TED material
// descriptor can carry a present-but-empty texture mini, and treating that as a
// declared texture would report a selector the map never asked for.
[[nodiscard]] bool declared(const std::optional<std::string>& name);

struct MaterialSlot final {
    std::uint8_t slot{};
    SurfaceEffect effect{SurfaceEffect::undeclared};
    // Authored names exactly as the TED declares them, plus the logical VFS
    // path each one is looked up under.  The suffix an authored name carries is
    // not necessarily the shipped one, so resolution stays the caller's job
    // through the asset layer's own probe.
    std::optional<std::string> primary_texture;
    std::optional<std::string> secondary_texture;
    // Cells in the heightfield that carry this slot.  A slot with no cells is
    // still reported: an unused declared slot is not the same as an absent one.
    std::uint64_t cells{};
};

struct Mesh final {
    assets::Source source;
    std::uint32_t chunk_cells{};
    std::uint32_t chunks_x{};
    std::uint32_t chunks_y{};
    std::uint32_t grid_width{};
    std::uint32_t grid_height{};
    float cell_spacing{};
    float height_scale{};
    std::vector<Chunk> chunks;
    std::vector<MaterialSlot> slots;
    std::uint64_t vertex_count{};
    std::uint64_t triangle_count{};
    // Source-basis bounds over every chunk.
    assets::Vec3f bounds_min{};
    assets::Vec3f bounds_max{};
    std::vector<assets::Notice> notices;
};

namespace diagnostic_codes {
inline constexpr std::string_view no_terrain = "EAWR-TERRAIN-0001";
inline constexpr std::string_view invalid_options = "EAWR-TERRAIN-0002";
inline constexpr std::string_view invalid_terrain = "EAWR-TERRAIN-0003";
inline constexpr std::string_view unsupported_surface = "EAWR-TERRAIN-0004";
} // namespace diagnostic_codes

// Builds chunked triangle meshes from a loaded map's semantic terrain view.
// The map is consumed as CPU data; nothing here opens a file or touches a
// graphics API.  A space map, or a structurally retained raw-only map, has no
// terrain and is an explicit diagnostic rather than an empty success.
[[nodiscard]] core::Result<Mesh> build(
    const assets::Map& map, const BuildOptions& options = {});

// Converts one chunk into the asset model shape the renderer already uploads,
// with one submesh per surface. The submesh shader name is the TERRAIN-family
// program its slot selects, so the recorded selector travels with the geometry.
[[nodiscard]] assets::Model chunk_model(const Mesh& mesh, const Chunk& chunk);

// One surface on its own. The renderer binds a single texture per uploaded
// asset, so a consumer that wants each slot's own diffuse texture uploads one
// surface at a time rather than one chunk at a time.
[[nodiscard]] assets::Model surface_model(
    const Mesh& mesh, const Chunk& chunk, const Surface& surface);

// ---- Layer blending (P1-06 #27) ---------------------------------------------
//
// How one slot's diffuse texture is laid over the ground, from the slot's
// 1/257/2/3 descriptor minis (docs/asset-formats.md): 0x07 world units per
// texture repeat, 0x08 rotation about source +Z in radians, 0x09 tilt of the
// projection about its U axis in radians, 0x0a diffuse tint. A missing or
// malformed mini keeps its default. blend_map keeps the authored values.
struct LayerMapping final {
    float tile_size{100.0F};
    float rotation{};
    float tilt{};
    assets::Vec3f tint{1.0F, 1.0F, 1.0F};
};
[[nodiscard]] LayerMapping decode_mapping(const assets::TerrainMaterial& material);

// Texture coordinates in the source basis: u = dot(u_axis, p), v = dot(v_axis, p).
struct TexGen final {
    assets::Vec3f u_axis;
    assets::Vec3f v_axis;
};

// The retail TerrainRenderBump TexU/TexV: rows 0 and 1 of the layer transform
// Scale(1/tile) * Rotate_X(tilt) * Rotate_Z(rotation), in row-vector order, so
// TexU = (cos r, -sin r, 0) / tile and TexV = (sin r cos t, cos r cos t, -sin t) / tile.
// Each diffuse pass dots them with the source position. This holds for every
// authored tilt, negative ones included, and for flat and steep ground alike:
// a positive rotation turns the texture clockwise seen from above (+Z).
[[nodiscard]] TexGen retail_texgen(const LayerMapping& mapping);

// Every grid sample names one slot. A slot's weight at a point is the bilinear
// interpolation, over the four corner samples of the point's cell, of "this
// sample names the slot", so the weights at every point sum to one and a
// transition spans one cell. Layers are the slots at least one sample names,
// in ascending slot order; sample_layers indexes them row-major.
struct BlendLayer final {
    std::uint8_t slot{};
    LayerMapping mapping;
    std::uint64_t samples{};
};
struct BlendMap final {
    std::uint32_t width{};
    std::uint32_t height{};
    float cell_spacing{};
    std::vector<BlendLayer> layers;
    std::vector<std::uint8_t> sample_layers;
};
[[nodiscard]] core::Result<BlendMap> blend_map(const assets::Map& map);

struct LayerWeight final {
    std::uint8_t layer{};
    float weight{};
};
// Weights at source (x, y), clamped to the grid, merged per layer and sorted
// by layer. Reference arithmetic for the GPU adapter's blend.
[[nodiscard]] std::vector<LayerWeight> blend_weights(const BlendMap& map, float x, float y);

// ---- Water (P1-06 #27) --------------------------------------------------------
//
// The map-wide plane from the 1/257/0 minis: 0x15 height, 0x16 family (0 none,
// 1 water, 2 lava, 3 ice), 0x1a colour, 0x20 alpha, 0x1d/0x1e bump and base
// texture names. Absent when the family is 0 or the header carries none.
struct WaterPlane final {
    std::uint32_t family{};
    float height{};
    assets::Vec3f color{1.0F, 1.0F, 1.0F};
    float alpha{0.5F};
    std::string bump_texture;
    std::string base_texture;
};
// Decode the authored header even for family zero: mode-5 ribbons use its
// colour and opacity without creating a map-wide water plane.
[[nodiscard]] std::optional<WaterPlane> water_header(const assets::Map& map);
[[nodiscard]] std::optional<WaterPlane> water_plane(const assets::Map& map);

// One authored river under 1/257/9: group 256 holds 257 (parameters; 0x03 is
// read as the flow scroll rate), 258 (0x07 source-basis control points), 259
// (the texture name as a bare string, not a mini stream) and 260 (0x08 width
// per point). A river whose point and width counts disagree, or that has
// fewer than two points, is skipped and counted.
struct River final {
    std::string texture;
    std::vector<assets::Vec3f> points;
    std::vector<float> widths;
    float flow{};
    std::uint32_t mode{}; // group 257 mini 0x01; 4 also carries non-water effect ribbons
    std::uint32_t type{}; // group 257 mini 0x10: 0 water, 1 road, 2 river
};
enum class TrackPass { water_decoration, track };
// FoC debug build: only water-type tracks enter water decorations (AU-137/138).
[[nodiscard]] constexpr TrackPass track_pass(const River& river) {
    return river.type == 0 ? TrackPass::water_decoration : TrackPass::track;
}
struct Rivers final {
    std::vector<River> rivers;
    std::uint32_t skipped{};
};
[[nodiscard]] Rivers rivers(const assets::Map& map);

// Every decoded terrain track is visible independently of its draw mode and
// map-wide water family. `track_pass` selects its retail submission pass.
[[nodiscard]] Rivers visible_rivers(const assets::Map& map);

// A ribbon along the river: Catmull-Rom through the control points,
// `subdivisions` segments per span, each cross-section as wide as the
// interpolated width in source XY at the curve's height plus `lift`.
// texcoord[0].x runs 0..1 across, .y is arc length divided by the local width.
// Vertex colour alpha fades to 0 at both banks. Source basis, like every other
// terrain model.
[[nodiscard]] assets::Model river_model(const River& river, std::uint32_t subdivisions, float lift);

} // namespace eawr::presentation::terrain
