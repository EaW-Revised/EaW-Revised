#include "effect_mode_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace effect_mode_detail {

[[nodiscard]] std::string canonical(const std::string_view name) {
    std::string result;
    result.reserve(name.size());
    for (const char character : name) {
        const char folded = character >= 'A' && character <= 'Z'
            ? static_cast<char>(character + ('a' - 'A')) : character;
        result.push_back(folded == '\\' ? '/' : folded);
    }
    return result;
}

// Particle texture names are often authored as absolute tool-machine paths;
// only the file name identifies the shipped texture, and only the file name is
// ever reported.
[[nodiscard]] std::string_view file_name(const std::string_view authored) {
    const std::size_t slash = authored.find_last_of("/\\");
    return slash == std::string_view::npos ? authored : authored.substr(slash + 1);
}

} // namespace effect_mode_detail

namespace {

// Authored texture names carry the source-art suffix where the shipped asset
// often carries another, so a reference is probed as written and then by stem,
// the same rule the map mode applies.
[[nodiscard]] std::optional<std::string> probe_texture(const vfs::Vfs& filesystem, const std::string_view name) {
    const std::string folded = canonical(file_name(name));
    if (folded.empty()) return std::nullopt;
    constexpr std::string_view root = "data/art/textures/";
    const std::string base = std::string(root) + folded;
    if (filesystem.stat(base)) return base;
    std::string stem = folded;
    for (const std::string_view suffix : {std::string_view(".tga"), std::string_view(".dds")}) {
        if (stem.size() > suffix.size() && stem.ends_with(suffix)) {
            stem.resize(stem.size() - suffix.size());
            break;
        }
    }
    for (const std::string_view suffix : {std::string_view(".tga"), std::string_view(".dds")}) {
        const std::string candidate = std::string(root) + stem + std::string(suffix);
        if (filesystem.stat(candidate)) return candidate;
    }
    return std::nullopt;
}

[[nodiscard]] std::array<float, 3> to_render(const particles::Vec3 value) { return {value.x, value.z, -value.y}; }

// Dry-run backend: accepts exactly the emitters the Godot backend will accept
// on texture resolution, and draws nothing. It lets the mode fit its capture
// camera to the effect before the graphical run without a second random draw.
class ProbeBackend final : public particles::RenderBackend {
public:
    explicit ProbeBackend(GodotParticleBackend::TextureResolver resolver) : resolver_(std::move(resolver)) {}
    std::uint64_t create_emitter(const particles::EmitterRenderPlan& plan) override {
        if (resolver_(plan.texture) != nullptr) return next_++;
        cause_ = "colour texture '" + plan.texture + "' did not resolve";
        return 0U;
    }
    void update_emitter(std::uint64_t, const particles::VertexStream&) override {}
    void destroy_emitter(std::uint64_t) override {}
    std::string failure_cause() const override { return cause_; }
private:
    std::string cause_;
    GodotParticleBackend::TextureResolver resolver_;
    std::uint64_t next_{1};
};

} // namespace

const assets::Texture* EffectMode::State::resolve(const std::string_view name) {
    const std::string key = canonical(file_name(name));
    auto found = textures.find(key);
    if (found == textures.end()) {
        ResolvedTexture resolved;
        if (filesystem) {
            if (const auto path = probe_texture(*filesystem, name)) {
                resolved.logical_path = *path;
                if (auto bytes = filesystem->open(*path)) resolved.sha256 = hash_bytes(bytes.value());
                if (auto decoded = assets::load_texture(*filesystem, *path)) resolved.texture = std::move(decoded.value());
            }
        }
        found = textures.emplace(key, std::move(resolved)).first;
    }
    return found->second.texture ? &*found->second.texture : nullptr;
}

