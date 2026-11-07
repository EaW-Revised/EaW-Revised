#include <utility>
#include "../support/check.hpp"
#include "eawr/vfs/vfs.hpp"
#include "map_test_support.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/assets/ui_map_markers.hpp"
#include "../support/temp_dir.hpp"
#include "../support/env.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::tests::map_contracts {
eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(const bool condition, const char* message) {
    local_checker.check(condition, message, std::cerr);
}
void u8(std::vector<std::byte>& out, const std::uint8_t value) { out.push_back(static_cast<std::byte>(value)); }
void u16(std::vector<std::byte>& out, const std::uint16_t value) { u8(out, value & 255U); u8(out, (value >> 8U) & 255U); }
void u32(std::vector<std::byte>& out, const std::uint32_t value) { for (unsigned shift = 0; shift < 32; shift += 8) u8(out, (value >> shift) & 255U); }
void f32(std::vector<std::byte>& out, const float value) { u32(out, std::bit_cast<std::uint32_t>(value)); }
void add(std::vector<std::byte>& out, const std::vector<std::byte>& value) { out.insert(out.end(), value.begin(), value.end()); }
std::vector<std::byte> chunk(const std::uint32_t id, const std::vector<std::byte>& payload, const bool group) {
    std::vector<std::byte> out; u32(out, id); u32(out, static_cast<std::uint32_t>(payload.size()) | (group ? 0x80000000U : 0)); add(out, payload); return out;
}
void mini(std::vector<std::byte>& out, const std::uint8_t id, const std::vector<std::byte>& payload) { u8(out, id); u8(out, static_cast<std::uint8_t>(payload.size())); add(out, payload); }
std::vector<std::byte> integer(const std::uint32_t value) { std::vector<std::byte> out; u32(out, value); return out; }
std::vector<std::byte> real(const float value) { std::vector<std::byte> out; f32(out, value); return out; }
std::vector<std::byte> vector3(const float x, const float y, const float z) { std::vector<std::byte> out; f32(out, x); f32(out, y); f32(out, z); return out; }
std::vector<std::byte> narrow(const std::string_view text) {
    std::vector<std::byte> out; for (const char c : text) u8(out, static_cast<std::uint8_t>(c)); u8(out, 0); return out;
}
std::vector<std::byte> wide(const std::u16string_view text) {
    std::vector<std::byte> out; for (const char16_t c : text) u16(out, static_cast<std::uint16_t>(c)); u16(out, 0); return out;
}
std::vector<std::byte> file_from(const std::vector<std::byte>& root, const std::vector<std::byte>& body) {
    std::vector<std::byte> file; u32(file, 0); u32(file, static_cast<std::uint32_t>(root.size())); add(file, root); add(file, body); return file;
}

std::vector<std::byte> placement_body(const Record& record, const std::uint32_t serial) {
    std::vector<std::byte> placed;
    const auto crc = [&] { mini(placed, 1, integer(eawr::assets::object_type_crc(record.type))); };
    const auto position = [&] { mini(placed, 4, vector3(10, 20, 30)); };
    const auto orientation = [&] {
        if (record.orientation) mini(placed, 5, vector3((*record.orientation)[0], (*record.orientation)[1], (*record.orientation)[2]));
    };
    if (record.shuffled) {
        mini(placed, 21, vector3(4, 5, 6)); orientation(); mini(placed, 2, integer(3)); position();
        mini(placed, 18, vector3(1, 2, 3)); crc(); mini(placed, 0, integer(serial));
    } else {
        mini(placed, 0, integer(serial)); crc(); position(); orientation();
        // Cached minis 18 and 21 deliberately disagree with 5 and 4.
        mini(placed, 18, vector3(1, 2, 3)); mini(placed, 21, vector3(4, 5, 6));
    }
    return placed;
}

