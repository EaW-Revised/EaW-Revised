#pragma once

// Private to the space environment translation units (space_environment*.cpp):
// the environment's State, its default view, and the helpers the units share.
#include "space_environment.hpp"
#include "capture_viewport.hpp"
#include "fog_mode.hpp"
#include "space_fog_units.hpp"
#include "space_populate.hpp"

#include "eawr/core/diagnostic.hpp"
#include "eawr/presentation/camera/constants_source.hpp"
#include "eawr/presentation/camera/overview.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/presentation/space/environment_scene.hpp"
#include "eawr/presentation/space/space.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/snapshot.hpp"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace space_environment_detail {

namespace tactical = presentation::camera;
namespace camera_input = viewer::camera_input;

[[nodiscard]] std::vector<MaterialBinding> meshgloss_bindings(
    const space::MeshGlossMaterial& material, const space::SkyLightPolicy& policy);
[[nodiscard]] std::vector<MaterialBinding> meshadditive_bindings(
    const space::MeshAdditiveMaterial& material, const space::MeshAdditiveInputs& inputs);
[[nodiscard]] bool shadow_control(std::string_view control);
[[nodiscard]] bool caster_control(std::string_view control);
[[nodiscard]] std::string number(double value, int precision = 6);
[[nodiscard]] std::string json(std::string_view value);
[[nodiscard]] std::string hash_bytes(std::span<const std::byte> bytes);
[[nodiscard]] std::optional<std::string> probe_reference(
    const vfs::Vfs& filesystem, std::string_view root,
    std::string_view name, std::span<const std::string_view> suffixes);
[[nodiscard]] std::optional<space::Rgb8Image> rgb_of(const std::vector<std::byte>& png);
[[nodiscard]] std::string write_file(const std::filesystem::path& path, std::span<const std::byte> bytes);
[[nodiscard]] tactical::TacticalFrame tactical_frame(const FixedCamera& source);
[[nodiscard]] FixedCamera fixed_camera(const tactical::TacticalFrame& source);

// Clean-room modern_spatial adapter for eawr-space-sky-meshgloss-v1: the
// MeshGloss sph_t0/sph_t0_p0 arithmetic of docs/behaviour/meshgloss-programmable.md
// fed with the surface's authored Emissive, Diffuse and Specular (Shininess is
// recorded, not consumed: the selected path's exponent is a fixed 16). The
// irradiance matrices, light-zero direction/specular and light scale are the
// declared sky light policy (space-sky-unlit-v1 unless a labelled control),
// under names the renderer's scene lighting never writes. The eye is the live
// camera. Render state is the sky preview policy shared with the diffuse
// adapter (seen from inside, never writes depth); only light-scale alpha 1 is
// admitted, which is MeshGloss's own opaque (non-blending) case.
constexpr std::string_view meshgloss_sky_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, cull_disabled, depth_draw_never;
uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform vec4 Emissive;
uniform vec4 Diffuse;
uniform vec4 Specular;
uniform vec4 eawr_sky_sph_r0;
uniform vec4 eawr_sky_sph_r1;
uniform vec4 eawr_sky_sph_r2;
uniform vec4 eawr_sky_sph_r3;
uniform vec4 eawr_sky_sph_g0;
uniform vec4 eawr_sky_sph_g1;
uniform vec4 eawr_sky_sph_g2;
uniform vec4 eawr_sky_sph_g3;
uniform vec4 eawr_sky_sph_b0;
uniform vec4 eawr_sky_sph_b1;
uniform vec4 eawr_sky_sph_b2;
uniform vec4 eawr_sky_sph_b3;
uniform vec3 eawr_sky_light_direction;
uniform vec3 eawr_sky_light_specular;
uniform vec4 eawr_sky_light_scale;
varying vec4 eawr_vertex_diffuse;
varying vec3 eawr_vertex_specular;

vec3 eawr_linear_to_srgb(vec3 linear_rgb) {
    vec3 nonnegative = max(linear_rgb, vec3(0.0));
    vec3 low = 12.92 * nonnegative;
    vec3 high = 1.055 * pow(nonnegative, vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(nonnegative, vec3(0.0031308)));
}

