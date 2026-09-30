#include "terrain_test_support.hpp"

namespace eawr_terrain_test {

void run_descriptor_tests() {
    using namespace eawr::presentation::terrain;

    // --- Refusals ---------------------------------------------------------
    eawr::assets::Map space;
    space.source = {"data/art/maps/space.ted", "space", "test", eawr::vfs::AssetOrigin::loose, 0};
    space.kind = eawr::assets::MapKind::space;
    space.semantic_complete = true;
    auto no_terrain = build(space);
    expect(!no_terrain && no_terrain.error().code == diagnostic_codes::no_terrain,
           "a space map is an explicit diagnostic, never an empty terrain");

    auto bad_options = build(synthetic_map(9, 5), BuildOptions{0});
    expect(!bad_options && bad_options.error().code == diagnostic_codes::invalid_options,
           "a zero chunk size is refused");
    auto huge_options = build(synthetic_map(9, 5), BuildOptions{max_chunk_cells + 1});
    expect(!huge_options && huge_options.error().code == diagnostic_codes::invalid_options,
           "a chunk size past the 16-bit index bound is refused");

    auto degenerate = build(synthetic_map(1, 5));
    expect(!degenerate && degenerate.error().code == diagnostic_codes::invalid_terrain,
           "a grid with no complete cell is refused");

    eawr::assets::Map short_samples = synthetic_map(9, 5);
    short_samples.terrain->samples.pop_back();
    auto truncated = build(short_samples);
    expect(!truncated && truncated.error().code == diagnostic_codes::invalid_terrain,
           "a sample count that disagrees with the grid is refused");

    const float infinity = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const float spacing : std::array<float, 5>{0.0F, -1.0F, nan, infinity, -infinity}) {
        auto invalid = synthetic_map(3, 3);
        invalid.terrain->cell_spacing = spacing;
        const auto result = build(invalid);
        expect(!result && result.error().code == diagnostic_codes::invalid_terrain
                   && result.error().logical_path == invalid.source.logical_path,
               "nonpositive or nonfinite spacing has a sourced terrain diagnostic");
    }
    for (const float scale : std::array<float, 3>{nan, infinity, -infinity}) {
        auto invalid = synthetic_map(3, 3);
        invalid.terrain->height_scale = scale;
        const auto result = build(invalid);
        expect(!result && result.error().code == diagnostic_codes::invalid_terrain,
               "nonfinite height scale is refused");
    }
    for (const float scale : std::array<float, 2>{0.0F, -1.0F}) {
        auto signed_scale = synthetic_map(3, 3, 2, true);
        signed_scale.terrain->height_scale = scale;
        const auto result = build(signed_scale);
        expect(result && result.value().bounds_max.z <= 0.0F,
               "finite zero or negative height scale retains existing geometry semantics");
    }

    auto wide_extent = synthetic_map(3, 3);
    wide_extent.terrain->cell_spacing = std::numeric_limits<float>::max();
    const auto outside_extent = build(wide_extent);
    expect(!outside_extent && outside_extent.error().code == diagnostic_codes::invalid_terrain,
           "finite spacing whose grid extent exceeds float range is refused");

    auto tall_sample = synthetic_map(3, 3);
    tall_sample.terrain->height_scale = std::numeric_limits<float>::max();
    tall_sample.terrain->samples.front().height_sample = 2;
    const auto outside_height = build(tall_sample);
    expect(!outside_height && outside_height.error().code == diagnostic_codes::invalid_terrain,
           "finite scale whose sample height exceeds float range is refused");

    auto too_many_materials = synthetic_map(3, 3, 257);
    const auto outside_descriptors = build(too_many_materials);
    expect(!outside_descriptors
               && outside_descriptors.error().code == diagnostic_codes::invalid_terrain,
           "257 material descriptors are refused before slot IDs wrap");

    auto last_slot = synthetic_map(2, 2, 256);
    last_slot.terrain->samples.front().material_slot = 255;
    const auto accepted_slot = build(last_slot);
    expect(accepted_slot && accepted_slot.value().slots.size() == 256
               && accepted_slot.value().chunks.front().surfaces.front().material_slot == 255,
           "256 descriptors retain the final 8-bit slot without wrapping");

