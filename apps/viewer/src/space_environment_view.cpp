#include "space_environment_internal.hpp"
#include "eawr/core/load_profile.hpp"
#include "render_profile_viewport.hpp"
#include "shutdown_trace.hpp"
#include "eawr/presentation/particles/map_attachment_owner.hpp"

namespace eawr::presentation::godot_backend {
namespace {

// ---------------------------------------------------------------------------
// E-space-environment-v1: the default space map view (P1 #27, rescope
// 2026-09-24). Everything below is judged by eye; the pure rules live in
// space/environment_scene.hpp.
// ---------------------------------------------------------------------------

constexpr std::string_view effect_clock_rule =
    "retail scene clock: TIME gains LogicalFPS / 1000 = 0.03 s per 30 Hz sim frame and wraps at 28800 s; here "
    "per 30 Hz presentation tick (real time live, held after the particle frames in a capture, plus "
    "--eawr-map-idle-offset). Nebula.fx moves only through it: vertex wave and UV scroll";

[[nodiscard]] TacticalLoad load_space_tactical(const vfs::Vfs& filesystem) {
    TacticalLoad result;
    constexpr std::string_view tactical_path = "data/xml/tacticalcameras.xml";
    constexpr std::string_view constants_path = "data/xml/gameconstants.xml";
    auto tactical_bytes = filesystem.open(tactical_path);
    auto constants_bytes = filesystem.open(constants_path);
    if (!tactical_bytes || !constants_bytes) {
        result.failure = core::format_diagnostic(!tactical_bytes ? tactical_bytes.error() : constants_bytes.error());
        return result;
    }
    const std::string tactical_sha = hash_bytes(tactical_bytes.value());
    const std::string constants_sha = hash_bytes(constants_bytes.value());
    auto loaded = presentation::camera::load_constants({tactical_bytes.value(), tactical_path, tactical_sha},
        {constants_bytes.value(), constants_path, constants_sha}, presentation::camera::Mode::space);
    if (!loaded) {
        result.failure = core::format_diagnostic(loaded.error());
        return result;
    }
    const presentation::camera::Constants& constants = loaded.value().constants;
    result.constants = constants;
    result.values.distance = constants.distance_default;
    result.values.pitch_degrees = constants.pitch_default;
    result.values.fov_degrees = constants.fov_default;
    result.values.near_plane = constants.near_clip;
    result.values.yaw_degrees = constants.yaw_default;
    result.source = "Space_Mode, data/xml/tacticalcameras.xml sha256 " + tactical_sha;
    return result;
}

} // namespace

namespace space_environment_detail {

bool EnvironmentView::ready(Node3D& host, const assets::Map& map, const vfs::Vfs& filesystem,
                            const assets::ObjectTypeCatalog& catalog, const bool catalog_loaded,
                            const std::string& catalog_failure) {
    core::load_profile::Scope scope(core::load_profile::Phase::scene_environment);
    filesystem_ = &filesystem;
    host_ = &host;
    catalog_failure_ = catalog_failure;
    environment_count_ = map.environments.size();
    if (map.environments.empty()) return fail("the space map declares no environment record");
    const assets::EnvironmentDescriptor& environment = map.environments.front();
    environment_name_ = environment.name.value_or("");
    primary_sky_ = environment.primary_sky.value_or("");
    secondary_sky_ = environment.secondary_sky.value_or("");
    light_ = space::environment_light(environment);
    if (environment.cloud_texture) {
        not_rendered_.push_back("cloud layer " + *environment.cloud_texture
                                + ": land weather layer; no space composition rule");
    }
    for (std::size_t index = 1; index < map.environments.size(); ++index) {
        not_rendered_.push_back("environment " + std::to_string(index) + " '"
            + map.environments[index].name.value_or("") + "': the view shows environment 0");
    }

    // Camera: the tactical bridge uses the authored space pose, unless an
    // explicit fixed camera was supplied for a capture.
    const FixedCamera defaults;
    tactical_ = load_space_tactical(filesystem);
    if (!tactical_.constants) return fail("Space_Mode camera: " + tactical_.failure);
    default_camera_ = space::default_space_camera(map, tactical_.values, defaults.width, defaults.height);
    if (!options_.camera.empty()) {
        const space::CameraParse parsed = space::parse_camera(options_.camera, defaults.width, defaults.height);
        camera_status_ = std::string(space::to_string(parsed.status));
        if (parsed.status != space::CameraStatus::valid) {
            return fail("--eawr-space-camera is " + camera_status_);
        }
        camera_ = parsed.camera;
        camera_source_ = "--eawr-space-camera";
    } else {
        camera_ = default_camera_.camera;
        camera_status_ = std::string(space::to_string(space::validate_camera(camera_)));
        if (space::validate_camera(camera_) != space::CameraStatus::valid) {
            return fail("the default space camera is " + camera_status_);
        }
    }
    if (options_.map_camera) {
        const auto& source = *options_.map_camera;
        const Vector2 viewport = host.get_viewport()->get_visible_rect().size;
        const bool capture = !options_.capture_path.empty();
        const std::uint32_t width = capture ? defaults.width : static_cast<std::uint32_t>(viewport.x);
        const std::uint32_t height = capture ? defaults.height : static_cast<std::uint32_t>(viewport.y);
        auto* display = DisplayServer::get_singleton();
        bridge_ = std::make_unique<viewer::MapCameraBridge>(
            display && display->window_is_focused(), camera_input::Context::space);
        const auto fixed = options_.camera.empty() ? std::nullopt
            : std::optional<tactical::TacticalFrame>(tactical_frame(camera_));
        if (auto active = bridge_->activate(source.config, source.constants, source.bindings_json,
                width, height, fixed); !active) {
            return fail(core::format_diagnostic(active.error()));
        }
        if (!fixed) {
            camera_ = fixed_camera(bridge_->frame());
            camera_source_ = "space map camera (project config, Space_Mode XML)";
            // #82: FoC's tactical overview past Distance_Max, from Space_Mode's
            // Tactical_Overview_* tags. The camera self-tests keep the plain camera.
            if (!options_.camera_selftest && !options_.camera_terminal_baseline_test
                && !options_.camera_terminal_hold_test && !options_.camera_terminal_release_test) {
                constexpr std::string_view tactical_path = "data/xml/tacticalcameras.xml";
                auto bytes = filesystem.open(tactical_path);
                auto constants = bytes ? tactical::load_overview_constants({bytes.value(), tactical_path, ""},
                                                                           tactical::Mode::space)
                                       : core::Result<tactical::OverviewConstants>::failure(bytes.error());
                if (constants) {
                    auto overview_constants = constants.value();
                    if (source.config.overview_clicks) overview_constants.clicks = *source.config.overview_clicks;
                    overview_.emplace(overview_constants);
                    overview_status_ = "off";
                } else {
                    overview_status_ = "unavailable: " + core::format_diagnostic(constants.error());
                }
            }
        }
        host.set_process_input(true);
        host.set_process_unhandled_input(true);
    }
    camera_ = space::environment_view_camera(camera_);
    if (!options_.capture_path.empty()) {
        // Every camera source above has the capture identity's size; the
        // root viewport is drawn at exactly that size whatever the OS window.
        pin_capture_viewport(*host.get_window(), camera_.width, camera_.height);
    }
    eye_source_ = space::source_from_render(camera_.eye);
    target_source_ = space::source_from_render(camera_.target);
    up_source_ = space::source_from_render(camera_.up);

    renderer_ = std::make_unique<GodotRenderer>(host, options_.shaders);
    renderer_->set_camera(camera_);
    renderer_->set_scene_bloom(options_.bloom);

    // Skies: camera-centred, scaled to the environment sky radius, oriented by
    // R-SKY-02 through the object rule.
    std::map<std::string, assets::Model> sky_models;
    for (const bool secondary : {false, true}) {
        const std::string& name = secondary ? secondary_sky_ : primary_sky_;
        const std::string role = secondary ? "secondary_sky" : "primary_sky";
        if (name.empty()) continue;
        Item missing;
        missing.role = role;
        missing.object = name;
        const assets::ObjectTypeRef* type = assets::find_object_type(catalog, name);
        const space::ModelSelection selection = space::select_model(type, catalog_loaded);
        if (selection.status != space::PlanStatus::ready) {
            missing.cause = "sky object " + std::string(space::to_string(selection.status));
            items_.push_back(std::move(missing));
            continue;
        }
        const auto path = probe_reference(filesystem, "data/art/models/", selection.declared_name, model_suffixes);
        if (!path) {
            missing.cause = "no data/art/models/ record for " + selection.declared_name;
            items_.push_back(std::move(missing));
            continue;
        }
        auto model = assets::load_model(filesystem, *path);
        if (!model) {
            missing.model = *path;
            missing.cause = core::format_diagnostic(model.error());
            items_.push_back(std::move(missing));
            continue;
        }
        const auto [slot, inserted] = sky_models.emplace(role, std::move(model.value()));
        static_cast<void>(inserted);
        const std::vector<space::SceneSurface> surfaces = space::scene_surfaces(slot->second);
        const float radius = space::surface_radius(surfaces);
        Placed placed;
        placed.role = role;
        placed.object = name;
        placed.model_path = *path;
        placed.model = &slot->second;
        placed.scale = radius > 0.0F ? space::environment_sky_radius / radius : 1.0F;
        placed.transform = space::object_transform(eye_source_, space::sky_orientation_degrees(environment, secondary),
                                                   placed.scale);
        compose_object(placed, surfaces);
    }

    // Planet and nebula placements (space::environment_family).
    if (options_.catalog == nullptr) {
        not_rendered_.push_back("planet and nebula placements: the XML catalog did not load (" + catalog_failure + ")");
    } else {
        scene::VfsAssetCache cache(filesystem);
        scene::BuildInput input;
        input.map = &map;
        input.map_sha256 = options_.map_sha256;
        input.catalog = options_.catalog;
        input.access = cache.access();
        const scene::Scene built = scene::build(input);
        for (const scene::Placement& placement : built.placements) {
            if (placement.model_path.empty()) continue;
            const assets::Model* model = cache.model(placement.model_path);
            if (model == nullptr) continue;
            const space::EnvironmentFamily family = space::environment_family(*model);
            if (family == space::EnvironmentFamily::none) continue;
            environment_records_.push_back(placement.record_ordinal);
            const assets::Placement* source = placement.record_ordinal < map.placements.size()
                ? &map.placements[placement.record_ordinal] : nullptr;
            Placed placed;
            placed.role = std::string(space::to_string(family));
            placed.object = placement.object_id;
            placed.record = placement.record_ordinal;
            placed.model_path = placement.model_path;
            placed.model = model;
            placed.scale = static_cast<float>(static_cast<double>(placement.scale_raw)
                                              / static_cast<double>(sim::math::Fixed::scale));
            if (source == nullptr || !source->position) {
                Item item;
                item.role = placed.role;
                item.object = placed.object;
                item.record = placed.record;
                item.model = placed.model_path;
                item.cause = "the placement has no position";
                items_.push_back(std::move(item));
                continue;
            }
            if (!placement.transform) {
                Item item;
                item.role = placed.role;
                item.object = placed.object;
                item.record = placed.record;
                item.model = placed.model_path;
                item.cause = "the placement has no valid sourced transform";
                items_.push_back(std::move(item));
                continue;
            }
            // R-ROT-01..03 and LZ-01: reuse the same scene matrix as props,
            // particle owners and bounds. environment_records_ prevents a second draw.
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t column = 0; column < 4; ++column) {
                    placed.transform[row * 4 + column] = static_cast<float>(
                        static_cast<double>(placement.transform->matrix.rows[row][column].raw())
                        / static_cast<double>(sim::math::Fixed::scale));
                }
            }
            const std::vector<space::SceneSurface> surfaces = space::scene_surfaces(*model);
            placed.radius = space::surface_radius(surfaces) * placed.scale;
            if (!bind_idle(placement, *model, filesystem, placed)) return false;
            compose_object(placed, surfaces);
        }
    }

