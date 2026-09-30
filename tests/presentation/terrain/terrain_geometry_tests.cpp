#include "terrain_test_support.hpp"

namespace eawr_terrain_test {

void run_geometry_tests() {
    using namespace eawr::presentation::terrain;

    // --- Basis conversion -------------------------------------------------
    const auto converted = asset_to_render({1.0F, 2.0F, 3.0F});
    expect(near_value(converted.x, 1.0F) && near_value(converted.y, 3.0F)
               && near_value(converted.z, -2.0F),
           "the one asset-to-render conversion is (x, y, z) -> (x, z, -y)");

    // --- A flat map -------------------------------------------------------
    const auto flat_map = synthetic_map(9, 5);
    auto flat = build(flat_map);
    expect(bool(flat), "a valid land heightfield builds");
    if (flat) {
        const Mesh& mesh = flat.value();
        expect(mesh.grid_width == 9 && mesh.grid_height == 5, "grid extents are retained");
        expect(near_value(mesh.cell_spacing, 20.0F), "cell spacing is the 20-unit source constant");
        expect(near_value(mesh.height_scale, 25.0F / 512.0F), "height scale is 25/512");
        // 8x4 cells at the default 32-cell chunk size is a single chunk.
        expect(mesh.chunks_x == 1 && mesh.chunks_y == 1 && mesh.chunks.size() == 1,
               "an 8x4-cell grid is one default-sized chunk");
        expect(mesh.triangle_count == 8 * 4 * 2, "every cell contributes two triangles");
        expect(mesh.vertex_count == 8 * 4 * 4, "a split-by-slot chunk keeps per-cell corners");

        // A flat map spans exactly (width-1) and (height-1) cells of 20 units
        // in the source basis, which is the basis the geometry stays in.
        expect(near_value(mesh.bounds_min.x, 0.0F) && near_value(mesh.bounds_max.x, 160.0F),
               "source X spans eight 20-unit cells");
        expect(near_value(mesh.bounds_min.y, 0.0F) && near_value(mesh.bounds_max.y, 80.0F),
               "source Y spans four 20-unit cells and is not pre-rotated");
        expect(near_value(mesh.bounds_min.z, 0.0F) && near_value(mesh.bounds_max.z, 0.0F),
               "a flat heightfield has zero source height");
        // The same bounds through the documented conversion are what a camera
        // consumer frames on.
        const auto render_max = asset_to_render(mesh.bounds_max);
        expect(near_value(render_max.x, 160.0F) && near_value(render_max.y, 0.0F)
                   && near_value(render_max.z, -80.0F),
               "converted bounds put source Y forward on negative render Z");

        std::size_t inspected{};
        for (const Chunk& chunk : mesh.chunks) {
            for (const Surface& surface : chunk.surfaces) {
                expect(surface.indices.size() % 3 == 0, "a surface is whole triangles");
                for (std::size_t index = 0; index + 2 < surface.indices.size(); index += 3) {
                    const auto& a = surface.vertices[surface.indices[index]].position;
                    const auto& b = surface.vertices[surface.indices[index + 1]].position;
                    const auto& c = surface.vertices[surface.indices[index + 2]].position;
                    const auto geometric = cross(difference(b, a), difference(c, a));
                    if (geometric.z <= 0.0F) {
                        expect(false, "every flat triangle winds counter-clockwise about source up");
                        break;
                    }
                    // The asset-to-render rotation is proper, so the same
                    // winding is counter-clockwise about render up as well.
                    const auto rendered = cross(
                        difference(asset_to_render(b), asset_to_render(a)),
                        difference(asset_to_render(c), asset_to_render(a)));
                    if (rendered.y <= 0.0F) {
                        expect(false, "the conversion preserves triangle winding");
                        break;
                    }
                    ++inspected;
                }
            }
        }
        expect(inspected == 8 * 4 * 2, "every triangle was wound-checked");

        // Slot assignment alternates by (column + row) % 2, so both declared
        // slots carry cells and the totals close.
        expect(mesh.slots.size() == 2, "each declared material descriptor is a reported slot");
        std::uint64_t slot_cells{};
        for (const MaterialSlot& slot : mesh.slots) slot_cells += slot.cells;
        expect(slot_cells == 8 * 4, "slot cell counts sum to the cell count");
        expect(mesh.slots[0].cells > 0 && mesh.slots[1].cells > 0,
               "an alternating heightfield uses both slots");
        expect(mesh.slots[0].effect == SurfaceEffect::terrain_render_bump_nobump,
               "one declared diffuse texture selects the single-diffuse TERRAIN effect");
        expect(mesh.slots[1].effect == SurfaceEffect::terrain_render_bump_dual,
               "two declared diffuse textures select the dual-diffuse TERRAIN effect");
        expect(effect_program(mesh.slots[0].effect) == "TerrainRenderBump.fx"
                   && effect_technique(mesh.slots[0].effect) == "nobump"
                   && effect_pass(mesh.slots[0].effect) == "nobump_p0",
               "the single-diffuse selector names a real descriptor-bundle technique");
        expect(effect_program(mesh.slots[1].effect) == "TerrainRenderBumpDual.fx"
                   && effect_technique(mesh.slots[1].effect) == "bump"
                   && effect_pass(mesh.slots[1].effect) == "bump_p0",
               "the dual-diffuse selector names a real descriptor-bundle technique");
        expect(effect_program(SurfaceEffect::undeclared).empty(),
               "an undeclared surface selects no effect rather than a default one");

        // Chunk order is row-major and stable, which is what makes an upload
        // identity reproducible between runs.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> order;
        for (const Chunk& chunk : mesh.chunks) order.emplace_back(chunk.chunk_y, chunk.chunk_x);
        expect(std::is_sorted(order.begin(), order.end()), "chunks are emitted in row-major order");

        auto again = build(flat_map);
        expect(bool(again) && again.value().vertex_count == mesh.vertex_count
                   && again.value().triangle_count == mesh.triangle_count,
               "building the same map twice produces the same counts");
        if (again) {
            bool identical = again.value().chunks.size() == mesh.chunks.size();
            for (std::size_t index = 0; identical && index < mesh.chunks.size(); ++index) {
                const Chunk& left = mesh.chunks[index];
                const Chunk& right = again.value().chunks[index];
                identical = left.surfaces.size() == right.surfaces.size();
                for (std::size_t surface = 0; identical && surface < left.surfaces.size(); ++surface) {
                    identical = left.surfaces[surface].indices == right.surfaces[surface].indices
                        && left.surfaces[surface].material_slot
                            == right.surfaces[surface].material_slot;
                }
            }
            expect(identical, "vertex and index order is deterministic");
        }

        {
            const eawr::assets::Model model = chunk_model(mesh, mesh.chunks.front());
            expect(model.meshes.size() == 1, "a chunk becomes one asset mesh");
            expect(model.meshes.front().submeshes.size() == mesh.chunks.front().surfaces.size(),
                   "each surface becomes one submesh");
            bool carries_selector = true;
            for (const auto& submesh : model.meshes.front().submeshes) {
                carries_selector = carries_selector && !submesh.shader.empty()
                    && !submesh.vertices.empty() && !submesh.indices.empty();
            }
            expect(carries_selector, "every submesh carries its TERRAIN selector and geometry");
        }
    }

    // --- A sloped map: normals are a closed form --------------------------
    auto sloped = build(synthetic_map(9, 5, 2, true));
    expect(bool(sloped), "a sloped heightfield builds");
    if (sloped) {
        // Height rises 8 samples per column, so source dz/dx is
        // 8 * (25/512) / 20 and the interior normal is the normalized
        // (-dz/dx, 0, 1) converted to the render basis.
        const float slope = 8.0F * (25.0F / 512.0F) / 20.0F;
        const float length = std::sqrt(slope * slope + 1.0F);
        bool interior_checked = false;
        for (const Chunk& chunk : sloped.value().chunks) {
            for (const Surface& surface : chunk.surfaces) {
                for (const auto& vertex : surface.vertices) {
                    // Only interior columns have a two-sided difference.
                    if (vertex.position.x < 25.0F || vertex.position.x > 135.0F) continue;
                    expect(near_value(vertex.normal.x, -slope / length, 1e-3F)
                               && near_value(vertex.normal.y, 0.0F, 1e-3F)
                               && near_value(vertex.normal.z, 1.0F / length, 1e-3F),
                           "an interior slope normal matches the closed form in the source basis");
                    interior_checked = true;
                    break;
                }
                if (interior_checked) break;
            }
            if (interior_checked) break;
        }
        expect(interior_checked, "at least one interior normal was checked");
        expect(sloped.value().bounds_max.z > sloped.value().bounds_min.z,
               "a sloped heightfield has a nonzero source height range");
    }

    // --- Chunking ---------------------------------------------------------
    auto chunked = build(synthetic_map(17, 17), BuildOptions{4});
    expect(bool(chunked), "a smaller chunk size builds");
    if (chunked) {
        const Mesh& mesh = chunked.value();
        expect(mesh.chunks_x == 4 && mesh.chunks_y == 4, "16 cells at 4 per chunk is a 4x4 grid");
        expect(mesh.triangle_count == 16 * 16 * 2,
               "chunking changes the partition, not the triangle count");
        std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
        for (const Chunk& chunk : mesh.chunks) {
            expect(chunk.cells_x <= 4 && chunk.cells_y <= 4, "no chunk exceeds its cell bound");
            expect(seen.emplace(chunk.chunk_x, chunk.chunk_y).second, "chunk coordinates are unique");
        }
    }

    // A ragged grid whose cells do not divide evenly still tiles exactly once.
    auto ragged = build(synthetic_map(10, 7), BuildOptions{4});
    expect(bool(ragged), "a ragged grid builds");
    if (ragged) {
        expect(ragged.value().triangle_count == 9 * 6 * 2,
               "a partial edge chunk covers the remaining cells exactly once");
    }
}

} // namespace eawr_terrain_test
