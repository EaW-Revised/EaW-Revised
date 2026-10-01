#include "viewer_host_internal.hpp"
#include "audio_output.hpp"

namespace eawr::presentation::godot_backend {
namespace viewer_host_detail {

[[nodiscard]] std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), converted.length());
}

[[nodiscard]] bool ieq(const std::string_view left, const std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](const char value) {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
        };
        if (fold(left[index]) != fold(right[index])) return false;
    }
    return true;
}

[[nodiscard]] std::string hash_bytes(const std::span<const std::byte> bytes) {
    return sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

} // namespace viewer_host_detail

namespace {

constexpr std::string_view expected_scene_hash =
    "de673739583babf0a4541feafc6bd1f0c40da36788a962e7d49b284be7611a18";

// "<width>x<height>" in whole pixels, each 1..16384.
[[nodiscard]] std::optional<Vector2i> parse_window_size(const std::string_view text) {
    const std::size_t separator = text.find('x');
    if (separator == std::string_view::npos) return std::nullopt;
    const auto dimension = [](const std::string_view part) -> std::optional<int32_t> {
        int32_t value{};
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (error != std::errc{} || end != part.data() + part.size() || value < 1 || value > 16384) {
            return std::nullopt;
        }
        return value;
    };
    const auto width = dimension(text.substr(0, separator));
    const auto height = dimension(text.substr(separator + 1));
    if (!width || !height) return std::nullopt;
    return Vector2i(*width, *height);
}

[[nodiscard]] std::optional<std::vector<std::byte>> read_bytes(
    const ViewerPath& path) {
    if (path.is_godot_resource()) {
        const std::string& identifier = path.value();
        const Ref<FileAccess> input = FileAccess::open(
            String::utf8(identifier.data(), static_cast<int64_t>(identifier.size())),
            FileAccess::READ);
        if (input.is_null()) return std::nullopt;
        const PackedByteArray packed = input->get_buffer(input->get_length());
        std::vector<std::byte> result(static_cast<std::size_t>(packed.size()));
        if (!result.empty()) std::memcpy(result.data(), packed.ptr(), result.size());
        return result;
    }
    std::ifstream input(path.native(), std::ios::binary | std::ios::ate);
    if (!input) return std::nullopt;
    const std::streampos end = input.tellg();
    if (end < 0) return std::nullopt;
    std::vector<std::byte> result(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!result.empty()) {
        input.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
    }
    return input || result.empty() ? std::optional(std::move(result)) : std::nullopt;
}

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

ViewerHost::ViewerHost() = default;
ViewerHost::~ViewerHost() { shutdown_trace::mark("viewer host freed (members follow)"); }

void ViewerHost::_bind_methods() {}

void ViewerHost::_ready() {
    mute_lane_audio_output();
    set_process(true);
    // UI-07: a text control taking focus cancels held world input (UI-I3).
    get_viewport()->connect("gui_focus_changed", callable_mp(this, &ViewerHost::on_gui_focus_changed));
    options_ = std::make_unique<Options>();
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index < arguments.size(); ++index) {
        const std::string argument = utf8(arguments[index]);
        const auto take_path = [&](std::filesystem::path& target) {
            if (index + 1 >= arguments.size()) return false;
            target = ViewerPath{utf8(arguments[++index])}.native();
            return true;
        };
        const auto take_input = [&](ViewerPath& target) {
            if (index + 1 >= arguments.size()) return false;
            target = ViewerPath{utf8(arguments[++index])};
            return true;
        };
        if (argument == "--eawr-game-root") {
            if (!take_path(options_->game_root)) break;
        } else if (argument == "--eawr-mod-root") {
            if (!take_path(options_->mod_root)) break;
        } else if (argument == "--eawr-profile") {
            if (index + 1 >= arguments.size()) {
                options_->profile_error = "missing value for --eawr-profile (expected eaw, foc or remake)";
                break;
            }
            options_->profile = utf8(arguments[++index]);
        } else if (argument == "--eawr-render-profile") {
            if (index + 1 >= arguments.size()) {
                options_->render_profile_missing = true;
                break;
            }
            options_->render_profile = utf8(arguments[++index]);
        } else if (argument == "--eawr-scene") {
            if (!take_input(options_->scene_path)) break;
        } else if (argument == "--eawr-replay") {
            if (!take_input(options_->replay_path)) break;
        } else if (argument == "--eawr-model") {
            if (index + 1 >= arguments.size()) break;
            options_->model_path = utf8(arguments[++index]);
        } else if (argument == "--eawr-mesh") {
            if (index + 1 >= arguments.size()) break;
            options_->mesh_name = utf8(arguments[++index]);
        } else if (argument == "--eawr-texture") {
            if (index + 1 >= arguments.size()) break;
            options_->texture_path = utf8(arguments[++index]);
        } else if (argument == "--eawr-animation") {
            if (index + 1 >= arguments.size()) break;
            options_->animation_path = utf8(arguments[++index]);
        } else if (argument == "--eawr-animation-time") {
            if (index + 1 >= arguments.size()) break;
            try {
                options_->animation_time_seconds = std::stof(utf8(arguments[++index]));
            } catch (const std::exception&) {
                options_->animation_time_seconds = std::numeric_limits<float>::quiet_NaN();
            }
        } else if (argument == "--eawr-map" || argument == "--eawr-skirmish-map") {
            if (index + 1 >= arguments.size()) break;
            options_->map_path = utf8(arguments[++index]);
        } else if (argument == "--eawr-live-session") {
            if (index + 1 >= arguments.size()) break;
            const auto live_mode = utf8(arguments[++index]);
            if (options_->map_path.empty() && (live_mode == "skirmish" || live_mode == "m2")) {
                options_->map_path = "data/art/maps/_mp_space_coruscant.ted";
            }
        } else if (argument == "--eawr-atlas") {
            if (index + 1 >= arguments.size()) break;
            options_->atlas_path = utf8(arguments[++index]);
        } else if (argument == "--eawr-icon") {
            if (index + 1 >= arguments.size()) break;
            options_->icon_name = utf8(arguments[++index]);
        } else if (argument == "--eawr-camera-mode") {
            if (index + 1 >= arguments.size()) break;
            options_->camera_mode = utf8(arguments[++index]);
        } else if (argument == "--eawr-camera-zoom") {
            if (index + 1 >= arguments.size()) break;
            options_->camera_zoom_requested = true;
            options_->camera_zoom = std::numeric_limits<float>::quiet_NaN();
            if (auto zoom = tactical_camera::parse_scalar(utf8(arguments[++index]))) {
                options_->camera_zoom = zoom.value();
            }
        } else if (argument == "--eawr-camera-interactive") {
            options_->camera_interactive = true;
        } else if (argument == "--eawr-camera-input-selftest") {
            options_->camera_interactive = true;
            options_->camera_input_selftest = true;
        } else if (argument == "--eawr-camera-bindings") {
            if (index + 1 >= arguments.size()) break;
            options_->camera_bindings_path = utf8(arguments[++index]);
        } else if (argument == "--eawr-capture") {
            if (!take_path(options_->capture_path)) break;
        } else if (argument == "--eawr-report") {
            if (!take_path(options_->report_path)) break;
        } else if (argument == "--eawr-window-resize-test") {
            const std::string value = index + 1 < arguments.size() ? utf8(arguments[++index]) : std::string{};
            options_->window_resize_test = parse_window_size(value);
            options_->window_resize_invalid = !options_->window_resize_test;
            options_->window_resize_pending = options_->window_resize_test.has_value();
        } else if (argument == "--eawr-headless-probe") {
            options_->headless_probe = true;
        } else if (argument == "--eawr-benchmark") {
            options_->benchmark = true;
        } else if (argument == "--eawr-renderer-runtime-test") {
            options_->renderer_runtime_test = true;
        }
    }

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

