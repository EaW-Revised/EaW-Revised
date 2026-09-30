#pragma once

// Private declarations for the renderer fault probe. The node lifecycle, the
// invented asset and fault-text helpers, the upload/diagnostic plumbing and the
// remaining scenarios live in renderer_fault_probe.cpp; the material scenarios
// in renderer_fault_material_tests.cpp and the fog variant scenarios in
// renderer_fault_fog_tests.cpp.

#include "adapter_source_faults.hpp"

#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/sim/snapshot.hpp"

#include <gdextension_interface.h>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
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
// Not for console output: godot-cpp links libstdc++ statically on Linux and
// from GCC 13 only <iostream> references the stream initialisation the
// report's ostringstream needs (see the churn probe).
#include <iostream> // IWYU pragma: keep
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


using namespace godot;

namespace eawr_renderer_fault_probe {

namespace sim = eawr::sim;
namespace assets = eawr::assets;
namespace presentation = eawr::presentation;
namespace faults = eawr::presentation::godot_backend::fault_injection;
using eawr::presentation::godot_backend::GodotRenderer;
using Slot = faults::Slot;

constexpr int view_width = 320;
constexpr int view_height = 180;
constexpr int settle_frames = 3;
constexpr int frame_budget = 600;

// ---- invented geometry and textures -------------------------------------

[[nodiscard]] sim::math::Mat3x4 lane(const std::int64_t x);

[[nodiscard]] assets::Submesh triangle(const float half, const float lift);

[[nodiscard]] assets::Model plate(const float half = 40.0F, const std::size_t surfaces = 1);

[[nodiscard]] assets::Model partially_invalid_plate();

[[nodiscard]] assets::Texture solid(const std::uint8_t shade, const std::uint32_t edge = 64);

[[nodiscard]] presentation::MaterialDescription modern(std::string program,
    std::vector<presentation::MaterialBinding> bindings = {});

constexpr std::string_view good_modern =
    "shader_type spatial;\nrender_mode unshaded, cull_disabled;\n"
    "void fragment() { ALBEDO = vec3(0.8, 0.2, 0.2); }\n";

[[nodiscard]] std::string sampler_program(const std::size_t count, const std::string_view declarations = {},
    const std::string_view fragment = {});

[[nodiscard]] std::vector<presentation::MaterialBinding> sampler_bindings(const std::size_t count);

[[nodiscard]] std::string block_program(const std::size_t count);

constexpr std::string_view typed_modern = R"GODOT(shader_type spatial;
render_mode unshaded, cull_disabled;
uniform vec4 eawr_tint = vec4(1.0);
uniform float eawr_gain = 1.0;
uniform vec2 eawr_offset = vec2(0.0);
uniform bool eawr_enabled = true;
uniform int eawr_count = 1;
uniform vec4 eawr_colour : source_color = vec4(1.0);
uniform vec3 eawr_rgb : source_color = vec3(1.0);
void fragment() {
    float on = eawr_enabled ? 1.0 : 0.5;
    ALBEDO = eawr_tint.rgb * eawr_gain * on * float(eawr_count) * eawr_colour.rgb * eawr_rgb
        + vec3(eawr_offset, 0.0) * 0.01;
}
)GODOT";

// One row per fixed legacy adapter source the renderer selects at upload.
struct Legacy final {
    std::string_view label;
    Slot slot;
    std::string_view program;
    std::string_view technique;
    std::string_view pass_name;
    presentation::RenderPass pass;
};

constexpr std::array<Legacy, 5> legacy_rows{{
    {"MeshGloss.fx opaque", Slot::meshgloss_opaque, "MeshGloss.fx", "sph_t0", "sph_t0_p0",
        presentation::RenderPass::opaque},
    {"MeshGloss.fx transparent", Slot::meshgloss_alpha, "MeshGloss.fx", "sph_t0", "sph_t0_p0",
        presentation::RenderPass::transparent},
    {"RSkinGloss.fx", Slot::rskin_opaque, "RSkinGloss.fx", "sph_t1", "sph_t1_p0",
        presentation::RenderPass::opaque},
    {"BatchMeshGloss.fx", Slot::fixed_mesh_opaque, "BatchMeshGloss.fx", "sph_t1", "sph_t1_p0",
        presentation::RenderPass::opaque},
    {"BatchMeshAlpha.fx", Slot::fixed_mesh_alpha, "BatchMeshAlpha.fx", "sph_t1", "sph_t1_p0",
        presentation::RenderPass::transparent},
}};

[[nodiscard]] presentation::MaterialDescription legacy(const Legacy& row,
    std::vector<presentation::MaterialBinding> bindings = {});

// ---- armed fault texts, derived from the production text of the slot -----

[[nodiscard]] std::string compile_error(const std::string_view source);

[[nodiscard]] std::string variant_only_error(const std::string_view source);

[[nodiscard]] std::optional<std::string> without_rewritable_mode(const std::string_view source);

[[nodiscard]] std::string escape(const std::string& text);

[[nodiscard]] std::string json_string(const std::string& text);
[[nodiscard]] std::string boolean(const bool value);

[[nodiscard]] std::string without_backend(const std::string& message);

} // namespace eawr_renderer_fault_probe

using namespace eawr_renderer_fault_probe;

class EawrRendererFaultProbe final : public Node {
    GDCLASS(EawrRendererFaultProbe, Node)

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
        friend bool operator==(const Sample&, const Sample&) = default;
    };

    void check(bool condition, const std::string& message);
    [[nodiscard]] Sample sample() const;
    [[nodiscard]] std::size_t reads(Slot slot) const;
    void arm(Slot slot, std::string text);
    void disarm();
    void add(std::string name, std::function<void()> act, std::function<void()> verify = {},
        int hold = settle_frames);
    // A failed upload whose code, message tail, registry and memory are
    // recorded as one case. `slot` is the armed slot, if any.
    void failed_upload(const std::string& name, sim::AssetId asset, std::function<eawr::core::Result<void>()> upload,
        std::string_view code, std::string expected_message, std::optional<Slot> slot);
    void accepted_upload(const std::string& name, sim::AssetId asset,
        std::function<eawr::core::Result<void>()> upload, std::optional<Slot> slot);
    void begin_renderer(bool shadows);
    void end_renderer();
    void submit(std::vector<std::pair<sim::EntityId, sim::AssetId>> members);
    void build(const std::string& scenario);
    void build_production_adapters();
    void build_legacy_compile_failures();
    void build_shadow_variant_failures();
    void build_fog_variant_failures();
    void build_fog_shadow_variant_failures();
    void build_modern_compile_failure();
    void build_material_admission();
    void build_upload_failure_kinds();
    void build_backend_unavailable();
    [[nodiscard]] std::string last_diagnostic() const;
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
    bool known_scenario_{};
    std::string scenario_;
    std::string leak_control_;
    std::string report_path_;
    std::vector<std::string> failures_;
    std::vector<std::string> cases_;
    std::vector<std::string> evidence_;
    Sample warm_;
    Sample baseline_;
    Sample before_;
    bool memory_visible_{};
    // RenderingServer returns shader code it was given. The headless dummy
    // server returns an empty string, so the renderer's lexical sampler2D
    // check refuses every fog-stub-v1 variant there, faulted or not.
    bool shader_code_readback_{};
    std::string skipped_;
    std::uint64_t tick_{1};
};
