// Canonical imported-scene evidence over the wholly synthetic P1-11 fixture.
#include "eawr/assets/map.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options final {
    std::filesystem::path fixture_root;
    std::filesystem::path output;
    std::string target;
    std::string build_id;
    std::string fixture_sha256;
    std::size_t workers{};
    bool self_test{};
};

bool hex(const std::string_view value, const std::size_t length) {
    return value.size() == length && std::all_of(value.begin(), value.end(), [](const char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool parse(const int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view flag = argv[index];
        if (flag == "--self-test") { options.self_test = true; continue; }
        if (index + 1 == argc) return false;
        const std::string value = argv[++index];
        if (flag == "--fixture-root") options.fixture_root = value;
        else if (flag == "--output") options.output = value;
        else if (flag == "--target") options.target = value;
        else if (flag == "--build-id") options.build_id = value;
        else if (flag == "--fixture-sha256") options.fixture_sha256 = value;
        else if (flag == "--workers") {
            if (value != "1" && value != "2" && value != "4") return false;
            options.workers = static_cast<std::size_t>(std::stoi(value));
        }
        else return false;
    }
    constexpr std::array<std::string_view, 6> targets{
        "windows-msvc", "windows-clang", "linux-x64-gcc", "linux-x64-clang", "linux-arm64-gcc", "local-test"};
    return !options.fixture_root.empty() && !options.output.empty()
        && std::find(targets.begin(), targets.end(), options.target) != targets.end()
        && hex(options.build_id, 40) && hex(options.fixture_sha256, 64)
        && options.workers != 0;
}

std::string digest(const std::span<const std::byte> bytes) {
    return eawr::sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

bool write(const std::filesystem::path& path, const std::string_view text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(file);
}

bool verify(const eawr::scene::Scene& scene, const eawr::scene::BuildInput& input,
            const eawr::assets::Map& source_map) {
    using eawr::sim::math::Fixed;
    if (scene.placements.size() != 7 || scene.assets.size() != 4
        || scene.resolved_count() != 3 || scene.drawable_count() != 4
        || scene.instances().size() != 4) return false;
    for (std::size_t index = 0; index < scene.placements.size(); ++index) {
        if (scene.placements[index].record_ordinal != index
            || scene.placements[index].entity_id != index + 1) return false;
    }
    for (std::size_t index = 0; index < scene.assets.size(); ++index) {
        if (scene.assets[index].asset_id != index + 1) return false;
        if (index > 0 && scene.assets[index - 1].logical_path >= scene.assets[index].logical_path) return false;
    }
    const auto& first = scene.placements[0];
    const auto& second = scene.placements[1];
    if (!first.transform || !second.transform
        || first.transform->position_raw[0] != 160 * Fixed::scale
        || second.transform->yaw_degrees_raw != 45 * Fixed::scale
        || first.scale_raw != 3 * (Fixed::scale / 2)) return false;
    if (eawr::scene::fixed_from_binary32(3.0F / 33554432.0F).value().raw() != 2) return false;
    if (eawr::scene::build(input).scene_sha256 != scene.scene_sha256) return false;
    eawr::assets::Map reordered = source_map;
    std::reverse(reordered.placements.begin(), reordered.placements.end());
    auto changed = input;
    changed.map = &reordered;
    if (eawr::scene::build(changed).scene_sha256 != scene.scene_sha256) return false;
    eawr::assets::Map moved = source_map;
    moved.placements.front().position->x += 1.0F;
    changed.map = &moved;
    if (eawr::scene::build(changed).scene_sha256 == scene.scene_sha256) return false;
    changed = input;
    const auto exists = input.access.exists;
    changed.access.exists = [exists](const std::string_view path) {
        return path != "data/art/models/eawr_scene_tower.alo" && exists(path);
    };
    return eawr::scene::build(changed).scene_sha256 != scene.scene_sha256;
}

} // namespace

int main(const int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        std::cerr << "invalid scene evidence arguments\n";
        return 2;
    }
    const std::array mounts{eawr::vfs::MountSpec{"synthetic", options.fixture_root / "GameData" / "Data", "data", {}}};
    auto filesystem = eawr::vfs::Vfs::mount(mounts);
    if (!filesystem) { std::cerr << "synthetic fixture mount failed\n"; return 1; }
    auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::eaw);
    if (!catalog) { std::cerr << "synthetic catalog failed\n"; return 1; }
    const auto types = eawr::assets::object_type_catalog(catalog.value().catalog);
    constexpr std::string_view map_path = "data/art/maps/eawr_scene_synthetic.ted";
    auto bytes = filesystem.value().open(map_path);
    auto map = eawr::assets::load_map(filesystem.value(), map_path, types);
    if (!bytes || !map) { std::cerr << "synthetic map failed\n"; return 1; }
    eawr::scene::VfsAssetCache cache(filesystem.value());
    eawr::scene::BuildInput input;
    input.map = &map.value();
    input.map_sha256 = digest(bytes.value());
    input.catalog = &catalog.value().catalog;
    input.access = cache.access();
    const eawr::platform::ThreadWorkerAdapter executor(options.workers);
    auto built = eawr::scene::build(input, executor);
    if (!built) { std::cerr << "parallel scene build failed: " << built.error().message << '\n'; return 1; }
    const auto& scene = built.value().scene;
    const auto& stats = built.value().execution;
    std::vector<std::size_t> expected_counts(options.workers);
    for (std::size_t index = 0; index < 7; ++index) ++expected_counts[index % options.workers];
    if (stats.workers_requested != options.workers || stats.partitions_completed != options.workers
        || stats.placement_count != 7 || stats.partition_placement_counts != expected_counts
        || stats.observed_worker_threads != options.workers) {
        std::cerr << "worker execution statistics mismatch\n";
        return 1;
    }
    if (eawr::scene::canonical_text(eawr::scene::build(input)) != eawr::scene::canonical_text(scene)) {
        std::cerr << "parallel and serial scene bytes differ\n";
        return 1;
    }
    if (options.self_test && !verify(scene, input, map.value())) {
        std::cerr << "synthetic scene invariant or mutation check failed\n";
        return 1;
    }
    const auto canonical = eawr::scene::canonical_text(scene);
    const auto actual_hash = digest(std::as_bytes(std::span(canonical.data(), canonical.size())));
    if (scene.scene_sha256 != actual_hash) {
        std::cerr << "builder scene hash differs from canonical input\n";
        return 1;
    }
    std::filesystem::create_directories(options.output);
    std::string counts = "[";
    for (std::size_t index = 0; index < stats.partition_placement_counts.size(); ++index) {
        if (index != 0) counts += ',';
        counts += std::to_string(stats.partition_placement_counts[index]);
    }
    counts += ']';
    const std::string metadata = "{\"schema\":2,\"contract_version\":1,\"target\":\"" + options.target
        + "\",\"build_id\":\"" + options.build_id + "\",\"fixture_sha256\":\"" + options.fixture_sha256
        + "\",\"map_sha256\":\"" + input.map_sha256 + "\",\"scene_sha256\":\"" + actual_hash
        + "\",\"workers_requested\":" + std::to_string(stats.workers_requested)
        + ",\"executor\":\"thread-worker-adapter\",\"parallel_stage\":\"placement-finalization-v1\""
        + ",\"partitions_completed\":" + std::to_string(stats.partitions_completed)
        + ",\"placement_count\":" + std::to_string(stats.placement_count)
        + ",\"partition_placement_counts\":" + counts
        + ",\"observed_worker_threads\":" + std::to_string(stats.observed_worker_threads) + "}\n";
    if (!write(options.output / "scene.txt", canonical) || !write(options.output / "evidence.json", metadata)) {
        std::cerr << "could not write scene evidence\n";
        return 1;
    }
    std::cout << options.target << " " << actual_hash << '\n';
    return 0;
}
