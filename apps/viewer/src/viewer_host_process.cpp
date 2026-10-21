#include "viewer_host_internal.hpp"
#include "loading_screen.hpp"
#include "audio_output.hpp"
#include "startup_trace.hpp"
#include "settings_store.hpp"
#include "gpu_memory_trace.hpp"
#include "galactic/galactic_launch_options.hpp"
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/image.hpp>

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

void ViewerHost::open_skirmish_loading() {
    const skirmish::SetupMap* map = skirmish_setup_->map(*pending_start_->map);
    auto* screen = memnew(EawrLoadingScreen);
    EawrLoadingScreen::Options screen_options;
    screen_options.content = skirmish_setup_->content();
    screen_options.map_path = *pending_start_->map;
    if (map != nullptr) {
        screen_options.context_name = map->context_name;
        screen_options.map_name = map->display_name.empty() ? map->name : map->display_name;
    }
    if (!screen->build(*skirmish_battle_host_, std::move(screen_options))) {
        // The battle still starts, behind the HUD's own Begin (WBF-10).
        UtilityFunctions::printerr(String::utf8(("EAWR loading screen: " + screen->failure()).c_str()));
        memdelete(screen);
        return;
    }
    skirmish_loading_ = screen;
}

void ViewerHost::leave_campaign_to_menu() {
    UtilityFunctions::print("EAWR campaign left for the main menu");
    world_capture_.clear();
    galactic_mode_->close();
    galactic_mode_.reset();
    if (galactic_host_) {
        remove_child(galactic_host_);
        memdelete(galactic_host_);
        galactic_host_ = nullptr;
    }
    front_end_->return_to_main_menu();
}

void ViewerHost::start_skirmish_battle() {
    auto selected = std::exchange(pending_start_, std::nullopt);
    const std::string map_path = *selected->map;
    gpu_memory_trace.sample("battle_start_from_setup");
    gpu_memory_trace.battle_started();
    map_mode_ = std::make_unique<MapMode>(MapMode::Options{
        .game_root = options_->game_root, .mod_root = options_->mod_root,
        .profile = options_->profile, .map_path = map_path,
        .report_path = options_->report_path,
        .capture_path = options_->skirmish_setup_test ? options_->capture_path.parent_path()
            / (options_->capture_path.stem().string() + ".battle.png") : std::filesystem::path{},
        .populate = true,
        .lighting = "sh", .shadows = "on", .environment = "map",
        .interactive = !options_->skirmish_setup_test, .setup = std::move(selected),
        .content = options_->cache_content ? skirmish_setup_->content() : nullptr,
        .shaders = skirmish_setup_->shaders(),
        .loading = skirmish_loading_, .lan = skirmish_setup_->lan_match()});
    if (!map_mode_->ready(*skirmish_battle_host_)) {
        map_mode_.reset();
        remove_child(skirmish_battle_host_);
        memdelete(skirmish_battle_host_);
        skirmish_battle_host_ = nullptr;
        skirmish_loading_ = nullptr;
        skirmish_setup_->show("This battle could not start. Check the map and factions or the run report.");
        if (options_->skirmish_setup_test) stop(2);
    }
}

void ViewerHost::start_replay_battle(const UiGalleryMode::ReplayRequest& request, const bool interactive) {
    // A recording without a map record is a legacy fixture; those were all recorded on this map.
    const std::string map_path = request.map.empty() ? "data/art/maps/_mp_space_coruscant.ted" : request.map;
    const auto beside = [](const std::filesystem::path& path, const char* suffix) {
        if (path.empty()) return std::filesystem::path{};
        std::filesystem::path name = path.stem(); // keeps a non-ASCII stem; only ASCII is appended
        name += suffix;
        return path.parent_path() / name;
    };
    // Its own host node, as the skirmish battle has, so a refused recording leaves nothing behind.
    auto* battle_host = memnew(Node3D);
    add_child(battle_host);
    map_mode_ = std::make_unique<MapMode>(MapMode::Options{
        .game_root = options_->game_root, .mod_root = options_->mod_root,
        .profile = options_->profile, .map_path = map_path,
        .report_path = beside(options_->report_path, ".replay.json"),
        .capture_path = beside(options_->capture_path, ".replay.png"),
        .populate = true,
        .lighting = "sh", .shadows = "on", .environment = "map",
        .interactive = interactive,
        .shaders = options_->cache_shaders ? std::make_shared<GodotShaderCache>() : nullptr,
        .replay_input = request.path});
    if (!map_mode_->ready(*battle_host)) {
        const std::string reason(map_mode_->failure());
        map_mode_.reset();
        remove_child(battle_host);
        memdelete(battle_host);
        if (!interactive) {
            stop(2);
            return;
        }
        // Back to the Load Game page with the reason; the viewer stays up.
        UiGalleryMode::Options options = UiGalleryMode::from_command_line();
        options.notice = reason.empty() ? std::string("The recording could not be played.") : reason;
        ui_gallery_mode_ = std::make_unique<UiGalleryMode>(std::move(options));
        if (!ui_gallery_mode_->ready(*this)) stop(2);
        return;
    }
    get_viewport()->connect("size_changed", callable_mp(this, &ViewerHost::on_map_viewport_size_changed));
}

