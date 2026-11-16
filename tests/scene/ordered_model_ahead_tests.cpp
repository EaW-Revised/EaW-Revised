#include "eawr/scene/ordered_model_ahead.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int failures = 0;
namespace scene = eawr::scene;

void expect(const bool condition, const std::string_view message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string reference_hash(std::span<const std::byte> bytes) {
    return eawr::sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

struct Listener final : scene::OrderedModelAheadListener {
    std::mutex mutex;
    std::vector<std::size_t> indexes;
    std::vector<std::thread::id> threads;
    void model_ready(const std::size_t index, const std::string&, const scene::AheadModel&) noexcept override {
        std::lock_guard lock(mutex);
        indexes.push_back(index);
        threads.push_back(std::this_thread::get_id());
    }
};

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path()
        / ("eawr-ordered-model-ahead-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto directory = root / "art" / "models";
    std::filesystem::create_directories(directory);
    const std::array<std::uint32_t, 11> words{
        0x200, 0x8000000cU, 0x201, 4, 0, 0x600, 0x80000014U, 0x601, 12, 0x00000401, 0x04040000};
    std::vector<std::byte> bytes;
    for (const auto word : words)
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::byte>((word >> shift) & 0xffU));
    bytes.insert(bytes.end(), 4, std::byte{});
    const auto write = [&](const char* name, std::span<const std::byte> data) {
        std::ofstream file(directory / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    };
    for (const char* name : {"a.alo", "b.alo", "c.alo", "d.alo"}) write(name, bytes);
    write("bad.alo", {});
    const std::array mounts{eawr::vfs::MountSpec{"first", root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "fixture mounts");
    if (!mounted) { std::filesystem::remove_all(root); return 1; }
    const auto& files = mounted.value();
    const auto expected = reference_hash(bytes);
    const auto path = [](const char* name) { return std::string("data/art/models/") + name; };

    // One read: the hash is of the bytes that were decoded; a failed decode keeps the hash of what was read; a missing file has none.
    {
        const auto good = scene::read_hash_decode_model(files, path("a.alo"));
        expect(static_cast<bool>(good.model) && good.sha256 == expected, "a model decodes and carries the hash of its file bytes");
        const auto bad = scene::read_hash_decode_model(files, path("bad.alo"));
        expect(!bad.model && bad.sha256 == reference_hash({}), "a failed decode keeps the hash of the bytes that were read");
        const auto missing = scene::read_hash_decode_model(files, path("missing.alo"));
        expect(!missing.model && missing.sha256.empty(), "a missing file fails with no hash");
    }

    // In order on one worker thread, the listener before the loader can ask, and every answer equal to the inline decode.
    for (const std::uint64_t window : {std::uint64_t{1} << 30U, std::uint64_t{1}}) {
        Listener listener;
        scene::OrderedModelAhead ahead(files, {path("a.alo"), path("b.alo"), path("bad.alo"), path("missing.alo"), path("c.alo"), path("a.alo")},
            &listener, window);
        expect(ahead.stats().planned == 5, "a repeated path is planned once");
        const auto before = ahead.take(path("a.alo"));
        expect(static_cast<bool>(before.model) && before.sha256 == expected, "take before start decodes inline");
        ahead.start();
        ahead.start();
        for (const char* name : {"b.alo", "bad.alo", "missing.alo", "c.alo"}) {
            const auto got = ahead.take(path(name));
            const auto inline_result = scene::read_hash_decode_model(files, path(name));
            expect(static_cast<bool>(got.model) == static_cast<bool>(inline_result.model) && got.sha256 == inline_result.sha256,
                std::string("ordered take equals the inline decode of ") + name);
        }
        const auto unplanned = ahead.take(path("d.alo"));
        expect(static_cast<bool>(unplanned.model) && unplanned.sha256 == expected, "an unplanned path is decoded inline");
        ahead.finish();
        ahead.finish();
        const auto stats = ahead.stats();
        expect(stats.served_inline >= 2, "the unstarted take and the unplanned take were served inline");
        {
            std::lock_guard lock(listener.mutex);
            bool ascending = true;
            for (std::size_t i = 1; i < listener.indexes.size(); ++i) ascending = ascending && listener.indexes[i - 1] < listener.indexes[i];
            expect(ascending, "the worker decodes in list order");
            bool off_thread = true;
            for (const auto id : listener.threads) off_thread = off_thread && id != std::this_thread::get_id();
            expect(off_thread, "the listener runs on the worker thread");
        }
    }

    // A loader that does not drain the window in order (it never asks for the earlier models) is never blocked by a held worker.
    {
        scene::OrderedModelAhead ahead(files, {path("a.alo"), path("b.alo"), path("c.alo"), path("d.alo")}, nullptr, 1);
        ahead.start();
        const auto last = ahead.take(path("d.alo"));
        const auto first = ahead.take(path("a.alo"));
        expect(static_cast<bool>(last.model) && static_cast<bool>(first.model) && last.sha256 == expected && first.sha256 == expected,
            "a loader asking out of order gets every model");
        ahead.finish();
    }

    // Destroying a started instance with models nobody took joins its worker and drops them.
    {
        scene::OrderedModelAhead ahead(files, {path("a.alo"), path("b.alo")}, nullptr, std::uint64_t{1} << 30U);
        ahead.start();
    }

    std::filesystem::remove_all(root);
    if (failures != 0) { std::cerr << failures << " failure(s)\n"; return 1; }
    std::cout << "ordered_model_ahead_tests passed\n";
    return 0;
}
