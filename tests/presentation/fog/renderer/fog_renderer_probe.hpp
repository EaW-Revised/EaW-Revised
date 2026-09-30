#pragma once

// Private declarations for the fog-stub-v1 renderer probe. The probe node's
// lifecycle lives in fog_renderer_probe.cpp, the synthetic surface helpers in
// fog_renderer_surface_probe.cpp, the step list in fog_renderer_instance_probe.cpp
// and the verification/report writing in fog_renderer_evidence.cpp.

#include "shader_adapter.hpp"

#include "eawr/presentation/fog/fog.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/sim/snapshot.hpp"

#include <gdextension_interface.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace godot;

namespace eawr_fog_renderer_probe {

namespace fog = eawr::presentation::fog;
namespace sim = eawr::sim;
namespace sim_fog = eawr::sim::fog;
namespace presentation = eawr::presentation;
using eawr::presentation::godot_backend::GodotRenderer;

constexpr std::int64_t one = std::int64_t{1} << 24;
constexpr int view_width = 400;
constexpr int view_height = 200;
constexpr float fov_degrees = 30.0F;
constexpr int settle_frames = 4;
constexpr int frame_budget = 3000;

// Camera straight down at source (0.5, 1.0): screen right is +X, screen up is
// world -Z (source +Y).
constexpr double camera_x = 0.5;
constexpr double camera_y = 12.0;
constexpr double camera_z = -1.0;

// Every fog byte any rendered grid uses; each has one control capture.
constexpr std::array<std::uint8_t, 7> control_bytes{0, 10, 60, 110, 160, 210, 255};

// Invented asset IDs.
constexpr sim::AssetId terrain_asset = 1;
constexpr sim::AssetId unit_asset = 2;
constexpr sim::AssetId late_unit_asset = 3;
constexpr sim::AssetId meshgloss_asset = 4;
constexpr sim::AssetId plain_modern_asset = 5;
constexpr sim::AssetId wrong_type_asset = 6;
// Supported synthetic controls: a shader whose eawr_fog_bound defaults to true
// (and whose texture defaults to white), and the terrain shader carrying its
// own pre-existing fog overrides through material bindings.
constexpr sim::AssetId bound_default_asset = 7;
constexpr sim::AssetId overrides_asset = 8;
// Refused: all four other fog uniforms correct, eawr_fog_texture not a
// single plain sampler2D (see sampler_cases below).
constexpr sim::AssetId sampler_case_base = 9;
constexpr sim::AssetId terrain_control_base = 1000;
constexpr sim::AssetId unit_control_base = 2000;
constexpr sim::AssetId alpha_asset = 3000;
constexpr sim::AssetId alpha_control_base = 3100;
constexpr sim::AssetId alpha_background_asset = 3200;
constexpr sim::AssetId shadow_alpha_asset = 4000;
constexpr sim::AssetId shadow_control_base = 4100;
constexpr sim::EntityId terrain_entity = 1;
constexpr sim::EntityId unit_entity = 2;

// Terrain-like plate: 10 x 4.5 source units in 20 x 9 quads, translated
// below the origin plane; unit-like plate: 2.4 x 3.0 in asset XY, raised,
// rotated 90 degrees about the vertical and translated, so it covers source
// [-0.625, 2.375] x [-0.325, 2.075]: three grid columns, both rows and dark
// outside the grid below and above.
constexpr double terrain_half_x = 5.0;
constexpr double terrain_half_y = 2.25;
constexpr double unit_half_x = 1.2;
constexpr double unit_half_y = 1.5;

constexpr std::string_view terrain_program = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, cull_disabled;
uniform vec3 eawr_surface_linear = vec3(0.8, 0.55, 0.3);
uniform sampler2D eawr_fog_texture : filter_nearest, repeat_disable;
uniform vec2 eawr_fog_origin = vec2(0.0);
uniform vec2 eawr_fog_extent = vec2(1.0);
uniform vec2 eawr_fog_size = vec2(1.0);
uniform bool eawr_fog_bound = false;
varying vec3 eawr_world_position;
vec3 eawr_linear_to_srgb(vec3 linear_rgb) {
    vec3 nonnegative = max(linear_rgb, vec3(0.0));
    vec3 low = 12.92 * nonnegative;
    vec3 high = 1.055 * pow(nonnegative, vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(nonnegative, vec3(0.0031308)));
}
void vertex() {
    eawr_world_position = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}
void fragment() {
    vec2 source = vec2(eawr_world_position.x, -eawr_world_position.z);
    vec2 uv = (source - eawr_fog_origin) / eawr_fog_extent;
    float attenuation = 0.0;
    if (eawr_fog_bound && uv.x >= 0.0 && uv.y >= 0.0 && uv.x < 1.0 && uv.y < 1.0) {
        vec2 texel = min(floor(uv * eawr_fog_size), eawr_fog_size - vec2(1.0));
        attenuation = texture(eawr_fog_texture, (texel + vec2(0.5)) / eawr_fog_size).r;
    }
    vec3 linear_rgb = eawr_surface_linear * attenuation;
    ALBEDO = OUTPUT_IS_SRGB ? eawr_linear_to_srgb(linear_rgb) : linear_rgb;
}
)GODOT";

