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
        a.advance.death_bursts != b.advance.death_bursts || a.advance.child_instances_started != b.advance.child_instances_started) {
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
            const std::size_t count = 40, const int frames = 45) {
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

} // namespace

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
    const std::vector<particles::EffectHandle> unknown{first.value(), particles::EffectHandle{999}};
    const auto missing = registry.present_all(unknown, test_camera());
    expect(!missing && missing.error().code == particles::diagnostic_codes::unknown_effect, "an unknown handle fails the batch");
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
}

} // namespace particle_render_contracts
