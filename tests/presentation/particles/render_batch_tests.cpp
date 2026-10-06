#include "render_test_support.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <thread>

// #638: EffectRegistry::advance_all and present_all step their instances as tasks on an executor
// and upload in order on the calling thread. Every result must equal one advance or present
// after another, whatever runs the tasks.
namespace particle_render_contracts {
namespace {

// Real threads claiming tasks from a shared counter, the calling thread included, as the viewer's
// pool does.
class ThreadExecutor final : public particles::StepExecutor {
public:
    explicit ThreadExecutor(const std::size_t threads) : threads_(threads) {}
    bool run(const std::size_t count, const std::function<void(std::size_t)>& task) const override {
        std::atomic<std::size_t> next{0};
        const auto work = [&] {
            for (std::size_t index = next.fetch_add(1); index < count; index = next.fetch_add(1)) task(index);
        };
        std::vector<std::thread> helpers;
        for (std::size_t helper = 1; helper < threads_; ++helper) helpers.emplace_back(work);
        work();
        for (std::thread& helper : helpers) helper.join();
        ++runs;
        return true;
    }
    mutable std::size_t runs{};

private:
    std::size_t threads_;
};

// Runs the tasks last to first on the calling thread: a task order no serial loop has.
class ReverseExecutor final : public particles::StepExecutor {
public:
    bool run(const std::size_t count, const std::function<void(std::size_t)>& task) const override {
        for (std::size_t index = count; index-- > 0;) task(index);
        return true;
    }
};

class FailingExecutor final : public particles::StepExecutor {
public:
    bool run(std::size_t, const std::function<void(std::size_t)>&) const override { return false; }
};

bool same_stats(const particles::EffectFrameStats& a, const particles::EffectFrameStats& b) {
    if (a.particles != b.particles || a.hash != b.hash || a.has_bounds != b.has_bounds || a.detached != b.detached ||
        a.finished != b.finished || a.emitters.size() != b.emitters.size() ||
        a.advance.spawned != b.advance.spawned || a.advance.killed != b.advance.killed ||
        a.advance.death_bursts != b.advance.death_bursts || a.advance.child_instances_started != b.advance.child_instances_started ||
        a.advance.child_instances_detached != b.advance.child_instances_detached ||
        a.advance.dropped_at_capacity != b.advance.dropped_at_capacity ||
        a.advance.instances_dropped_at_capacity != b.advance.instances_dropped_at_capacity) {
        return false;
    }
    if (a.has_bounds && (std::bit_cast<std::array<float, 3>>(a.bounds_min) != std::bit_cast<std::array<float, 3>>(b.bounds_min) ||
                         std::bit_cast<std::array<float, 3>>(a.bounds_max) != std::bit_cast<std::array<float, 3>>(b.bounds_max))) {
        return false;
    }
    for (std::size_t index = 0; index < a.emitters.size(); ++index) {
        const auto& left = a.emitters[index];
        const auto& right = b.emitters[index];
        if (left.particles != right.particles || left.quads != right.quads || left.hash != right.hash ||
            left.drawn != right.drawn || std::bit_cast<std::uint32_t>(left.maximum_alpha) != std::bit_cast<std::uint32_t>(right.maximum_alpha)) {
            return false;
        }
    }
    return true;
}

// One scripted battle of `count` effects over `frames` frames: effects with their own seeds,
// brightness and frames, some detached, some released, an advance and a present each frame.
// `batched` uses advance_all and present_all (on `executor`); otherwise one call per handle.
struct Script final {
    std::vector<particles::EffectFrameStats> stats;  // every advance's statistics, in handle order
    std::vector<std::uint64_t> uploads;              // every uploaded stream's hash, in upload order
    particles::RegistryWorkCounts work;
};
Script play(const bool batched, const particles::StepExecutor* executor, const bool hashes,
            const std::size_t count = 40, const int frames = 45, const bool detail_changes = false) {
    RecordingBackend backend;
    particles::EffectRegistry registry(backend);
    registry.set_executor(executor);
    registry.set_stream_hashes(hashes);
    std::vector<particles::EffectHandle> handles;
    for (std::size_t index = 0; index < count; ++index) {
        const auto handle = registry.spawn(mixed_system(), 900U + static_cast<std::uint32_t>(index), 256);
        expect(bool(handle), "batch fixture spawns");
        if (!handle) return {};
        const float x = static_cast<float>(index) * 3.0F;
        const particles::Basis3 identity{{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}};
        expect(bool(registry.set_frame(handle.value(), {{x, 0.0F, 0.0F}, identity})), "batch fixture frame");
        if (index % 4 == 1) expect(bool(registry.set_brightness(handle.value(), 0.5F)), "batch fixture brightness");
        handles.push_back(handle.value());
    }
    Script script;
    std::vector<particles::EffectFrameStats> batch;
    const auto camera = test_camera();
    for (int frame = 0; frame < frames; ++frame) {
        if (detail_changes) {
            constexpr std::array<float, 5> levels{0, 0.4F, 0.6F, 0.8F, 1};
            constexpr std::array<float, 8> local{0, 0.4999F, 0.5F, 0.5001F, 0.6999F, 0.7F, 0.7001F, 1};
            expect(bool(registry.set_detail({levels[static_cast<std::size_t>(frame) % levels.size()],
                local[static_cast<std::size_t>(frame) % local.size()]})), "script changes independent detail inputs");
        }
        if (frame == 15) {
            for (std::size_t index = 0; index < handles.size(); index += 5) {
                expect(bool(registry.stop_emission(handles[index])), "batch fixture stops emission");
            }
        }
        if (frame == 30) {
            // A released handle leaves both runs alike.
            expect(bool(registry.release(handles[3])), "batch fixture releases");
            handles.erase(handles.begin() + 3);
        }
        if (batched) {
            expect(bool(registry.advance_all(handles, 1.0F / 30.0F, camera, batch)), "advance_all succeeds");
            expect(batch.size() == handles.size(), "one statistics entry per handle");
            script.stats.insert(script.stats.end(), batch.begin(), batch.end());
            expect(bool(registry.present_all(handles, camera)), "present_all succeeds");
        } else {
            for (const auto handle : handles) {
                auto advanced = registry.advance(handle, 1.0F / 30.0F, camera);
                expect(bool(advanced), "advance succeeds");
                if (advanced) script.stats.push_back(std::move(advanced.value()));
            }
            for (const auto handle : handles) expect(bool(registry.present(handle, camera)), "present succeeds");
        }
    }
    script.uploads = backend.record;
    script.work = registry.work();
    return script;
}

bool same_script(const Script& a, const Script& b) {
    if (a.stats.size() != b.stats.size() || a.uploads != b.uploads) return false;
    for (std::size_t index = 0; index < a.stats.size(); ++index) {
        if (!same_stats(a.stats[index], b.stats[index])) return false;
    }
    return a.work.steps == b.work.steps && a.work.presents == b.work.presents &&
           a.work.streams_built == b.work.streams_built && a.work.streams_hashed == b.work.streams_hashed &&
           a.work.uploads == b.work.uploads;
}

struct AttachmentScript final {
    std::vector<particles::AttachmentStep> steps;
    std::vector<std::uint64_t> uploads;
    std::size_t created{}, destroyed{};
};

AttachmentScript play_attachments(const bool batched, const particles::StepExecutor* executor) {
    RecordingBackend backend;
    particles::EffectRegistry registry(backend);
    registry.set_executor(executor);
    std::vector<particles::AttachmentLifecycle> lives;
    constexpr std::size_t count = 24;
    lives.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const bool mesh = index % 5 == 0;
        auto system = mesh ? mesh_system(particles::MeshSpawnMode::every_vertex) : mixed_system();
        system.leave_particles = index % 4 != 0;
        for (auto& emitter : system.emitters) emitter.lifetime = 0.5F;
        const auto spawn = [system, mesh](particles::EffectRegistry& owner, const std::uint32_t seed) {
            if (mesh) return owner.spawn(system, seed, 256, test_mesh_binding());
            return owner.spawn(system, seed, 256);
        };
        lives.emplace_back(registry, spawn, 500U + static_cast<std::uint32_t>(index),
            static_cast<particles::ReappearancePolicy>(index % 3), index % 2 == 0 ? 1 : 8);
    }
    AttachmentScript script;
    const auto camera = test_camera();
    for (std::size_t frame = 0; frame < 90; ++frame) {
        std::vector<particles::PreparedAttachmentStep> prepared;
        std::vector<particles::EffectHandle> handles;
        std::vector<float> deltas;
        for (std::size_t index = 0; index < lives.size(); ++index) {
            const bool visible = frame < 45 && (frame + index) % 9 < 4;
            const particles::EmitterFrame pose{{static_cast<float>(frame), static_cast<float>(index), 0},
                {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
            const particles::MeshFrame mesh{pose.origin, pose.basis};
            const auto* mesh_frame = index % 5 == 0 ? &mesh : nullptr;
            if (batched) {
                auto sample = lives[index].prepare_step(visible, pose, mesh_frame, 1.0F / 30.0F);
                expect(bool(sample), "attachment batch prepares visibility and frames");
                if (!sample) return {};
                handles.insert(handles.end(), sample.value().handles.begin(), sample.value().handles.end());
                deltas.insert(deltas.end(), sample.value().deltas.begin(), sample.value().deltas.end());
                prepared.push_back(std::move(sample.value()));
            } else {
                auto step = lives[index].step(visible, pose, mesh_frame, 1.0F / 30.0F, camera);
                expect(bool(step), "serial attachment advances");
                if (!step) return {};
                script.steps.push_back(std::move(step.value()));
            }
        }
        if (batched) {
            std::vector<particles::EffectFrameStats> stats;
            const auto advanced = registry.advance_all(handles, deltas, camera, stats);
            expect(bool(advanced), "mixed zero/full delta attachment batch advances");
            if (!advanced) return {};
            std::size_t next = 0;
            for (std::size_t index = 0; index < lives.size(); ++index) {
                const auto size = prepared[index].handles.size();
                auto step = lives[index].complete_step(std::move(prepared[index]),
                    std::span<const particles::EffectFrameStats>(stats).subspan(next, size));
                expect(bool(step), "attachment batch completes in owner order");
                if (!step) return {};
                next += size;
                script.steps.push_back(std::move(step.value()));
            }
        }
    }
    for (auto& life : lives) expect(bool(life.release_all()), "batched attachment cleanup");
    expect(registry.live_effects() == 0 && registry.live_backend_resources() == 0 && backend.live.empty(),
        "batched attachments release every active and draining resource");
    expect(backend.wrong_thread == 0, "attachment batches create, upload and destroy only on the calling thread");
    expect(backend.invalid_updates == 0 && backend.invalid_destroys == 0, "attachment batch backend handles stay live");
    script.uploads = backend.record;
    script.created = backend.created;
    script.destroyed = backend.destroyed;
    return script;
}

bool same_attachments(const AttachmentScript& left, const AttachmentScript& right) {
    if (left.steps.size() != right.steps.size() || left.uploads != right.uploads ||
        left.created != right.created || left.destroyed != right.destroyed) return false;
    for (std::size_t index = 0; index < left.steps.size(); ++index) {
        const auto& a = left.steps[index];
        const auto& b = right.steps[index];
        if (a.visible != b.visible || a.spawned != b.spawned || a.detached != b.detached ||
            a.drains_released != b.drains_released || a.drains_cut_short != b.drains_cut_short ||
            a.drains_reset != b.drains_reset || a.live_instances != b.live_instances || a.active != b.active ||
            !same_stats(a.stats, b.stats)) return false;
    }
    return true;
}

} // namespace

void test_particle_detail_batch() {
    const auto serial = play(false, nullptr, true, 24, 45, true);
    ReverseExecutor reverse;
    expect(same_script(serial, play(true, &reverse, true, 24, 45, true)),
        "LOD statistics, uploads and deterministic work counts match reverse scheduling");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        ThreadExecutor executor(workers);
        expect(same_script(serial, play(true, &executor, true, 24, 45, true)),
            "LOD work and geometry match on 1/2/4/8 workers");
    }
}

void test_batch_matches_serial() {
    const Script serial = play(false, nullptr, true);
    expect(!serial.stats.empty() && serial.stats.front().hash != 0, "the serial reference hashes its frames");
    const Script inline_batch = play(true, nullptr, true);
    const ReverseExecutor reverse;
    const Script reversed = play(true, &reverse, true);
    const ThreadExecutor four(4);
    const Script threaded = play(true, &four, true);
    const ThreadExecutor eight(8);
    const Script wide = play(true, &eight, true);
    expect(same_script(serial, inline_batch), "a batch on the calling thread equals one advance after another");
    expect(same_script(serial, reversed), "a batch whose tasks run last to first equals the serial run");
    expect(same_script(serial, threaded), "a batch on 4 threads equals the serial run (statistics, hashes, uploads)");
    expect(same_script(serial, wide), "a batch on 8 threads equals the serial run");
    expect(four.runs > 0 && eight.runs > 0, "the thread executors ran the batches");
}

void test_attachment_batch_matches_serial() {
    const auto serial = play_attachments(false, nullptr);
    const ThreadExecutor one(1), four(4), eight(8);
    const ReverseExecutor reverse;
    expect(!serial.steps.empty() && serial.created > 24, "attachment batch fixture respawns generations");
    expect(same_attachments(serial, play_attachments(true, nullptr)), "attachment inline batch equals serial lifecycle");
    expect(same_attachments(serial, play_attachments(true, &one)), "attachment 1-thread batch equals serial lifecycle");
    expect(same_attachments(serial, play_attachments(true, &four)), "attachment 4-thread batch equals serial lifecycle");
    expect(same_attachments(serial, play_attachments(true, &eight)), "attachment 8-thread batch equals serial lifecycle");
    expect(same_attachments(serial, play_attachments(true, &reverse)), "attachment reverse tasks equal serial lifecycle");
    expect(one.runs > 0 && four.runs > 0 && eight.runs > 0, "attachment fixture uses every executor");
    bool fresh = false, drains = false, cut = false, reset = false, finished = false;
    for (const auto& step : serial.steps) {
        fresh = fresh || step.spawned.has_value();
        drains = drains || step.detached == particles::EffectDetachState::draining;
        cut = cut || step.drains_cut_short > 0;
        reset = reset || step.drains_reset > 0;
        finished = finished || step.drains_released > 0;
    }
    expect(fresh && drains && cut && reset && finished, "attachment batch fixture covers fresh, detach, cut, reset and finished drains");
}

void test_batch_hashes_on_request() {
    const ThreadExecutor four(4);
    const Script hashed = play(true, &four, true);
    const Script plain = play(true, &four, false);
    bool zero = !plain.stats.empty();
    bool rest_equal = hashed.stats.size() == plain.stats.size() && hashed.uploads == plain.uploads;
    for (std::size_t index = 0; index < plain.stats.size() && index < hashed.stats.size(); ++index) {
        particles::EffectFrameStats unhashed = hashed.stats[index];
        unhashed.hash = 0;
        for (auto& emitter : unhashed.emitters) emitter.hash = 0;
        zero = zero && plain.stats[index].hash == 0;
        for (const auto& emitter : plain.stats[index].emitters) zero = zero && emitter.hash == 0;
        rest_equal = rest_equal && same_stats(unhashed, plain.stats[index]);
    }
    expect(zero, "without stream hashes the frame and emitter hashes are zero");
    expect(rest_equal, "without stream hashes every other statistic and every upload is unchanged");
    expect(plain.work.streams_hashed == 0, "without stream hashes no stream is hashed");
    expect(hashed.work.streams_hashed == hashed.work.steps * 4, "with stream hashes each advanced emitter stream is hashed once");
}

// The budget: a frame of N effects steps each once, builds each emitter's stream once, uploads each
// drawn stream once and hashes nothing in the live battle, in at most 64 tasks per batch.
void test_batch_work_counts() {
    const ThreadExecutor four(4);
    const std::size_t count = 100;
    const int frames = 10;
    const Script script = play(true, &four, false, count, frames);
    // Frames 0-29 run all 100 effects (the fixture releases one at frame 30, past these 10).
    const std::uint64_t instance_frames = count * static_cast<std::uint64_t>(frames);
    expect(script.work.steps == instance_frames, "one step per effect per frame");
    expect(script.work.presents == instance_frames, "one present per effect per frame");
    expect(script.work.streams_built == instance_frames * 4 * 2, "each of the 4 emitters' streams built once per advance and present");
    expect(script.work.uploads == instance_frames * 3 * 2, "each of the 3 drawn streams uploaded once per advance and present");
    expect(script.work.streams_hashed == 0, "the live battle's frames hash no stream");
    expect(script.work.batches == static_cast<std::uint64_t>(frames) * 2, "one executor batch per advance_all and present_all");
    expect(script.work.tasks == script.work.batches * 64, "a batch of 100 effects runs as 64 tasks");
}

void test_batch_rejects_repeats_and_unknown() {
    RecordingBackend backend;
    particles::EffectRegistry registry(backend);
    const ThreadExecutor four(4);
    registry.set_executor(&four);
    const auto first = registry.spawn(mixed_system(), 11, 128);
    const auto second = registry.spawn(mixed_system(), 12, 128);
    expect(bool(first) && bool(second), "batch rejection fixture spawns");
    if (!first || !second) return;
    std::vector<particles::EffectFrameStats> stats;
    const std::vector<particles::EffectHandle> repeated{first.value(), second.value(), first.value()};
    const auto twice = registry.advance_all(repeated, 1.0F / 30.0F, test_camera(), stats);
    expect(!twice && twice.error().code == particles::diagnostic_codes::batch, "a handle given twice fails the batch");
    const std::vector<float> deltas{0.0F, 1.0F / 30.0F, 1.0F / 30.0F};
    const auto variable_twice = registry.advance_all(repeated, deltas, test_camera(), stats);
    expect(!variable_twice && variable_twice.error().code == particles::diagnostic_codes::batch,
        "a repeated handle fails the mixed delta batch before stepping");
    const auto mismatch = registry.advance_all(repeated, std::span<const float>(deltas).first(2), test_camera(), stats);
    expect(!mismatch && mismatch.error().code == particles::diagnostic_codes::batch,
        "a mismatched delta count fails before stepping");
    const std::vector<particles::EffectHandle> unknown{first.value(), particles::EffectHandle{999}};
    const auto missing = registry.present_all(unknown, test_camera());
    expect(!missing && missing.error().code == particles::diagnostic_codes::unknown_effect, "an unknown handle fails the batch");
    const auto variable_missing = registry.advance_all(unknown, std::span<const float>(deltas).first(2), test_camera(), stats);
    expect(!variable_missing && variable_missing.error().code == particles::diagnostic_codes::unknown_effect,
        "an unknown handle fails the mixed delta batch before stepping");
    expect(backend.record.empty() && registry.work().steps == 0 && registry.work().presents == 0,
           "a failed batch steps, builds and uploads nothing");
    // Nothing moved: the instances continue exactly as fresh ones.
    RecordingBackend fresh_backend;
    particles::EffectRegistry fresh(fresh_backend);
    const auto fresh_first = fresh.spawn(mixed_system(), 11, 128);
    const auto a = registry.advance(first.value(), 1.0F / 30.0F, test_camera());
    const auto b = fresh.advance(fresh_first.value(), 1.0F / 30.0F, test_camera());
    expect(a && b && a.value().hash == b.value().hash, "an effect in a failed batch was not advanced");
    const FailingExecutor failing;
    registry.set_executor(&failing);
    const std::vector<particles::EffectHandle> both{first.value(), second.value()};
    const auto refused = registry.advance_all(both, 1.0F / 30.0F, test_camera(), stats);
    expect(!refused && refused.error().code == particles::diagnostic_codes::batch, "an executor that cannot run fails the batch");
    const auto uploads = backend.record.size();
    const auto variable_refused = registry.advance_all(both, std::span<const float>(deltas).first(2), test_camera(), stats);
    expect(!variable_refused && variable_refused.error().code == particles::diagnostic_codes::batch &&
        backend.record.size() == uploads, "a refused mixed delta batch uploads nothing");
}

namespace {
particles::CullingFrame box_view(const float center = 0) {
    return {{{{1, 0, 0, center + 5}, {-1, 0, 0, 5 - center},
        {0, 1, 0, 5}, {0, -1, 0, 5}, {0, 0, 1, 5}, {0, 0, -1, 5}}}, true, true};
}
class CullingBackend final : public particles::RenderBackend {
public:
    RecordingBackend recording;
    particles::CullingFrame view{box_view()};
    std::map<std::uint64_t, bool> shown;
    std::uint64_t groups{}, prepared{}, visible{}, stepped{};
    std::uint64_t create_emitter(const particles::EmitterRenderPlan& plan) override { return recording.create_emitter(plan); }
    void update_emitter(const std::uint64_t id, const particles::VertexStream& stream) override { recording.update_emitter(id, stream); }
    void destroy_emitter(const std::uint64_t id) override { recording.destroy_emitter(id); }
    particles::CullingFrame culling_frame() const override { return view; }
    void set_emitter_visible(const std::uint64_t id, const bool value) override { shown[id] = value; }
    void record_group_work(const bool is_visible, const bool is_prepared, const bool is_stepped) override {
        ++groups; visible += is_visible ? 1U : 0U; prepared += is_prepared ? 1U : 0U; stepped += is_stepped ? 1U : 0U;
    }
};
particles::SystemDefinition culling_system() {
    particles::SystemDefinition system;
    auto emitter = drawable_emitter();
    emitter.lifetime = 0.5F;
    emitter.velocity = {};
    emitter.particles_per_interval = 4;
    emitter.spawn_interval = 0.05F;
    emitter.bursting = true;
    system.leave_particles = true;
    system.emitters.push_back(emitter);
    return system;
}
}

void test_offscreen_updates_and_bounds() {
    // PS-38: split calls accumulate to exactly the same admitted update, with
    // no stream work outside the frustum and no clock reset on re-entry.
    CullingBackend paced_backend, accumulated_backend;
    particles::EffectRegistry paced(paced_backend), accumulated(accumulated_backend);
    const auto a = paced.spawn(culling_system(), 17, 128).value();
    const auto b = accumulated.spawn(culling_system(), 17, 128).value();
    const particles::EmitterFrame away{{100, 0, 0}, {}};
    expect(bool(paced.set_frame(a, away)) && bool(accumulated.set_frame(b, away)), "offscreen frames bind");
    const auto camera = test_camera();
    expect(bool(paced.advance(a, std::numeric_limits<float>::min(), camera)) &&
        bool(accumulated.advance(b, std::numeric_limits<float>::min(), camera)), "first positive update initializes generations");
    const auto first = paced.advance(a, 0.05F, camera).value();
    expect(close(first.elapsed_seconds, 0) && close(first.deferred_seconds, 0.05F), "unrendered subthreshold update retains elapsed time");
    const auto equality = paced.advance(a, 0.05F, camera).value();
    const auto once = accumulated.advance(b, 0.1F, camera).value();
    expect(same_stats(equality, once) && close(equality.elapsed_seconds, 0.1F) && equality.deferred_seconds == 0,
        "0.1-second equality admits the complete accumulated update");
    expect(paced.work().steps == 2 && paced.work().updates_deferred == 1 && paced.work().streams_built == 0 && paced.work().uploads == 0,
        "offscreen work has two initialized/admitted updates and zero preparation/upload");
    paced_backend.view = accumulated_backend.view = box_view(100);
    expect(bool(paced.present(a, camera)) && bool(accumulated.present(b, camera)), "camera re-entry prepares retained ages");
    expect(paced_backend.recording.record == accumulated_backend.recording.record && paced.work().uploads == 1,
        "re-entry uploads exactly the accumulated particle state");
    // Successful submission keeps the next update at normal cadence.
    const auto rendered = paced.advance(a, 0.025F, camera).value();
    expect(close(rendered.elapsed_seconds, 0.125F) && rendered.deferred_seconds == 0, "rendered-last-update bypasses the throttle");
    paced_backend.view = box_view();
    expect(bool(paced.present(a, camera)) && !paced_backend.shown.begin()->second, "offscreen transition hides retained backend geometry");
    expect(bool(paced.stop_emission(a)), "hidden host stops new emission");
    const auto consumed = paced.advance(a, 0.025F, camera).value();
    expect(close(consumed.elapsed_seconds, 0.15F), "last successful draw latch survives a hidden present until consumed");
    expect(bool(paced.advance(a, 0.05F, camera)), "hidden live particles defer");
    const auto before_bad = paced.work().steps;
    expect(bool(paced.advance(a, std::numeric_limits<float>::infinity(), camera)) && paced.work().steps == before_bad,
        "invalid deltas do not poison the saved clock");
    const auto drain = paced.advance(a, 0.45F, camera).value();
    expect(drain.finished && drain.particles == 0 && close(drain.elapsed_seconds, 0.65F), "hidden live particles drain with all saved time");
    const auto empty_steps = paced.work().steps;
    expect(bool(paced.advance(a, 0.1F, camera)) && paced.work().steps == empty_steps, "hidden empty finished groups skip CPU work");

    // PS-39: moving the host outside view cannot hide its world-space trail.
    CullingBackend trail_backend;
    particles::EffectRegistry trail(trail_backend);
    const auto trail_handle = trail.spawn(culling_system(), 31, 128).value();
    expect(bool(trail.advance(trail_handle, 0.1F, camera)), "trail emits in view");
    expect(bool(trail.set_frame(trail_handle, away)) && bool(trail.present(trail_handle, camera)), "trail host moves away");
    expect(trail_backend.visible == 2 && trail_backend.prepared == 2, "particles outside host bounds still admit preparation");

    // Check all generated corners, including rotated billboards and both kite
    // families, against the independent conservative bounds.
    for (auto plan : particles::plan_system(mixed_system())) {
        if (!plan.drawable) continue;
        for (const bool legacy : {false, true}) {
            plan.legacy_kite_motion = legacy;
            particles::Particle particle;
            particle.emitter_index = 0;
            plan.emitter_index = 0;
            particle.position = {9, -6, 2}; particle.size = 3; particle.rotation = 0.37F;
            particle.velocity = {40, -20, 8}; particle.motion_velocity = particle.velocity;
            const std::array plans{plan}; const std::array live{particle};
            const auto bounds = particles::particle_bounds(plans, live);
            particles::VertexStream stream;
            particles::build_stream(plan, live, camera, stream);
            expect(bounds.valid && stream.quads == 1, "drawable bound fixture builds");
            for (const auto& vertex : stream.vertices) {
                const auto p = vertex.position;
                expect(p.x >= bounds.minimum.x && p.x <= bounds.maximum.x && p.y >= bounds.minimum.y && p.y <= bounds.maximum.y &&
                    p.z >= bounds.minimum.z && p.z <= bounds.maximum.z, "conservative particle bounds contain every rendered corner");
            }
        }
    }
    auto malformed = box_view(); malformed.planes[5].x = std::numeric_limits<float>::quiet_NaN();
    expect(particles::intersects({{100, 0, 0}, {101, 1, 1}, true}, malformed), "unknown frustum fails open even after a rejecting plane");
    expect(particles::intersects({{5, 0, 0}, {5, 1, 1}, true}, box_view()), "bounds on a frustum plane remain visible");

    // Serial, reversed and real worker schedules must agree on work counts,
    // complete statistics, uploads and re-entry ages under changing cameras.
    const auto play_culled = [&](const particles::StepExecutor* executor) {
        CullingBackend backend;
        particles::EffectRegistry registry(backend); registry.set_executor(executor);
        std::vector<particles::EffectHandle> handles;
        for (std::uint32_t index = 0; index < 20; ++index) {
            const auto handle = registry.spawn(culling_system(), index + 42, 128).value(); handles.push_back(handle);
            expect(bool(registry.set_frame(handle, {{static_cast<float>(index * 10), 0, 0}, {}})), "culled batch frame");
        }
        Script script; std::vector<particles::EffectFrameStats> stats;
        for (int frame = 0; frame < 24; ++frame) {
            backend.view = box_view(frame < 8 || frame >= 16 ? 0.0F : 100.0F);
            expect(bool(registry.advance_all(handles, 1.0F / 30.0F, camera, stats)) && bool(registry.present_all(handles, camera)), "culled batch runs");
            script.stats.insert(script.stats.end(), stats.begin(), stats.end());
        }
        script.uploads = backend.recording.record; script.work = registry.work(); return script;
    };
    const auto serial = play_culled(nullptr);
    const ThreadExecutor one(1), four(4), eight(8); const ReverseExecutor reverse;
    for (const particles::StepExecutor* executor : std::array<const particles::StepExecutor*, 4>{&one, &four, &eight, &reverse}) {
        const auto other = play_culled(executor);
        expect(serial.uploads == other.uploads && serial.stats.size() == other.stats.size(), "culled worker schedules retain ordered uploads");
        for (std::size_t index = 0; index < serial.stats.size(); ++index) {
            expect(same_stats(serial.stats[index], other.stats[index]) && serial.stats[index].elapsed_seconds == other.stats[index].elapsed_seconds &&
                serial.stats[index].deferred_seconds == other.stats[index].deferred_seconds, "culled worker schedules retain statistics and re-entry ages");
        }
        expect(serial.work.steps == other.work.steps && serial.work.streams_built == other.work.streams_built && serial.work.uploads == other.work.uploads &&
            serial.work.groups_prepared == other.work.groups_prepared && serial.work.updates_deferred == other.work.updates_deferred, "culled worker schedules retain deterministic counts");
    }
}

} // namespace particle_render_contracts