vec3 eawr_srgb_to_linear(vec3 encoded_rgb) {
    vec3 nonnegative = max(encoded_rgb, vec3(0.0));
    vec3 low = nonnegative / 12.92;
    vec3 high = pow((nonnegative + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, lessThanEqual(nonnegative, vec3(0.04045)));
}

void vertex() {
    vec3 position_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    vec3 normal_world = normalize(mat3(MODEL_MATRIX) * NORMAL);
    vec4 normal_h = vec4(normal_world, 1.0);
    mat4 sph_r = mat4(eawr_sky_sph_r0, eawr_sky_sph_r1, eawr_sky_sph_r2, eawr_sky_sph_r3);
    mat4 sph_g = mat4(eawr_sky_sph_g0, eawr_sky_sph_g1, eawr_sky_sph_g2, eawr_sky_sph_g3);
    mat4 sph_b = mat4(eawr_sky_sph_b0, eawr_sky_sph_b1, eawr_sky_sph_b2, eawr_sky_sph_b3);
    vec3 irradiance = vec3(
        dot(normal_h, sph_r * normal_h),
        dot(normal_h, sph_g * normal_h),
        dot(normal_h, sph_b * normal_h));
    vec3 eye_direction = normalize(CAMERA_POSITION_WORLD - position_world);
    vec3 half_direction = normalize(eye_direction + eawr_sky_light_direction);
    float specular_factor = pow(max(dot(normal_world, half_direction), 0.0), 16.0);
    eawr_vertex_diffuse = vec4(
        Diffuse.rgb * irradiance * eawr_sky_light_scale.rgb + Emissive.rgb,
        eawr_sky_light_scale.a);
    eawr_vertex_specular = Specular.rgb * (specular_factor * eawr_sky_light_specular);
}

void fragment() {
    vec4 base_sample = texture(BaseTexture, UV);
    vec3 base_linear_rgb = eawr_srgb_to_linear(base_sample.rgb);
    vec3 eawr_linear_rgb = 2.0 * eawr_vertex_diffuse.rgb * base_linear_rgb
        + eawr_vertex_specular * base_sample.a;
    ALBEDO = eawr_linear_to_srgb(eawr_linear_rgb);
}
)GODOT";

// Clean-room modern_spatial adapter for eawr-space-sky-meshadditive-t0-v1: the
// MeshAdditive t0/t0_p0 arithmetic of docs/behaviour/meshadditive-sun-billboard.md
// A-02..A-10 for a non-billboard surface, fed with the surface's authored Color
// and UVScrollRate and the declared TIME and LIGHT_SCALE inputs under names no
// engine clock or scene light writes (Godot's TIME is not read). Declared
// state policy space-sky-meshadditive-state-v1: ONE/ONE additive rgb
// (blend_add with fragment alpha kept at 1), no depth write, the engine depth
// test, fog disabled, the sky preview's cull_disabled. Texels are sampled
// without sRGB decode and the product is written as a stored value (the
// stored-value output of docs/rendering.md; the Compatibility fallback
// compensates its ALBEDO round trip), so the arithmetic and the blend act on
// stored values.
constexpr std::string_view meshadditive_sky_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, cull_disabled, depth_draw_never, blend_add;
uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform vec4 Color;
uniform vec4 UVScrollRate;
uniform float eawr_sky_time;
uniform vec4 eawr_sky_light_scale;
varying vec2 eawr_scrolled_uv;
varying vec3 eawr_vertex_color;

void vertex() {
    // Unwrapped; the sampler's repeat addressing wraps it per sample.
    eawr_scrolled_uv = UV + eawr_sky_time * UVScrollRate.xy;
    eawr_vertex_color = clamp(Color.rgb * eawr_sky_light_scale.rgb * eawr_sky_light_scale.a, 0.0, 1.0);
}

// ALBEDO under the stored-value output (docs/rendering.md, colour policy);
// the Compatibility fallback compiles compatibility_albedo_writer instead.
vec3 eawr_stored_albedo(vec3 stored_rgb) {
    return stored_rgb;
}

