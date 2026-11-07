#include "eawr/core/load_profile.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;
namespace lp = eawr::core::load_profile;

void expect(const bool condition, const std::string_view message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string reference_hash(std::span<const std::byte> bytes) {
    return eawr::sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

// The model hash comes from the bytes the decode already read: it equals the hash of the file, the file is read once,
// and a request that arrives before any decode still hashes through its own read.
void hash_from_decoded_bytes() {
    const auto root = std::filesystem::temp_directory_path()
        / ("eawr-model-sha-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto directory = root / "art" / "models";
    std::filesystem::create_directories(directory);
    const std::array<std::uint32_t, 11> words{
        0x200, 0x8000000cU, 0x201, 4, 0, 0x600, 0x80000014U, 0x601, 12, 0x00000401, 0x04040000};
    std::vector<std::byte> bytes;
    for (const auto word : words)
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::byte>((word >> shift) & 0xffU));
    bytes.insert(bytes.end(), 4, std::byte{});
    const auto write = [&](const char* name, std::span<const std::byte> data) {
        std::ofstream file(directory / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    };
    write("good.alo", bytes);
    write("other.alo", bytes);
    write("bad.alo", {});
    write("late.alo", bytes);
    const std::array mounts{eawr::vfs::MountSpec{"first", root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "fixture mounts");
    if (!mounted) { std::filesystem::remove_all(root); return; }
    const auto expected = reference_hash(bytes);
    const auto empty = reference_hash({});

    lp::Recording recording;
    lp::active = &recording;
    eawr::scene::VfsAssetCache cache(mounted.value());
    expect(cache.model("data/art/models/good.alo") != nullptr, "model decodes");
    const auto reused = cache.sha256("data/art/models/good.alo");
    const auto sha_bytes = recording.counters[static_cast<std::size_t>(lp::Counter::sha_bytes)];
    const auto reused_bytes = recording.counters[static_cast<std::size_t>(lp::Counter::sha_reused_bytes)];
    const auto read_bytes = recording.counters[static_cast<std::size_t>(lp::Counter::model_read_bytes)];
    lp::active = nullptr;
    expect(reused == expected, "the reused hash equals the hash of the file bytes");
    expect(sha_bytes == bytes.size() && reused_bytes == bytes.size() && read_bytes == bytes.size(),
           "the file is read once and hashed once");
    expect(recording.models.size() == 1 && recording.models.front().file_bytes == bytes.size()
               && recording.costs[static_cast<std::size_t>(lp::Phase::model_read)].calls == 1
               && recording.costs[static_cast<std::size_t>(lp::Phase::model_parse)].calls == 1,
           "read and parse are recorded apart");

    // A failed decode still hashes the bytes that were read, as the separate read did.
    expect(cache.model("data/art/models/bad.alo") == nullptr && cache.sha256("data/art/models/bad.alo") == empty,
           "a failed decode keeps the hash of its bytes");
    // Hash first, decode later: the fallback read gives the same value.
    eawr::scene::VfsAssetCache late(mounted.value());
    expect(late.sha256("data/art/models/other.alo") == expected, "a hash requested before any decode still matches");
    expect(late.sha256("data/art/models/absent.alo").empty(), "a missing file hashes to nothing");
    // A second wrapper of the same mount sees the decoded entry and the same hash.
    eawr::scene::VfsAssetCache again(mounted.value());
    expect(again.model("data/art/models/good.alo") != nullptr && again.sha256("data/art/models/good.alo") == expected,
           "another wrapper of the mount agrees");
    // Gameplay-phase decodes add no hash work: after the load bracket closes, the fallback read supplies it.
    cache.end_model_load(~std::uint64_t{});
    lp::Recording gameplay;
    lp::active = &gameplay;
    expect(cache.model("data/art/models/late.alo") != nullptr, "gameplay-phase decode works");
    const auto hashed_in_decode = gameplay.counters[static_cast<std::size_t>(lp::Counter::sha_reused_bytes)];
    lp::active = nullptr;
    expect(hashed_in_decode == 0, "a gameplay-phase decode does not hash");
    expect(cache.sha256("data/art/models/late.alo") == expected, "its hash still comes out the same");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

} // namespace

int main() {
    hash_from_decoded_bytes();
    return failures == 0 ? 0 : 1;
}