    if (options_.populate) {
        SpacePopulateContext context{*renderer_, camera_, map, light_ ? &*light_ : nullptr, environment_records_,
                                     next_asset_, static_cast<sim::EntityId>(next_asset_)};
        auto populated = options_.populate(context);
        if (!populated) return fail("populate: " + core::format_diagnostic(populated.error()));
        populate_result_ = std::move(populated.value());
        populated_ = populate_result_->instances.size();
        populate_first_ = instances_.size();
        instances_.insert(instances_.end(), populate_result_->instances.begin(), populate_result_->instances.end());
    }

    const bool sky_drawn = std::any_of(items_.begin(), items_.end(), [](const Item& item) {
        return item.role == "primary_sky" && item.status == "drawn";
    });
    // #908: some valid maps author only a secondary sky (Bespin). An absent
    // primary is optional; a declared primary still needs a supported route.
    if (!primary_sky_.empty() && !sky_drawn) return fail("the primary sky drew no surface");
    snapshot_ = std::make_shared<const sim::RenderSnapshot>(0, instances_);
    if (!initialize_idle()) return false;
    return true;
}

void EnvironmentView::show_live_error(const std::string& message) {
    if (!live_error_.empty()) return;
    live_error_ = message;
    UtilityFunctions::printerr(String(("live session stopped: " + message).c_str()));
    if (host_ == nullptr) return;
    auto* layer = memnew(CanvasLayer);
    host_->add_child(layer);
    auto* label = memnew(Label);
    label->set_text(String(("Live session stopped - the simulation thread failed:\n" + message).c_str()));
    label->add_theme_color_override("font_color", Color(1.0F, 0.35F, 0.3F));
    label->set_position(Vector2(16.0F, 16.0F));
    layer->add_child(label);
}