void fragment() {
    vec3 stored_rgb = eawr_vertex_color * texture(BaseTexture, eawr_scrolled_uv).rgb;
    ALBEDO = eawr_stored_albedo(stored_rgb);
}
)GODOT";

// `meshadditive-synthetic` is a labelled control, never the shipped sky: it
// passes space::meshadditive_material_routes() so an exact MeshAdditive.fx
// non-billboard surface can be planned and drawn. Tokens after the name are
// separated by spaces: time=<seconds>, light-scale=<r>,<g>,<b>,<a> (declared
// inputs overriding space-sky-meshadditive-inputs-v1) and `occluder` (the
// foreground depth control). Anything else fails closed.
constexpr std::string_view meshadditive_control = "meshadditive-synthetic";

struct AdditiveControl final {
    bool enabled{};
    std::string base{"none"};
    space::MeshAdditiveInputs inputs = space::meshadditive_default_inputs();
    std::string failure;
};

constexpr sim::AssetId occluder_asset = 1000;
constexpr sim::EntityId occluder_entity = 1000;

constexpr std::array<std::string_view, 2> texture_suffixes{".tga", ".dds"};
constexpr std::array<std::string_view, 1> model_suffixes{".alo"};

// Declared duration of the terminal probe step: a test constant, not an
// original input cadence.
constexpr float terminal_step_seconds = 0.05F;
constexpr std::uint32_t settle_frame_count = 3;
constexpr std::size_t selftest_check_count = 18;

// Space_Mode values for the default camera; the XML layer when it loads,
// otherwise the struct's own values, reported as such.
struct TacticalLoad final {
    space::TacticalDefaults values;
    std::string source{"project fallback (Space_Mode XML did not load)"};
    std::string failure;
};

class EnvironmentView final {
public:
    explicit EnvironmentView(const SpaceEnvironment::Options& options) : options_(options) {}
    EnvironmentView(const EnvironmentView&) = delete;
    EnvironmentView& operator=(const EnvironmentView&) = delete;
    ~EnvironmentView() { release(); }

    [[nodiscard]] bool ready(Node3D& host, const assets::Map& map, const vfs::Vfs& filesystem,
                             const assets::ObjectTypeCatalog& catalog, bool catalog_loaded,
                             const std::string& catalog_failure);
    [[nodiscard]] std::optional<int> process(double delta);
    // The player closed the window: stops the live session, releases the scene and writes the
    // report as a finished run does (status "space_environment_closed"). Idempotent; the run
    // ends by the engine's own quit, so no process() follows.
    void close();
    void camera_event(const viewer::camera_input::RawEvent& event);
    void camera_focus(bool focused);
    void camera_pointer_left();
    void camera_viewport(float width, float height);
    // #82: see SpaceEnvironment::live_camera_frame and the rest.
    [[nodiscard]] std::optional<tactical::TacticalFrame> drawn_frame() const;
    void focus(float source_x, float source_y);
    [[nodiscard]] std::optional<presentation::camera::SourceTargetBounds> camera_bounds() const;
    void overview_key();
    [[nodiscard]] std::string overview_level() const;

private:
    struct Item final {
        std::string role;
        std::string object;
        std::optional<std::uint32_t> record;
        std::string model;
        std::string mesh;
        std::string shader;
        std::string route;
        std::uint32_t billboard{};
        std::string texture;
        std::string status{"not_drawn"};
        std::string cause;
        sim::AssetId asset{};
        RenderPass pass{RenderPass::opaque};
    };
    struct Placed final {
        std::string role;
        std::string object;
        std::optional<std::uint32_t> record;
        std::string model_path;
        const assets::Model* model{};
        space::Affine transform = space::identity_affine;
        float scale{1.0F};
        // World radius of the object's plain surfaces; its glow sits behind it.
        float radius{};
    };

