#include "eawr/assets/assets.hpp"
#include "../ui/ui_test_support.hpp"

#include <algorithm>

namespace {
using namespace eawr::assets;
using eawr::test::ui::expect;
using eawr::test::ui::write_bytes;
using Bytes = std::vector<std::byte>;

void put(Bytes& bytes, const std::size_t offset, const std::uint32_t value,
         const unsigned count = 4U) {
    for (unsigned i = 0; i < count; ++i)
        bytes.at(offset + i) = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
}
Source source(const Bytes& bytes) {
    return {"data/art/textures/mt_commandbar.tga", "synthetic", "test",
            eawr::vfs::AssetOrigin::loose, bytes.size()};
}
Bytes bmp() {
    Bytes bytes(54U + 16U * 8U * 4U);
    put(bytes, 0, 0x4d42, 2); put(bytes, 2, static_cast<std::uint32_t>(bytes.size()));
    put(bytes, 10, 54); put(bytes, 14, 40); put(bytes, 18, 16); put(bytes, 22, 8);
    put(bytes, 26, 1, 2); put(bytes, 28, 32, 2);
    for (std::size_t i = 54; i < bytes.size(); i += 4) put(bytes, i, 0x80402010);
    put(bytes, bytes.size() - 4U, 0xff030201);
    return bytes;
}
Bytes stale_dds() {
    // A valid 1x1 BGRA page cannot contain the mod's second atlas rectangle.
    Bytes bytes(132U);
    put(bytes, 0, 0x20534444); put(bytes, 4, 124); put(bytes, 8, 0x100f);
    put(bytes, 12, 1); put(bytes, 16, 1); put(bytes, 20, 4); put(bytes, 28, 1);
    put(bytes, 76, 32); put(bytes, 80, 0x41); put(bytes, 88, 32);
    put(bytes, 92, 0x00ff0000); put(bytes, 96, 0x0000ff00);
    put(bytes, 100, 0x000000ff); put(bytes, 104, 0xff000000);
    put(bytes, 108, 0x1000); put(bytes, 128, 0x80402010);
    return bytes;
}
Bytes directory(const std::uint32_t width, const std::uint32_t height) {
    Bytes bytes(4U + 2U * 81U);
    put(bytes, 0, 2);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto offset = 4U + i * 81U;
        bytes[offset] = i == 0 ? std::byte{'A'} : std::byte{'B'};
        put(bytes, offset + 64U, i == 0 ? 0U : width - 1U);
        put(bytes, offset + 68U, i == 0 ? 0U : height - 1U);
        put(bytes, offset + 72U, 1); put(bytes, offset + 76U, 1);
        bytes[offset + 80U] = std::byte{1};
    }
    return bytes;
}
Bytes rle_tga(const std::uint16_t width, const std::uint16_t height) {
    Bytes bytes(18U);
    bytes[2] = std::byte{10};
    put(bytes, 12, width, 2); put(bytes, 14, height, 2);
    bytes[16] = std::byte{32}; bytes[17] = std::byte{0x28};
    std::size_t remaining = static_cast<std::size_t>(width) * height;
    while (remaining != 0U) {
        const auto count = std::min(remaining, std::size_t{128});
        bytes.push_back(static_cast<std::byte>(0x80U | (count - 1U)));
        bytes.insert(bytes.end(), {std::byte{3}, std::byte{2}, std::byte{1}, std::byte{0x80}});
        remaining -= count;
    }
    return bytes;
}
void malformed_bmp() {
    const auto original = bmp();
    for (const auto size : {2U, 14U, 53U, 54U, 565U}) {
        auto bytes = original; bytes.resize(size);
        const auto decoded = load_texture(bytes, source(bytes));
        expect(!decoded && decoded.error().code == diagnostic_codes::truncated,
               "BMP truncation has a named diagnostic");
    }
    const auto reject = [&](const std::size_t offset, const std::uint32_t value,
                            const std::string_view code) {
        auto bytes = original; put(bytes, offset, value);
        const auto decoded = load_texture(bytes, source(bytes));
        expect(!decoded && decoded.error().code == code &&
                   decoded.error().logical_path == source(bytes).logical_path,
               "malformed BMP fails closed with named diagnostic and provenance");
    };
    reject(10, 53, diagnostic_codes::bounds);
    reject(10, 0xffffffffU, diagnostic_codes::bounds);
    reject(2, 0, diagnostic_codes::bounds);
    reject(34, 1, diagnostic_codes::bounds);
    reject(14, 108, diagnostic_codes::texture_format);
    reject(28, 24, diagnostic_codes::texture_format);
    reject(30, 3, diagnostic_codes::texture_format);
    reject(46, 1, diagnostic_codes::texture_format);
    reject(26, 32U << 16U, diagnostic_codes::texture_header);
    reject(18, 0, diagnostic_codes::limit);
    reject(18, 0xffffffffU, diagnostic_codes::limit);
    reject(22, 0, diagnostic_codes::limit);
    reject(22, 0x80000000U, diagnostic_codes::limit);
    reject(18, 0x7fffffffU, diagnostic_codes::limit);
    auto bytes = original; bytes.push_back(std::byte{0});
    auto decoded = load_texture(bytes, source(bytes));
    expect(!decoded && decoded.error().code == diagnostic_codes::bounds, "BMP trailing bytes rejected");
    bytes = original; put(bytes, 22, 0xfffffff8U); put(bytes, 34, 16U * 8U * 4U);
    decoded = load_texture(bytes, source(bytes));
    expect(decoded && decoded.value().source_origin == ImageOrigin::top_left &&
               decoded.value().mips[0].bytes.front() == std::byte{0x10},
           "top-down BMP preserves row order and accepts exact image size");
}
void fixtures() {
    eawr::test::ui::TempTree tree("mod-atlas");
    const auto root = tree.root / "art/textures";
    write_bytes(root / "mt_commandbar.mtd", directory(16, 8));
    write_bytes(root / "mt_commandbar.tga", bmp());
    write_bytes(root / "mt_commandbarcompressed.dds", stale_dds());
    const std::array mounts{eawr::vfs::MountSpec{"mod", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(bool(mounted), "atlas fixture VFS mounts");
    if (!mounted) return;
    const auto stale = load_mega_texture_atlas(mounted.value(),
        "data/art/textures/mt_commandbar.mtd", "data/art/textures/mt_commandbarcompressed.dds");
    expect(!stale && stale.error().code == diagnostic_codes::mega_texture_rectangle,
           "stale DDS decodes but does not fit the modded directory");
    {
        auto atlas = load_mega_texture_atlas(mounted.value(), "data/art/textures/mt_commandbar.mtd");
        expect(atlas && atlas.value().directory.entries.size() == 2U &&
                   atlas.value().page.width == 16 && atlas.value().page.height == 8,
               "atlas_bmp_page: BMP content under TGA name binds two-entry MTD");
        if (atlas) {
            const auto& page = atlas.value().page;
            expect(page.format == PixelFormat::bgra8 && page.has_alpha &&
                       page.source_origin == ImageOrigin::bottom_left && page.mips[0].row_pitch == 64 &&
                       page.mips[0].bytes[3] == std::byte{0x80} &&
                       page.mips[0].bytes[508] == std::byte{1},
                   "BMP channels, alpha, origin and final pixel are retained");
            expect(page.source.logical_path == "data/art/textures/mt_commandbar.tga",
                   "atlas_stale_compressed: same-stem page wins over stale Compressed.dds");
        }
    }
    write_bytes(root / "large.mtd", directory(8192, 4096));
    write_bytes(root / "large.tga", rle_tga(8192, 4096));
    mounted = eawr::vfs::Vfs::mount(mounts);
    expect(bool(mounted), "large atlas fixture VFS mounts");
    if (!mounted) return;
    {
        auto atlas = load_mega_texture_atlas(mounted.value(), "data/art/textures/large.mtd");
        expect(atlas && atlas.value().page.width == 8192 && atlas.value().page.height == 4096 &&
                   atlas.value().page.mips[0].bytes.size() == 8192U * 4096U * 4U &&
                   atlas.value().page.mips[0].bytes.back() == std::byte{0x80},
               "atlas_large_page: 8192x4096 RLE page and edge rectangle load");
    }
    // Exercise the exact budget cheaply with grayscale RLE (64 MiB decoded).
    Bytes limit(18U); limit[2] = std::byte{11}; limit[16] = std::byte{8};
    put(limit, 12, 8192, 2); put(limit, 14, 8192, 2);
    for (std::size_t i = 0; i < 8192U * 8192U / 128U; ++i) {
        limit.push_back(std::byte{0xff}); limit.push_back(std::byte{0x42});
    }
    {
        auto decoded = load_texture(limit, source(limit));
        expect(decoded && decoded.value().mips[0].bytes.size() == 8192U * 8192U,
               "texture pixel budget accepts 8192 squared");
    }
    put(limit, 14, 8193, 2);
    const auto excessive = load_texture(limit, source(limit));
    expect(!excessive && excessive.error().code == diagnostic_codes::limit,
           "texture pixel budget rejects one row beyond 8192 squared");
}
void corpus() {
    const auto check = [](const eawr::vfs::Vfs& filesystem, const std::string& name) {
        auto atlas = load_mega_texture_atlas(filesystem, "data/art/textures/mt_commandbar.mtd");
        if (!atlas) std::cerr << name << ": " << eawr::core::format_diagnostic(atlas.error()) << '\n';
        expect(bool(atlas), "corpus command-bar atlas loads through shared page pairing");
        if (!atlas) return;
        const auto& value = atlas.value();
        expect(!value.directory.entries.empty(), "corpus atlas is nonempty");
        std::cout << name << ": " << value.directory.entries.size() << " entries, "
                  << value.page.width << 'x' << value.page.height << ", "
                  << value.page.source.logical_path << ", " << value.page.source.layer_id << '\n';
    };
    if (auto foc = eawr::test::ui::foc_corpus("atlas")) check(*foc, "FoC");
    for (const auto& mod : eawr::test::ui::mod_corpora("atlas")) check(mod.filesystem, mod.name);
}
} // namespace

int main() {
    malformed_bmp();
    fixtures();
    corpus();
    return eawr::test::ui::failures() == 0 ? 0 : 1;
}