core::Result<EffectMode::State::HostFrames> EffectMode::State::host_frames(const float time) const {
    if (!player) return core::Result<HostFrames>::success({});
    auto pose = player->sample({time, animation::PlaybackMode::loop, 0.0F});
    if (!pose) return core::Result<HostFrames>::failure(pose.error());
    HostFrames frames;
    if (visibility_bone) frames.visible = pose.value().bones[*visibility_bone].visible;
    if (proxy_mesh) {
        const auto extracted = particles::proxy_mesh_frames(
            pose.value(), *proxy_mesh, frames.emitter, frames.mesh);
        if (!extracted) return core::Result<HostFrames>::failure(extracted.error());
        return core::Result<HostFrames>::success(frames);
    }
    auto attachment = player->attachment(pose.value(), attach_bone, animation::AttachmentSpace::model);
    if (!attachment) return core::Result<HostFrames>::failure(attachment.error());
    frames.emitter = particles::emitter_frame_from_render(attachment.value().column_major);
    return core::Result<HostFrames>::success(frames);
}

core::Result<void> EffectMode::State::apply_host_frames(
    particles::EffectRegistry& target, const particles::EffectHandle effect,
    const float time, const bool record) {
    if (!player) return core::Result<void>::success();
    const auto frames = host_frames(time);
    if (!frames) return core::Result<void>::failure(frames.error());
    auto emitter = target.set_frame(effect, frames.value().emitter);
    if (!emitter) return emitter;
    if (proxy_mesh) {
        auto mesh = target.set_mesh_frame(effect, frames.value().mesh);
        if (!mesh) return mesh;
    }
    if (record) record_origins(frames.value());
    return core::Result<void>::success();
}

void EffectMode::State::record_origins(const HostFrames& frames) {
    if (proxy_mesh) {
        proxy_origins.push_back(frames.emitter.origin);
        mesh_origins.push_back(frames.mesh.origin);
    } else {
        attachment_origins.push_back(frames.emitter.origin);
    }
}

core::Result<particles::EffectHandle> EffectMode::State::spawn(
    particles::EffectRegistry& target) const {
    if (proxy_mesh) return target.spawn(system, options.seed, options.capacity, proxy_mesh->binding);
    return target.spawn(system, options.seed, options.capacity);
}

core::Result<FrameRecord> EffectMode::State::step(
    particles::EffectRegistry& target, const particles::EffectHandle effect, Lifecycle& life,
    const std::uint32_t index, const particles::CameraFrame& view, const bool record) {
    const bool graphical = registry && &target == registry.get();
    FrameRecord frame;
    frame.frame = index + 1;
    frame.time = static_cast<float>(index) * options.delta_seconds;
    if (life.live) {
        auto attached = apply_host_frames(target, effect, frame.time, record);
        if (!attached) return core::Result<FrameRecord>::failure(attached.error());
    }
    if (life.live && detach_frame && *detach_frame == index) {
        const std::size_t rids = graphical && backend ? backend->live_rids() : 0U;
        auto detached = target.detach(effect);
        if (!detached) return core::Result<FrameRecord>::failure(detached.error());
        life.result = detached.value();
        if (detached.value() == particles::EffectDetachState::released) {
            life.live = false;
            life.release_index = index;
            life.released_by = "detach";
            if (graphical) live_rids_before_release = rids;
        }
    }
    if (life.live) {
        auto stats = target.advance(effect, index == 0 ? 0.0F : options.delta_seconds, view);
        if (!stats) return core::Result<FrameRecord>::failure(stats.error());
        frame.stats = std::move(stats.value());
        if (frame.stats.finished) {
            // A completed drain is released exactly once, by its owner.
            if (graphical && backend) live_rids_before_release = backend->live_rids();
            auto released = target.release(effect);
            if (!released) return core::Result<FrameRecord>::failure(released.error());
            life.live = false;
            life.release_index = index;
            life.released_by = "completion";
        }
    } else {
        frame.advanced = false;
        frame.stats.detached = true;
        frame.stats.finished = life.released_by == "completion";
    }
    frame.released = !life.live;
    frame.resources = target.live_backend_resources();
    return core::Result<FrameRecord>::success(std::move(frame));
}