std::vector<std::byte> object_record(const std::vector<std::byte>& body) {
    std::vector<std::byte> data; add(data, chunk(1200, body));
    std::vector<std::byte> object; add(object, chunk(1113, data, true));
    return chunk(1100, object, true);
}

std::vector<std::byte> ted(const Fixture& fixture) {
    std::vector<std::byte> root;
    mini(root, 0, integer(0x0201)); mini(root, 1, integer(fixture.kind));
    if (fixture.new_header) {
        mini(root, 0x0B, {std::byte{1}});
        mini(root, 0x08, wide(u""));
        mini(root, 0x09, wide(u"Fixture_World"));
        mini(root, 0x10, real(4000.0F)); mini(root, 0x11, real(3000.0F));
        mini(root, 0x12, {std::byte{0}});
    }
    add(root, fixture.extra_root);
    if (!fixture.include_main) return file_from(root, {});
    std::vector<std::byte> main;
    {
        std::vector<std::byte> environments; add(environments, chunk(5, {}));
        if (fixture.environment_records.empty()) {
            std::vector<std::byte> environment; mini(environment, 0x14, narrow("Fixture_Env")); mini(environment, 0x19, narrow("Fixture_Sky"));
            add(environments, chunk(6, environment));
        } else {
            for (const auto& environment : fixture.environment_records) add(environments, chunk(6, environment));
        }
        std::vector<std::byte> bundle; add(bundle, chunk(4, environments, true));
        if (fixture.environment_extra) add(bundle, chunk(8, vector3(0, 0, 0)));
        add(main, chunk(256, bundle, true));
    }
    if (fixture.land_groups) {
        std::vector<std::byte> header; mini(header, 0, integer(fixture.width)); mini(header, 1, integer(fixture.height));
        mini(header, 4, integer(fixture.cell_count)); mini(header, 5, integer(static_cast<std::uint32_t>(fixture.materials)));
        if (fixture.water) { mini(header, 0x1D, narrow("Fixture_Water")); mini(header, 0x1E, narrow("Fixture_Water_Bump")); mini(header, 0x20, real(0.5F)); }
        std::vector<std::byte> material; mini(material, 12, narrow("grass"));
        std::vector<std::byte> materials; for (std::size_t index = 0; index < fixture.materials; ++index) add(materials, chunk(3, material));
        std::vector<std::byte> plane;
        for (std::size_t index = 0; index < fixture.heights.size(); ++index) {
            u16(plane, std::bit_cast<std::uint16_t>(fixture.heights[index])); u8(plane, fixture.slots[index]); u8(plane, fixture.intensities[index]);
        }
        if (fixture.plane_delta < 0) plane.pop_back();
        if (fixture.plane_delta > 0) u8(plane, 0);
        std::vector<std::byte> terrain; add(terrain, chunk(0, header)); add(terrain, chunk(2, materials, true));
        add(terrain, chunk(fixture.legacy_plane ? 1U : 5U, plane));
        if (fixture.waves) {
            std::vector<std::byte> points; mini(points, 0, vector3(1, 2, 3)); mini(points, 1, narrow("Fixture_Wave"));
            std::vector<std::byte> wave; add(wave, chunk(257, points)); add(wave, chunk(258, {std::byte{7}}));
            std::vector<std::byte> group; add(group, chunk(256, wave, true)); add(group, chunk(256, wave, true));
            add(terrain, chunk(9, group, true));
        }
        add(main, chunk(257, terrain, true));
        add(main, chunk(266, plane));
        if (fixture.patch_group) add(main, chunk(267, {}, true));
    }
    std::vector<std::byte> objects; std::uint32_t serial = 77;
    for (const auto& record : fixture.records) add(objects, object_record(placement_body(record, serial++)));
    for (const auto& raw : fixture.raw_records) add(objects, raw);
    std::vector<std::byte> graph; add(graph, chunk(1, objects, true)); add(main, chunk(258, graph, true));
    if (!fixture.volume_stream.empty()) {
        add(main, chunk(259, fixture.volume_stream));
    } else if (fixture.old_volumes) {
        std::vector<std::byte> volumes;
        mini(volumes, 0, integer(1)); mini(volumes, 3, vector3(9, 9, 9)); mini(volumes, 4, vector3(1, 1, 1));
        mini(volumes, 5, vector3(0, 0, 0)); mini(volumes, 6, vector3(8, 8, 8)); mini(volumes, 7, integer(2));
        add(main, chunk(259, volumes));
    } else if (fixture.volumes) {
        std::vector<std::byte> volumes;
        mini(volumes, 0, vector3(0, 0, -10)); mini(volumes, 1, vector3(40, 60, 90));
        mini(volumes, 2, vector3(-5, -5, -5)); mini(volumes, 3, vector3(5, 5, 5));
        add(main, chunk(259, volumes));
    }
    if (fixture.unknown_chunk) add(main, chunk(0x9876U, {std::byte{1}, std::byte{2}, std::byte{3}}));
    std::vector<std::byte> body = chunk(1, main, true);
    if (fixture.preview) add(body, chunk(19, {std::byte{9}, std::byte{9}}));
    return file_from(root, body);
}

