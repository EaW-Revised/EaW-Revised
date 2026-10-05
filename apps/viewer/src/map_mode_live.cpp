#include "frame_timer.hpp"
#include "startup_trace.hpp"
#include "battle_content.hpp"
#include "map_mode.hpp"
#include "map_mode_internal.hpp"
#include "eawr/presentation/camera/overview.hpp"
#include "eawr/presentation/space/projectiles.hpp"
#include "render_profile_viewport.hpp"
#include "viewer_path.hpp"

#include <godot_cpp/classes/project_settings.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace eawr::presentation::godot_backend {
namespace {

[[nodiscard]] std::uint8_t camera_modifiers(const InputEventWithModifiers& event) {
    std::uint8_t result{};
    if (event.is_shift_pressed()) result |= viewer::camera_input::modifier::shift;
    if (event.is_ctrl_pressed()) result |= viewer::camera_input::modifier::ctrl;
    if (event.is_alt_pressed()) result |= viewer::camera_input::modifier::alt;
    if (event.is_meta_pressed()) result |= viewer::camera_input::modifier::meta;
    return result;
}

[[nodiscard]] std::optional<viewer::camera_input::RawEvent> map_camera_event(
    const Ref<InputEvent>& event) {
    namespace input = viewer::camera_input;
    input::RawEvent raw;
    if (const auto* key = Object::cast_to<InputEventKey>(event.ptr())) {
        const Key code = key->get_physical_keycode() != KEY_NONE
            ? key->get_physical_keycode() : key->get_keycode();
        raw.kind = input::RawKind::key;
        raw.code = input::key_code(std::string(OS::get_singleton()->get_keycode_string(code).utf8().get_data()))
            .value_or(0U);
        raw.pressed = key->is_pressed();
        raw.echo = key->is_echo();
        raw.modifiers = camera_modifiers(*key);
        return raw;
    }
    if (const auto* button = Object::cast_to<InputEventMouseButton>(event.ptr())) {
        raw.modifiers = camera_modifiers(*button);
        raw.pressed = button->is_pressed();
        raw.position_x = button->get_position().x;
        raw.position_y = button->get_position().y;
        raw.has_position = true;
        switch (button->get_button_index()) {
        case MOUSE_BUTTON_LEFT: raw.code = input::mouse_code::left; break;
        case MOUSE_BUTTON_RIGHT: raw.code = input::mouse_code::right; break;
        case MOUSE_BUTTON_MIDDLE: raw.code = input::mouse_code::middle; break;
        case MOUSE_BUTTON_WHEEL_UP:
            raw.kind = input::RawKind::mouse_wheel;
            raw.code = input::mouse_code::wheel_up;
            raw.factor = button->get_factor();
            return raw;
        case MOUSE_BUTTON_WHEEL_DOWN:
            raw.kind = input::RawKind::mouse_wheel;
            raw.code = input::mouse_code::wheel_down;
            raw.factor = button->get_factor();
            return raw;
        default: return std::nullopt;
        }
        raw.kind = input::RawKind::mouse_button;
        return raw;
    }
    if (const auto* motion = Object::cast_to<InputEventMouseMotion>(event.ptr())) {
        raw.kind = input::RawKind::mouse_motion;
        raw.modifiers = camera_modifiers(*motion);
        raw.relative_x = motion->get_relative().x;
        raw.relative_y = motion->get_relative().y;
        raw.position_x = motion->get_position().x;
        raw.position_y = motion->get_position().y;
        raw.has_position = true;
        return raw;
    }
    return std::nullopt;
}


} // namespace

void MapMode::close_requested() {
    if (state_->space && state_->live_session) state_->space->close();
}