    // Retail for every capture and test, enhanced for a player's view (#184).
    apply_render_profile(*get_viewport(), requested_render_profile.value_or(default_render_profile({
        .interactive = options_->camera_interactive,
        .self_test = options_->camera_input_selftest,
        .capture = !options_->capture_path.empty(),
        .resize_test = options_->window_resize_test.has_value(),
    })));

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
        renderer_ = std::make_unique<GodotRenderer>(*this);
        if (!start_renderer_runtime_exercise()) {
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
}

bool ViewerHost::load_scene() {
    const auto scene = read_bytes(options_->scene_path);
    if (!scene) {
        status_message_ = "common scene could not be read";
        return false;
    }
    scene_hash_ = hash_bytes(*scene);
    if (scene_hash_ != expected_scene_hash) {
        status_message_ = "common scene SHA-256 mismatch";
        return false;
    }
    active_content_profile_ = options_->profile.empty() ? (!options_->mod_root.empty()
        ? "remake" : std::filesystem::is_directory(options_->game_root / "corruption" / "Data")
            ? "foc" : "eaw") : options_->profile;
    auto filesystem = mount_content_layers(
        options_->game_root, options_->mod_root, active_content_profile_, status_message_);
    if (!filesystem) return false;
    // One plan separates the frozen Hangar draw (default run or explicit
    // `--eawr-mesh Hangar`) from the opt-in exploratory Hull preview and from
    // unpinned models; each carries its own mesh, material, texture, hash pins
    // and camera policy.
    auto planned = model_preview::plan_for({options_->model_path, options_->mesh_name,
        options_->texture_path, !options_->animation_path.empty()});
    if (!planned) {
        status_message_ = planned.failure;
        return false;
    }
    const model_preview::Plan& plan = *planned.value;
    auto model_bytes = filesystem->open(options_->model_path);
    if (!model_bytes || !model_preview::hash_matches(
            hash_bytes(model_bytes.value()), plan.expected_model_sha256)) {
        status_message_ = "selected model missing or SHA-256 mismatch";
        return false;
    }
    auto loaded_model = assets::load_model(*filesystem, options_->model_path);
    if (!loaded_model) {
        status_message_ = core::format_diagnostic(loaded_model.error());
        return false;
    }
    model_.source = loaded_model.value().source;
    model_.bones = loaded_model.value().bones;
    const auto selection = model_preview::select_submesh(loaded_model.value(), plan);
    if (!selection) {
        status_message_ = selection.failure;
        return false;
    }
    const assets::Mesh& source_mesh = loaded_model.value().meshes[selection.value->mesh_index];
    const assets::Submesh* selected_submesh =
        &source_mesh.submeshes[selection.value->submesh_index];
    const model_preview::LegacySelection* selected_material = &selection.value->material;
    {
        assets::Mesh mesh = source_mesh;
        mesh.submeshes = {*selected_submesh};
        model_.meshes.push_back(std::move(mesh));
    }
    if (plan.camera == model_preview::CameraPolicy::legacy_mesh_bounds) {
        const float center_x = (source_mesh.bounds_min.x + source_mesh.bounds_max.x) * 0.5F;
        const float center_y = (source_mesh.bounds_min.y + source_mesh.bounds_max.y) * 0.5F;
        const float center_z = (source_mesh.bounds_min.z + source_mesh.bounds_max.z) * 0.5F;
        const float extent_x = source_mesh.bounds_max.x - source_mesh.bounds_min.x;
        const float extent_y = source_mesh.bounds_max.y - source_mesh.bounds_min.y;
        const float extent_z = source_mesh.bounds_max.z - source_mesh.bounds_min.z;
        const float extent = std::max({extent_x, extent_y, extent_z, 1.0F});
        capture_camera_.target = {center_x, center_z, -center_y};
        capture_camera_.eye = {center_x, center_z + extent * 0.35F,
            -center_y + extent * 2.25F};
        capture_camera_.near_plane = std::max(0.01F, extent * 0.01F);
        capture_camera_.far_plane = std::max(100.0F, extent * 20.0F);
    } else if (plan.camera == model_preview::CameraPolicy::hull_bounds_fit) {
        // Trust the bounds only after the rest hierarchy agrees with the
        // renderer's rest placement, then frame the instance the fixed
        // capture actually submits.
        auto rest = model_preview::rest_placement(loaded_model.value(),
            selection.value->mesh_index, selection.value->submesh_index);
        if (!rest) {
            status_message_ = rest.failure;
            return false;
        }
        if (!fixed_capture_snapshot_ || fixed_capture_snapshot_->instances().empty()) {
            status_message_ = "exploratory Hull preview requires the fixed capture instance";
            return false;
        }
        const std::vector<PresentationTransform> placed =
            adapt_snapshot(*fixed_capture_snapshot_);
        const model_preview::Bounds world =
            model_preview::world_bounds(rest.value->asset, placed.front().column_major);
        FixedCamera framing = capture_camera_;
        const Vector2 viewport = get_viewport()->get_visible_rect().size;
        if (viewport.x >= 1.0F && viewport.y >= 1.0F) {
            framing.width = static_cast<std::uint32_t>(viewport.x);
            framing.height = static_cast<std::uint32_t>(viewport.y);
        }
        auto fit = model_preview::fit_camera(world, framing, model_preview::hull_view_direction,
            model_preview::hull_fit_margin);
        if (!fit) {
            status_message_ = fit.failure;
            return false;
        }
        capture_camera_ = fit.value->camera;
        model_preview_ = std::make_unique<ModelPreview>();
        model_preview_->rest = std::move(*rest.value);
        model_preview_->world = world;
        model_preview_->fit = *fit.value;
    }
    if (plan.exploratory) {
        if (!model_preview_) {
            status_message_ = "exploratory preview has no framing evidence";
            return false;
        }
        // Its framing is checked against a read-back even without a capture,
        // so the viewport keeps the framed size whatever the OS window.
        pin_capture_viewport(*get_window(), capture_camera_.width, capture_camera_.height);
        model_preview_->plan = plan;
        model_preview_->mesh_name = source_mesh.name;
        model_preview_->mesh_bone = source_mesh.bone;
        model_preview_->submesh_index = selection.value->submesh_index;
        model_preview_->index_count = selected_submesh->indices.size();
        model_preview_->technique = selected_material->technique;
        model_preview_->pass = selected_material->pass;
        model_preview_->base_texture =
            model_preview::base_texture_name(*selected_submesh).value_or(std::string{});
    }
    selected_model_path_ = options_->model_path;
    selected_model_hash_ = hash_bytes(model_bytes.value());
    selected_program_ = selected_submesh->shader;
    auto texture_path = model_preview::texture_path_for(plan, *selected_submesh);
    if (!texture_path) {
        status_message_ = texture_path.failure;
        return false;
    }
    selected_texture_path_ = std::move(*texture_path.value);
    auto texture_bytes = filesystem.value().open(selected_texture_path_);
    if (!texture_bytes || !model_preview::hash_matches(
            hash_bytes(texture_bytes.value()), plan.expected_texture_sha256)) {
        status_message_ = "selected texture missing or SHA-256 mismatch";
        return false;
    }
    auto loaded_texture = assets::load_texture(filesystem.value(), selected_texture_path_);
    if (!loaded_texture) {
        status_message_ = core::format_diagnostic(loaded_texture.error());
        return false;
    }
    selected_texture_hash_ = hash_bytes(texture_bytes.value());
    if (!options_->animation_path.empty()) {
        auto animation_bytes = filesystem.value().open(options_->animation_path);
        if (!animation_bytes) {
            status_message_ = core::format_diagnostic(animation_bytes.error());
            return false;
        }
        auto loaded_animation = assets::load_animation(filesystem.value(), options_->animation_path);
        if (!loaded_animation) {
            status_message_ = core::format_diagnostic(loaded_animation.error());
            return false;
        }
        auto player = animation::Player::create(model_, &loaded_animation.value());
        if (!player) {
            status_message_ = core::format_diagnostic(player.error());
            return false;
        }
        auto pose = player.value().sample({options_->animation_time_seconds,
            animation::PlaybackMode::loop, 0.0F});
        if (!pose) {
            status_message_ = core::format_diagnostic(pose.error());
            return false;
        }
        animation_ = std::move(loaded_animation.value());
        selected_animation_path_ = options_->animation_path;
        selected_animation_hash_ = hash_bytes(animation_bytes.value());
        animation_player_ = std::move(player.value());
        animation_pose_ = std::move(pose.value());
        animation_time_seconds_ = animation_pose_->sampled_time_seconds;
    } else if (!model_.bones.empty()
        && (!selected_submesh->skin_bones.empty() || model_.meshes.front().bone >= 0)) {
        auto player = animation::Player::create(model_);
        if (!player) {
            status_message_ = core::format_diagnostic(player.error());
            return false;
        }
        auto pose = player.value().sample({});
        if (!pose) {
            status_message_ = core::format_diagnostic(pose.error());
            return false;
        }
        animation_player_ = std::move(player.value());
        animation_pose_ = std::move(pose.value());
    }
    texture_ = std::move(loaded_texture.value());
    material_ = MaterialDescription{
        .schema_version = 1,
        .route = MaterialRoute::legacy_effect,
        .pass = RenderPass::opaque,
        .program = selected_submesh->shader,
        .technique = selected_material->technique,
        .pass_name = selected_material->pass,
        .bindings = {},
    };
    for (const assets::MaterialParameter& parameter : model_.meshes.front().submeshes.front().parameters) {
        material_.bindings.push_back({parameter.name, parameter.value});
    }
    return true;
}

bool ViewerHost::apply_animation_pose(
    const std::shared_ptr<const sim::RenderSnapshot>& snapshot) {
    if (!animation_pose_ || !snapshot || !renderer_) return true;
    for (const sim::RenderInstance& instance : snapshot->instances()) {
        auto applied = renderer_->set_skin_pose(
            instance.entity_id, instance.asset_id, animation_pose_->bones);
        if (!applied) {
            status_message_ = core::format_diagnostic(applied.error());
            return false;
        }
    }
    skin_palette_bound_ = !renderer_->skin_bindings().empty();
    if (!skin_palette_bound_) {
        status_message_ = "animation pose did not produce a live Godot skin binding";
        return false;
    }
    return true;
}

bool ViewerHost::load_replay() {
    const auto replay_bytes = read_bytes(options_->replay_path);
    if (!replay_bytes) {
        status_message_ = "replay fixture could not be read";
        return false;
    }
    replay_hash_ = hash_bytes(*replay_bytes);
    std::vector<std::uint8_t> bytes(replay_bytes->size());
    if (!bytes.empty()) std::memcpy(bytes.data(), replay_bytes->data(), bytes.size());
    auto replay = sim::parse_replay(bytes, options_->replay_path.value());
    if (!replay) {
        status_message_ = core::format_diagnostic(replay.error());
        return false;
    }
    auto world_result = sim::World::create(replay.value());
    if (!world_result) {
        status_message_ = core::format_diagnostic(world_result.error());
        return false;
    }
    sim::World world = std::move(world_result.value());
    snapshots_.push_back(world.snapshot());
    const sim::InlineExecutor executor;
    while (world.completed_tick() < world.final_tick_count()) {
        auto tick = world.step(executor);
        if (!tick) {
            status_message_ = core::format_diagnostic(tick.error());
            return false;
        }
        snapshots_.push_back(tick.value().snapshot);
    }
    // The retained P0 prototype captures on its frame 119 after submitting
    // snapshots_[119 / 100], i.e. replay tick 1. Pin the production fixed
    // capture to that observed tick rather than the previously guessed tick 0.
    if (snapshots_.size() > 1 && !snapshots_[1]->instances().empty()) {
        fixed_capture_snapshot_ = std::make_shared<const sim::RenderSnapshot>(
            snapshots_[1]->completed_tick(),
            std::vector<sim::RenderInstance>{snapshots_[1]->instances().front()});
    }
    return snapshots_.size() > 1;
}

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
        if (const std::optional<int> exit_code = map_mode_->process(delta)) stop(*exit_code);
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

void ViewerHost::stop(const int exit_code) {
    completed_ = true;
    if (get_tree()) get_tree()->quit(exit_code);
}

} // namespace eawr::presentation::godot_backend
