#pragma once

// Private to the viewer host translation units (viewer_host*.cpp): the host's
// nested run records, the namespace aliases they use and the helpers the
// units share.
#include "viewer_host.hpp"
#include "capture_viewport.hpp"
#include "model_preview.hpp"
#include "render_profile_viewport.hpp"
#include "shutdown_trace.hpp"
#include "viewer_path.hpp"

#include "diagnostic_buffer.hpp"

#include "eawr/core/diagnostic.hpp"
#include "eawr/presentation/camera/constants_source.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/world.hpp"
#include "eawr/vfs/vfs.hpp"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/world2d.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <utility>
#include <variant>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace viewer_host_detail {

[[nodiscard]] sim::math::Mat3x4 runtime_transform(
    const std::int64_t x, const std::int64_t z = 0);
[[nodiscard]] assets::Model runtime_model(const float half_extent = 110.0F);
[[nodiscard]] assets::Texture runtime_texture();
[[nodiscard]] MaterialDescription runtime_material(
    const RenderPass pass, const std::string_view source);


constexpr std::string_view expected_scene_hash =
    "de673739583babf0a4541feafc6bd1f0c40da36788a962e7d49b284be7611a18";


namespace model_preview = eawr::viewer::model_preview;
// The MC-50 model, Hangar texture and their SHA-256 pins live with the
// selection rules in model_preview.hpp.
constexpr std::string_view default_model_path = model_preview::pinned_model_path;

namespace tactical_camera = eawr::presentation::camera;
namespace camera_input = eawr::viewer::camera_input;

[[nodiscard]] std::string json(std::string_view value);
[[nodiscard]] std::optional<std::vector<std::byte>> read_bytes(const ViewerPath& path);
[[nodiscard]] std::string utf8(const String& value);
[[nodiscard]] bool ieq(std::string_view left, std::string_view right);
[[nodiscard]] std::string hash_bytes(std::span<const std::byte> bytes);
[[nodiscard]] std::optional<vfs::Vfs> mount_content_layers(
    const std::filesystem::path& game_root,
    const std::filesystem::path& mod_root,
    std::string_view profile,
    std::string& failure);
[[nodiscard]] std::array<float, 3> view_direction(const FixedCamera& camera);

} // namespace viewer_host_detail

using namespace viewer_host_detail;

struct ViewerHost::Options final {
    std::filesystem::path game_root;
    std::filesystem::path mod_root;
    std::string profile;
    std::string profile_error;
    // --eawr-render-profile retail|enhanced; empty picks default_render_profile().
    std::string render_profile;
    bool render_profile_missing{};
    ViewerPath scene_path{std::string{"res://common/scene.json"}};
    ViewerPath replay_path{std::string{"res://common/original-v1.eawr-replay"}};
    std::string model_path{default_model_path};
    std::string mesh_name;
    std::string texture_path;
    std::string animation_path;
    std::string map_path;
    bool skirmish_setup{};
    bool skirmish_setup_test{};
    bool skirmish_setup_returned{};
    bool startup_metrics{};
    bool cache_content{true};
    bool cache_shaders{true};
    std::uint32_t load_bench_starts{1}, load_bench_completed{};
    std::string atlas_path;
    std::string icon_name;
    std::string camera_mode;
    float camera_zoom{};
    bool camera_zoom_requested{};
    // P1-09 opt-in interaction. The default bindings are project-authored and
    // explicitly non-retail; see apps/viewer/project/config/camera-bindings.json.
    bool camera_interactive{};
    bool camera_input_selftest{};
    std::string camera_bindings_path{"res://config/camera-bindings.json"};
    float animation_time_seconds{};
    std::filesystem::path capture_path;
    std::filesystem::path report_path;
    // Test only: resize the OS window on the first frame, as a window manager
    // may, so a graphical test proves the capture keeps its requested size.
    std::optional<Vector2i> window_resize_test;
    bool window_resize_invalid{};
    bool window_resize_pending{};
    bool headless_probe{};
    bool benchmark{};
    bool renderer_runtime_test{};
};

// The opt-in exploratory Hull preview's selection identity, bounds-fitted
// framing and capture evidence. It exists only for that preview, so the frozen
// Hangar report schema is unchanged.
struct ViewerHost::ModelPreview final {
    model_preview::Plan plan;
    std::string mesh_name;
    std::int32_t mesh_bone{-1};
    std::size_t submesh_index{};
    std::size_t index_count{};
    std::string technique;
    std::string pass;
    std::string base_texture;
    model_preview::RestPlacement rest;
    model_preview::Bounds world;
    model_preview::Fit fit;
    // Decoded-capture evidence: distinguishable pixels against the corner
    // background and their bounding box. The Hull must not touch an edge.
    std::int64_t capture_width{};
    std::int64_t capture_height{};
    std::int64_t drawn_pixels{};
    std::int64_t drawn_min_x{-1};
    std::int64_t drawn_min_y{-1};
    std::int64_t drawn_max_x{-1};
    std::int64_t drawn_max_y{-1};
    bool capture_verified{};
};