void ViewerHost::_process(const double delta) {
    if (completed_) return;
    if (!options_mode_ && !map_mode_) {
        if (const auto* active = ActiveSettings::get()) {
            const bool menu = !map_mode_ && !galactic_mode_
                && (front_end_ || skirmish_setup_ || lan_lobby_mode_ || attract_runner_);
            update_frame_pacing(active->video, menu);
        }
    }
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
    if (options_mode_) {
        if (const std::optional<int> exit_code = options_mode_->process()) stop(*exit_code);
        return;
    }
    if (attract_runner_) {
        if (const std::optional<int> exit_code = attract_runner_->standalone_frame(delta, *this)) stop(*exit_code);
        return;
    }
    if (ui_gallery_mode_) {
        if (const std::optional<int> exit_code = ui_gallery_mode_->process()) {
            if (const auto replay = ui_gallery_mode_->take_replay_request(); replay && *exit_code == 0) {
                ui_gallery_mode_.reset();
                // A run with a capture is a test: the replay runs its frame window and ends.
                start_replay_battle(*replay, options_->capture_path.empty());
            } else {
                stop(*exit_code);
            }
        }
        return;
    }
    if (lan_lobby_mode_) {
        if (const std::optional<int> exit_code = lan_lobby_mode_->process()) stop(*exit_code);
        return;
    }
    if (galactic_mode_) {
        process_campaign_frame(delta);
        return;
    }
    // UI-07: a modal that opened or closed since the last event ends held world input before
    // the world or the camera steps (a held key has no events to notice it by).
    sync_modal_holds();
    if (input_routing_mode_) {
        if (const std::optional<int> exit_code = input_routing_mode_->process()) stop(*exit_code);
        return;
    }
    if (front_end_ && !map_mode_) {
        // The menus and the settings screen are the whole frame; the setup screen runs below.
        if (const std::optional<int> exit_code = front_end_->process()) {
            stop(*exit_code);
            return;
        }
        if (front_end_->owns_frame()) return;
    }
    if (map_mode_) {
        process_battle_frame(delta);
        return;
    }
    if (skirmish_setup_) {
        process_battle_setup_frame();
        return;
    }
    if (!renderer_) return;
    ++frame_;
    if (tactical_camera_) {
        process_camera_exercise_frame(delta);
        return;
    }
    if (atlas_overlay_) {
        process_atlas_exercise_frame();
        return;
    }
    if (runtime_exercise_) {
        process_renderer_exercise_frame();
        return;
    }
    process_replay_frame();
}

void ViewerHost::process_campaign_frame(const double delta) {
    sync_modal_holds();
    if (const std::optional<int> exit_code = galactic_mode_->process(delta)) {
        // #4039: a failed campaign ends the session; say why, or the log stops at the host teardown.
        if (*exit_code != 0) UtilityFunctions::printerr("EAWR galactic mode ended with exit code ", *exit_code, ": ",
            String::utf8(galactic_mode_->failure().c_str()));
        stop(*exit_code);
    }
    else if (const auto destination = galactic_mode_->score_destination();
             galactic_mode_->leave_requested()
             || (destination && *destination == platform::CampaignScoreDestination::main_menu)) {
        // The in-game menu's Quit Game and the score screen's Main Menu: the host owns what they
        // mean. With the front end behind the campaign it is the main menu; a run started
        // straight into the mode has no menu to show and ends.
        if (galactic_host_ && front_end_) leave_campaign_to_menu();
        else {
            UtilityFunctions::print(galactic_mode_->leave_requested()
                ? "EAWR campaign exit: host ends the session (no main menu behind it)"
                : "EAWR score exit: host ends the session (no main menu behind it)");
            stop(0);
        }
    } else if (destination && *destination != platform::CampaignScoreDestination::none) {
        // The LAN lobby has no screen to return to from a campaign yet: the host ends the session.
        UtilityFunctions::print("EAWR score exit: host ends the session (no LAN lobby behind it)");
        stop(0);
    }
}