particles::AttachmentLifecycle EffectMode::State::make_visibility_life(particles::EffectRegistry& target) const {
    return particles::AttachmentLifecycle(target,
        [this](particles::EffectRegistry& registry, const std::uint32_t seed) {
            if (proxy_mesh) return registry.spawn(system, seed, options.capacity, proxy_mesh->binding);
            return registry.spawn(system, seed, options.capacity);
        },
        options.seed, *visibility);
}

core::Result<FrameRecord> EffectMode::State::visibility_step(
    particles::EffectRegistry& target, particles::AttachmentLifecycle& life, const std::uint32_t index,
    const particles::CameraFrame& view, const bool record, VisibilityFrame& event) {
    FrameRecord frame;
    frame.frame = index + 1;
    frame.time = static_cast<float>(index) * options.delta_seconds;
    const auto frames = host_frames(frame.time);
    if (!frames) return core::Result<FrameRecord>::failure(frames.error());
    if (record) record_origins(frames.value());
    auto step = life.step(frames.value().visible, frames.value().emitter,
        proxy_mesh ? &frames.value().mesh : nullptr, index == 0 ? 0.0F : options.delta_seconds, view);
    if (!step) return core::Result<FrameRecord>::failure(step.error());
    const particles::AttachmentStep& result = step.value();
    event.visible = result.visible;
    event.spawned = result.spawned.has_value();
    event.detached = result.detached ? std::string(to_string(*result.detached)) : std::string{};
    event.drains_released = result.drains_released;
    event.drains_cut_short = result.drains_cut_short;
    event.live_instances = result.live_instances;
    event.active = result.active;
    frame.stats = std::move(step.value().stats);
    // A frame with no live instance advanced nothing and carries no stream.
    frame.advanced = !frame.stats.emitters.empty();
    frame.released = result.live_instances == 0;
    frame.resources = target.live_backend_resources();
    return core::Result<FrameRecord>::success(std::move(frame));
}

