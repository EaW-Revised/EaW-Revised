#include "frame_timer.hpp"
#include "startup_trace.hpp"
#include "gpu_memory_trace.hpp"
#include "battle_content.hpp"
#include "map_mode.hpp"
#include "map_mode_internal.hpp"
#include "eawr/presentation/camera/overview.hpp"
#include "render_profile_viewport.hpp"
#include "settings_store.hpp"
#include "viewer_path.hpp"
#include "eawr/presentation/particles/map_attachment_owner.hpp"

#include <godot_cpp/classes/project_settings.hpp>

#include <cctype>
#include <cmath>
#include <cstdlib>

#include "map_mode_ready_internal.hpp"

namespace eawr::presentation::godot_backend {

bool MapMode::State::prepare_live_population(Node3D& host, const assets::Map& map, std::optional<eawr::viewer::MapCameraSource>& space_camera_source, std::vector<std::uint32_t>& session_records) {
    State& state = *this;
    // #80: the session's units are drawn from its snapshots, through
    // the placed-ship upload path.
    if (state.space_place_object || !state.space_place_at.empty()) {
        return state.fail_ready("--eawr-live-session is not composed with --eawr-space-place-object or -at");
    }
    if (!state.catalog) return state.fail_ready("--eawr-live-session requires the XML catalog, which did not load");
    state.live_options.real_time = state.options.interactive;
    state.live_options.warmup_frames = state.options.warmup_frames;
    // #1923: the loading screen covers the preparation below (BLD-10); the map is read.
    state.open_loading_screen(host, map);
    state.load_report(presentation::ui::load_stage_map, 1, 1);
    if (state.live_options.audio_pace && !state.live_options.real_time) {
        // #474: see the Options::audio_pace comment (live_session_options.hpp). A frame at
        // this rate covers exactly one tick's worth of real time, so BattleAudio's
        // requests reach Godot's real mixer on the same real schedule as the battle's
        // own tick rate, on every host.
        const double fps = static_cast<double>(sim::tactical::logical_frames_per_second)
            / state.live_options.ticks_per_frame;
        Engine::get_singleton()->set_max_fps(std::max(1, static_cast<int>(std::llround(fps))));
    }
    state.live_session = std::make_unique<LiveSessionView>(state.live_options);
    std::string live_failure;
    if (!state.live_session->prepare(*state.filesystem, *state.catalog, state.options.map_path, live_failure)) {
        return state.fail_ready(live_failure);
    }
    state.load_report(presentation::ui::load_stage_session, 1, 1);
    if (state.options.attract) {
        // MAT-10: the map's saved camera, locked; a map without one keeps the default view.
        state.attract_camera = skirmish::attract_camera(map);
        if (state.attract_camera) state.attract_motion.emplace(*state.attract_camera);
    } else if (!space_camera_source && state.live_session->start_data()) {
        const auto& camera_setup = state.live_options.borrowed_battle
            ? state.live_options.borrowed_battle->replay.setup : state.live_session->start_data()->setup;
        const bool native_camera = state.live_options.borrowed_battle || camera_setup.native_scene.has_value();
        auto config = native_camera
            ? eawr::viewer::campaign_camera_config(map, camera_setup, state.live_session->local_player())
            : eawr::viewer::skirmish_camera_config(
                map, *state.live_session->start_data(), state.live_session->local_player());
        if (!config) return state.fail_ready(core::format_diagnostic(config.error()));
        space_camera_source = state.load_map_camera(camera::Mode::space, live_failure, &config.value());
        if (!space_camera_source) return state.fail_ready(live_failure);
    }
    // #391: the breakoff props join the placed ships before they are composed.
    // #638: the particle systems of the battle step on a small pool of their own
    // (ParticleWorkers::default_count; --eawr-live-particle-workers sets it).
    state.particle_workers = std::make_unique<ParticleWorkers>(state.live_options.particle_workers.value_or(
        ParticleWorkers::default_count(platform::ThreadWorkerAdapter::hardware_worker_count())));
    state.debris_props = std::make_unique<DebrisProps>(host, *state.filesystem, *state.catalog);
    // One participant uses the retained serial pose path, without worker staging.
    state.live_session->set_pose_workers(
        state.particle_workers->count() > 1 ? state.particle_workers.get() : nullptr);
    state.debris_props->set_workers(state.particle_workers.get());
    if (auto detail = state.debris_props->set_particle_detail(state.live_options.particle_detail); !detail)
        return state.fail_ready(core::format_diagnostic(detail.error()));
    state.live_session->attach_debris(*state.debris_props);
    state.load_report(presentation::ui::load_stage_effects, 1, 5);
    // #456: so do the model projectiles' slots.
    state.battle_effects = std::make_unique<BattleEffects>(host, *state.filesystem, *state.catalog);
    state.battle_effects->set_workers(state.particle_workers.get());
    if (auto detail = state.battle_effects->set_particle_detail(state.live_options.particle_detail); !detail)
        return state.fail_ready(core::format_diagnostic(detail.error()));
    state.battle_effects->collect_axis_diagnostics(!state.options.report_path.empty());
    state.battle_effects->prepare(*state.live_session->tables(), *state.live_session->combat());
    state.live_session->attach_projectile_models(*state.battle_effects);
    state.load_report(presentation::ui::load_stage_effects, 2, 5);
    state.space_place_at = state.live_session->placed_ships();
    session_records = state.live_session->session_records();
    // MAT-10, WGUI-76: a demo battle takes no selection or orders, so it has no battle input.
    if (!state.options.attract) {
        state.battle = std::make_unique<BattleInput>(host);
        state.battle->prepare(*state.filesystem, *state.catalog, *state.live_session,
                              state.options.shaders);
    }
    state.load_report(presentation::ui::load_stage_effects, 3, 5);
    if (state.battle) state.battle->set_move_feedback([&state](const sim::math::Vec3& point,
        const ui::OrderMode mode, const bool double_click, const std::uint64_t tick) {
        if (state.battle_effects) state.battle_effects->move_feedback(point, mode, double_click, tick);
        if (state.hud && state.hud->minimap()) {
            const auto bounds = state.space ? state.space->live_camera_bounds() : std::nullopt;
            if (!bounds) return;
            constexpr double unit = static_cast<double>(sim::math::Fixed::scale);
            const auto centre = ui::minimap_point(ui::minimap_extents(
                bounds->min_x, bounds->max_x, bounds->min_y, bounds->max_y),
                static_cast<double>(point.x.raw()) / unit, static_cast<double>(point.y.raw()) / unit);
            state.hud->minimap()->move_feedback(centre, mode, double_click);
        }
    });
    // #494: the local player's fog of war in the world.
    state.live_fog = std::make_unique<LiveFogView>(host);
    state.live_fog->prepare(*state.filesystem, state.live_options.reveal, state.live_options.deploy_overlay);
    state.load_report(presentation::ui::load_stage_effects, 4, 5);
    // #76: the command bar's ability buttons read and switch the simulated abilities;
    // --eawr-live-ability-demo keeps the stand-in's staged states to look at.
    if (state.battle && !state.live_session->options().ability_demo) {
        state.battle->set_abilities(&state.live_session->abilities(), &state.live_session->abilities());
    }
    // #84: sound by default in the interactive view, muted in a capture run.
    // MAT-18: a demo battle starts no 3D sound event, queues no speech, announcement or
    // GUI event and starts no tactical music, and mutes no bus; the menu's music and UI
    // sounds, which are the front end's, are not touched.
    const bool audible = state.audio_argument.empty() ? state.options.interactive
                                                      : state.audio_argument == "on";
    // #2927: the saved volumes, when a player's view has them; FoC's 0.75 otherwise.
    BattleAudio::Options audio_options{.muted = !audible && !state.options.attract.has_value(),
                                       .attract_silent = state.options.attract.has_value()};
    if (const auto* saved = ActiveSettings::get()) {
        const presentation::ui::SessionValues values = presentation::ui::session_values(*saved);
        audio_options.levels.master = values.master_gain;
        audio_options.levels.sfx = values.sfx_gain;
        audio_options.levels.speech = values.speech_gain;
        audio_options.levels.music = values.music_gain;
    }
    state.battle_audio = std::make_unique<BattleAudio>(host, state.filesystem, *state.catalog,
                                                       audio_options);
    state.battle_audio->prepare(*state.live_session->tables(), *state.live_session->combat(),
                                state.live_session->local_faction(), state.live_session->audio_preload_types());
    state.load_report(presentation::ui::load_stage_effects, 5, 5);
    // #394: the live units' own emitters (engines, destroyed hardpoints' damage), which
    // the static attached plan cannot run on moving units.
    if (state.options.map_effects) {
        state.unit_emitters = std::make_unique<UnitEmitters>(host, *state.filesystem,
            state.live_session->durability()->profiles);
        state.unit_emitters->collect_axis_diagnostics(!state.options.report_path.empty());
        state.unit_emitters->set_workers(state.particle_workers.get());
        if (auto detail = state.unit_emitters->set_particle_detail(state.live_options.particle_detail); !detail)
            return state.fail_ready(core::format_diagnostic(detail.error()));
    }
    return true;
}

SpacePopulateHook MapMode::State::make_population_hook() {
    State& state = *this;
    return state.space_population
        ? SpacePopulateHook{[population = state.space_population.get(), filesystem = &*state.filesystem,
                             live_session = state.live_session.get(), map_state = &state,
                             effects = state.battle_effects.get(), debris = state.debris_props.get(),
                             emitters = state.unit_emitters.get(), sound = state.battle_audio.get(),
                             fog = state.live_fog.get()]
            (const SpacePopulateContext& context) -> core::Result<SpacePopulateResult> {
            if (!population->compose(context.renderer, context.map, *filesystem, context.camera,
                                     context.environment_records, context.first_asset, context.first_entity)) {
                const std::string failure = population->failure();
                population->release(context.renderer);
                return core::Result<SpacePopulateResult>::failure({.code = "EAWR-VIEWER-SPACE-POPULATE",
                                                                    .message = failure});
            }
            if (live_session && !live_session->warm_campaign_slots(*population, context.renderer)) {
                const std::string failure = population->failure();
                population->release(context.renderer);
                return core::Result<SpacePopulateResult>::failure({.code = "EAWR-VIEWER-SPACE-POPULATE", .message = failure});
            }
            if (emitters) {
                core::load_profile::Scope prepare_emitters(core::load_profile::Phase::particles);
                emitters->prepare_assets(*population);
                map_state->load_report(presentation::ui::load_stage_scene, 1, 1);
            }
            SpacePopulateResult result;
            result.instances = population->instances();
            result.tick = [population, live_session, map_state](GodotRenderer& renderer, const std::uint32_t sample) {
                population->trace_frames(map_state->perf_trace.has_value());
                if (!live_session) population->pose_units(renderer, sample);
                else if (map_state->options.attract) population->defer_unit_sample(sample);
            };
            result.release = [population, live_session, effects, debris, emitters, sound, fog](GodotRenderer& renderer) {
                if (live_session) live_session->finish();
                if (fog) fog->release();
                if (effects) effects->release();
                if (sound) sound->release();
                if (debris) debris->release();
                if (emitters) emitters->release();
                population->release(renderer);
            };
            result.write_report = [population, live_session, map_state, effects, debris, emitters,
                                   sound, fog](std::ostream& output) {
                population->write_report(output);
                if (live_session) live_session->write_report(output);
                if (fog) fog->write_report(output);
                if (effects) effects->write_report(output);
                if (sound) sound->write_report(output);
                if (debris) debris->write_report(output);
                if (emitters) emitters->write_report(output);
                startup_trace.write(output);
                gpu_memory_trace.write(output);
                if (map_state->battle && map_state->space) map_state->battle->write_report(output, *map_state->space);
            };
            if (live_session) {
                if (!map_state->options.attract) result.render_clock = &live_session->render_clock();
                // State owns these borrowed components through the final frame; release never invokes the probe.
                if (MemoryAttribution::enabled()) {
                    live_session->memory_capacity_probe([sound, fog, map_state] {
                    diagnostics::MemoryCapacity result;
                    if (sound) result.append(sound->memory_capacity());
                    if (fog) result.append(fog->memory_capacity());
                    if (map_state->hud) result.append(map_state->hud->memory_capacity());
                    return result;
                    });
                    live_session->owner_census_probe([emitters, map_state, renderer = &context.renderer](diagnostics::OwnerCensus& output) {
                        scene::VfsAssetCache(*map_state->filesystem).owner_census(output);
                        renderer->owner_census(output);
                        if (map_state->battle) map_state->battle->owner_census(output);
                        if (map_state->space) map_state->space->owner_census(output);
                        if (map_state->particles) map_state->particles->owner_census(output);
                        if (emitters) emitters->owner_census(output);
                        for (const auto& player : map_state->unit_clips) {
                            if (!output.step(diagnostics::OwnerCensus::animation)) break;
                            if (player) player->owner_census(output);
                        }
                        for (const auto& clip : map_state->attached_owners.clips) {
                            if (!output.step(diagnostics::OwnerCensus::animation)) break;
                            if (clip.player) clip.player->owner_census(output);
                        }
                        // XML can own far more distinct small ranges than the other
                        // families. Let it consume the remaining bounded slots last.
                        if (map_state->catalog) map_state->catalog->owner_census(output);
                    });
                }
                // The session starts once its units are composed (#80).
                std::string live_failure;
                live_session->trace_frames(map_state->perf_trace.has_value());
                if (!live_session->start(live_failure)) {
                    population->release(context.renderer);
                    return core::Result<SpacePopulateResult>::failure(
                        {.code = "EAWR-VIEWER-LIVE-SESSION", .message = live_failure});
                }
                // start() keeps standalone admission paused at tick 0 until the existing loading gate.
                // The probe never changes that gate or samples an already running borrowed feed.
                live_session->capture_memory_capacity_post_load(*population);
                // The rig's live-quit driver waits for this line before it lets the battle run.
                godot::UtilityFunctions::print("EAWR live session started");
                result.live = [population, live_session, effects, debris, emitters, sound, fog,
                               map_state](GodotRenderer& renderer, const double delta,
                                          const FixedCamera& camera) {
                    live_session->trace_frames(map_state->perf_trace.has_value());
                    population->trace_frames(map_state->perf_trace.has_value());
                    map_state->fog_ms = 0.0;
                    map_state->audio_ms = 0.0;
                    auto update = live_session->frame(*population, renderer, delta);
                    if (startup_trace.pending()) {
                        const auto costs = live_session->tick_costs_after(0);
                        if (!costs.empty()) {
                            double lua = 0.0, fog_cost = 0.0;
                            for (const auto& phase : costs.front().phases) {
                                if (phase.name == "lua") lua = phase.ms;
                                if (phase.name == "fog") fog_cost = phase.ms;
                            }
                            startup_trace.first_tick(costs.front().total_ms, lua, fog_cost);
                        }
                    }
                    if (map_state->options.setup && !map_state->options.interactive && live_session->battle_end()
                        && live_session->battle_end()->ended_frame) {
                        live_session->quit();
                        if (update) update.value().quit = true;
                    }
                    // #494: the fog plane follows this frame's snapshot before it is drawn.
                    if (fog && update && !update.value().error) {
                        FrameTimer timer(map_state->perf_trace ? &map_state->fog_ms : nullptr);
                        fog->frame(*live_session, map_state->space ? map_state->space->live_camera_bounds()
                                                                   : std::nullopt,
                            map_state->hud && map_state->hud->production() && map_state->hud->production()->pane_open(),
                            map_state->battle && map_state->battle->placing().has_value());
                    }
                    // #453, #459: the HUD shows this frame's time panel and outcome.
                    {
                        FrameTimer timer(map_state->perf_trace ? &map_state->hud_ms : nullptr, true);
                        map_state->sync_battle_hud();
                    }
                    const auto& battle = live_session->battle_frame();
                    map_state->particle_ms = 0.0;
                    map_state->emitter_frame_ms = map_state->effects_frame_ms = map_state->debris_frame_ms = 0.0;
                    GodotParticleBackend::begin_frame_measurement(map_state->perf_trace.has_value());
                    const bool fine_particles=map_state->perf_trace.has_value()&&PerfTrace::fine_particle_timings();
                    if (emitters) emitters->measure_particles(fine_particles);
                    if (effects) effects->measure_particles(fine_particles);
                    if (debris) debris->measure_particles(fine_particles);
                    if (emitters) emitters->measure_preparation(map_state->perf_trace.has_value());
                    if (effects) effects->measure_preparation(map_state->perf_trace.has_value());
                    if (!update || update.value().error || !battle.latest) return update;
                    // #638: the perf trace's particle_ms, the main thread's time in the unit
                    // emitters', battle effects' and breakoff props' frames below.
                    using ParticleClock = std::chrono::steady_clock;
                    const auto particle_since = [map_state](const ParticleClock::time_point start) {
                        map_state->particle_ms
                            += std::chrono::duration<double, std::milli>(ParticleClock::now() - start).count();
                    };
                    core::load_profile::Scope startup_particles(core::load_profile::Phase::particles);
                    auto particle_start = ParticleClock::now();
                    FrameTimer emitter_timer(map_state->perf_trace ? &map_state->emitter_frame_ms : nullptr);
                    if (map_state->battle) map_state->battle->follow_waypoint_lighting(renderer);
                    if (emitters) emitters->follow_lighting(renderer);
                    // #394: the units' engine and damage emitters, at the poses just drawn.
                    if (emitters && !emitters->frame(*population, battle.reached,
                            [live_session](const std::uint64_t tick) { return live_session->snapshot_at(tick); },
                            live_session->local_player(), *battle.previous, *battle.latest,
                            // #421: a death clone's pose and clip pose at a sample's tick.
                            [live_session, population, emitters](const std::size_t ship, const double tick)
                                -> std::optional<UnitEmitters::ClonePose> {
                                FrameTimer timer(emitters->clone_prepare_timer(), true);
                                const auto shown = live_session->clone_frame(*population, ship, tick);
                                const animation::Player* player = population->live_clip(ship);
                                if (!shown || !shown->death || player == nullptr) return std::nullopt;
                                const auto transform = population->live_transform(shown->pose);
                                if (!transform) return std::nullopt;
                                const auto held = population->held_clip_bones(ship, shown->death->position,
                                    shown->death->blend_from);
                                if (!held.empty()) return UnitEmitters::ClonePose{*transform,
                                    std::vector<animation::BonePose>(held.begin(), held.end())};
                                auto pose = animation::sample_death_frame(*player, *shown->death);
                                if (!pose) return std::nullopt;
                                return UnitEmitters::ClonePose{*transform, std::move(pose.value().bones)};
                            },
                            // #456: a model projectile's pose at a sample's tick.
                            [live_session, population, effects](const std::size_t ship, const double tick)
                                -> std::optional<sim::math::Mat3x4> {
                                if (effects == nullptr) return std::nullopt;
                                const auto pose = effects->projectile_model_pose_at(ship,
                                    [live_session](const std::uint64_t at) { return live_session->snapshot_at(at); }, tick);
                                return pose ? population->live_transform(*pose) : std::nullopt;
                            },
                            camera, battle.presented_tick, live_session->options().reveal,
                            [live_session](const sim::EntityId entity) { return live_session->unit_opacity(entity); },
                            live_session->unfogged_map_props(),
                            [effects](const std::size_t ship) {
                                return effects ? effects->projectile_model_opacity(ship) : 0.0F;
                            })) {
                        return core::Result<SpaceLiveUpdate>::failure(
                            {.code = "EAWR-VIEWER-UNIT-EMITTERS", .message = emitters->failure()});
                    }
                    particle_since(particle_start);
                    // #429: the clones that left this frame go once the emitters ran the
                    // samples they still stood in.
                    {
                        FrameTimer retirement(emitters ? emitters->clone_retire_timer() : nullptr);
                        live_session->retire_clones(*population, renderer, [population, emitters](const std::size_t ship) {
                            return !emitters || emitters->retire_clone(ship, population->terminal_clip_bones(ship));
                        });
                    }
                    emitter_timer.finish();
                    if (!effects) return update;
                    // #80: the frame's shots, hits and explosions, from the snapshots only.
                    particle_start = ParticleClock::now();
                    FrameTimer effects_timer(map_state->perf_trace ? &map_state->effects_frame_ms : nullptr);
                    auto effects_camera = camera;
                    if (map_state->space) {
                        if (const auto tactical_frame = map_state->space->live_effects_camera_frame()) {
                            effects_camera.near_plane = tactical_frame->near_plane;
                            effects_camera.far_plane = tactical_frame->far_plane;
                        }
                    }
                    const bool shown = effects->frame(battle.reached, *battle.previous, *battle.latest,
                        battle.alpha, [live_session](const sim::EntityId entity) {
                            return live_session->unit_frame(entity);
                        }, camera, battle.presented_tick,
                        [live_session](const std::uint64_t tick) { return live_session->snapshot_at(tick); },
                        [population](const sim::EntityId entity, const std::uint32_t bone)
                            -> std::optional<BattleEffects::ContactBoneFrame> {
                            const auto posed = population->live_bone_frame(entity, bone);
                            if (!posed) return std::nullopt;
                            return BattleEffects::ContactBoneFrame{
                                particles::source_emitter_frame(posed->transform), posed->visible};
                        }, &effects_camera);
                    particle_since(particle_start);
                    effects_timer.finish();
                    if (!shown) {
                        return core::Result<SpaceLiveUpdate>::failure(
                            {.code = "EAWR-VIEWER-BATTLE-EFFECTS", .message = effects->failure()});
                    }
                    // #84: the frame's sounds and the unit responses to the player's gestures.
                    // The ability clicks are taken every frame so a run without battle audio keeps none.
                    auto ability_clicks = live_session->take_ability_clicks();
                    auto reinforcement_feedback = live_session->take_reinforcement_feedback();
                    if (sound) {
                        FrameTimer timer(map_state->perf_trace ? &map_state->audio_ms : nullptr);
                        sound->frame(*live_session, map_state->battle ? map_state->battle->take_acknowledgements()
                                                                      : std::vector<BattleInput::Acknowledgement>{},
                                     std::move(ability_clicks), camera, delta, std::move(reinforcement_feedback),
                                     effects->take_terminal_sounds());
                    }
                    // #391: the breakoff props' fires and explosions.
                    particle_start = ParticleClock::now();
                    FrameTimer debris_timer(map_state->perf_trace ? &map_state->debris_frame_ms : nullptr);
                    if (debris && !debris->effects(camera, battle.presented_tick)) {
                        return core::Result<SpaceLiveUpdate>::failure(
                            {.code = "EAWR-VIEWER-BREAKOFF-PROPS", .message = debris->failure()});
                    }
                    particle_since(particle_start);
                    return update;
                };
            }
            return core::Result<SpacePopulateResult>::success(std::move(result));
        }} : SpacePopulateHook{};
}

bool MapMode::State::ready_space(Node3D& host, const assets::Map& map,
    const assets::ObjectTypeCatalog& placement_catalog, const std::string& catalog_failure) {
    State& state = *this;

        if (state.effect_animation_idle()) {
            return state.fail_ready("--eawr-map-effect-animation idle applies only to land maps");
        }
        std::optional<eawr::viewer::MapCameraSource> space_camera_source;
        if (!state.map_camera_config_path.empty()) {
            // P1 #30: the space tactical camera is tactical only. Free flight
            // has no space policy, so its opt-ins are refused explicitly.
            if (state.map_free_selftest || state.map_free_terminal_hold_test
                || state.map_free_terminal_release_test) {
                return state.fail_ready("the space map camera has no free flight: free-camera options are unsupported");
            }
            std::string camera_failure;
            space_camera_source = state.load_map_camera(camera::Mode::space, camera_failure);
            if (!space_camera_source) return state.fail_ready(camera_failure);
        }
        if (state.fog) {
            // Opt-in synthetic space fog: units only from caller-declared XML
            // element types; the sky is never a fog consumer.
            if (const std::string invalid = space_fog::validate_admission(state.space_fog_admit); !invalid.empty()) {
                return state.fail_ready(invalid + "; no space placement classification is inferred");
            }
            if (state.fog_paint_requested || state.fog_inject_input) {
                return state.fail_ready("interactive fog painting and injected fog input are not wired for space maps; "
                               "use --eawr-fog-paint-evidence without --eawr-capture");
            }
        }
        const bool live = !state.live_options.fixture.empty();
        if (state.populate) {
            // P1-11 #32: the default environment view calls the population
            // hook after composing its sky and background placements.
            if (state.fog) return state.fail_ready("--eawr-populate is not composed with space fog, which admits its own units");
            if (!state.space_control.empty()) {
                return state.fail_ready("--eawr-populate takes no --eawr-space-control");
            }
            std::string environment_failure;
            const auto space_lighting = SpacePopulation::environment(map, state.environment_choice, environment_failure);
            if (!space_lighting) return state.fail_ready(environment_failure);
            if (!state.space_hardpoint_states.empty() && !state.space_place_object) {
                return state.fail_ready("--eawr-space-hardpoint-state requires --eawr-space-place-object");
            }
            if (state.space_place_object) state.space_place_object->hardpoint_states = state.space_hardpoint_states;
            if (state.space_place_object && !state.space_place_at.empty()) {
                return state.fail_ready("--eawr-space-place-at is not composed with --eawr-space-place-object");
            }
            std::vector<std::uint32_t> session_records;
            if (live) {
                if (!state.prepare_live_population(host, map, space_camera_source, session_records)) return false;
            }
            state.space_population = std::make_unique<SpacePopulation>(SpacePopulation::Options{
                .map_sha256 = state.map_hash,
                .catalog = state.catalog ? &*state.catalog : nullptr,
                .policy = state.policy,
                .shadows = state.shadows,
                .environment = *space_lighting,
                .environment_source = state.environment_choice,
                .animation_frames = state.options.particle_frames,
                .live_clock = state.options.interactive || (state.live_session != nullptr && !state.options.attract),
                .idle_offset = state.idle_offset.value_or(0U),
                .team_colour = [](const scene::Placement& placement) { return placement.team_colour; },
                .reject_upload_for_test = state.space_populate_reject_upload_test,
                .debug_ship = state.space_place_object,
                .placed_ships = state.space_place_at,
                .session_records = std::move(session_records),
                // The composed scene (the debug ship included) and its
                // hardpoint states re-plan the attached effects (#136). The
                // live session's units move: their emitters are UnitEmitters' (#394),
                // so this static plan plans none.
                .attached_effects = state.options.map_effects && state.catalog && !live
                    ? std::function<bool(SpacePopulation&)>([&state, host = &host](SpacePopulation& population) {
                          return state.sync_space_attached(*host, population);
                      })
                    : std::function<bool(SpacePopulation&)>{},
                // BLD-10: placements, then one final emitter-asset preparation batch.
                .progress = live ? std::function<void(std::size_t, std::size_t)>(
                                       [&state](const std::size_t done, const std::size_t total) {
                                           state.load_report(presentation::ui::load_stage_scene, done,
                                                             state.unit_emitters ? total + 1 : total);
                                       })
                                 : std::function<void(std::size_t, std::size_t)>{},
            });
        } else if (state.space_place_object || !state.space_hardpoint_states.empty()) {
            return state.fail_ready("--eawr-space-place-object requires --eawr-populate");
        } else if (!state.space_place_at.empty()) {
            return state.fail_ready("--eawr-space-place-at requires --eawr-populate");
        } else if (live) {
            return state.fail_ready("--eawr-live-session requires --eawr-populate");
        } else if (state.idle_offset) {
            return state.fail_ready("--eawr-map-idle-offset requires --eawr-populate");
        } else if (state.policy != lighting::Policy::off || state.shadows || state.environment_choice != "default") {
            return state.fail_ready("space map mode composes no lighting without --eawr-populate; the sky adapter is unshaded");
        }
        if (state.populate && state.options.map_effects && state.catalog && !live) {
            scene::VfsAssetCache cache(*state.filesystem);
            scene::BuildInput input;
            input.map = &map;
            input.map_sha256 = state.map_hash;
            input.catalog = &*state.catalog;
            input.access = cache.access();
            state.scene = scene::build(input);
            state.build_attached_plan(cache, state.space_population.get());
            if (!state.attached_animation_failure.empty()) return state.fail_ready(state.attached_animation_failure);
            if (!state.attached_plan.records.empty()) {
                state.particles = std::make_unique<MapParticleProvider>(host, *state.filesystem);
                if (!state.particles->prepare_attached(state.attached_plan, *state.scene)) {
                    return state.fail_ready("space attached effect preparation failed");
                }
            }
        }
        if (state.space_population) {
            SpacePopulation::AttachedEffects attached = state.space_attached_effects();
            if (live) attached.cause = "--eawr-live-session: the live units' emitters follow them under unit_emitters (#394)";
            state.space_population->set_attached_effects(std::move(attached));
        }
        // Population compose re-plans over its own scene, which may admit
        // emitters (the debug ship's) this map-only plan has none of.
        const bool attached_effects = state.populate && state.options.map_effects && state.catalog && !live;
        state.space = std::make_unique<SpaceEnvironment>(SpaceEnvironment::Options{
            .map_path = state.options.map_path,
            .map_sha256 = state.map_hash,
            .semantic_complete = state.semantic_complete,
            .profile = state.profile,
            .layers = state.layers,
            .shaders = state.options.shaders,
            .report_path = state.options.report_path,
            .capture_path = state.options.capture_path,
            .warmup_frames = state.options.warmup_frames,
            .timed_frames = state.options.timed_frames,
            .real_time_clock = state.options.interactive,
            .clock_hold_ticks = state.options.particle_frames,
            .clock_offset = state.idle_offset.value_or(0U),
            .camera = state.space_camera,
            .saved_camera = state.attract_camera,
            .control = state.space_control,
            .map_camera = std::move(space_camera_source),
            .camera_selftest = state.map_camera_selftest,
            .camera_terminal_baseline_test = state.map_camera_terminal_baseline_test,
            .camera_terminal_hold_test = state.map_camera_terminal_hold_test,
            .camera_terminal_release_test = state.map_camera_terminal_release_test,
            .unlocked_capture_path = state.map_camera_unlocked_capture_path,
            .bloom = state.scene_bloom,
            .bloom_skipped_unlit = state.bloom_skipped_unlit,
            .fog = state.fog ? &*state.fog : nullptr,
            .catalog = state.catalog ? &*state.catalog : nullptr,
            .fog_admit = state.space_fog_admit,
            .fog_paint_evidence = state.fog ? state.fog_paint_evidence : std::filesystem::path{},
            .populate = state.make_population_hook(),
            .effects = attached_effects ? SpaceEffectsHook{
                .tick = [&state](const FixedCamera& camera, const std::uint32_t tick) {
                    if (!state.particles || !state.particles->has_work()) return true;
                    // Sample n is n/30 s. Tactical ticks come from FD-74
                    // admission; standalone captures retain their held sample.
                    const bool admitted = state.live_session && !state.options.attract;
                    const std::uint32_t last = state.options.interactive || admitted
                        ? tick : std::min(tick, state.options.particle_frames - 1U);
                    const auto view = particles::camera_frame_from_render(camera.eye, camera.target, camera.up);
                    for (std::uint32_t due = particles::map_owner_samples_due(state.particles->frames(), last);
                         due != 0; --due) {
                        if (!state.particles->advance(particles::map_owner_delta(state.particles->frames()), view)) {
                            return false;
                        }
                    }
                    return true;
                },
                .follow_lighting = [&state](const GodotRenderer& renderer) {
                    if (state.particles) state.particles->follow_lighting(renderer);
                },
                .release = [&state]() {
                    if (state.particles) state.particles->release();
                },
                .write_report = [&state](std::ostream& output) {
                    if (!state.particles) return;
                    output << "  \"map_particles\": {\"enabled\": true, \"advanced_frames\": "
                        << state.particles->frames() << ", \"clock\": "
                        << json(state.live_session && !state.options.attract ? "admitted"
                            : state.options.interactive ? "live" : "held") << ", \"attached_records\": "
                        << state.attached_plan.records.size()
                        << ", \"aggregate_capacity\": " << state.attached_plan.aggregate_capacity
                        << ", \"allocated_capacity\": " << state.attached_plan.allocated_capacity
                        << ", \"live_rids_after_release\": "
                        << state.particles->live_rids() << ", \"live_resources_after_release\": "
                        << state.particles->live_resources() << ", \"placements\": [";
                    const auto& placed = state.particles->placements();
                    for (std::size_t index = 0; index < placed.size(); ++index) {
                        output << (index ? ", " : "") << "{\"identity\": " << json(placed[index].identity)
                            << ", \"status\": " << json(placed[index].status)
                            << ", \"effect\": " << json(placed[index].logical_path)
                            << ", \"seed\": " << placed[index].seed
                            << ", \"capacity\": " << placed[index].capacity
                            << ", \"particles\": " << placed[index].stats.particles
                            << ", \"particle_hash\": " << placed[index].stats.hash << "}";
                    }
                    output << "]},\n";
                },
            } : SpaceEffectsHook{},
            .write_hud_report = [&state](std::ostream& output) {
                if (state.loading) output << "  \"loading_screen\": " << state.loading->report_json() << ",\n";
                else if (!state.loading_failure.empty())
                    output << "  \"loading_screen\": {\"shown\": false, \"failure\": " << json(state.loading_failure) << "},\n";
                if (state.hud) output << "  \"hud\": " << state.hud->report_json() << ",\n";
                output << "  \"perf_overlay\": " << state.perf_report_json() << ",\n";
                output << "  \"net_perf\": " << state.net_perf_json() << ",\n";
                if (state.live_session) output << "  \"overview_ui\": " << state.overview_report_json() << ",\n";
            },
        });
        if (live) state.load_report(presentation::ui::load_stage_interface, 0, 1);
        if (!state.build_hud(host, map.context_name)) return state.fail_ready(state.failure);
        if (live) state.load_report(presentation::ui::load_stage_interface, 1, 1);
        const bool ready = state.space->ready(host, map, *state.filesystem, placement_catalog, static_cast<bool>(state.catalog), catalog_failure);
        if (ready && state.live_session) {
            state.live_session->loading_complete();
            if (state.live_session->phase() == LiveSessionView::Phase::ready && state.battle_audio)
                state.battle_audio->loading_complete();
            // BLD-11: the bar is full and Begin shows; a driven run that began itself just clears it.
            state.load_progress.complete();
            if (state.loading) {
                if (state.live_session->phase() == LiveSessionView::Phase::ready) state.loading->show_ready();
                else state.loading->dismiss();
            }
            state.sync_battle_hud();
        } else if (!ready && state.loading) {
            state.loading->dismiss();
        }
        if (!ready && state.failure.empty()) {
            // The view's own cause (the report carries it too), so the caller can say why.
            state.failure = state.space->failure();
            if (!state.failure.empty()) godot::UtilityFunctions::printerr(godot::String::utf8(state.failure.c_str()));
        }
        // Keep initial-load sharing, then release models without active owners.
        if (ready && MemoryAttribution::enabled())
            MemoryAttribution::load_mark("pre_trim", scene::VfsAssetCache(*state.filesystem).model_stats().cache_retained_bytes);
        if (ready) scene::VfsAssetCache(*state.filesystem).set_model_cache_budget(128ULL * 1024 * 1024);
        if (ready && MemoryAttribution::enabled())
            MemoryAttribution::load_mark("post_trim", scene::VfsAssetCache(*state.filesystem).model_stats().cache_retained_bytes);
        startup_trace.ready();
        return ready;
    }

bool MapMode::State::ready_land_population(const assets::Map& map, std::vector<sim::RenderInstance>& instances,
    sim::AssetId& next_asset, sim::EntityId& next_entity) {
    State& state = *this;
    if (state.populate) {
        if (!state.compose_placements(map, instances, next_asset, next_entity)) {
            return state.fail_ready(state.failure);
        }
    }

    return true;
}

} // namespace eawr::presentation::godot_backend