Fixture space_fixture() {
    Fixture fixture; fixture.kind = 2; fixture.land_groups = false; return fixture;
}

eawr::assets::ObjectTypeRef type_ref(const std::string& id, const char* model) {
    eawr::assets::ObjectTypeRef entry;
    entry.logical_name = id;
    entry.source = {id + ".xml", id, "test", 1, 1};
    if (model != nullptr) entry.model_name = std::string(model);
    return entry;
}

// An original synthetic VFS stand-in: the probe answers only for the paths this
// test declares, so no installed corpus is consulted.
eawr::assets::AssetProbe probe_over(std::set<std::string> present) {
    return [holdings = std::move(present)](const std::string_view path) {
        return holdings.count(std::string(path)) != 0;
    };
}

eawr::assets::Source source(const std::size_t size) { return {"data/art/maps/fixture.ted", "fixture", "test", eawr::vfs::AssetOrigin::loose, size}; }

const eawr::assets::ObjectTypeCatalog& fixture_catalog() {
    static const eawr::assets::ObjectTypeCatalog catalog{{type_ref("Fixture_Prop", "fixture_prop.alo")}};
    return catalog;
}

eawr::core::Result<eawr::assets::Map> load(const Fixture& fixture,
                                           const eawr::assets::ObjectTypeCatalog& catalog) {
    const auto bytes = ted(fixture);
    return eawr::assets::load_map(bytes, source(bytes.size()), catalog);
}

eawr::core::Result<eawr::assets::Map> load_bytes(const std::vector<std::byte>& bytes) {
    return eawr::assets::load_map(bytes, source(bytes.size()), fixture_catalog());
}

std::size_t count_issue(const eawr::assets::Map& map, const eawr::assets::MapIssue kind) {
    return static_cast<std::size_t>(std::count_if(map.issues.begin(), map.issues.end(),
        [&](const eawr::assets::MapNotice& notice) { return notice.issue == kind; }));
}

const eawr::assets::MapNotice* find_issue(const eawr::assets::Map& map, const eawr::assets::MapIssue kind) {
    const auto found = std::find_if(map.issues.begin(), map.issues.end(),
        [&](const eawr::assets::MapNotice& notice) { return notice.issue == kind; });
    return found == map.issues.end() ? nullptr : &*found;
}

// The raw 1/257/5 (or /1) leaf, found in the retained lossless tree.
const eawr::assets::RawChunk* raw_terrain_plane(const eawr::assets::Map& map, const std::uint32_t leaf) {
    for (const auto& top : map.chunks) {
        if (top.id != 1) continue;
        for (const auto& group : top.children) {
            if (group.id != 257) continue;
            for (const auto& child : group.children) if (child.id == leaf) return &child;
        }
    }
    return nullptr;
}