bool EnvironmentView::write_report() const {
    if (options_.report_path.empty()) return true;
    std::error_code error;
    if (!options_.report_path.parent_path().empty()) {
        std::filesystem::create_directories(options_.report_path.parent_path(), error);
    }
    std::ofstream output(options_.report_path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    const BackendInfo backend = renderer_
        ? renderer_->backend_info() : BackendInfo{"Godot 4.7.2-stable", "renderer_not_created", {}, {}, {}};
    const auto list = [](const std::vector<std::string>& values) {
        std::string text = "[";
        for (std::size_t index = 0; index < values.size(); ++index) text += (index == 0 ? "" : ", ") + json(values[index]);
        return text + "]";
    };
    const auto triple = [](const auto& value) {
        return "[" + number(value[0]) + ", " + number(value[1]) + ", " + number(value[2]) + "]";
    };
    const auto vec3 = [](const assets::Vec3f& value) {
        return "[" + number(value.x) + ", " + number(value.y) + ", " + number(value.z) + "]";
    };
    const double milliseconds = timed_seconds_ > 0.0 && options_.timed_frames > 0
        ? timed_seconds_ * 1000.0 / static_cast<double>(options_.timed_frames) : 0.0;
    std::size_t drawn = 0;
    for (const Item& item : items_) drawn += item.status == "drawn" ? 1U : 0U;
    output << "{\n  \"schema_version\": 1,\n  \"mode\": \"map\",\n"
        << "  \"status\": " << json(status_) << ",\n  \"failure\": " << json(failure_) << ",\n"
        << "  \"backend\": {\"engine\": " << json(backend.engine) << ", \"rendering_method\": "
        << json(backend.rendering_method) << ", \"adapter_vendor\": " << json(backend.adapter_vendor)
        << ", \"adapter_name\": " << json(backend.adapter_name) << ", \"driver_api\": " << json(backend.driver_api)
        << "},\n  \"render_profile\": " << render_profile_report()
        << ",\n  \"profile\": " << json(options_.profile) << ",\n  \"layers\": " << list(options_.layers) << ",\n"
        << "  \"map\": {\"logical_path\": " << json(options_.map_path) << ", \"sha256\": " << json(options_.map_sha256)
        << ", \"kind\": \"space\", \"semantic_complete\": " << (options_.semantic_complete ? "true" : "false") << "},\n"
        << "  \"terrain\": {\"chunks\": 0, \"status\": \"not_applicable\", \"cause\": \"a kind-2 map has no terrain\"},\n";
    if (options_.write_hud_report) options_.write_hud_report(output);
    const lighting::bloom::SceneBloom bloom = options_.bloom.value_or(lighting::bloom::SceneBloom{});
    output << "  \"scene_bloom\": {\"status\": "
        << json(!options_.bloom ? (options_.bloom_skipped_unlit ? "lighting_off" : "off") : !renderer_ ? "renderer_not_created"
                : renderer_->scene_bloom_active() ? "on" : "unsupported_backend")
        << ", \"source\": " << json(environment_count_ > 0 ? "environment 0" : "loader defaults")
        << ", \"strength\": " << number(bloom.strength) << ", \"cutoff\": " << number(bloom.cutoff)
        << ", \"size\": " << number(bloom.size) << ", \"rule\": \"docs/rendering.md#bloom\"},\n";
    if (populate_result_ && populate_result_->write_report) populate_result_->write_report(output);
    else output << "  \"populate\": {\"requested\": " << (options_.populate ? "true" : "false")
                << ", \"composed\": " << populated_ << "},\n";
    if (options_.effects.write_report) options_.effects.write_report(output);
    output << "  \"space\": {\"slice\": " << json(space::environment_scene_id)
        << ", \"catalog_failure\": " << json(catalog_failure_)
        << ",\n    \"environment\": {\"index\": 0, \"count\": " << environment_count_
        << ", \"name\": " << json(environment_name_) << ", \"primary_sky\": " << json(primary_sky_)
        << ", \"secondary_sky\": " << json(secondary_sky_) << ", \"light_0\": ";
    if (light_) {
        output << "{\"rule\": \"R-LIT-01..05 of docs/behaviour/p1-effective-environment.md\", \"toward_light_source\": "
            << vec3(light_->toward_light) << ", \"diffuse\": " << vec3(light_->diffuse)
            << ", \"specular\": " << vec3(light_->specular) << ", \"ambient\": " << vec3(light_->ambient) << '}';
    } else {
        output << "null";
    }
    output << "},\n    \"effect_clock\": {\"clock\": " << json(options_.real_time_clock ? "live" : "held")
        << ", \"offset\": " << options_.clock_offset << ", \"tick_at_capture\": ";
    if (effect_tick_) output << *effect_tick_; else output << "null";
    output << ", \"time_seconds\": " << (effect_tick_ ? number(space::environment_effect_time(*effect_tick_)) : "null")
        << ", \"surfaces\": " << effect_clock_assets_.size() << ", \"rule\": " << json(effect_clock_rule) << '}';
    output << ",\n    \"camera\": {\"source\": " << json(camera_source_) << ", \"status\": " << json(camera_status_)
        << ", \"input\": " << json(options_.camera) << ", \"default_policy\": " << json(space::default_camera_policy)
        << ", \"default\": {\"target_kind\": " << json(default_camera_.target_kind) << ", \"target_record\": ";
    if (default_camera_.target_record) output << *default_camera_.target_record; else output << "null";
    output << ", \"target_type\": " << json(default_camera_.target_type)
        << ", \"target_source\": " << vec3(default_camera_.target_source)
        << ", \"centre_source\": " << vec3(default_camera_.centre_source)
        << ", \"tactical\": {\"distance\": " << number(tactical_.values.distance)
        << ", \"pitch_degrees\": " << number(tactical_.values.pitch_degrees)
        << ", \"fov_degrees\": " << number(tactical_.values.fov_degrees)
        << ", \"near\": " << number(tactical_.values.near_plane) << ", \"source\": " << json(tactical_.source)
        << ", \"failure\": " << json(tactical_.failure) << "}}},\n"
        << "    \"drawn_surfaces\": " << drawn << ",\n    \"surfaces\": [";
    for (std::size_t index = 0; index < items_.size(); ++index) {
        const Item& item = items_[index];
        output << (index == 0 ? "\n      " : ",\n      ") << "{\"role\": " << json(item.role)
            << ", \"object\": " << json(item.object) << ", \"record\": ";
        if (item.record) output << *item.record; else output << "null";
        output << ", \"model\": " << json(item.model) << ", \"mesh\": " << json(item.mesh)
            << ", \"shader\": " << json(item.shader) << ", \"route\": " << json(item.route)
            << ", \"billboard\": " << item.billboard << ", \"texture\": " << json(item.texture)
            << ", \"status\": " << json(item.status) << ", \"cause\": " << json(item.cause)
            << ", \"asset\": " << item.asset << ", \"pass\": " << json(item.status == "drawn" ? to_string(item.pass) : "")
            << '}';
    }
    output << (items_.empty() ? "" : "\n    ") << "],\n    \"idle_placements\": [";
    for (std::size_t index = 0; index < environment_idle_.size(); ++index) {
        const auto& idle = environment_idle_[index];
        output << (index == 0 ? "" : ",") << "{\"object\":" << json(idle.object)
            << ",\"clip\":" << json(idle.clip_path) << ",\"start_frame\":" << idle.playback.start_frame
            << ",\"instances\":" << idle.instances.size() << ",\"sampled_time\":" << number(idle.pose.pose.sampled_time_seconds) << '}';
    }
    output << "],\n    \"not_rendered\": " << list(not_rendered_) << "},\n"
        << "  \"capture_identity\": {\"viewport\": {\"width\": " << camera_.width << ", \"height\": " << camera_.height
        << "}, \"png\": ";
    if (capture_size_) {
        output << "{\"width\": " << (*capture_size_)[0] << ", \"height\": " << (*capture_size_)[1] << '}';
    } else {
        output << "null";
    }
    output << ", \"camera\": ";
    if (camera_status_ == "valid") {
        output << "{\"projection\": \"perspective\", \"position\": " << triple(camera_.eye)
            << ", \"target\": " << triple(camera_.target) << ", \"up\": " << triple(camera_.up)
            << ", \"fov_degrees\": " << number(camera_.vertical_fov_degrees)
            << ", \"near\": " << number(camera_.near_plane) << ", \"far\": " << number(camera_.far_plane) << '}';
    } else {
        output << "null";
    }
    output << "},\n";
    if (bridge_) {
        output << "  \"map_camera\": {\"mode\": \"space\", \"context\": \"space\", \"steps\": "
            << bridge_->steps() << ", \"config_sha256\": " << json(options_.map_camera->config_sha256)
            << ", \"bindings_sha256\": " << json(options_.map_camera->bindings_sha256)
            << viewer::map_overview_source_members(*options_.map_camera) << "},\n";
    }
    output << "  \"frame_time\": {\"warmup_frames\": " << options_.warmup_frames
        << ", \"timed_frames\": " << options_.timed_frames << ", \"elapsed_seconds\": " << number(timed_seconds_)
        << ", \"milliseconds_per_frame\": " << number(milliseconds) << "},\n"
        << "  \"captures\": {" << (capture_hash_.empty() ? "" : "\"configured\": " + json(capture_hash_));
    for (std::size_t index = 0; index < live_captures_.size(); ++index) {
        output << (index == 0 && capture_hash_.empty() ? "" : ", ") << json(live_captures_[index].first) << ": "
               << json(live_captures_[index].second);
    }
    output << "}\n}\n";
    output.close();
    return static_cast<bool>(output);
}

// Defined after the evidence harness: the view submits only after its own
// uploads made every environment surface non-casting.
void EnvironmentView::camera_event(const viewer::camera_input::RawEvent& event) {
    if (bridge_ && overview_ && event.kind == camera_input::RawKind::mouse_wheel && event.pressed) {
        // FoC: a wheel click up zooms in; clicks out at Distance_Max count toward the overview,
        // whose levels take the wheel from the tactical camera.
        const float notches = event.factor > 0.0F ? event.factor : 1.0F;
        const float detents = event.code == camera_input::mouse_code::wheel_up ? -notches : notches;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - overview_clock_).count();
        if (!overview_->wheel(detents, bridge_->controller().state().distance,
                              bridge_->constants().distance_max, now)) {
            return;
        }
    }
    if (bridge_) {
        if (auto handled = bridge_->handle(event); !handled) failure_ = core::format_diagnostic(handled.error());
    }
}

