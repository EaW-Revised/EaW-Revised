#include "map_mode_internal.hpp"
#include "land_battle_camera.hpp"
#include "land_battle_scene.hpp"
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace eawr::presentation::godot_backend {

bool MapMode::State::ready_land_live(Node3D& host, const assets::Map& map,
    std::vector<sim::RenderInstance>& instances, sim::AssetId& next_asset, sim::EntityId& next_entity) {
    live_options.land = true;
    live_options.real_time = options.interactive;
    live_options.warmup_frames = options.warmup_frames;
    // BLD-11/12/13: a land battle waits behind the loading screen's Begin like space does. A hosted
    // screen sets the barrier itself; otherwise `--eawr-live-begin` or the interactive default decides.
    open_loading_screen(host, map);
    UtilityFunctions::print("EAWR land camera viewport at load: ", camera.width, "x", camera.height);
    if (!catalog) return fail_ready("land live battle requires the XML catalog");
    live_session = std::make_unique<LiveSessionView>(live_options);
    std::string error;
    if (!live_session->prepare(*filesystem, *catalog, options.map_path, error) || !live_session->start(error))
        return fail_ready(error);
    if (options.interactive && !map_camera) {
        if (!map.terrain || !tactical || !live_session->start_data())
            return fail_ready("interactive land battle needs terrain and tactical camera constants");
        // Project camera policy: the existing land XML pose, bounded by the terrain,
        // starts on the local player's prepared company members instead of a fixed player marker.
        eawr::viewer::MapCameraConfig config;
        config.mode = camera::Mode::land;
        config.map_path = options.map_path;
        config.map_sha256 = map_hash;
        config.bindings_path = "map-camera-bindings.json";
        config.bounds = {0.0F, static_cast<float>(map.terrain->width - 1) * map.terrain->cell_spacing,
            0.0F, static_cast<float>(map.terrain->height - 1) * map.terrain->cell_spacing,
            options.map_path, "land-skirmish-terrain-extent", "project-authored"};
        double x = 0.0, y = 0.0;
        std::size_t count = 0;
        const auto& start_units = live_session->start_data()->units;
        // The squads, not the base building: it can stand far from them. A start without squads keeps every unit.
        const auto is_squad = [](const eawr::battle::StartUnit& unit) {
            return unit.role == eawr::battle::UnitRole::free_unit || unit.role == eawr::battle::UnitRole::craft;
        };
        const bool has_squads = std::any_of(start_units.begin(), start_units.end(), [&](const auto& unit) {
            return unit.state.owner == live_session->local_player() && is_squad(unit);
        });
        for (const auto& unit : start_units) {
            if (unit.state.owner != live_session->local_player()) continue;
            if (has_squads && !is_squad(unit)) continue;
            x += static_cast<double>(unit.state.position.x.raw()) / static_cast<double>(sim::math::Fixed::scale);
            y += static_cast<double>(unit.state.position.y.raw()) / static_cast<double>(sim::math::Fixed::scale);
            ++count;
        }
        if (count == 0) return fail_ready("interactive land camera has no local starting members");
        const auto target = requested_target.value_or(std::array<float, 2>{
            static_cast<float>(x / static_cast<double>(count)), static_cast<float>(y / static_cast<double>(count))});
        config.target_x = std::clamp(target[0], config.bounds.min_x, config.bounds.max_x);
        config.target_y = std::clamp(target[1], config.bounds.min_y, config.bounds.max_y);
        config.zoom = tactical->zoom;
        config.yaw_degrees = tactical->yaw_degrees;
        auto source = load_map_camera(camera::Mode::land, error, &config);
        if (!source) return fail_ready(error);
        if (!activate_land_camera(host, map, std::move(*source))) return false;
        renderer->set_camera(camera);
    }
    particle_workers = std::make_unique<ParticleWorkers>(live_options.particle_workers.value_or(
        ParticleWorkers::default_count(platform::ThreadWorkerAdapter::hardware_worker_count())));
    live_session->set_pose_workers(particle_workers.get());
    live_session->prepare_land_models(*filesystem, *catalog);
    SpacePopulation::Options population;
    population.map_sha256 = map_hash; population.catalog = &*catalog;
    population.policy = policy; population.shadows = shadows;
    population.environment_source = environment_choice;
    const auto scene_environment = SpacePopulation::environment(map, environment_choice, error);
    if (!scene_environment) return fail_ready(error);
    population.environment = *scene_environment;
    population.animation_frames = options.particle_frames; population.live_clock = true;
    population.placed_ships = live_session->placed_ships();
    land_population = std::make_unique<LandBattleScene>(std::move(population), live_session->land_terrain());
    assets::Map members;
    members.source = map.source; members.kind = assets::MapKind::land;
    if (!land_population->compose(*renderer, members, *filesystem, camera, {}, next_asset, next_entity))
        return fail_ready(land_population->failure());
    land_static_instances = instances;
    instances.insert(instances.end(), land_population->instances().begin(), land_population->instances().end());
    battle = std::make_unique<BattleInput>(host);
    battle->prepare(*filesystem, *catalog, *live_session, options.shaders);
    host.set_process_input(true); host.set_process_unhandled_input(true);
    // Retail land and space share one tactical shell. Land fills its selection cards from the squad roster;
    // the reinforcement pane, garrison timer and minimap stay inert until their tickets.
    if (!build_hud(host, map.context_name)) return fail_ready(failure);
    live_session->loading_complete();
    load_progress.complete();
    if (loading) {
        if (live_session->phase() == LiveSessionView::Phase::ready) loading->show_ready();
        else loading->dismiss();
    }
    UtilityFunctions::print("EAWR land camera viewport at ready: ", camera.width, "x", camera.height);
    sync_battle_hud();
    return true;
}

std::optional<int> MapMode::State::process_land_live(double delta) {
    auto update = live_session->frame(*land_population, *renderer, delta);
    if (!update) {
        failure = core::format_diagnostic(update.error()); completed = true;
        live_session->finish(); status = "failed"; (void)write_report(); return 2;
    }
    const auto& state = update.value();
    if (state.instances) {
        auto instances = land_static_instances;
        instances.insert(instances.end(), state.instances->begin(), state.instances->end());
        snapshot = std::make_shared<const sim::RenderSnapshot>(frame, std::move(instances));
    }
    if (const auto failed = draw_map_frame(delta)) return failed;
    const auto tick = static_cast<std::uint64_t>(live_session->presented_tick());
    if (!options.report_path.empty() && land_trace_tick != tick && land_pose_trace.size() < 8192) {
        land_trace_tick = tick;
        for (const auto& unit : live_session->visible_units()) {
            if (land_pose_trace.size() >= 8192) break;
            const auto root = live_session->squadron_of().find(unit.entity);
            if (root == live_session->squadron_of().end()) continue;
            const auto field = live_session->land_terrain();
            const auto height = field->height_at(
                sim::math::Fixed::from_raw(static_cast<std::int64_t>(unit.position[0] * sim::math::Fixed::scale)),
                sim::math::Fixed::from_raw(static_cast<std::int64_t>(unit.position[1] * sim::math::Fixed::scale)));
            std::ostringstream row;
            row << std::setprecision(12) << "{\"tick\":" << tick << ",\"entity\":" << unit.entity
                << ",\"squad\":" << root->second << ",\"owner\":" << unit.owner << ",\"position\":["
                << unit.position[0] << ',' << unit.position[1] << ',' << unit.position[2] << "],\"yaw\":" << unit.yaw
                << ",\"drawn\":" << (land_population->live_ship_drawn(unit.ship) ? "true" : "false")
                << ",\"ground_height\":";
            if (height) row << static_cast<double>(height->raw()) / static_cast<double>(sim::math::Fixed::scale);
            else row << "null";
            row << '}';
            land_pose_trace.push_back(row.str());
        }
    }
    LandBattleCamera view(camera, map_camera.get());
    battle->frame(*live_session, *land_population, view);
    battle->cursor_frame(*live_session, view, delta, !options.capture_path.empty());
    live_session->retire_clones(*land_population, *renderer);
    if (hud) { sync_battle_hud(); sync_cards(); sync_production(); }
    const auto capture = [&](std::filesystem::path path) {
        auto result = renderer->capture(camera);
        if (!result) { failure = core::format_diagnostic(result.error()); return false; }
        std::error_code code;
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), code);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(result.value().png_bytes.data()),
            static_cast<std::streamsize>(result.value().png_bytes.size()));
        if (!output) failure = "cannot write land live capture";
        return bool(output);
    };
    if (state.capture_suffix && !options.capture_path.empty()) {
        auto path = options.capture_path;
        path.replace_filename(path.stem().string() + *state.capture_suffix + path.extension().string());
        if (!capture(path)) { live_session->finish(); completed = true; status = "failed"; (void)write_report(); return 2; }
    }
    if (!state.quit && (options.interactive || state.hold || frame < options.warmup_frames + options.timed_frames))
        return std::nullopt;
    live_session->finish();
    if (!options.capture_path.empty() && !capture(options.capture_path)) {
        completed = true; status = "failed"; (void)write_report(); return 2;
    }
    completed = true;
    if (state.error) failure = *state.error;
    if (failure.empty() && !live_session->finished_successfully()) failure = "land live session or headless verification failed";
    status = failure.empty() ? "land_live_passed" : "failed";
    if (!write_report()) return 2;
    return failure.empty() ? 0 : 2;
}

} // namespace eawr::presentation::godot_backend
