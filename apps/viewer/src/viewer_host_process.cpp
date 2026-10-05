#include "viewer_host_internal.hpp"
#include "audio_output.hpp"
#include "startup_trace.hpp"
#include <godot_cpp/classes/rendering_server.hpp>

namespace eawr::presentation::godot_backend {

namespace {

[[nodiscard]] bool write_capture(
    const std::filesystem::path& path,
    const std::span<const std::byte> bytes,
    std::string& failure) {
    if (bytes.empty()) {
        failure = "renderer returned an empty capture";
        return false;
    }
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        failure = "capture exceeds the platform write limit";
        return false;
    }
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) {
            failure = "capture output directory could not be created";
            return false;
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        failure = "capture output could not be opened";
        return false;
    }
    output.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        failure = "capture output could not be written completely";
        return false;
    }
    return true;
}
} // namespace

void ViewerHost::_process(const double delta) {
    if (completed_) return;
    if (options_ && options_->window_resize_pending) {
        options_->window_resize_pending = false;
        const Vector2i before = get_window()->get_size();
        get_window()->set_size(*options_->window_resize_test);
        const Vector2i after = get_window()->get_size();
        const Vector2 viewport = get_viewport()->get_visible_rect().size;
        UtilityFunctions::print(String((std::string("EAWR window resize test: window ") + std::to_string(before.x)
            + "x" + std::to_string(before.y) + " -> " + std::to_string(after.x) + "x" + std::to_string(after.y)
            + ", viewport " + std::to_string(static_cast<int>(viewport.x)) + "x"
            + std::to_string(static_cast<int>(viewport.y))).c_str()));
    }
    if (unit_mode_) {
        if (const std::optional<int> exit_code = unit_mode_->process()) stop(*exit_code);
        return;
    }
    if (effect_mode_) {
        if (const std::optional<int> exit_code = effect_mode_->process()) stop(*exit_code);
        return;
    }
    if (font_mode_) {
        if (const std::optional<int> exit_code = font_mode_->process()) stop(*exit_code);
        return;
    }
    if (ui_gallery_mode_) {
        if (const std::optional<int> exit_code = ui_gallery_mode_->process()) stop(*exit_code);
        return;
    }
    // UI-07: a modal that opened or closed since the last event ends held world input before
    // the world or the camera steps (a held key has no events to notice it by).
    sync_modal_holds();
    if (input_routing_mode_) {
        if (const std::optional<int> exit_code = input_routing_mode_->process()) stop(*exit_code);
        return;
    }
    if (map_mode_) {
        if (const std::optional<int> exit_code = map_mode_->process(delta)) {
            if (!skirmish_setup_) stop(*exit_code);
            else {
                const auto return_start = StartupTrace::Clock::now();
                world_capture_.clear();
                map_mode_.reset();
                remove_child(skirmish_battle_host_);
                memdelete(skirmish_battle_host_);
                skirmish_battle_host_ = nullptr;
                skirmish_setup_->show(*exit_code == 0 ? "Battle finished. Choose your next battle." : "Battle could not finish; see the run report.");
                startup_trace.returning(return_start);
                RenderingServer::get_singleton()->connect("frame_post_draw",
                    callable_mp(this, &ViewerHost::on_setup_frame_drawn), Object::CONNECT_ONE_SHOT);
                if (options_->skirmish_setup_test) {
                    if (*exit_code != 0) { stop(*exit_code); return; }
                    options_->skirmish_setup_returned = ++options_->load_bench_completed >= options_->load_bench_starts;
                    frame_ = 0;
                }
            }
        }
        if (startup_trace.pending() && map_mode_) {
            auto* server = RenderingServer::get_singleton();
            const auto callback = callable_mp(this, &ViewerHost::on_battle_frame_drawn);
            if (!server->is_connected("frame_post_draw", callback))
                server->connect("frame_post_draw", callback, Object::CONNECT_ONE_SHOT);
        }
        return;
    }
    if (skirmish_setup_) {
        skirmish_setup_->process();
        if (options_->skirmish_setup_returned && ++frame_ >= 30) { stop(0); return; }
        if (auto selected = skirmish_setup_->take_start()) {
            skirmish_setup_->hide();
            skirmish_battle_host_ = memnew(Node3D);
            add_child(skirmish_battle_host_);
            map_mode_ = std::make_unique<MapMode>(MapMode::Options{
                .game_root = options_->game_root, .mod_root = options_->mod_root,
                .profile = options_->profile, .map_path = *selected->map,
                .report_path = options_->report_path,
                .capture_path = options_->skirmish_setup_test ? options_->capture_path.parent_path()
                    / (options_->capture_path.stem().string() + ".battle.png") : std::filesystem::path{},
                .populate = true,
                .lighting = "sh", .shadows = "on", .environment = "map",
                .interactive = !options_->skirmish_setup_test, .setup = std::move(selected),
                .content = options_->cache_content ? skirmish_setup_->content() : nullptr,
                .shaders = skirmish_setup_->shaders()});
            if (!map_mode_->ready(*skirmish_battle_host_)) {
                map_mode_.reset();
                remove_child(skirmish_battle_host_);
                memdelete(skirmish_battle_host_);
                skirmish_battle_host_ = nullptr;
                skirmish_setup_->show("This battle could not start. Check the map and factions or the run report.");
                if (options_->skirmish_setup_test) { stop(2); return; }
            }
        }
        return;
    }
    if (!renderer_) return;
    ++frame_;
    if (tactical_camera_) {
        constexpr std::uint64_t camera_capture_frame = 8;
        if (camera_interaction_) {
            if (!step_camera_interaction(delta)) {
                static_cast<void>(write_report("failed"));
                stop(2);
                return;
            }
            if (completed_) return;
            if (!camera_interaction_->capture_locked_for_run) {
                // Interactive run without a capture: keep rendering until the
                // window closes or the self-test reports.
                renderer_->submit(snapshots_.front());
                return;
            }
        }
        renderer_->submit(snapshots_.front());
        if (frame_ < camera_capture_frame) return;
        auto capture = renderer_->capture(capture_camera_);
        if (!capture) {
            status_message_ = core::format_diagnostic(capture.error());
            static_cast<void>(write_report("failed"));
            stop(2);
            return;
        }
        capture_hash_ = hash_bytes(capture.value().png_bytes);
        tactical_camera_verified_ = verify_draw_capture(capture.value());
        if (!options_->capture_path.empty()
            && !write_capture(options_->capture_path, capture.value().png_bytes,
                              status_message_)) {
            static_cast<void>(write_report("failed"));
            stop(2);
            return;
        }
        if (!tactical_camera_verified_) {
            status_message_ =
                "the solved tactical camera framed no distinguishable geometry";
            static_cast<void>(write_report("failed"));
            stop(2);
            return;
        }
        stop(write_report("tactical_camera_exercise_passed") ? 0 : 2);
        return;
    }
    if (atlas_overlay_) {
        // The canvas quad is static, so a small settle window is enough for the
        // engine to present it before the viewport texture is read back.
        constexpr std::uint64_t atlas_capture_frame = 8;
        if (frame_ < atlas_capture_frame) return;
        auto capture = renderer_->capture(capture_camera_);
        if (!capture) {
            status_message_ = core::format_diagnostic(capture.error());
            static_cast<void>(write_report("failed"));
            stop(2);
            return;
        }
        capture_hash_ = hash_bytes(capture.value().png_bytes);
        atlas_overlay_verified_ = verify_atlas_capture(capture.value());
        if (!options_->capture_path.empty()
            && !write_capture(options_->capture_path, capture.value().png_bytes, status_message_)) {
            static_cast<void>(write_report("failed"));
            stop(2);
            return;
        }
        release_atlas_overlay();
        if (!atlas_overlay_verified_) {
            status_message_ =
                "atlas overlay capture did not prove the sampled rectangle and alpha contract";
            static_cast<void>(write_report("failed"));
            stop(2);
            return;
        }
        stop(write_report("atlas_overlay_exercise_passed") ? 0 : 2);
        return;
    }
    if (runtime_exercise_) {
        constexpr std::uint64_t lifecycle_frames = 40;
        const auto& runtime_snapshot = frame_ <= lifecycle_frames
            ? runtime_lifecycle_snapshots_[static_cast<std::size_t>(
                (frame_ - 1) % runtime_lifecycle_snapshots_.size())]
            : snapshots_.front();
        renderer_->submit(runtime_snapshot);
        ++runtime_scene_switches_;
        const std::size_t expected_instances = runtime_snapshot->instances().empty()
            ? 0U
            : std::count_if(runtime_snapshot->instances().begin(),
                runtime_snapshot->instances().end(), [](const sim::RenderInstance& instance) {
                    return instance.asset_id != 404;
                });
        if (renderer_->instance_count() != expected_instances) {
            status_message_ = "runtime scene switch left stale or missing instances";
            static_cast<void>(write_report("failed"));
            stop(2);
            return;
        }
        if (frame_ != 48) return;
        auto capture = renderer_->capture(capture_camera_);
        if (!capture) {
            status_message_ = core::format_diagnostic(capture.error());
            static_cast<void>(write_report("failed"));
            stop(2);
            return;
        }
        capture_hash_ = hash_bytes(capture.value().png_bytes);
        runtime_capture_verified_ = verify_runtime_capture(capture.value());
        if (!options_->capture_path.empty()
            && !write_capture(options_->capture_path, capture.value().png_bytes, status_message_)) {
            static_cast<void>(write_report("failed"));
            stop(2);
            return;
        }
        const auto observed = renderer_->submission_evidence();
        std::vector<RenderPass> observed_passes;
        for (const GodotRenderer::SubmissionEvidence& draw : observed) {
            if (observed_passes.empty() || observed_passes.back() != draw.pass) {
                observed_passes.push_back(draw.pass);
            }
        }
        const bool observed_order = observed_passes.size() == render_pass_order.size()
            && std::equal(observed_passes.begin(), observed_passes.end(),
                render_pass_order.begin());
        const bool released = renderer_->release(1) && renderer_->release(1)
            && renderer_->release(2) && renderer_->release(3) && renderer_->release(4);
        runtime_shutdown_resources_empty_ = released && renderer_->resources().empty()
            && renderer_->instance_count() == 0;
        if (!runtime_capture_verified_ || !observed_order || runtime_scene_switches_ < 40
            || !runtime_shutdown_resources_empty_) {
            status_message_ = !runtime_capture_verified_
                ? "runtime capture did not prove the overlap/post pixel contract"
                : (!observed_order ? "runtime submission evidence did not observe four-pass order"
                    : "runtime RID release left live resources or instances");
            static_cast<void>(write_report("failed"));
            stop(2);
            return;
        }
        // The report is the exercise's only evidence; a pass that could not be
        // persisted is a failed run, not a silent success. The marker lets a
        // persistence probe prove it failed on this path, not an earlier one.
        UtilityFunctions::print("EAWR runtime: all exercise checks passed; persisting report");
        stop(write_report("renderer_runtime_exercise_passed") ? 0 : 2);
        return;
    }
    // Match the accepted prototype's fixed-camera frame: restore the first
    // frozen-scene instance before capture, while ordinary frames still submit
    // every entity from the immutable replay snapshot.
    const std::shared_ptr<const sim::RenderSnapshot> snapshot =
        frame_ >= 118 && frame_ <= 120 && fixed_capture_snapshot_
        ? fixed_capture_snapshot_
        : snapshots_[static_cast<std::size_t>((frame_ / 100) % snapshots_.size())];
    renderer_->submit(snapshot);
    if (!apply_animation_pose(snapshot)) {
        static_cast<void>(write_report("failed"));
        stop(2);
        return;
    }
    if (frame_ != 120) return;
    // The exploratory Hull preview always captures in memory so its framing
    // is checked against real pixels, even when no PNG was requested.
    const bool exploratory_preview = model_preview_ && model_preview_->plan.exploratory;
    if (!options_->capture_path.empty() || exploratory_preview) {
        auto result = renderer_->capture(capture_camera_);
        if (!result) {
            status_message_ = core::format_diagnostic(result.error());
            static_cast<void>(write_report("failed"));
            stop(2);
            return;
        }
        const CaptureResult& capture = result.value();
        if (animation_pose_) {
            animation_capture_verified_ = verify_draw_capture(capture);
            if (!animation_capture_verified_) {
                status_message_ = "animation capture contained no distinguishable skinned draw";
                static_cast<void>(write_report("failed"));
                stop(2);
                return;
            }
        }
        if (exploratory_preview && !verify_preview_capture(capture)) {
            status_message_ = "exploratory Hull preview capture drew nothing distinguishable or "
                              "touched the viewport edge";
            capture_hash_ = hash_bytes(capture.png_bytes);
            static_cast<void>(write_report("failed"));
            stop(2);
            return;
        }
        if (!options_->capture_path.empty()
            && !write_capture(options_->capture_path, capture.png_bytes, status_message_)) {
            static_cast<void>(write_report("failed"));
            stop(2);
            return;
        }
        capture_hash_ = hash_bytes(capture.png_bytes);
    }
    // An exploratory preview never reports the acceptance-shaped "passed".
    if (!write_report(exploratory_preview ? "exploratory_preview_captured" : "passed")) {
        stop(2);
        return;
    }
    if (options_->benchmark) stop(0);
}
} // namespace eawr::presentation::godot_backend