std::optional<tactical::TacticalFrame> EnvironmentView::drawn_frame() const {
    if (!renderer_ || completed_) return std::nullopt;
    return tactical_frame(camera_);
}

std::optional<tactical::TacticalFrame> EnvironmentView::effects_frame() const {
    if (!drawn_frame() || !tactical_.constants) return std::nullopt;
    // BP-04/BP-07: a config-free replay still uses Space_Mode's laser depth
    // range. The bridge can override it, but does not establish its authority.
    const auto& constants = bridge_ && bridge_->active() ? bridge_->constants() : *tactical_.constants;
    return tactical_frame(space::environment_laser_camera(camera_, constants));
}

void EnvironmentView::focus(const float source_x, const float source_y) {
    // Source (x, y) is render (x, -y) on the battle plane.
    if (!bridge_ || completed_) return;
    if (auto moved = bridge_->focus(source_x, -source_y); !moved) failure_ = core::format_diagnostic(moved.error());
}

std::optional<presentation::camera::SourceTargetBounds> EnvironmentView::camera_bounds() const {
    if (!bridge_ || !bridge_->active()) return std::nullopt;
    return bridge_->controller().source_bounds();
}

void EnvironmentView::overview_key() {
    if (overview_) overview_->toggle();
}

std::string EnvironmentView::overview_level() const {
    if (!overview_) return overview_status_;
    switch (overview_->level()) {
    case tactical::OverviewLevel::off: return "off";
    case tactical::OverviewLevel::overview: return "overview";
    case tactical::OverviewLevel::map: return "map";
    }
    return "off";
}

