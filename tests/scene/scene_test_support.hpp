#pragma once

// Private support for the scene_tests contract runner: the shared failure
// counter, synthetic TED/XML/asset fixtures and the contract groups main runs.

#include "eawr/scene/scene.hpp"
#include "eawr/platform/sim_workers.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/sim/math/fixed.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace eawr::tests::scene_tests {

using eawr::scene::Cause;
using Fixed = eawr::sim::math::Fixed;

extern int failures;
void expect(bool condition, std::string_view message);

class ReversedExecutor final : public eawr::sim::PartitionExecutor {
public:
    explicit ReversedExecutor(std::size_t workers, bool fail = false, bool duplicate = false,
                              bool* active = nullptr)
        : workers_(workers), fail_(fail), duplicate_(duplicate), active_(active) {}
    std::size_t worker_count() const noexcept override { return workers_; }
    eawr::core::Result<void> execute(std::size_t count,
        const std::function<void(std::size_t)>& partition) const override {
        if (active_) *active_ = true;
        for (std::size_t index = count; index > 0; --index) partition(index - 1);
        if (active_) *active_ = false;
        if (duplicate_) partition(0);
        if (fail_) return eawr::core::Result<void>::failure({
            "EAWR-TEST-EXECUTOR", eawr::core::Severity::error, "injected executor failure", {}, {}, {}, {}});
        return eawr::core::Result<void>::success();
    }
private:
    std::size_t workers_;
    bool fail_;
    bool duplicate_;
    bool* active_;
};

struct Record final {
    std::string type;          // object id whose CRC is written; empty = no CRC mini
    std::optional<std::array<float, 3>> position;
    std::array<float, 3> orientation{};
    bool orientation_absent{};  // true = no orientation mini is written
    std::optional<std::int32_t> owner{};  // mini 2, the owning player index
};

std::vector<std::byte> ted(const std::vector<Record>& records);

struct TempTree final {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / ("eawr-scene-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempTree() { std::filesystem::create_directories(root); }
    ~TempTree() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
};

void write_catalog(const std::filesystem::path& root);

eawr::assets::Model model(const std::vector<std::string>& shaders, const std::string& texture,
                          const std::vector<std::string>& proxies);

struct Assets final {
    std::map<std::string, eawr::assets::Model> models;
    std::set<std::string> files;

    Assets() {
        models.emplace("data/art/models/eawr_scene_prop.alo",
                       model({"MeshGloss.fx"}, "eawr_scene_prop.tga", {"eawr_scene_smoke", "Eawr_Scene_Smoke_ALT2"}));
        models.emplace("data/art/models/eawr_scene_plain.alo",
                       model({"MeshGloss.fx", "RSkinGloss.fx"}, "eawr_scene_plain.tga", {}));
        models.emplace("data/art/models/eawr_scene_odd.alo",
                       model({"MeshGloss.fx", "EawrUnlisted.fx"}, "eawr_scene_missing.tga", {"eawr_scene_gone", "eawr_scene_smoke_ALTx"}));
        for (const auto& [path, value] : models) files.insert(path);
        files.insert("data/art/models/eawr_scene_smoke.alo");
        files.insert("data/art/models/eawr_scene_prop_idle_00.ala");
        files.insert("data/art/textures/eawr_scene_prop.dds");
        files.insert("data/art/textures/eawr_scene_plain.dds");
    }

    eawr::scene::AssetAccess access() const {
        return {
            [this](const std::string_view path) { return files.contains(std::string(path)); },
            [this](const std::string_view path) -> const eawr::assets::Model* {
                const auto found = models.find(std::string(path));
                return found == models.end() ? nullptr : &found->second;
            },
            [](const std::string_view path) { return "sha-of:" + std::string(path); },
            [](const std::string_view) { return false; },
        };
    }
};

bool has(const eawr::scene::Placement& placement, Cause cause, std::string_view detail = {});

// scene_build_tests.cpp
void conversion_contracts();
void scene_contracts();

// scene_issue_tests.cpp
void scene_issue_contracts(const TempTree& tree, const eawr::data::Catalog& catalog, const std::vector<Record>& records,
                           const eawr::core::Result<eawr::assets::Map>& map, const eawr::scene::BuildInput& input,
                           const eawr::scene::Scene& scene, const std::string& serial_bytes);

// scene_asset_state_tests.cpp
void selector_contracts();
void static_mesh_state_contracts();
void uncaptured_capture_point_contracts();
} // namespace eawr::tests::scene_tests
