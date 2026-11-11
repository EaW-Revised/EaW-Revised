#include "compose_texture_cache.hpp"

#include "bc3_fixture.hpp"
#include "texture_prefetch_policy.hpp"
#include "eawr/assets/texture_chain.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/scene/texture_prefetch.hpp"
#include "eawr/vfs/vfs.hpp"
#include "texture_mip_tail.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using eawr::presentation::godot_backend::ComposeTextureCache;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

constexpr std::uint64_t kib = 1ULL << 10U;
constexpr std::uint64_t mib = 1ULL << 20U;

eawr::assets::Texture texture_of(const std::uint64_t bytes) {
    eawr::assets::Texture texture;
    texture.width = 4;
    texture.height = 4;
    eawr::assets::MipLevel level;
    level.width = 4;
    level.height = 4;
    level.bytes.assign(static_cast<std::size_t>(bytes), std::byte{7});
    texture.mips.push_back(std::move(level));
    return texture;
}

// Mirrors the call in upload_surface; the loader counts its own invocations.
std::shared_ptr<const eawr::assets::Texture> fetch(ComposeTextureCache& cache, const std::string& path,
    const std::uint64_t bytes, std::uint64_t& loader_calls) {
    return cache.get(path, [&]() -> std::optional<eawr::assets::Texture> {
        ++loader_calls;
        return texture_of(bytes);
    });
}

void unbounded_holds_everything() {
    ComposeTextureCache cache; // the behaviour of earlier builds
    std::uint64_t calls = 0;
    for (int index = 0; index < 100; ++index) fetch(cache, "t" + std::to_string(index), 64 * kib, calls);
    const auto& stats = cache.stats();
    expect(calls == 100 && stats.decodes == 100, "an unbounded cache decodes each path once");
    expect(stats.evictions == 0 && stats.redecodes == 0, "an unbounded cache never evicts");
    expect(stats.held_bytes == 100 * 64 * kib && stats.peak_held_bytes == 100 * 64 * kib,
        "an unbounded cache holds every texture");
    fetch(cache, "t0", 64 * kib, calls);
    expect(calls == 100 && cache.stats().hits == 1, "a held path is served without a decode");
}

void bounded_release_keeps_the_budget() {
    // A scene of 200 models, each with 4 textures used back to back (three surfaces per texture), 512 KiB per texture.
    constexpr std::uint64_t texture_bytes = 512 * kib;
    constexpr std::uint64_t budget = 4 * mib; // eight textures
    ComposeTextureCache cache(budget);
    std::uint64_t calls = 0;
    for (int model = 0; model < 200; ++model) {
        for (int surface = 0; surface < 3; ++surface)
            for (int texture = 0; texture < 4; ++texture)
                fetch(cache, "m" + std::to_string(model) + "_" + std::to_string(texture), texture_bytes, calls);
    }
    const auto& stats = cache.stats();
    expect(stats.peak_held_bytes <= budget, "the held bytes never pass the budget");
    expect(stats.held_bytes <= budget, "the held bytes at the end are within the budget");
    expect(stats.decodes == 800 && calls == 800, "back-to-back reuse inside a model costs no repeat decode");
    expect(stats.redecodes == 0, "no path is decoded twice");
    expect(stats.evictions == 800 - 8, "everything over the budget leaves, oldest first");
    expect(stats.hits == 800 * 2, "the other two surfaces of every texture hit");
}

void least_recently_used_leaves_first() {
    ComposeTextureCache cache(3 * 64 * kib);
    std::uint64_t calls = 0;
    fetch(cache, "a", 64 * kib, calls);
    fetch(cache, "b", 64 * kib, calls);
    fetch(cache, "c", 64 * kib, calls);
    fetch(cache, "a", 64 * kib, calls);  // a is now the most recent; b is the oldest
    fetch(cache, "d", 64 * kib, calls);  // evicts b
    expect(cache.stats().evictions == 1, "one entry left for the fourth texture");
    const auto before = calls;
    fetch(cache, "a", 64 * kib, calls);
    fetch(cache, "c", 64 * kib, calls);
    fetch(cache, "d", 64 * kib, calls);
    expect(calls == before, "a, c and d are still held");
    fetch(cache, "b", 64 * kib, calls);
    expect(calls == before + 1 && cache.stats().redecodes == 1, "the evicted path decodes again and is counted");
}