struct ViewerHost::AtlasOverlay final {
    AtlasOverlay() = default;
    ~AtlasOverlay() { free_rids(); }
    AtlasOverlay(const AtlasOverlay&) = delete;
    AtlasOverlay& operator=(const AtlasOverlay&) = delete;

    void free_rids() {
        RenderingServer* rendering = RenderingServer::get_singleton();
        if (!rendering) return;
        if (canvas_item.is_valid()) rendering->free_rid(canvas_item);
        if (texture.is_valid()) rendering->free_rid(texture);
        canvas_item = RID();
        texture = RID();
    }

    std::string mtd_logical_path;
    std::string mtd_sha256;
    std::string page_logical_path;
    std::string page_sha256;
    std::string icon_name;
    std::string page_format;
    std::string page_origin;
    assets::AtlasRectangle rectangle{};
    bool icon_has_alpha{};
    bool icon_flip_x{};
    bool icon_flip_y{};
    std::uint32_t page_width{};
    std::uint32_t page_height{};
    Ref<Image> page_image;
    RID texture;
    RID canvas_item;
    std::int32_t scale{1};
    std::int32_t quad_x{};
    std::int32_t quad_y{};
    std::int32_t quad_width{};
    std::int32_t quad_height{};
    std::int64_t opaque_sampled{};
    std::int64_t opaque_non_background{};
    std::int64_t transparent_sampled{};
    std::int64_t transparent_background{};
    std::int64_t distinct_opaque_colours{};
    bool orientation_consistent{};
};

struct ViewerHost::TacticalCameraRun final {
    tactical_camera::Mode mode{tactical_camera::Mode::land};
    tactical_camera::Constants constants;
    tactical_camera::State state;

    std::string camera_logical_path;
    std::string camera_sha256;
    std::string constants_logical_path;
    std::string constants_sha256;

    // Recorded input-mapping evidence: the model is data driven, so the report
    // carries the values the bindings actually produced rather than a claim.
    float zoom_step{};
    float pan_x_per_second{};
    float push_pan_x_per_second{};
    int edge_scroll_left{};
    int edge_scroll_right{};
    float yaw_after_drag{};
    float pitch_after_drag{};
    std::array<float, 3> eye{};
};

struct ViewerHost::CameraInteraction final {
    CameraInteraction(const camera_input::Context context, const bool focused)
        : adapter(context, focused) {}

    camera_input::Adapter adapter;
    camera_input::TacticalPose pose;
    float default_zoom{};
    // The interactive render camera. capture_camera_ stays the pinned capture
    // authority and is never written by interaction code.
    FixedCamera camera;
    std::string bindings_path;
    std::string bindings_sha256;
    // A capture was requested: #22 precedence locks input for the whole run.
    bool capture_locked_for_run{};
    bool selftest{};
    std::uint64_t input_callbacks{};
    std::uint64_t focus_notifications{};
    std::uint64_t resize_notifications{};
    std::uint64_t steps{};
    std::uint64_t moving_steps{};
    std::uint64_t selftest_frame{};
    std::vector<std::pair<std::string, bool>> checks;
    camera_input::TacticalPose mark;
    FixedCamera capture_mark;
    FixedCamera interactive_mark;
    // Real window resize probe: the window size to restore and the counters
    // observed before the resize request.
    Vector2i window_size_mark;
    std::uint64_t resize_mark{};
    std::uint64_t generation_mark{};
    std::string rejected_event;
    // UI-07 modal probe: a modal layer the self-test opens over held camera input.
    godot::Control* selftest_modal{};

    // Free flight (schema v2 `free` context). The controller exists exactly
    // while free flight is active. `saved_*` hold the complete tactical state
    // taken at entry and are restored unchanged on exit; nothing tactical is
    // advanced while flying.
    std::optional<tactical_camera::FreeCameraController> free_controller;
    camera_input::TacticalPose saved_pose;
    FixedCamera saved_camera;
    camera_input::Context saved_context{camera_input::Context::land};
    // Most recent free pose (kept after exit for the report).
    std::optional<tactical_camera::FreeCameraPose> free_pose;
    std::uint64_t free_entries{};
    std::uint64_t free_exits{};
    std::uint64_t free_rejections{};
    std::uint64_t free_steps{};
    std::uint64_t free_moving_steps{};
    std::string free_rejection;
    // (kind, host frame) for every enter, exit and rejected entry.
    std::vector<std::pair<std::string, std::uint64_t>> free_transitions;
    tactical_camera::FreeCameraPose free_mark;
};

} // namespace eawr::presentation::godot_backend
