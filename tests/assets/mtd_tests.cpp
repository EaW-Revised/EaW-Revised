#include "eawr/assets/assets.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
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
void u8(std::vector<std::byte>& bytes, const std::uint8_t value) {
    bytes.push_back(static_cast<std::byte>(value));
}
void u16(std::vector<std::byte>& bytes, const std::uint16_t value) {
    u8(bytes, static_cast<std::uint8_t>(value & 0xffU));
    u8(bytes, static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}
void u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        u8(bytes, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}
void entry(std::vector<std::byte>& bytes, const std::string& name,
           const std::uint32_t x, const std::uint32_t y,
           const std::uint32_t width, const std::uint32_t height,
           const bool alpha) {
    for (const char character : name) u8(bytes, static_cast<std::uint8_t>(character));
    for (std::size_t index = name.size(); index < 64U; ++index) u8(bytes, 0U);
    u32(bytes, x); u32(bytes, y); u32(bytes, width); u32(bytes, height);
    u8(bytes, alpha ? 1U : 0U);
}
std::vector<std::byte> fixture() {
    std::vector<std::byte> bytes;
    u32(bytes, 2U);
    entry(bytes, "I_ALPHA.TGA", 1U, 2U, 16U, 8U, true);
    entry(bytes, "I_OPAQUE.TGA", 32U, 4U, 12U, 6U, false);
    return bytes;
}
std::vector<std::byte> duplicate_fixture() {
    std::vector<std::byte> bytes;
    u32(bytes, 2U);
    entry(bytes, "I_SHARED.TGA", 1U, 2U, 3U, 4U, true);
    entry(bytes, "i_shared.tga", 5U, 6U, 7U, 8U, false);
    return bytes;
}
std::vector<std::byte> one_entry_fixture(const std::string& name,
                                         const std::uint32_t x,
                                         const std::uint32_t y,
                                         const std::uint32_t width,
                                         const std::uint32_t height,
                                         const bool alpha = true) {
    std::vector<std::byte> bytes;
    u32(bytes, 1U);
    entry(bytes, name, x, y, width, height, alpha);
    return bytes;
}
std::vector<std::byte> tga_fixture(const std::uint16_t width,
                                   const std::uint16_t height) {
    std::vector<std::byte> bytes;
    u8(bytes, 0U); u8(bytes, 0U); u8(bytes, 2U);
    for (int index = 0; index < 5; ++index) u8(bytes, 0U);
    for (int index = 0; index < 4; ++index) u8(bytes, 0U);
    u16(bytes, width); u16(bytes, height); u8(bytes, 24U); u8(bytes, 0x20U);
    bytes.resize(18U + static_cast<std::size_t>(width) * height * 3U, std::byte{0});
    return bytes;
}
std::vector<std::byte> dds_fixture() {
    std::vector<std::byte> bytes;
    u32(bytes, 0x20534444U); u32(bytes, 124U); u32(bytes, 0x100fU);
    u32(bytes, 1U); u32(bytes, 1U); u32(bytes, 4U); u32(bytes, 0U); u32(bytes, 1U);
    for (int index = 0; index < 11; ++index) u32(bytes, 0U);
    u32(bytes, 32U); u32(bytes, 0x41U); u32(bytes, 0U); u32(bytes, 32U);
    u32(bytes, 0x00ff0000U); u32(bytes, 0x0000ff00U);
    u32(bytes, 0x000000ffU); u32(bytes, 0xff000000U);
    u32(bytes, 0x1000U);
    for (int index = 0; index < 4; ++index) u32(bytes, 0U);
    u8(bytes, 3U); u8(bytes, 2U); u8(bytes, 1U); u8(bytes, 4U);
    return bytes;
}
void write_bytes(const std::filesystem::path& path,
                 const std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}
std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input) return {};
    return bytes;
}
struct TempTree final {
    std::filesystem::path root;
    TempTree() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
            ("eawr-mtd-" + std::to_string(stamp));
        std::filesystem::create_directories(root);
    }
    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};
eawr::assets::Source source(const std::string& path, const std::size_t size) {
    return {path, "synthetic", "test", eawr::vfs::AssetOrigin::loose, size};
}
} // namespace