    void compose_object(const Placed& placed, const std::vector<space::SceneSurface>& surfaces);
    void upload(Item item, const space::SceneSurface& surface, const assets::Model& geometry,
                const space::Affine& transform, const assets::Model* planned_from,
                bool sunlight_glow = false);
    [[nodiscard]] std::optional<assets::Texture> texture(const std::string& declared, std::string& logical,
                                                         std::string& failure);
    [[nodiscard]] bool fail(std::string message) {
        failure_ = std::move(message);
        status_ = "failed";
        release();
        static_cast<void>(write_report());
        return false;
    }
    void release() {
        if (!renderer_) return;
        if (!effects_released_ && options_.effects.release) {
            options_.effects.release();
            effects_released_ = true;
        }
        if (populate_result_ && populate_result_->release) populate_result_->release(*renderer_);
        for (const sim::AssetId asset : assets_) static_cast<void>(renderer_->release(asset));
        assets_.clear();
    }
    [[nodiscard]] bool write_report() const;
    // Puts a live source's stop message on screen once and prints it (#316 review 2).
    void show_live_error(const std::string& message);

    const SpaceEnvironment::Options& options_;
    Node3D* host_{};
    std::string live_error_;
    const vfs::Vfs* filesystem_{};
    std::unique_ptr<GodotRenderer> renderer_;
    FixedCamera camera_;
    std::unique_ptr<viewer::MapCameraBridge> bridge_;
    // #82: FoC's tactical overview over an interactive space map camera, and the clock its wheel
    // clicks are timed on. Absent for a locked (capture) camera and the camera self-tests.
    std::optional<tactical::TacticalOverview> overview_;
    std::string overview_status_{"unavailable"};
    std::chrono::steady_clock::time_point overview_clock_{std::chrono::steady_clock::now()};
    std::string camera_source_{"default"};
    std::string camera_status_{"not_built"};
    space::DefaultCamera default_camera_;
    TacticalLoad tactical_;
    assets::Vec3f eye_source_;
    assets::Vec3f target_source_;
    assets::Vec3f up_source_{0.0F, 0.0F, 1.0F};
    std::string environment_name_;
    std::size_t environment_count_{};
    std::string primary_sky_;
    std::string secondary_sky_;
    std::optional<space::EnvironmentLight> light_;
    std::vector<Item> items_;
    std::vector<std::string> not_rendered_;
    std::vector<std::uint32_t> environment_records_;
    std::vector<sim::AssetId> assets_;
    std::vector<sim::RenderInstance> instances_;
    std::vector<std::pair<std::size_t, space::Affine>> sky_instances_;
    // Planet and Nebula surfaces: their eawr_effect_time follows the effect
    // clock (#185), set when its tick changes.
    std::vector<sim::AssetId> effect_clock_assets_;
    std::optional<std::uint64_t> effect_tick_;
    std::size_t populated_{};
    // Where the population's instances start in instances_.
    std::size_t populate_first_{};
    // Mid-run captures of a live population: file name and PNG SHA-256.
    std::vector<std::pair<std::string, std::string>> live_captures_;
    double clock_seconds_{};
    std::optional<SpacePopulateResult> populate_result_;
    bool effects_released_{};
    std::shared_ptr<const sim::RenderSnapshot> snapshot_;
    std::map<std::string, std::pair<std::string, std::optional<assets::Texture>>> textures_;
    std::map<std::string, std::string> texture_failures_;
    sim::AssetId next_asset_{1};
    std::uint32_t frame_{};
    bool completed_{};
    std::chrono::steady_clock::time_point timing_start_{};
    double timed_seconds_{};
    std::string capture_hash_;
    // The read-back PNG's size, reported beside the camera identity.
    std::optional<std::array<std::uint32_t, 2>> capture_size_;
    std::string catalog_failure_;
    std::string status_{"failed"};
    std::string failure_;
};

} // namespace space_environment_detail

using namespace space_environment_detail;

struct SpaceEnvironment::State final {
    explicit State(Options value) : options(std::move(value)) {}

    Options options;
    // The default environment view; null on the evidence harness path.
    std::unique_ptr<EnvironmentView> view;
    std::unique_ptr<GodotRenderer> renderer;
    FixedCamera camera;
    space::CameraStatus camera_status{space::CameraStatus::malformed};
    std::string control{"none"};

    space::SkyPlan plan;
    bool plan_built{};
    // The MeshGloss route's external inputs; a labelled control may swap it.
    space::SkyLightPolicy sky_light = space::unlit_sky_policy();
    // The labelled meshadditive-synthetic control: route opt-in and inputs.
    AdditiveControl additive;
    std::uint64_t declared_placements{};
    std::string catalog_failure;

