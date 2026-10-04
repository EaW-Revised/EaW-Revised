#include "map_test_support.hpp"

namespace eawr::tests::map_contracts {
// --- Terrain semantics T1, T2 and T4 ----------------------------------------
void terrain_values() {
    using namespace eawr::assets;
    auto result = load(Fixture{});
    expect(bool(result), "minimal clean-note land TED loads");
    if (!result) return;
    const auto& map = result.value();
    expect(map.semantic_complete, "semantic map is not reported raw-only");
    expect(map.terrain.has_value() && map.terrain->samples.size() == 6, "terrain samples are retained");
    if (!map.terrain || map.terrain->samples.size() != 6) return;
    const auto& terrain = *map.terrain;
    expect(terrain.width == 2 && terrain.height == 3 && terrain.cell_spacing == 20.0F && terrain.height_scale == 25.0F / 512.0F,
           "terrain grid keeps the fixed spacing and height scale");
    const float expected_z[]{-25.0F, -0.048828125F, 0.0F, 0.048828125F, 25.0F, 100.0F};
    const float expected_x[]{0, 20, 0, 20, 0, 20};
    const float expected_y[]{0, 0, 20, 20, 40, 40};
    const float expected_gray[]{0.0F, 0.2F, 0.4F, 0.6F, 0.8F, 1.0F};
    for (std::size_t index = 0; index < 6; ++index) {
        const auto& sample = terrain.samples[index];
        const float x = static_cast<float>(index % terrain.width) * terrain.cell_spacing;
        const float y = static_cast<float>(index / terrain.width) * terrain.cell_spacing;
        const float z = static_cast<float>(sample.height_sample) * terrain.height_scale;
        expect(x == expected_x[index] && y == expected_y[index], "source XY follows row-major 20-unit spacing");
        expect(z == expected_z[index], "source Z is the signed sample times exactly 25/512");
        expect(near(static_cast<float>(sample.vertex_intensity) / 255.0F, expected_gray[index]), "intensity is linear v/255 grayscale");
        expect(sample.material_slot == index, "byte 2 is the material slot");
    }
    expect(terrain.samples[1].height_sample == -1, "a -1 height does not decode as 65535");
    expect(terrain.samples[2].height_sample == 0 && terrain.samples[2].material_slot == 2, "sample 2 is (x=0,y=1)");
    expect(terrain.header_fields.size() == 7, "every terrain header mini, grid and water, is retained");
    expect(terrain.raw_plane.size() == 24, "the raw sample plane is retained");
    expect(map.raw_passability.size() == 24, "passability plane is retained after grid validation");

    // T4: a flat 2 x 2 grid with slots [0,1;0,1]; only intensity varies.
    Fixture flat; flat.width = 2; flat.height = 2; flat.cell_count = 4; flat.materials = 2;
    flat.heights = {0, 0, 0, 0}; flat.slots = {0, 1, 0, 1}; flat.intensities = {10, 20, 30, 40};
    Fixture brighter = flat; brighter.intensities = {200, 210, 220, 230};
    auto dim = load(flat); auto bright = load(brighter);
    expect(dim && bright && dim.value().terrain && bright.value().terrain, "flat material-boundary grids load");
    if (dim && bright && dim.value().terrain && bright.value().terrain) {
        const auto& left = dim.value().terrain->samples; const auto& right = bright.value().terrain->samples;
        bool same_geometry = left.size() == 4 && right.size() == 4;
        for (std::size_t index = 0; same_geometry && index < 4; ++index) {
            same_geometry = left[index].height_sample == right[index].height_sample && left[index].material_slot == right[index].material_slot;
        }
        expect(same_geometry, "intensity changes neither geometry nor material selection");
        expect(left.size() == 4 && left[0].material_slot == 0 && left[1].material_slot == 1 && left[2].material_slot == 0 && left[3].material_slot == 1,
               "byte 2 alone selects the material region");
        expect(left.size() == 4 && right.size() == 4 && left[0].vertex_intensity != right[0].vertex_intensity, "only vertex intensity differs");
    }
}

// --- T3 and M4: a failed terrain view keeps the load and the raw tree ------
void expect_terrain_view_absent(const Fixture& fixture, const eawr::assets::MapIssue kind, const char* message,
                                const std::uint32_t leaf = 5) {
    auto result = load(fixture);
    expect(bool(result), message);
    if (!result) return;
    const auto& map = result.value();
    expect(!map.terrain.has_value(), "a failed terrain view produces no partial terrain");
    expect(!map.semantic_complete, "a failed terrain view is not semantic completion");
    expect(count_issue(map, kind) == 1, message);
    expect(raw_terrain_plane(map, leaf) != nullptr, "the raw terrain plane stays in the lossless tree");
    expect(map.placements.size() == 1, "placements still decode when terrain does not");
    expect(map.raw_passability.empty(), "passability is not exposed without a validated grid");
    expect(count_issue(map, eawr::assets::MapIssue::passability_size) == 1, "the unvalidated passability plane is named");
}

void terrain_failures() {
    using eawr::assets::MapIssue;
    Fixture short_plane; short_plane.plane_delta = -1;
    expect_terrain_view_absent(short_plane, MapIssue::terrain_plane_size, "a plane one byte short is a notice, not a failed load");
    Fixture long_plane; long_plane.plane_delta = 1;
    expect_terrain_view_absent(long_plane, MapIssue::terrain_plane_size, "a plane one byte long is a notice");
    Fixture zero; zero.width = 0; zero.cell_count = 0;
    expect_terrain_view_absent(zero, MapIssue::terrain_dimensions, "a zero dimension is a notice");
    Fixture overflow; overflow.width = 0xffffffffU; overflow.height = 0xffffffffU; overflow.cell_count = 0;
    expect_terrain_view_absent(overflow, MapIssue::terrain_dimensions, "an overflowing product is a notice");
    Fixture bad_count; bad_count.cell_count = 7;
    expect_terrain_view_absent(bad_count, MapIssue::terrain_cell_count, "a header cell count that disagrees is a notice");
    Fixture slot; slot.slots = {0, 1, 2, 3, 4, 6}; // slot 6 == materials.size()
    expect_terrain_view_absent(slot, MapIssue::terrain_material_slot, "a slot equal to the material count is rejected");
    Fixture no_materials; no_materials.materials = 0; no_materials.slots = {0, 0, 0, 0, 0, 0};
    expect_terrain_view_absent(no_materials, MapIssue::terrain_material_slot, "zero descriptors accept no slot at all");
    if (auto slot_result = load(slot)) {
        const auto* notice = find_issue(slot_result.value(), MapIssue::terrain_material_slot);
        const auto* plane = raw_terrain_plane(slot_result.value());
        expect(notice && plane && notice->byte_offset == plane->header_offset + 8U + 5U * 4U + 2U && notice->chunk_path == "1/257/5",
               "the slot notice names the sample's own byte offset and chunk path");
    }
    Fixture legacy; legacy.legacy_plane = true;
    expect_terrain_view_absent(legacy, MapIssue::terrain_legacy_plane, "a legacy 1/257/1 plane is named, never given an invented intensity", 1);
}

// --- D5: kind and body structure agree, or say so --------------------------
void kind_structure() {
    using namespace eawr::assets;
    auto space = load(space_fixture());
    expect(space && space.value().semantic_complete && !space.value().terrain, "space map has no invented land terrain");
    Fixture space_with_land = space_fixture(); space_with_land.land_groups = true;
    auto mixed = load(space_with_land);
    expect(bool(mixed), "a space map carrying land groups still loads");
    if (mixed) {
        expect(!mixed.value().terrain && mixed.value().raw_passability.empty(), "a space map never gains a terrain view");
        expect(count_issue(mixed.value(), MapIssue::kind_structure_mismatch) == 3, "each land-only group in a space map is named");
        expect(!mixed.value().semantic_complete, "a kind/structure mismatch is not semantic completion");
        expect(raw_terrain_plane(mixed.value()) != nullptr, "the space map's land bytes stay raw");
    }
    Fixture land_without_patches; land_without_patches.patch_group = false;
    auto partial = load(land_without_patches);
    expect(partial && count_issue(partial.value(), MapIssue::kind_structure_mismatch) == 1 && !partial.value().semantic_complete,
           "a land map missing 1/267 is a named mismatch");
    Fixture bare_land; bare_land.land_groups = false;
    auto bare = load(bare_land);
    expect(bare && count_issue(bare.value(), MapIssue::kind_structure_mismatch) == 3 && !bare.value().semantic_complete,
           "a land map with no land groups is a named mismatch");
    Fixture raw_only; raw_only.include_main = false;
    auto raw = load(raw_only);
    expect(raw && !raw.value().semantic_complete && count_issue(raw.value(), MapIssue::main_group_absent) == 1,
           "raw-only map is not semantic completion");
    if (raw) {
        const auto resolved = resolve_map(raw.value(), fixture_catalog(), probe_over({}));
        expect(resolved.placements == 0 && resolved.crc_unique == 0, "a raw-only map contributes an honest zero, not a skipped denominator");
    }
}

// --- D6, D7 and the malformed envelope --------------------------------------
void framing() {
    using namespace eawr::assets;
    Fixture unknown; unknown.unknown_chunk = true; unknown.preview = true; unknown.environment_extra = false;
    unknown.extra_root = {std::byte{0x40}, std::byte{1}, std::byte{7}, std::byte{0x40}, std::byte{1}, std::byte{8}};
    auto result = load(unknown);
    expect(bool(result), "bounded unknowns load");
    if (result) {
        const auto& map = result.value();
        expect(map.chunks.size() == 2, "bounded unknown chunks are retained");
        const auto* chunk_notice = find_issue(map, MapIssue::unknown_chunk);
        expect(chunk_notice && chunk_notice->chunk_path == "1/39030" && chunk_notice->byte_size == 3,
               "an unknown chunk is named with its path and size");
        const auto* root_notice = find_issue(map, MapIssue::unknown_root_field);
        expect(count_issue(map, MapIssue::unknown_root_field) == 1 && root_notice && root_notice->field_id == 0x40,
               "an unknown root mini is named once per id");
        expect(map.root_fields.size() == 4, "every root mini is retained raw");
        expect(count_issue(map, MapIssue::preview_not_decoded) == 1, "the preview block is skipped with a notice");
        const auto* optional_notice = find_issue(map, MapIssue::optional_section_absent);
        expect(optional_notice && optional_notice->chunk_path == "1/256/8", "an absent 1/256/8 is named");
        expect(map.notices.size() == map.issues.size(), "every typed notice is mirrored in the generic list");
        expect(map.semantic_complete, "retained unknowns do not block an otherwise complete view");
    }

    const auto header_only = [](const std::vector<std::byte>& root) { return file_from(root, {}); };
    std::vector<std::byte> good_root; mini(good_root, 0, integer(0x0201)); mini(good_root, 1, integer(1));
    std::vector<std::byte> duplicate_version = good_root; mini(duplicate_version, 0, integer(0x0201));
    expect(!load_bytes(header_only(duplicate_version)), "a duplicate version mini is rejected");
    std::vector<std::byte> duplicate_kind = good_root; mini(duplicate_kind, 1, integer(2));
    expect(!load_bytes(header_only(duplicate_kind)), "a duplicate kind mini is rejected");

    std::vector<std::byte> short_header; u32(short_header, 0); u8(short_header, 4);
    auto short_result = load_bytes(short_header);
    expect(!short_result && short_result.error().code == diagnostic_codes::truncated, "a short root header is rejected");
    std::vector<std::byte> overflow; u32(overflow, 0); u32(overflow, 64); add(overflow, good_root);
    auto overflow_result = load_bytes(overflow);
    expect(!overflow_result && overflow_result.error().code == diagnostic_codes::bounds, "a root payload past the file end is rejected");
    std::vector<std::byte> short_mini = good_root; u8(short_mini, 9);
    expect(!load_bytes(header_only(short_mini)), "a short root mini header is rejected");
    std::vector<std::byte> crossing = good_root; u8(crossing, 9); u8(crossing, 8); u8(crossing, 0);
    auto crossing_result = load_bytes(header_only(crossing));
    expect(!crossing_result && crossing_result.error().code == diagnostic_codes::bounds, "a mini crossing the root end is rejected");
    std::vector<std::byte> bad_version; mini(bad_version, 0, integer(0x0200)); mini(bad_version, 1, integer(1));
    auto version_result = load_bytes(header_only(bad_version));
    expect(!version_result && version_result.error().code == diagnostic_codes::unsupported, "an unsupported version is rejected");
    std::vector<std::byte> kind_three; mini(kind_three, 0, integer(0x0201)); mini(kind_three, 1, integer(3));
    auto kind_result = load_bytes(header_only(kind_three));
    expect(!kind_result && kind_result.error().code == diagnostic_codes::unsupported, "kind 3 is rejected");
    for (const std::uint8_t field : {std::uint8_t{8}, std::uint8_t{9}, std::uint8_t{10}}) {
        std::vector<std::byte> odd = good_root; mini(odd, field, {std::byte{'a'}, std::byte{0}, std::byte{0}});
        expect(!load_bytes(header_only(odd)), "an odd-length UTF-16 root field is rejected");
        std::vector<std::byte> unterminated = good_root; mini(unterminated, field, {std::byte{'a'}, std::byte{0}});
        expect(!load_bytes(header_only(unterminated)), "an unterminated UTF-16 root field is rejected");
        std::vector<std::byte> lone = good_root; mini(lone, field, {std::byte{0x00}, std::byte{0xD8}, std::byte{0}, std::byte{0}});
        expect(!load_bytes(header_only(lone)), "an unpaired UTF-16 surrogate is rejected");
    }
    std::vector<std::byte> nested = chunk(7, {});
    for (int depth = 0; depth < 258; ++depth) nested = chunk(7, nested, true);
    auto nested_result = load_bytes(file_from(good_root, nested));
    expect(!nested_result && nested_result.error().code == diagnostic_codes::limit, "excessive nesting is rejected");
    std::vector<std::byte> trailing = ted(Fixture{}); u32(trailing, 5); u8(trailing, 0);
    auto trailing_result = load_bytes(trailing);
    expect(!trailing_result && trailing_result.error().code == diagnostic_codes::truncated, "a trailing partial chunk header is rejected");
    auto truncated = ted(Fixture{}); truncated.pop_back();
    expect(!load_bytes(truncated), "truncated TED is rejected");
    std::vector<std::byte> oversized; u32(oversized, 0); u32(oversized, 0); u32(oversized, 1); u32(oversized, 0x7fffffffU);
    auto oversized_result = load_bytes(oversized);
    expect(!oversized_result && oversized_result.error().code == diagnostic_codes::bounds, "oversized declared chunk is rejected before allocation");
}

// --- Version variants and M1, M2, M3 root/body views ------------------------
void header_views() {
    using namespace eawr::assets;
    {
        std::vector<std::byte> root;
        mini(root, 0, integer(0x0201)); mini(root, 1, integer(2));
        mini(root, 2, integer(9)); mini(root, 3, integer(5));
        mini(root, 5, integer(2)); mini(root, 6, integer(7));
        mini(root, 10, wide(u"CUSTOM_MODE"));
        mini(root, 11, {std::byte{1}}); mini(root, 18, {std::byte{1}});
        std::vector<std::byte> points;
        u32(points, 3);
        for (int i = 0; i < 3; ++i) { f32(points, static_cast<float>(i * 20)); f32(points, -25.0F); }
        const auto read = [&](const std::vector<std::byte>& payload, bool group = false) {
            return load_bytes(file_from(root, chunk(3, payload, group)));
        };
        auto authored = read(points);
        expect(authored && authored.value().lobby.capacity == 9U && authored.value().lobby.levels == 5U
            && authored.value().lobby.owner == 2U && authored.value().lobby.terrain == 7U
            && authored.value().lobby.custom == true && authored.value().lobby.new_markers == true
            && authored.value().lobby.game_types == "CUSTOM_MODE", "WSS-05: lobby header fields decode independently");
        expect(authored && authored.value().lobby.start_positions && authored.value().lobby.start_positions->size() == 3
            && authored.value().lobby.start_positions->at(2).x == 40.0F
            && authored.value().lobby.start_positions->at(2).y == -25.0F, "nine players and three source XY starts remain distinct");
        auto invalid = points; invalid.pop_back();
        expect(read(invalid) && !read(invalid).value().lobby.start_positions, "short start array has no semantic value");
        invalid = points; u8(invalid, 0);
        expect(read(invalid) && !read(invalid).value().lobby.start_positions, "overlong start array has no semantic value");
        invalid.clear(); u32(invalid, 0xFFFFFFFFU);
        expect(read(invalid) && !read(invalid).value().lobby.start_positions, "hostile start count is bounded before allocation");
        invalid.clear(); u32(invalid, 1); f32(invalid, std::numeric_limits<float>::infinity()); f32(invalid, 0.0F);
        expect(read(invalid) && !read(invalid).value().lobby.start_positions, "nonfinite start array has no semantic value");
        auto duplicate = load_bytes(file_from(root, chunk(3, points)));
        auto body = chunk(3, points); add(body, chunk(3, points));
        duplicate = load_bytes(file_from(root, body));
        expect(duplicate && !duplicate.value().lobby.start_positions, "duplicate start chunks select no winner");
        mini(root, 2, integer(4)); mini(root, 11, {std::byte{0}});
        duplicate = read(points);
        expect(duplicate && !duplicate.value().lobby.capacity && !duplicate.value().lobby.custom,
            "duplicate lobby fields select no winner");
    }
    auto old_header = load(Fixture{});
    expect(old_header && !old_header.value().context_name && !old_header.value().declared_extents,
           "an old header without 0x09/0x10/0x11 stays valid and invents nothing");
    Fixture newer; newer.new_header = true;
    auto new_header = load(newer);
    expect(bool(new_header), "a new header with 0x0B before 0x08 is accepted");
    if (new_header) {
        const auto& map = new_header.value();
        expect(map.context_name == std::string("Fixture_World"), "0x09 decodes as context_name");
        expect(map.declared_extents && map.declared_extents->first_field_id == 0x10 && map.declared_extents->first == 4000.0F
                   && map.declared_extents->second_field_id == 0x11 && map.declared_extents->second == 3000.0F,
               "the declared extent pair keeps its field ids and values");
        if (map.declared_extents) expect(map.declared_extents->first_byte_offset < map.declared_extents->second_byte_offset,
                                         "declared extents keep ordered mini header offsets");
        expect(map.issues.empty(), "known newer-header minis produce no notice");
    }
    Fixture empty_context; empty_context.extra_root = wide(u"");
    empty_context.extra_root.insert(empty_context.extra_root.begin(), {std::byte{9}, std::byte{2}});
    auto empty = load(empty_context);
    expect(empty && empty.value().context_name == std::string(), "an empty context name is valid");
    Fixture half; mini(half.extra_root, 0x10, real(1.0F));
    auto half_result = load(half);
    expect(half_result && !half_result.value().declared_extents && count_issue(half_result.value(), MapIssue::declared_extents_invalid) == 1,
           "half an extent pair is named, not completed");

    auto space = load(space_fixture());
    expect(bool(space), "space fixture loads");
    if (space) {
        const auto& volumes = space.value().volumes;
        expect(volumes.size() == 2, "1/259 vector pairs decode in mini order");
        if (volumes.size() == 2) {
            expect(volumes[0].min_field_id == 0 && volumes[0].max_field_id == 1 && volumes[0].minimum.z == -10.0F && volumes[0].maximum.y == 60.0F,
                   "the first volume keeps its field ids and uncentred values");
            expect(volumes[1].min_field_id == 2 && volumes[1].max_field_id == 3 && volumes[1].minimum.x == -5.0F && volumes[1].maximum.x == 5.0F,
                   "the second volume keeps its field ids");
            expect(volumes[0].min_byte_offset == space.value().volume_fields[0].byte_offset
                       && volumes[0].max_byte_offset == space.value().volume_fields[1].byte_offset
                       && volumes[1].min_byte_offset == space.value().volume_fields[2].byte_offset
                       && volumes[1].max_byte_offset == space.value().volume_fields[3].byte_offset,
                   "each decoded volume cites its retained mini headers");
        }
        expect(space.value().water_textures.empty() && space.value().water_records.empty(), "a space map has no water");
    }
    Fixture waves; waves.waves = true;
    auto wave_result = load(waves);
    expect(bool(wave_result), "a nested wave group loads");
    if (wave_result) {
        const auto& records = wave_result.value().water_records;
        expect(records.size() == 3 && records[1].chunk_path == "1/257/9/256/257" && records[1].fields.size() == 2
                   && records[2].chunk_path == "1/257/9/256/257",
               "every mini-stream wave leaf is a lossless water record with its path");
        const auto* invalid = find_issue(wave_result.value(), MapIssue::water_stream_invalid);
        expect(count_issue(wave_result.value(), MapIssue::water_stream_invalid) == 1 && invalid && invalid->chunk_path == "1/257/9/256/258",
               "a wave leaf that is not a mini stream is named once per path and kept raw");
        expect(wave_result.value().semantic_complete, "wave retention does not affect terrain completion");
    }
    Fixture older = space_fixture(); older.old_volumes = true;
    auto older_result = load(older);
    expect(bool(older_result), "the older 1/259 form loads");
    if (older_result) {
        const auto& volumes = older_result.value().volumes;
        // 3 -> 4 is adjacent and consecutive but does not bound (9 > 1), so it
        // is not a volume; 5 -> 6 is.  4 stays a standalone raw vector.
        expect(volumes.size() == 1 && volumes[0].min_field_id == 5 && volumes[0].max_field_id == 6
                   && volumes[0].maximum.x == 8.0F,
               "only an adjacent, consecutive, bounding vector pair is a volume");
        expect(older_result.value().volume_fields.size() == 6, "every 1/259 mini stays raw by field id");
        expect(count_issue(older_result.value(), MapIssue::volume_stream_invalid) == 0,
               "standalone vectors are retained, not reported as malformed");
    }
    Fixture no_volumes = space_fixture(); no_volumes.volumes = false;
    auto without = load(no_volumes);
    expect(without && without.value().volumes.empty() && count_issue(without.value(), MapIssue::optional_section_absent) == 1,
           "an absent 1/259 is named");

    Fixture mixed = space_fixture();
    mixed.volume_stream.clear();
    mini(mixed.volume_stream, 7, vector3(1, 2, 3));
    mini(mixed.volume_stream, 8, vector3(4, 5, 6));
    mini(mixed.volume_stream, 7, vector3(9, 9, 9));
    mini(mixed.volume_stream, 8, vector3(10, 10, 10));
    mini(mixed.volume_stream, 9, vector3(0, 0, 0));
    mini(mixed.volume_stream, 11, vector3(1, 1, 1));
    mini(mixed.volume_stream, 12, vector3(std::numeric_limits<float>::infinity(), 2, 3));
    mini(mixed.volume_stream, 13, vector3(4, 5, 6));
    mini(mixed.volume_stream, 14, real(1));
    mini(mixed.volume_stream, 15, vector3(4, 5, 6));
    auto mixed_result = load(mixed);
    expect(mixed_result && mixed_result.value().volume_fields.size() == 10
               && mixed_result.value().volumes.size() == 2
               && mixed_result.value().volumes[0].min_field_id == 7
               && mixed_result.value().volumes[1].min_field_id == 7,
           "repeated pairs retain order; nonconsecutive, nonfinite and wrong-sized candidates stay unpaired");

    auto land = load(Fixture{});
    if (land) {
        const auto& map = land.value();
        expect(map.water_textures.size() == 2 && map.water_textures[0].field_id == 0x1D && map.water_textures[0].logical_name == "Fixture_Water"
                   && map.water_textures[1].field_id == 0x1E && map.water_textures[1].logical_name == "Fixture_Water_Bump",
               "water textures are named texture references with their field ids");
        expect(map.water_records.size() == 1 && map.water_records[0].fields.size() == 3,
               "the water record holds the non-grid header minis only");
        expect(map.water_records.size() == 1 && map.water_records[0].chunk_path == "1/257/0", "the water header record names its path");
        bool grid_free = true;
        if (!map.water_records.empty()) for (const auto& field : map.water_records[0].fields) grid_free = grid_free && field.id != 0 && field.id != 1 && field.id != 4 && field.id != 5;
        expect(grid_free, "grid minis are not reported as water data");
    }
}

// --- D8: an oversize VFS record is refused from its stat size ---------------
void oversize_record() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() / ("eawr-map-" + std::to_string(stamp));
    std::error_code error;
    std::filesystem::create_directories(root / "Art" / "Maps", error);
    const auto path = root / "Art" / "Maps" / "Huge.ted";
    { std::ofstream touch(path, std::ios::binary); }
    // Extending the file does not write its bytes; the loader must not read them.
    std::filesystem::resize_file(path, 512ULL * 1024ULL * 1024ULL + 1ULL, error);
    if (!error) {
        const std::array mounts{eawr::vfs::MountSpec{"fixture", root, "data", {}}};
        if (auto mounted = eawr::vfs::Vfs::mount(mounts)) {
            auto result = eawr::assets::load_map(mounted.value(), "data/art/maps/huge.ted");
            expect(!result && result.error().code == eawr::assets::diagnostic_codes::limit
                       && result.error().message.find("before read") != std::string::npos,
                   "an oversize TED record is rejected from its stat size before open");
        } else expect(false, "synthetic oversize VFS mounts");
    } else {
        std::cout << "note: oversize VFS check skipped; the temporary file could not be extended\n";
    }
    std::filesystem::remove_all(root, error);
}
} // namespace eawr::tests::map_contracts
