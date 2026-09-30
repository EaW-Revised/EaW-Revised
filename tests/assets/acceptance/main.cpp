#include "eawr/assets/assets.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures{};

void expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void u8(std::vector<std::byte>& out, const std::uint8_t value) { out.push_back(static_cast<std::byte>(value)); }
void u16(std::vector<std::byte>& out, const std::uint16_t value) { u8(out, value & 0xffU); u8(out, value >> 8U); }
void u32(std::vector<std::byte>& out, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) u8(out, static_cast<std::uint8_t>(value >> shift));
}
void f32(std::vector<std::byte>& out, const float value) { u32(out, std::bit_cast<std::uint32_t>(value)); }
void add(std::vector<std::byte>& out, const std::vector<std::byte>& value) { out.insert(out.end(), value.begin(), value.end()); }
void text(std::vector<std::byte>& out, const char* value) { while (*value) u8(out, static_cast<std::uint8_t>(*value++)); u8(out, 0); }
void mini(std::vector<std::byte>& out, const std::uint8_t type, const std::vector<std::byte>& value) {
    u8(out, type); u8(out, static_cast<std::uint8_t>(value.size())); add(out, value);
}
std::vector<std::byte> chunk(const std::uint32_t type, const std::vector<std::byte>& payload, const bool group = false) {
    std::vector<std::byte> out; u32(out, type); u32(out, static_cast<std::uint32_t>(payload.size()) | (group ? 0x80000000U : 0U)); add(out, payload); return out;
}
std::vector<std::byte> integer(const std::uint32_t value) { std::vector<std::byte> out; u32(out, value); return out; }

eawr::assets::Source source(const char* path, const std::size_t size) {
    return {path, "acceptance:synthetic", "acceptance", eawr::vfs::AssetOrigin::loose, size};
}

// These bytes are deliberately authored here from the public chunk grammar;
// they are not serialized by the production library.
std::vector<std::byte> model_fixture(const std::int32_t parent = -1) {
    std::vector<std::byte> count; u32(count, 1);
    std::vector<std::byte> bone_name; text(bone_name, "ROOT");
    std::vector<std::byte> bone_data; u32(bone_data, std::bit_cast<std::uint32_t>(parent)); u32(bone_data, 1);
    for (int i = 0; i < 12; ++i) f32(bone_data, (i == 0 || i == 4 || i == 8) ? 1.0F : 0.0F);
    std::vector<std::byte> bone; add(bone, chunk(0x203, bone_name)); add(bone, chunk(0x205, bone_data, false));
    std::vector<std::byte> skeleton; add(skeleton, chunk(0x201, count)); add(skeleton, chunk(0x202, bone, true));
    std::vector<std::byte> connection_header; mini(connection_header, 1, integer(0)); mini(connection_header, 4, integer(0));
    std::vector<std::byte> connections; add(connections, chunk(0x601, connection_header));
    std::vector<std::byte> result; add(result, chunk(0x200, skeleton, true)); add(result, chunk(0x600, connections, true)); return result;
}

std::vector<std::byte> animation_fixture() {
    std::vector<std::byte> info; mini(info, 1, integer(1)); std::vector<std::byte> fps; f32(fps, 30.0F); mini(info, 2, fps); mini(info, 3, integer(0));
    std::vector<std::byte> root; add(root, chunk(0x1001, info)); return chunk(0x1000, root, true);
}

std::vector<std::byte> dds_fixture() {
    std::vector<std::byte> out; u32(out, 0x20534444U); u32(out, 124); u32(out, 0x100f); u32(out, 1); u32(out, 1); u32(out, 4); u32(out, 0); u32(out, 1);
    for (int i = 0; i < 11; ++i) u32(out, 0);
    u32(out, 32); u32(out, 0x41); u32(out, 0); u32(out, 32); u32(out, 0x00ff0000U); u32(out, 0x0000ff00U); u32(out, 0x000000ffU); u32(out, 0xff000000U);
    u32(out, 0x1000); for (int i = 0; i < 4; ++i) u32(out, 0); u8(out, 3); u8(out, 2); u8(out, 1); u8(out, 4); return out;
}

std::vector<std::byte> tga_rle_fixture(const bool top_left) {
    std::vector<std::byte> out; u8(out, 0); u8(out, 0); u8(out, 10); for (int i = 0; i < 5; ++i) u8(out, 0); for (int i = 0; i < 4; ++i) u8(out, 0);
    u16(out, 2); u16(out, 1); u8(out, 32); u8(out, top_left ? 0x20 : 0x00); u8(out, 0x81); u8(out, 9); u8(out, 8); u8(out, 7); u8(out, 6); return out;
}

} // namespace

std::vector<std::byte> file_bytes(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto length = file.tellg();
    if (length < 0) return {};
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), length);
    return bytes;
}

