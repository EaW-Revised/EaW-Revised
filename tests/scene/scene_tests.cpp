#include "scene_test_support.hpp"

// P1-11 static scene builder contracts. Every value here is invented for this
// test: the TED bytes are assembled below, the XML objects are written to a
// temporary tree, and models are supplied in memory through AssetAccess.

#include "eawr/scene/scene.hpp"
#include "eawr/platform/sim_workers.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/sim/math/fixed.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace eawr::tests::scene_tests {

int failures{};

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

// --- TED assembly (same chunk/mini layout the map loader documents) --------

void u8(std::vector<std::byte>& out, const std::uint8_t value) { out.push_back(static_cast<std::byte>(value)); }
void u32(std::vector<std::byte>& out, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) u8(out, static_cast<std::uint8_t>((value >> shift) & 255U));
}
void f32(std::vector<std::byte>& out, const float value) { u32(out, std::bit_cast<std::uint32_t>(value)); }
void add(std::vector<std::byte>& out, const std::vector<std::byte>& value) { out.insert(out.end(), value.begin(), value.end()); }
std::vector<std::byte> chunk(const std::uint32_t id, const std::vector<std::byte>& payload, const bool group = false) {
    std::vector<std::byte> out;
    u32(out, id);
    u32(out, static_cast<std::uint32_t>(payload.size()) | (group ? 0x80000000U : 0U));
    add(out, payload);
    return out;
}
void mini(std::vector<std::byte>& out, const std::uint8_t id, const std::vector<std::byte>& payload) {
    u8(out, id);
    u8(out, static_cast<std::uint8_t>(payload.size()));
    add(out, payload);
}
std::vector<std::byte> integer(const std::uint32_t value) { std::vector<std::byte> out; u32(out, value); return out; }
std::vector<std::byte> vector3(const float x, const float y, const float z) {
    std::vector<std::byte> out; f32(out, x); f32(out, y); f32(out, z); return out;
}

std::vector<std::byte> ted(const std::vector<Record>& records) {
    std::vector<std::byte> root;
    mini(root, 0, integer(0x0201));
    mini(root, 1, integer(1));
    std::vector<std::byte> file;
    u32(file, 0);
    u32(file, static_cast<std::uint32_t>(root.size()));
    add(file, root);
    std::vector<std::byte> main;
    add(main, chunk(256, {}, true));
    std::vector<std::byte> header;
    mini(header, 0, integer(2)); mini(header, 1, integer(2)); mini(header, 4, integer(4)); mini(header, 5, integer(1));
    std::vector<std::byte> material;
    mini(material, 12, {std::byte{'g'}, std::byte{0}});
    std::vector<std::byte> materials;
    add(materials, chunk(3, material));
    std::vector<std::byte> plane;
    for (int index = 0; index < 4; ++index) { u8(plane, 0); u8(plane, 0); u8(plane, 0); u8(plane, 255); }
    std::vector<std::byte> terrain;
    add(terrain, chunk(0, header)); add(terrain, chunk(2, materials, true)); add(terrain, chunk(5, plane));
    add(main, chunk(257, terrain, true));
    add(main, chunk(266, plane));
    add(main, chunk(267, {}, true));
    std::vector<std::byte> objects;
    std::uint32_t serial = 100;
    for (const Record& record : records) {
        std::vector<std::byte> placed;
        mini(placed, 0, integer(serial++));
        if (!record.type.empty()) mini(placed, 1, integer(eawr::assets::object_type_crc(record.type)));
        if (record.owner) mini(placed, 2, integer(static_cast<std::uint32_t>(*record.owner)));
        if (record.position) mini(placed, 4, vector3((*record.position)[0], (*record.position)[1], (*record.position)[2]));
        if (!record.orientation_absent) {
            mini(placed, 5, vector3(record.orientation[0], record.orientation[1], record.orientation[2]));
        }
        std::vector<std::byte> data; add(data, chunk(1200, placed));
        std::vector<std::byte> object; add(object, chunk(1113, data, true));
        add(objects, chunk(1100, object, true));
    }
    std::vector<std::byte> graph; add(graph, chunk(1, objects, true));
    add(main, chunk(258, graph, true));
    add(file, chunk(1, main, true));
    return file;
}

// --- XML catalog in a temporary tree ---------------------------------------

void write(const std::filesystem::path& path, const std::string_view value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
}

