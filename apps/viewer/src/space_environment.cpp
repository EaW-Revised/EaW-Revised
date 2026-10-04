#include "space_environment.hpp"
#include "space_environment_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace {



[[nodiscard]] Ref<Image> decode_png(const std::vector<std::byte>& bytes) {
    PackedByteArray encoded;
    encoded.resize(static_cast<int64_t>(bytes.size()));
    if (!bytes.empty()) std::memcpy(encoded.ptrw(), bytes.data(), bytes.size());
    Ref<Image> image;
    image.instantiate();
    if (image->load_png_from_buffer(encoded) != OK || image->is_empty()) return {};
    return image;
}





} // namespace

namespace space_environment_detail {

[[nodiscard]] std::filesystem::path sibling(const std::filesystem::path& capture, const std::string& suffix) {
    std::filesystem::path path = capture;
    path.replace_extension(std::filesystem::path(suffix));
    return path;
}


[[nodiscard]] std::string hash_bytes(const std::span<const std::byte> bytes) {
    return sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

// The map mode's reference rule: probe the authored name as written, then by
// stem with every known suffix of the kind. The winner is the effective VFS
// record, whose source and layer are retained by the loaders.
[[nodiscard]] std::optional<std::string> probe_reference(
    const vfs::Vfs& filesystem, const std::string_view root,
    const std::string_view name, const std::span<const std::string_view> suffixes) {
    std::string canonical;
    canonical.reserve(name.size());
    for (const char character : name) {
        const char folded = character >= 'A' && character <= 'Z'
            ? static_cast<char>(character + ('a' - 'A')) : character;
        canonical.push_back(folded == '\\' ? '/' : folded);
    }
    if (canonical.empty()) return std::nullopt;
    const std::string base = std::string(root) + canonical;
    if (filesystem.stat(base)) return base;
    std::string stem = canonical;
    for (const std::string_view suffix : suffixes) {
        if (stem.size() > suffix.size() && stem.ends_with(suffix)) {
            stem.resize(stem.size() - suffix.size());
            break;
        }
    }
    for (const std::string_view suffix : suffixes) {
        const std::string candidate = std::string(root) + stem + std::string(suffix);
        if (filesystem.stat(candidate)) return candidate;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<space::Rgb8Image> rgb_of(const std::vector<std::byte>& png) {
    const Ref<Image> image = decode_png(png);
    if (image.is_null()) return std::nullopt;
    image->convert(Image::FORMAT_RGB8);
    const PackedByteArray data = image->get_data();
    space::Rgb8Image result;
    result.width = static_cast<std::uint32_t>(image->get_width());
    result.height = static_cast<std::uint32_t>(image->get_height());
    result.rgb.resize(static_cast<std::size_t>(data.size()));
    if (!result.rgb.empty()) std::memcpy(result.rgb.data(), data.ptr(), result.rgb.size());
    if (result.rgb.size() != static_cast<std::size_t>(result.width) * result.height * 3U) return std::nullopt;
    return result;
}

// Empty on success; otherwise why the requested artifact was not persisted.
[[nodiscard]] std::string write_file(const std::filesystem::path& path, const std::span<const std::byte> bytes) {
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return "cannot create its parent directory (" + error.message() + ")";
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return "cannot be opened for writing";
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) return "write or close failed";
    return {};
}

[[nodiscard]] tactical::TacticalFrame tactical_frame(const FixedCamera& source) {
    return {source.width, source.height, source.vertical_fov_degrees,
        source.near_plane, source.far_plane, source.eye, source.target, source.up};
}

[[nodiscard]] FixedCamera fixed_camera(const tactical::TacticalFrame& source) {
    FixedCamera result;
    result.width = source.width;
    result.height = source.height;
    result.vertical_fov_degrees = source.vertical_fov_degrees;
    result.near_plane = source.near_plane;
    result.far_plane = source.far_plane;
    result.eye = source.eye;
    result.target = source.target;
    result.up = source.up;
    return result;
}
} // namespace space_environment_detail

SpaceEnvironment::SpaceEnvironment(Options options) : state_(std::make_unique<State>(std::move(options))) {}
SpaceEnvironment::~SpaceEnvironment() = default;



void SpaceEnvironment::camera_event(const viewer::camera_input::RawEvent& event) {
    State& state = *state_;
    if (state.view) { state.view->camera_event(event); return; }
    if (!state.bridge) return;
    if (state.options.camera_terminal_baseline_test || state.options.camera_terminal_hold_test
        || state.options.camera_terminal_release_test) return;
    if (auto handled = state.bridge->handle(event); !handled) state.failure = core::format_diagnostic(handled.error());
}

void SpaceEnvironment::camera_focus(const bool focused) {
    State& state = *state_;
    if (state.view) { state.view->camera_focus(focused); return; }
    if (!state.bridge) return;
    if (state.options.camera_terminal_baseline_test || state.options.camera_terminal_hold_test
        || state.options.camera_terminal_release_test) return;
    ++state.focus_notifications;
    state.bridge->set_focus(focused);
}

void SpaceEnvironment::camera_pointer_left() {
    if (state_->view) { state_->view->camera_pointer_left(); return; }
    if (state_->options.camera_terminal_baseline_test || state_->options.camera_terminal_hold_test
        || state_->options.camera_terminal_release_test) return;
    if (state_->bridge) state_->bridge->pointer_left();
}

void SpaceEnvironment::camera_viewport(const float width, const float height) {
    State& state = *state_;
    if (state.view) { state.view->camera_viewport(width, height); return; }
    if (!state.bridge) return;
    if (state.options.camera_terminal_baseline_test || state.options.camera_terminal_hold_test
        || state.options.camera_terminal_release_test) return;
    ++state.resize_notifications;
    // Frozen from the terminal frame through readback.
    if (state.settle_started) return;
    if (auto resized = state.bridge->set_viewport(width, height); !resized) {
        state.failure = core::format_diagnostic(resized.error());
    }
}

std::optional<int> SpaceEnvironment::process(const double delta) {
    State& state = *state_;
    if (state.view) return state.view->process(delta);
    if (state.completed || !state.renderer) return std::nullopt;
    if (state.interactive()) return state.interactive_process(delta);
    if (state.frame > state.options.warmup_frames + state.options.timed_frames) {
        // Comparison phases: the same camera with only the submission changed.
        // The renderer reads back the last drawn frame, so frames are drawn
        // after each change before the capture.
        State::Phase& phase = state.phases.front();
        if (phase.fog_step != 0 && !phase.snapshot) phase.snapshot = state.fog_phase_snapshot(phase.fog_step);
        state.renderer->submit(phase.snapshot);
        if (!state.fog_bound()) return state.fail(state.fog_failure);
        if (++state.phase_frames < 4) return std::nullopt;
        state.phase_frames = 0;
        auto capture = state.renderer->capture(state.camera);
        if (!capture) {
            state.completed = true;
            state.give_up(core::format_diagnostic(capture.error()));
            return 2;
        }
        state.capture_hashes[phase.name] = hash_bytes(capture.value().png_bytes);
        if (!state.options.capture_path.empty()
            && !state.persist(sibling(state.options.capture_path, "." + phase.name + ".png"), capture.value().png_bytes)) {
            state.completed = true;
            state.give_up("requested artifact was not written: " + state.artifacts_failed.back());
            return 2;
        }
        if (phase.fog_step != 0) {
            const std::string label(State::fog_label(phase.fog_step));
            const auto status = state.renderer->fog_status();
            state.fog_phase_hashes[label] = state.capture_hashes[phase.name];
            state.fog_phase_uploads[label] = status.cache.uploads;
            state.fog_phase_updates[label] = status.cache.updates;
            state.fog_phase_revisions[label] = status.bound_revision.value_or(0);
            if (phase.fog_step == 1) {
                // Presence witnesses from the renderer itself: an all-dark
                // unit cannot show that it was drawn.
                state.fog_observed = state.renderer->submission_evidence();
                state.fog_consumers = state.renderer->fog_consumers();
            }
            if (!state.options.fog_paint_evidence.empty()) {
                std::filesystem::path path = state.options.fog_paint_evidence;
                path.replace_extension(std::filesystem::path("." + label + ".png"));
                if (!state.persist(path, capture.value().png_bytes)) {
                    return state.fail("requested artifact was not written: " + state.artifacts_failed.back());
                }
            }
        }
        if (phase.isolated) state.isolated_captures[*phase.isolated] = capture.value().png_bytes;
        else state.captures[phase.name] = std::move(capture.value().png_bytes);
        state.phases.pop_front();
        if (!state.phases.empty()) return std::nullopt;
        return state.finish();
    }
    if (state.control == "reload-cycle" && state.lifecycle_released.size() < lifecycle_cycles) {
        return state.lifecycle_step();
    }
    if (state.bridge) {
        // Locked to the fixed camera: the step is inert and hostile self-test
        // input is ignored; the fixed camera below is never rewritten.
        if (auto stepped = state.bridge->step(static_cast<float>(delta)); !stepped) {
            return state.fail(core::format_diagnostic(stepped.error()));
        }
        state.selftest_tick(state.frame + 1U);
        if (!state.failure.empty()) return state.fail(state.failure);
    }
    ++state.frame;
    state.renderer->submit(state.configured_snapshot);
    if (!state.fog_bound()) return state.fail(state.fog_failure);
    if (state.frame == 1) state.record_submissions();
    if (state.frame == state.options.warmup_frames) {
        state.timing_start = std::chrono::steady_clock::now();
        return std::nullopt;
    }
    if (state.frame < state.options.warmup_frames + state.options.timed_frames) return std::nullopt;
    state.timed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.timing_start).count();
    auto capture = state.renderer->capture(state.camera);
    if (!capture || capture.value().png_bytes.empty()) {
        state.completed = true;
        state.give_up(capture ? std::string("capture is empty") : core::format_diagnostic(capture.error()));
        return 2;
    }
    if (capture.value().width != state.camera.width || capture.value().height != state.camera.height) {
        state.completed = true;
        state.give_up("the capture viewport " + std::to_string(capture.value().width) + "x"
            + std::to_string(capture.value().height) + " disagrees with the fixed camera viewport "
            + std::to_string(state.camera.width) + "x" + std::to_string(state.camera.height));
        return 2;
    }
    if (state.bridge && state.options.camera_selftest && state.host) {
        // The locked probe's host resize must still be in effect at readback.
        state.resize_active_at_capture = state.host->get_window()->get_size() != state.window_size;
        if (!state.resize_active_at_capture) {
            return state.fail("locked capture restored the host window before readback");
        }
        state.host->get_window()->set_size(state.window_size);
    }
    state.capture_hashes["configured"] = hash_bytes(capture.value().png_bytes);
    state.capture_size = {capture.value().width, capture.value().height};
    if (!state.options.capture_path.empty() && !state.persist(state.options.capture_path, capture.value().png_bytes)) {
        state.completed = true;
        state.give_up("requested artifact was not written: " + state.artifacts_failed.back());
        return 2;
    }
    state.captures["configured"] = std::move(capture.value().png_bytes);
    ++state.frame;
    return std::nullopt;
}
} // namespace eawr::presentation::godot_backend
