#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/sim/snapshot.hpp"

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <chrono>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace eawr::godot_prototype {

class EawrGodotPrototype final : public godot::Node3D {
    GDCLASS(EawrGodotPrototype, godot::Node3D)

public:
    EawrGodotPrototype();
    ~EawrGodotPrototype() override;

    void _ready() override;
    void _process(double delta) override;
    void _input(const godot::Ref<godot::InputEvent>& event) override;

protected:
    static void _bind_methods();

private:
    struct Options;
    bool initialize(const Options& options);
    bool load_common_scene(const Options& options);
    bool load_replay_snapshots(const Options& options);
    bool create_render_resources();
    bool create_mesh();
    bool create_texture();
    bool create_material();
    void create_camera_light_environment();
    void apply_snapshot(std::size_t index, bool record_comparable_sample = false);
    void update_camera();
    void finish_measurement();
    void write_report(bool headless_probe, const std::string& status, const std::string& failure = {});
    void cleanup();

    std::unique_ptr<Options> options_;
    assets::Model model_;
    assets::Texture texture_;
    std::vector<const assets::Submesh*> matching_submeshes_;
    std::vector<std::shared_ptr<const sim::RenderSnapshot>> snapshots_;
    std::vector<std::string> replay_hashes_;
    std::vector<double> cpu_samples_ms_;
    std::vector<double> comparable_snapshot_samples_ms_;
    std::vector<double> frame_cadence_samples_ms_;
    std::vector<godot::RID> entity_instances_;

    godot::RID mesh_;
    godot::RID texture_rid_;
    godot::RID shader_;
    godot::RID material_;
    godot::RID alpha_shader_;
    godot::RID alpha_material_;
    godot::RID modern_shader_;
    godot::RID modern_material_;
    godot::RID camera_;
    godot::RID light_;
    godot::RID light_instance_;
    godot::RID environment_;
    godot::RID scenario_;
    godot::RID viewport_;

    std::uint64_t frame_{0};
    std::uint64_t draw_calls_{0};
    std::uint64_t object_count_{0};
    std::uint64_t vertex_count_{0};
    std::uint64_t index_count_{0};
    std::uint64_t load_time_us_{0};
    std::chrono::steady_clock::time_point previous_frame_start_{};
    std::array<double, 3> bounds_min_{};
    std::array<double, 3> bounds_max_{};
    std::string renderer_name_;
    std::string renderer_vendor_;
    std::string renderer_device_;
    std::string renderer_api_;
    std::string scene_hash_;
    std::string model_hash_;
    std::string texture_hash_;
    std::string report_status_{"initializing"};
    std::array<double, 3> closeup_center_{};
    std::array<double, 3> closeup_eye_{};
    double closeup_radius_{0.0};
    double orbit_yaw_{0.0};
    double orbit_pitch_{0.0};
    double orbit_distance_{1129.0};
    bool dragging_{false};
    bool fixed_camera_{true};
    bool completed_{false};
    bool control_probe_exercised_{false};
    bool closeup_camera_{false};
    bool closeup_capture_done_{false};
    bool baseline_capture_restored_{false};
    bool have_previous_frame_start_{false};
    std::uint64_t modern_smoke_draw_calls_{0};
};

} // namespace eawr::godot_prototype