    // The reported control: the labelled meshadditive-synthetic control keeps
    // its name, with the scene control it carries (`+occluder`) appended.
    [[nodiscard]] std::string control_label() const {
        if (!additive.enabled) return control;
        return std::string(meshadditive_control) + (control == "none" ? "" : "+" + control);
    }

    struct Uploaded final {
        std::size_t surface{};
        sim::AssetId asset{};
        sim::EntityId entity{};
        bool submitted{true};
    };
    std::vector<Uploaded> uploaded;
    // Per plan surface: "not_attempted", "compiled", "rejected".
    std::vector<std::string> compiler;
    std::vector<std::string> upload_failure;
    std::string load_status{"not_attempted"};
    std::string material_status{"not_attempted"};
    std::string submission_status{"not_attempted"};
    std::string pixel_status{"not_attempted"};

    // The foreground control (occluder or lit receiver) shares one asset.
    std::optional<space::ScreenMask> occluder_mask;
    bool occluder_uploaded{};
    std::string foreground_role{"occluder_control"};
    // Only the sky-shadow-cast positive control lets the sky cast.
    bool sky_casts{};
    std::string lit_foreground{"not_executed"};

    std::vector<GodotRenderer::SubmissionEvidence> observed;
    std::vector<std::string> missing_submissions;

    // Requested capture/mask files, by path; a failed one names its cause.
    std::vector<std::string> artifacts_written;
    std::vector<std::string> artifacts_failed;

    std::uint32_t lifecycle_frames{};
    std::vector<std::size_t> lifecycle_instances_before;
    std::vector<std::size_t> lifecycle_resources_before;
    std::vector<std::size_t> lifecycle_instances_after;
    std::vector<std::size_t> lifecycle_released;
    std::vector<std::size_t> lifecycle_reuploaded;
    std::size_t partial_failure_remaining{};
    bool partial_failure{};
    std::optional<std::size_t> teardown_remaining;
    std::optional<std::size_t> teardown_instances;

    std::shared_ptr<const sim::RenderSnapshot> configured_snapshot;
    std::shared_ptr<const sim::RenderSnapshot> disabled_snapshot;
    std::vector<std::shared_ptr<const sim::RenderSnapshot>> isolated_snapshots;

    struct Phase final {
        std::string name;
        std::shared_ptr<const sim::RenderSnapshot> snapshot;
        std::optional<std::size_t> isolated;
        // Space fog phases (0: none). Their snapshot is made when the phase
        // starts, since painting changes the presentation grid and stream.
        std::uint8_t fog_step{};
    };
    std::deque<Phase> phases;
    bool phase_started{};
    std::uint32_t phase_frames{};
    std::map<std::string, std::vector<std::byte>> captures;
    // The configured capture's read-back size, reported beside the camera.
    std::optional<std::array<std::uint32_t, 2>> capture_size;
    std::map<std::string, std::string> capture_hashes;
    std::vector<std::optional<std::vector<std::byte>>> isolated_captures;

    space::PixelEvaluation pixels;
    std::uint32_t frame{};
    std::chrono::steady_clock::time_point timing_start{};
    double timed_seconds{};
    bool completed{};
    std::string status{"failed"};
    std::string failure;

    // P1 #30 space tactical camera; null without Options::map_camera.
    std::unique_ptr<viewer::MapCameraBridge> bridge;
    Node3D* host{};
    std::string camera_source{"--eawr-space-camera (synthetic capture input; no corpus camera is frozen)"};
    FixedCamera camera_initial;
    std::uint64_t focus_notifications{};
    std::uint64_t resize_notifications{};
    bool settle_started{};
    std::uint32_t settle_frames{};
    std::uint64_t steps_at_freeze{};
    bool terminal_held_at_step{};
    bool resize_active_at_capture{};
    Vector2i window_size{};
    std::array<float, 3> mark{};
    float zoom_mark{};
    float target_zoom_mark{};
    float distance_mark{};
    float distance_step{};
    float yaw_mark{};
    float pitch_mark{};
    float orbit_radius_mark{};
    std::uint64_t generation_mark{};
    bool pan_moved{};
    bool edge_changed_only_target{};
    std::vector<std::pair<std::string, bool>> checks;
    bool compare_started{};
    std::uint32_t compare_frames{};
    std::vector<std::byte> interactive_capture;
    std::string drawn_status{"not_attempted"};
    std::uint64_t drawn_changed_pixels{};
    std::uint64_t drawn_sampled_pixels{};