bool EffectMode::State::fit_camera() {
    // One fixed view direction: in front of the effect and a little above it.
    // Billboard orientation depends only on that direction, so the dry run
    // below produces the same streams the fitted camera will see.
    const std::array<float, 3> direction{0.0F, 0.33F, 0.944F};
    const auto frame_for = [&](const std::array<float, 3>& target, const float distance) {
        FixedCamera fitted;
        fitted.target = target;
        fitted.eye = {target[0] + direction[0] * distance, target[1] + direction[1] * distance,
                      target[2] + direction[2] * distance};
        fitted.up = {0.0F, 1.0F, 0.0F};
        return fitted;
    };
    FixedCamera provisional = frame_for({0.0F, 0.0F, 0.0F}, 100.0F);
    ProbeBackend probe([this](const std::string_view name) { return resolve(name); });
    particles::EffectRegistry dry(probe);
    const auto spawned = spawn(dry);
    if (!spawned) {
        failure = core::format_diagnostic(spawned.error());
        return false;
    }
    // The dry run's plans carry texture-resolution refusals, so a failure below
    // still reports every emitter's cause.
    plans = *dry.plans(spawned.value());
    // The visibility lifecycle spawns its own generations; this instance only
    // supplied the plans.
    std::optional<particles::AttachmentLifecycle> dry_visibility;
    if (visibility) {
        if (auto released = dry.release(spawned.value()); !released) {
            failure = core::format_diagnostic(released.error());
            return false;
        }
        dry_visibility.emplace(make_visibility_life(dry));
    }
    const particles::CameraFrame provisional_frame =
        particles::camera_frame_from_render(provisional.eye, provisional.target, provisional.up);
    particles::EffectFrameStats last;
    // Without a schedule the camera frames the final frame, exactly as before.
    // With one it frames every drawn frame, so the pre-detach, draining and
    // final captures all share one camera.
    particles::EffectFrameStats seen;
    Lifecycle dry_life;
    for (std::uint32_t index = 0; index < options.frames; ++index) {
        VisibilityFrame ignored;
        auto frame = dry_visibility
            ? visibility_step(dry, *dry_visibility, index, provisional_frame, false, ignored)
            : step(dry, spawned.value(), dry_life, index, provisional_frame, false);
        if (!frame) {
            failure = core::format_diagnostic(frame.error());
            return false;
        }
        last = std::move(frame.value().stats);
        if (!last.has_bounds) continue;
        if (!seen.has_bounds) {
            seen = last;
        } else {
            seen.bounds_min = {std::min(seen.bounds_min.x, last.bounds_min.x),
                std::min(seen.bounds_min.y, last.bounds_min.y), std::min(seen.bounds_min.z, last.bounds_min.z)};
            seen.bounds_max = {std::max(seen.bounds_max.x, last.bounds_max.x),
                std::max(seen.bounds_max.y, last.bounds_max.y), std::max(seen.bounds_max.z, last.bounds_max.z)};
        }
    }
    if (detach_frame || visibility) {
        if (!seen.has_bounds) {
            failure = detach_frame ? "the effect never drew a particle in any frame of the detach schedule"
                                   : "the effect never drew a particle while its host bone was visible";
            return false;
        }
        last = seen;
    }
    if (!last.has_bounds) {
        failure = "no drawn particle is live in the final frame";
        return false;
    }
    const std::array<float, 3> low = to_render(last.bounds_min);
    const std::array<float, 3> high = to_render(last.bounds_max);
    std::array<float, 3> centre{};
    float radius = 0.0F;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        centre[axis] = (low[axis] + high[axis]) * 0.5F;
        radius += (high[axis] - low[axis]) * (high[axis] - low[axis]) * 0.25F;
    }
    radius = std::max(std::sqrt(radius), 0.5F);
    const float half_fov = camera.vertical_fov_degrees * 0.5F * 3.14159265F / 180.0F;
    const float distance = radius / std::sin(half_fov) * 1.35F;
    const FixedCamera fitted = frame_for(centre, distance);
    camera.eye = fitted.eye;
    camera.target = fitted.target;
    camera.up = fitted.up;
    camera.near_plane = std::max(0.01F, distance * 0.01F);
    camera.far_plane = distance * 10.0F + radius * 4.0F;
    camera_frame = particles::camera_frame_from_render(camera.eye, camera.target, camera.up);

    // A second, independent CPU instance under the fitted camera: its per-frame
    // stream hashes must equal the graphical run's, frame for frame.
    ProbeBackend replay_probe([this](const std::string_view name) { return resolve(name); });
    particles::EffectRegistry replay(replay_probe);
    std::optional<particles::AttachmentLifecycle> replay_visibility_life;
    particles::EffectHandle replayed{};
    if (visibility) {
        replay_visibility_life.emplace(make_visibility_life(replay));
    } else {
        const auto spawned_replay = spawn(replay);
        if (!spawned_replay) {
            failure = core::format_diagnostic(spawned_replay.error());
            return false;
        }
        replayed = spawned_replay.value();
    }
    Lifecycle replay_life;
    std::vector<bool> drawn;
    for (std::uint32_t index = 0; index < options.frames; ++index) {
        VisibilityFrame event;
        const auto frame = replay_visibility_life
            ? visibility_step(replay, *replay_visibility_life, index, camera_frame, false, event)
            : step(replay, replayed, replay_life, index, camera_frame, false);
        if (!frame) {
            failure = core::format_diagnostic(frame.error());
            return false;
        }
        probe_hashes.push_back(frame.value().stats.hash);
        drawn.push_back(frame.value().stats.has_bounds);
        if (replay_visibility_life) replay_visibility.push_back(std::move(event));
    }
    if (replay_visibility_life) {
        if (auto released = replay_visibility_life->release_all(); !released) {
            failure = core::format_diagnostic(released.error());
            return false;
        }
        plan_visibility_captures(drawn);
        return true;
    }
    replay_result = replay_life.result;
    if (!detach_frame || !replay_result) return true;

    // Intermediate captures, chosen from the replay that the graphical run
    // must equal frame for frame: the last attached frame, then one frame of
    // the drain (midway to its last drawn frame) or the frame the immediate
    // release emptied. The final frame is always captured separately.
    const std::uint32_t detached_at = *detach_frame;
    const std::uint32_t final_index = options.frames - 1;
    if (detached_at > 0 && drawn[detached_at - 1]) capture_plan.emplace_back("pre-detach", detached_at - 1);
    if (*replay_result == particles::EffectDetachState::released) {
        if (detached_at < final_index) capture_plan.emplace_back("released", detached_at);
    } else {
        std::optional<std::uint32_t> last_drawn;
        for (std::uint32_t index = detached_at; index < options.frames; ++index) {
            if (drawn[index]) last_drawn = index;
        }
        if (last_drawn) {
            const std::uint32_t middle = detached_at + (*last_drawn - detached_at) / 2U;
            if (middle < final_index) capture_plan.emplace_back("draining", middle);
        }
    }
    return true;
}