void EnvironmentView::camera_focus(const bool focused) {
    if (bridge_) bridge_->set_focus(focused);
}

void EnvironmentView::camera_pointer_left() {
    if (bridge_) bridge_->pointer_left();
}

void EnvironmentView::camera_viewport(const float width, const float height) {
    if (bridge_ && !completed_) {
        if (auto resized = bridge_->set_viewport(width, height); !resized) {
            failure_ = core::format_diagnostic(resized.error());
        }
    }
}

std::optional<int> EnvironmentView::process(const double delta) {
    if (!renderer_ || completed_) return std::nullopt;
    // #888 perf trace: the main thread's ms in this frame's snapshot builds and renderer submit.
    using SubmitClock = std::chrono::steady_clock;
    submit_ms_ = 0.0;
    const auto submit_since = [this](const SubmitClock::time_point start) {
        submit_ms_ += std::chrono::duration<double, std::milli>(SubmitClock::now() - start).count();
    };
    if (!failure_.empty()) {
        completed_ = true;
        static_cast<void>(fail(failure_));
        return 2;
    }
    if (bridge_) {
        // Pan follows the overview's drawn yaw; the tactical yaw is kept for its return.
        if (overview_ && bridge_->active()) {
            bridge_->set_overview_view_yaw(overview_->view_yaw(bridge_->controller().yaw_degrees()));
        }
        if (auto stepped = bridge_->step(static_cast<float>(delta)); !stepped) {
            static_cast<void>(fail(core::format_diagnostic(stepped.error())));
            completed_ = true;
            return 2;
        }
        tactical::TacticalFrame frame = bridge_->frame();
        if (overview_) {
            overview_->advance(delta);
            const auto& bounds = bridge_->controller().render_bounds();
            // The map overview fits the larger half extent of the camera's target bounds, the
            // project's stand-in for the retail map box.
            const float half_extent = 0.5F * std::max(bounds.max_x - bounds.min_x, bounds.max_z - bounds.min_z);
            auto shown = overview_->frame(frame, bridge_->controller().yaw_degrees(), half_extent);
            if (!shown) {
                static_cast<void>(fail("tactical overview: " + core::format_diagnostic(shown.error())));
                completed_ = true;
                return 2;
            }
            frame = shown.value();
        }
        camera_ = space::environment_view_camera(fixed_camera(frame));
        renderer_->set_camera(camera_);
        const assets::Vec3f eye = space::source_from_render(camera_.eye);
        for (const auto& [index, original] : sky_instances_) {
            const auto matrix = instance_matrix(space::camera_relative_sky_transform(original, eye_source_, eye));
            if (!matrix) {
                completed_ = true;
                static_cast<void>(fail("camera-centred sky transform exceeds the renderer range"));
                return 2;
            }
            instances_[index].fixed_transform = *matrix;
        }
        const auto sky_start = SubmitClock::now();
        snapshot_ = std::make_shared<const sim::RenderSnapshot>(0, instances_);
        submit_since(sky_start);
    }
    // The idle clips' 30 Hz clock follows real time in the live view. The
    // attached effects run on it too, so their period does not scale with
    // the display rate (#186).
    const std::uint32_t tick = options_.real_time_clock ? live_idle_tick(clock_seconds_, delta) : frame_;
    if (options_.effects.follow_lighting) options_.effects.follow_lighting(*renderer_);
    if (options_.effects.tick && !options_.effects.tick(camera_, tick)) {
        completed_ = true;
        static_cast<void>(fail("space attached effect advance failed"));
        return 2;
    }
    if (populate_result_ && populate_result_->tick) populate_result_->tick(*renderer_, tick);
    SpaceLiveUpdate live;
    if (populate_result_ && populate_result_->live) {
        auto update = populate_result_->live(*renderer_, delta, camera_);
        if (!update) {
            completed_ = true;
            static_cast<void>(fail("live population: " + core::format_diagnostic(update.error())));
            return 2;
        }
        live = std::move(update.value());
        if (live.error) {
            show_live_error(*live.error);
            if (!options_.real_time_clock) {
                completed_ = true;
                static_cast<void>(fail("live population: " + *live.error));
                return 2;
            }
        }
        if (live.instances) {
            const auto instances_start = SubmitClock::now();
            instances_.resize(populate_first_);
            instances_.insert(instances_.end(), live.instances->begin(), live.instances->end());
            populated_ = live.instances->size();
            snapshot_ = std::make_shared<const sim::RenderSnapshot>(0, instances_);
            submit_since(instances_start);
        }
    }
    if (const auto failed = advance_effects(tick)) return failed;
    ++frame_;
    const auto submit_start = SubmitClock::now();
    const std::uint64_t sent_before = renderer_->submit_work().transforms_sent;
    renderer_->submit(snapshot_);
    submit_since(submit_start);
    submit_sent_ = renderer_->submit_work().transforms_sent - sent_before;
    if (live.capture_suffix && !options_.capture_path.empty()) {
        std::filesystem::path path = options_.capture_path;
        path.replace_filename(ViewerPath{ViewerPath::utf8(path.stem()) + *live.capture_suffix
            + ViewerPath::utf8(path.extension())}.native());
        auto capture = renderer_->capture(camera_);
        if (!capture || capture.value().png_bytes.empty()) {
            completed_ = true;
            static_cast<void>(fail(capture ? std::string("live capture is empty") : core::format_diagnostic(capture.error())));
            return 2;
        }
        if (const std::string problem = write_file(path, capture.value().png_bytes); !problem.empty()) {
            completed_ = true;
            static_cast<void>(fail("live capture " + ViewerPath::utf8(path) + " " + problem));
            return 2;
        }
        live_captures_.emplace_back(ViewerPath::utf8(path.filename()), hash_bytes(capture.value().png_bytes));
    }
    if (frame_ == options_.warmup_frames) timing_start_ = std::chrono::steady_clock::now();
    if (!live.quit && (frame_ < options_.warmup_frames + options_.timed_frames || live.hold)) return std::nullopt;
    timed_seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - timing_start_).count();
    if (!options_.capture_path.empty()) {
        auto capture = renderer_->capture(camera_);
        if (!capture || capture.value().png_bytes.empty()) {
            completed_ = true;
            static_cast<void>(fail(capture ? std::string("capture is empty") : core::format_diagnostic(capture.error())));
            return 2;
        }
        if (const std::string problem = capture_size_problem(capture.value(), camera_); !problem.empty()) {
            completed_ = true;
            static_cast<void>(fail(problem));
            return 2;
        }
        capture_hash_ = hash_bytes(capture.value().png_bytes);
        capture_size_ = {capture.value().width, capture.value().height};
        if (const std::string problem = write_file(options_.capture_path, capture.value().png_bytes); !problem.empty()) {
            completed_ = true;
            static_cast<void>(fail("requested capture " + ViewerPath::utf8(options_.capture_path) + " " + problem));
            return 2;
        }
    }
    completed_ = true;
    status_ = "space_environment_rendered";
    release();
    if (!write_report()) return 2;
    return 0;
}

