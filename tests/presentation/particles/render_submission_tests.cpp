#include "render_test_support.hpp"

namespace particle_render_contracts {

particles::AttachmentLifecycle::Spawn system_spawner(const particles::SystemDefinition& system,
                                                     const std::size_t capacity) {
    return [system, capacity](particles::EffectRegistry& registry, const std::uint32_t seed) {
        return registry.spawn(system, seed, capacity);
    };
}

// A host that travels +10 ALO X per second with the given per-frame visibility,
// sampled through the production Player on a fixed 1/30 s clock.
struct VisibilityHost final {
    eawr::assets::Model model;
    eawr::assets::Animation animation;
    std::optional<playback::Player> player;
};
VisibilityHost visibility_host(const std::vector<bool>& visible, const float fps) {
    VisibilityHost host;
    eawr::assets::Bone root; root.name = "root"; root.parent = -1;
    root.relative_transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    eawr::assets::Bone socket = root; socket.name = "socket"; socket.parent = 0;
    host.model.bones = {root, socket};
    host.animation.stored_frame_count = static_cast<std::uint32_t>(visible.size());
    host.animation.playable_frame_count = host.animation.stored_frame_count - 1U;
    host.animation.frames_per_second = fps;
    host.animation.duration_seconds = static_cast<float>(host.animation.playable_frame_count) / fps;
    eawr::assets::AnimationTrack track; track.bone_index = 1; track.bone_name = "socket";
    for (std::size_t frame = 0; frame < visible.size(); ++frame) {
        const float x = 10.0F * static_cast<float>(frame) / fps;
        track.samples.push_back({{x, 0, 0}, {1, 1, 1}, {0, 0, 0, 1}, visible[frame]});
    }
    host.animation.tracks.push_back(track);
    auto player = playback::Player::create(host.model, &host.animation);
    expect(bool(player), "visibility host binds");
    if (player) host.player.emplace(std::move(player.value()));
    return host;
}

struct HostSample final {
    bool visible{};
    particles::EmitterFrame frame;
};
HostSample sample_host(const VisibilityHost& host, const float time) {
    const auto pose = host.player->sample({time, playback::PlaybackMode::clamp, 0.0F});
    const auto attachment = host.player->attachment(pose.value(), "socket", playback::AttachmentSpace::model);
    return {pose.value().bones[1].visible, particles::emitter_frame_from_render(attachment.value().column_major)};
}

// A small drawable spray: particles are born at the host frame and live 0.5 s.
particles::SystemDefinition attached_spray(const bool leave_particles) {
    particles::SystemDefinition system;
    auto emitter = drawable_emitter();
    emitter.spawn_interval = 0.1F; emitter.particles_per_interval = 2; emitter.stop_time = 0.0F;
    emitter.lifetime = 0.5F;
    system.emitters.push_back(emitter);
    system.leave_particles = leave_particles;
    return system;
}

struct ScriptedRun final {
    std::vector<particles::AttachmentStep> steps;
    std::vector<std::uint64_t> hashes;
    std::uint32_t generations{}, detaches{}, drains_released{}, drains_cut_short{};
    std::size_t released_at_end{}, resources_after{}, diagnostics{};
};
ScriptedRun scripted(RecordingBackend& backend, const particles::SystemDefinition& system,
                     const VisibilityHost& host, const int frames, const particles::ReappearancePolicy policy,
                     const std::uint32_t seed = 20260923U, const std::size_t max_draining = 8) {
    ScriptedRun run;
    particles::EffectRegistry registry(backend);
    particles::AttachmentLifecycle life(registry, system_spawner(system, 256), seed, policy, max_draining);
    constexpr float dt = 1.0F / 30.0F;
    for (int index = 0; index < frames; ++index) {
        const HostSample sample = sample_host(host, static_cast<float>(index) * dt);
        auto step = life.step(sample.visible, sample.frame, nullptr, index == 0 ? 0.0F : dt, test_camera());
        expect(bool(step), "visibility step succeeds");
        if (!step) break;
        run.hashes.push_back(step.value().stats.hash);
        run.steps.push_back(std::move(step.value()));
    }
    run.generations = life.generations(); run.detaches = life.detaches();
    run.drains_released = life.drains_released(); run.drains_cut_short = life.drains_cut_short();
    const auto released = life.release_all();
    expect(bool(released), "release_all succeeds");
    run.released_at_end = released ? released.value() : 0;
    run.resources_after = registry.live_backend_resources();
    run.diagnostics = registry.diagnostics().size();
    return run;
}

std::vector<bool> pattern(const std::size_t frames, const std::vector<std::pair<std::size_t, std::size_t>>& hidden) {
    std::vector<bool> visible(frames, true);
    for (const auto& [from, to] : hidden) for (std::size_t frame = from; frame < to && frame < frames; ++frame) visible[frame] = false;
    return visible;
}

void test_attachment_generation_seed() {
    expect(particles::generation_seed(1234, 0) == 1234, "generation 0 keeps the owner's seed");
    std::vector<std::uint32_t> seeds;
    for (std::uint32_t generation = 0; generation < 64; ++generation) {
        const std::uint32_t seed = particles::generation_seed(1234, generation);
        expect(seed != 0, "generation seeds are nonzero");
        seeds.push_back(seed);
    }
    std::sort(seeds.begin(), seeds.end());
    expect(std::adjacent_find(seeds.begin(), seeds.end()) == seeds.end(), "generation seeds are distinct");
    expect(particles::to_string(particles::ReappearancePolicy::respawn) == "respawn" &&
           particles::to_string(particles::ReappearancePolicy::stay_detached) == "stay_detached",
           "policy names are stable report values");
}

// A host that never hides must not move a single bit of the unmanaged stream.
void test_attachment_always_visible_matches_unmanaged() {
    RecordingBackend managed_backend;
    particles::EffectRegistry registry(managed_backend);
    particles::AttachmentLifecycle life(registry, system_spawner(mixed_system(), 512), 1234,
                                        particles::ReappearancePolicy::respawn);
    std::vector<std::uint64_t> managed;
    bool single = true;
    for (int frame = 0; frame < 90; ++frame) {
        // A generation's first advance is zero-delta, like EffectMode's first frame.
        const auto step = life.step(true, {}, nullptr, 1.0F / 30.0F, test_camera());
        expect(bool(step), "always-visible step succeeds");
        managed.push_back(step.value().stats.hash);
        single = single && step.value().live_instances == 1 && step.value().active && !step.value().detached;
    }
    expect(single && life.generations() == 1 && life.detaches() == 0, "an always-visible host keeps one generation");
    RecordingBackend zero_backend;
    particles::EffectRegistry zero_registry(zero_backend);
    const auto handle = zero_registry.spawn(mixed_system(), 1234, 512);
    std::vector<std::uint64_t> zero_first;
    for (int frame = 0; frame < 90; ++frame)
        zero_first.push_back(zero_registry.advance(handle.value(), frame == 0 ? 0.0F : 1.0F / 30.0F, test_camera()).value().hash);
    expect(managed == zero_first && managed_backend.record == zero_backend.record,
           "an always-visible attachment reproduces the unmanaged stream exactly");
    expect(bool(life.release_all()) && registry.live_backend_resources() == 0, "the always-visible run releases cleanly");
}

void test_attachment_hide_drains_exactly_once() {
    // 10 fps clip: visible 0-5, hidden from frame 6 to the end (0.6 s on).
    const auto host = visibility_host(pattern(31, {{6, 31}}), 10.0F);
    if (!host.player) return;
    RecordingBackend backend;
    const auto result = scripted(backend, attached_spray(true), host, 90, particles::ReappearancePolicy::respawn);
    std::size_t detaches{}, spawns{}, first_hidden{};
    bool found_hidden = false, quiet = true;
    for (std::size_t index = 0; index < result.steps.size(); ++index) {
        const auto& step = result.steps[index];
        if (step.detached) ++detaches;
        if (step.spawned) ++spawns;
        if (!step.visible && !found_hidden) { found_hidden = true; first_hidden = index; }
        if (found_hidden && index > first_hidden) quiet = quiet && !step.detached && !step.spawned;
    }
    expect(found_hidden && result.steps[first_hidden].detached == particles::EffectDetachState::draining,
           "the first hidden sample detaches the active generation into a drain");
    expect(detaches == 1 && result.detaches == 1 && spawns == 1 && quiet,
           "repeated hidden samples neither detach again nor spawn");
    expect(result.steps[first_hidden - 1].stats.particles > 0 && result.steps[first_hidden].stats.particles > 0,
           "residual particles are still drawn after the hide");
    std::optional<std::size_t> released_at;
    for (std::size_t index = first_hidden; index < result.steps.size(); ++index) {
        if (result.steps[index].drains_released != 0) { released_at = index; break; }
        expect(result.steps[index].stats.advance.spawned == 0 || index == first_hidden,
               "a draining generation spawns no root particle");
    }
    expect(released_at && result.steps[*released_at].drains_released == 1 && result.drains_released == 1,
           "the finished drain is released exactly once by its owner");
    if (released_at) {
        for (std::size_t index = *released_at + 1; index < result.steps.size(); ++index) {
            const auto& step = result.steps[index];
            expect(step.live_instances == 0 && step.stats.particles == 0 && !step.stats.has_bounds &&
                   step.stats.emitters.empty() && step.stats.hash == 0,
                   "after the drain completes nothing is alive or drawn");
        }
    }
    expect(result.released_at_end == 0 && result.resources_after == 0 && backend.live.empty() &&
           backend.invalid_destroys == 0 && backend.destroyed == backend.created && result.diagnostics == 0,
           "drain completion leaves no resource and no diagnostic");
}

void test_attachment_hide_releases_without_leave_particles() {
    const auto host = visibility_host(pattern(31, {{6, 31}}), 10.0F);
    if (!host.player) return;
    RecordingBackend backend;
    const auto result = scripted(backend, attached_spray(false), host, 60, particles::ReappearancePolicy::respawn);
    std::size_t detaches{};
    for (std::size_t index = 0; index < result.steps.size(); ++index) {
        const auto& step = result.steps[index];
        if (!step.detached) continue;
        ++detaches;
        expect(*step.detached == particles::EffectDetachState::released && step.live_instances == 0 &&
               step.stats.particles == 0 && !step.stats.has_bounds,
               "leave-particles clear releases the hidden generation at once");
        expect(index > 0 && result.steps[index - 1].stats.particles > 0, "the effect drew before it was hidden");
    }
    expect(detaches == 1 && result.drains_released == 0 && result.generations == 1,
           "one release, no drain, no second generation while hidden");
    expect(result.resources_after == 0 && backend.live.empty() && backend.destroyed == backend.created &&
           backend.invalid_destroys == 0 && result.diagnostics == 0, "immediate release cleans up once");
}

void test_attachment_reappearance_policies() {
    // Visible 0-5, hidden 6-9, visible 10-15, hidden 16-30 at 10 fps.
    const auto visible = pattern(31, {{6, 10}, {16, 31}});
    const auto host = visibility_host(visible, 10.0F);
    if (!host.player) return;
    RecordingBackend respawn_backend;
    const auto respawn = scripted(respawn_backend, attached_spray(true), host, 90,
                                  particles::ReappearancePolicy::respawn);
    std::vector<std::size_t> spawned_at, detached_at;
    std::size_t concurrent{};
    for (std::size_t index = 0; index < respawn.steps.size(); ++index) {
        const auto& step = respawn.steps[index];
        if (step.spawned) spawned_at.push_back(index);
        if (step.detached) detached_at.push_back(index);
        concurrent = std::max(concurrent, step.live_instances);
        expect(step.active == step.visible, "exactly the visible samples have an active generation");
    }
    expect(spawned_at.size() == 2 && detached_at.size() == 2 && respawn.generations == 2 && respawn.detaches == 2,
           "each visible span starts one generation and each hide detaches it once");
    expect(spawned_at.size() == 2 && spawned_at[0] == 0 && respawn.steps[spawned_at[1]].visible &&
           !respawn.steps[spawned_at[1] - 1].visible, "the second generation starts on the reappearance sample");
    expect(spawned_at.size() == 2 && respawn.steps[spawned_at[1]].spawned > respawn.steps[spawned_at[0]].spawned,
           "a reappearance takes a new handle; handles are never reused");
    expect(concurrent == 2, "the new generation runs beside the previous generation's drain");
    expect(respawn.drains_released == 2 && respawn.resources_after == 0 && respawn_backend.live.empty() &&
           respawn_backend.invalid_destroys == 0 && respawn.diagnostics == 0,
           "both drains finish and release once; nothing leaks");

    RecordingBackend stay_backend;
    const auto stay = scripted(stay_backend, attached_spray(true), host, 90,
                               particles::ReappearancePolicy::stay_detached);
    std::size_t stay_spawns{}, stay_detaches{};
    for (const auto& step : stay.steps) {
        if (step.spawned) ++stay_spawns;
        if (step.detached) ++stay_detaches;
        expect(!step.active || step.visible, "stay_detached never has an active generation while hidden");
    }
    expect(stay_spawns == 1 && stay_detaches == 1 && stay.generations == 1,
           "stay_detached ignores the reappearance");
    expect(!stay.steps[spawned_at.size() == 2 ? spawned_at[1] : 0].active,
           "stay_detached keeps the host without an instance after it reappears");
    expect(std::equal(stay.hashes.begin(), stay.hashes.begin() + static_cast<std::ptrdiff_t>(spawned_at.back()),
                      respawn.hashes.begin()), "both policies agree until the reappearance");
    expect(stay.resources_after == 0 && stay_backend.live.empty() && stay.drains_released == 1,
           "stay_detached drains and releases its one generation");

    // reset (BP-48, the debug build's group reset): the first hide's drain (0.5 s particles, hidden 0.4 s) is
    // still alive when the host shows again, and is dropped as the new generation starts.
    RecordingBackend reset_backend;
    const auto reset = scripted(reset_backend, attached_spray(true), host, 90, particles::ReappearancePolicy::reset);
    std::vector<std::size_t> reset_spawned_at;
    std::size_t reset_concurrent{}, dropped{};
    for (std::size_t index = 0; index < reset.steps.size(); ++index) {
        const auto& step = reset.steps[index];
        if (step.spawned) reset_spawned_at.push_back(index);
        reset_concurrent = std::max(reset_concurrent, step.live_instances);
        dropped += step.drains_reset;
        expect(step.active == step.visible, "reset has an active generation exactly on the visible samples");
    }
    expect(reset_spawned_at == spawned_at && reset.generations == 2 && reset.detaches == 2,
           "reset starts its generations where respawn does");
    expect(spawned_at.size() == 2 && respawn.steps[spawned_at[1]].live_instances == 2
               && reset.steps[spawned_at[1]].drains_reset == 1 && reset.steps[spawned_at[1]].live_instances == 1,
           "on reappearance the old drain is dropped rather than kept beside the new generation");
    expect(dropped == 1 && reset_concurrent == 1 && reset.drains_released == 1,
           "only the last hide drains to completion");
    expect(std::equal(reset.hashes.begin(), reset.hashes.begin() + static_cast<std::ptrdiff_t>(spawned_at.back()),
                      respawn.hashes.begin()), "reset and respawn agree until the reappearance");
    expect(reset.resources_after == 0 && reset_backend.live.empty() && reset_backend.invalid_destroys == 0
               && reset.diagnostics == 0, "reset releases every instance once");
    expect(particles::to_string(particles::ReappearancePolicy::reset) == "reset", "the reset policy's report name");
}

void test_attachment_hidden_at_start_spawns_on_first_visible() {
    const auto host = visibility_host(pattern(31, {{0, 4}}), 10.0F);
    if (!host.player) return;
    RecordingBackend backend;
    const auto result = scripted(backend, attached_spray(true), host, 30, particles::ReappearancePolicy::stay_detached);
    std::optional<std::size_t> first;
    for (std::size_t index = 0; index < result.steps.size(); ++index) {
        if (result.steps[index].spawned && !first) first = index;
        if (!first) expect(result.steps[index].live_instances == 0 && result.steps[index].stats.hash == 0,
                           "a hidden host has no instance before its first visible sample");
    }
    expect(first && *first > 0 && result.steps[*first].visible && result.generations == 1 && result.detaches == 0,
           "the first visible sample starts generation 0 even under stay_detached");
    expect(first && result.steps[*first].stats.advance.spawned > 0, "the new generation's zero-delta frame emits");
    expect(result.released_at_end == 1 && result.resources_after == 0 && backend.live.empty(),
           "the owner releases the active generation at the end");
}

void test_attachment_moving_host() {
    // Hidden 0.5-1.0 s: the reappeared generation must be born where the host is now.
    const auto host = visibility_host(pattern(31, {{5, 10}}), 10.0F);
    if (!host.player) return;
    RecordingBackend backend;
    particles::EffectRegistry registry(backend);
    particles::AttachmentLifecycle life(registry, system_spawner(attached_spray(true), 256), 7,
                                        particles::ReappearancePolicy::respawn);
    constexpr float dt = 1.0F / 30.0F;
    bool checked_respawn = false, drain_follows = true;
    for (int index = 0; index < 60; ++index) {
        const float time = static_cast<float>(index) * dt;
        const HostSample sample = sample_host(host, time);
        const auto step = life.step(sample.visible, sample.frame, nullptr, index == 0 ? 0.0F : dt, test_camera());
        expect(bool(step), "moving host step succeeds");
        if (!step) return;
        if (step.value().spawned && index > 0) {
            // Only the new generation's zero-delta birth batch sits at the host;
            // the old drain died out during the hidden span (0.5 s lifetime).
            const float x = sample.frame.origin.x;
            expect(step.value().live_instances == 1 && step.value().stats.has_bounds &&
                   step.value().stats.bounds_min.x >= x - 1.01F && step.value().stats.bounds_max.x <= x + 1.01F,
                   "the reappeared generation is born at the host's current origin");
            expect(x > 9.0F, "the host moved while it was hidden");
            checked_respawn = true;
        }
        if (!life.draining().empty()) {
            // Draining instances keep receiving the host frame (reference
            // ParticleSystemInstance::Update reads the bone every update).
            drain_follows = drain_follows && step.value().live_instances >= 1;
        }
    }
    expect(checked_respawn && drain_follows, "respawn and drain both track the moving host");
    expect(bool(life.release_all()) && registry.live_backend_resources() == 0 && backend.live.empty(),
           "the moving host run releases everything");
}

void test_attachment_draining_bound() {
    // Toggle every 0.1 s: each hide adds a drain; a bound of one cuts the oldest short.
    std::vector<bool> visible(31, true);
    for (std::size_t frame = 0; frame < visible.size(); ++frame) visible[frame] = frame % 2 == 0;
    const auto host = visibility_host(visible, 10.0F);
    if (!host.player) return;
    RecordingBackend backend;
    auto spray = attached_spray(true);
    spray.emitters[0].lifetime = 5.0F;
    const auto result = scripted(backend, spray, host, 60, particles::ReappearancePolicy::respawn, 99, 1);
    bool bounded = true;
    for (const auto& step : result.steps) bounded = bounded && step.live_instances <= 2;
    expect(bounded && result.drains_cut_short > 0, "at most one drain and one active generation are alive");
    expect(result.drains_cut_short + result.drains_released <= result.detaches,
           "only detached generations are cut short or released as drains");
    expect(result.resources_after == 0 && backend.live.empty() && backend.invalid_destroys == 0 &&
           backend.destroyed == backend.created && result.diagnostics == 0, "cut drains leak nothing");
}

void test_attachment_determinism() {
    const auto host = visibility_host(pattern(31, {{6, 10}, {16, 31}}), 10.0F);
    if (!host.player) return;
    // Random velocity and lifetime, so every generation's seed shows in its stream.
    auto spray = attached_spray(true);
    spray.emitters[0].velocity.shape = particles::Shape::sphere;
    spray.emitters[0].velocity.radius_min = 1; spray.emitters[0].velocity.radius_max = 3;
    spray.emitters[0].lifetime_variation = 0.3F;
    RecordingBackend first_backend, second_backend, other_backend;
    const auto first = scripted(first_backend, spray, host, 90, particles::ReappearancePolicy::respawn);
    const auto second = scripted(second_backend, spray, host, 90, particles::ReappearancePolicy::respawn);
    const auto other = scripted(other_backend, spray, host, 90, particles::ReappearancePolicy::respawn, 5);
    expect(first.hashes == second.hashes && first_backend.record == second_backend.record,
           "a fixed seed and visibility clip reproduce every merged stream");
    expect(first.hashes != other.hashes, "the seed reaches every generation");
    bool events = first.steps.size() == second.steps.size();
    for (std::size_t index = 0; events && index < first.steps.size(); ++index) {
        events = first.steps[index].spawned == second.steps[index].spawned &&
                 first.steps[index].detached == second.steps[index].detached &&
                 first.steps[index].drains_released == second.steps[index].drains_released &&
                 first.steps[index].live_instances == second.steps[index].live_instances;
    }
    expect(events, "lifecycle events repeat frame for frame");
}

void test_attachment_merge_stats() {
    particles::EffectFrameStats a, b, total;
    a.particles = 2; a.hash = 11; a.emitters = {{2, 2, 5, true}}; a.has_bounds = true;
    a.bounds_min = {0, 0, 0}; a.bounds_max = {1, 1, 1}; a.advance.spawned = 2;
    b.particles = 3; b.hash = 13; b.emitters = {{3, 3, 7, true}}; b.has_bounds = true;
    b.bounds_min = {-1, 0, 0}; b.bounds_max = {1, 4, 1}; b.detached = true; b.finished = true;
    particles::merge_frame_stats(total, a, true);
    expect(total.hash == a.hash && total.particles == 2 && !total.finished, "one instance passes through unchanged");
    particles::merge_frame_stats(total, b, false);
    expect(total.particles == 5 && total.emitters[0].particles == 5 && total.emitters[0].quads == 5 &&
           total.advance.spawned == 2 && total.detached && !total.finished,
           "counts add and finished needs every instance");
    expect(close(total.bounds_min.x, -1) && close(total.bounds_max.y, 4) && total.hash == ((11ULL ^ 13ULL) * 0x100000001b3ULL),
           "bounds widen and the hash chains in order");
}

void test_heat_pixel_change_bound() {
    // The map pixel check counts a pixel as changed above 0.04 summed RGB.
    constexpr float threshold = 0.04F;
    constexpr float amount = 0.01F;
    const auto bound = [](const float alpha, const std::int32_t width = 1280, const std::int32_t height = 720) {
        return particles::heat_distortion_pixel_change_bound(alpha, amount, width, height);
    };
    expect(bound(0.0F) == 0.0F, "zero vertex alpha draws nothing");
    // The Alderaan speeder shimmer keys 5/255: at most 3 steps per channel
    // even in the 2558x1360 window the graphical tests get without --resolution.
    expect(bound(5.0F / 255.0F) < threshold && bound(5.0F / 255.0F, 2558, 1360) < threshold,
           "5/255 heat cannot reach the pixel threshold");
    // 8/255 moves the sample only 0.4 px, which the old one-pixel gate
    // excused, but a hard edge under it can change a pixel above threshold.
    expect(bound(8.0F / 255.0F) >= threshold && 8.0F / 255.0F * amount * 1280.0F < 1.0F,
           "8/255 heat is checked although it moves less than one pixel");
    expect(bound(0.07F) >= threshold, "the reviewed 0.07 case is checked");
    expect(bound(6.0F / 255.0F, 3840, 2160) >= threshold, "a larger viewport checks fainter heat");
    expect(bound(0.5F) > bound(0.1F) && close(bound(1.0F), 3.0F), "a full-alpha swap can change every channel fully");
}

} // namespace particle_render_contracts