void EffectMode::State::plan_visibility_captures(const std::vector<bool>& drawn) {
    // Intermediate captures from the replay the graphical run must equal: the
    // last drawn frame before the first hide, one frame of the hidden span
    // (midway through a drawn drain, or the frame an immediate release
    // emptied), and midway through the first reappeared span. The final frame
    // is always captured separately; an index is captured at most once.
    const std::uint32_t final_index = options.frames - 1;
    const auto first_after = [&](const std::uint32_t from, const auto& predicate) -> std::optional<std::uint32_t> {
        for (std::uint32_t index = from; index < options.frames; ++index) {
            if (predicate(replay_visibility[index])) return index;
        }
        return std::nullopt;
    };
    const auto add = [&](const std::string& label, const std::uint32_t index) {
        if (index >= final_index) return;
        for (const auto& entry : capture_plan) if (entry.second == index) return;
        capture_plan.emplace_back(label, index);
    };
    const auto hidden = first_after(0, [](const VisibilityFrame& entry) { return !entry.detached.empty(); });
    if (!hidden) return;
    if (*hidden > 0 && drawn[*hidden - 1]) add("pre-hide", *hidden - 1);
    const auto respawned = first_after(*hidden, [](const VisibilityFrame& entry) { return entry.spawned; });
    const std::uint32_t span_end = respawned ? *respawned : options.frames;
    if (replay_visibility[*hidden].detached == "released") {
        add("hidden-released", *hidden);
    } else {
        std::optional<std::uint32_t> last_drawn;
        for (std::uint32_t index = *hidden; index < span_end; ++index) if (drawn[index]) last_drawn = index;
        if (last_drawn) add("hidden-draining", *hidden + (*last_drawn - *hidden) / 2U);
    }
    if (!respawned) return;
    const auto next_hide = first_after(*respawned + 1, [](const VisibilityFrame& entry) { return !entry.detached.empty(); });
    const std::uint32_t visible_end = next_hide ? *next_hide - 1 : final_index;
    const std::uint32_t middle = *respawned + (visible_end - *respawned) / 2U;
    if (drawn[middle]) add("reappeared", middle);
}

bool EffectMode::State::empty_capture_allowed(const std::string_view label) const {
    // An empty frame is a lifecycle outcome only after a successful detach, or
    // while the visibility host has no live instance or is hidden.
    if (visibility) {
        return !visibility_frames.empty()
            && (!visibility_frames.back().visible || visibility_frames.back().live_instances == 0);
    }
    if (label == "final") return lifecycle.result.has_value();
    return label == "released";
}

