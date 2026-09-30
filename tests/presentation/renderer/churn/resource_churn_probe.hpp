#pragma once

// Private declarations for the resource churn probe. The node lifecycle and the
// invented asset helpers live in resource_churn_probe.cpp, the step list in
// resource_churn_probe_steps.cpp and the draw/diagnostic evidence and report in
// resource_churn_probe_evidence.cpp.

#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/sim/snapshot.hpp"

#include <gdextension_interface.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
// Not for console output: godot-cpp links libstdc++ statically on Linux, and
// from GCC 13 only <iostream> references the library's stream initialisation
// (ios_base_library_init). Without it, g++-14 on Linux crashed with SIGSEGV at
// the report's first ostringstream << integer in finish().
#include <iostream> // IWYU pragma: keep
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


using namespace godot;

namespace eawr_resource_churn_probe {

namespace sim = eawr::sim;
namespace assets = eawr::assets;
namespace presentation = eawr::presentation;
using eawr::presentation::godot_backend::GodotRenderer;

constexpr int view_width = 320;
constexpr int view_height = 180;
constexpr int settle_frames = 3;
constexpr int frame_budget = 4000;
constexpr int churn_cycles = 60;
constexpr int steady_missing_frames = 90;

// Asset IDs. 404/405 are never uploaded; 77 is uploaded only for recovery.
constexpr sim::AssetId red = 1;
constexpr sim::AssetId green = 2;
constexpr sim::AssetId skinned = 3;
constexpr sim::AssetId partial = 50;
constexpr sim::AssetId late = 77;
constexpr sim::AssetId unavailable = 404;
constexpr sim::AssetId unavailable_other = 405;

[[nodiscard]] sim::math::Mat3x4 lane(const std::int64_t x);

[[nodiscard]] assets::Submesh triangle(const float half, const float lift);

[[nodiscard]] assets::Model plate(const float half, const std::size_t surfaces = 1, const bool rigid_skin = false);

[[nodiscard]] assets::Model partially_invalid_plate();

[[nodiscard]] assets::Texture solid(const std::uint8_t r, const std::uint8_t g, const std::uint8_t b,
    const std::uint32_t edge = 16);

[[nodiscard]] presentation::MaterialDescription unshaded(const std::string_view color);

struct Member final {
    sim::EntityId entity{};
    sim::AssetId asset{};
    std::int64_t x{};
};

[[nodiscard]] std::shared_ptr<const sim::RenderSnapshot> scene(
    const std::uint64_t tick, const std::vector<Member>& members);

// The invented textures and materials the steps upload, made once per step list.
struct ProbeAssets final {
    assets::Texture red_texture;
    assets::Texture green_texture;
    assets::Texture blue_texture;
    presentation::MaterialDescription red_material;
    presentation::MaterialDescription green_material;
    presentation::MaterialDescription blue_material;
};

[[nodiscard]] std::string escape(const std::string& text);

} // namespace eawr_resource_churn_probe

using namespace eawr_resource_churn_probe;

class EawrResourceChurnProbe final : public Node {
    GDCLASS(EawrResourceChurnProbe, Node)

protected:
    static void _bind_methods() {}

public:
    void _ready() override;
    void _process(double delta) override;

private:
    struct Step final {
        std::string name;
        std::function<void()> act;
        std::function<void()> verify;
        int hold{settle_frames};
    };
    struct Sample final {
        std::int64_t objects{};
        std::int64_t texture{};
        std::int64_t buffer{};
    };

    void check(bool condition, const std::string& message);
    [[nodiscard]] Sample sample() const;
    void record(const std::string& step, const Sample& value);
    void upload(sim::AssetId asset, const assets::Model& model, const assets::Texture& texture,
        const presentation::MaterialDescription& material);
    // `drawn` is the surfaces expected in the next frames; it defaults to one
    // per instance, which holds for every single-surface asset here.
    void submit(std::shared_ptr<const sim::RenderSnapshot> snapshot, std::size_t expected,
        std::size_t drawn = static_cast<std::size_t>(-1));
    void expect_drawn(const std::string& step);
    [[nodiscard]] std::size_t count_code(std::string_view code) const;
    [[nodiscard]] bool has_message(std::string_view text) const;
    [[nodiscard]] std::vector<std::string> tail(std::size_t count) const;
    [[nodiscard]] std::size_t references(sim::AssetId asset) const;
    [[nodiscard]] const GodotRenderer::InstanceEvidence* evidence(sim::EntityId entity);
    void build_steps();
    // The step groups build_steps() appends, in order.
    void build_baseline_steps(const ProbeAssets& probe_assets);
    void build_upload_steps(const ProbeAssets& probe_assets);
    void build_churn_steps();
    void verify_churn_switch(int cycle, std::size_t item, std::size_t expected, sim::EntityId fresh);
    void build_churn_summary_step();
    void build_unavailable_steps(const ProbeAssets& probe_assets);
    void build_skin_pose_steps();
    void build_pose_retire_steps();
    void build_skin_pose_release_step(const ProbeAssets& probe_assets);
    void build_shutdown_steps(const ProbeAssets& probe_assets);
    void finish();

    SubViewport* viewport_{};
    Node3D* host_{};
    std::unique_ptr<GodotRenderer> renderer_;
    std::vector<Step> steps_;
    std::size_t index_{};
    int wait_{};
    int frames_{};
    bool finished_{};
    bool finishing_{};
    bool leak_control_{};
    std::string report_path_;
    std::vector<std::string> failures_;
    std::vector<std::string> samples_;
    std::vector<std::string> evidence_;
    std::vector<GodotRenderer::InstanceEvidence> instance_readback_;
    std::size_t expected_instances_{};
    std::size_t expected_drawn_{};
    Sample baseline_;
    Sample with_assets_;
    Sample churn_full_;
    Sample after_shutdown_;
    Sample after_restart_;
    std::size_t churn_steps_{};
    std::size_t churn_frames_{};
    std::size_t churn_texture_drift_{};
    std::int64_t churn_max_texture_{};
    std::size_t churn_posed_{};
    std::size_t churn_posed_readback_{};
    std::size_t churn_max_poses_{};
    std::size_t pose_base_{};
    std::vector<std::string> steady_tail_;
};
