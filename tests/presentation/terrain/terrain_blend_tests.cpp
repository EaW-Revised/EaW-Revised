#include "terrain_test_support.hpp"

namespace eawr_terrain_test {

namespace {

constexpr float pi_f = 3.14159265F;

void rotate_rows(std::array<std::array<float, 3>, 3>& m, const std::size_t a, const std::size_t b,
                 const float angle) {
    const float s = std::sin(angle);
    const float c = std::cos(angle);
    for (auto& row : m) {
        const float first = row[a];
        const float second = row[b];
        row[a] = c * first + s * second;
        row[b] = -s * first + c * second;
    }
}

// The retail rows are rebuilt here from the transform's own steps, so the
// closed form in retail_texgen is checked against the construction.
std::array<std::array<float, 3>, 3> retail_rows(const eawr::presentation::terrain::LayerMapping& mapping) {
    std::array<std::array<float, 3>, 3> m{};
    for (std::size_t i = 0; i < 3; ++i) m[i][i] = 1.0F / mapping.tile_size;
    rotate_rows(m, 1, 2, mapping.tilt);     // Rotate_X
    rotate_rows(m, 0, 1, mapping.rotation); // Rotate_Z
    return m;
}

bool same_axis(const eawr::assets::Vec3f axis, const std::array<float, 3>& row) {
    return near_value(axis.x, row[0], 1e-5F) && near_value(axis.y, row[1], 1e-5F)
        && near_value(axis.z, row[2], 1e-5F);
}

float wrapped(const float angle) {
    constexpr float full_turn = 6.28318531F;
    const float turn = std::fmod(angle, full_turn);
    return turn < 0.0F ? turn + full_turn : turn;
}

// --- Blend descriptors and bilinear weights --------------------------
void blend_descriptor_tests() {
    using namespace eawr::presentation::terrain;
    auto blended = synthetic_map(2, 2);
    blended.terrain->samples[0].material_slot = 0;
    blended.terrain->samples[1].material_slot = 1;
    blended.terrain->samples[2].material_slot = 1;
    blended.terrain->samples[3].material_slot = 1;
    blended.terrain->materials[1].fields = {
        {0x07, 0, f32(8.0F)}, {0x08, 0, f32(0.0F)},
        {0x09, 0, f32(0.0F)}, {0x0a, 0, vec3(0.25F, 0.5F, 0.75F)}};
    const auto blend = blend_map(blended);
    expect(blend && blend.value().layers.size() == 2
               && blend.value().sample_layers == std::vector<std::uint8_t>({0, 1, 1, 1}),
           "sample slots are compacted into stable blend layer indices");
    if (blend) {
        const auto& mapping = blend.value().layers[1].mapping;
        const auto axes = retail_texgen(mapping);
        expect(near_value(mapping.tile_size, 8.0F) && near_value(mapping.tint.y, 0.5F)
                   && near_value(axes.u_axis.x, 0.125F) && near_value(axes.v_axis.y, 0.125F),
               "tile size and tint minis decode into the layer texgen");
        const auto weights = blend_weights(blend.value(), 10.0F, 10.0F);
        expect(weights.size() == 2 && weights[0].layer == 0 && weights[1].layer == 1
                   && near_value(weights[0].weight, 0.25F)
                   && near_value(weights[1].weight, 0.75F),
               "three corners sharing a layer combine their bilinear weights");
    }
}

// --- Layer projection: the retail TexU/TexV on every fragment --------
void retail_projection_tests() {
    using namespace eawr::presentation::terrain;
    bool retail_matches_construction = true;
    bool wrapping_keeps_axes = true;
    for (const float tilt : {-4.0F, -pi_f / 2.0F, -0.8029F, -2e-4F, 0.0F, 2e-4F, 0.8029F,
                             pi_f / 2.0F, 1.2217F, pi_f, 4.0F}) {
        for (const float rotation : {-1.2F, 0.0F, 0.5585F, 2.932F, 4.765F, 7.5F}) {
            const LayerMapping mapping{50.0F, rotation, tilt, {1.0F, 1.0F, 1.0F}};
            const TexGen retail = retail_texgen(mapping);
            const auto rows = retail_rows(mapping);
            retail_matches_construction = retail_matches_construction
                && same_axis(retail.u_axis, rows[0]) && same_axis(retail.v_axis, rows[1]);
            // The viewer's parameter texture stores both angles wrapped into
            // [0, 2pi) and no sign; the axes must not change.
            const TexGen stored = retail_texgen({50.0F, wrapped(rotation), wrapped(tilt), {}});
            wrapping_keeps_axes = wrapping_keeps_axes
                && same_axis(stored.u_axis, {retail.u_axis.x, retail.u_axis.y, retail.u_axis.z})
                && same_axis(stored.v_axis, {retail.v_axis.x, retail.v_axis.y, retail.v_axis.z});
        }
    }
    expect(retail_matches_construction,
           "retail TexU/TexV are rows 0 and 1 of Scale, Rotate_X(tilt), Rotate_Z(rotation)");
    expect(wrapping_keeps_axes,
           "rotation and tilt wrapped into [0, 2pi) keep the retail axes for every sign");
}

void rotation_sign_tests() {
    using namespace eawr::presentation::terrain;
    // #125: the rotation sign on flat ground. A quarter turn sends U from
    // source +X to -Y and V from +Y to +X, so a positive rotation turns the
    // texture clockwise seen from +Z. The viewer's earlier flat projection,
    // U = (cos r, sin r, 0) / tile, turned it the other way.
    const TexGen quarter = retail_texgen({10.0F, pi_f / 2.0F, 0.0F, {}});
    const TexGen authored = retail_texgen({50.0F, 0.5585F, 0.0F, {}});
    expect(same_axis(quarter.u_axis, {0.0F, -0.1F, 0.0F}) && same_axis(quarter.v_axis, {0.1F, 0.0F, 0.0F})
               && authored.u_axis.x > 0.0F && authored.u_axis.y < 0.0F
               && authored.v_axis.x > 0.0F && authored.v_axis.y > 0.0F
               && near_value(authored.u_axis.x * authored.v_axis.y - authored.u_axis.y * authored.v_axis.x,
                             1.0F / 2500.0F, 1e-8F),
           "a positive layer rotation turns U toward source -Y, clockwise seen from +Z, unmirrored");

    const LayerMapping upright_wall{50.0F, 0.0F, pi_f / 2.0F, {1.0F, 1.0F, 1.0F}};
    expect(retail_texgen(upright_wall).v_axis.z < 0.0F
               && retail_texgen({50.0F, 0.0F, 0.8029F, {}}).v_axis.z < 0.0F
               && retail_texgen({50.0F, 0.0F, -0.8029F, {}}).v_axis.z > 0.0F
               && retail_texgen({50.0F, 0.5585F, 0.0F, {}}).v_axis.z == 0.0F,
           "a positive retail tilt leans V down source Z, a negative one up, zero not at all");
    expect(retail_texgen({50.0F, 0.0F, pi_f, {}}).v_axis.y < 0.0F
               && retail_texgen({50.0F, 0.0F, -0.8029F, {}}).v_axis.y > 0.0F,
           "a tilt past a quarter turn mirrors V, a small negative one does not");
}

void authored_tilt_tests() {
    using namespace eawr::presentation::terrain;
    // blend_map carries the authored descriptor through, not a per-map choice
    // of tilt sign.
    auto cliff = synthetic_map(3, 3);
    for (std::size_t i = 0; i < cliff.terrain->samples.size(); ++i) {
        cliff.terrain->samples[i].material_slot = 1;
        cliff.terrain->samples[i].height_sample =
            static_cast<std::int16_t>(1000 - static_cast<int>(i / 3) * 1000);
    }
    bool authored_tilt_kept = true;
    for (const float tilt : {-0.8029F, 0.0F, 0.8029F}) {
        cliff.terrain->materials[1].fields = {{0x09, 0, f32(tilt)}};
        const auto cliff_blend = blend_map(cliff);
        authored_tilt_kept = authored_tilt_kept && cliff_blend
            && cliff_blend.value().layers.size() == 1
            && cliff_blend.value().layers[0].mapping.tilt == tilt;
    }
    expect(authored_tilt_kept, "steep layers keep their authored tilt, negative ones included");
}

// Effective FoC family-zero maps: only um11 mixes water and road track types.
void family_zero_selection_tests(eawr::assets::Map& wet, const eawr::assets::RawChunk& river) {
    using namespace eawr::presentation::terrain;
    struct FamilyZeroCase final {
        const char* name;
        std::uint32_t mode;
        std::uint32_t non_water_type;
        std::size_t groups;
        std::size_t water_tracks;
    };
    constexpr std::array family_zero_cases{
        FamilyZeroCase{"_land_planet_bespin_01", 4, 1, 4, 0},
        FamilyZeroCase{"_land_planet_utapau_01", 4, 1, 18, 0},
        FamilyZeroCase{"_mp_land_bespin", 4, 1, 4, 0},
        FamilyZeroCase{"_mp_land_naboo", 5, 2, 4, 0},
        FamilyZeroCase{"_mp_land_utapau", 4, 1, 18, 0},
        FamilyZeroCase{"um01_a_crimelord_unleashed", 4, 1, 8, 0},
        FamilyZeroCase{"um04_visions_of_the_past", 4, 1, 65, 0},
        FamilyZeroCase{"um11_raiders_of_the_lost_holocron", 4, 1, 231, 42},
    };
    for (const auto& entry : family_zero_cases) {
        wet.water_records[0].fields[1].bytes[0] = std::byte{0};
        auto group = river;
        group.children[0].payload.clear();
        mini(group.children[0].payload, 0x01, {std::byte{static_cast<std::uint8_t>(entry.mode)}, std::byte{0}, std::byte{0}, std::byte{0}});
        mini(group.children[0].payload, 0x10, {std::byte{static_cast<std::uint8_t>(entry.non_water_type)}, std::byte{0}, std::byte{0}, std::byte{0}});
        wet.chunks[0].children[0].children[0].children.assign(entry.groups, group);
        for (std::size_t index = 0; index < entry.water_tracks; ++index) {
            wet.chunks[0].children[0].children[0].children[index].children[0].payload[8] = std::byte{0};
        }
        const auto selected = visible_rivers(wet);
        if (selected.rivers.size() != entry.groups ||
            std::any_of(selected.rivers.begin(), selected.rivers.end(),
                [&](const auto& value) { return value.mode != entry.mode; }) ||
            std::count_if(selected.rivers.begin(), selected.rivers.end(), [](const auto& value) {
                return track_pass(value) == TrackPass::water_decoration;
            }) != static_cast<std::ptrdiff_t>(entry.water_tracks)) {
            std::cerr << "FAIL: family-zero selection " << entry.name << '\n';
            ++failures;
        }
    }
}

void track_classification_tests(eawr::assets::Map& wet, const eawr::assets::RawChunk& river) {
    using namespace eawr::presentation::terrain;
    wet.water_records[0].fields[1].bytes[0] = std::byte{0};
    auto group = river;
    group.children[0].payload.clear();
    for (const std::uint32_t mode : {4U, 5U, 9U}) {
        for (const std::uint32_t type : {0U, 1U, 2U}) {
            group.children[0].payload.clear();
            mini(group.children[0].payload, 0x01, {std::byte{static_cast<std::uint8_t>(mode)}, std::byte{0}, std::byte{0}, std::byte{0}});
            mini(group.children[0].payload, 0x10, {std::byte{static_cast<std::uint8_t>(type)}, std::byte{0}, std::byte{0}, std::byte{0}});
            wet.chunks[0].children[0].children[0].children = {group};
            const auto selected = visible_rivers(wet);
            expect(selected.rivers.size() == 1 && selected.rivers[0].mode == mode
                       && selected.rivers[0].type == type
                       && track_pass(selected.rivers[0]) == (type == 0 ? TrackPass::water_decoration : TrackPass::track),
                   "track pass follows mini 0x10 independently of draw mode");
        }
    }
}

// --- Water header and nested river mini streams ----------------------
void water_tests() {
    using namespace eawr::presentation::terrain;
    auto wet = synthetic_map(2, 2);
    wet.water_records.push_back({"1/257/0", 0,
        {{0x15, 0, f32(12.5F)}, {0x16, 0, {std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}}},
         {0x1a, 0, vec3(0.2F, 0.4F, 0.6F)}, {0x20, 0, f32(1.5F)}}});
    wet.water_textures = {{0x1d, "bump.tga"}, {0x1e, "water.tga"}};
    const auto plane = water_plane(wet);
    expect(plane && plane->family == 1 && near_value(plane->height, 12.5F)
               && near_value(plane->alpha, 1.0F) && near_value(plane->color.y, 0.4F)
               && plane->base_texture == "water.tga" && plane->bump_texture == "bump.tga",
           "water header minis decode family, height, tint, clamped alpha and textures");
    wet.water_records[0].fields[1].bytes[0] = std::byte{0};
    expect(!water_plane(wet) && water_header(wet) && near_value(water_header(wet)->alpha, 1.0F),
           "water family zero has no plane but retains ribbon colour and opacity");