bool EffectMode::State::verify_capture(const CaptureResult& capture, const FrameRecord& frame,
                                       CaptureEvidence& result, const bool allow_empty) const {
    result.width = capture.width;
    result.height = capture.height;
    if (const std::string problem = capture_size_problem(capture, camera); !problem.empty()) {
        result.failure = problem;
        return false;
    }
    PackedByteArray encoded;
    encoded.resize(static_cast<int64_t>(capture.png_bytes.size()));
    if (!capture.png_bytes.empty()) std::memcpy(encoded.ptrw(), capture.png_bytes.data(), capture.png_bytes.size());
    Ref<Image> image;
    image.instantiate();
    if (image->load_png_from_buffer(encoded) != OK || image->is_empty()) {
        result.failure = "capture PNG could not be decoded";
        return false;
    }
    const Color background = image->get_pixel(0, 0);
    const auto is_drawn = [&](const Color& pixel) {
        return std::abs(pixel.r - background.r) + std::abs(pixel.g - background.g)
            + std::abs(pixel.b - background.b) > 0.04F;
    };
    if (!frame.stats.has_bounds) {
        // Only a successful detach explains a frame with nothing drawn; the
        // decoded capture must then be empty everywhere, not merely unverified.
        if (!allow_empty) {
            result.failure = "no drawn particle is live in the captured frame";
            return false;
        }
        result.check = "empty_after_detach";
        std::uint64_t total{};
        for (int32_t y = 0; y < image->get_height(); y += 2) {
            for (int32_t x = 0; x < image->get_width(); x += 2) {
                ++total;
                if (is_drawn(image->get_pixel(x, y))) ++result.drawn_samples;
            }
        }
        result.outside_coverage = total == 0 ? 1.0F
            : static_cast<float>(result.drawn_samples) / static_cast<float>(total);
        if (total == 0 || result.drawn_samples != 0) {
            result.failure = "pixels were drawn although the detached effect has no drawn particle";
            return false;
        }
        return true;
    }
    // Project the eight corners of the frame's drawn bounds through the same
    // look-at camera the renderer was given.
    const auto normalize = [](std::array<float, 3> value) {
        const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
        return std::array<float, 3>{value[0] / length, value[1] / length, value[2] / length};
    };
    const auto cross = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::array<float, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    const auto dot = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const std::array<float, 3> forward = normalize({camera.target[0] - camera.eye[0],
        camera.target[1] - camera.eye[1], camera.target[2] - camera.eye[2]});
    const std::array<float, 3> right = normalize(cross(forward, camera.up));
    const std::array<float, 3> up = cross(right, forward);
    const float width = static_cast<float>(image->get_width());
    const float height = static_cast<float>(image->get_height());
    const float tangent = std::tan(camera.vertical_fov_degrees * 0.5F * 3.14159265F / 180.0F);
    const float aspect = width / height;
    std::array<float, 4>& projected = result.projected;
    projected = {width, height, 0.0F, 0.0F};
    const particles::Vec3 low = frame.stats.bounds_min;
    const particles::Vec3 high = frame.stats.bounds_max;
    for (int corner = 0; corner < 8; ++corner) {
        const particles::Vec3 point{(corner & 1) ? high.x : low.x, (corner & 2) ? high.y : low.y,
                                    (corner & 4) ? high.z : low.z};
        const std::array<float, 3> world = to_render(point);
        const std::array<float, 3> relative{world[0] - camera.eye[0], world[1] - camera.eye[1], world[2] - camera.eye[2]};
        const float depth = dot(relative, forward);
        if (depth <= camera.near_plane) {
            result.failure = "the effect's bounds cross the capture camera's near plane";
            return false;
        }
        const float x = dot(relative, right) / (depth * tangent * aspect);
        const float y = dot(relative, up) / (depth * tangent);
        const float pixel_x = (x * 0.5F + 0.5F) * width;
        const float pixel_y = (0.5F - y * 0.5F) * height;
        projected[0] = std::min(projected[0], pixel_x);
        projected[1] = std::min(projected[1], pixel_y);
        projected[2] = std::max(projected[2], pixel_x);
        projected[3] = std::max(projected[3], pixel_y);
    }
    const float inner_margin = 2.0F;
    const float outer_margin = 0.04F * width;
    std::uint64_t inside_total{};
    std::uint64_t outside_total{};
    std::uint64_t outside_drawn{};
    for (int32_t y = 0; y < image->get_height(); y += 2) {
        for (int32_t x = 0; x < image->get_width(); x += 2) {
            const bool drawn = is_drawn(image->get_pixel(x, y));
            if (drawn) ++result.drawn_samples;
            const float px = static_cast<float>(x);
            const float py = static_cast<float>(y);
            const bool inside = px >= projected[0] - inner_margin && px <= projected[2] + inner_margin
                && py >= projected[1] - inner_margin && py <= projected[3] + inner_margin;
            const bool outside = px < projected[0] - outer_margin || px > projected[2] + outer_margin
                || py < projected[1] - outer_margin || py > projected[3] + outer_margin;
            if (inside) {
                ++inside_total;
                if (drawn) ++result.inside_drawn;
            } else if (outside) {
                ++outside_total;
                if (drawn) ++outside_drawn;
            }
        }
    }
    if (inside_total == 0 || outside_total == 0) {
        result.failure = "capture geometry left an empty evidence population";
        return false;
    }
    result.inside_coverage = static_cast<float>(result.inside_drawn) / static_cast<float>(inside_total);
    result.outside_coverage = static_cast<float>(outside_drawn) / static_cast<float>(outside_total);
    // Heat only distorts what is already on screen; over a uniform clear colour
    // it cannot change a pixel, so a heat-only effect is checked for stream
    // determinism and capture identity, not for coverage.
    const bool any_visible = std::any_of(plans.begin(), plans.end(), [](const particles::EmitterRenderPlan& plan) {
        return plan.drawable && plan.blend != particles::Blend::heat_distortion;
    });
    if (!any_visible) {
        result.check = "not_applicable_heat_only";
        return result.outside_coverage <= 0.005F;
    }
    if (result.inside_drawn < 20 || result.inside_coverage < 0.01F) {
        result.failure = "no particle pixels were drawn inside the projected emitter bounds";
        return false;
    }
    if (result.outside_coverage > 0.005F) {
        result.failure = "pixels were drawn outside the projected emitter bounds";
        return false;
    }
    return true;
}