void EnvironmentView::close() {
    if (completed_ || !renderer_) return;
    completed_ = true;
    status_ = "space_environment_closed";
    release();
    static_cast<void>(write_report());
    shutdown_trace::mark("close: scene released, report written");
}

} // namespace space_environment_detail

void SpaceEnvironment::close() {
    if (state_->view) state_->view->close();
}

std::optional<presentation::camera::TacticalFrame> SpaceEnvironment::live_camera_frame() const {
    return state_->view ? state_->view->drawn_frame() : std::nullopt;
}

std::optional<presentation::camera::TacticalFrame> SpaceEnvironment::live_effects_camera_frame() const {
    return state_->view ? state_->view->effects_frame() : std::nullopt;
}

std::pair<bool, bool> space_environment_detail::EnvironmentView::pointer_mode(const bool ctrl) const {
    if (!bridge_ || !bridge_->active()) return {};
    const auto& adapter = bridge_->adapter();
    // CU-04: use held camera actions; edge scrolling does not change the art.
    const bool grab = adapter.is_held(camera_input::Action::rotate_grab);
    return {adapter.is_held(camera_input::Action::push_scroll) || (grab && !ctrl), grab && ctrl};
}

std::pair<bool, bool> SpaceEnvironment::live_camera_pointer_mode(const bool ctrl) const {
    return state_->view ? state_->view->pointer_mode(ctrl) : std::pair<bool, bool>{};
}

void SpaceEnvironment::live_camera_focus(const float source_x, const float source_y) {
    if (state_->view) state_->view->focus(source_x, source_y);
}

std::optional<presentation::camera::SourceTargetBounds> SpaceEnvironment::live_camera_bounds() const {
    return state_->view ? state_->view->camera_bounds() : std::nullopt;
}

double SpaceEnvironment::live_submit_ms() const { return state_->view ? state_->view->submit_ms() : 0.0; }

std::size_t SpaceEnvironment::live_submit_pieces() const {
    return state_->view ? state_->view->submit_pieces() : 0U;
}

std::uint64_t SpaceEnvironment::live_submit_sent() const { return state_->view ? state_->view->submit_sent() : 0U; }

void SpaceEnvironment::live_camera_overview_key() {
    if (state_->view) state_->view->overview_key();
}

std::string SpaceEnvironment::live_camera_overview() const {
    return state_->view ? state_->view->overview_level() : std::string("unavailable");
}

} // namespace eawr::presentation::godot_backend