    // P1-07 #28 opt-in synthetic space fog; inert unless options.fog is set.
    std::optional<SpaceFogUnits> fog_units;
    std::vector<sim::RenderInstance> sky_instances;
    std::uint64_t fog_renderer_stream{};
    struct FogUnitEvidence final {
        bool potentially_visible{};
        std::uint64_t mask_pixels{};
        std::uint64_t interior_pixels{};
        std::uint64_t lit_interior{};
        std::uint64_t dark_interior{};
        std::uint64_t changed_interior{};
        std::array<double, 3> mean_rgb{};
        bool submitted{};
        bool fog_material{};
    };
    std::vector<FogUnitEvidence> fog_evidence;
    std::string fog_status{"not_requested"};
    std::string fog_failure;
    std::uint64_t fog_outside_pixels{};
    std::uint64_t fog_changed_outside{};
    std::uint64_t fog_differing_outside{};
    std::map<std::string, std::string> fog_phase_hashes;
    std::map<std::string, std::uint64_t> fog_phase_uploads;
    std::map<std::string, std::uint64_t> fog_phase_updates;
    std::map<std::string, std::uint64_t> fog_phase_revisions;
    std::vector<GodotRenderer::SubmissionEvidence> fog_observed;
    std::vector<GodotRenderer::FogConsumerEvidence> fog_consumers;
    std::optional<GodotRenderer::FogStatus> fog_final;
    [[nodiscard]] static std::string_view fog_label(const std::uint8_t step) {
        return step == 1 ? "source" : (step == 2 ? "painted" : "restored");
    }
    [[nodiscard]] std::shared_ptr<const sim::RenderSnapshot> fog_phase_snapshot(std::uint8_t step);
    [[nodiscard]] bool fog_bound();
    void evaluate_fog();
    void write_fog_report(std::ostream& output) const;

    [[nodiscard]] bool interactive() const { return bridge && !bridge->controller().capture_locked(); }
    [[nodiscard]] bool activate_camera(Node3D& node);
    void record_submissions();
    void selftest_tick(std::uint32_t tick);
    [[nodiscard]] bool selftest_passed() const {
        return !options.camera_selftest || (checks.size() == selftest_check_count
            && std::all_of(checks.begin(), checks.end(), [](const auto& entry) { return entry.second; }));
    }
    [[nodiscard]] std::optional<int> interactive_process(double delta);
    [[nodiscard]] std::optional<int> finish_interactive();
    [[nodiscard]] std::optional<int> fail(std::string message) {
        completed = true;
        static_cast<void>(give_up(std::move(message)));
        return 2;
    }

    bool give_up(std::string message, std::string outcome = "failed") {
        failure = std::move(message);
        status = std::move(outcome);
        release_all();
        static_cast<void>(write_report());
        return false;
    }
    [[nodiscard]] bool persist(const std::filesystem::path& path, const std::span<const std::byte> bytes) {
        const std::string problem = write_file(path, bytes);
        if (problem.empty()) {
            artifacts_written.push_back(path.generic_string());
            return true;
        }
        artifacts_failed.push_back(path.generic_string() + ": " + problem);
        return false;
    }
    void release_all();
    [[nodiscard]] bool upload_all(const std::string& control_name);
    [[nodiscard]] bool compose_occluder();
    [[nodiscard]] bool compose_receiver();
    [[nodiscard]] std::optional<int> lifecycle_step();
    [[nodiscard]] std::optional<int> finish();
    [[nodiscard]] bool write_report() const;
    void write_camera_report(std::ostream& output, int precision) const;
};

} // namespace eawr::presentation::godot_backend
