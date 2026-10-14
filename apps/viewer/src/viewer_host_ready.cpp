#include "viewer_host_internal.hpp"
#include "audio_buses.hpp"
#include "audio_output.hpp"
#include "gpu_memory_trace.hpp"
#include "settings_store.hpp"
#include "startup_trace.hpp"
#include <godot_cpp/classes/rendering_server.hpp>

namespace eawr::presentation::godot_backend {

namespace {


} // namespace

// Exceptions must not escape a GDExtension callback into the engine.
void ViewerHost::_ready() try {
    mute_lane_audio_output();
    set_process(true);
    // UI-07: a text control taking focus cancels held world input (UI-I3).
    get_viewport()->connect("gui_focus_changed", callable_mp(this, &ViewerHost::on_gui_focus_changed));
    options_ = std::make_unique<Options>();
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    read_host_options(arguments);

    if (options_->profile_error.empty() && !options_->profile.empty()
        && options_->profile != "eaw" && options_->profile != "foc" && options_->profile != "remake") {
        options_->profile_error = "--eawr-profile must be eaw, foc or remake";
    }
    if (!options_->profile_error.empty()) {
        status_message_ = options_->profile_error;
        UtilityFunctions::printerr(String(status_message_.c_str()));
        static_cast<void>(write_report("failed"));
        stop(2);
        return;
    }
    if (options_->window_resize_invalid
        || (options_->window_resize_test && options_->camera_interactive && options_->capture_path.empty())) {
        status_message_ = "--eawr-window-resize-test expects <width>x<height> (each 1..16384) and a probe, "
                          "not an interactive run without --eawr-capture";
        UtilityFunctions::printerr(String(status_message_.c_str()));
        static_cast<void>(write_report("failed"));
        stop(2);
        return;
    }
    const std::optional<RenderProfile> requested_render_profile = parse_render_profile(options_->render_profile);
    if (options_->render_profile_missing || (!options_->render_profile.empty() && !requested_render_profile)) {
        status_message_ = "--eawr-render-profile expects retail or enhanced";
        UtilityFunctions::printerr(String(status_message_.c_str()));
        static_cast<void>(write_report("failed"));
        stop(2);
        return;
    }

    // #2927: a player's view takes the saved settings (the window, vsync, the render profile, and
    // through ActiveSettings the battle's volumes and speed step); a capture, report or test run
    // never does, so evidence runs do not depend on the machine's settings file.
    const bool evidence_run = !options_->capture_path.empty() || !options_->report_path.empty()
        || options_->camera_input_selftest || options_->window_resize_test.has_value() || options_->headless_probe
        || options_->benchmark || options_->renderer_runtime_test || options_->startup_metrics;
    std::optional<RenderProfile> saved_render_profile;
    if (!evidence_run) {
        const SettingsStore store(options_->settings_file);
        if (store.exists()) {
            presentation::ui::SettingsLoad saved = store.load();
            if (!saved.defaulted) {
                ApplyReport applied;
                ApplyTargets targets;
                targets.texture_detail = !options_->texture_mip_drop_explicit;
                apply_video(saved.settings.video, targets, applied);
                // #3165: the saved volumes reach the shared buses before the front end's music starts.
                apply_audio(saved.settings.audio, applied);
                // The log is the receipt that a player's start-up took the saved record (#2927).
                for (const std::string& line : applied.applied) {
                    UtilityFunctions::print(String("EAWR settings applied: ") + String::utf8(line.c_str()));
                }
                for (const std::string& line : applied.skipped) {
                    UtilityFunctions::print(String("EAWR settings skipped: ") + String::utf8(line.c_str()));
                }
                if (saved.settings.video.render_profile != presentation::ui::RenderProfileSetting::automatic) {
                    saved_render_profile = resolve_render_profile(saved.settings.video.render_profile,
                                                                  RenderProfile::retail);
                }
                ActiveSettings::set(std::move(saved.settings));
            }
        }
    }

    // #3165: the shared audio buses exist from the viewer's start, front end included; a run
    // without saved settings has them at FoC's 0.75 (BattleAudio and MenuAudio use these).
    if (!audio_buses::exist()) audio_buses::ensure(audio::MixLevels{});

    // Retail for every capture and test, enhanced for a player's view (#184), or what the
    // player chose (#2927); --eawr-render-profile wins over both.
    apply_render_profile(*get_viewport(), requested_render_profile.value_or(saved_render_profile.value_or(
        default_render_profile({
            .interactive = options_->camera_interactive,
            .self_test = options_->camera_input_selftest,
            .capture = !options_->capture_path.empty(),
            .resize_test = options_->window_resize_test.has_value(),
        }))));

    // The OS window still has the requested size (--resolution, else the
    // project's window size) here: a window manager's resize reaches Godot
    // only once the first frame processes events. Every mode below sizes its
    // capture from this viewport or re-pins it to its own fixed camera. The
    // host's own probes read the viewport back without a capture too: the
    // tactical camera, atlas and runtime exercises. Map, effect and unit modes
    // pin their own probes; an interactive run follows its window.
    const bool host_probe = options_->map_path.empty()
        && ((!options_->camera_mode.empty() && !options_->camera_interactive) || !options_->atlas_path.empty()
            || options_->renderer_runtime_test);
    if (!options_->capture_path.empty() || host_probe) {
        const Vector2 requested = get_viewport()->get_visible_rect().size;
        if (requested.x >= 1.0F && requested.y >= 1.0F) {
            pin_capture_viewport(*get_window(), static_cast<std::uint32_t>(requested.x),
                static_cast<std::uint32_t>(requested.y));
        }
    }

    // Interaction runs on the host tactical camera path, or with --eawr-map on
    // the map camera loop (MapMode checks its own --eawr-map-camera-config).
    // Atlas and effect modes own separate loops and are not interactive.
    const bool map_interaction = !options_->map_path.empty();
    if (options_->camera_interactive
        && (map_interaction
                ? (!options_->camera_mode.empty() || options_->camera_input_selftest
                    || !options_->atlas_path.empty() || EffectMode::requested())
                : (options_->camera_mode.empty() || ieq(options_->camera_mode, "unlocked")
                    || !options_->atlas_path.empty() || EffectMode::requested()))) {
        status_message_ = "--eawr-camera-interactive requires either --eawr-camera-mode land or space, "
                          "or --eawr-map with --eawr-map-camera-config, and no atlas or effect mode; "
                          "the unlocked mode is not interactive (free flight is toggled from land or space)";
        static_cast<void>(write_report("failed"));
        stop(2);
        return;
    }

    if (!options_->skirmish_setup && !options_->front_end && (!options_->report_path.empty() || options_->startup_metrics))
        startup_trace.begin(false, process_start);
    if (options_->skirmish_setup || options_->front_end) {
        // The front end draws the menus itself and takes captures of its own; the setup screen
        // behind it only captures in the stand-alone mode.
        skirmish_setup_ = std::make_unique<SkirmishSetupMode>(SkirmishSetupMode::Options{
            options_->game_root, options_->mod_root, options_->report_path,
            options_->front_end ? std::filesystem::path{} : options_->capture_path,
            options_->profile, options_->cache_shaders});
        if (!skirmish_setup_->ready(*this)) {
            status_message_ = skirmish_setup_->failure();
            static_cast<void>(write_report("failed"));
            stop(2);
            return;
        }
        gpu_memory_trace.sample("setup_ready");
        if (options_->front_end) {
            skirmish_setup_->leave();
            auto front_options = FrontEndMode::from_command_line(
                options_->capture_path, options_->game_root, options_->mod_root, options_->profile);
            // The campaign hand-offs: a refused start owns only its own subtree and leaves the menu usable.
            const auto start_galactic = [this](GalacticMode::Options campaign_options) {
                campaign_options.game_root = options_->game_root;
                campaign_options.mod_root = options_->mod_root;
                auto* campaign_host = memnew(Node3D);
                add_child(campaign_host);
                auto campaign = std::make_unique<GalacticMode>(std::move(campaign_options));
                if (!campaign->ready(*campaign_host)) {
                    const std::string reason = campaign->failure();
                    campaign.reset();
                    remove_child(campaign_host);
                    memdelete(campaign_host);
                    return reason.empty() ? std::string("The campaign could not be started.") : reason;
                }
                galactic_mode_ = std::move(campaign);
                return std::string{};
            };
            front_options.saved_game_launcher = [start_galactic](const presentation::ui::SavedGameRequest& request) {
                auto campaign_options = GalacticMode::from_command_line();
                campaign_options.restore_save = GalacticMode::Options::RestoreSave{request.directory, request.name};
                return start_galactic(std::move(campaign_options));
            };
            // MNU-34: the staging screen's chosen campaign entry (WGP-29: the set's entry of the chosen
            // faction). Faction and difficulty need no option of their own: the entry names its faction,
            // and the mode starts every campaign at the authored difficulty. Accepted technology
            // options cross the same campaign-creation boundary (WGP-190).
            front_options.campaign_launcher = [start_galactic](const presentation::ui::CampaignStartRequest& request) {
                auto campaign_options = GalacticMode::from_command_line();
                campaign_options.campaign = request.campaign;
                campaign_options.custom_economy = request.economy;
                return start_galactic(std::move(campaign_options));
            };
            front_end_ = std::make_unique<FrontEndMode>(std::move(front_options));
            if (!front_end_->ready(*this, *skirmish_setup_)) {
                status_message_ = front_end_->failure();
                UtilityFunctions::printerr(String::utf8(status_message_.c_str()));
                static_cast<void>(write_report("failed"));
                stop(2);
                return;
            }
        }
        skirmish_setup_->start_button()->connect("pressed", callable_mp(this, &ViewerHost::on_skirmish_start));
        set_process_input(true);
        set_process_unhandled_input(true);
        get_viewport()->connect("size_changed", callable_mp(this, &ViewerHost::on_map_viewport_size_changed));
        return;
    }

    if (UnitMode::requested()) {
        unit_mode_ = std::make_unique<UnitMode>(UnitMode::from_command_line());
        if (!unit_mode_->ready(*this)) stop(2);
        return;
    }

    if (EffectMode::requested()) {
        effect_mode_ = std::make_unique<EffectMode>(EffectMode::from_command_line());
        if (!effect_mode_->ready(*this)) stop(2);
        return;
    }

    if (FontMode::requested()) {
        font_mode_ = std::make_unique<FontMode>(FontMode::from_command_line());
        if (!font_mode_->ready(*this)) stop(2);
        return;
    }

    if (UiGalleryMode::requested()) {
        ui_gallery_mode_ = std::make_unique<UiGalleryMode>(UiGalleryMode::from_command_line());
        if (!ui_gallery_mode_->ready(*this)) stop(2);
        return;
    }

    if (LanLobbyMode::requested()) {
        lan_lobby_mode_ = std::make_unique<LanLobbyMode>(LanLobbyMode::from_command_line());
        if (!lan_lobby_mode_->ready(*this)) stop(2);
        return;
    }

    if (GalacticMode::requested()) {
        galactic_mode_ = std::make_unique<GalacticMode>(GalacticMode::from_command_line());
        if (!galactic_mode_->ready(*this)) {
            stop(2);
            return;
        }
        set_process_input(true);
        set_process_unhandled_input(true);
        return;
    }

    if (OptionsMode::requested()) {
        options_mode_ = std::make_unique<OptionsMode>(OptionsMode::from_command_line());
        if (!options_mode_->ready(*this)) stop(2);
        return;
    }

    if (AttractRunner::requested()) {
        AttractRunner::Options attract = AttractRunner::from_command_line();
        if (attract.game_root.empty()) attract.game_root = options_->game_root;
        if (attract.mod_root.empty()) attract.mod_root = options_->mod_root;
        if (attract.profile.empty()) attract.profile = options_->profile;
        attract_runner_ = std::make_unique<AttractRunner>(std::move(attract));
        set_process_input(true);
        set_process_unhandled_input(true);
        return;
    }

    if (InputRoutingMode::requested()) {
        input_routing_mode_ = std::make_unique<InputRoutingMode>(InputRoutingMode::from_command_line());
        if (!input_routing_mode_->ready(*this)) stop(2);
        return;
    }

    if (!options_->map_path.empty()) {
        map_mode_ = std::make_unique<MapMode>(MapMode::Options{
            .game_root = options_->game_root,
            .mod_root = options_->mod_root,
            .profile = options_->profile,
            .map_path = options_->map_path,
            .report_path = options_->report_path,
            .capture_path = options_->capture_path,
            .warmup_frames = 30,
            .timed_frames = 120,
            .interactive = options_->camera_interactive,
            .camera_zoom = options_->camera_zoom_requested
                ? std::optional<float>(options_->camera_zoom) : std::nullopt,
            .shaders = options_->cache_shaders ? std::make_shared<GodotShaderCache>() : nullptr,
        });
        if (!map_mode_->ready(*this)) {
            stop(2);
            return;
        }
        get_viewport()->connect("size_changed",
            callable_mp(this, &ViewerHost::on_map_viewport_size_changed));
        if (options_->camera_interactive) {
            UtilityFunctions::print("EAWR interactive map camera: pan with WASD, arrows or the screen edge, "
                                    "zoom with the wheel or PageUp/PageDown, rotate with middle-drag, "
                                    "pan with Alt and mouse motion, "
                                    "Home resets; close the window to exit (bindings are project-authored)");
        }
        return;
    }

    if (!options_->camera_mode.empty()) {
        renderer_ = std::make_unique<GodotRenderer>(*this);
        if (!start_tactical_camera()
            || (options_->camera_interactive && !start_camera_interaction())) {
            static_cast<void>(write_report("failed"));
            stop(2);
        }
        return;
    }

    if (!options_->atlas_path.empty()) {
        renderer_ = std::make_unique<GodotRenderer>(*this);
        if (!start_atlas_overlay()) {
            static_cast<void>(write_report("failed"));
            stop(2);
        }
        return;
    }

    if (options_->renderer_runtime_test) {
        runtime_exercise_ = true;
        const auto shaders = options_->cache_shaders ? std::make_shared<GodotShaderCache>() : nullptr;
        renderer_ = std::make_unique<GodotRenderer>(*this, shaders);
        if (!start_renderer_runtime_exercise(shaders)) {
            static_cast<void>(write_report("failed"));
            stop(2);
        }
        return;
    }

    if (!load_replay()) {
        static_cast<void>(write_report("failed"));
        stop(2);
        return;
    }
    const bool headless = utf8(DisplayServer::get_singleton()->get_name()) == "headless";
    if (headless || options_->headless_probe) {
        const auto scene = read_bytes(options_->scene_path);
        scene_hash_ = scene ? hash_bytes(*scene) : std::string{};
        if (scene_hash_ != expected_scene_hash) {
            status_message_ = "common scene missing or SHA-256 mismatch";
            static_cast<void>(write_report("failed"));
            stop(2);
            return;
        }
        stop(write_report("headless_startup_and_core_replay_passed") ? 0 : 2);
        return;
    }
    if (!load_scene()) {
        static_cast<void>(write_report("failed"));
        stop(2);
        return;
    }

    renderer_ = std::make_unique<GodotRenderer>(*this);
    std::set<sim::AssetId> asset_ids;
    for (const auto& snapshot : snapshots_) {
        for (const sim::RenderInstance& instance : snapshot->instances()) {
            asset_ids.insert(instance.asset_id);
        }
    }
    for (const sim::AssetId asset_id : asset_ids) {
        auto upload = renderer_->upload(asset_id, model_, texture_, material_);
        if (!upload) {
            status_message_ = core::format_diagnostic(upload.error());
            static_cast<void>(write_report("failed"));
            stop(2);
            return;
        }
    }
    renderer_->set_camera(capture_camera_);
    renderer_->submit(snapshots_.front());
    if (!apply_animation_pose(snapshots_.front())) {
        static_cast<void>(write_report("failed"));
        stop(2);
    }
} catch (const std::exception& error) {
    status_message_ = std::string("Viewer startup failed: ") + error.what() +
        ". Check read permissions for this account and verify or repair the FoC installation.";
    UtilityFunctions::printerr(String::utf8(status_message_.c_str()));
    if (options_) static_cast<void>(write_report("failed"));
    stop(2);
}
} // namespace eawr::presentation::godot_backend