bool near(const float left, const float right) { return std::fabs(left - right) <= 1.0e-6F; }

} // namespace eawr::tests::map_contracts

using namespace eawr::tests::map_contracts;

void ui_marker_contracts() {
    using namespace eawr;
    const auto root = std::filesystem::temp_directory_path() /
        ("eawr-ui-markers-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "XML");
    const auto write = [&](const char* name, const std::string& text) { std::ofstream out(root / "XML" / name); out << text; };
    write("GameObjectFiles.xml", "<Game_Object_Files><File>objects.xml</File></Game_Object_Files>");
    write("objects.xml", "<Objects>"
        "<Marker Name=\"IN BASE SPECIAL STRUCTURE POSITION\"><Include_In_UI_Map_Header>Yes</Include_In_UI_Map_Header></Marker>"
        "<Marker Name=\"OUT BASE SPECIAL STRUCTURE POSITION\"><Include_In_UI_Map_Header>Yes</Include_In_UI_Map_Header></Marker>"
        "<Marker Name=\"DEFENDING FORCES POSITION\"><Include_In_UI_Map_Header>Yes</Include_In_UI_Map_Header></Marker>"
        "<Marker Name=\"Specific\"><Include_In_UI_Map_Header>Yes</Include_In_UI_Map_Header><Marker_For_Specific_Object_Type>Fixture</Marker_For_Specific_Object_Type></Marker>"
        "<Marker Name=\"REINFORCEMENT_POINT_A\"><Include_In_UI_Map_Header>Yes</Include_In_UI_Map_Header></Marker>"
        "<Marker Name=\"GARRISON_BUNKER_CAPTURABLE\"><Include_In_UI_Map_Header>Yes</Include_In_UI_Map_Header></Marker>"
        "<Marker Name=\"SKIRMISH_BUILD_PAD\"><Include_In_UI_Map_Header>Yes</Include_In_UI_Map_Header></Marker>"
        "<Marker Name=\"Hidden\"><Include_In_UI_Map_Header>No</Include_In_UI_Map_Header></Marker></Objects>");
    const std::array mounts{vfs::MountSpec{"base", root, "data", {}}};
    const auto mounted = vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "marker fixture mounts");
    if (!mounted) { testing::remove_temp_directory(root); return; }
    const auto catalog = data::load_catalog(mounted.value(), data::Profile::foc);
    expect(static_cast<bool>(catalog), "marker fixture catalog loads");
    if (!catalog) { testing::remove_temp_directory(root); return; }
    Fixture fixture; fixture.new_header = true; fixture.records.clear();
    const auto types = assets::object_type_catalog(catalog.value().catalog);
    const auto add = [&](const std::string& name, float x, std::uint32_t record) {
        while (fixture.raw_records.size() < record) fixture.raw_records.push_back(object_record({}));
        std::vector<std::byte> body;
        mini(body, 1, integer(assets::object_type_crc(name)));
        mini(body, 4, vector3(x, 0, 0));
        fixture.raw_records.push_back(object_record(body));
    };
    add("IN BASE SPECIAL STRUCTURE POSITION", -20, 1);
    add("IN BASE SPECIAL STRUCTURE POSITION", 0, 2);
    add("IN BASE SPECIAL STRUCTURE POSITION", 20, 3);
    add("OUT BASE SPECIAL STRUCTURE POSITION", 10, 4);
    add("Specific", 0, 5);
    for (std::uint32_t i = 0; i < 12; ++i) add("DEFENDING FORCES POSITION", 0, 10 + i);
    add("REINFORCEMENT_POINT_A", 0, 30); add("GARRISON_BUNKER_CAPTURABLE", 0, 31);
    add("SKIRMISH_BUILD_PAD", 0, 32); add("Hidden", 0, 33);
    auto decoded = load(fixture, types);
    expect(static_cast<bool>(decoded), "synthetic marker TED loads");
    if (!decoded) { testing::remove_temp_directory(root); return; }
    auto map = std::move(decoded).value();
    expect(assets::ui_map_chunk_count(map) > 0, "parse work counts nested chunks deterministically");
    const auto markers = assets::ui_map_markers(map, catalog.value().catalog);
    expect(markers.size() == 18 && markers[0].record == 2 && markers[3].record == 5 && markers[4].record == 4,
        "marker order, eligibility and unit cap");
    if (markers.size() != 18) { testing::remove_temp_directory(root); return; }
    expect(markers[0].x == 0.5F && markers[0].y == 0.5F && markers[3].specific_types == "Fixture",
        "marker normalisation and specific type");
    expect(markers[15].kind == assets::UiMarkerKind::reinforcement && markers[16].kind == assets::UiMarkerKind::bunker
        && markers[17].kind == assets::UiMarkerKind::build_pad, "display-only groups retained");
    for (std::uint32_t i = 0; i < 90; ++i) add("OUT BASE SPECIAL STRUCTURE POSITION", 0, 100 + i);
    decoded = load(fixture, types);
    expect(static_cast<bool>(decoded), "capped synthetic TED loads");
    if (!decoded) { testing::remove_temp_directory(root); return; }
    map = std::move(decoded).value();
    const auto capped = assets::ui_map_markers(map, catalog.value().catalog);
    expect(std::count_if(capped.begin(), capped.end(), [](const auto& marker) {
        return marker.kind == assets::UiMarkerKind::in_base || marker.kind == assets::UiMarkerKind::specific
            || marker.kind == assets::UiMarkerKind::out_base;
    }) == 80, "build pads cap at 80");
    map.declared_extents.reset();
    expect(assets::ui_map_markers(map, catalog.value().catalog).empty(), "missing map size yields no guessed markers");
    testing::remove_temp_directory(root);
}