void oversized_texture_is_held_until_the_next_request() {
    ComposeTextureCache cache(64 * kib);
    std::uint64_t calls = 0;
    const auto big = fetch(cache, "big", 256 * kib, calls);
    expect(big && cache.stats().held_bytes == 256 * kib, "a texture over the budget stays while it is the newest");
    fetch(cache, "big", 256 * kib, calls);
    expect(calls == 1, "and serves its next surface without a decode");
    fetch(cache, "small", 16 * kib, calls);
    expect(cache.stats().held_bytes == 16 * kib && cache.stats().evictions == 1, "the next request releases it");
    expect(big->mips.front().bytes.size() == 256 * kib, "a caller keeps a valid texture after the eviction");
}

void zero_budget_holds_only_the_newest() {
    ComposeTextureCache cache(0);
    std::uint64_t calls = 0;
    fetch(cache, "a", kib, calls);
    fetch(cache, "b", kib, calls);
    expect(cache.stats().held_bytes == kib && cache.stats().evictions == 1, "a zero budget keeps one entry");
}

void failed_loads_are_not_held() {
    ComposeTextureCache cache(mib);
    std::uint64_t calls = 0;
    const auto missing = cache.get("gone", [&]() -> std::optional<eawr::assets::Texture> {
        ++calls;
        return std::nullopt;
    });
    expect(!missing && cache.stats().decodes == 0 && cache.stats().held_bytes == 0, "a failed decode holds nothing");
    static_cast<void>(cache.get("gone", [&]() -> std::optional<eawr::assets::Texture> {
        ++calls;
        return std::nullopt;
    }));
    expect(calls == 2, "the next request tries again");
}

void budget_can_change_between_requests() {
    ComposeTextureCache cache;
    std::uint64_t calls = 0;
    fetch(cache, "a", 64 * kib, calls);
    fetch(cache, "b", 64 * kib, calls);
    cache.set_budget(64 * kib);
    fetch(cache, "c", 64 * kib, calls);
    expect(cache.stats().held_bytes == 64 * kib && cache.stats().evictions == 2, "a lower budget applies on the next request");
}

// #5989: the single-level BC3 texture of a MeshAdditive surface reaches the renderer's upload with a full chain, built by
// the read-ahead; anything else, and everything composed without a read-ahead, arrives as it was decoded.
namespace fs = std::filesystem;