void ViewerHost::process_battle_frame(const double delta) {
    if (const std::optional<int> exit_code = map_mode_->process(delta)) {
        if (!skirmish_setup_) stop(*exit_code);
        else {
            if (skirmish_setup_->lan_match() && !options_->capture_path.empty()) {
                const auto path = options_->capture_path.parent_path() / (options_->capture_path.stem().string() + ".battle.png");
                const auto image = get_viewport()->get_texture()->get_image();
                if (image.is_null() || image->save_png(String::utf8(ViewerPath::utf8(path).c_str())) != OK) {
                    UtilityFunctions::printerr("EAWR LAN result capture failed: ", String::utf8(ViewerPath::utf8(path).c_str()));
                    stop(2); return;
                }
            }
            UtilityFunctions::print("EAWR battle left with exit code ", *exit_code);
            const auto return_start = StartupTrace::Clock::now();
            world_capture_.clear();
            // Quit Game in the battle's menu goes to the main menu, not back to the setup screen.
            const bool to_menu = front_end_ && map_mode_->leave_to_menu();
            map_mode_.reset();
            remove_child(skirmish_battle_host_);
            memdelete(skirmish_battle_host_);
            skirmish_battle_host_ = nullptr;
            skirmish_loading_ = nullptr;
            gpu_memory_trace.battle_released();
            if (to_menu) {
                // The shell leaves the setup screen on its next frame and shows the main menu.
                front_end_->return_to_main_menu();
                startup_trace.returning(return_start);
                return;
            }
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
}

void ViewerHost::process_battle_setup_frame() {
    skirmish_setup_->process();
    gpu_memory_trace.setup_frame();
    if (options_->skirmish_setup_returned && ++frame_ >= 30) { stop(0); return; }
    if (pending_start_) {
        // The screen's Controls draw once in a normal frame before the preparation draws it on the spot.
        if (skirmish_loading_ == nullptr || skirmish_loading_->frames() >= 2) start_skirmish_battle();
        return;
    }
    if (auto lan = skirmish_setup_->lan_match()) {
        if (auto launch = lan->take_galactic_launch()) {
            GalacticMode::Options campaign_options;
            viewer::galactic::apply_lobby_launch(campaign_options, std::move(launch));
            campaign_options.game_root = options_->game_root;
            campaign_options.mod_root = options_->mod_root;
            if (!skirmish_setup_->gc_acceptance_directory().empty()) {
                campaign_options.lan_acceptance_directory = skirmish_setup_->gc_acceptance_directory();
                campaign_options.report_path = campaign_options.lan_acceptance_directory / "report.json";
            }
            auto* campaign_host = memnew(Node3D);
            add_child(campaign_host);
            auto campaign = std::make_unique<GalacticMode>(std::move(campaign_options));
            if (!campaign->ready(*campaign_host)) {
                const auto reason = campaign->failure();
                campaign.reset(); remove_child(campaign_host); memdelete(campaign_host);
                lan->cancel();
                skirmish_setup_->show(reason.empty() ? "The campaign could not be started." : reason);
                return;
            }
            skirmish_setup_->hide();
            if (front_end_) front_end_->campaign_started_by_host();
            galactic_mode_ = std::move(campaign); galactic_host_ = campaign_host;
            return;
        }
    }
    if (auto selected = skirmish_setup_->take_start()) {
        skirmish_setup_->hide();
        skirmish_battle_host_ = memnew(Node3D);
        add_child(skirmish_battle_host_);
        pending_start_ = std::move(selected);
        // BLD-01: a player's battle always loads behind the screen; a setup test only when it waits for Begin.
        if (!options_->skirmish_setup_test || options_->live_begin == "manual") open_skirmish_loading();
        if (skirmish_loading_ == nullptr) start_skirmish_battle();
    }
}

void ViewerHost::process_camera_exercise_frame(const double delta) {
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
}

void ViewerHost::process_atlas_exercise_frame() {
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
}

void ViewerHost::process_renderer_exercise_frame() {
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
}

void ViewerHost::process_replay_frame() {
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