std::optional<int> MapMode::process(const double delta) {
    State& state = *state_;
    if (state.perf_trace) state.perf_trace->begin_frame();
    state.hud_ms = 0.0;
    core::load_profile::Scope first_frame_scope(core::load_profile::Phase::session);
    state.sync_perf_overlay();
    if (state.space) {
        const std::optional<int> finished = state.space->process(delta);
        // #82: selection and orders see the frame the view just drew.
        // #848: the overview level the camera just took decides what the battle UI draws this frame.
        if (!finished) state.sync_overview_ui();
        if (!finished && state.battle && state.live_session && state.space_population) {
            FrameTimer timer(state.perf_trace ? &state.hud_ms : nullptr, true);
            state.battle->frame(*state.live_session, *state.space_population, *state.space);
            state.live_session->placement_preview(state.battle->placing(), state.battle->placement_point());
            state.battle->cursor_frame(*state.live_session, *state.space, delta, !state.options.capture_path.empty());
            state.sync_cards();
            state.sync_production();
            state.sync_minimap();
        }
        if (state.perf_trace && state.live_session) {
            PerfTrace::Frame frame;
            frame.frame_ms = delta * 1000.0;
            frame.presented_tick = static_cast<std::uint64_t>(std::max(0.0, state.live_session->presented_tick()));
            frame.units = state.live_session->visible_units().size();
            if (const auto snapshot = state.live_session->snapshot_at(frame.presented_tick)) {
                frame.projectiles = snapshot->projectiles().size();
            }
            if (state.battle_effects) {
                frame.effects = state.battle_effects->live_effects();
                frame.effect_particles = state.battle_effects->particles();
            }
            if (state.unit_emitters) frame.emitter_particles = state.unit_emitters->particles();
            frame.particle_ms = state.particle_ms;
            frame.emitter_frame_ms = state.emitter_frame_ms;
            frame.effects_frame_ms = state.effects_frame_ms;
            frame.debris_frame_ms = state.debris_frame_ms;
            frame.tick_wait_ms = state.live_session->tick_wait_ms();
            frame.live_frame_ms = state.live_session->live_frame_ms();
            frame.session_tail_ms = state.live_session->session_tail_ms();
            frame.clip_pose_ms = state.live_session->clip_pose_ms();
            if (state.unit_emitters) {
                frame.emitter_prepare_ms = state.unit_emitters->emitter_prepare_ms();
                frame.clone_prepare_ms = state.unit_emitters->clone_prepare_ms();
            }
            if (state.battle_effects) frame.projectile_prepare_ms = state.battle_effects->projectile_prepare_ms();
            frame.particle_work = GodotParticleBackend::frame_work();
            frame.submit_ms = state.space->live_submit_ms();
            frame.pieces = state.space->live_submit_pieces();
            frame.sent = state.space->live_submit_sent();
            frame.bookkeeping_ms = state.live_session->bookkeeping_ms();
            frame.hud_ms = state.hud_ms;
            frame.audio_ms = state.audio_ms;
            frame.fog_ms = state.fog_ms;
            frame.pose_ms = state.live_session->pose_ms();
            frame.opacity_ms = state.live_session->opacity_ms();
            frame.compose_ms = state.space_population->compose_ms();
            frame.refresh_ms = state.space_population->refresh_ms();
            frame.idle_frame_ms = state.space_population->idle_frame_ms();
            frame.idle_sample_ms = state.space_population->idle_sample_ms();
            frame.idle_upload_ms = state.space_population->idle_upload_ms();

            for (const platform::LiveTickCost& cost : state.live_session->tick_costs_after(state.perf_trace_tick)) {
                state.perf_trace_tick = cost.tick;
                ++frame.ticks;
                frame.tick_ms += cost.total_ms;
                for (const platform::LivePhaseCost& phase : cost.phases) {
                    if (phase.name == "ai") {
                        frame.ai_ms += phase.ms;
                        frame.ai_worst_ms = std::max(frame.ai_worst_ms, phase.ms);
                    } else if (phase.name == "lua") {
                        frame.lua_ms += phase.ms;
                        frame.lua_worst_ms = std::max(frame.lua_worst_ms, phase.ms);
                    }
                }
            }
            state.perf_trace->frame(frame);
        }
        // #447 --eawr-live-follow: the next frame looks at the unit where this one drew it.
        if (!finished && state.live_session && state.live_session->options().follow
            && !state.followed_projectile) {
            if (const auto unit = state.live_session->unit_frame(*state.live_session->options().follow)) {
                state.space->live_camera_focus(static_cast<float>(unit->position[0]),
                                               static_cast<float>(unit->position[1]));
            }
        }
        // #660 capture-only: BP-61 supplies the same interpolation the missile model uses.
        // Once selected, keep this ID after impact so a later shot cannot jump the camera.
        if (!finished && state.live_session && state.live_session->options().follow_projectile) {
            const auto& frame = state.live_session->battle_frame();
            if (frame.latest) {
                const auto projectiles = frame.latest->projectiles();
                if (!state.followed_projectile) {
                    const auto first = std::find_if(projectiles.begin(), projectiles.end(), [&](const auto& shot) {
                        return shot.homing && shot.shooter == *state.live_session->options().follow_projectile;
                    });
                    if (first != projectiles.end()) state.followed_projectile = first->id;
                }
                if (state.followed_projectile) {
                    const auto latest = std::find_if(projectiles.begin(), projectiles.end(), [&](const auto& shot) {
                        return shot.id == *state.followed_projectile;
                    });
                    if (latest != projectiles.end()) {
                        const sim::tactical::Projectile* previous = nullptr;
                        if (frame.previous) {
                            const auto before = frame.previous->projectiles();
                            const auto found = std::find_if(before.begin(), before.end(), [&](const auto& shot) {
                                return shot.id == latest->id;
                            });
                            if (found != before.end()) previous = &*found;
                        }
                        if (const auto pose = space::interpolate_projectile(previous, *latest, frame.alpha)) {
                            state.space->live_camera_focus(static_cast<float>(pose->position[0]),
                                                           static_cast<float>(pose->position[1]));
                        }
                    }
                }
            }
        }
        return finished;
    }
    if (state.completed || !state.renderer) return std::nullopt;
    if (state.map_camera && !state.failure.empty()) {
        state.completed = true;
        static_cast<void>(state.write_report());
        return 2;
    }
    if (state.map_camera && state.phases.empty() && !state.map_camera_settle_started) {
        if (!std::isfinite(delta) || delta < 0.0
            || delta > static_cast<double>(std::numeric_limits<float>::max())) {
            state.failure = "map camera process delta is invalid";
            state.completed = true;
            static_cast<void>(state.write_report());
            return 2;
        }
        const std::uint32_t tick = state.frame + 1U;
        const std::uint32_t terminal = state.options.warmup_frames + state.options.timed_frames;
        const bool free_terminal_test = state.map_free_terminal_hold_test
            || state.map_free_terminal_release_test;
        if (free_terminal_test && tick == terminal) {
            if (!state.map_camera->free_active()) {
                state.failure = "free terminal test did not enter free flight";
            } else if (!state.map_camera->adapter().is_held(
                viewer::camera_input::Action::free_move_forward)) {
                state.failure = "free terminal forward input did not reach adapter before ordinary step";
            } else {
                state.map_free_terminal_forward_held_at_step = true;
            }
        }
        const float step_seconds = free_terminal_test && tick == terminal
            ? 0.05F : static_cast<float>(delta);
        if (auto stepped = state.map_camera->step(step_seconds); !stepped) {
            state.failure = core::format_diagnostic(stepped.error());
            state.completed = true;
            static_cast<void>(state.write_report());
            return 2;
        }
        state.camera_selftest_tick();
        state.free_selftest_tick();
        if (state.map_camera_terminal_hold_test) {
            if (tick == terminal - 1U) {
                // Drive the bridge directly for the declared final pan step;
                // OS input delivery can otherwise slip past the capture tick.
                if (auto pressed = state.map_camera->handle(viewer::camera_input::RawEvent{
                        .kind = viewer::camera_input::RawKind::key,
                        .code = *viewer::camera_input::key_code("D"),
                        .pressed = true}); !pressed) {
                    state.failure = core::format_diagnostic(pressed.error());
                }
            }
            if (tick == terminal) {
                // Leave the key pressed through the final ordinary interactive frame.
                if (auto moved = state.map_camera->step(0.05F); !moved) {
                    state.failure = core::format_diagnostic(moved.error());
                }
            }
        }
        if (state.map_free_terminal_hold_test || state.map_free_terminal_release_test) {
            if (tick == 1U) inject_map_key(KEY_F, true);
            if (tick == 2U) inject_map_key(KEY_F, false);
            // Godot delivers injected events after this process callback. Press
            // before terminal so its ordinary step consumes the held action.
            if (tick == terminal - 1U) inject_map_key(KEY_W, true);
            if (tick == terminal && state.map_free_terminal_release_test) {
                inject_map_key(KEY_W, false);
            }
        }
        state.camera = render_camera(state.map_camera->frame());
        state.renderer->set_camera(state.camera);
    }
    if (!state.phases.empty()) {
        // Comparison phases: the same camera with one setting changed. The
        // renderer reads back the last drawn frame, so a few frames are drawn
        // before each capture is taken.
        State::Phase& phase = state.phases.front();
        if (!phase.started) {
            phase.started = true;
            state.comparison_frames = 0;
            if (phase.apply) phase.apply(state);
            if (state.particles) state.particles->follow_lighting(*state.renderer);
        }
        state.renderer->submit(phase.terrain_only ? state.terrain_snapshot : state.snapshot);
        if (state.fog && !state.renderer->fog_status().ready()) {
            state.completed = true;
            state.failure = "fog renderer rejected selected source during comparison capture";
            state.release_particles();
            static_cast<void>(state.write_report());
            return 2;
        }
        if (++state.comparison_frames < 4) return std::nullopt;
        auto capture = state.renderer->capture(state.camera);
        if (!capture) {
            state.completed = true;
            state.failure = core::format_diagnostic(capture.error());
            state.release_particles();
            static_cast<void>(state.write_report());
            return 2;
        }
        state.capture_hashes[phase.name] = hash_bytes(capture.value().png_bytes);
        if (!state.options.capture_path.empty()) {
            // Comparison captures sit next to the main capture, named by phase.
            std::filesystem::path path = state.options.capture_path;
            path.replace_extension(std::filesystem::path("." + phase.name + ".png"));
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (output) {
                output.write(reinterpret_cast<const char*>(capture.value().png_bytes.data()),
                    static_cast<std::streamsize>(capture.value().png_bytes.size()));
            }
        }
        state.captures[phase.name] = std::move(capture.value().png_bytes);
        state.phases.pop_front();
        if (!state.phases.empty()) return std::nullopt;
        return state.finish();
    }
    ++state.frame;
    // The frame's presentation tick: its frame count in a capture, real time
    // at 30 Hz in the live view. A capture holds after its particle frames.
    // Map particles and unit idle clips both run on it (#157, #196).
    const std::uint32_t tick = state.options.interactive
        ? live_idle_tick(state.idle_clock_seconds, delta)
        : std::min(state.frame - 1U, state.options.particle_frames - 1U);
    if (state.particles) state.particles->follow_lighting(*state.renderer);
    if (state.particles && state.particles->has_work()) {
        // Sample n is n/30 s. A capture takes one per frame; the live view
        // takes those its real-time tick is due, as the space view does (#186).
        const auto camera_frame = particles::camera_frame_from_render(
            state.camera.eye, state.camera.target, state.camera.up);
        for (std::uint32_t due = particles::map_owner_samples_due(state.particles->frames(), tick);
             due != 0; --due) {
            if (!state.particles->advance(particles::map_owner_delta(state.particles->frames()), camera_frame)) {
                state.completed = true;
                state.release_particles();
                state.failure = "map particle advance failed; identities and causes are in map_particles";
                static_cast<void>(state.write_report());
                return 2;
            }
        }
    }
    state.pose_units(tick);
    // Foliage bends on the scene clock (#147), which follows the same clock.
    state.advance_wind(delta);
    state.renderer->submit(state.snapshot);
    if (state.fog && !state.renderer->fog_status().ready()) {
        state.completed = true;
        const auto fog_status = state.renderer->fog_status();
        state.failure = fog_status.last_rejection
            ? core::format_diagnostic(*fog_status.last_rejection)
            : "fog renderer did not bind the selected source";
        state.release_particles();
        static_cast<void>(state.write_report());
        return 2;
    }
    if (!state.fog_paint_evidence.empty() && (state.frame == 4 || state.frame == 8 || state.frame == 12)) {
        const std::string label = state.frame == 4 ? "source"
            : (state.frame == 8 ? "painted" : "restored");
        auto evidence = state.renderer->capture(state.camera);
        if (!evidence) {
            state.completed = true;
            state.failure = core::format_diagnostic(evidence.error());
            state.release_particles();
            static_cast<void>(state.write_report());
            return 2;
        }
        std::filesystem::path path = state.fog_paint_evidence;
        path.replace_extension(std::filesystem::path("." + label + ".png"));
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(evidence.value().png_bytes.data()),
            static_cast<std::streamsize>(evidence.value().png_bytes.size()));
        if (!output) {
            state.completed = true;
            state.failure = "cannot write fog paint evidence " + ViewerPath::utf8(path);
            state.release_particles();
            static_cast<void>(state.write_report());
            return 2;
        }
        state.fog_paint_hashes[label] = hash_bytes(evidence.value().png_bytes);
        state.fog_paint_uploads[label] = state.renderer->fog_status().cache.uploads;
        state.fog_paint_updates[label] = state.renderer->fog_status().cache.updates;
        if (state.frame == 4) {
            const auto* source = state.fog->source().find(state.fog->team());
            const std::uint32_t x = source->desc().width / 2;
            const std::uint32_t y = source->desc().height / 2;
            const auto current = source->cell(x, y);
            static_cast<void>(state.fog->set_painting(true));
            static_cast<void>(state.fog->paint_cell(x, y, current && *current == 255 ? 0 : 255));
            state.refresh_fog_snapshots();
        } else if (state.frame == 8) {
            static_cast<void>(state.fog->set_painting(false));
            state.refresh_fog_snapshots();
        }
    }
    if (state.frame == state.options.warmup_frames) {
        state.timing_start = std::chrono::steady_clock::now();
        return std::nullopt;
    }
    if (state.frame < state.options.warmup_frames + state.options.timed_frames) {
        return std::nullopt;
    }
    state.timed_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - state.timing_start).count();

    if (state.map_camera && !state.map_camera->controller().capture_locked()) {
        const std::uint32_t terminal = state.options.warmup_frames + state.options.timed_frames;
        if (state.map_camera_selftest && state.frame == terminal) {
            auto before = state.renderer->capture(state.camera);
            if (!before) {
                state.failure = core::format_diagnostic(before.error());
                state.completed = true;
                static_cast<void>(state.write_report());
                return 2;
            }
            state.map_camera_terminal_before = std::move(before.value().png_bytes);
        }
        const std::uint32_t last_interactive = terminal + (state.map_camera_selftest ? 3U : 0U);
        if (state.frame < last_interactive) return std::nullopt;
        if (!state.map_camera_settle_started) {
            state.map_camera_settle_started = true;
            state.map_camera_steps_at_freeze = state.map_camera->steps();
            return std::nullopt;
        }
        ++state.map_camera_settle_frames;
        if (state.map_camera_settle_frames < 3U) return std::nullopt;
        if (state.map_camera->steps() != state.map_camera_steps_at_freeze
            || camera_frame(render_camera(state.map_camera->frame())) != camera_frame(state.camera)) {
            state.failure = "map camera changed during terminal settle";
            state.completed = true;
            static_cast<void>(state.write_report());
            return 2;
        }
    }

    auto capture = state.renderer->capture(state.camera);
    if (!capture) {
        state.completed = true;
        state.failure = core::format_diagnostic(capture.error());
        state.release_particles();
        static_cast<void>(state.write_report());
        return 2;
    }
    if (state.map_camera) {
        const Ref<Image> image = decode_png(capture.value().png_bytes);
        if (image.is_null() || image->get_width() != static_cast<int32_t>(state.camera.width)
            || image->get_height() != static_cast<int32_t>(state.camera.height)
            || capture.value().width != state.camera.width
            || capture.value().height != state.camera.height) {
            state.completed = true;
            state.failure = "map camera capture dimensions differ from camera identity";
            static_cast<void>(state.write_report());
            return 2;
        }
        if ((state.map_camera_selftest || state.map_free_selftest)
            && !state.options.capture_path.empty()) {
            state.map_camera_resize_active_at_capture =
                state.map_camera_host->get_window()->get_size() != state.map_camera_window_size;
            if (!state.map_camera_resize_active_at_capture) {
                state.completed = true;
                state.failure = "locked capture restored the host window before readback";
                static_cast<void>(state.write_report());
                return 2;
            }
            state.map_camera_host->get_window()->set_size(state.map_camera_window_size);
        }
    } else if (capture.value().width != state.camera.width
        || capture.value().height != state.camera.height) {
        state.completed = true;
        state.failure = "the map capture viewport " + std::to_string(capture.value().width) + "x"
            + std::to_string(capture.value().height) + " disagrees with the fixed camera viewport "
            + std::to_string(state.camera.width) + "x" + std::to_string(state.camera.height);
        state.release_particles();
        static_cast<void>(state.write_report());
        return 2;
    }
    state.capture_hash = hash_bytes(capture.value().png_bytes);
    state.capture_size = {capture.value().width, capture.value().height};
    state.evidence_verified = state.map_camera && state.options.capture_path.empty()
        ? state.verify_unlocked_capture(capture.value()) : state.verify_capture(capture.value());
    if (!state.options.capture_path.empty()) {
        std::error_code error;
        const std::filesystem::path parent = state.options.capture_path.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, error);
        std::ofstream output(state.options.capture_path, std::ios::binary | std::ios::trunc);
        if (output) {
            output.write(reinterpret_cast<const char*>(capture.value().png_bytes.data()),
                static_cast<std::streamsize>(capture.value().png_bytes.size()));
        }
    }
    if (!state.map_camera_unlocked_capture_path.empty()) {
        std::error_code error;
        const auto parent = state.map_camera_unlocked_capture_path.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, error);
        std::ofstream output(state.map_camera_unlocked_capture_path, std::ios::binary | std::ios::trunc);
        if (output) {
            output.write(reinterpret_cast<const char*>(capture.value().png_bytes.data()),
                static_cast<std::streamsize>(capture.value().png_bytes.size()));
        }
        if (error || !output) {
            state.failure = "cannot write unlocked map camera capture";
            state.completed = true;
            static_cast<void>(state.write_report());
            return 2;
        }
    }
    if (capture.value().png_bytes.empty()) {
        state.completed = true;
        state.failure = "capture is empty";
        state.release_particles();
        static_cast<void>(state.write_report());
        return 2;
    }
    state.capture_hashes["configured"] = state.capture_hash;
    state.captures["configured"] = capture.value().png_bytes;
    if (state.map_camera && state.options.capture_path.empty()) {
        state.release_particles();
        const bool checks = !state.map_camera_selftest ||
            (state.map_camera_checks.size() == map_camera_selftest_check_count && std::all_of(
                state.map_camera_checks.begin(), state.map_camera_checks.end(),
                [](const auto& entry) { return entry.second; }));
        const bool free_checks = !state.map_free_selftest ||
            (!state.map_free_checks.empty() && std::all_of(
                state.map_free_checks.begin(), state.map_free_checks.end(),
                [](const auto& entry) { return entry.second; }));
        state.completed = true;
        const bool particles_clean = !state.particles || (!state.particles->failed()
            && state.particle_rids_after_release == 0
            && state.particle_resources_after_release == 0);
        state.status = state.evidence_verified && checks && free_checks && particles_clean && state.failure.empty()
            ? ((state.map_camera_selftest || state.map_free_selftest)
                ? "map_camera_selftest_passed" : "map_camera_render_passed")
            : "failed";
        if (!checks && state.failure.empty()) state.failure = "map camera graphical selftest failed";
        if (!free_checks && state.failure.empty()) state.failure = "map free camera graphical selftest failed";
        if (!state.write_report()) return 2;
        return state.status == "failed" ? 2 : 0;
    }
    if (state.fog && state.particles) {
        state.particles->capture_fog_evidence(*state.renderer, *state.fog);
        const auto fog_status = state.renderer->fog_status();
        state.particle_fog_consumers_at_capture = fog_status.external_consumers;
        state.particle_fog_bound_at_capture = fog_status.ready();
    }
    if (state.particles) {
        state.particle_rids_at_capture = state.particles->live_rids();
        state.particle_resources_at_capture = state.particles->live_resources();
    }
    if (state.fog && state.populate && state.terrain_snapshot) {
        // A hidden unit cannot be established from the PNG alone. Verify the
        // configured frame actually submitted every snapshot instance and
        // that each populated asset draws with its attached fog material.
        std::map<sim::EntityId, sim::AssetId> expected;
        std::set<sim::EntityId> terrain_entities;
        for (const auto& instance : state.snapshot->instances()) {
            expected.emplace(instance.entity_id, instance.asset_id);
        }
        for (const auto& instance : state.terrain_snapshot->instances()) {
            terrain_entities.insert(instance.entity_id);
        }
        std::map<sim::EntityId, sim::AssetId> observed;
        for (const auto& entry : state.renderer->submission_evidence()) {
            observed.emplace(entry.entity_id, entry.asset_id);
        }
        const auto consumers = state.renderer->fog_consumers();
        bool bound_units = false;
        for (const auto& [entity, asset] : expected) {
            if (terrain_entities.contains(entity)) continue;
            const auto consumer = std::find_if(consumers.begin(), consumers.end(),
                [asset](const auto& value) { return value.asset_id == asset; });
            if (consumer == consumers.end() || !consumer->attached || consumer->surfaces == 0
                || consumer->surfaces_with_fog_material != consumer->surfaces) {
                bound_units = false;
                break;
            }
            bound_units = true;
        }
        state.fog_unit_submissions_verified = bound_units && observed == expected
            && state.renderer->instance_count() == expected.size();
    }

    const bool lit = state.policy != lighting::Policy::off;
    if (state.shadows) {
        state.phases.push_back({"shadows_off", [](State& self) { self.renderer->set_shadows_enabled(false); }, false, false});
    }
    if (lit && !state.fog) {
        state.phases.push_back({"other_policy", [](State& self) {
            GodotRenderer::LightingState other = self.other_lighting;
            other.shadows = false;
            self.renderer->set_lighting(other);
        }, false, false});
    }
    // With idle-clip owners the comparison is always captured, so a final
    // phase in which every owner is empty is verified to change no pixel.
    if (state.particles && (state.particles->has_live() || state.particles->has_owners())) {
        state.phases.push_back({"effects_off", [](State& self) {
            if (self.policy != lighting::Policy::off) self.renderer->set_lighting(self.configured_lighting);
            self.release_particles();
        }, false, false});
    }
    if (state.populate) {
        state.phases.push_back({"terrain_only", [lit](State& self) {
            if (lit) {
                GodotRenderer::LightingState configured = self.configured_lighting;
                configured.shadows = false;
                self.renderer->set_lighting(configured);
            }
        }, true, false});
    }
    if (!state.phases.empty() && state.scene_bloom_applied) {
        state.phases.push_front({"bloom_off", [](State& self) { self.renderer->set_scene_bloom(std::nullopt); },
                                 false, false});
    }
    if (!state.phases.empty()) return std::nullopt;
    return state.finish();
}

