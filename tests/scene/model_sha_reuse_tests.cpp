#include "eawr/core/load_profile.hpp"
#include "eawr/core/prepared_content.hpp"
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
#include <memory>
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

// The compose of each arriving slot makes a fresh wrapper of the same mount: the file hash is computed once for the mount,
// not once per wrapper; a file that is not there is never remembered.
void hash_shared_across_wrappers() {
    const auto root = std::filesystem::temp_directory_path()
        / ("eawr-model-sha-shared-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto directory = root / "art" / "models";
    std::filesystem::create_directories(directory);
    const std::array<std::byte, 6> bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6}};
    {
        std::ofstream file(directory / "ship.alo", std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const std::array mounts{eawr::vfs::MountSpec{"first", root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "shared-hash fixture mounts");
    if (!mounted) { std::filesystem::remove_all(root); return; }
    const auto expected = reference_hash(bytes);
    const auto sha_bytes = [](const lp::Recording& recording) {
        return recording.counters[static_cast<std::size_t>(lp::Counter::sha_bytes)];
    };

    lp::Recording first;
    lp::active = &first;
    eawr::scene::VfsAssetCache one(mounted.value());
    const auto hashed = one.sha256("data/art/models/ship.alo");
    lp::active = nullptr;
    expect(hashed == expected && sha_bytes(first) == bytes.size(), "the first wrapper reads and hashes the file once");

    lp::Recording second;
    lp::active = &second;
    eawr::scene::VfsAssetCache two(mounted.value());
    const auto again = two.sha256("data/art/models/ship.alo");
    const auto third = two.sha256("data/art/models/ship.alo");
    lp::active = nullptr;
    expect(again == expected && third == expected, "another wrapper of the mount gets the same hash");
    expect(sha_bytes(second) == 0 && second.costs[static_cast<std::size_t>(lp::Phase::model_sha256)].calls == 0,
           "and does not read or hash the file again");

    // A missing file hashes to nothing and is not remembered: once it exists, a wrapper hashes it.
    eawr::scene::VfsAssetCache three(mounted.value());
    expect(three.sha256("data/art/models/late.alo").empty(), "a missing file hashes to nothing");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

// The memo is per mount and is not a way round a verified view's content contract.
void hash_memo_respects_mount_and_contract() {
    const auto root = std::filesystem::temp_directory_path()
        / ("eawr-model-sha-contract-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto directory = root / "art" / "models";
    std::filesystem::create_directories(directory);
    const auto write = [&](const char* name, const std::vector<std::byte>& data) {
        std::ofstream file(directory / name, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    };
    const std::vector<std::byte> x{std::byte{1}, std::byte{2}, std::byte{3}};
    const std::vector<std::byte> y{std::byte{9}, std::byte{8}, std::byte{7}, std::byte{6}};
    write("a.alo", x);
    write("b.alo", x);
    const std::array mounts{eawr::vfs::MountSpec{"first", root, "data", {}}};

    // Same path, different bytes, a fresh mount: the new mount does not see the old mount's hash.
    {
        auto first = eawr::vfs::Vfs::mount(mounts);
        expect(static_cast<bool>(first), "contract fixture mounts");
        if (!first) { std::filesystem::remove_all(root); return; }
        eawr::scene::VfsAssetCache cache(first.value());
        expect(cache.sha256("data/art/models/a.alo") == reference_hash(x), "the first mount hashes the first bytes");
    }
    write("a.alo", y);
    auto remount = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(remount), "remount works");
    if (!remount) { std::filesystem::remove_all(root); return; }
    {
        eawr::scene::VfsAssetCache cache(remount.value());
        expect(cache.sha256("data/art/models/a.alo") == reference_hash(y), "a remount hashes the new bytes, not the old mount's memo");
    }

    // A sealed preparation view keeps its own read: a file another wrapper of the shared store already hashed is still
    // refused outside the agreed closure, and a file inside it still hashes.
    auto source = std::make_shared<eawr::vfs::Vfs>(std::move(remount.value()));
    {
        eawr::scene::VfsAssetCache unrestricted(*source);
        expect(unrestricted.sha256("data/art/models/b.alo") == reference_hash(x), "an unrestricted wrapper hashes b");
        expect(unrestricted.sha256("data/art/models/a.alo") == reference_hash(y), "an unrestricted wrapper hashes a");
    }
    auto view = eawr::vfs::Vfs::preparation_view(source, {});
    expect(static_cast<bool>(view), "preparation view opens");
    if (view) {
        expect(static_cast<bool>(view.value().open("data/art/models/a.alo")), "the view reads a before the seal");
        expect(static_cast<bool>(view.value().seal_content()), "the view seals with a in its closure");
        eawr::scene::VfsAssetCache viewed(view.value());
        expect(viewed.sha256("data/art/models/b.alo").empty(), "a sealed view does not hash a file outside its closure, memo or not");
        expect(viewed.sha256("data/art/models/a.alo") == reference_hash(y), "a sealed view hashes a file inside its closure");
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

} // namespace

// Scope::mark: the table at the mark plus the share of each open scope already spent, so costs - at_mark is what ran after it.
void phase_mark_contract() {
    lp::Recording recording;
    lp::active = &recording;
    const auto spin = [](const double ms) {
        const auto until = lp::Clock::now() + std::chrono::duration<double, std::milli>(ms);
        while (lp::Clock::now() < until) {}
    };
    {
        lp::Scope outer(lp::Phase::scene_population);
        spin(8.0);
        {
            lp::Scope done(lp::Phase::model_read);
            spin(4.0);
        }
        lp::Scope inner(lp::Phase::compose_hull);
        spin(6.0);
        lp::Scope::mark(recording);
        spin(10.0);
    }
    lp::active = nullptr;
    const auto after = [&](const lp::Phase phase) {
        const auto index = static_cast<std::size_t>(phase);
        return std::array<double, 3>{recording.costs[index].exclusive_ms - recording.at_mark[index].exclusive_ms,
                                     recording.costs[index].inclusive_ms - recording.at_mark[index].inclusive_ms,
                                     static_cast<double>(recording.costs[index].calls - recording.at_mark[index].calls)};
    };
    expect(recording.marked, "a mark is recorded");
    const auto inner = after(lp::Phase::compose_hull);
    expect(inner[0] >= 9.5 && inner[0] < 13.0 && inner[1] >= 9.5 && inner[1] < 13.0 && inner[2] == 1.0,
           "the inner scope's share after the mark is the 10 ms spun after it, one call closed after the mark");
    const auto outer = after(lp::Phase::scene_population);
    expect(outer[0] > -0.5 && outer[0] < 1.5 && outer[1] >= 9.5 && outer[1] < 13.0 && outer[2] == 1.0,
           "the outer scope's own time after the mark is nothing (the 10 ms ran in its child); its inclusive time is the 10 ms");
    const auto closed = after(lp::Phase::model_read);
    expect(closed[0] == 0.0 && closed[1] == 0.0 && closed[2] == 0.0, "a scope closed before the mark adds nothing after it");
}

int main() {
    phase_mark_contract();
    hash_from_decoded_bytes();
    hash_shared_across_wrappers();
    hash_memo_respects_mount_and_contract();
    return failures == 0 ? 0 : 1;
}