int main(int argc, char** argv) {
    using namespace eawr::assets;

    if (argc == 3) {
        const auto model_bytes = file_bytes(argv[1]);
        const auto animation_bytes = file_bytes(argv[2]);
        auto model = load_model(model_bytes, source("private-real.alo", model_bytes.size()));
        auto animation = load_animation(animation_bytes, source("private-real.ala", animation_bytes.size()));
        if (!model || !animation) return 2;
        std::size_t vertices{}, indices{}, parameters{};
        for (const auto& mesh : model.value().meshes) for (const auto& submesh : mesh.submeshes) {
            vertices += submesh.vertices.size(); indices += submesh.indices.size(); parameters += submesh.parameters.size();
        }
        std::size_t samples{}; for (const auto& track : animation.value().tracks) samples += track.samples.size();
        std::cout << "model bones=" << model.value().bones.size() << " meshes=" << model.value().meshes.size()
                  << " lights=" << model.value().lights.size() << " proxies=" << model.value().proxies.size()
                  << " dazzles=" << model.value().dazzles.size() << " vertices=" << vertices << " indices=" << indices
                  << " parameters=" << parameters << '\n';
        std::cout << "animation version=" << static_cast<unsigned>(animation.value().version)
                  << " frames=" << animation.value().stored_frame_count << " tracks=" << animation.value().tracks.size()
                  << " samples=" << samples << " duration=" << animation.value().duration_seconds << '\n';
        return 0;
    }

    auto model_bytes = model_fixture();
    auto model = load_model(model_bytes, source("synthetic.alo", model_bytes.size()));
    expect(bool(model), "independent ALO fixture loads");
    if (model) {
        expect(model.value().bones.size() == 1, "ALO bone count retained");
        expect(model.value().bones[0].relative_transform[4] == 1.0F, "ALO transform floats retained");
        expect(model.value().source.source_id == "acceptance:synthetic", "ALO provenance retained");
    }
    auto cycle = model_fixture(0);
    auto cycle_result = load_model(cycle, source("cycle.alo", cycle.size()));
    expect(!cycle_result && cycle_result.error().code == diagnostic_codes::hierarchy_cycle, "ALO hierarchy cycle rejected");
    auto truncated = model_bytes; truncated.pop_back();
    auto trunc_result = load_model(truncated, source("truncated.alo", truncated.size()));
    expect(!trunc_result, "ALO enclosing chunk truncation rejected");
    auto mismatch = load_model(model_bytes, source("mismatch.alo", model_bytes.size() + 1));
    expect(!mismatch && mismatch.error().code == diagnostic_codes::source_mismatch, "ALO provenance mismatch rejected");
    auto unknown_root = model_bytes; const auto connection_offset = unknown_root.size() - 28U; unknown_root.insert(unknown_root.begin() + static_cast<std::ptrdiff_t>(connection_offset), 8, std::byte{0});
    std::vector<std::byte> unknown_header; u32(unknown_header, 0x9999); u32(unknown_header, 0); std::copy(unknown_header.begin(), unknown_header.end(), unknown_root.begin() + static_cast<std::ptrdiff_t>(connection_offset));
    auto unknown_result = load_model(unknown_root, source("unknown.alo", unknown_root.size()));
    expect(!unknown_result && unknown_result.error().code == diagnostic_codes::unsupported, "unknown essential ALO root rejected");

    auto animation_bytes = animation_fixture();
    auto animation = load_animation(animation_bytes, source("synthetic.ala", animation_bytes.size()));
    expect(bool(animation), "independent ALA fixture loads");
    if (animation) {
        expect(animation.value().stored_frame_count == 1, "ALA stored frame count retained");
        expect(animation.value().playable_frame_count == 0, "ALA duplicated terminal frame excluded");
        expect(animation.value().duration_seconds == 0.0F, "ALA playable duration derived from terminal convention");
    }
    auto animation_truncated = animation_bytes; animation_truncated.pop_back();
    expect(!load_animation(animation_truncated, source("truncated.ala", animation_truncated.size())), "ALA truncation rejected");

    auto dds_bytes = dds_fixture();
    auto dds = load_texture(dds_bytes, source("synthetic.dds", dds_bytes.size()));
    expect(bool(dds), "independent DDS fixture loads");
    if (dds) {
        expect(dds.value().format == PixelFormat::bgra8, "DDS masks retain BGRA format");
        expect(dds.value().mips.size() == 1 && dds.value().mips[0].bytes.size() == 4, "DDS mip payload retained");
    }
    auto dds_truncated = dds_bytes; dds_truncated.pop_back();
    expect(!load_texture(dds_truncated, source("truncated.dds", dds_truncated.size())), "DDS payload truncation rejected");
    auto tga = tga_rle_fixture(false);
    auto tga_result = load_texture(tga, source("bottom.tga", tga.size()));
    expect(bool(tga_result), "independent RLE TGA fixture loads");
    if (tga_result) {
        expect(tga_result.value().source_origin == ImageOrigin::bottom_left, "TGA origin retained");
        expect(tga_result.value().format == PixelFormat::rgba8 && tga_result.value().has_alpha, "TGA BGR expands to RGBA with alpha");
        expect(tga_result.value().mips[0].bytes[0] == std::byte{7} && tga_result.value().mips[0].bytes[3] == std::byte{6}, "TGA colour channels and source alpha correct");
    }
    auto tga_overrun = tga; tga_overrun[18] = std::byte{0x82};
    auto overrun_result = load_texture(tga_overrun, source("overrun.tga", tga_overrun.size()));
    expect(!overrun_result && overrun_result.error().code == diagnostic_codes::bounds, "TGA RLE pixel-count overrun rejected");

    if (failures == 0) std::cout << "p0-06 independent asset acceptance passed\n";
    return failures == 0 ? 0 : 1;
}