struct LightTree final {
    fs::path root;
    LightTree() {
        root = fs::temp_directory_path()
            / ("eawr_compose_chain_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root / "art" / "textures");
    }
    ~LightTree() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    LightTree(const LightTree&) = delete;
    LightTree& operator=(const LightTree&) = delete;
    void write(const std::string& name, const std::vector<char>& bytes) const {
        std::ofstream file(root / "art" / "textures" / name, std::ios::binary);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
};

void ship_materials_choose_their_chain() {
    using eawr::presentation::godot_backend::texture_processing_for;
    using eawr::scene::TextureProcessing;
    std::size_t opaque = 0, transparent = 0;
    for (const auto& selector : eawr::scene::legacy_selectors()) {
        const bool additive = selector.program == "MeshAdditive.fx" || selector.program == "MeshAdditive2x.fx"
            || selector.program == "MeshAdditiveOffset.fx" || selector.program == "MeshAdditiveVColor.fx"
            || selector.program == "RSkinAdditive.fx";
        const auto want = additive ? TextureProcessing::compensated_light_mip_chain
            : selector.transparent ? TextureProcessing::none : TextureProcessing::colour_mip_chain;
        expect(texture_processing_for(selector) == want, "each material selects its chain: " + std::string(selector.program));
        (selector.transparent ? transparent : opaque) += 1;
    }
    expect(opaque > 0 && transparent > 0, "the selector table has both kinds");
    // Additive lights receive compensated chains by default.
    for (const char* program : {"MeshAdditive.fx", "MeshAdditive2x.fx", "MeshAdditiveOffset.fx", "MeshAdditiveVColor.fx", "RSkinAdditive.fx"}) {
        const auto* selector = eawr::scene::find_legacy_selector(program);
        if (selector != nullptr) expect(texture_processing_for(*selector) == TextureProcessing::compensated_light_mip_chain, std::string(program) + " asks for compensated chains");
    }
    const auto* hull = eawr::scene::find_legacy_selector("MeshGloss.fx");
    expect(hull != nullptr && texture_processing_for(*hull) == TextureProcessing::colour_mip_chain, "MeshGloss.fx asks for a chain");
    expect(ComposeTextureCache::key("data/a.dds", TextureProcessing::none) == "data/a.dds"
               && ComposeTextureCache::key("data/a.dds", TextureProcessing::colour_mip_chain) != "data/a.dds"
               && ComposeTextureCache::key("data/a.dds", TextureProcessing::normal_mip_chain) != "data/a.dds"
               && ComposeTextureCache::key("data/a.dds", TextureProcessing::normal_mip_chain)
                   != ComposeTextureCache::key("data/a.dds", TextureProcessing::colour_mip_chain),
           "the held key tells the three forms of one file apart");
}

void hull_textures_reach_the_renderer_with_a_full_chain() {
    using eawr::presentation::godot_backend::texture_processing_for;
    using eawr::scene::TextureProcessing;
    constexpr std::uint32_t side = 256;
    const auto colour = eawr::testing::single_level_bc3(side);
    const auto normal = eawr::testing::single_level_bc3_of(side, eawr::testing::normal_texels(side));
    LightTree tree;
    tree.write("hull.dds", eawr::testing::dds_bc3_bytes(colour));
    tree.write("hull_normal.dds", eawr::testing::dds_bc3_bytes(normal));
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "the fixture mounts");
    if (!mounted) return;
    const auto vfs = std::move(mounted).value();
    const std::string base_path = "data/art/textures/hull.dds";
    const std::string normal_path = "data/art/textures/hull_normal.dds";
    const auto* hull = eawr::scene::find_legacy_selector("MeshGloss.fx");
    const auto* light = eawr::scene::find_legacy_selector("MeshAdditive.fx");
    expect(hull != nullptr && light != nullptr, "both selectors exist");
    if (hull == nullptr || light == nullptr) return;
    const auto on_disk = eawr::assets::load_texture(vfs, base_path);
    expect(on_disk && on_disk.value().mips.size() == 1, "the fixture texture is single-level on disk");

    // The same calls upload_surface makes, with the read-ahead the initial compose starts.
    const auto upload_texture = [&](ComposeTextureCache& cache, const std::string& path, const TextureProcessing processing) {
        return cache.get(ComposeTextureCache::key(path, processing), [&]() -> std::optional<eawr::assets::Texture> {
            auto decoded = cache.decode(vfs, path, processing);
            return decoded ? std::optional<eawr::assets::Texture>(std::move(decoded.value())) : std::nullopt;
        });
    };
    {
        eawr::scene::TexturePrefetchOptions options;
        options.fixed_readers = 1;
        eawr::scene::TexturePrefetch prefetch(vfs, options);
        ComposeTextureCache cache;
        cache.set_prefetch(&prefetch);
        prefetch.offer(base_path, 0, texture_processing_for(*hull));
        prefetch.offer(normal_path, 0, TextureProcessing::normal_mip_chain);
        prefetch.offer(base_path, 1, texture_processing_for(*light));
        for (int i = 0; i < 500 && prefetch.stats().decoded < 3; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        expect(prefetch.stats().decoded == 3, "the reader decodes all three entries before the compose asks");
        const auto base = upload_texture(cache, base_path, texture_processing_for(*hull));
        expect(base && base->mips.size() == 9, "the single-level base texture of an opaque material reaches the upload with all nine levels");
        if (base && base->mips.size() == 9) {
            expect(base->mips[0].bytes == colour.mips[0].bytes, "its top level is the file's, byte for byte");
            expect(eawr::presentation::godot_backend::detail::texture_mip_tail_bytes(*base) == 0, "the renderer has no tail left to fill in");
            expect(base->mips.back().width == 1 && base->mips.back().height == 1, "the chain ends at 1 x 1");
        }
        const auto normals = upload_texture(cache, normal_path, TextureProcessing::normal_mip_chain);
        expect(normals && normals->mips.size() == 9, "its normal map reaches the upload with all nine levels");
        if (normals && normals->mips.size() == 9) {
            const auto coarse = eawr::assets::decode_blocks(eawr::assets::PixelFormat::bc3, normals->mips[4].bytes, normals->mips[4].width,
                                                            normals->mips[4].height);
            double length = 0.0;
            for (std::size_t i = 0; i + 3 < coarse.size(); i += 4) {
                double sum = 0.0;
                for (std::size_t c = 0; c < 3; ++c) sum += std::pow((static_cast<double>(coarse[i + c]) - 127.5) / 127.5, 2.0);
                length += std::sqrt(sum);
            }
            length /= static_cast<double>(coarse.size() / 4U);
            if (eawr::assets::renormalize_normal_chains) expect(length > 0.92 && length < 1.08, "and its coarse levels still hold unit normals (" + std::to_string(length) + ")");
            else expect(length < 0.9, "and its coarse levels are the plain box average, shorter than unit (" + std::to_string(length) + ")");
        }
        const auto lights = upload_texture(cache, base_path, texture_processing_for(*light));
        expect(lights && lights->mips.size() == 9 && lights->mips[0].bytes == colour.mips[0].bytes,
               "the same file under an additive effect receives a chain and keeps its authored level");
        prefetch.finish();
        const auto stats = prefetch.stats();
        expect(stats.chained == 3 && stats.chained_by_loader == 0, "all chains were built by a reader, not the loader");
        expect(stats.chain_added_bytes == 2 * eawr::assets::mip_chain_added_bytes(colour) + eawr::assets::mip_chain_added_bytes(normal),
               "and added the bytes the model predicts");
    }
    {
        // No read-ahead (a ship composed during the battle): as decoded, no generation on the loader.
        ComposeTextureCache cache;
        const auto base = upload_texture(cache, base_path, texture_processing_for(*hull));
        expect(base && base->mips.size() == 1, "without a read-ahead the texture is returned as decoded");
    }
}

} // namespace

// The read-ahead (and so the chains) must not be wired for the attract demo only: the choice takes no attract input, so a
// skirmish or replay battle gets it on the Separate render thread, and the A/B override can only switch it off.
void every_live_session_gets_the_read_ahead_on_the_separate_render_thread() {
    using eawr::presentation::godot_backend::choose_texture_prefetch;
    using eawr::presentation::godot_backend::population_uses_texture_prefetch;
    const std::vector<std::string> none;
    expect(choose_texture_prefetch(true, none).enabled, "a session without arguments gets the read-ahead on Separate");
    expect(choose_texture_prefetch(true, none).readers == 0U, "and lets the disk kind pick the reader count");
    expect(!choose_texture_prefetch(false, none).enabled, "the Safe render thread has none");
    const std::vector<std::string> off{"--eawr-live-session", "skirmish", "--eawr-texture-prefetch", "off"};
    expect(!choose_texture_prefetch(true, off).enabled, "the off override stays");
    const std::vector<std::string> on{"--eawr-texture-prefetch", "on"};
    expect(choose_texture_prefetch(true, on).enabled, "on keeps it on");
    expect(!choose_texture_prefetch(false, on).enabled, "on never beats the Safe render thread");
    const std::vector<std::string> readers{"--eawr-texture-readers", "3"};
    expect(choose_texture_prefetch(true, readers).enabled && choose_texture_prefetch(true, readers).readers == 3U,
        "a reader count fixes the readers and leaves the read-ahead on");
    const std::vector<std::string> negative{"--eawr-texture-readers", "-2"};
    expect(choose_texture_prefetch(true, negative).readers == 0U, "a negative reader count means automatic");
    const std::vector<std::string> trailing{"--eawr-texture-prefetch"};
    expect(choose_texture_prefetch(true, trailing).enabled, "a trailing flag without a value changes nothing");
    expect(population_uses_texture_prefetch(true, true), "a live session's population runs the read-ahead");
    expect(!population_uses_texture_prefetch(false, true), "a static populate map keeps plain textures");
    expect(!population_uses_texture_prefetch(true, false), "and nothing runs when the choice is off");
}

int main() {
    every_live_session_gets_the_read_ahead_on_the_separate_render_thread();
    ship_materials_choose_their_chain();
    hull_textures_reach_the_renderer_with_a_full_chain();
    unbounded_holds_everything();
    bounded_release_keeps_the_budget();
    least_recently_used_leaves_first();
    oversized_texture_is_held_until_the_next_request();
    zero_budget_holds_only_the_newest();
    failed_loads_are_not_held();
    budget_can_change_between_requests();
    if (failures != 0) {
        std::cerr << failures << " compose texture cache contract(s) failed\n";
        return 1;
    }
    std::cout << "compose texture cache contracts passed\n";
    return 0;
}
