#include "particle_texture_load.hpp"

#include "bc3_fixture.hpp"
#include "eawr/assets/texture_chain.hpp"
#include "eawr/vfs/vfs.hpp"
#include "particle_texture.hpp"
#include "texture_mip_tail.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// MT-03 (docs/behaviour/texture-mip-upload.md): a particle colour or bump normal texture that stops above 1x1 arrives on the GPU with a full chain. The upload
// bytes below are what GodotParticleBackend::acquire_texture hands to Image::create_from_data, so a one-level upload
// (the build before this change) fails the first case.
namespace {

namespace fs = std::filesystem;
using eawr::presentation::godot_backend::load_particle_texture;
using eawr::presentation::godot_backend::detail::prepare_particle_texture;
using eawr::presentation::godot_backend::detail::texture_mip_tail_bytes;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

struct Tree final {
    fs::path root;
    Tree() {
        root = fs::temp_directory_path()
            / ("eawr_particle_chain_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root / "art" / "textures");
    }
    ~Tree() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;
    void write(const std::string& name, const std::vector<char>& bytes) const {
        std::ofstream file(root / "art" / "textures" / name, std::ios::binary);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
};

std::uint64_t full_chain_bytes(const eawr::assets::Texture& texture) {
    std::uint64_t total = 0;
    for (std::uint32_t edge = texture.width; ; edge /= 2) {
        const std::uint64_t blocks = (std::max(edge, 1U) + 3U) / 4U;
        total += blocks * blocks * 16U;
        if (edge <= 1U) break;
    }
    return total;
}

void particle_textures_reach_the_upload_with_a_full_chain() {
    constexpr std::uint32_t side = 256;
    const auto single = eawr::testing::single_level_bc3(side);
    auto partial = single;
    {
        auto grown = single;
        eawr::assets::extend_mip_chain(grown, eawr::assets::ChainKind::colour);
        partial.mips.assign(grown.mips.begin(), grown.mips.begin() + 3);
    }
    const auto complete = [&] {
        auto grown = single;
        eawr::assets::extend_mip_chain(grown, eawr::assets::ChainKind::colour);
        return grown;
    }();
    Tree tree;
    tree.write("spark_single.dds", eawr::testing::dds_bc3_bytes(single));
    tree.write("spark_partial.dds", eawr::testing::dds_bc3_bytes(partial));
    tree.write("spark_full.dds", eawr::testing::dds_bc3_bytes(complete));
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "the fixture mounts");
    if (!mounted) return;
    const auto vfs = std::move(mounted).value();

    const auto raw = eawr::assets::load_texture(vfs, "data/art/textures/spark_single.dds");
    expect(raw && raw.value().mips.size() == 1, "the fixture file is single-level on disk");

    // Single level: all nine levels, the authored top level untouched, nothing left for the renderer's tail fill.
    const auto loaded = load_particle_texture(vfs, "data/art/textures/spark_single.dds");
    expect(loaded && loaded.value().mips.size() == 9, "a single-level particle texture loads with all nine levels");
    if (loaded && loaded.value().mips.size() == 9) {
        const auto& texture = loaded.value();
        expect(texture.mips[0].bytes == single.mips[0].bytes, "its top level is the file's, byte for byte");
        expect(texture.mips.back().width == 1 && texture.mips.back().height == 1, "the chain ends at 1 x 1");
        expect(texture.mips[1].bytes == complete.mips[1].bytes, "the levels are the plain box chain (colour kind)");
        expect(texture_mip_tail_bytes(texture) == 0, "the renderer has no tail left to fill in");
        const auto upload = prepare_particle_texture(texture);
        expect(upload && upload.bytes.size() == full_chain_bytes(texture),
            "the bytes handed to the GPU hold the full chain, not one level");
    }

    // Partial chain: completed with real box levels, authored levels unchanged.
    const auto grown = load_particle_texture(vfs, "data/art/textures/spark_partial.dds");
    expect(grown && grown.value().mips.size() == 9, "a partial particle texture is completed to 1 x 1");
    if (grown && grown.value().mips.size() == 9) {
        for (std::size_t level = 0; level < 3; ++level)
            expect(grown.value().mips[level].bytes == partial.mips[level].bytes, "authored levels stay byte for byte");
        expect(texture_mip_tail_bytes(grown.value()) == 0, "no replicated tail remains");
    }

    // A texture that already has its chain is returned exactly as decoded (stock particle textures).
    const auto stock = load_particle_texture(vfs, "data/art/textures/spark_full.dds");
    const auto stock_plain = eawr::assets::load_texture(vfs, "data/art/textures/spark_full.dds");
    expect(stock && stock_plain && stock.value().mips.size() == stock_plain.value().mips.size(), "a full chain keeps its level count");
    if (stock && stock_plain && stock.value().mips.size() == stock_plain.value().mips.size()) {
        bool same = true;
        for (std::size_t level = 0; level < stock.value().mips.size(); ++level)
            same = same && stock.value().mips[level].bytes == stock_plain.value().mips[level].bytes;
        expect(same, "a texture with a full chain is byte-identical to the plain load");
        const auto a = prepare_particle_texture(stock.value());
        const auto b = prepare_particle_texture(stock_plain.value());
        expect(a && b && a.bytes == b.bytes, "and so is its upload");
    }

    expect(!load_particle_texture(vfs, "data/art/textures/missing.dds"), "a missing file still fails as before");
}

} // namespace

int main() {
    particle_textures_reach_the_upload_with_a_full_chain();
    if (failures != 0) {
        std::cerr << failures << " particle_texture_load check(s) failed\n";
        return 1;
    }
    std::cout << "particle_texture_load_tests ok\n";
    return 0;
}