// The same terrain arithmetic with the attenuation as a material constant.
constexpr std::string_view terrain_control_program = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, cull_disabled;
uniform vec3 eawr_surface_linear = vec3(0.8, 0.55, 0.3);
uniform float eawr_control_scale = 1.0;
vec3 eawr_linear_to_srgb(vec3 linear_rgb) {
    vec3 nonnegative = max(linear_rgb, vec3(0.0));
    vec3 low = 12.92 * nonnegative;
    vec3 high = 1.055 * pow(nonnegative, vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(nonnegative, vec3(0.0031308)));
}
void fragment() {
    vec3 linear_rgb = eawr_surface_linear * eawr_control_scale;
    ALBEDO = OUTPUT_IS_SRGB ? eawr_linear_to_srgb(linear_rgb) : linear_rgb;
}
)GODOT";

// Declares eawr_fog_bound with the wrong type: must be refused.
constexpr std::string_view wrong_type_program = R"GODOT(
shader_type spatial;
render_mode unshaded;
uniform sampler2D eawr_fog_texture : filter_nearest, repeat_disable;
uniform vec2 eawr_fog_origin = vec2(0.0);
uniform vec2 eawr_fog_extent = vec2(1.0);
uniform vec2 eawr_fog_size = vec2(1.0);
uniform float eawr_fog_bound = 0.0;
void fragment() {
    ALBEDO = vec3(eawr_fog_bound) + texture(eawr_fog_texture, eawr_fog_origin + eawr_fog_extent + eawr_fog_size).rgb;
}
)GODOT";

[[nodiscard]] std::string bound_default_program();

// Four correct fog uniforms plus an eawr_fog_texture declaration that is not a
// single plain sampler2D; every one compiles.
struct SamplerCase {
    std::string_view label;
    std::string_view declaration;
    std::string_view sample; // float expression reading the texture at vec2 uv
    std::string_view reason; // word the refusal must contain
};
constexpr std::array<SamplerCase, 7> sampler_cases{{
    {"samplerCube", "uniform samplerCube eawr_fog_texture;", "texture(eawr_fog_texture, vec3(uv, 1.0)).r",
        "not a sampler2D (Texture2D)"},
    {"sampler2DArray", "uniform sampler2DArray eawr_fog_texture;", "texture(eawr_fog_texture, vec3(uv, 0.0)).r",
        "not a sampler2D (Texture2D)"},
    {"sampler3D", "uniform sampler3D eawr_fog_texture;", "texture(eawr_fog_texture, vec3(uv, 0.0)).r",
        "not a sampler2D (Texture2D)"},
    {"usampler2D", "uniform usampler2D eawr_fog_texture;", "float(texture(eawr_fog_texture, uv).r)",
        "declared as usampler2D"},
    {"isampler2D", "uniform highp isampler2D eawr_fog_texture;", "float(texture(eawr_fog_texture, uv).r)",
        "declared as isampler2D"},
    {"sampler2D[2]", "uniform sampler2D eawr_fog_texture[2];", "texture(eawr_fog_texture[0], uv).r",
        "eawr_fog_texture as Array"},
    // Commented-out sampler2D decoys ahead of the real declaration. (A
    // preprocessor-hidden declaration cannot get this far: RenderingServer
    // shader code is not preprocessed and '#' fails to compile at upload.)
    {"comment-decoy",
        "// uniform sampler2D eawr_fog_texture;\n/* uniform sampler2D eawr_fog_texture; */\n"
        "uniform usampler2D /* sampler2D */ eawr_fog_texture;",
        "float(texture(eawr_fog_texture, uv).r)", "declared as usampler2D"},
}};

[[nodiscard]] std::string sampler_case_program(const SamplerCase& sampler);

[[nodiscard]] std::vector<presentation::MaterialBinding> pre_existing_overrides();

[[nodiscard]] std::string escape(const std::string& text);

[[nodiscard]] sim::math::Fixed fixed(const double value);

[[nodiscard]] sim::math::Mat3x4 affine(const std::array<std::array<double, 4>, 3>& rows);

// Terrain: translated only. Unit: rotated +90 degrees about Y (x' = z,
// z' = -x) and translated.
const sim::math::Mat3x4 terrain_transform = affine({{{1, 0, 0, 0.5}, {0, 1, 0, -0.25}, {0, 0, 1, -1.0}}});
const sim::math::Mat3x4 unit_transform = affine({{{0, 0, 1, 0.875}, {0, 1, 0, 0.5}, {-1, 0, 0, -0.875}}});
const sim::math::Mat3x4 unit_moved_transform = affine({{{0, 0, 1, 1.25}, {0, 1, 0, 0.75}, {-1, 0, 0, -1.5}}});