int main(const int argc, char** argv) {
    using namespace eawr::assets;
    if (argc == 6 && std::string_view(argv[1]) == "--inspect-loose-atlas") {
        const auto mtd_bytes = read_bytes(argv[2]);
        const auto page_bytes = read_bytes(argv[3]);
        auto directory = load_mega_texture(
            mtd_bytes, source(argv[4], mtd_bytes.size()));
        auto page = load_texture(page_bytes, source(argv[5], page_bytes.size()));
        if (!directory || !page) {
            const auto& error = directory ? page.error() : directory.error();
            std::cerr << eawr::core::format_diagnostic(error) << '\n';
            return 2;
        }
        auto bound = bind_mega_texture(
            std::move(directory.value()), std::move(page.value()));
        if (!bound) {
            std::cerr << eawr::core::format_diagnostic(bound.error()) << '\n';
            return 2;
        }
        std::cout << "entries=" << bound.value().directory.entries.size()
                  << " page=" << bound.value().page.width << 'x'
                  << bound.value().page.height << '\n';
        return 0;
    }
    const auto bytes = fixture();
    auto atlas = load_mega_texture(bytes, source("data/art/textures/test.mtd", bytes.size()));
    expect(bool(atlas), "two-entry synthetic MTD loads");
    if (atlas) {
        expect(atlas.value().backing_page_stem == "data/art/textures/test",
               "backing page stem is derived from the MTD path");
        expect(atlas.value().entries.size() == 2U, "both entries are returned");
        expect(atlas.value().entries[0].name == "I_ALPHA.TGA", "entry name is retained");
        expect(atlas.value().entries[0].rectangle == AtlasRectangle{1U, 2U, 16U, 8U},
               "entry rectangle is retained");
        expect(atlas.value().entries[0].has_alpha, "alpha-on record is retained");
        expect(!atlas.value().entries[1].has_alpha, "alpha-off record is retained");
        expect(!atlas.value().entries[0].flip_x && !atlas.value().entries[0].flip_y,
               "top-left MTD coordinates require no derived entry flip");
        expect(atlas.value().find("i_alpha.tga") == &atlas.value().entries[0],
               "name lookup is ASCII case-insensitive");
        expect(atlas.value().find("I_MISSING.TGA") == nullptr,
               "missing name lookup returns null");
    }

    auto truncated = bytes;
    truncated.pop_back();
    auto short_result = load_mega_texture(
        truncated, source("short.mtd", truncated.size()));
    expect(!short_result && short_result.error().code == diagnostic_codes::truncated,
           "truncated entry table is rejected");

    auto trailing = bytes;
    trailing.push_back(std::byte{0});
    auto trailing_result = load_mega_texture(
        trailing, source("trailing.mtd", trailing.size()));
    expect(!trailing_result && trailing_result.error().code == diagnostic_codes::bounds,
           "unexplained trailing bytes are rejected");

    auto malformed_alpha = bytes;
    malformed_alpha[4U + 80U] = std::byte{2};
    auto alpha_result = load_mega_texture(
        malformed_alpha, source("alpha.mtd", malformed_alpha.size()));
    expect(!alpha_result && alpha_result.error().code == diagnostic_codes::mega_texture_entry,
           "non-boolean alpha field is rejected");

    auto unterminated = bytes;
    for (std::size_t index = 4U; index < 68U; ++index) unterminated[index] = std::byte{'A'};
    auto name_result = load_mega_texture(
        unterminated, source("name.mtd", unterminated.size()));
    expect(!name_result && name_result.error().code == diagnostic_codes::mega_texture_entry,
           "unterminated name is rejected");

    auto bad_source = load_mega_texture(bytes, source("test.bin", bytes.size()));
    expect(!bad_source && bad_source.error().code == diagnostic_codes::mega_texture_header,
           "non-MTD provenance path is rejected");

    const auto duplicates = duplicate_fixture();
    auto duplicate_atlas = load_mega_texture(
        duplicates, source("duplicates.mtd", duplicates.size()));
    expect(duplicate_atlas &&
               duplicate_atlas.value().find("I_SHARED.TGA") ==
                   &duplicate_atlas.value().entries[1],
           "final case-insensitive duplicate wins lookup");

    MegaTexture caller_directory;
    caller_directory.source = source("caller.mtd", 0U);
    caller_directory.backing_page_stem = "caller";
    caller_directory.entries.push_back(MegaTextureEntry{
        "I_OVERFLOW.TGA",
        AtlasRectangle{std::numeric_limits<std::uint32_t>::max(), 0U, 2U, 1U},
        true, false, false});
    Texture caller_page;
    caller_page.source = source("caller.tga", 0U);
    caller_page.width = 64U;
    caller_page.height = 64U;
    auto caller_bounds = bind_mega_texture(
        std::move(caller_directory), std::move(caller_page));
    expect(!caller_bounds &&
               caller_bounds.error().code == diagnostic_codes::mega_texture_rectangle,
           "caller-constructed rectangle overflow is rejected without addition");

    TempTree tree;
    const auto texture_root = tree.root / "Art" / "Textures";
    write_bytes(texture_root / "TgaAtlas.MTD", bytes);
    write_bytes(texture_root / "TgaAtlas.TGA", tga_fixture(64U, 16U));
    const auto tiny = one_entry_fixture("I_TINY.TGA", 0U, 0U, 1U, 1U, false);
    write_bytes(texture_root / "DdsAtlas.MTD", tiny);
    write_bytes(texture_root / "DdsAtlas.DDS", dds_fixture());
    write_bytes(texture_root / "Missing.MTD", tiny);
    write_bytes(texture_root / "Bad.MTD", tiny);
    const std::array bad_texture{std::byte{'n'}, std::byte{'o'}, std::byte{'p'}, std::byte{'e'}};
    write_bytes(texture_root / "Bad.TGA", bad_texture);
    const auto outside = one_entry_fixture("I_OUTSIDE.TGA", 63U, 15U, 2U, 2U);
    write_bytes(texture_root / "Outside.MTD", outside);
    write_bytes(texture_root / "Outside.TGA", tga_fixture(64U, 16U));
    write_bytes(texture_root / "Both.MTD", tiny);
    write_bytes(texture_root / "Both.TGA", tga_fixture(1U, 1U));
    write_bytes(texture_root / "Both.DDS", dds_fixture());

    const std::array mounts{eawr::vfs::MountSpec{"fixture", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(bool(mounted), "synthetic atlas VFS mounts");
    if (mounted) {
        auto tga = load_mega_texture_atlas(
            mounted.value(), "data/art/textures/tgaatlas.mtd");
        expect(bool(tga), "automatic atlas resolution loads an actual TGA page");
        if (tga) {
            expect(tga.value().page.width == 64U && tga.value().page.height == 16U,
                   "decoded TGA dimensions are retained");
            expect(tga.value().directory.entries[0].has_alpha &&
                       !tga.value().directory.entries[1].has_alpha,
                   "bound atlas retains per-entry alpha metadata");
            expect(tga.value().directory.source.logical_path ==
                       "data/art/textures/tgaatlas.mtd" &&
                       tga.value().page.source.logical_path ==
                       "data/art/textures/tgaatlas.tga",
                   "bound atlas retains independent MTD and page logical paths");
            expect(tga.value().directory.source.layer_id == "fixture" &&
                       tga.value().page.source.layer_id == "fixture" &&
                       tga.value().directory.source.origin == eawr::vfs::AssetOrigin::loose &&
                       tga.value().page.source.origin == eawr::vfs::AssetOrigin::loose,
                   "bound atlas retains VFS layer and origin provenance");
            expect(!tga.value().directory.source.source_id.empty() &&
                       !tga.value().page.source.source_id.empty() &&
                       tga.value().directory.source.source_id !=
                           tga.value().page.source.source_id &&
                       tga.value().directory.source.stored_size == bytes.size() &&
                       tga.value().page.source.stored_size == tga_fixture(64U, 16U).size(),
                   "bound atlas retains VFS source identifiers and stored sizes");
        }

        auto dds = load_mega_texture_atlas(
            mounted.value(), "data/art/textures/ddsatlas.mtd");
        expect(dds && dds.value().page.format == PixelFormat::bgra8,
               "automatic atlas resolution continues to support DDS pages");

        auto missing = load_mega_texture_atlas(
            mounted.value(), "data/art/textures/missing.mtd");
        expect(!missing && missing.error().code == diagnostic_codes::mega_texture_page,
               "missing sibling page has a stable atlas-page diagnostic");

        auto bad = load_mega_texture_atlas(
            mounted.value(), "data/art/textures/bad.mtd");
        expect(!bad && bad.error().code == diagnostic_codes::texture_format,
               "invalid sibling texture retains the texture-loader diagnostic");

        auto out_of_page = load_mega_texture_atlas(
            mounted.value(), "data/art/textures/outside.mtd");
        expect(!out_of_page &&
                   out_of_page.error().code == diagnostic_codes::mega_texture_rectangle &&
                   out_of_page.error().message.find("I_OUTSIDE.TGA") != std::string::npos &&
                   out_of_page.error().message.find("64x16") != std::string::npos,
               "out-of-page rectangle has a stable, dimensioned diagnostic");

        auto ambiguous = load_mega_texture_atlas(
            mounted.value(), "data/art/textures/both.mtd");
        expect(!ambiguous && ambiguous.error().code == diagnostic_codes::mega_texture_page,
               "automatic lookup rejects ambiguous DDS/TGA siblings");
        auto explicit_page = load_mega_texture_atlas(
            mounted.value(), "data/art/textures/both.mtd",
            "data/art/textures/both.tga");
        expect(explicit_page && explicit_page.value().page.format == PixelFormat::rgba8,
               "explicit page path resolves an otherwise ambiguous atlas");
    }

    if (failures == 0) std::cout << "MTD tests passed\n";
    return failures == 0 ? 0 : 1;
}