    const auto finite_mesh = [](const Mesh& mesh) {
        const auto finite_vec = [](const eawr::assets::Vec3f value) {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        };
        if (!finite_vec(mesh.bounds_min) || !finite_vec(mesh.bounds_max)) return false;
        for (const Chunk& chunk : mesh.chunks) {
            if (!finite_vec(chunk.bounds_min) || !finite_vec(chunk.bounds_max)) return false;
            for (const Surface& surface : chunk.surfaces) {
                for (const auto& vertex : surface.vertices) {
                    if (!finite_vec(vertex.position) || !finite_vec(vertex.normal)) return false;
                    const auto& normal = vertex.normal;
                    const double length = std::hypot(
                        static_cast<double>(normal.x), normal.y, normal.z);
                    if (std::abs(length - 1.0) > 1e-6) return false;
                }
            }
        }
        return true;
    };
    auto finite_limit = synthetic_map(3, 3);
    finite_limit.terrain->cell_spacing = std::numeric_limits<float>::max() / 2.0F;
    finite_limit.terrain->height_scale = std::numeric_limits<float>::max();
    for (std::size_t index = 0; index < finite_limit.terrain->samples.size(); ++index) {
        finite_limit.terrain->samples[index].height_sample = index % 2 == 0 ? 1 : -1;
    }
    const auto at_limit = build(finite_limit);
    expect(at_limit && finite_mesh(at_limit.value()),
           "representable extreme positions and height differences yield finite unit normals");

    auto tiny_spacing = synthetic_map(3, 3);
    tiny_spacing.terrain->cell_spacing = std::numeric_limits<float>::denorm_min();
    tiny_spacing.terrain->height_scale = std::numeric_limits<float>::max();
    tiny_spacing.terrain->samples[1].height_sample = 1;
    const auto steep = build(tiny_spacing);
    expect(steep && finite_mesh(steep.value()),
           "tiny positive spacing with a steep finite height difference has unit normals");

    eawr::assets::Map bad_slot = synthetic_map(9, 5);
    bad_slot.terrain->samples.front().material_slot = 9;
    auto outside = build(bad_slot);
    expect(!outside && outside.error().code == diagnostic_codes::invalid_terrain,
           "a sample naming a slot outside the descriptors is refused");

    // --- A present-but-empty texture name --------------------------------
    expect(!declared(std::nullopt) && !declared(std::optional<std::string>{})
               && !declared(std::optional<std::string>{"   "})
               && declared(std::optional<std::string>{"a.tga"}),
           "only a name with content counts as declared");
    eawr::assets::Map empty_name = synthetic_map(9, 5);
    empty_name.terrain->materials[1].primary_texture = "";
    auto empty_result = build(empty_name);
    expect(bool(empty_result), "a present-but-empty texture name still builds");
    if (empty_result) {
        expect(empty_result.value().slots[1].effect == SurfaceEffect::undeclared,
               "a present-but-empty texture name selects no effect");
    }
    eawr::assets::Map empty_secondary = synthetic_map(9, 5);
    empty_secondary.terrain->materials[1].secondary_texture = "";
    auto secondary_result = build(empty_secondary);
    expect(bool(secondary_result)
               && secondary_result.value().slots[1].effect
                   == SurfaceEffect::terrain_render_bump_nobump,
           "a present-but-empty secondary name falls back to the single-diffuse effect");

    // --- No declared materials -------------------------------------------
    auto untextured = build(synthetic_map(5, 5, 0));
    expect(bool(untextured), "a heightfield with no material descriptors still builds");
    if (untextured) {
        expect(untextured.value().slots.empty(), "no descriptors means no reported slots");
        bool noticed = false;
        for (const auto& notice : untextured.value().notices) {
            noticed = noticed || notice.message.find("no material descriptors") != std::string::npos;
        }
        expect(noticed, "building untextured geometry is noticed, not silent");
    }
}

} // namespace eawr_terrain_test
