#include "bc3_fixture.hpp"
#include "eawr/assets/assets.hpp"
#include "eawr/assets/texture_chain.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/scene/texture_prefetch.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using eawr::scene::TexturePrefetch;
using eawr::scene::TexturePrefetchOptions;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

template <class Predicate>
bool wait_until(Predicate&& done) {
    for (int i = 0; i < 500; ++i) { // 5 s
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

// An uncompressed 32-bit TGA of `side` x `side` pixels, every pixel `shade`.
std::vector<char> tga_bytes(const std::uint16_t side, const unsigned char shade) {
    std::vector<char> bytes(18U + static_cast<std::size_t>(side) * side * 4U, static_cast<char>(shade));
    for (std::size_t i = 0; i < 18; ++i) bytes[i] = 0;
    bytes[2] = 2;
    bytes[12] = static_cast<char>(side & 0xFFU);
    bytes[13] = static_cast<char>(side >> 8U);
    bytes[14] = static_cast<char>(side & 0xFFU);
    bytes[15] = static_cast<char>(side >> 8U);
    bytes[16] = 32;
    bytes[17] = 0x08;
    return bytes;
}

struct TempTree final {
    fs::path root;
    TempTree() {
        static std::atomic<unsigned> counter{};
        root = fs::temp_directory_path() / ("eawr_texture_prefetch_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                                            + "_" + std::to_string(counter++));
        fs::create_directories(root / "art" / "textures");
    }
    ~TempTree() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
    void write(const std::string& name, const std::vector<char>& bytes) const {
        std::ofstream file(root / "art" / "textures" / name, std::ios::binary);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
};

std::string path_of(const int index) { return "data/art/textures/t" + std::to_string(index) + ".tga"; }

// `count` textures; sizes grow with the index so each differs in bytes and in content.
void write_textures(const TempTree& tree, const int count) {
    for (int i = 0; i < count; ++i)
        tree.write("t" + std::to_string(i) + ".tga", tga_bytes(static_cast<std::uint16_t>(8 + 4 * (i % 5)), static_cast<unsigned char>(10 + i)));
}

eawr::vfs::Vfs mount(const TempTree& tree) {
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    if (!mounted) {
        std::cerr << "FAIL: fixture does not mount\n";
        std::exit(2);
    }
    return std::move(mounted).value();
}

bool same_texture(const eawr::assets::Texture& left, const eawr::assets::Texture& right) {
    if (left.width != right.width || left.height != right.height || left.format != right.format || left.mips.size() != right.mips.size()
        || left.source.logical_path != right.source.logical_path) return false;
    for (std::size_t i = 0; i < left.mips.size(); ++i) {
        if (left.mips[i].bytes != right.mips[i].bytes) return false;
    }
    return true;
}

// A gate the decode hook waits at, so a test decides when a reader may finish.
struct Gate final {
    std::mutex mutex;
    std::condition_variable changed;
    bool open{};
    std::atomic<int> entered{};
    void wait() {
        ++entered;
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return open; });
    }
    void release() {
        {
            std::lock_guard lock(mutex);
            open = true;
        }
        changed.notify_all();
    }
};

void reader_policy() {
    expect(TexturePrefetch::policy_readers(false, 16) == 1U, "a spinning or unknown volume gets one reader");
    expect(TexturePrefetch::policy_readers(true, 0) == 1U && TexturePrefetch::policy_readers(true, 2) == 1U, "tiny machines get one reader");
    expect(TexturePrefetch::policy_readers(true, 4) == 2U && TexturePrefetch::policy_readers(true, 12) == 8U
               && TexturePrefetch::policy_readers(true, 64) == 8U,
           "solid state gets clamp(cores - 2, 1, 8) readers");
}

// The loader gets what assets::load_texture gives, in the loader's own order, whatever the readers did first.
void results_equal_plain_decode_in_loader_order() {
    TempTree tree;
    write_textures(tree, 24);
    const auto vfs = mount(tree);
    for (const unsigned readers : {1U, 2U, 4U}) {
        const auto label = " (readers " + std::to_string(readers) + ")";
        TexturePrefetchOptions options;
        options.fixed_readers = readers;
        TexturePrefetch prefetch(vfs, options);
        for (int i = 0; i < 24; ++i) prefetch.offer(path_of(i), static_cast<std::uint64_t>(i));
        prefetch.offer(path_of(3), 99); // a duplicate is ignored
        prefetch.offer("DATA/ART/TEXTURES/T3.TGA", 99); // so is a case variant
        prefetch.offer("data/art/textures/missing.tga", 5); // a missing file stays the loader's
        bool all_equal = true;
        for (int i = 0; i < 24; ++i) {
            auto got = prefetch.acquire(path_of(i));
            auto plain = eawr::assets::load_texture(vfs, path_of(i));
            all_equal = all_equal && got && plain && same_texture(got.value(), plain.value());
            prefetch.advance(static_cast<std::uint64_t>(i + 1));
        }
        expect(all_equal, "every acquired texture equals the plain decode" + label);
        prefetch.finish();
        const auto stats = prefetch.stats();
        expect(stats.planned == 24, "the plan holds the distinct existing files" + label);
        expect(stats.served_ready + stats.served_inflight + stats.served_unstarted == 24 && stats.unplanned == 0,
               "every ask was planned and served once" + label);
        expect(stats.readers == readers, "reader count as asked" + label);
        expect(prefetch.held_count() == 0 && prefetch.charged_bytes() == 0, "a finished prefetch holds nothing" + label);
        // A texture asked for again is not served twice from the store: it is decoded by the caller.
        auto again = prefetch.acquire(path_of(0));
        expect(static_cast<bool>(again), "a repeat ask decodes on the caller" + label);
    }
}

// Held plus in-flight bytes stay inside the window, readers stop when it is full and go on as the loader takes textures.
void window_is_bounded() {
    TempTree tree;
    write_textures(tree, 40);
    const auto vfs = mount(tree);
    TexturePrefetchOptions options;
    options.fixed_readers = 3;
    options.window_bytes = 6U << 10U; // a few of the 0.3-2.3 KiB textures at a time
    TexturePrefetch prefetch(vfs, options);
    for (int i = 0; i < 40; ++i) prefetch.offer(path_of(i), static_cast<std::uint64_t>(i));
    expect(wait_until([&] { return prefetch.stats().decoded >= 3; }), "readers start decoding");
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // let the window fill and the readers stall
    const auto stalled = prefetch.stats();
    expect(stalled.decoded < 40, "the window stops the readers before the whole plan is decoded");
    expect(prefetch.charged_bytes() <= options.window_bytes, "charged bytes stay inside the window");
    for (int i = 0; i < 40; ++i) {
        auto got = prefetch.acquire(path_of(i));
        expect(static_cast<bool>(got), "every texture is served through a full window");
    }
    prefetch.finish();
    const auto stats = prefetch.stats();
    expect(stats.held_bytes_peak <= options.window_bytes, "the peak of held and in-flight bytes never passed the window");
    expect(stats.served_ready + stats.served_inflight + stats.served_unstarted == 40, "all asks were served");
}

// A texture bigger than the window still runs, alone.
void oversized_texture_runs_alone() {
    TempTree tree;
    tree.write("big.tga", tga_bytes(64, 7));
    tree.write("small.tga", tga_bytes(8, 9));
    const auto vfs = mount(tree);
    TexturePrefetchOptions options;
    options.fixed_readers = 2;
    options.window_bytes = 1024; // smaller than the 16 KiB texture
    TexturePrefetch prefetch(vfs, options);
    prefetch.offer("data/art/textures/big.tga", 0);
    prefetch.offer("data/art/textures/small.tga", 1);
    expect(wait_until([&] { return prefetch.stats().decoded >= 1; }), "an oversized texture is decoded");
    auto big = prefetch.acquire("data/art/textures/big.tga");
    auto small = prefetch.acquire("data/art/textures/small.tga");
    expect(big && small, "both are served");
    prefetch.finish();
}

// A decode that fails on a reader (or a file that is gone) is the loader's own failure, with the loader's own diagnostic.
void failures_are_the_loaders() {
    TempTree tree;
    tree.write("bad.tga", std::vector<char>(40, static_cast<char>(0x55))); // not a texture
    tree.write("good.tga", tga_bytes(8, 3));
    const auto vfs = mount(tree);
    TexturePrefetchOptions options;
    options.fixed_readers = 2;
    TexturePrefetch prefetch(vfs, options);
    prefetch.offer("data/art/textures/bad.tga", 0);
    prefetch.offer("data/art/textures/good.tga", 1);
    expect(wait_until([&] { return prefetch.stats().decoded >= 2; }), "both are attempted");
    auto bad = prefetch.acquire("data/art/textures/bad.tga");
    auto bad_plain = eawr::assets::load_texture(vfs, "data/art/textures/bad.tga");
    expect(!bad && !bad_plain, "a corrupt texture fails through the prefetch");
    if (!bad && !bad_plain) {
        expect(eawr::core::format_diagnostic(bad.error()) == eawr::core::format_diagnostic(bad_plain.error()),
               "the failure is the plain decode's, text for text");
    }
    auto good = prefetch.acquire("data/art/textures/good.tga");
    expect(static_cast<bool>(good), "a good texture next to a failed one is served");
    auto missing = prefetch.acquire("data/art/textures/never_offered.tga");
    auto missing_plain = eawr::assets::load_texture(vfs, "data/art/textures/never_offered.tga");
    expect(!missing && !missing_plain
               && eawr::core::format_diagnostic(missing.error()) == eawr::core::format_diagnostic(missing_plain.error()),
           "a path nobody offered reports the plain decode's failure");
    prefetch.finish();
    const auto stats = prefetch.stats();
    expect(stats.failed == 1, "the reader counted its failed decode");
    expect(stats.unplanned == 1, "the unplanned ask is counted");
}

// The loader moving past a step drops the hints of the steps before it.
void advance_drops_dead_hints() {
    TempTree tree;
    write_textures(tree, 12);
    const auto vfs = mount(tree);
    TexturePrefetchOptions options;
    options.fixed_readers = 2;
    options.window_bytes = 64U << 20U;
    TexturePrefetch prefetch(vfs, options);
    for (int i = 0; i < 12; ++i) prefetch.offer(path_of(i), static_cast<std::uint64_t>(i));
    expect(wait_until([&] { return prefetch.stats().decoded == 12; }), "a roomy window decodes the whole plan");
    expect(prefetch.held_count() == 12, "all twelve are held");
    prefetch.advance(5); // hints 0..4 were never asked for
    expect(prefetch.held_count() == 7, "advance frees the held decodes of earlier steps");
    auto kept = prefetch.acquire(path_of(7));
    expect(static_cast<bool>(kept), "a later hint is still served");
    auto dropped = prefetch.acquire(path_of(2));
    auto plain = eawr::assets::load_texture(vfs, path_of(2));
    expect(dropped && plain && same_texture(dropped.value(), plain.value()), "a dropped hint is decoded by the caller, same result");
    prefetch.finish();
    const auto stats = prefetch.stats();
    expect(stats.skipped == 5 && stats.unrequested >= 5, "five hints were skipped and counted as unrequested");
}

// The loader asking for a path a reader is decoding waits for it and gets the reader's result.
void inflight_ask_waits() {
    TempTree tree;
    write_textures(tree, 4);
    const auto vfs = mount(tree);
    Gate gate;
    TexturePrefetchOptions options;
    options.fixed_readers = 1;
    options.decode_hook = [&](std::string_view) { gate.wait(); };
    TexturePrefetch prefetch(vfs, options);
    prefetch.offer(path_of(0), 0);
    expect(wait_until([&] { return gate.entered.load() >= 1; }), "the reader is inside its decode");
    std::optional<eawr::core::Result<eawr::assets::Texture>> got;
    std::thread asker([&] { got.emplace(prefetch.acquire(path_of(0))); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    expect(!got.has_value(), "the ask waits while the reader decodes");
    gate.release();
    asker.join();
    auto plain = eawr::assets::load_texture(vfs, path_of(0));
    expect(got.has_value() && *got && plain && same_texture(got->value(), plain.value()), "the waiting ask gets the decode");
    prefetch.finish();
    const auto stats = prefetch.stats();
    expect(stats.served_inflight == 1 && stats.served_unstarted == 0, "the ask counted as served in flight");
}

// finish() with readers mid-decode and a queue behind them: joins, frees, and the loader still works; destroying a
// prefetch with queued hints does not hang.
void cancel_and_shutdown() {
    TempTree tree;
    write_textures(tree, 20);
    const auto vfs = mount(tree);
    {
        Gate gate;
        TexturePrefetchOptions options;
        options.fixed_readers = 2;
        options.decode_hook = [&](std::string_view) { gate.wait(); };
        TexturePrefetch prefetch(vfs, options);
        for (int i = 0; i < 20; ++i) prefetch.offer(path_of(i), static_cast<std::uint64_t>(i));
        expect(wait_until([&] { return gate.entered.load() >= 2; }), "both readers are inside a decode");
        std::thread stopper([&] { prefetch.finish(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        gate.release(); // each reader finishes at most its current file
        stopper.join();
        const auto stats = prefetch.stats();
        expect(stats.decoded <= 2, "no reader started another file after the stop");
        expect(prefetch.held_count() == 0 && prefetch.charged_bytes() == 0, "a stopped prefetch holds nothing");
        prefetch.offer(path_of(1), 1);
        expect(prefetch.stats().planned == 20, "hints after the stop are ignored");
        auto got = prefetch.acquire(path_of(5));
        expect(static_cast<bool>(got), "the loader still gets its texture after the stop");
        prefetch.finish(); // idempotent
    }
    {
        TexturePrefetchOptions options;
        options.fixed_readers = 3;
        TexturePrefetch prefetch(vfs, options);
        for (int i = 0; i < 20; ++i) prefetch.offer(path_of(i), static_cast<std::uint64_t>(i));
    } // destroyed with work queued
    {
        // A stop while the loader waits for an in-flight decode releases the loader, which decodes the path itself.
        Gate gate;
        TexturePrefetchOptions options;
        options.fixed_readers = 1;
        options.decode_hook = [&](std::string_view) { gate.wait(); };
        TexturePrefetch prefetch(vfs, options);
        prefetch.offer(path_of(0), 0);
        expect(wait_until([&] { return gate.entered.load() >= 1; }), "the reader is inside its decode (cancel while waiting)");
        std::optional<eawr::core::Result<eawr::assets::Texture>> got;
        std::thread asker([&] { got.emplace(prefetch.acquire(path_of(0))); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::thread stopper([&] { prefetch.finish(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        gate.release();
        stopper.join();
        asker.join();
        auto plain = eawr::assets::load_texture(vfs, path_of(0));
        expect(got.has_value() && *got && plain && same_texture(got->value(), plain.value()), "the waiting ask is served when the prefetch stops");
    }
}

// #5989: a reader completes the mip chain of a single-level BC3 texture when the offer asks for it, and only then.
void mip_chain_is_built_by_a_reader_when_asked() {
    using eawr::scene::TextureProcessing;
    TempTree tree;
    const auto single = eawr::testing::single_level_bc3(64);
    auto authored = single;
    expect(eawr::assets::extend_mip_chain(authored, eawr::assets::ChainKind::colour), "setup: an authored chain exists");
    tree.write("light.dds", eawr::testing::dds_bc3_bytes(single));
    tree.write("authored.dds", eawr::testing::dds_bc3_bytes(authored));
    {
        // An 8-bit grey TGA (image type 3): a format the chain generator does not handle.
        std::vector<char> grey(18U + 64U, 0);
        grey[2] = 3;
        grey[12] = 8;
        grey[14] = 8;
        grey[16] = 8;
        grey[17] = 0x20;
        tree.write("flat.tga", grey);
    }
    const auto vfs = mount(tree);
    const std::string light = "data/art/textures/light.dds";
    const std::string full = "data/art/textures/authored.dds";
    const std::string flat = "data/art/textures/flat.tga";

    TexturePrefetchOptions options;
    options.fixed_readers = 2;
    TexturePrefetch prefetch(vfs, options);
    prefetch.offer(light, 0, TextureProcessing::colour_mip_chain);
    prefetch.offer(full, 1, TextureProcessing::colour_mip_chain);
    prefetch.offer(flat, 2, TextureProcessing::colour_mip_chain);
    prefetch.offer(light, 3); // the same file wanted plain by another effect: a second entry
    expect(wait_until([&] { return prefetch.stats().decoded == 4; }), "the readers decode all four entries");

    auto expected = eawr::assets::load_texture(vfs, light);
    expect(static_cast<bool>(expected) && eawr::assets::extend_mip_chain(expected.value(), eawr::assets::ChainKind::colour), "setup: the expected chain exists");
    const auto chained = prefetch.acquire(light, TextureProcessing::colour_mip_chain);
    expect(chained && expected && chained.value().mips.size() == 7 && same_texture(chained.value(), expected.value()),
           "a single-level BC3 texture arrives with its full chain, as the generator makes it");
    const auto plain = prefetch.acquire(light);
    const auto plain_decode = eawr::assets::load_texture(vfs, light);
    expect(plain && plain_decode && same_texture(plain.value(), plain_decode.value()) && plain.value().mips.size() == 1,
           "asked plain, the same file is untouched");
    const auto kept = prefetch.acquire(full, TextureProcessing::colour_mip_chain);
    const auto kept_decode = eawr::assets::load_texture(vfs, full);
    expect(kept && kept_decode && same_texture(kept.value(), kept_decode.value()), "an authored chain is untouched");
    const auto other = prefetch.acquire(flat, TextureProcessing::colour_mip_chain);
    const auto other_decode = eawr::assets::load_texture(vfs, flat);
    expect(other && other_decode && same_texture(other.value(), other_decode.value()), "a texture of a format the generator does not handle is untouched");

    auto stats = prefetch.stats();
    expect(stats.chained == 1 && stats.chained_by_loader == 0, "one chain, built by a reader");
    expect(stats.chain_added_bytes == eawr::assets::mip_chain_added_bytes(single), "the added bytes are the model's");

    // A hint that was dropped or never offered: the loader decodes and chains it itself, and it is counted.
    const auto late = prefetch.acquire(light, TextureProcessing::colour_mip_chain);
    expect(late && expected && same_texture(late.value(), expected.value()), "an ask the readers did not cover still gets the chain");
    prefetch.finish();
    stats = prefetch.stats();
    expect(stats.chained == 1 && stats.chained_by_loader == 1, "the loader's own chain is counted apart");
}

} // namespace

int main() {
    mip_chain_is_built_by_a_reader_when_asked();
    reader_policy();
    results_equal_plain_decode_in_loader_order();
    window_is_bounded();
    oversized_texture_runs_alone();
    failures_are_the_loaders();
    advance_drops_dead_hints();
    inflight_ask_waits();
    cancel_and_shutdown();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "texture prefetch tests passed\n";
    return 0;
}