std::filesystem::path EffectMode::State::capture_path_for(const std::string_view label) const {
    if (options.capture_path.empty()) return {};
    std::filesystem::path path = options.capture_path;
    path.replace_filename(options.capture_path.stem().string() + "-" + std::string(label)
        + options.capture_path.extension().string());
    return path;
}

namespace effect_mode_detail {

[[nodiscard]] bool write_png(const std::filesystem::path& path, const CaptureResult& capture) {
    if (path.empty()) return false;
    std::error_code error;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(capture.png_bytes.data()),
        static_cast<std::streamsize>(capture.png_bytes.size()));
    return static_cast<bool>(output);
}

} // namespace effect_mode_detail

void EffectMode::State::capture_intermediate() {
    CaptureEvidence result;
    result.label = pending_capture;
    result.frame = records.back().frame;
    auto capture = renderer->capture(camera);
    if (!capture) {
        result.failure = core::format_diagnostic(capture.error());
    } else {
        result.sha256 = hash_bytes(capture.value().png_bytes);
        result.verified = verify_capture(capture.value(), records.back(), result,
            empty_capture_allowed(pending_capture));
        result.written = write_png(capture_path_for(pending_capture), capture.value());
    }
    detach_captures.push_back(std::move(result));
}

} // namespace eawr::presentation::godot_backend