[[nodiscard]] Transform3D godot_transform(const sim::math::Mat3x4& matrix);

[[nodiscard]] eawr::assets::Model plate(const double half_x, const double half_y, const int nx, const int ny);

[[nodiscard]] eawr::assets::Texture solid_texture(const std::array<std::uint8_t, 4> rgba);

[[nodiscard]] presentation::MaterialDescription batch_mesh_gloss(std::vector<presentation::MaterialBinding> bindings = {});

[[nodiscard]] presentation::MaterialDescription batch_mesh_alpha(const float rgb_scale = 1.0F);

[[nodiscard]] presentation::MaterialDescription modern(const std::string_view program,
    std::vector<presentation::MaterialBinding> bindings = {});

sim_fog::FogGridDesc base_desc(const std::uint64_t revision, const std::uint32_t team = 0);

const std::vector<std::uint8_t> base_cells{10, 60, 110, 160, 210, 255};

[[nodiscard]] std::string readiness_name(const GodotRenderer::FogReadiness readiness);

[[nodiscard]] bool same_resources(
    const std::vector<presentation::ResourceReference>& left,
    const std::vector<presentation::ResourceReference>& right);

struct Surface {
    Transform3D transform;
    Transform3D inverse;
    double half_x{};
    double half_y{};
};

} // namespace eawr_fog_renderer_probe

using namespace eawr_fog_renderer_probe;

class EawrFogRendererProbe final : public Node {
    GDCLASS(EawrFogRendererProbe, Node)

protected:
    static void _bind_methods() {}

public:
    void _ready() override;
    void _process(double delta) override;

private:
    enum class Capture { none, control, baseline, compare, raw, keep, kept,
        alpha_background, alpha_control, alpha_compare, alpha_spatial };
    struct Step {
        std::string name;
        std::function<void()> act;
        Capture capture{Capture::none};
        // For Capture::compare: the grid the consumers should show, or nullptr
        // for dark (unbound); `unit` selects which unit surface is drawn.
        std::function<const sim_fog::FogGrid*()> expected;
        std::uint8_t control_byte{};
    };

    void check(bool condition, const std::string& message);
    sim_fog::FogGrid make_grid(const sim_fog::FogGridDesc& desc, const std::vector<std::uint8_t>& cells);
    sim_fog::FogGridSet make_set(std::vector<sim_fog::FogGrid> grids);
    void upload(sim::AssetId asset, const eawr::assets::Model& model, const eawr::assets::Texture& texture,
        const presentation::MaterialDescription& material);
    void submit_scene(std::uint64_t tick, sim::AssetId terrain, sim::AssetId unit,
        const sim_fog::FogGridSet& grids, const sim::math::Mat3x4& unit_at = unit_transform);
    void expect_action(fog::SubmitAction action, const std::string& label);
    void expect_rejection(std::string_view code, bool retained, const std::string& label);
    void record_stage(const std::string& name);
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> fog_parameters(sim::AssetId asset);
    void expect_forced_unbound(sim::AssetId asset, const std::string& label);
    void record_parameters(const std::string& label, sim::AssetId asset);
    [[nodiscard]] PackedByteArray grab();
    void verify(const Step& step, const PackedByteArray& pixels);
    void build_steps();
    void build_alpha_steps();
    void destroy_renderer();
    void finish();

    SubViewport* viewport_{};
    Node3D* host_{};
    std::unique_ptr<GodotRenderer> renderer_;
    std::map<std::uint8_t, PackedByteArray> controls_;
    std::map<std::uint8_t, PackedByteArray> alpha_controls_;
    PackedByteArray alpha_background_;
    PackedByteArray baseline_;
    PackedByteArray kept_;
    std::map<sim::AssetId, std::vector<std::pair<std::string, std::string>>> parameters_before_;
    std::vector<std::string> parameter_evidence_;
    std::vector<Surface> surfaces_; // unit first (it lies above the terrain)
    std::vector<Step> steps_;
    std::size_t index_{};
    int wait_{};
    int frames_{};
    bool finished_{};
    bool early_exit_{};
    bool alpha_only_{};
    bool early_exit_armed_{};
    bool finishing_{};
    int finishing_frames_{};
    std::string report_path_;
    std::vector<std::string> failures_;
    std::vector<std::string> stages_;
    std::vector<std::string> renders_;
    std::vector<std::string> evidence_;
    std::optional<sim_fog::FogGrid> current_;
    std::optional<sim_fog::FogGrid> team7_;
    std::shared_ptr<const sim::RenderSnapshot> retained_;
    std::vector<presentation::ResourceReference> resources_before_;
    std::uint64_t tick_{};
    std::uint64_t texture_memory_baseline_{};
    std::uint64_t texture_memory_before_enable_{};
    std::uint64_t texture_memory_with_fog_{};
    std::uint64_t texture_memory_after_disable_{};
    std::uint64_t texture_memory_after_destroy_{};
    std::uint64_t texture_memory_with_assets_{};
    bool destroyed_with_fog_enabled_{};
};
