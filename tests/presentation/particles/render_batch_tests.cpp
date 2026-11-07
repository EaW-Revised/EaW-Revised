#include "render_test_support.hpp"
#include "eawr/presentation/particles/catchup_screen_space.hpp"
#include "eawr/presentation/particles/lane_guard.hpp"
#include "particle_adapter.hpp"
#include "particle_latch.hpp"
#include "particle_reuse.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>

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
    [[nodiscard]] std::size_t participants() const noexcept override { return threads_; }
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
            const std::size_t count = 40, const int frames = 45, const bool detail_changes = false, const bool measure = false) {
    RecordingBackend backend;
    particles::EffectRegistry registry(backend);
    registry.set_executor(executor);
    registry.set_stream_hashes(hashes);
    registry.begin_measurement(measure);
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
        if (index % 5 == 1) {
            system.emitters.resize(2);
            system.emitters[0].lifetime = 0.1F;
            system.emitters[1].creator_id = 40;
            system.emitters[1].parent_emitter = 0;
            system.emitters[1].bursting = true;
        }
        system.leave_particles = index % 4 != 0;
        for (std::size_t emitter = 0; emitter < system.emitters.size(); ++emitter)
            if (index % 5 != 1 || emitter != 0) system.emitters[emitter].lifetime = 0.5F;
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

// L1.3: the registry script of `test_batch_matches_serial`, played by `run_on_lane` (the lane thread: the registry and its
// backend are built there, so that thread is the backend's caller, as the viewer's lane is) on `executor`, equals the
// serial script played on this thread.
bool lane_twin_matches(const std::function<void(const std::function<void()>&)>& run_on_lane,
                       const particles::StepExecutor* executor) {
    particles::EffectRegistry::set_stream_bucketing_default(false);
    const Script serial = play(false, nullptr, true);
    particles::EffectRegistry::set_stream_bucketing_default(true);
    Script on_lane;
    run_on_lane([&] { on_lane = play(true, executor, true); });
    return !serial.stats.empty() && serial.stats.front().hash != 0 && same_script(serial, on_lane);
}

void test_particle_detail_batch() {
    const auto serial = play(false, nullptr, true, 24, 45, true);
    ReverseExecutor reverse;
    expect(same_script(serial, play(true, &reverse, true, 24, 45, true)),
        "LOD statistics, uploads and deterministic work counts match reverse scheduling");
    for (const std::size_t workers : {1U, 2U, 4U, 6U, 8U}) {
        ThreadExecutor executor(workers);
        expect(same_script(serial, play(true, &executor, true, 24, 45, true)),
            "LOD work and geometry match on 1/2/4/6/8 workers");
    }
}

// The default particle pool size by host: hosts under eight hardware threads keep the earlier
// participant count (the main thread alone from 1 to 7 threads), eight and up take half, capped at 4 below twelve and at 6 from twelve.
void test_catchup_defer_predicate() {
    struct Row { std::uint64_t due, samples; bool defers; };
    const bool saved = particles::EffectRegistry::catchup_folding_default();
    particles::EffectRegistry::set_catchup_folding_default(true);
    for (const Row row : {Row{0, 0, false}, Row{1, 0, false}, Row{2, 0, true}, Row{2, 1, false}, Row{3, 1, true},
                          Row{5, 4, false}, Row{5, 0, true}, Row{3, 3, false}, Row{2, 5, false}}) {
        expect(particles::EffectRegistry::defers_catchup_uploads(row.due, row.samples) == row.defers,
            "a frame defers its uploads exactly when two or more samples are due");
    }
    particles::EffectRegistry::set_catchup_folding_default(false);
    for (const Row row : {Row{1, 0, false}, Row{2, 0, false}, Row{3, 1, false}, Row{5, 0, false}}) {
        expect(!particles::EffectRegistry::defers_catchup_uploads(row.due, row.samples),
            "the per-sample reference path never defers uploads");
    }
    particles::EffectRegistry::set_catchup_folding_default(saved);
}

// L1.2: the lane's phase-exclusion guards count a main-thread entry while a lane job owns the state and never abort.
void test_lane_guard_counts_and_never_aborts() {
    namespace lane = eawr::presentation::particles::lane;
    static_cast<void>(lane::take_violations());
    expect(!lane::window_open() && !lane::guard_main() && lane::violations() == 0,
        "no window is open outside a lane job: a main-thread entry is not a violation");
    {
        const lane::WindowScope window;
        expect(lane::window_open(), "the window is open between the kick and the join");
        expect(lane::guard_main() && lane::guard_main() && lane::violations() == 2,
            "a main-thread entry inside the window counts and the process goes on");
        // The lane thread's own calls into the owners are the owners', not violations.
        bool lane_counted = true;
        std::thread lane_thread([&] {
            const lane::LaneThreadScope on_lane;
            lane_counted = lane::guard_main();
        });
        lane_thread.join();
        expect(!lane_counted && lane::violations() == 2, "the lane thread's calls inside the window are not violations");
        // Any other thread (a helper that is not the lane) is counted like main.
        bool other_counted = false;
        std::thread other([&] { other_counted = lane::guard_main(); });
        other.join();
        expect(other_counted && lane::violations() == 3, "a non-lane thread inside the window counts");
    }
    expect(!lane::window_open() && !lane::guard_main() && lane::violations() == 3,
        "closing the window (also by scope exit on a failed frame) stops the counting");
    expect(lane::take_violations() == 3 && lane::violations() == 0, "take_violations returns the count and starts over");
    // The window is re-openable and close() is idempotent.
    lane::WindowScope again;
    again.close();
    again.close();
    expect(!lane::window_open() && !lane::guard_main(), "close twice leaves the window closed");
}

void test_default_worker_count_table() {
    struct Row { std::size_t threads, workers; };
    for (const Row row : {Row{0, 1}, Row{1, 1}, Row{2, 1}, Row{4, 1}, Row{6, 1}, Row{7, 1}, Row{8, 4},
                          Row{10, 4}, Row{11, 4}, Row{12, 6}, Row{16, 6}, Row{19, 6}, Row{20, 8}, Row{28, 8}, Row{64, 8}}) {
        expect(particles::default_particle_worker_count(row.threads) == row.workers,
            "the default particle worker count follows the hardware thread table");
    }
}

void test_deferred_frame_uploads() {
    for (const std::size_t workers : {1U, 2U, 4U, 6U, 8U}) {
        RecordingBackend backend, reference_backend;
        // Written for a present that always builds and uploads; a clean present is counted by
        // test_clean_present_matches_rebuild.
        particles::EffectRegistry::set_clean_present_default(false);
        particles::EffectRegistry registry(backend), reference(reference_backend);
        particles::EffectRegistry::set_clean_present_default(true);
        ThreadExecutor executor(workers);
        registry.set_executor(&executor);
        reference.set_executor(&executor);
        std::vector<particles::EffectHandle> handles, reference_handles;
        for (std::uint32_t seed = 1; seed <= 4; ++seed) {
            handles.push_back(registry.spawn(mixed_system(), seed, 256).value());
            reference_handles.push_back(reference.spawn(mixed_system(), seed, 256).value());
        }
        const auto camera = test_camera();
        std::vector<particles::EffectFrameStats> actual, expected;
        {
            const auto uploads = registry.defer_uploads();
            const auto immediate = reference.defer_uploads(false);
            for (int tick = 0; tick < 36; ++tick) {
                expect(bool(registry.advance_all(handles, 1.0F / 30.0F, camera, actual)), "deferred batch advances");
                expect(bool(reference.advance_all(reference_handles, 1.0F / 30.0F, camera, expected)), "reference advances");
                for (std::size_t index = 0; index < actual.size(); ++index)
                    expect(same_stats(actual[index], expected[index]), "deferred uploads retain every CPU sample and statistic");
                expect(backend.record.empty(), "catch-up samples never upload intermediate streams");
                expect(!reference_backend.record.empty(), "disabled scope retains immediate submission");
            }
            {
                const auto nested = registry.defer_uploads();
                expect(bool(registry.present_all(handles, camera)), "nested presentation succeeds");
            }
            expect(backend.record.empty(), "nested scope does not flush an unfinished frame");
            // A released group must not leave a queued dangling pointer or submit a dead RID.
            expect(bool(registry.release(handles.back())), "release during deferred frame succeeds");
            expect(bool(reference.release(reference_handles.back())), "reference releases the same group");
        }
        expect(backend.record.size() == 9, "one final upload per live drawable emitter across 36 samples");
        for (const auto& [resource, stream] : backend.last)
            expect(particles::stream_hash(stream) == particles::stream_hash(reference_backend.last.at(resource)),
                "the frame submits the final sample's stream");
        expect(backend.invalid_updates == 0 && backend.wrong_thread == 0, "deferred commits use live resources on the owner thread");
        const auto before = backend.record.size();
        expect(bool(registry.present_all(std::span<const particles::EffectHandle>(handles).first(3), camera)), "ordinary present after deferral succeeds");
        expect(backend.record.size() == before + 9, "scope exit restores immediate submission");
    }
}

namespace {
// Restores the registry defaults on every exit (a throwing play() included).
struct BucketingOff final {
    BucketingOff() { particles::EffectRegistry::set_stream_bucketing_default(false); }
    ~BucketingOff() { particles::EffectRegistry::set_stream_bucketing_default(true); }
    BucketingOff(const BucketingOff&) = delete;
    BucketingOff& operator=(const BucketingOff&) = delete;
};
struct SliceCap final {
    explicit SliceCap(const std::size_t slices) { particles::EffectRegistry::set_slice_cap_default(slices); }
    ~SliceCap() { particles::EffectRegistry::set_slice_cap_default(particles::StepExecutor::max_ordered); }
    SliceCap(const SliceCap&) = delete;
    SliceCap& operator=(const SliceCap&) = delete;
};
} // namespace

void test_batch_matches_serial() {
    test_deferred_frame_uploads();
    particles::EffectRegistry::set_stream_bucketing_default(false);
    const Script serial = play(false, nullptr, true);
    particles::EffectRegistry::set_stream_bucketing_default(true);
    expect(!serial.stats.empty() && serial.stats.front().hash != 0, "the serial reference hashes its frames");
    const Script inline_batch = play(true, nullptr, true);
    const ReverseExecutor reverse;
    const Script reversed = play(true, &reverse, true);
    const ThreadExecutor two(2), four(4);
    const Script threaded = play(true, &four, true);
    const ThreadExecutor eight(8);
    const Script wide = play(true, &eight, true);
    expect(same_script(serial, inline_batch), "a batch on the calling thread equals one advance after another");
    expect(same_script(serial, reversed), "a batch whose tasks run last to first equals the serial run");
    expect(same_script(serial, play(true, &two, true)), "a batch on 2 threads equals the serial run");
    expect(same_script(serial, threaded), "a batch on 4 threads equals the serial run (statistics, hashes, uploads)");
    expect(same_script(serial, wide), "a batch on 8 threads equals the serial run");
    expect(four.runs > 0 && eight.runs > 0, "the thread executors ran the batches");
    expect(same_script(serial,play(true,&four,true,40,45,false,true)),"opt-in timing cannot change worker output");
    // PF lane-serial (c): the slice count never changes the output. 300 effects pass the 256-slice cap. Like the
    // reference above, the serial run keeps the reference (unbucketed) stream path, so this also covers bucketing.
    const Script serial_large = [] {
        const BucketingOff reference;
        return play(false, nullptr, true, 300, 20);
    }();
    for (const std::size_t slices : {std::size_t{7}, std::size_t{64}, particles::StepExecutor::max_ordered}) {
        const SliceCap cap(slices);
        expect(same_script(serial_large, play(true, &four, true, 300, 20)) && same_script(serial_large, play(true, &eight, true, 300, 20)),
            "a batch cut into 7, 64 or 256 slices equals the serial run");
    }
}

void test_attachment_batch_matches_serial() {
    particles::EffectRegistry::set_stream_bucketing_default(false);
    const auto serial = play_attachments(false, nullptr);
    particles::EffectRegistry::set_stream_bucketing_default(true);
    const ThreadExecutor one(1), two(2), four(4), eight(8);
    const ReverseExecutor reverse;
    expect(!serial.steps.empty() && serial.created > 24, "attachment batch fixture respawns generations");
    expect(std::any_of(serial.steps.begin(), serial.steps.end(), [](const auto& step) {
        return step.stats.advance.death_bursts != 0;
    }), "attachment batch fixture exercises dependent death bursts");
    expect(same_attachments(serial, play_attachments(true, nullptr)), "attachment inline batch equals serial lifecycle");
    expect(same_attachments(serial, play_attachments(true, &one)), "attachment 1-thread batch equals serial lifecycle");
    expect(same_attachments(serial, play_attachments(true, &two)), "attachment 2-thread batch equals serial lifecycle");
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
// drawn stream once and hashes nothing in the live battle, in at most StepExecutor::max_ordered (256) tasks per batch.
void test_batch_work_counts() {
    const ThreadExecutor four(4);
    const std::size_t count = 100;
    const int frames = 10;
    // Written for a present that always builds; the dirty-tracked present is counted by
    // test_clean_present_matches_rebuild.
    particles::EffectRegistry::set_clean_present_default(false);
    const Script script = play(true, &four, false, count, frames);
    particles::EffectRegistry::set_clean_present_default(true);
    std::uint64_t particle_samples = 0;
    for (const auto& frame : script.stats) particle_samples += frame.particles;
    expect(script.work.particle_bucket_visits == particle_samples * 2,
        "advance and present each partition the system particles exactly once");
    expect(script.work.stream_particle_visits <= script.work.particle_bucket_visits,
        "stream candidate visits stay linear in live particles, independent of emitter count");
    // Frames 0-29 run all 100 effects (the fixture releases one at frame 30, past these 10).
    const std::uint64_t instance_frames = count * static_cast<std::uint64_t>(frames);
    expect(script.work.steps == instance_frames, "one step per effect per frame");
    expect(script.work.presents == instance_frames, "one present per effect per frame");
    expect(script.work.streams_built == instance_frames * 4 * 2, "each of the 4 emitters' streams built once per advance and present");
    expect(script.work.uploads == instance_frames * 3 * 2, "each of the 3 drawn streams uploaded once per advance and present");
    expect(script.work.streams_hashed == 0, "the live battle's frames hash no stream");
    expect(script.work.batches == static_cast<std::uint64_t>(frames) * 2, "one executor batch per advance_all and present_all");
    expect(script.work.tasks == script.work.batches * 100, "a batch of 100 effects runs as 100 tasks (one slice each, under the 256 cap)");
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
    // L1.2: the backend's own rule (GodotParticleBackend::culling_frame): a latched frame wins and the live view is
    // never read; unlatched, the live view is read. live_reads counts the reads of the live source.
    eawr::presentation::godot_backend::detail::ParticleLatch<particles::CullingFrame> latch;
    mutable std::uint64_t live_reads{};
    std::map<std::uint64_t, bool> shown;
    std::uint64_t groups{}, prepared{}, visible{}, stepped{};
    std::uint64_t create_emitter(const particles::EmitterRenderPlan& plan) override { return recording.create_emitter(plan); }
    void prepare_emitter(const std::uint64_t id, const particles::VertexStream& stream) override { recording.prepare_emitter(id, stream); }
    void update_emitter(const std::uint64_t id, const particles::VertexStream& stream) override { recording.update_emitter(id, stream); }
    void destroy_emitter(const std::uint64_t id) override { recording.destroy_emitter(id); }
    particles::CullingFrame culling_frame() const override {
        return latch.read([this] { ++live_reads; return view; });
    }
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
    expect(!paced_backend.recording.last.empty() && paced_backend.recording.last.begin()->second.indices.empty(),
        "a finished hidden drain clears its last backend submission");
    const auto empty_steps = paced.work().steps;
    expect(bool(paced.advance(a, 0.1F, camera)) && paced.work().steps == empty_steps, "hidden empty finished groups skip CPU work");

    // PS-39: moving the host outside view cannot hide its world-space trail.
    CullingBackend trail_backend;
    particles::EffectRegistry trail(trail_backend);
    const auto trail_handle = trail.spawn(culling_system(), 31, 128).value();
    expect(bool(trail.advance(trail_handle, 0.1F, camera)), "trail emits in view");
    expect(bool(trail.set_frame(trail_handle, away)) && bool(trail.present(trail_handle, camera)), "trail host moves away");
    // Particle bounds, not the host's, decide admission. The trail has no particle in the emitter's
    // frame, so the present after the host moved keeps the build of the advance (a rebuild would be
    // identical): every build so far was prepared and visible, and nothing hid the trail.
    expect(trail_backend.groups >= 1 && trail_backend.prepared == trail_backend.groups
        && trail_backend.visible == trail_backend.groups, "particles outside host bounds are admitted");
    expect(std::all_of(trail_backend.shown.begin(), trail_backend.shown.end(),
        [](const auto& entry) { return entry.second; }), "moving the host outside view never hides its trail");
    {
        particles::EffectRegistry::set_clean_present_default(false);
        CullingBackend rebuilt_backend;
        particles::EffectRegistry rebuilt(rebuilt_backend);
        particles::EffectRegistry::set_clean_present_default(true);
        const auto rebuilt_handle = rebuilt.spawn(culling_system(), 31, 128).value();
        expect(bool(rebuilt.advance(rebuilt_handle, 0.1F, camera)) && bool(rebuilt.set_frame(rebuilt_handle, away))
            && bool(rebuilt.present(rebuilt_handle, camera)), "the rebuilding trail twin runs");
        expect(rebuilt_backend.visible == 2 && rebuilt_backend.prepared == 2,
            "a rebuilt present admits preparation again with the same result");
        expect(trail_backend.recording.last.begin()->second.vertices.size()
            == rebuilt_backend.recording.last.begin()->second.vertices.size(),
            "the kept build is the stream a rebuild produces");
    }

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

// L1.2 gate (a): a latched culling frame gives the group counts and streams of the live path, whatever the live source
// does between the kick and the owner's reads (a lane thread would see an empty camera map there: fail open).
void test_culling_latch_matches_live() {
    // Host far outside the box_view(0) frustum, inside box_view(100): the path alternates culled and visible frames.
    const particles::EmitterFrame away{{100, 0, 0}, {}};
    const std::array<float, 8> path{0, 100, 100, 0, 0, 100, 0, 100};
    const auto camera = test_camera();
    CullingBackend live_backend, latched_backend, unlatched_hostile;
    particles::EffectRegistry live(live_backend), latched(latched_backend), hostile(unlatched_hostile);
    const auto live_handle = live.spawn(culling_system(), 17, 128).value();
    const auto latched_handle = latched.spawn(culling_system(), 17, 128).value();
    const auto hostile_handle = hostile.spawn(culling_system(), 17, 128).value();
    expect(bool(live.set_frame(live_handle, away)) && bool(latched.set_frame(latched_handle, away))
        && bool(hostile.set_frame(hostile_handle, away)), "the three twins bind their hosts");
    for (const float center : path) {
        // Twin 1: the live view, as the inline path reads it.
        live_backend.view = box_view(center);
        expect(bool(live.advance(live_handle, 0.05F, camera)) && bool(live.present(live_handle, camera)), "live twin runs");
        // Twin 2: main latches the view at the kick; the live source then reads as an empty map (what a second
        // thread's own thread_local camera table holds), and must not be consulted.
        latched_backend.latch.set(box_view(center));
        latched_backend.view = particles::CullingFrame{};
        expect(bool(latched.advance(latched_handle, 0.05F, camera)) && bool(latched.present(latched_handle, camera)),
            "latched twin runs");
        latched_backend.latch.set(std::nullopt);
        // Twin 3: the same hostile live source without the latch (the failure the latch prevents).
        unlatched_hostile.view = particles::CullingFrame{};
        expect(bool(hostile.advance(hostile_handle, 0.05F, camera)) && bool(hostile.present(hostile_handle, camera)),
            "unlatched hostile twin runs");
    }
    expect(latched_backend.live_reads == 0, "a latched frame never reads the live culling source");
    expect(live_backend.groups > 0 && live.work().streams_built > 0 && live.work().streams_built < hostile.work().streams_built,
        "the camera path really culls: the live path builds streams only in the visible frames");
    expect(latched_backend.groups == live_backend.groups && latched_backend.visible == live_backend.visible
        && latched_backend.prepared == live_backend.prepared && latched_backend.stepped == live_backend.stepped,
        "latched visible/prepared/stepped group counts equal the live path's");
    expect(latched_backend.recording.record == live_backend.recording.record && latched_backend.shown == live_backend.shown,
        "latched streams and visibility equal the live path's");
    const auto& live_work = live.work();
    const auto& latched_work = latched.work();
    expect(latched_work.steps == live_work.steps && latched_work.presents == live_work.presents
        && latched_work.uploads == live_work.uploads && latched_work.streams_built == live_work.streams_built
        && latched_work.updates_deferred == live_work.updates_deferred && latched_work.groups_visible == live_work.groups_visible
        && latched_work.groups_prepared == live_work.groups_prepared, "latched registry work counters equal the live path's");
    expect(hostile.work().streams_built > live_work.streams_built, "without the latch an empty camera table fails open");
}

// L1.2 gate (b): the latched frame number drives the residency expiry and the restore backoff exactly as the engine's
// frame counter does when it holds still, and keeps doing so when the live counter moves under the reader.
void test_frame_number_latch_matches_live() {
    namespace gb = eawr::presentation::godot_backend;
    using Cache = gb::detail::ParticleReuseCache<std::unique_ptr<int>>;
    const gb::detail::ParticleReusePolicy policy{"colour", "normal", "program", 8};
    struct Outcome {
        std::vector<std::uint64_t> released_at;   // frame of each expiry that released an entry
        std::vector<std::uint8_t> admitted;       // retry admission at each probe
    };
    // `engine` is the live counter. The inline twin reads it; the lane twin reads the value main latched at the kick,
    // while the live counter keeps moving (+7) as it does during a lane job.
    const auto run = [&](const bool latched) {
        gb::detail::ParticleLatch<std::uint64_t> latch;
        std::uint64_t engine = 1000;
        std::uint64_t live_reads = 0;
        const auto frame = [&] { return latch.read([&] { ++live_reads; return engine; }); };
        Cache cache;
        gb::detail::ParticleRestoreRetry retry;
        Outcome out;
        for (std::uint64_t step = 0; step < 400; ++step) {
            engine = 1000 + step;                       // the engine's frame for this kick
            if (latched) latch.set(engine);
            if (step % 40 == 0) {                       // a retirement at this frame
                auto value = std::make_unique<int>(static_cast<int>(step));
                static_cast<void>(cache.retain(policy, 10, value, frame()));
            }
            if (step % 25 == 0) static_cast<void>(retry.failed(frame()));
            if (step % 5 == 0) out.admitted.push_back(retry.admits(frame()) ? 1 : 0);
            if (latched) engine += 7;                   // the live counter moves during the lane job
            const auto now = frame();
            cache.expire(now, [&](std::unique_ptr<int>&) { out.released_at.push_back(now); });
            if (latched) latch.set(std::nullopt);
        }
        if (latched) expect(live_reads == 0, "a latched frame number never reads the engine counter");
        return out;
    };
    const Outcome live = run(false), lane_twin = run(true);
    expect(live.released_at.size() >= 3, "the scripted path expires several entries");
    expect(std::count(live.admitted.begin(), live.admitted.end(), 0) > 0 && std::count(live.admitted.begin(), live.admitted.end(), 1) > 0,
        "the scripted path both backs off and re-admits");
    expect(live.released_at == lane_twin.released_at && live.admitted == lane_twin.admitted,
        "the latched frame number gives the same expiry frames and retry admissions as the live counter");
}

// L1.2 gate (c): merging the lane thread's counters into main's adds the sum once and adds nothing when measurement is off.
void test_frame_work_merge_adds_once() {
    using Work = eawr::presentation::godot_backend::GodotParticleBackend::FrameWork;
    Work lane;
    lane.streams = 3; lane.uploads = 2; lane.replacements = 1;
    lane.bounds_calls = {5, 3, 2}; lane.visibility_calls = {4, 4, 0};
    lane.conversion_ms = 1.5; lane.submission_ms = 2.0; lane.retirement_ms = 0.25;
    lane.groups = 9; lane.visible_groups = 6; lane.prepared_groups = 5; lane.stepped_groups = 4;
    lane.census.streams = 7; lane.census.vertices = 70; lane.census.identical = 2;
    lane.census.class_streams = {1, 2, 3}; lane.census.class_keys = {3, 2, 1};
    lane.merged_groups = 2; lane.merged_streams = 8; lane.merged_vertices = 80; lane.merged_replacements = 1;
    lane.merge_flush_ms = 0.5;
    Work main_side;
    main_side.streams = 10; main_side.groups = 1; main_side.bounds_calls = {1, 1, 0}; main_side.census.class_keys = {1, 1, 1};
    main_side.merge_flush_ms = 0.125;
    main_side.merge(lane, true);
    expect(main_side.streams == 13 && main_side.uploads == 2 && main_side.replacements == 1 && main_side.groups == 10
        && main_side.visible_groups == 6 && main_side.prepared_groups == 5 && main_side.stepped_groups == 4,
        "one merge adds each integer counter once");
    expect(main_side.bounds_calls.attempted == 6 && main_side.bounds_calls.sent == 4 && main_side.bounds_calls.skipped == 2
        && main_side.visibility_calls.attempted == 4 && main_side.visibility_calls.sent == 4,
        "the call counters merge field by field");
    expect(main_side.census.streams == 7 && main_side.census.vertices == 70 && main_side.census.identical == 2
        && main_side.census.class_streams[2] == 3 && main_side.census.class_keys[0] == 4 && main_side.census.class_keys[2] == 2,
        "the batch census merges, class by class");
    expect(main_side.merged_groups == 2 && main_side.merged_streams == 8 && main_side.merged_vertices == 80
        && main_side.merged_replacements == 1, "the merged-draw counters merge");
    expect(main_side.conversion_ms == 1.5 && main_side.submission_ms == 2.0 && main_side.retirement_ms == 0.25
        && main_side.merge_flush_ms == 0.625, "the times merge");
    main_side.merge(lane, true);
    expect(main_side.streams == 16 && main_side.groups == 19 && main_side.census.class_keys[0] == 7,
        "a second merge adds the sum again (callers merge each lane frame once)");
    const Work before = main_side;
    main_side.merge(lane, false);
    expect(main_side.streams == before.streams && main_side.uploads == before.uploads && main_side.groups == before.groups
        && main_side.bounds_calls.attempted == before.bounds_calls.attempted
        && main_side.census.vertices == before.census.vertices && main_side.merge_flush_ms == before.merge_flush_ms,
        "a merge with measurement off adds nothing");
}

void test_empty_stream_residency() {
    auto system = culling_system();
    auto& emitter = system.emitters.front();
    emitter.spawn_interval = 0.4F;
    emitter.stop_time = 0;
    emitter.lifetime = 0.06F;
    emitter.lifetime_variation = 0.2F;
    emitter.velocity.shape = particles::Shape::sphere;
    emitter.velocity.radius_min = 1;
    emitter.velocity.radius_max = 3;
    const auto plans = particles::plan_system(system);
    const auto camera = test_camera();
    // A finite system may be naturally exhausted without being detached. The
    // query cannot consume future scheduling, RNG or the current live drain.
    {
        RecordingBackend backend;
        particles::EffectRegistry registry(backend);
        auto finite = system;
        finite.emitters.front().blend_mode = 0;
        finite.emitters.front().spawn_interval = 10;
        finite.emitters.front().freeze_time = 1;
        finite.emitters.front().lifetime = 0.1F;
        const auto handle = registry.spawn(finite, 11, 64).value();
        expect(bool(registry.advance(handle, 0.01F, camera)), "opaque finite generation starts");
        const auto empty = registry.advance(handle, 0.2F, camera).value();
        expect(empty.particles == 0 && !registry.exhausted(handle)
            && backend.empty_notifications.size() == 1 && !backend.empty_notifications.back().second,
            "an empty opaque span first notifies resumable residency");
        expect(bool(registry.advance(handle, 1.0F, camera)) && registry.exhausted(handle)
            && backend.empty_notifications.size() == 2 && backend.empty_notifications.back().second,
            "natural freeze inside an empty span upgrades the same resource to terminal");
        expect(backend.empty_notifications.front().first == backend.empty_notifications.back().first,
            "terminal upgrade preserves the logical resource handle");
        expect(bool(registry.present(handle, camera)) && bool(registry.advance(handle, 0.2F, camera))
            && backend.empty_notifications.size() == 2,
            "terminal emptiness is not re-notified after the upgrade");
    }
    {
        RecordingBackend backend;
        particles::EffectRegistry registry(backend);
        auto finite = system;
        finite.emitters.front().spawn_interval = 0.05F;
        finite.emitters.front().stop_time = 0.1F;
        finite.emitters.front().lifetime = 0.2F;
        const auto handle = registry.spawn(finite, 11, 64).value();
        const auto looping = registry.spawn(system, 11, 64).value();
        expect(!registry.exhausted(handle) && !registry.exhausted(looping) && !registry.exhausted(9999),
            "finite future births, loops and unknown handles are not exhausted");
        expect(bool(registry.advance(handle, 0.01F, camera)), "finite generation starts");
        auto alive = registry.advance(handle, 0.16F, camera).value();
        expect(alive.particles > 0 && !registry.exhausted(handle), "finite live particles keep the effect unexhausted");
        const auto empty = registry.advance(handle, 0.5F, camera).value();
        expect(empty.particles == 0 && !empty.finished && !empty.detached && registry.exhausted(handle),
            "finite exhaustion is read-only and independent of the detached finished predicate");
        expect(registry.exhausted(handle) && registry.live_effects() == 2,
            "repeated exhaustion queries leave ownership intact");
        expect(bool(registry.advance(looping, 0.01F, camera)) && !registry.exhausted(looping),
            "a loop remains live between its future root emissions");
        expect(bool(registry.set_detail({0, 0, false})) && !registry.exhausted(looping),
            "transient detail gates cannot prove exhaustion");
        auto delayed = finite;
        delayed.emitters.front().start_delay = 10;
        const auto pending = registry.spawn(delayed, 11, 64).value();
        expect(!registry.exhausted(pending), "delayed future emission is not exhausted before its first birth");
    }
    {
        auto finite = system;
        finite.emitters.front().spawn_interval = 0.05F;
        finite.emitters.front().stop_time = 0.1F;
        finite.emitters.front().lifetime = 0.2F;
        auto child = spray_emitter();
        child.creator_id = 39;
        child.parent_emitter = 0;
        child.stop_time = 0;
        child.spawn_interval = 0.1F;
        child.particles_per_interval = 2;
        child.lifetime = 0.3F;
        finite.emitters.push_back(child);
        RecordingBackend queried_backend, reference_backend;
        particles::EffectRegistry queried(queried_backend), reference(reference_backend);
        const auto a = queried.spawn(finite, 97, 256).value();
        const auto b = reference.spawn(finite, 97, 256).value();
        bool saw_child{};
        for (int frame = 0; frame < 90; ++frame) {
            for (int query = 0; query < 5; ++query) static_cast<void>(queried.exhausted(a));
            const auto qa = queried.advance(a, 1.0F / 30.0F, camera).value();
            const auto rb = reference.advance(b, 1.0F / 30.0F, camera).value();
            expect(same_stats(qa, rb) && qa.elapsed_seconds == rb.elapsed_seconds
                && qa.deferred_seconds == rb.deferred_seconds && qa.advance.requested == rb.advance.requested,
                "exhaustion queries preserve RNG, scheduling, complete streams and advance statistics against an unqueried twin");
            if (qa.emitters[1].particles != 0) {
                saw_child = true;
                expect(!queried.exhausted(a), "a live child drain keeps a finite parent effect unexhausted");
            }
        }
        expect(saw_child && queried.exhausted(a) && !queried.advance(a, 0, camera).value().finished,
            "a finite undetached effect with children naturally exhausts after the last child dies");
        expect(bool(queried.stop_emission(a)) && queried.exhausted(a)
            && queried.advance(a, 0, camera).value().finished,
            "an empty detached effect is both exhausted and finished");
    }
    const auto run = [&](const particles::StepExecutor* executor) {
        RecordingBackend backend;
        particles::EffectRegistry registry(backend);
        registry.set_executor(executor);
        constexpr std::size_t count = 16;
        std::vector<particles::EffectHandle> handles;
        std::vector<particles::CpuSystem> references;
        for (std::uint32_t index = 0; index < count; ++index) {
            handles.push_back(registry.spawn(system, 170 + index, 64).value());
            references.emplace_back(system, 170 + index, 64);
        }
        std::vector<particles::EffectFrameStats> stats;
        std::vector<std::uint64_t> hashes;
        std::size_t empty_frames{}, resumptions{};
        bool was_empty = false;
        for (int frame = 0; frame < 90; ++frame) {
            const float dt = frame == 0 ? 0 : 1.0F / 30.0F;
            expect(bool(registry.advance_all(handles, dt, camera, stats)), "dormant batch advances");
            const bool empty = stats.front().particles == 0;
            if (empty) ++empty_frames;
            if (was_empty && !empty) ++resumptions;
            was_empty = empty;
            for (std::size_t index = 0; index < count; ++index) {
                const auto advanced = references[index].advance(dt);
                particles::VertexStream stream;
                particles::build_stream(plans.front(), references[index].particles(), camera, stream);
                expect(stats[index].hash == particles::stream_hash(stream)
                    && stats[index].advance.spawned == advanced.spawned
                    && stats[index].advance.killed == advanced.killed,
                    "dormancy preserves every RNG bit, birth and death against the direct CPU reference");
                expect(!stats[index].finished && stats[index].emitters.front().drawn,
                    "empty undetached emitters retain logical draw admission and future emission");
                hashes.push_back(stats[index].hash);
            }
            const auto uploads = backend.record.size();
            expect(bool(registry.present_all(handles, camera)), "dormant batch presents");
            if (empty) expect(backend.record.size() == uploads, "repeated empty presents do no backend work");
        }
        expect(empty_frames > 30 && resumptions > 4, "fixture repeatedly drains and emits again on the same handles");
        expect(backend.created == count && registry.live_effects() == count,
            "resource dormancy never restarts the CPU lifecycle or replaces logical handles");
        for (const auto handle : handles) expect(bool(registry.release(handle)), "dormant handles release");
        expect(backend.created == backend.destroyed && backend.invalid_destroys == 0 && backend.wrong_thread == 0,
            "empty/resumed handles release exactly once on the caller");
        return hashes;
    };
    const auto serial = run(nullptr);
    for (const std::size_t workers : {1U, 2U, 4U, 6U, 8U}) {
        const ThreadExecutor executor(workers);
        expect(run(&executor) == serial, "dormant/resumed streams match on 1/2/4/6/8 workers");
    }
}


// The streams a backend holds, by resource: what the screen shows after the last upload.
std::map<std::uint64_t, std::uint64_t> held_streams(const RecordingBackend& backend) {
    std::map<std::uint64_t, std::uint64_t> held;
    for (const auto& [resource, stream] : backend.last) held[resource] = particles::stream_hash(stream);
    return held;
}

// A backend that cannot always finish an upload asks the registry for the same stream again.
class IncompleteBackend final : public particles::RenderBackend {
public:
    std::uint64_t create_emitter(const particles::EmitterRenderPlan&) override { return next_++; }
    void update_emitter(std::uint64_t, const particles::VertexStream&) override { ++uploads; }
    void destroy_emitter(std::uint64_t) override {}
    [[nodiscard]] std::uint64_t incomplete_updates() const noexcept override { return incomplete; }
    std::uint64_t incomplete{};
    std::size_t uploads{};
private:
    std::uint64_t next_{1};
};

// Dirty-tracked present: a present whose instance, camera and culling frame are unchanged keeps
// the streams of its last build. The twin registry rebuilds every present; after every call the
// two backends must hold the same streams, and only the work counts differ.
void test_clean_present_matches_rebuild() {
    particles::EffectRegistry::set_clean_present_default(false);
    RecordingBackend rebuilt_backend;
    particles::EffectRegistry rebuilt(rebuilt_backend);
    particles::EffectRegistry::set_clean_present_default(true);
    RecordingBackend clean_backend;
    particles::EffectRegistry clean(clean_backend);
    constexpr std::size_t count = 6;
    std::vector<particles::EffectHandle> handles;
    const particles::Basis3 identity{{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}};
    for (std::size_t index = 0; index < count; ++index) {
        const auto seed = 40U + static_cast<std::uint32_t>(index);
        const auto a = rebuilt.spawn(mixed_system(), seed, 128);
        const auto b = clean.spawn(mixed_system(), seed, 128);
        expect(bool(a) && bool(b) && a.value() == b.value(), "twin registries spawn alike");
        if (!a || !b) return;
        const particles::EmitterFrame frame{{static_cast<float>(index) * 2.0F, 0.0F, 0.0F}, identity};
        expect(bool(rebuilt.set_frame(a.value(), frame)) && bool(clean.set_frame(b.value(), frame)), "twin frames");
        handles.push_back(a.value());
    }
    auto camera = test_camera();
    std::vector<particles::EffectFrameStats> stats_a, stats_b;
    const auto same_screen = [&](const char* when) {
        expect(held_streams(rebuilt_backend) == held_streams(clean_backend), when);
    };
    const auto present_both = [&](const particles::CameraFrame& view, const char* when) {
        expect(bool(rebuilt.present_all(handles, view)) && bool(clean.present_all(handles, view)), "twin presents");
        same_screen(when);
    };
    for (int frame = 0; frame < 6; ++frame) {
        expect(bool(rebuilt.advance_all(handles, 1.0F / 30.0F, camera, stats_a))
            && bool(clean.advance_all(handles, 1.0F / 30.0F, camera, stats_b)), "twin advances");
        same_screen("an advance is always built");
    }
    // Nothing changed since the advance: the clean twin builds and uploads nothing.
    auto before = clean.work();
    present_both(camera, "a present after an advance shows the advance");
    present_both(camera, "a repeated present shows the same streams");
    expect(clean.work().presents == before.presents && clean.work().uploads == before.uploads
        && clean.work().streams_built == before.streams_built, "unchanged presents build and upload nothing");
    expect(clean.work().presents_clean == before.presents_clean + 2 * count, "every skipped present is counted");
    expect(rebuilt.work().presents == before.presents + 2 * count && rebuilt.work().presents_clean == 0,
        "the rebuilding twin builds each present");
    // A moved emitter frame dirties exactly that instance.
    before = clean.work();
    const particles::EmitterFrame moved{{9.0F, 4.0F, 1.0F}, identity};
    expect(bool(rebuilt.set_frame(handles[2], moved)) && bool(clean.set_frame(handles[2], moved)), "twin move");
    expect(bool(rebuilt.set_frame(handles[3], {{6.0F, 0.0F, 0.0F}, identity}))
        && bool(clean.set_frame(handles[3], {{6.0F, 0.0F, 0.0F}, identity})), "an equal frame is no change");
    present_both(camera, "a moved emitter follows its frame");
    expect(clean.work().presents + clean.work().presents_unmoved == before.presents + before.presents_unmoved + 1
        && clean.work().presents_clean == before.presents_clean + count - 1,
        "only the moved instance is looked at");
    // Brightness, a changed camera, detail and the end of emission each rebuild what they change.
    before = clean.work();
    expect(bool(rebuilt.set_brightness(handles[1], 0.5F)) && bool(clean.set_brightness(handles[1], 0.5F)), "twin brightness");
    expect(bool(rebuilt.set_brightness(handles[4], 1.0F)) && bool(clean.set_brightness(handles[4], 1.0F)), "equal brightness");
    present_both(camera, "brightness reaches the stream");
    expect(clean.work().presents == before.presents + 1, "only the dimmed instance is built");
    auto turned = camera;
    turned.position.x += 3.0F;
    turned.right.y += 0.25F;
    before = clean.work();
    present_both(turned, "a new camera rebuilds every stream");
    expect(clean.work().presents == before.presents + count, "a changed camera builds every instance");
    before = clean.work();
    present_both(turned, "the new camera, presented again");
    expect(clean.work().presents == before.presents, "the new camera builds once");
    expect(bool(rebuilt.set_detail({0.5F, 0.5F})) && bool(clean.set_detail({0.5F, 0.5F})), "twin detail");
    present_both(turned, "detail reaches the streams");
    expect(bool(rebuilt.stop_emission(handles[0])) && bool(clean.stop_emission(handles[0])), "twin stop");
    present_both(turned, "a stopped emission is built");
    expect(bool(rebuilt.advance_all(handles, 0.1F, turned, stats_a)) && bool(clean.advance_all(handles, 0.1F, turned, stats_b)),
        "twin advance after the changes");
    present_both(turned, "a present after the last advance");
    expect(clean_backend.invalid_updates == 0 && clean_backend.wrong_thread == 0, "the clean twin uploads only valid streams on the caller");
}


// A moved emitter frame with nothing living in it keeps the streams of the last build; one with
// a particle in the emitter's frame (translater 26) is built again. The rebuilding twin must
// hold the same streams after every present.
void test_unmoved_frame_keeps_streams() {
    particles::EffectRegistry::set_clean_present_default(false);
    RecordingBackend rebuilt_backend;
    particles::EffectRegistry rebuilt(rebuilt_backend);
    particles::EffectRegistry::set_clean_present_default(true);
    RecordingBackend clean_backend;
    particles::EffectRegistry clean(clean_backend);
    particles::SystemDefinition linked;
    auto glow = spray_emitter();
    glow.translater_id = 26;
    linked.emitters.push_back(glow);
    const particles::Basis3 identity{{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}};
    std::vector<particles::EffectHandle> handles;
    for (const auto* system : {&linked, &linked}) {
        const auto a = rebuilt.spawn(*system, 5U + static_cast<std::uint32_t>(handles.size()), 128);
        const auto b = clean.spawn(*system, 5U + static_cast<std::uint32_t>(handles.size()), 128);
        expect(bool(a) && bool(b) && a.value() == b.value(), "twin registries spawn alike");
        if (!a || !b) return;
        handles.push_back(a.value());
    }
    {
        const auto a = rebuilt.spawn(mixed_system(), 9, 128);
        const auto b = clean.spawn(mixed_system(), 9, 128);
        expect(bool(a) && bool(b) && a.value() == b.value(), "twin registries spawn the unlinked effect");
        if (!a || !b) return;
        handles.push_back(a.value());
    }
    const auto camera = test_camera();
    std::vector<particles::EffectFrameStats> stats_a, stats_b;
    for (int frame = 0; frame < 8; ++frame) {
        expect(bool(rebuilt.advance_all(handles, 1.0F / 30.0F, camera, stats_a))
            && bool(clean.advance_all(handles, 1.0F / 30.0F, camera, stats_b)), "twin advances");
    }
    expect(bool(rebuilt.present_all(handles, camera)) && bool(clean.present_all(handles, camera)), "settling present");
    const auto before = clean.work();
    for (int step = 1; step <= 4; ++step) {
        const particles::EmitterFrame moved{{static_cast<float>(step) * 1.5F, 0.5F * static_cast<float>(step), 0.0F}, identity};
        for (const auto handle : handles) {
            expect(bool(rebuilt.set_frame(handle, moved)) && bool(clean.set_frame(handle, moved)), "twin move");
        }
        expect(bool(rebuilt.present_all(handles, camera)) && bool(clean.present_all(handles, camera)), "twin presents a moved frame");
        expect(held_streams(rebuilt_backend) == held_streams(clean_backend), "a moved frame shows the same streams");
    }
    const auto after = clean.work();
    expect(after.presents - before.presents == 8, "the two effects with a linked particle are built after every move");
    expect(after.presents_unmoved - before.presents_unmoved == 4, "the effect with nothing in the emitter's frame is not");
}

void test_clean_present_resubmits_after_incomplete_update() {
    IncompleteBackend backend;
    particles::EffectRegistry registry(backend);
    const auto handle = registry.spawn(mixed_system(), 3, 64);
    expect(bool(handle), "incomplete-update fixture spawns");
    if (!handle) return;
    const auto camera = test_camera();
    std::vector<particles::EffectFrameStats> stats;
    const std::array<particles::EffectHandle, 1> one{handle.value()};
    expect(bool(registry.advance_all(one, 0.1F, camera, stats)) && bool(registry.present_all(one, camera)), "first frame");
    const auto settled = backend.uploads;
    expect(bool(registry.present_all(one, camera)) && backend.uploads == settled, "a settled present uploads nothing");
    backend.incomplete = 1;
    expect(bool(registry.present_all(one, camera)) && backend.uploads > settled,
        "a backend that left an upload undone gets the stream again");
    const auto resubmitted = backend.uploads;
    expect(bool(registry.present_all(one, camera)) && backend.uploads == resubmitted, "and only once");
}

// A backend that holds an upload back while a failed resource waits out its retry backoff.
class BackoffBackend final : public particles::RenderBackend {
public:
    std::uint64_t create_emitter(const particles::EmitterRenderPlan&) override { return next_++; }
    void update_emitter(std::uint64_t, const particles::VertexStream&) override {
        if (hold) { ++deferred; return; }
        ++uploads;
    }
    void destroy_emitter(std::uint64_t) override {}
    [[nodiscard]] std::uint64_t deferred_updates() const noexcept override { return deferred; }
    bool hold{};
    std::uint64_t deferred{};
    std::size_t uploads{};
private:
    std::uint64_t next_{1};
};

// A held-back upload is not a failure: only the instance whose update was held back builds and
// submits again; the rest of the frame stays clean.
void test_clean_present_resubmits_only_deferred_instance() {
    BackoffBackend backend;
    particles::EffectRegistry registry(backend);
    const auto first = registry.spawn(mixed_system(), 3, 64);
    const auto second = registry.spawn(mixed_system(), 4, 64);
    expect(bool(first) && bool(second), "backoff fixture spawns");
    if (!first || !second) return;
    const auto camera = test_camera();
    std::vector<particles::EffectFrameStats> stats;
    const std::array<particles::EffectHandle, 2> both{first.value(), second.value()};
    expect(bool(registry.advance_all(both, 0.1F, camera, stats)) && bool(registry.present_all(both, camera)), "first frame");
    const auto settled = backend.uploads;
    auto work = registry.work();
    expect(bool(registry.present_all(both, camera)) && backend.uploads == settled
        && registry.work().presents == work.presents, "a settled frame builds and uploads nothing");
    backend.hold = true;
    // Brightness, not a frame: a frame-only change with nothing linked keeps the last build here.
    expect(bool(registry.set_brightness(first.value(), 0.5F)), "dim the first");
    work = registry.work();
    expect(bool(registry.present_all(both, camera)) && backend.uploads == settled && backend.deferred > 0
        && registry.work().presents == work.presents + 1, "the held-back upload built only the dimmed instance");
    work = registry.work();
    expect(bool(registry.present_all(both, camera)) && registry.work().presents == work.presents + 1,
        "and only that instance asks again while the backoff lasts");
    backend.hold = false;
    expect(bool(registry.present_all(both, camera)) && backend.uploads > settled, "the stream lands once the backoff ends");
    const auto landed = backend.uploads;
    work = registry.work();
    expect(bool(registry.present_all(both, camera)) && backend.uploads == landed && registry.work().presents == work.presents,
        "then the frame is clean again");
}

const RecordingBackend& recorded(const RecordingBackend& backend) { return backend; }
const RecordingBackend& recorded(const CullingBackend& backend) { return backend.recording; }

// Two registries over backends of one type: one rebuilds every present, the other keeps clean builds.
template <class Backend>
struct TwinRegistries final {
    TwinRegistries() {
        particles::EffectRegistry::set_clean_present_default(false);
        rebuilt = std::make_unique<particles::EffectRegistry>(rebuilt_backend);
        particles::EffectRegistry::set_clean_present_default(true);
        clean = std::make_unique<particles::EffectRegistry>(clean_backend);
    }
    template <class Call> void both(const char* what, Call&& call) {
        const bool a = bool(call(*rebuilt));
        const bool b = bool(call(*clean));
        expect(a == b, what);
    }
    [[nodiscard]] bool same_screen() const {
        return held_streams(recorded(rebuilt_backend)) == held_streams(recorded(clean_backend));
    }
    Backend rebuilt_backend, clean_backend;
    std::unique_ptr<particles::EffectRegistry> rebuilt, clean;
};

particles::SystemDefinition linked_system() {
    auto system = mixed_system();
    for (auto& emitter : system.emitters) emitter.translater_id = 26;
    return system;
}

// Every input a present looks at, against the rebuilding twin: after each step the two backends
// hold the same streams. Linked (translater 26) particles make the emitter frame matter even when
// the advance consumed no time.
template <class Backend>
void clean_present_matches_rebuild_inputs() {
    TwinRegistries<Backend> twin;
    const particles::Basis3 identity{{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}};
    const particles::Basis3 turned{{0.0F, 0.0F, -1.0F}, {0.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}};
    const auto linked = twin.rebuilt->spawn(linked_system(), 21, 128);
    const auto linked_clean = twin.clean->spawn(linked_system(), 21, 128);
    const auto plain = twin.rebuilt->spawn(mixed_system(), 22, 128);
    const auto plain_clean = twin.clean->spawn(mixed_system(), 22, 128);
    expect(bool(linked) && bool(linked_clean) && bool(plain) && bool(plain_clean)
        && linked.value() == linked_clean.value() && plain.value() == plain_clean.value(), "twin registries spawn alike");
    if (!linked || !linked_clean || !plain || !plain_clean) return;
    const std::array<particles::EffectHandle, 2> handles{linked.value(), plain.value()};
    auto camera = test_camera();
    std::vector<particles::EffectFrameStats> stats_a, stats_b;
    const auto frames = [&](const float delta) {
        expect(bool(twin.rebuilt->advance_all(handles, delta, camera, stats_a))
            && bool(twin.clean->advance_all(handles, delta, camera, stats_b)), "twin advances");
    };
    const auto present = [&](const char* when) {
        expect(bool(twin.rebuilt->present_all(handles, camera)) && bool(twin.clean->present_all(handles, camera)), "twin presents");
        expect(twin.same_screen(), when);
    };
    const auto move = [&](const particles::EffectHandle handle, const particles::EmitterFrame& frame) {
        twin.both("twin frames", [&](particles::EffectRegistry& registry) { return registry.set_frame(handle, frame); });
    };
    move(linked.value(), {{0.0F, 0.0F, 0.0F}, identity});
    move(plain.value(), {{2.0F, 0.0F, 0.0F}, identity});
    for (int index = 0; index < 5; ++index) { frames(1.0F / 30.0F); expect(twin.same_screen(), "an advance is built"); }
    present("a present after the advances");

    // A frame set, then a zero-delta advance (builds without following), then a present.
    const auto before = held_streams(recorded(twin.clean_backend));
    move(linked.value(), {{7.0F, 1.0F, -2.0F}, identity});
    frames(0.0F);
    present("a linked particle follows a frame set before a zero-delta advance");
    expect(held_streams(recorded(twin.clean_backend)) != before, "the moved linked emitter changes its stream");
    move(linked.value(), {{7.0F, 1.0F, -2.0F}, turned});
    frames(0.0F);
    present("a linked particle follows a turn set before a zero-delta advance");
    const auto scalar = [&](const float delta) {
        twin.both("twin scalar advance", [&](particles::EffectRegistry& registry) { return registry.advance(handles[0], delta, camera); });
    };
    move(linked.value(), {{-4.0F, 0.0F, 3.0F}, identity});
    scalar(0.0F);
    twin.both("twin scalar present", [&](particles::EffectRegistry& registry) { return registry.present(handles[0], camera); });
    expect(twin.same_screen(), "the scalar zero-delta advance and present agree");
    move(linked.value(), {{-1.0F, 5.0F, 0.0F}, turned});
    scalar(std::numeric_limits<float>::quiet_NaN());
    twin.both("twin scalar present", [&](particles::EffectRegistry& registry) { return registry.present(handles[0], camera); });
    expect(twin.same_screen(), "an invalid delta leaves the follow to the present");

    // Mesh frame, dynamic and pinched-cylinder controls; a system without those bindings refuses
    // them alike in both twins, and the dirty mark must still not let a stale stream through.
    const particles::MeshFrame mesh{{1.0F, 2.0F, 3.0F}, identity};
    twin.both("twin mesh frame", [&](particles::EffectRegistry& registry) { return registry.set_mesh_frame(handles[1], mesh); });
    present("a mesh frame");
    twin.both("twin dynamic", [&](particles::EffectRegistry& registry) { return registry.set_dynamic(handles[1], 2.0F, 0.5F); });
    present("a dynamic size and energy");
    twin.both("twin pinch", [&](particles::EffectRegistry& registry) { return registry.set_pinched_cylinder(handles[1], 1.0F, 2.0F, 0.5F); });
    present("a pinched cylinder");

    // A changed culling frame rebuilds every instance.
    if constexpr (std::is_same_v<Backend, CullingBackend>) {
        twin.rebuilt_backend.view = twin.clean_backend.view = box_view(100);
        present("a new culling frame");
        twin.rebuilt_backend.view = twin.clean_backend.view = box_view();
        frames(0.05F);
        present("the culling frame back again");
    }

    // A released handle's resources and instance are reused by the next spawn.
    for (auto* registry : {twin.rebuilt.get(), twin.clean.get()}) expect(bool(registry->release(handles[1])), "twin release");
    const auto again = twin.rebuilt->spawn(mixed_system(), 23, 128);
    const auto again_clean = twin.clean->spawn(mixed_system(), 23, 128);
    expect(bool(again) && bool(again_clean) && again.value() == again_clean.value(), "twin respawn");
    if (!again || !again_clean) return;
    const std::array<particles::EffectHandle, 2> reused{handles[0], again.value()};
    move(again.value(), {{1.0F, 1.0F, 1.0F}, identity});
    expect(bool(twin.rebuilt->advance_all(reused, 0.05F, camera, stats_a))
        && bool(twin.clean->advance_all(reused, 0.05F, camera, stats_b)), "twin advance after reuse");
    expect(bool(twin.rebuilt->present_all(reused, camera)) && bool(twin.clean->present_all(reused, camera)), "twin present after reuse");
    expect(twin.same_screen(), "a reused handle shows its own build");

    // Uploads deferred to the end of a scope are still pending at the next present.
    move(linked.value(), {{3.0F, 3.0F, 3.0F}, identity});
    {
        const auto scope_a = twin.rebuilt->defer_uploads();
        const auto scope_b = twin.clean->defer_uploads();
        expect(bool(twin.rebuilt->advance_all(reused, 0.0F, camera, stats_a))
            && bool(twin.clean->advance_all(reused, 0.0F, camera, stats_b)), "twin deferred advance");
        expect(bool(twin.rebuilt->present_all(reused, camera)) && bool(twin.clean->present_all(reused, camera)),
            "twin deferred present");
    }
    expect(twin.same_screen(), "deferred uploads land once the scope ends");
    expect(bool(twin.rebuilt->present_all(reused, camera)) && bool(twin.clean->present_all(reused, camera)), "twin last present");
    expect(twin.same_screen(), "and stay the same afterwards");
}

void test_clean_present_matches_rebuild_for_every_input() {
    clean_present_matches_rebuild_inputs<RecordingBackend>();
    clean_present_matches_rebuild_inputs<CullingBackend>();
}

// The held-back upload after a MOVED frame: linked particles follow the frame, so the instance
// rebuilds even with the unmoved-frame skip, and only it asks again while the backoff lasts.
void test_clean_present_resubmits_deferred_moved_instance() {
    BackoffBackend backend;
    particles::EffectRegistry registry(backend);
    const auto first = registry.spawn(linked_system(), 3, 64);
    const auto second = registry.spawn(mixed_system(), 4, 64);
    expect(bool(first) && bool(second), "moved-frame backoff fixture spawns");
    if (!first || !second) return;
    const auto camera = test_camera();
    std::vector<particles::EffectFrameStats> stats;
    const std::array<particles::EffectHandle, 2> both{first.value(), second.value()};
    expect(bool(registry.advance_all(both, 0.1F, camera, stats)) && bool(registry.present_all(both, camera)), "first frame");
    const auto settled = backend.uploads;
    auto work = registry.work();
    expect(bool(registry.present_all(both, camera)) && backend.uploads == settled
        && registry.work().presents == work.presents, "a settled frame builds and uploads nothing");
    backend.hold = true;
    expect(bool(registry.set_frame(first.value(), {{3.0F, 0.0F, 0.0F}, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}})), "move the first");
    work = registry.work();
    expect(bool(registry.present_all(both, camera)) && backend.uploads == settled && backend.deferred > 0
        && registry.work().presents == work.presents + 1 && registry.work().presents_unmoved == work.presents_unmoved,
        "the moved linked instance was built, not skipped, and its upload held back");
    work = registry.work();
    expect(bool(registry.present_all(both, camera)) && registry.work().presents == work.presents + 1,
        "and only that instance asks again while the backoff lasts");
    backend.hold = false;
    expect(bool(registry.present_all(both, camera)) && backend.uploads > settled, "the stream lands once the backoff ends");
    const auto landed = backend.uploads;
    work = registry.work();
    expect(bool(registry.present_all(both, camera)) && backend.uploads == landed && registry.work().presents == work.presents,
        "then the frame is clean again");
}

// Runs every job on helper threads only: the calling thread is given none, so an ordered run
// must finish its commits after the helpers return.
class HelpersOnlyExecutor final : public particles::StepExecutor {
public:
    bool run(const std::size_t count, const std::function<void(std::size_t)>& task) const override {
        std::vector<std::thread> helpers;
        for (std::size_t index = 0; index < count; ++index) helpers.emplace_back([&task, index] { task(index); });
        for (std::thread& helper : helpers) helper.join();
        return true;
    }
    [[nodiscard]] std::size_t participants() const noexcept override { return 3; }
};

// StepExecutor::run_ordered: every task once, every commit once, in order, on the caller, each
// after its task, whatever the executor and however long the tasks take. The task durations are
// timing-driven (sleep_for), so the interleavings differ from run to run; the invariants checked
// do not depend on which interleaving happens.
void test_ordered_run_commits_in_order() {
    const ThreadExecutor one(1), two(2), four(4), eight(8);
    const HelpersOnlyExecutor helpers;
    const std::array<const particles::StepExecutor*, 5> executors{&one, &two, &four, &eight, &helpers};
    for (const auto* executor : executors) {
        for (const std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{64}}) {
            std::array<std::atomic<int>, particles::StepExecutor::max_ordered> built{};
            std::vector<std::size_t> committed;
            const auto caller = std::this_thread::get_id();
            bool in_order_after_task = true, on_caller = true;
            const bool ran = executor->run_ordered(count, [&](const std::size_t index) {
                // Later tasks finish first more often than not.
                std::this_thread::sleep_for(std::chrono::microseconds(40 * (count - index)));
                built[index].fetch_add(1);
            }, [&](const std::size_t index) {
                if (built[index].load() != 1 || committed.size() != index) in_order_after_task = false;
                if (std::this_thread::get_id() != caller) on_caller = false;
                committed.push_back(index);
            });
            expect(ran && committed.size() == count && in_order_after_task && on_caller,
                "an ordered run commits every task once, in order, after it, on the caller");
            for (std::size_t index = 0; index < count; ++index) expect(built[index].load() == 1, "every task runs exactly once");
        }
    }
    expect(!ThreadExecutor(2).run_ordered(particles::StepExecutor::max_ordered + 1, [](std::size_t) {}, [](std::size_t) {}),
        "more tasks than the ordered run takes fail");
    std::size_t consumed = 0;
    const bool failed = ThreadExecutor(4).run_ordered(8, [](const std::size_t index) {
        if (index == 3) throw std::runtime_error("task failure");
    }, [&](std::size_t) { ++consumed; });
    expect(!failed && consumed <= 3, "a failing task fails the run and nothing after it is committed");
    // A commit that throws fails the run, ends the commits and is not retried by a later job call.
    std::vector<std::size_t> committed;
    for (const auto* executor : executors) {
        committed.clear();
        const bool thrown = executor->run_ordered(8, [](std::size_t) {}, [&](const std::size_t index) {
            if (index == 2) throw std::runtime_error("commit failure");
            committed.push_back(index);
        });
        expect(!thrown && committed == std::vector<std::size_t>({0, 1}),
            "a throwing commit fails the run after the commits before it, once");
    }
}

// Pipelined commits against the batched serial run: the same statistics and the same uploads in
// the same order, on every worker count; the pipelined batches are counted.
void test_pipelined_commit_matches_serial_batches() {
    const auto serial = play(true, nullptr, true);
    expect(serial.work.pipelined_batches == 0, "no executor never pipelines");
    const ThreadExecutor two(2), four(4), eight(8);
    const HelpersOnlyExecutor helpers;
    const ThreadExecutor one(1);
    const std::array<const particles::StepExecutor*, 5> executors{&one, &two, &four, &eight, &helpers};
    for (const auto* executor : executors) {
        const auto piped = play(true, executor, true);
        expect(piped.uploads == serial.uploads, "pipelined commits upload the same streams in the same order");
        expect(piped.stats.size() == serial.stats.size(), "pipelined commits keep one statistics entry per advance");
        bool same = piped.stats.size() == serial.stats.size();
        for (std::size_t index = 0; same && index < serial.stats.size(); ++index) same = same_stats(piped.stats[index], serial.stats[index]);
        expect(same, "pipelined commits keep the statistics of the serial run");
        expect(piped.work.uploads == serial.work.uploads && piped.work.streams_built == serial.work.streams_built,
            "pipelined commits count the same work");
        expect((executor == &one) == (piped.work.pipelined_batches == 0), "only an executor with threads pipelines");
    }
    // Detail changes, a stop and a release in the script cover the other dirty sources.
    const auto detail_serial = play(true, nullptr, true, 24, 45, true);
    const auto detail_piped = play(true, &four, true, 24, 45, true);
    expect(detail_piped.uploads == detail_serial.uploads, "pipelined commits follow detail changes alike");
}

// A backend that says its commit must not overlap the builds: the registry then runs the
// ordinary batches (nothing pipelined), with the same uploads as the serial run.
class NoOverlapBackend final : public particles::RenderBackend {
public:
    RecordingBackend recording;
    std::uint64_t create_emitter(const particles::EmitterRenderPlan& plan) override { return recording.create_emitter(plan); }
    void prepare_emitter(const std::uint64_t id, const particles::VertexStream& stream) override { recording.prepare_emitter(id, stream); }
    void update_emitter(const std::uint64_t id, const particles::VertexStream& stream) override { recording.update_emitter(id, stream); }
    void empty_emitter(const std::uint64_t id, const bool terminal) override { recording.empty_emitter(id, terminal); }
    void destroy_emitter(const std::uint64_t id) override { recording.destroy_emitter(id); }
};

void test_pipelined_commit_off_when_backend_forbids_overlap() {
    const ThreadExecutor four(4);
    const auto run = [&](const particles::StepExecutor* executor) {
        NoOverlapBackend backend;
        particles::EffectRegistry registry(backend);
        registry.set_executor(executor);
        std::vector<particles::EffectHandle> handles;
        for (std::uint32_t index = 0; index < 12; ++index) {
            const auto handle = registry.spawn(mixed_system(), 70U + index, 128);
            expect(bool(handle), "no-overlap fixture spawns");
            if (handle) handles.push_back(handle.value());
        }
        const auto camera = test_camera();
        std::vector<particles::EffectFrameStats> stats;
        for (int frame = 0; frame < 8; ++frame) {
            expect(bool(registry.advance_all(handles, 1.0F / 30.0F, camera, stats)) && bool(registry.present_all(handles, camera)),
                "no-overlap frame");
        }
        return std::pair{backend.recording.record, registry.work().pipelined_batches};
    };
    const auto serial = run(nullptr);
    const auto asked = run(&four);
    expect(asked.first == serial.first, "a backend that forbids overlap uploads the serial run's streams in its order");
    expect(asked.second == 0, "and nothing was pipelined");
}

namespace {

// N1: an executor that can run a batch on a thread of its own, the calling thread elsewhere: the pool's phase is called by
// that thread as worker 0. Records which threads ran tasks and whether a batch was joined.
class AsyncExecutor final : public particles::StepExecutor {
public:
    explicit AsyncExecutor(const std::size_t threads) : inner_(threads) {}
    ~AsyncExecutor() override {
        if (thread_.joinable()) thread_.join();
    }
    [[nodiscard]] std::size_t participants() const noexcept override { return inner_.participants(); }
    bool run(const std::size_t count, const std::function<void(std::size_t)>& task) const override { return inner_.run(count, task); }
    bool start_async(const std::size_t count, const std::function<void(std::size_t)>& task) const override {
        if (refuse || running_) return false;
        running_ = true;
        ++starts;
        thread_ = std::thread([this, count, &task] {
            ok_ = inner_.run(count, [this, &task](const std::size_t index) {
                {
                    const std::lock_guard lock(mutex_);
                    task_threads.insert(std::this_thread::get_id());
                }
                task(index);
            });
        });
        return true;
    }
    bool join_async() const override {
        thread_.join();
        running_ = false;
        ++joins;
        return ok_;
    }
    mutable bool refuse{};
    mutable std::size_t starts{}, joins{};
    mutable std::set<std::thread::id> task_threads;

private:
    ThreadExecutor inner_;
    mutable std::mutex mutex_;
    mutable std::thread thread_;
    mutable bool running_{};
    mutable bool ok_{};
};

} // namespace

// N1: begin_advance_all + finish_advance_all give advance_all's statistics, uploads (same streams, same order) and
// counters, with the commits on the calling thread after the join and no task on the calling thread.
// PF lane-serial (c): also with 300 effects cut into 7, 64 and 256 slices (the async start reads the same cap).
void test_async_advance_matches_advance_all() {
    const auto camera = test_camera();
    struct Case { std::uint32_t effects; std::size_t slices; };
    for (const Case shape : {Case{24, particles::StepExecutor::max_ordered}, Case{300, 7}, Case{300, 64},
             Case{300, particles::StepExecutor::max_ordered}}) {
    const SliceCap cap(shape.slices);
    for (const std::size_t threads : {std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        const ThreadExecutor serial_pool(threads);
        AsyncExecutor async_pool(threads);
        RecordingBackend sync_backend, async_backend;
        particles::EffectRegistry sync_registry(sync_backend), async_registry(async_backend);
        sync_registry.set_executor(&serial_pool);
        async_registry.set_executor(&async_pool);
        std::vector<particles::EffectHandle> sync_handles, async_handles;
        for (std::uint32_t index = 0; index < shape.effects; ++index) {
            const auto left = sync_registry.spawn(mixed_system(), 300U + index, 256);
            const auto right = async_registry.spawn(mixed_system(), 300U + index, 256);
            expect(bool(left) && bool(right), "async fixture spawns");
            if (!left || !right) return;
            sync_handles.push_back(left.value());
            async_handles.push_back(right.value());
        }
        std::vector<particles::EffectFrameStats> sync_stats, async_stats;
        const auto caller = std::this_thread::get_id();
        for (int frame = 0; frame < 24; ++frame) {
            if (frame == 9) {
                // A drain released between frames leaves both runs alike.
                expect(bool(sync_registry.release(sync_handles[5])) && bool(async_registry.release(async_handles[5])), "async fixture releases");
                sync_handles.erase(sync_handles.begin() + 5);
                async_handles.erase(async_handles.begin() + 5);
            }
            std::vector<float> deltas;
            for (std::size_t index = 0; index < sync_handles.size(); ++index)
                deltas.push_back((1.0F + static_cast<float>(index % 3)) / 60.0F);
            expect(bool(sync_registry.advance_all(sync_handles, deltas, camera, sync_stats)), "inline batch");
            const std::size_t committed_before = async_backend.record.size();
            const auto begun = async_registry.begin_advance_all(async_handles, deltas, camera, async_stats);
            expect(bool(begun) && begun.value(), "the batch starts without the caller");
            expect(async_registry.advance_pending(), "the batch is pending");
            // The caller does other work now: no stream is committed yet.
            expect(async_backend.record.size() == committed_before, "nothing is committed before the join");
            expect(bool(async_registry.finish_advance_all()), "the batch joins");
            expect(!async_registry.advance_pending(), "nothing is pending after the join");
            expect(sync_stats.size() == async_stats.size(), "one statistics entry per handle");
            for (std::size_t index = 0; index < sync_stats.size() && index < async_stats.size(); ++index)
                expect(same_stats(sync_stats[index], async_stats[index]), "the async batch's statistics equal the inline batch's");
            expect(bool(sync_registry.present_all(sync_handles, camera)) && bool(async_registry.present_all(async_handles, camera)),
                "a present after the async batch");
        }
        expect(async_backend.record == sync_backend.record, "the async batch commits the inline batch's streams in its order");
        expect(async_backend.wrong_thread == 0, "every commit ran on the calling thread");
        expect(!async_pool.task_threads.contains(caller), "no task ran on the calling thread");
        expect(async_pool.starts == 24 && async_pool.joins == 24, "every batch was started and joined once");
        expect(async_registry.work().steps == sync_registry.work().steps && async_registry.work().uploads == sync_registry.work().uploads
                && async_registry.work().streams_built == sync_registry.work().streams_built
                && async_registry.work().groups_prepared == sync_registry.work().groups_prepared,
            "the async batch counts the inline batch's work");
    }
    }
}

// N1: what begin_advance_all leaves to the caller: no executor able to run async, one handle, a deferred-upload scope, a
// backend that forbids overlap, a refused start; a bad batch fails before anything changes; the destructor joins.
void test_async_advance_falls_back_and_joins() {
    const auto camera = test_camera();
    const auto deltas = [](const std::size_t count) { return std::vector<float>(count, 1.0F / 30.0F); };
    const auto make = [](particles::EffectRegistry& registry, const std::size_t count) {
        std::vector<particles::EffectHandle> handles;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto handle = registry.spawn(mixed_system(), 500U + index, 128);
            expect(bool(handle), "fallback fixture spawns");
            if (handle) handles.push_back(handle.value());
        }
        return handles;
    };
    std::vector<particles::EffectFrameStats> stats;
    {
        // An executor with no thread of its own, no executor at all, one participant: the caller runs advance_all.
        const ThreadExecutor pool(4);
        const ThreadExecutor lone(1);
        AsyncExecutor single(1);
        for (const particles::StepExecutor* executor : std::array<const particles::StepExecutor*, 4>{&pool, nullptr, &lone, &single}) {
            RecordingBackend backend;
            particles::EffectRegistry registry(backend);
            registry.set_executor(executor);
            const auto handles = make(registry, 6);
            const auto begun = registry.begin_advance_all(handles, deltas(6), camera, stats);
            expect(bool(begun) && !begun.value() && !registry.advance_pending(), "the caller keeps a batch the executor cannot run async");
            expect(bool(registry.advance_all(handles, deltas(6), camera, stats)) && stats.size() == 6, "and runs it itself");
        }
    }
    AsyncExecutor pool(4);
    {
        RecordingBackend backend;
        particles::EffectRegistry registry(backend);
        registry.set_executor(&pool);
        const auto handles = make(registry, 6);
        const auto one = registry.begin_advance_all(std::span<const particles::EffectHandle>(handles).first(1), deltas(1), camera, stats);
        expect(bool(one) && !one.value(), "a single handle is not worth the thread");
        {
            const auto scope = registry.defer_uploads(true);
            const auto deferred = registry.begin_advance_all(handles, deltas(6), camera, stats);
            expect(bool(deferred) && !deferred.value(), "inside a deferred-upload scope the pack waits for the scope's exit");
        }
        pool.refuse = true;
        const auto refused = registry.begin_advance_all(handles, deltas(6), camera, stats);
        expect(bool(refused) && !refused.value() && !registry.advance_pending(), "a refused start leaves the batch to the caller");
        expect(bool(registry.advance_all(handles, deltas(6), camera, stats)), "which then runs inline after a refused start");
        pool.refuse = false;
        const auto mismatch = registry.begin_advance_all(handles, deltas(5), camera, stats);
        expect(!mismatch && !registry.advance_pending(), "one delta per handle");
        auto unknown = handles;
        unknown.push_back(987654321U);
        const auto missing = registry.begin_advance_all(unknown, deltas(7), camera, stats);
        expect(!missing && !registry.advance_pending() && pool.starts == 0, "an unknown handle fails before any task starts");
        const auto started = registry.begin_advance_all(handles, deltas(6), camera, stats);
        expect(bool(started) && started.value(), "a good batch starts");
        const auto second = registry.begin_advance_all(handles, deltas(6), camera, stats);
        expect(!second, "a second batch cannot start while one is pending");
        expect(bool(registry.finish_advance_all()) && bool(registry.finish_advance_all()), "finishing twice is harmless");
    }
    expect(pool.starts == pool.joins, "the first batches were all joined");
    {
        // The destructor joins a batch nobody finished.
        RecordingBackend backend;
        std::size_t joins_before = 0;
        {
            particles::EffectRegistry registry(backend);
            registry.set_executor(&pool);
            const auto handles = make(registry, 6);
            joins_before = pool.joins;
            const auto started = registry.begin_advance_all(handles, deltas(6), camera, stats);
            expect(bool(started) && started.value(), "an unfinished batch starts");
        }
        expect(pool.joins == joins_before + 1 && pool.starts == pool.joins, "the destructor joined the unfinished batch");
    }
    {
        // A backend that forbids overlap keeps the batch on the caller.
        NoOverlapBackend backend;
        particles::EffectRegistry registry(backend);
        registry.set_executor(&pool);
        const auto handles = make(registry, 6);
        const auto begun = registry.begin_advance_all(handles, deltas(6), camera, stats);
        expect(bool(begun) && !begun.value(), "a backend that forbids overlap does not pack beside the caller's work");
    }
}

namespace {

// Counts packs, can refuse overlap, can throw from the Nth pack, and carries a culling view.
class ProbeBackend final : public particles::RenderBackend {
public:
    RecordingBackend recording;
    particles::CullingFrame view{};
    bool overlap{true};
    std::atomic<std::uint64_t> prepares{};
    std::atomic<std::int64_t> throw_at{-1};
    std::uint64_t create_emitter(const particles::EmitterRenderPlan& plan) override { return recording.create_emitter(plan); }
    void prepare_emitter(const std::uint64_t id, const particles::VertexStream& stream) override {
        const auto call = static_cast<std::int64_t>(prepares.fetch_add(1));
        if (call == throw_at.load()) throw std::runtime_error("injected pack failure");
        recording.prepare_emitter(id, stream);
    }
    void update_emitter(const std::uint64_t id, const particles::VertexStream& stream) override { recording.update_emitter(id, stream); }
    void destroy_emitter(const std::uint64_t id) override { recording.destroy_emitter(id); }
    [[nodiscard]] bool overlapped_commit_safe() const noexcept override { return overlap; }
    [[nodiscard]] particles::CullingFrame culling_frame() const override { return view; }
};

// An executor whose runs can be refused, for the exit batch of a deferred-upload scope.
class SwitchExecutor final : public particles::StepExecutor {
public:
    ThreadExecutor inner{4};
    mutable bool refuse{};
    [[nodiscard]] std::size_t participants() const noexcept override { return inner.participants(); }
    bool run(const std::size_t count, const std::function<void(std::size_t)>& task) const override {
        return refuse ? false : inner.run(count, task);
    }
};

// The group staging of the Godot backend in miniature: a pack compares with the stream committed last (or,
// as before the fix, with the previous pack), a commit dirties the group when the emitter's stream changed,
// and a flush copies the packed bytes of every live emitter when the group is dirty. What the GPU holds must
// be the last committed stream.
class GroupModelBackend final : public particles::RenderBackend {
public:
    struct Slot {
        std::uint64_t packed_hash{};
        std::size_t packed_vertices{};
        bool unchanged{};
        bool in_live{};
        std::size_t committed_vertices{};
        std::uint64_t committed_hash{};
        std::uint64_t gpu_hash{};
    };
    std::map<std::uint64_t, Slot> slots;
    // true: a pack compares with the stream committed last (the adapter's rule); false: with the previous pack
    // (the rule before the fix, kept to show the failure test can fail).
    bool compare_with_commit{true};
    std::atomic<std::int64_t> prepares{};
    std::atomic<std::int64_t> throw_at{-1};
    bool dirty{};
    std::uint64_t next{1};
    std::uint64_t create_emitter(const particles::EmitterRenderPlan&) override {
        slots[next];
        return next++;
    }
    void prepare_emitter(const std::uint64_t id, const particles::VertexStream& stream) override {
        if (prepares.fetch_add(1) == throw_at.load()) throw std::runtime_error("injected pack failure");
        Slot& slot = slots.at(id);
        const auto hash = particles::stream_hash(stream);
        slot.unchanged = compare_with_commit ? slot.committed_vertices == stream.vertices.size() && hash == slot.committed_hash
                                             : slot.packed_vertices == stream.vertices.size() && hash == slot.packed_hash;
        slot.packed_hash = hash;
        slot.packed_vertices = stream.vertices.size();
    }
    void update_emitter(const std::uint64_t id, const particles::VertexStream& stream) override {
        Slot& slot = slots.at(id);
        slot.committed_hash = particles::stream_hash(stream);
        slot.committed_vertices = stream.vertices.size();
        if (stream.vertices.empty()) {
            slot.in_live = false;
            dirty = true;
            return;
        }
        if (!slot.in_live) {
            slot.in_live = true;
            dirty = true;
        } else if (!slot.unchanged) {
            dirty = true;
        }
    }
    void destroy_emitter(const std::uint64_t id) override { slots.erase(id); }
    [[nodiscard]] bool overlapped_commit_safe() const noexcept override { return true; }
    void flush() {
        if (!dirty) return;
        dirty = false;
        for (auto& entry : slots) {
            if (entry.second.in_live) entry.second.gpu_hash = entry.second.packed_hash;
        }
    }
};

} // namespace

// A deferred-upload scope packs nothing in its passes and each committed stream once at its exit:
// the committed streams equal those of immediate submission, whatever the executor and the view
// (culled instances are not prepared), with an instance created in the scope and one released in it.
void test_lazy_packing_matches_eager_commit() {
    const auto camera = test_camera();
    const ThreadExecutor one(1), two(2), four(4), eight(8);
    const std::array<const particles::StepExecutor*, 5> executors{nullptr, &one, &two, &four, &eight};
    for (const auto* executor : executors) {
        for (const bool overlap : {true, false}) {
            ProbeBackend lazy_backend, eager_backend;
            lazy_backend.overlap = eager_backend.overlap = overlap;
            lazy_backend.view = eager_backend.view = box_view();
            particles::EffectRegistry::set_clean_present_default(false);
            particles::EffectRegistry lazy(lazy_backend), eager(eager_backend);
            particles::EffectRegistry::set_clean_present_default(true);
            lazy.set_executor(executor);
            eager.set_executor(executor);
            std::vector<particles::EffectHandle> lazy_handles, eager_handles;
            const auto spawn_into = [&](particles::EffectRegistry& registry, std::vector<particles::EffectHandle>& handles,
                                        const std::uint32_t index) {
                const auto handle = registry.spawn(culling_system(), index + 42, 128);
                expect(bool(handle), "the fixture spawns");
                if (!handle) return;
                const particles::EmitterFrame frame{{static_cast<float>(index) * 2.0F, 0, 0}, {}};
                expect(bool(registry.set_frame(handle.value(), frame)), "the fixture frame is set");
                handles.push_back(handle.value());
            };
            for (std::uint32_t index = 0; index < 6; ++index) {
                spawn_into(lazy, lazy_handles, index);
                spawn_into(eager, eager_handles, index);
            }
            std::vector<particles::EffectFrameStats> lazy_stats, eager_stats;
            std::uint64_t packed_visible_frames = 0;
            for (int frame = 0; frame < 24; ++frame) {
                lazy_backend.view = eager_backend.view = box_view(frame >= 8 && frame < 16 ? 100.0F : 0.0F);
                const auto packs_before = lazy_backend.prepares.load();
                const auto commits_before = lazy_backend.recording.record.size();
                {
                    const auto scope = lazy.defer_uploads();
                    expect(bool(lazy.advance_all(lazy_handles, 1.0F / 30.0F, camera, lazy_stats)), "lazy first pass");
                    expect(bool(lazy.advance_all(lazy_handles, 1.0F / 30.0F, camera, lazy_stats)), "lazy second pass");
                    if (frame == 3) spawn_into(lazy, lazy_handles, 6);
                    expect(bool(lazy.present_all(lazy_handles, camera)), "lazy third pass");
                    expect(lazy_backend.prepares.load() == packs_before, "no pass of the scope packs");
                    expect(lazy_backend.recording.record.size() == commits_before, "no pass of the scope commits");
                }
                expect(bool(eager.advance_all(eager_handles, 1.0F / 30.0F, camera, eager_stats)), "eager first pass");
                expect(bool(eager.advance_all(eager_handles, 1.0F / 30.0F, camera, eager_stats)), "eager second pass");
                if (frame == 3) spawn_into(eager, eager_handles, 6);
                expect(bool(eager.present_all(eager_handles, camera)), "eager third pass");
                const auto packs = lazy_backend.prepares.load() - packs_before;
                const auto commits = lazy_backend.recording.record.size() - commits_before;
                expect(packs <= commits, "at most one pack per committed stream");
                if (packs != 0) ++packed_visible_frames;
                expect(frame < 8 || frame >= 16 || packs == 0, "a culled frame packs nothing");
                for (const auto& entry : eager_backend.recording.last) {
                    if (!eager_backend.recording.live.contains(entry.first)) continue;
                    const auto found = lazy_backend.recording.last.find(entry.first);
                    expect(found != lazy_backend.recording.last.end()
                        && particles::stream_hash(found->second) == particles::stream_hash(entry.second),
                        "the scope's exit commits the final stream of immediate submission");
                }
                if (frame == 20) {
                    expect(bool(lazy.advance_all(lazy_handles, 1.0F / 30.0F, camera, lazy_stats)), "pending before release");
                    {
                        const auto scope = lazy.defer_uploads();
                        expect(bool(lazy.advance_all(lazy_handles, 1.0F / 30.0F, camera, lazy_stats)), "pass before release");
                        expect(bool(lazy.release(lazy_handles.back())), "a parked instance is released in the scope");
                        lazy_handles.pop_back();
                    }
                    expect(bool(eager.advance_all(eager_handles, 1.0F / 30.0F, camera, eager_stats)), "eager pass before release");
                    expect(bool(eager.advance_all(eager_handles, 1.0F / 30.0F, camera, eager_stats)), "eager second pass before release");
                    expect(bool(eager.release(eager_handles.back())), "eager releases the same instance");
                    eager_handles.pop_back();
                }
            }
            expect(packed_visible_frames > 0, "visible frames pack");
            expect(lazy_backend.recording.wrong_thread == 0 && lazy_backend.recording.invalid_updates == 0
                && lazy_backend.recording.invalid_destroys == 0, "the exit commits on the caller with live resources");
            particles::EffectRegistry::set_clean_present_default(true);
        }
    }
}

// An exit batch that cannot run, or whose pack throws, still commits every parked instance once and in
// the order of an exit that ran, packing before each commit.
void test_lazy_packing_completes_after_failures() {
    const auto camera = test_camera();
    const auto play = [&](const bool refuse, const std::int64_t throw_at) {
        SwitchExecutor executor;
        ProbeBackend backend;
        backend.throw_at = throw_at;
        backend.view = box_view();
        particles::EffectRegistry registry(backend);
        registry.set_executor(&executor);
        std::vector<particles::EffectHandle> handles;
        for (std::uint32_t index = 0; index < 5; ++index) {
            const auto handle = registry.spawn(culling_system(), index + 7, 128);
            expect(bool(handle), "failure fixture spawns");
            if (!handle) continue;
            expect(bool(registry.set_frame(handle.value(), {{static_cast<float>(index), 0, 0}, {}})), "failure fixture frame");
            handles.push_back(handle.value());
        }
        std::vector<particles::EffectFrameStats> stats;
        for (int frame = 0; frame < 6; ++frame) {
            {
                const auto scope = registry.defer_uploads();
                expect(bool(registry.advance_all(handles, 1.0F / 30.0F, camera, stats)), "failure fixture pass");
                expect(bool(registry.present_all(handles, camera)), "failure fixture present");
                executor.refuse = refuse && frame == 3;
            }
            executor.refuse = false;
            if (throw_at >= 0 && frame == 0) backend.throw_at = -1;
        }
        expect(backend.recording.wrong_thread == 0 && backend.recording.invalid_updates == 0, "failure exits stay on the caller");
        return backend.recording.record;
    };
    const auto reference = play(false, -1);
    expect(!reference.empty(), "the reference exit commits");
    expect(play(true, -1) == reference, "a refused exit batch commits the same streams in the same order");
    expect(play(false, 0) == reference, "a pack that throws is packed again and commits the same streams in the same order");
    expect(play(false, 2) == reference, "a later pack that throws does too");
}

// The staged group must hold the last committed stream after every frame, with and without an executor.
// This registry packs once per exit, so the test does not tell the compare rules apart (the previous-pack
// rule passes it too); it pins the invariant. The rule itself is told apart by the control in
// test_lazy_packing_keeps_merged_groups_current_after_failures, where a stream is packed twice before one
// commit. (The adapter's old per-pass packing, which made a scope's second pack compare equal with the
// first, is gone from the registry.)
void test_lazy_packing_keeps_merged_groups_current() {
    const auto camera = test_camera();
    const ThreadExecutor four(4);
    for (const particles::StepExecutor* executor : std::array<const particles::StepExecutor*, 2>{nullptr, &four}) {
        GroupModelBackend backend;
        particles::EffectRegistry::set_clean_present_default(false);
        particles::EffectRegistry registry(backend);
        particles::EffectRegistry::set_clean_present_default(true);
        registry.set_executor(executor);
        std::vector<particles::EffectHandle> handles;
        for (std::uint32_t seed = 1; seed <= 3; ++seed) handles.push_back(registry.spawn(mixed_system(), seed, 256).value());
        std::vector<particles::EffectFrameStats> stats;
        const auto check = [&](const char* when) {
            backend.flush();
            for (const auto& entry : backend.slots) {
                if (!entry.second.in_live) continue;
                expect(entry.second.gpu_hash == entry.second.committed_hash, when);
            }
        };
        expect(bool(registry.advance_all(handles, 1.0F / 30.0F, camera, stats)) && bool(registry.present_all(handles, camera)), "first frame");
        check("an ordinary frame leaves the group at the committed stream");
        for (int frame = 0; frame < 10; ++frame) {
            {
                const auto scope = registry.defer_uploads();
                expect(bool(registry.advance_all(handles, 1.0F / 30.0F, camera, stats)), "scope advance");
                expect(bool(registry.present_all(handles, camera)), "scope present rebuilds the advance's stream");
            }
            check("a deferred frame whose second pass equals its first leaves the group at the committed stream");
        }
    }
}

// The registry-level failure contract of the pipelined commit: a build that throws fails the batch with
// the batch diagnostic, the instances committed before the failed slice stay committed (a prefix of what
// an error-free run commits), and the next frame commits everything again.
void test_pipelined_failure_keeps_a_prefix_and_recovers() {
    const auto camera = test_camera();
    const ThreadExecutor four(4);
    const auto make = [&](ProbeBackend& backend, particles::EffectRegistry& registry, std::vector<particles::EffectHandle>& handles) {
        backend.view = box_view();
        registry.set_executor(&four);
        for (std::uint32_t index = 0; index < 10; ++index) {
            const auto handle = registry.spawn(culling_system(), index + 3, 128);
            expect(bool(handle), "failure fixture spawns");
            if (!handle) continue;
            const particles::EmitterFrame frame{{static_cast<float>(index) * 0.5F, 0, 0}, {}};
            expect(bool(registry.set_frame(handle.value(), frame)), "failure fixture frame");
            handles.push_back(handle.value());
        }
    };
    ProbeBackend clean_backend, failing_backend;
    particles::EffectRegistry clean(clean_backend), failing(failing_backend);
    std::vector<particles::EffectHandle> clean_handles, failing_handles;
    make(clean_backend, clean, clean_handles);
    make(failing_backend, failing, failing_handles);
    std::vector<particles::EffectFrameStats> stats;
    // Warm up so every emitter has live particles (a burst needs more than one 1/30 s step to start).
    for (int frame = 0; frame < 3; ++frame) {
        expect(bool(clean.advance_all(clean_handles, 1.0F / 30.0F, camera, stats)), "warm-up of the error-free registry");
        expect(bool(failing.advance_all(failing_handles, 1.0F / 30.0F, camera, stats)), "warm-up of the failing registry");
    }
    const auto clean_before = clean_backend.recording.record.size();
    expect(bool(clean.advance_all(clean_handles, 1.0F / 30.0F, camera, stats)), "the error-free batch succeeds");
    const std::vector<std::uint64_t> complete(clean_backend.recording.record.begin() + static_cast<std::ptrdiff_t>(clean_before),
        clean_backend.recording.record.end());
    expect(complete.size() > 3, "the error-free batch commits several streams");
    const auto failing_before = failing_backend.recording.record.size();
    failing_backend.throw_at = static_cast<std::int64_t>(failing_backend.prepares.load()) + 3;
    const auto failed = failing.advance_all(failing_handles, 1.0F / 30.0F, camera, stats);
    expect(!failed && failed.error().code == particles::diagnostic_codes::batch, "a build that throws fails the batch");
    const std::vector<std::uint64_t> prefix(failing_backend.recording.record.begin() + static_cast<std::ptrdiff_t>(failing_before),
        failing_backend.recording.record.end());
    expect(prefix.size() < complete.size(), "the failed slice and the ones after it are not committed");
    expect(std::equal(prefix.begin(), prefix.end(), complete.begin()), "what was committed is a prefix of the error-free commits");
    failing_backend.throw_at = -1;
    const auto before = failing_backend.recording.record.size();
    expect(bool(failing.advance_all(failing_handles, 1.0F / 30.0F, camera, stats)), "the next frame succeeds");
    expect(failing_backend.recording.record.size() - before == complete.size(), "and commits every instance again");
    expect(failing_backend.recording.wrong_thread == 0 && failing_backend.recording.invalid_updates == 0,
        "commits stay on the caller with live resources");
}

namespace {
// While failing, runs every job on one helper thread and then reports failure. A job of an ordered batch that runs
// off the calling thread only builds (packs), it never commits, so the exit's run packs everything, commits
// nothing and fails: the serial fallback then packs each instance a second time.
class RunThenFailExecutor final : public particles::StepExecutor {
public:
    ThreadExecutor inner{4};
    mutable bool failing{};
    [[nodiscard]] std::size_t participants() const noexcept override { return inner.participants(); }
    bool run(const std::size_t count, const std::function<void(std::size_t)>& task) const override {
        if (!failing) return inner.run(count, task);
        std::thread helper([&] {
            for (std::size_t index = 0; index < count; ++index) task(index);
        });
        helper.join();
        return false;
    }
};
} // namespace

// A failed exit packs instances a second time (in the run and again in the fallback) before they are committed once.
// The staged group must still hold the last committed stream: the pack compares with the commit, not with the previous
// pack. The control compares with the previous pack, as the adapter did before, and must come out stale.
void test_lazy_packing_keeps_merged_groups_current_after_failures() {
    const auto camera = test_camera();
    const auto play = [&](const bool compare_with_commit, const bool throwing) {
        RunThenFailExecutor executor;
        GroupModelBackend backend;
        backend.compare_with_commit = compare_with_commit;
        particles::EffectRegistry::set_clean_present_default(false);
        particles::EffectRegistry registry(backend);
        particles::EffectRegistry::set_clean_present_default(true);
        registry.set_executor(&executor);
        std::vector<particles::EffectHandle> handles;
        for (std::uint32_t seed = 1; seed <= 3; ++seed) handles.push_back(registry.spawn(mixed_system(), seed, 256).value());
        std::vector<particles::EffectFrameStats> stats;
        expect(bool(registry.advance_all(handles, 1.0F / 30.0F, camera, stats)) && bool(registry.present_all(handles, camera)), "first frame");
        backend.flush();
        bool stale = false;
        for (int frame = 0; frame < 8; ++frame) {
            if (throwing) backend.throw_at = backend.prepares.load() + frame % 3;
            {
                const auto scope = registry.defer_uploads();
                expect(bool(registry.advance_all(handles, 1.0F / 30.0F, camera, stats)), "scope advance");
                expect(bool(registry.present_all(handles, camera)), "scope present");
                executor.failing = !throwing;
            }
            executor.failing = false;
            backend.throw_at = -1;
            backend.flush();
            for (const auto& entry : backend.slots) {
                if (entry.second.in_live && entry.second.gpu_hash != entry.second.committed_hash) stale = true;
            }
        }
        return stale;
    };
    expect(!play(true, false), "after an exit whose run packed everything and then failed, the group holds the committed streams");
    expect(!play(true, true), "after an exit whose pack throws, the group holds the committed streams");
    expect(play(false, false), "control: comparing with the previous pack leaves the group stale after such an exit");
}

// D-ss: the per-ship decision to drop the historical catch-up pass. Presentation data only.
void test_catchup_screen_space_decision() {
    using namespace particles;
    // Camera at the origin: right = +x, up = +y, so forward = up x right = -z.
    CameraFrame camera;
    camera.position = {0.0F, 0.0F, 0.0F};
    camera.right = {1.0F, 0.0F, 0.0F};
    camera.up = {0.0F, 1.0F, 0.0F};
    const auto view = catchup_view(camera, 90.0F, 1000U, 1000U, 1.0F);
    expect(std::fabs(view.forward.z + 1.0F) < 1.0e-6F && std::fabs(view.forward.x) < 1.0e-6F, "forward is up x right");
    expect(std::fabs(view.focal_pixels - 500.0F) < 1.0e-3F, "a 90 degree, 1000 row view has a 500 px focal length");
    const auto near_equal = [](const float left, const float right) { return std::fabs(left - right) < 1.0e-4F; };

    // Truth table of the error: (chord * turn + lever * turn^2) / 8, in pixels at the nearest depth.
    expect(catchup_error_pixels(0.0F, 0.0F, 0.0F, 100.0F, 500.0F) == 0.0F, "no motion, no error");
    expect(catchup_error_pixels(100.0F, 0.0F, 0.0F, 100.0F, 500.0F) == 0.0F, "a straight line is exact");
    expect(catchup_error_pixels(0.0F, 0.0F, 50.0F, 100.0F, 500.0F) == 0.0F, "no turn, no lever error");
    expect(near_equal(catchup_error_pixels(8.0F, 0.5F, 0.0F, 100.0F, 500.0F), 2.5F), "8 units x 0.5 rad / 8 at depth 100 = 0.5 world = 2.5 px");
    expect(near_equal(catchup_error_pixels(0.0F, 0.5F, 8.0F, 100.0F, 500.0F), 0.125F * 0.25F * 8.0F * 5.0F), "the lever term is lever * turn^2 / 8");
    expect(catchup_error_pixels(10.0F, 0.1F, 0.0F, 0.0F, 500.0F) == std::numeric_limits<float>::infinity(), "no depth keeps the pass");
    expect(catchup_error_pixels(10.0F, 0.1F, 0.0F, -5.0F, 500.0F) == std::numeric_limits<float>::infinity(), "behind the camera keeps the pass");
    expect(near_equal(catchup_error_pixels(10.0F, 0.2F, 0.0F, 100.0F, 500.0F), 2.0F * catchup_error_pixels(10.0F, 0.1F, 0.0F, 100.0F, 500.0F)),
        "the chord term is linear in the turn");
    expect(near_equal(catchup_error_pixels(10.0F, 0.1F, 0.0F, 50.0F, 500.0F), 2.0F * catchup_error_pixels(10.0F, 0.1F, 0.0F, 100.0F, 500.0F)),
        "half the depth doubles the error");

    // The decision: enter at 0.5 px, leave below 0.25 px, culled never, NaN keeps.
    expect(!catchup_keeps_historical_pass(false, 0.49F, false) && catchup_keeps_historical_pass(false, 0.5F, false), "enter at 0.5 px");
    expect(catchup_keeps_historical_pass(true, 0.25F, false) && !catchup_keeps_historical_pass(true, 0.2499F, false), "leave below 0.25 px");
    expect(catchup_keeps_historical_pass(true, 0.4F, false) && !catchup_keeps_historical_pass(false, 0.4F, false),
        "between the bounds the last decision holds");
    expect(!catchup_keeps_historical_pass(true, 99.0F, true) && !catchup_keeps_historical_pass(false, 99.0F, true), "culled takes one pass");
    expect(catchup_keeps_historical_pass(false, std::numeric_limits<float>::quiet_NaN(), false)
        && catchup_keeps_historical_pass(true, std::numeric_limits<float>::quiet_NaN(), false), "a NaN error keeps the pass");
    expect(catchup_keeps_historical_pass(false, std::numeric_limits<float>::infinity(), false), "an infinite error keeps the pass");
    {   // Hysteresis over a sequence: no flicker inside the band.
        bool per_tick = false;
        std::size_t switches = 0;
        for (const float error : {0.1F, 0.4F, 0.6F, 0.4F, 0.3F, 0.4F, 0.2F, 0.4F, 0.1F}) {
            const bool next = catchup_keeps_historical_pass(per_tick, error, false);
            switches += next != per_tick;
            per_tick = next;
        }
        expect(switches == 2 && !per_tick, "0.1 .4 .6 .4 .3 .4 .2 .4 .1 px switches on at .6 and off at .2 only");
    }

    // Camera moves: the same motion crosses the threshold in the same frame when the camera comes closer or zooms.
    const Vec3 ship{0.0F, 0.0F, -400.0F};
    const float chord = 12.0F, turn = 0.2F;
    const auto far_error = catchup_error_pixels(chord, turn, 0.0F, catchup_project(view, ship, 10.0F).depth, view.focal_pixels);
    CameraFrame close = camera;
    close.position = {0.0F, 0.0F, -300.0F};
    const auto close_view = catchup_view(close, 90.0F, 1000U, 1000U, 1.0F);
    const auto near_error = catchup_error_pixels(chord, turn, 0.0F, catchup_project(close_view, ship, 10.0F).depth, close_view.focal_pixels);
    expect(far_error < catchup_enter_pixels && near_error >= catchup_enter_pixels, "moving the camera closer flips the same motion over the threshold");
    const auto narrow = catchup_view(camera, 20.0F, 1000U, 1000U, 1.0F);
    expect(narrow.focal_pixels > 4.0F * view.focal_pixels, "a narrower field of view raises the pixels per unit");
    const auto zoom_error = catchup_error_pixels(chord, turn, 0.0F, catchup_project(narrow, ship, 10.0F).depth, narrow.focal_pixels);
    expect(zoom_error > catchup_enter_pixels, "zooming in flips the same motion over the threshold");

    // Projection: depth is that of the sphere's nearest point, clamped at the near plane; culled outside the sides.
    expect(near_equal(catchup_project(view, ship, 10.0F).depth, 390.0F), "depth is the nearest point of the bounding sphere");
    expect(catchup_project(view, {0.0F, 0.0F, -5.0F}, 50.0F).depth == view.near_plane, "a camera inside the sphere clamps to the near plane");
    expect(!catchup_project(view, ship, 10.0F).culled, "a ship in front is not culled");
    expect(catchup_project(view, {0.0F, 0.0F, 400.0F}, 10.0F).culled, "a ship behind the camera is culled");
    expect(!catchup_project(view, {0.0F, 0.0F, 5.0F}, 50.0F).culled, "a large ship straddling the camera is not culled");
    expect(catchup_project(view, {1000.0F, 0.0F, -400.0F}, 10.0F).culled, "far to the right is culled");
    expect(!catchup_project(view, {405.0F, 0.0F, -400.0F}, 10.0F).culled, "just inside the right edge with its radius is kept");
    expect(catchup_project(view, {0.0F, -1000.0F, -400.0F}, 10.0F).culled, "far below is culled");
    expect(!catchup_project(view, {1000.0F, 0.0F, -400.0F}, 700.0F).culled, "a huge ship whose bound reaches the view is kept");

    // The per-ship decision and the census (the viewer glue only gathers these inputs).
    {
        CatchupCensus census;
        CatchupShip fighter{{0.0F, 0.0F, -400.0F}, 10.0F, 0.0F, 0.0F, false};
        auto decision = decide_catchup_ship(view, fighter, 2);
        census.record(decision);
        expect(!decision.keep && !decision.culled && decision.error_pixels == 0.0F, "a straight, steady ship takes one pass");
        fighter.chord_per_tick = 6.0F;
        fighter.turn_per_tick = 0.1F;   // over 2 ticks: chord 12, turn 0.2: sagitta 0.3 units
        decision = decide_catchup_ship(view, fighter, 2);
        census.record(decision);
        expect(near_equal(decision.error_pixels, catchup_error_pixels(12.0F, 0.2F, 10.0F, 390.0F, view.focal_pixels)),
            "the error scales chord and turn by the ticks owed");
        expect(decision.keep == (decision.error_pixels >= catchup_enter_pixels), "the first decision enters at the threshold");
        fighter.per_tick = true;
        fighter.chord_per_tick = 3.0F;
        fighter.turn_per_tick = 0.05F;
        decision = decide_catchup_ship(view, fighter, 2);
        census.record(decision);
        CatchupShip away{{5000.0F, 0.0F, -100.0F}, 10.0F, 50.0F, 1.0F, true};
        decision = decide_catchup_ship(view, away, 2);
        census.record(decision);
        expect(decision.culled && !decision.keep, "a culled ship takes one pass whatever it does");
        census.unknown = 2;
        census.frames = 1;
        expect(census.decisions() == 4 + 2 && census.binned() == census.per_tick + census.single_pass,
            "the decision counters sum to the ships seen and the bins to the ships that were not culled");
        expect(census.culled == 1, "one culled ship was counted");
    }

    // Heading change from two fixed-point placements.
    const auto fixed = [](const double value) {
        return eawr::sim::math::Fixed::from_raw(static_cast<std::int64_t>(std::llround(value * static_cast<double>(eawr::sim::math::Fixed::scale))));
    };
    const auto yaw = [&](const double angle, const double scale) {
        eawr::sim::math::Mat3x4 matrix;
        matrix.rows[0] = {fixed(scale * std::cos(angle)), fixed(-scale * std::sin(angle)), fixed(0.0), fixed(7.0)};
        matrix.rows[1] = {fixed(scale * std::sin(angle)), fixed(scale * std::cos(angle)), fixed(0.0), fixed(0.0)};
        matrix.rows[2] = {fixed(0.0), fixed(0.0), fixed(scale), fixed(0.0)};
        return matrix;
    };
    expect(catchup_turn_radians(yaw(0.0, 1.0), yaw(0.0, 1.0)) < 1.0e-3F, "the same placement does not turn");
    expect(std::fabs(catchup_turn_radians(yaw(0.0, 1.0), yaw(0.3, 1.0)) - 0.3F) < 1.0e-3F, "a 0.3 rad yaw is read as 0.3 rad");
    expect(std::fabs(catchup_turn_radians(yaw(0.0, 5.0), yaw(0.3, 5.0)) - 0.3F) < 1.0e-3F, "a uniform scale does not matter");
    expect(std::fabs(catchup_turn_radians(yaw(0.3, 1.0), yaw(0.0, 1.0)) - 0.3F) < 1.0e-3F, "the angle is unsigned");
}

} // namespace particle_render_contracts