void installed_ui_marker_contracts() {
    using namespace eawr;
    const auto root = testing::game_root();
    if (!root) { std::cout << "installed marker assertions skipped: no game data\n"; return; }
    std::vector<vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                     std::pair{std::string("base"), std::string("GameData")}}) {
        auto manifest = vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data");
        expect(static_cast<bool>(manifest), "installed marker fixture layer mounts");
        if (!manifest) return;
        mounts.push_back(std::move(manifest).value().mount);
    }
    auto fs = vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(fs), "installed marker fixture vfs mounts");
    if (!fs) return;
    auto catalog = data::load_catalog(fs.value(), data::Profile::foc);
    expect(static_cast<bool>(catalog), "installed marker fixture catalog loads");
    if (!catalog) return;
    const auto types = assets::object_type_catalog(catalog.value().catalog);
    auto map = assets::load_map(fs.value(), "data/art/maps/_land_planet_abregadorae_02.ted", types);
    expect(static_cast<bool>(map), "installed planet map loads");
    if (!map) return;
    const auto markers = assets::ui_map_markers(map.value(), catalog.value().catalog);
    const auto builds = std::count_if(markers.begin(), markers.end(), [](const auto& marker) {
        return marker.kind == assets::UiMarkerKind::in_base || marker.kind == assets::UiMarkerKind::out_base
            || marker.kind == assets::UiMarkerKind::specific;
    });
    const auto units = std::count_if(markers.begin(), markers.end(), [](const auto& marker) {
        return marker.kind == assets::UiMarkerKind::defender;
    });
    expect(builds > 0 && builds <= 80 && units > 0 && units <= 10, "installed marker class counts are populated and bounded");
    expect(assets::ui_map_chunk_count(map.value()) > 0, "installed map provides parse work count");
}

int main() {
    ui_marker_contracts();
    installed_ui_marker_contracts();
    terrain_values();
    terrain_failures();
    kind_structure();
    framing();
    header_views();
    placements();
    resolution();
    environment_reference_ledger();
    oversize_record();
    if (failures == 0) std::cout << "map tests passed\n";
    return failures == 0 ? 0 : 1;
}