void write_catalog(const std::filesystem::path& root) {
    write(root / "XML" / "GameObjectFiles.xml",
          "<Game_Object_Files><File>eawr_scene_objects.xml</File></Game_Object_Files>");
    write(root / "XML" / "HardpointDataFiles.xml", "<Hard_Point_Files></Hard_Point_Files>");
    write(root / "XML" / "FactionFiles.xml", "<Faction_Files><File>eawr_scene_factions.xml</File></Faction_Files>");
    // Player index i names the i-th faction loaded.
    write(root / "XML" / "eawr_scene_factions.xml", R"xml(<Factions>
<Faction Name="EAWR_RED">
  <Color> 200, 10, 20, 255 </Color>
</Faction>
<Faction Name="EAWR_BLUE">
  <Color>10,20,200</Color>
</Faction>
<Faction Name="EAWR_BARE">
</Faction>
<Faction Name="EAWR_BROKEN">
  <Color>300, 0, 0</Color>
</Faction>
</Factions>)xml");
    write(root / "XML" / "CampaignFiles.xml", "<Campaign_Files></Campaign_Files>");
    write(root / "XML" / "SFXEventFiles.xml", "<SFXEvent_Files></SFXEvent_Files>");
    write(root / "XML" / "eawr_scene_objects.xml", R"xml(<Objects>
<GroundBuildable Name="EAWR_SCENE_PROP">
  <Land_Model_Name>Eawr_Scene_Prop.alo</Land_Model_Name>
  <Scale_Factor>1.5</Scale_Factor>
  <Behavior>REVEAL, CAPTURE_POINT</Behavior>
</GroundBuildable>
<GroundBuildable Name="EAWR_SCENE_DERIVED">
  <Variant_Of_Existing_Type>EAWR_SCENE_PROP</Variant_Of_Existing_Type>
</GroundBuildable>
<GroundBuildable Name="EAWR_SCENE_PLAIN">
  <Model_Name>eawr_scene_plain.alo</Model_Name>
</GroundBuildable>
<GroundBuildable Name="EAWR_SCENE_NO_MODEL">
  <Scale_Factor>2</Scale_Factor>
</GroundBuildable>
<GroundBuildable Name="EAWR_SCENE_LOST_MODEL">
  <Land_Model_Name>eawr_scene_lost.alo</Land_Model_Name>
</GroundBuildable>
<GroundBuildable Name="EAWR_SCENE_ODD_SHADER">
  <Land_Model_Name>eawr_scene_odd.alo</Land_Model_Name>
</GroundBuildable>
<GroundBuildable Name="EAWR_SCENE_BAD_SCALE">
  <Land_Model_Name>eawr_scene_plain.alo</Land_Model_Name>
  <Scale_Factor>-1</Scale_Factor>
</GroundBuildable>
<SpaceProp Name="EAWR_SCENE_BACKGROUND">
  <Model_Name>eawr_scene_plain.alo</Model_Name>
  <Layer_Z_Adjust>-2500</Layer_Z_Adjust>
</SpaceProp>
<SpaceProp Name="EAWR_SCENE_BAD_HEIGHT">
  <Model_Name>eawr_scene_plain.alo</Model_Name>
  <Layer_Z_Adjust>invalid</Layer_Z_Adjust>
</SpaceProp>
</Objects>)xml");
}

// --- In-memory assets ------------------------------------------------------

eawr::assets::Model model(const std::vector<std::string>& shaders, const std::string& texture,
                          const std::vector<std::string>& proxies) {
    eawr::assets::Model result;
    eawr::assets::Bone bone;
    bone.name = "Root";
    result.bones.push_back(bone);
    eawr::assets::Mesh mesh;
    mesh.name = "Body";
    mesh.visible = true;
    for (const std::string& shader : shaders) {
        eawr::assets::Submesh submesh;
        submesh.shader = shader;
        submesh.parameters.push_back({"BaseTexture", eawr::assets::ParameterKind::texture, texture});
        mesh.submeshes.push_back(submesh);
    }
    result.meshes.push_back(mesh);
    eawr::assets::Mesh hidden = mesh;
    hidden.name = "Collision";
    hidden.visible = false;
    hidden.submeshes.front().shader = "NeverDrawn.fx";
    result.meshes.push_back(hidden);
    for (const std::string& name : proxies) result.proxies.push_back({name, 0, true, false});
    return result;
}

bool has(const eawr::scene::Placement& placement, const Cause cause, const std::string_view detail) {
    return std::any_of(placement.issues.begin(), placement.issues.end(), [&](const auto& issue) {
        return issue.cause == cause && (detail.empty() || issue.detail == detail);
    });
}

} // namespace eawr::tests::scene_tests

using namespace eawr::tests::scene_tests;

int main() {
    conversion_contracts();
    scene_contracts();
    selector_contracts();
    static_mesh_state_contracts();
    uncaptured_capture_point_contracts();
    if (failures != 0) {
        std::cerr << failures << " scene contract(s) failed\n";
        return 1;
    }
    std::cout << "scene contracts passed\n";
    return 0;
}