void MapMode::observe_pointer(const Ref<InputEvent>& event) {
    if (state_->battle) state_->battle->observe_pointer(event);
}

void MapMode::input(const Ref<InputEvent>& event) {
    State& state = *state_;
    if (state.live_session && state.live_session->phase() == LiveSessionView::Phase::ready) {
        // WBF-10: keyboard Begin remains available with the optional HUD disabled.
        if (const auto* key = Object::cast_to<InputEventKey>(event.ptr());
            key != nullptr && key->is_pressed() && !key->is_echo()
            && (key->get_keycode() == KEY_ENTER || key->get_keycode() == KEY_KP_ENTER || key->get_keycode() == KEY_SPACE)) {
            state.live_session->begin();
            state.sync_battle_hud();
        }
        return;
    }
    // #558: the performance overlay's key, in any run; nothing else sees it.
    if (const auto* pressed = Object::cast_to<InputEventKey>(event.ptr());
        pressed != nullptr && pressed->is_pressed() && !pressed->is_echo() && !pressed->is_shift_pressed()
        && !pressed->is_ctrl_pressed() && !pressed->is_alt_pressed() && !pressed->is_meta_pressed()
        && (pressed->get_physical_keycode() != KEY_NONE ? pressed->get_physical_keycode() : pressed->get_keycode())
            == KEY_F3) {
        state.set_perf_overlay(state.perf == nullptr || !state.perf->shown());
        return;
    }
    if (state.hud) state.hud->world_input(event);
    if (state.space) {
        // #82: the live battle's world layer takes selection and order input ahead of the camera.
        if (state.battle && state.live_session && state.space_population
            && state.battle->input(event, *state.live_session, *state.space_population, *state.space)) {
            state.sync_cards();
            return;
        }
        // The same Godot event translation as the land camera; the space
        // environment ignores it unless its opt-in camera is active.
        if (event.is_valid()) {
            if (const auto raw = map_camera_event(event)) state.space->camera_event(*raw);
        }
        return;
    }
    if (state.map_camera && event.is_valid()) {
        if (const auto raw = map_camera_event(event)) {
            if (auto handled = state.map_camera->handle(*raw); !handled) {
                state.failure = core::format_diagnostic(handled.error());
            }
        }
    }
    if (!state.fog || event.is_null() || !state.snapshot) return;
    const auto* key = Object::cast_to<InputEventKey>(event.ptr());
    if (!key || !key->is_pressed() || key->is_echo()) return;
    const Key code = key->get_physical_keycode() != KEY_NONE
        ? key->get_physical_keycode() : key->get_keycode();
    if (state.fog->fixed_capture()) {
        if (code == KEY_F || code == KEY_T || code == KEY_P) ++state.fog_ignored_inputs;
        return;
    }
    if (code == KEY_F) {
        static_cast<void>(state.fog->set_painting(!state.fog->painting()));
        state.refresh_fog_snapshots();
    } else if (code == KEY_T) {
        const auto grids = state.fog->source().grids();
        const auto current = std::find_if(grids.begin(), grids.end(), [&](const auto& grid) {
            return grid.team_id() == state.fog->team();
        });
        if (current != grids.end() && !grids.empty()) {
            const auto next = std::next(current) == grids.end() ? grids.begin() : std::next(current);
            if (state.fog->set_team(next->team_id())) {
                state.renderer->set_fog_team(next->team_id());
                state.refresh_fog_snapshots();
            }
        }
    } else if (code == KEY_P && state.fog->painting()) {
        // A deterministic grid-cell brush avoids inventing terrain picking.
        // Repeated presses alternate one selected source cell between dark and
        // fully visible; the source grid remains immutable.
        const auto* source = state.fog->source().find(state.fog->team());
        if (!source) return;
        const std::uint32_t x = source->desc().width / 2;
        const std::uint32_t y = source->desc().height / 2;
        const auto current = state.fog->effective().find(state.fog->team())->cell(x, y);
        const std::uint8_t next = current && *current == 255 ? 0 : 255;
        if (state.fog->paint_cell(x, y, next)) state.refresh_fog_snapshots();
    }
}

void MapMode::focus(const bool focused) {
    if (state_->battle && !focused) state_->battle->cancel();
    if (state_->space) {
        state_->space->camera_focus(focused);
        return;
    }
    if (!state_->map_camera) return;
    ++state_->map_camera_focus_notifications;
    state_->map_camera->set_focus(focused);
}

void MapMode::pointer_left() {
    if (state_->battle) state_->battle->cancel();
    if (state_->space) state_->space->camera_pointer_left();
    if (state_->map_camera) state_->map_camera->pointer_left();
}

void MapMode::viewport_changed(const float width, const float height) {
    if (state_->space) {
        state_->space->camera_viewport(width, height);
        return;
    }
    if (!state_->map_camera) return;
    ++state_->map_camera_resize_notifications;
    if (state_->map_camera_settle_started) return;
    if (auto resized = state_->map_camera->set_viewport(width, height); !resized) {
        state_->failure = core::format_diagnostic(resized.error());
    }
}

} // namespace eawr::presentation::godot_backend