    eawr::assets::RawChunk parameters{.id = 257};
    mini(parameters.payload, 0x01, {std::byte{5}, std::byte{0}, std::byte{0}, std::byte{0}});
    mini(parameters.payload, 0x03, f32(2.0F));
    mini(parameters.payload, 0x10, {std::byte{2}, std::byte{0}, std::byte{0}, std::byte{0}});
    eawr::assets::RawChunk points{.id = 258};
    mini(points.payload, 0x07, vec3(0.0F, 0.0F, 1.0F));
    mini(points.payload, 0x07, vec3(10.0F, 0.0F, 1.0F));
    eawr::assets::RawChunk name{.id = 259};
    for (const char c : std::string{"river.tga"}) name.payload.push_back(std::byte{static_cast<unsigned char>(c)});
    name.payload.push_back(std::byte{0});
    eawr::assets::RawChunk widths{.id = 260};
    mini(widths.payload, 0x08, f32(4.0F));
    mini(widths.payload, 0x08, f32(6.0F));
    eawr::assets::RawChunk river{.id = 256, .group = true};
    river.children = {parameters, points, name, widths};
    eawr::assets::RawChunk wave_group{.id = 9, .group = true};
    wave_group.children = {river};
    eawr::assets::RawChunk bundle{.id = 257, .group = true};
    bundle.children = {wave_group};
    eawr::assets::RawChunk root{.id = 1, .group = true};
    root.children = {bundle};
    wet.chunks = {root};
    expect(rivers(wet).rivers.size() == 1 && visible_rivers(wet).rivers.size() == 1,
           "family zero draws its authored mode-5 water ribbon without a plane");
    wet.chunks[0].children[0].children[0].children[0].children[0].payload[2] = std::byte{4};
    expect(visible_rivers(wet).rivers.size() == 1 && visible_rivers(wet).rivers[0].mode == 4,
           "family zero still draws a mode-4 effect ribbon");
    wet.chunks[0].children[0].children[0].children[0].children[0].payload[2] = std::byte{5};
    wet.water_records[0].fields[1].bytes[0] = std::byte{1};
    const auto decoded_rivers = rivers(wet);
    expect(decoded_rivers.rivers.size() == 1 && decoded_rivers.skipped == 0
               && decoded_rivers.rivers[0].points.size() == 2
               && decoded_rivers.rivers[0].widths.size() == 2
               && decoded_rivers.rivers[0].texture == "river.tga"
               && decoded_rivers.rivers[0].mode == 5 && decoded_rivers.rivers[0].type == 2
               && near_value(decoded_rivers.rivers[0].flow, 2.0F),
           "nested river records decode draw mode, track type, points, widths, texture and flow");
    expect(visible_rivers(wet).rivers.size() == 1,
           "an active water family admits authored rivers for drawing");
    family_zero_selection_tests(wet, river);
    track_classification_tests(wet, river);
    wet.chunks = {root};
    wet.water_records[0].fields[1].bytes[0] = std::byte{1};
    const auto level_model = river_model(decoded_rivers.rivers[0], 1, 0.0F);
    auto falling = decoded_rivers.rivers[0];
    falling.points[1].z += 10.0F;
    const auto falling_model = river_model(falling, 1, 0.0F);
    expect(!level_model.meshes.empty() && !falling_model.meshes.empty()
               && falling_model.meshes[0].submeshes[0].vertices[0].color.x
                  > level_model.meshes[0].submeshes[0].vertices[0].color.x,
           "river vertices carry local drop for generic waterfall sheen");
    wet.chunks[0].children[0].children[0].children[0].children[3].payload.clear();
    const auto bad_rivers = rivers(wet);
    expect(bad_rivers.rivers.empty() && bad_rivers.skipped == 1,
           "a river with mismatched points and widths is skipped");
}

} // namespace

void run_blend_tests() {
    blend_descriptor_tests();
    retail_projection_tests();
    rotation_sign_tests();
    authored_tilt_tests();
    water_tests();
}

} // namespace eawr_terrain_test
