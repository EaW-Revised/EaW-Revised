#include "render_internal.hpp"
#include "eawr/presentation/particles/prewarmed_capacity.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstring>
#include <thread>
#include <type_traits>
#include <limits>
#include <new>
#include <ostream>
#include <stdexcept>
#include <utility>

// Quad construction follows the MIT-licensed alo-viewer revision
// 9bb0053919cc5df8377610d4f91b11d956d6c2f4 (DirectX9/ParticleRenderers.cpp):
// corner order, the 0-1-2 / 2-1-3 index pattern, the texture-coordinate
// assignment. Legacy kite geometry follows MD-07. Blend, depth-write and phase policy
// are taken from the render state of the public Engine/Prim* effects that the
// legacy selector names; see docs/reports/P1-08-rendering.md.

namespace eawr::presentation::particles {
using namespace render_detail;
namespace {
using TimingClock = std::chrono::steady_clock;
std::atomic<bool> default_stream_bucketing{true};
std::atomic<bool> default_catchup_folding{true};
std::atomic<bool> default_clean_present{true};
std::atomic<std::size_t> default_slice_cap{StepExecutor::max_ordered};
// Exact bit equality: a value that merely compares equal (-0 and +0) may still build another stream.
template<class T> bool same_bits(const T& left, const T& right) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    return std::memcmp(&left, &right, sizeof(T)) == 0;
}
bool same_camera(const CameraFrame& left, const CameraFrame& right) noexcept {
    return same_bits(left.position, right.position) && same_bits(left.right, right.right) &&
           same_bits(left.up, right.up) && left.collect_axis_diagnostics == right.collect_axis_diagnostics;
}
bool same_culling(const CullingFrame& left, const CullingFrame& right) noexcept {
    return left.valid == right.valid && left.enabled == right.enabled && same_bits(left.planes, right.planes);
}
bool same_emitter_frame(const EmitterFrame& left, const EmitterFrame& right) noexcept {
    return same_bits(left.origin, right.origin) && same_bits(left.basis, right.basis);
}
struct Measure final {
    explicit Measure(double* destination):destination_(destination),start_(destination?TimingClock::now():TimingClock::time_point{}){}
    ~Measure(){if(destination_)*destination_+=std::chrono::duration<double,std::milli>(TimingClock::now()-start_).count();}
    double* destination_;
    TimingClock::time_point start_;
};
}

struct EffectRegistry::LayoutTelemetry final {
    std::shared_ptr<const SystemDefinition> definition;
    std::vector<EmitterCapacity> capacities;
    std::vector<std::size_t> retired_dropped;
    std::size_t released{};
};

struct EffectRegistry::Instance final {
    Instance(EffectHandle id, SystemDefinition definition, const std::uint32_t seed,
             const std::size_t capacity, std::optional<MeshBinding> mesh_binding)
        : handle(id), leave_particles(definition.leave_particles), plans(plan_system(definition)),
          cpu(std::move(definition), seed, capacity, std::move(mesh_binding)), streams(plans.size()),
          particle_buckets(plans.size()) {
        bucket_particles = default_stream_bucketing.load(std::memory_order_relaxed) && std::count_if(plans.begin(), plans.end(), [](const auto& plan) {
            return plan.drawable;
        }) > 1;
        for (std::size_t index = 0; index < plans.size(); ++index) {
            if (!plans[index].drawable) continue;
            const auto count = cpu.emitter_capacities()[index].reserved;
            streams[index].vertices.reserve(count * (plans[index].triangles ? 3U : 4U));
            streams[index].indices.reserve(count * (plans[index].triangles ? 3U : 6U));
            if (plans[index].sort_particles || plans[index].legacy_slot_order)
                streams[index].sorted_particles.reserve(count);
            if (bucket_particles) particle_buckets[index].reserve(count);
        }
    }

    EffectHandle handle{};
    std::shared_ptr<const SystemDefinition> admitted_definition;
    std::size_t charged_bytes{};
    LayoutTelemetry* telemetry{};
    bool leave_particles{};
    std::vector<EmitterRenderPlan> plans;
    std::vector<std::uint64_t> resources;
    CpuSystem cpu;
    // One stream per plan, kept between frames so a grown stream allocates nothing (#439); the
    // batched calls build them on the executor and upload them afterwards (#638).
    std::vector<VertexStream> streams;
    // Dense worker-produced render set; the caller commits it in authored plan order.
    std::vector<std::size_t> render_active;
    std::vector<bool> backend_empty;
    std::vector<bool> backend_terminal;
    // Disjoint instance-local scratch: one scan replaces one full-system scan
    // per drawable emitter. Pointers keep particle bits and input order intact.
    std::vector<std::vector<const Particle*>> particle_buckets;
    bool bucket_particles{};
    std::uint64_t bucket_visits{}, stream_visits{};
    float brightness{1.0F};
    // Dirty tracking for present: the streams built last are exactly what a build now would
    // produce while nothing below changed (EffectRegistry::set_clean_present_default).
    bool dirty{true};
    bool frame_dirty{};
    bool skipped{};
    bool frame_known{}, mesh_known{}, dynamic_known{};
    EmitterFrame frame_set{};
    MeshFrame mesh_set{};
    float dynamic_size{}, dynamic_energy{};
    std::uint64_t built_environment{~std::uint64_t{0}};
    bool visible{true};
    bool prepared{true};
    bool pending_upload{};
    bool stepped{};
    bool deferred{};
    bool updated{};
    bool was_rendered{};
    bool backend_visible{true};
    bool exhausted{};
    float accumulated{};
    double step_ms{}, build_ms{};
    StreamBuildTimings stream_timing;
    double packing_ms{};
    bool measured_on_main{};
};

EffectRegistry::EffectRegistry(RenderBackend& backend)
    : backend_(&backend), clean_present_(default_clean_present.load(std::memory_order_relaxed)) {}

namespace {
// One slot per task: 0 not done, 2 done, 3 failed. The builders claim tasks in order, so the
// earliest finish first and the calling thread rarely has to wait.
struct OrderedRun final {
    const std::function<void(std::size_t)>* task;
    const std::function<void(std::size_t)>* consume;
    std::size_t count;
    std::thread::id caller;
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    std::array<std::atomic<std::uint8_t>, StepExecutor::max_ordered> state{};
    std::size_t consumed{};

    void build(const std::size_t index) {
        bool ok = false;
        try {
            (*task)(index);
            ok = true;
        } catch (...) {
        }
        if (!ok) failed.store(true, std::memory_order_relaxed);
        state[index].store(ok ? 2 : 3, std::memory_order_release);
        state[index].notify_all();
    }

    // The calling thread commits in order; when the next task is not done it builds one that
    // no thread has claimed, and waits only when every task is claimed.
    void commit() {
        while (consumed < count) {
            std::uint8_t seen = state[consumed].load(std::memory_order_acquire);
            while (seen < 2) {
                const std::size_t claim = next.fetch_add(1, std::memory_order_relaxed);
                if (claim < count) build(claim);
                else state[consumed].wait(seen, std::memory_order_acquire);
                seen = state[consumed].load(std::memory_order_acquire);
            }
            if (seen == 3) return;
            // A throwing commit fails the run and ends the commits (nothing is retried): the
            // executor's job wrapper would otherwise catch it and call commit() again.
            try {
                (*consume)(consumed);
            } catch (...) {
                failed.store(true, std::memory_order_relaxed);
                consumed = count;
                return;
            }
            ++consumed;
        }
    }

    // A builder that is not the caller: claims tasks until none is left; returns how many it built.
    std::size_t build_claims() {
        std::size_t built = 0;
        for (std::size_t claim = next.fetch_add(1, std::memory_order_relaxed); claim < count;
             claim = next.fetch_add(1, std::memory_order_relaxed)) {
            build(claim);
            ++built;
        }
        return built;
    }

    void work() {
        if (std::this_thread::get_id() == caller) {
            commit();
            return;
        }
        static_cast<void>(build_claims());
    }

    // HelpBoard::Entry::work: a thread outside the executor (PF lane-serial (a)).
    static std::size_t help(void* context) { return static_cast<OrderedRun*>(context)->build_claims(); }
};
} // namespace

bool StepExecutor::run_ordered(const std::size_t count, const std::function<void(std::size_t)>& task,
    const std::function<void(std::size_t)>& consume) const {
    if (count > max_ordered) return false;
    OrderedRun shared{.task = &task, .consume = &consume, .count = count, .caller = std::this_thread::get_id()};
    OrderedRun* const pointer = &shared;
    // Published for the run's whole length; retracted (no helper inside any more) before `shared` dies.
    HelpBoard* const board = help_board();
    const HelpBoard::Entry entry{.work = &OrderedRun::help, .context = pointer};
    struct Retract final {
        HelpBoard* board;
        ~Retract() { if (board != nullptr) board->retract(); }
    } retract{board};
    if (board != nullptr) board->publish(&entry);
    const bool ran = run(std::max<std::size_t>(participants(), 1), [pointer](const std::size_t) { pointer->work(); });
    // An executor that gave the caller no job leaves the commits to it, every task being done.
    if (ran && !shared.failed.load(std::memory_order_relaxed) && shared.consumed < shared.count) shared.commit();
    return ran && !shared.failed.load(std::memory_order_relaxed) && shared.consumed == shared.count;
}

void EffectRegistry::set_clean_present_default(const bool on) noexcept {
    default_clean_present.store(on, std::memory_order_relaxed);
}

void EffectRegistry::set_slice_cap_default(const std::size_t slices) noexcept {
    default_slice_cap.store(std::clamp<std::size_t>(slices, 1, StepExecutor::max_ordered), std::memory_order_relaxed);
}

void EffectRegistry::observe_environment(const CameraFrame& camera) {
    culling_ = backend_->culling_frame();
    // A backend that could not finish an upload asks for the same stream again.
    const std::uint64_t incomplete = backend_->incomplete_updates();
    const bool retry = incomplete != incomplete_updates_;
    incomplete_updates_ = incomplete;
    if (!retry && environment_known_ && same_camera(camera, environment_camera_) && same_culling(culling_, environment_culling_)) return;
    environment_known_ = true;
    environment_camera_ = camera;
    environment_culling_ = culling_;
    ++environment_;
}

EffectRegistry::FrameUploads::FrameUploads(EffectRegistry& registry, const bool enabled) noexcept
    : registry_(registry), enabled_(enabled) {
    if (enabled_) ++registry_.upload_scopes_;
}

EffectRegistry::FrameUploads::~FrameUploads() {
    if (!enabled_ || --registry_.upload_scopes_ != 0) return;
    // A destructor must not throw: an instance that cannot be packed or committed is rebuilt and
    // uploaded by its next present.
    try {
        static_cast<void>(registry_.commit_pending_uploads());
    } catch (...) {
        registry_.abandon_pending_uploads();
    }
}

core::Result<void> EffectRegistry::commit_pending_uploads() {
    batch_.clear();
    for (const auto& instance : instances_) {
        if (instance->pending_upload) batch_.push_back(instance.get());
    }
    auto outcome = [this]() -> core::Result<void> {
        if (batch_.empty()) return core::Result<void>::success();
        measurement_caller_ = measure_ ? std::this_thread::get_id() : std::thread::id{};
        return run_batch([this](const std::size_t index) {
            Instance& instance = *batch_[index];
            instance.measured_on_main = measure_ && std::this_thread::get_id() == measurement_caller_;
            pack_pending(instance);
        }, [this](const std::size_t index) { finish_pending(*batch_[index]); });
    }();
    // A failed or partial run leaves instances parked: pack and commit them here, in order.
    for (Instance* const instance : batch_) {
        if (!instance->pending_upload) continue;
        instance->measured_on_main = measure_;
        try {
            pack_pending(*instance);
        } catch (...) {
            instance->pending_upload = false;
            instance->dirty = true;
            continue;
        }
        finish_pending(*instance);
    }
    batch_.clear();
    return outcome;
}

void EffectRegistry::pack_pending(Instance& instance) const {
    instance.packing_ms = 0;
    // build() returned early for an instance that was not prepared, and an exhausted instance's
    // streams were reset after its last build: neither has bytes to pack. (An eager pass packed the
    // exhausted stream before the reset; the commit's update(empty) takes the backend's empty path
    // either way, so the drawn result is the same.)
    if (!instance.prepared) return;
    for (const std::size_t index : instance.render_active) {
        const VertexStream& stream = instance.streams[index];
        if (instance.resources[index] == 0U || stream.indices.empty()) continue;
        Measure timer(measure_ ? &instance.packing_ms : nullptr);
        backend_->prepare_emitter(instance.resources[index], stream);
    }
}

void EffectRegistry::finish_pending(Instance& instance) noexcept {
    instance.pending_upload = false;
    if (measure_) {
        auto& stages = instance.measured_on_main ? timings_.main_stages_ms : timings_.worker_stages_ms;
        stages[4] += instance.packing_ms;
    }
    try {
        commit_upload(instance);
    } catch (...) {
        instance.dirty = true;
    }
}

void EffectRegistry::abandon_pending_uploads() noexcept {
    for (const auto& instance : instances_) {
        if (!instance->pending_upload) continue;
        instance->pending_upload = false;
        instance->dirty = true;
    }
}

void EffectRegistry::set_catchup_folding_default(const bool on) noexcept {
    default_catchup_folding.store(on, std::memory_order_relaxed);
}

bool EffectRegistry::catchup_folding_default() noexcept {
    return default_catchup_folding.load(std::memory_order_relaxed);
}

void EffectRegistry::set_stream_bucketing_default(const bool on) noexcept {
    default_stream_bucketing.store(on, std::memory_order_relaxed);
}

EffectRegistry::~EffectRegistry() {
    static_cast<void>(finish_advance_all());
    while (!instances_.empty()) static_cast<void>(release(instances_.back()->handle));
    evict_idle();
}

void EffectRegistry::evict_idle() noexcept {
    for (const auto& instance : idle_) {
        budget_->release(ResourceKind::cpu_effect, instance->charged_bytes);
        admission_.idle_bytes -= instance->charged_bytes;
        ++admission_.evicted;
    }
    idle_.clear();
}

void EffectRegistry::write_admission_report(std::ostream& output) const {
    output << "{\"eligible\":" << admission_.eligible << ",\"admitted\":" << admission_.admitted
        << ",\"refused\":" << admission_.refused << ",\"wanted_live_including_drains\":" << instances_.size()
        << ",\"reused\":" << admission_.reused << ",\"evicted\":" << admission_.evicted
        << ",\"cpu_allocations\":" << admission_.allocations << ",\"cpu_live_payload_bytes\":" << admission_.live_bytes
        << ",\"cpu_idle_payload_bytes\":" << admission_.idle_bytes << ",\"cpu_high_water_payload_bytes\":" << admission_.high_water_bytes
        << ",\"requested_payload_bytes\":" << admission_.requested_bytes << ",\"refused_payload_bytes\":" << admission_.refused_bytes;
    const auto cpu = budget_->snapshot(ResourceKind::cpu_effect);
    const auto gpu = budget_->snapshot(ResourceKind::gpu_proxy);
    output << ",\"cpu_limit_bytes\":";
    if (cpu.limit_bytes) output << *cpu.limit_bytes; else output << "null";
    output << ",\"cpu_probe_available\":" << (cpu.probe_available ? "true" : "false")
        << ",\"gpu_probe_available\":" << (gpu.probe_available ? "true" : "false")
        << ",\"gpu_reserved_bytes\":";
    if (gpu.probe_available) output << gpu.reserved_bytes; else output << "null";
    output << ",\"refusal_reasons\":{\"budget_pressure\":" << admission_.refusals[1]
        << ",\"allocation_failed\":" << admission_.refusals[2] << ",\"definition_failure\":" << admission_.refusals[3]
        << ",\"arithmetic_failure\":" << admission_.refusals[4]
        << "},\"allocation_failure_stages\":{\"cpu\":" << admission_.allocation_failure_stages[1]
        << ",\"backend\":" << admission_.allocation_failure_stages[2]
        << ",\"publication\":" << admission_.allocation_failure_stages[3] << "},\"emitters\":[";
    bool first = true;
    for (const auto& instance : instances_) {
        const auto capacities = instance->cpu.emitter_capacities();
        for (std::size_t index = 0; index < capacities.size(); ++index) {
            const auto& capacity = capacities[index];
            const auto live = std::count_if(instance->cpu.particles().begin(), instance->cpu.particles().end(),
                [&](const auto& particle) { return particle.emitter_index == index; });
            output << (first ? "" : ",") << "{\"handle\":" << instance->handle << ",\"emitter\":" << index
                << ",\"native_requested\":" << capacity.native_requested << ",\"scheduling_requested\":" << capacity.requested
                << ",\"reserved\":" << capacity.reserved << ",\"live\":" << live
                << ",\"dropped\":" << instance->cpu.emitter_dropped(index) << '}';
            first = false;
        }
    }
    output << "],\"layouts\":[";
    first = true;
    std::size_t layout_index{};
    for (const auto& layout : layout_telemetry_) {
        for (std::size_t index = 0; index < layout->capacities.size(); ++index) {
            const auto& capacity = layout->capacities[index];
            std::size_t live{}, dropped = layout->retired_dropped[index], owners{};
            for (const auto& instance : instances_) if (instance->telemetry == layout.get()) {
                ++owners;
                for (const auto& particle : instance->cpu.particles()) if (particle.emitter_index == index) ++live;
                const auto extra = instance->cpu.emitter_dropped(index);
                dropped += std::min(extra, std::numeric_limits<std::size_t>::max() - dropped);
            }
            output << (first ? "" : ",") << "{\"layout\":" << layout_index << ",\"emitter\":" << index << ",\"native_requested\":" << capacity.native_requested
                << ",\"scheduling_requested\":" << capacity.requested << ",\"reserved_per_instance\":" << capacity.reserved
                << ",\"live_instances\":" << owners << ",\"released_instances\":" << layout->released
                << ",\"live\":" << live << ",\"dropped\":" << dropped << '}';
            first = false;
        }
        ++layout_index;
    }
    output << "]}";
}

EffectRegistry::Admission EffectRegistry::spawn_authored(
    std::shared_ptr<const SystemDefinition> system, const std::uint32_t seed,
    std::optional<MeshBinding> mesh) {
    ++admission_.eligible;
    const auto fail_stage = std::exchange(fail_stage_, AdmissionStage::none);
    std::unique_ptr<Instance> instance;
    std::size_t charged{};
    std::size_t requested{};
    auto stage = AdmissionStage::cpu;
    const auto discard = [&] {
        if (instance) for (const auto resource : instance->resources) if (resource != 0) {
            backend_->destroy_emitter(resource); --live_resources_;
        }
        budget_->release(ResourceKind::cpu_effect, charged);
        instance.reset(); charged = 0;
    };
    const auto reserve_payload = [&](const std::size_t bytes) {
        auto reservation = budget_->reserve(ResourceKind::cpu_effect, bytes);
        if (reservation == ReservationOutcome::budget_pressure) {
            const auto snapshot = budget_->snapshot(ResourceKind::cpu_effect);
            const auto fixed = snapshot.reserved_bytes - std::min(snapshot.reserved_bytes, admission_.idle_bytes);
            // Includes other budget consumers and any provisional charge for
            // this admission. Do not evict if reclaiming all our idle payload
            // still cannot make this request fit.
            if (snapshot.limit_bytes && (fixed > *snapshot.limit_bytes || bytes > *snapshot.limit_bytes - fixed))
                return reservation;
        }
        // Stable oldest-idle eviction; active and draining instances never move.
        while (reservation == ReservationOutcome::budget_pressure && !idle_.empty()) {
            const auto bytes_to_release = idle_.front()->charged_bytes;
            budget_->release(ResourceKind::cpu_effect, bytes_to_release); admission_.idle_bytes -= bytes_to_release;
            idle_.erase(idle_.begin()); ++admission_.evicted;
            reservation = budget_->reserve(ResourceKind::cpu_effect, bytes);
        }
        return reservation;
    };
    const auto refuse = [&](const Refusal reason, const char* message) {
        discard();
        ++admission_.refused; ++admission_.refusals[static_cast<std::size_t>(reason)];
        if (reason == Refusal::allocation_failed) ++admission_.allocation_failure_stages[static_cast<std::size_t>(stage)];
        // The counters retain every refusal; diagnostic text is sampled once
        // per reason so repeated pressure or invalid definitions stay bounded.
        if (admission_.refusals[static_cast<std::size_t>(reason)] == 1) {
            try {
                diagnostics_.push_back({std::string(diagnostic_codes::backend_resource), core::Severity::warning,
                    message, {}, {}, {}, {}});
            } catch (const std::bad_alloc&) { /* Typed refusal survives diagnostic allocation failure. */ }
        }
        admission_.refused_bytes += std::min(requested, std::numeric_limits<std::size_t>::max() - admission_.refused_bytes);
        admission_.requested_bytes += std::min(requested, std::numeric_limits<std::size_t>::max() - admission_.requested_bytes);
        return Admission{0, reason, false, requested, 0, stage};
    };
    try {
        if (!system) return refuse(Refusal::definition_failure, "effect definition is absent");
        // A binding is inert without a mesh emitter; do not retain it in an
        // otherwise reusable CPU layout or let it prevent compatible reuse.
        if (std::none_of(system->emitters.begin(), system->emitters.end(),
            [](const auto& emitter) { return emitter.creator_id == 35; })) mesh.reset();
        if (next_handle_ == std::numeric_limits<EffectHandle>::max())
            return refuse(Refusal::arithmetic_failure, "effect handle generations exhausted");
        // Publication cannot allocate once backend ownership has been acquired.
        if (instances_.size() == instances_.capacity())
            instances_.reserve(instances_.size() < instances_.max_size() / 2 ? (instances_.size() + 1) * 2 : instances_.max_size());
        const auto owners = idle_.size() + instances_.size() + 1;
        if (idle_.capacity() < owners)
            idle_.reserve(owners < idle_.max_size() / 2 ? owners * 2 : idle_.max_size());
        const auto reusable = std::find_if(idle_.begin(), idle_.end(), [&](const auto& entry) {
            return !mesh && entry->admitted_definition == system;
        });
        const bool reused = reusable != idle_.end();
        if (reused) {
            instance = std::move(*reusable); idle_.erase(reusable);
            charged = instance->charged_bytes;
            requested = charged;
            admission_.idle_bytes -= charged;
            instance->cpu.reset(seed);
            instance->handle = next_handle_++;
            instance->brightness = 1; instance->visible = instance->prepared = instance->backend_visible = true;
            instance->pending_upload = instance->stepped = instance->deferred = instance->updated = instance->was_rendered = instance->exhausted = false;
            instance->accumulated = 0; instance->render_active.clear();
            instance->dirty = true; instance->frame_dirty = false; instance->skipped = false;
            instance->frame_known = instance->mesh_known = instance->dynamic_known = false;
            instance->built_environment = ~std::uint64_t{0};
            std::fill(instance->backend_empty.begin(), instance->backend_empty.end(), false);
            std::fill(instance->backend_terminal.begin(), instance->backend_terminal.end(), false);
            for (auto& stream : instance->streams) stream.clear();
            for (auto& bucket : instance->particle_buckets) bucket.clear();
        } else {
            std::size_t vertices{};
            if (mesh) {
                bool surface{};
                for (const auto& emitter : system->emitters)
                    surface |= emitter.creator_id == 35 && emitter.mesh_mode == MeshSpawnMode::random_surface;
                if (!validate_mesh_binding(*mesh, surface ? MeshSpawnMode::random_surface : MeshSpawnMode::random_vertex))
                    return refuse(Refusal::definition_failure, "effect mesh binding is invalid");
                for (const auto& submesh : mesh->geometry.submeshes) {
                    if (submesh.vertices.size() > std::numeric_limits<std::size_t>::max() - vertices)
                        return refuse(Refusal::arithmetic_failure, "effect vertex count overflows");
                    vertices += submesh.vertices.size();
                }
            }
            for (const auto& emitter : system->emitters) if (emitter.creator_id == 35 && !mesh)
                return refuse(Refusal::definition_failure, "effect mesh emitter requires a binding");
            const auto capacity = authored_capacity(*system, vertices);
            if (!capacity) return refuse(capacity.error().message.find("overflow") != std::string::npos
                ? Refusal::arithmetic_failure : Refusal::definition_failure, capacity.error().message.c_str());
            auto plans = plan_system(*system);
            // Render support is per emitter, independent of CPU validity. Keep
            // unsupported parents alive so their drawable children still emit.
            if (std::none_of(plans.begin(), plans.end(), [](const auto& plan) { return plan.drawable; }))
                return refuse(Refusal::definition_failure, "effect has no supported render plan");
            const auto payload = CpuSystem::planned_memory_bytes(*system, capacity.value().emitters);
            if (!payload) return refuse(Refusal::arithmetic_failure, "effect CPU payload size overflows");
            std::size_t wanted = payload.value();
            const bool buckets = default_stream_bucketing.load(std::memory_order_relaxed)
                && std::count_if(plans.begin(), plans.end(), [](const auto& plan) { return plan.drawable; }) > 1;
            for (std::size_t index = 0; index < plans.size(); ++index) {
                const auto& plan = plans[index];
                if (!plan.drawable) continue;
                const auto width = (plan.triangles ? 3U : 4U) * sizeof(ParticleVertex)
                    + (plan.triangles ? 3U : 6U) * sizeof(std::uint32_t)
                    + ((plan.sort_particles || plan.legacy_slot_order) ? sizeof(const Particle*) : 0U)
                    + (buckets ? sizeof(const Particle*) : 0U);
                const auto count = capacity.value().emitters[index].reserved;
                if (count > (std::numeric_limits<std::size_t>::max() - wanted) / width)
                    return refuse(Refusal::arithmetic_failure, "effect stream payload size overflows");
                wanted += count * width;
            }
            requested = wanted;
            const auto reservation = reserve_payload(wanted);
            if (reservation != ReservationOutcome::reserved) return refuse(
                reservation == ReservationOutcome::budget_pressure ? Refusal::budget_pressure : Refusal::arithmetic_failure,
                "effect payload reservation refused");
            charged = wanted;
            if (fail_stage == AdmissionStage::cpu) throw std::bad_alloc{};
            instance = std::make_unique<Instance>(next_handle_++, *system, seed, capacity.value().total, std::move(mesh));
            ++admission_.allocations;
            std::size_t actual = instance->cpu.reserved_memory_bytes();
            std::size_t reserved{};
            for (const auto& entry : instance->cpu.emitter_capacities()) reserved += entry.reserved;
            if (reserved != capacity.value().total)
                return refuse(Refusal::definition_failure, "effect CPU layout does not support the authored reserves");
            for (const auto& stream : instance->streams) actual += stream.vertices.capacity() * sizeof(ParticleVertex)
                + stream.indices.capacity() * sizeof(std::uint32_t) + stream.sorted_particles.capacity() * sizeof(const Particle*);
            for (const auto& bucket : instance->particle_buckets) actual += bucket.capacity() * sizeof(const Particle*);
            if (actual < charged) budget_->release(ResourceKind::cpu_effect, charged - actual);
            else if (actual > charged) {
                const auto extra = reserve_payload(actual - charged);
                if (extra != ReservationOutcome::reserved) return refuse(
                    extra == ReservationOutcome::budget_pressure ? Refusal::budget_pressure : Refusal::arithmetic_failure,
                    "effect actual CPU payload reservation refused");
            }
            charged = actual;
            // Mesh frame state is binding-specific and never enters the reuse list.
            if (!instance->cpu.emitter_capacities().empty()) {
                for (const auto& emitter : system->emitters) if (emitter.creator_id == 35) { plans.clear(); break; }
                if (!plans.empty()) instance->admitted_definition = system;
            }
            instance->charged_bytes = charged;
            instance->resources.assign(instance->plans.size(), 0U);
            instance->backend_empty.assign(instance->plans.size(), false);
            instance->backend_terminal.assign(instance->plans.size(), false);
            const auto layout = std::find_if(layout_telemetry_.begin(), layout_telemetry_.end(), [&](const auto& entry) {
                return entry->definition == system && entry->capacities.size() == capacity.value().emitters.size()
                    && std::equal(entry->capacities.begin(), entry->capacities.end(), capacity.value().emitters.begin(),
                        [](const auto& a, const auto& b) { return a.native_requested == b.native_requested
                            && a.requested == b.requested && a.reserved == b.reserved; });
            });
            if (layout != layout_telemetry_.end()) instance->telemetry = layout->get();
            else {
                auto entry = std::make_unique<LayoutTelemetry>();
                entry->definition = system; entry->capacities = capacity.value().emitters;
                entry->retired_dropped.resize(entry->capacities.size());
                instance->telemetry = entry.get(); layout_telemetry_.push_back(std::move(entry));
            }
        }
        static_cast<void>(instance->cpu.set_detail(detail_));
        if (fail_stage == AdmissionStage::cpu) throw std::bad_alloc{};
        stage = AdmissionStage::backend;
        for (std::size_t index = 0; index < instance->plans.size(); ++index) {
            if (!instance->plans[index].drawable) continue;
            const auto resource = backend_->create_emitter(instance->plans[index]);
            if (resource == 0) return refuse(Refusal::allocation_failed, "effect backend allocation failed");
            instance->resources[index] = resource; ++live_resources_;
        }
        if (fail_stage == AdmissionStage::backend) throw std::bad_alloc{};
        const auto handle = instance->handle;
        stage = AdmissionStage::publication;
        if (fail_stage == AdmissionStage::publication) throw std::bad_alloc{};
        instances_.push_back(std::move(instance));
        admission_.live_bytes += charged;
        admission_.high_water_bytes = std::max(admission_.high_water_bytes, admission_.live_bytes + admission_.idle_bytes);
        ++admission_.admitted; if (reused) ++admission_.reused;
        admission_.requested_bytes += std::min(requested, std::numeric_limits<std::size_t>::max() - admission_.requested_bytes);
        return {handle, Refusal::none, reused, requested, charged};
    } catch (const std::bad_alloc&) {
        return refuse(Refusal::allocation_failed, "effect allocation failed before atomic publication");
    } catch (const std::length_error&) {
        return refuse(Refusal::arithmetic_failure, "effect allocation length overflows");
    }
}

// Handles only grow (spawn appends next_handle_++) and release erases in place, so instances_ is
// sorted by handle: a lookup is a binary search, not a scan (#439: presented per running effect
// every drawn frame).
EffectRegistry::Instance* EffectRegistry::find(const EffectHandle handle) const noexcept {
    const auto found = std::lower_bound(instances_.begin(), instances_.end(), handle,
        [](const auto& instance, const EffectHandle wanted) { return instance->handle < wanted; });
    return found != instances_.end() && (*found)->handle == handle ? found->get() : nullptr;
}

core::Diagnostic EffectRegistry::unknown(const EffectHandle handle) {
    core::Diagnostic diagnostic{std::string(diagnostic_codes::unknown_effect), core::Severity::error,
        "effect handle " + std::to_string(handle) + " is not live", {}, {}, {}, {}};
    diagnostics_.push_back(diagnostic);
    return diagnostic;
}

core::Result<EffectHandle> EffectRegistry::spawn(
    SystemDefinition system, const std::uint32_t seed, const std::size_t max_particles) {
    return spawn_impl(std::move(system),seed,max_particles,std::nullopt);
}

core::Result<EffectHandle> EffectRegistry::spawn(
    SystemDefinition system, const std::uint32_t seed, const std::size_t max_particles,
    MeshBinding mesh_binding) {
    return spawn_impl(std::move(system),seed,max_particles,std::move(mesh_binding));
}

core::Result<EffectHandle> EffectRegistry::spawn_impl(
    SystemDefinition system, const std::uint32_t seed, const std::size_t max_particles,
    std::optional<MeshBinding> mesh_binding) {
    const auto spawn_start=measure_?TimingClock::now():TimingClock::time_point{};
    const auto fail_mesh=[&](const std::string& message){
        core::Diagnostic diagnostic{std::string(diagnostic_codes::mesh_binding),
            core::Severity::error,message,{},{},{},{}};
        diagnostics_.push_back(diagnostic);
        return core::Result<EffectHandle>::failure(std::move(diagnostic));
    };
    bool needs_mesh{},needs_surface{};
    for(std::size_t index=0;index<system.emitters.size();++index){
        const auto& emitter=system.emitters[index];
        if(system.version!=AloParticleVersion::legacy_v1||!emitter.cpu_ready||
           emitter.creator_id!=35)continue;
        needs_mesh=true;
        if((emitter.mesh_mode!=MeshSpawnMode::random_vertex&&
            emitter.mesh_mode!=MeshSpawnMode::random_surface&&
            emitter.mesh_mode!=MeshSpawnMode::every_vertex)||
            !std::isfinite(emitter.mesh_surface_offset))
            return fail_mesh("EnhancedMesh emitter has invalid mode or surface offset");
        needs_surface|=emitter.mesh_mode==MeshSpawnMode::random_surface;
        std::size_t node=index;
        for(std::size_t hops=0;hops<=system.emitters.size();++hops){
            const auto parent=system.emitters[node].parent_emitter;
            if(parent==EmitterDefinition::no_parent)break;
            if(parent>=system.emitters.size()||hops==system.emitters.size())
                return fail_mesh("EnhancedMesh parent metadata contains an invalid or cyclic link");
            node=parent;
        }
    }
    if(needs_mesh&&!mesh_binding)return fail_mesh("EnhancedMesh emitter requires a mesh binding");
    if(mesh_binding){
        const auto checked=validate_mesh_binding(*mesh_binding,
            needs_surface?MeshSpawnMode::random_surface:MeshSpawnMode::random_vertex);
        if(!checked)return fail_mesh(checked.error().message);
        std::size_t vertices{};
        for(const auto& submesh:mesh_binding->geometry.submeshes)vertices+=submesh.vertices.size();
        for(const auto& emitter:system.emitters)if(system.version==AloParticleVersion::legacy_v1&&
            emitter.cpu_ready&&emitter.creator_id==35){
            const long double base=emitter.bursting?
                std::trunc(static_cast<long double>(emitter.particles_per_interval)):1.0L;
            const long double limit=std::ldexp(1.0L,std::numeric_limits<std::size_t>::digits);
            if(!std::isfinite(emitter.particles_per_interval)||base<0||
                base>=limit||
                (emitter.mesh_mode==MeshSpawnMode::every_vertex&&
                 base*vertices>=limit))
                return fail_mesh("EnhancedMesh spawn count overflows");
        }
    }
    if (next_handle_ == std::numeric_limits<EffectHandle>::max()) {
        core::Diagnostic diagnostic{std::string(diagnostic_codes::backend_resource),
            core::Severity::error, "effect handle space is exhausted", {}, {}, {}, {}};
        diagnostics_.push_back(diagnostic);
        return core::Result<EffectHandle>::failure(std::move(diagnostic));
    }
    auto instance = std::make_unique<Instance>(next_handle_++, std::move(system), seed,
        max_particles, std::move(mesh_binding));
    static_cast<void>(instance->cpu.set_detail(detail_));
    const auto backend_start=measure_?TimingClock::now():TimingClock::time_point{};
    if(measure_)timings_.spawn_cpu_ms+=std::chrono::duration<double,std::milli>(backend_start-spawn_start).count();
    Measure backend_time(measure_?&timings_.spawn_backend_ms:nullptr);
    instance->resources.assign(instance->plans.size(), 0U);
    instance->backend_empty.assign(instance->plans.size(), false);
    instance->backend_terminal.assign(instance->plans.size(), false);
    for (std::size_t index = 0; index < instance->plans.size(); ++index) {
        EmitterRenderPlan& plan = instance->plans[index];
        if (!plan.drawable) continue;
        const std::uint64_t resource = backend_->create_emitter(plan);
        if (resource == 0U) {
            // The backend refused (for example a texture that did not resolve
            // or a shader the compiler rejected): the emitter is reported as
            // not drawn with that cause instead of being drawn another way.
            plan.drawable = false;
            const std::string detail = backend_->failure_cause();
            plan.cause = "backend could not create the emitter's resources"
                + (detail.empty() ? std::string{} : ": " + detail);
            diagnostics_.push_back({std::string(diagnostic_codes::backend_resource),
                core::Severity::warning, "emitter " + std::to_string(index) + ": " + plan.cause,
                {}, {}, {}, {}});
            continue;
        }
        instance->resources[index] = resource;
        ++live_resources_;
    }
    const EffectHandle handle = instance->handle;
    instances_.push_back(std::move(instance));
    return core::Result<EffectHandle>::success(handle);
}

core::Result<void> EffectRegistry::set_frame(const EffectHandle handle, const EmitterFrame& frame) {
    Instance* const instance = find(handle);
    if (instance == nullptr) return core::Result<void>::failure(unknown(handle));
    if(!finite(frame.origin)||!finite(frame.basis.x)||!finite(frame.basis.y)||
       !finite(frame.basis.z)){
        core::Diagnostic diagnostic{std::string(diagnostic_codes::invalid_value),
            core::Severity::error,"emitter frame contains a non-finite value",{},{},{},{}};
        diagnostics_.push_back(diagnostic);
        return core::Result<void>::failure(std::move(diagnostic));
    }
    if (!instance->frame_known || !same_emitter_frame(instance->frame_set, frame)) {
        instance->frame_known = true;
        instance->frame_set = frame;
        instance->frame_dirty = true;
    }
    instance->cpu.set_origin(frame.origin);
    instance->cpu.set_basis(frame.basis);
    return core::Result<void>::success();
}

core::Result<void> EffectRegistry::set_mesh_frame(const EffectHandle handle, const MeshFrame& frame) {
    Instance* const instance = find(handle);
    if (instance == nullptr) return core::Result<void>::failure(unknown(handle));
    if (!instance->mesh_known || !same_bits(instance->mesh_set, frame)) {
        instance->mesh_known = true;
        instance->mesh_set = frame;
        instance->dirty = true;
    }
    auto result=instance->cpu.set_mesh_frame(frame);
    if(!result)diagnostics_.push_back(result.error());
    return result;
}

core::Result<void> EffectRegistry::set_detail(ParticleDetail detail) {
    if (!std::isfinite(detail.global) || !std::isfinite(detail.local)) {
        core::Diagnostic diagnostic{std::string(diagnostic_codes::invalid_value), core::Severity::error,
            "particle detail inputs must be finite", {}, {}, {}, {}};
        diagnostics_.push_back(diagnostic);
        return core::Result<void>::failure(std::move(diagnostic));
    }
    detail.global = std::clamp(detail.global, 0.0F, 1.0F);
    detail.local = std::clamp(detail.local, 0.0F, 1.0F);
    if (detail == detail_) return core::Result<void>::success();
    detail_ = detail;
    for (const auto& instance : instances_) {
        instance->dirty = true;
        static_cast<void>(instance->cpu.set_detail(detail));
    }
    return core::Result<void>::success();
}

core::Result<void> EffectRegistry::set_brightness(const EffectHandle handle, const float brightness) {
    Instance* const instance = find(handle);
    if (instance == nullptr) return core::Result<void>::failure(unknown(handle));
    if (!std::isfinite(brightness) || brightness < 0.0F) {
        core::Diagnostic diagnostic{std::string(diagnostic_codes::invalid_value), core::Severity::error,
            "effect brightness must be finite and not negative", {}, {}, {}, {}};
        diagnostics_.push_back(diagnostic);
        return core::Result<void>::failure(std::move(diagnostic));
    }
    if (!same_bits(instance->brightness, brightness)) instance->dirty = true;
    instance->brightness = brightness;
    return core::Result<void>::success();
}

core::Result<void> EffectRegistry::set_dynamic(const EffectHandle handle, const float size, const float energy) {
    Instance* const instance = find(handle);
    if (!instance) return core::Result<void>::failure(unknown(handle));
    if (!instance->dynamic_known || !same_bits(instance->dynamic_size, size) || !same_bits(instance->dynamic_energy, energy)) {
        instance->dynamic_known = true;
        instance->dynamic_size = size;
        instance->dynamic_energy = energy;
        instance->dirty = true;
    }
    return instance->cpu.set_dynamic(size, energy);
}

core::Result<void> EffectRegistry::set_pinched_cylinder(const EffectHandle handle,
    const float radius, const float height, const float fraction) {
    Instance* const instance = find(handle);
    if (!instance) return core::Result<void>::failure(unknown(handle));
    // This changes the definition's shape; its original immutable reuse key is
    // no longer compatible. Ordinary dynamic/frame controls are reset on reuse.
    instance->admitted_definition.reset();
    instance->dirty = true;
    return instance->cpu.set_pinched_cylinder(radius, height, fraction);
}

core::Result<EffectFrameStats> EffectRegistry::advance(
    const EffectHandle handle, const float delta_seconds, const CameraFrame& camera) {
    Instance* const instance = find(handle);
    if (instance == nullptr) return core::Result<EffectFrameStats>::failure(unknown(handle));
    EffectFrameStats stats;
    observe_environment(camera);
    instance->measured_on_main = true;
    instance->cpu.measure_phases(measure_);
    instance->step_ms = instance->build_ms = 0;
    { Measure timer(measure_?&instance->step_ms:nullptr); step(*instance, delta_seconds, stats); }
    { Measure timer(measure_?&instance->build_ms:nullptr); build(*instance, camera, &stats); }
    if (measure_) { timings_.scalar_step_ms += instance->step_ms; timings_.scalar_build_ms += instance->build_ms; }
    upload(*instance, true, stream_hashes_);
    return core::Result<EffectFrameStats>::success(std::move(stats));
}

core::Result<void> EffectRegistry::present(const EffectHandle handle, const CameraFrame& camera) {
    Instance* const instance = find(handle);
    if (instance == nullptr) return core::Result<void>::failure(unknown(handle));
    observe_environment(camera);
    if (clean_present_ && is_clean(*instance)) {
        ++work_.presents_clean;
        return core::Result<void>::success();
    }
    instance->measured_on_main = true;
    instance->cpu.measure_phases(measure_);
    instance->step_ms = instance->build_ms = 0;
    bool skipped = false;
    { Measure timer(measure_?&instance->build_ms:nullptr);
      const std::size_t moved = instance->cpu.follow_emitter();
      if (clean_present_ && moved == 0 && follow_only(*instance)) skipped = true;
      else build(*instance, camera, nullptr); }
    if (measure_) timings_.scalar_build_ms += instance->build_ms;
    if (skipped) {
        instance->frame_dirty = false;
        ++work_.presents_unmoved;
        return core::Result<void>::success();
    }
    upload(*instance, false, false);
    return core::Result<void>::success();
}

core::Result<void> EffectRegistry::advance_all(const std::span<const EffectHandle> handles,
    const float delta_seconds, const CameraFrame& camera, std::vector<EffectFrameStats>& stats) {
    return advance_batch(handles, delta_seconds, {}, camera, stats);
}

core::Result<void> EffectRegistry::advance_all(const std::span<const EffectHandle> handles,
    const std::span<const float> delta_seconds, const CameraFrame& camera, std::vector<EffectFrameStats>& stats) {
    if (delta_seconds.size() != handles.size()) {
        core::Diagnostic diagnostic{std::string(diagnostic_codes::batch), core::Severity::error,
            "the particle batch needs one delta per effect", {}, {}, {}, {}};
        diagnostics_.push_back(diagnostic);
        return core::Result<void>::failure(std::move(diagnostic));
    }
    return advance_batch(handles, 0.0F, delta_seconds, camera, stats);
}

core::Result<void> EffectRegistry::advance_batch(const std::span<const EffectHandle> handles,
    const float uniform_delta, const std::span<const float> deltas, const CameraFrame& camera,
    std::vector<EffectFrameStats>& stats) {
    if (auto resolved = resolve(handles); !resolved) return resolved;
    observe_environment(camera);
    stats.resize(batch_.size());
    measurement_caller_ = measure_ ? std::this_thread::get_id() : std::thread::id{};
    struct Inputs final {
        float uniform_delta;
        std::span<const float> deltas;
        const CameraFrame& camera;
        std::vector<EffectFrameStats>& stats;
    };
    const Inputs inputs{uniform_delta, deltas, camera, stats};
    auto ran = run_batch([this, &inputs](const std::size_t index) {
        advance_task(*batch_[index], inputs.deltas.empty() ? inputs.uniform_delta : inputs.deltas[index], inputs.camera,
            inputs.stats[index], measure_ && std::this_thread::get_id() == measurement_caller_);
    }, [this](const std::size_t index) {
        Instance* const instance = batch_[index];
        if(measure_){timings_.batch_step_cpu_ms+=instance->step_ms;timings_.batch_build_cpu_ms+=instance->build_ms;}
        upload(*instance, true, stream_hashes_);
    });
    return ran;
}

void EffectRegistry::advance_task(Instance& instance, const float delta, const CameraFrame& camera,
    EffectFrameStats& slot, const bool on_caller) const {
    // The slot keeps its per-emitter vector's storage; every field starts over.
    std::vector<EmitterFrameStats> emitters = std::move(slot.emitters);
    emitters.clear();
    slot = EffectFrameStats{};
    slot.emitters = std::move(emitters);
    instance.measured_on_main = on_caller;
    instance.cpu.measure_phases(measure_);
    instance.step_ms=instance.build_ms=0.0;
    { Measure timer(measure_?&instance.step_ms:nullptr); step(instance, delta, slot); }
    { Measure timer(measure_?&instance.build_ms:nullptr); build(instance, camera, &slot); }
}

core::Result<bool> EffectRegistry::begin_advance_all(const std::span<const EffectHandle> handles,
    const std::span<const float> delta_seconds, const CameraFrame& camera, std::vector<EffectFrameStats>& stats) {
    if (async_.active) {
        core::Diagnostic diagnostic{std::string(diagnostic_codes::batch), core::Severity::error,
            "an asynchronous particle batch is still running", {}, {}, {}, {}};
        diagnostics_.push_back(diagnostic);
        return core::Result<bool>::failure(std::move(diagnostic));
    }
    if (delta_seconds.size() != handles.size()) {
        core::Diagnostic diagnostic{std::string(diagnostic_codes::batch), core::Severity::error,
            "the particle batch needs one delta per effect", {}, {}, {}, {}};
        diagnostics_.push_back(diagnostic);
        return core::Result<bool>::failure(std::move(diagnostic));
    }
    // Inside a deferred-upload scope the pack waits for the scope's exit; a backend that does not allow
    // its commit beside the pack may not pack while the caller does other work either.
    if (executor_ == nullptr || handles.size() < 2 || upload_scopes_ != 0 || executor_->participants() < 2
        || !backend_->overlapped_commit_safe()) return core::Result<bool>::success(false);
    if (auto resolved = resolve(handles); !resolved) return core::Result<bool>::failure(resolved.error());
    observe_environment(camera);
    stats.resize(batch_.size());
    async_.batch.swap(batch_);
    batch_.clear();
    async_.deltas.assign(delta_seconds.begin(), delta_seconds.end());
    async_.camera = camera;
    async_.stats = &stats;
    const std::size_t max_tasks = default_slice_cap.load(std::memory_order_relaxed);
    async_.tasks = std::min(async_.batch.size(), max_tasks);
    // The workers have no calling thread to measure: their stages count as worker stages.
    measurement_caller_ = std::thread::id{};
    async_.job = [this](const std::size_t slice) {
        const std::size_t count = async_.batch.size();
        const std::size_t first = count * slice / async_.tasks;
        const std::size_t last = count * (slice + 1) / async_.tasks;
        for (std::size_t index = first; index < last; ++index) {
            advance_task(*async_.batch[index], async_.deltas[index], async_.camera, (*async_.stats)[index], false);
        }
    };
    if (!executor_->start_async(async_.tasks, async_.job)) {
        // Nothing started: hand the instances back so the caller's advance_all resolves them again.
        batch_.swap(async_.batch);
        batch_.clear();
        async_.job = nullptr;
        async_.stats = nullptr;
        return core::Result<bool>::success(false);
    }
    async_.active = true;
    return core::Result<bool>::success(true);
}

core::Result<void> EffectRegistry::finish_advance_all() {
    if (!async_.active) return core::Result<void>::success();
    async_.active = false;
    const bool ran = executor_->join_async();
    ++work_.batches;
    work_.tasks += async_.tasks;
    auto outcome = core::Result<void>::success();
    if (!ran) {
        core::Diagnostic diagnostic{std::string(diagnostic_codes::batch), core::Severity::error,
            "the particle executor could not run a batch of " + std::to_string(async_.batch.size()) + " effects", {}, {}, {}, {}};
        diagnostics_.push_back(diagnostic);
        outcome = core::Result<void>::failure(std::move(diagnostic));
    } else {
        // The commit, in handle order, on the calling thread: what the pipelined commit does one by one.
        for (Instance* const instance : async_.batch) {
            if(measure_){timings_.batch_step_cpu_ms+=instance->step_ms;timings_.batch_build_cpu_ms+=instance->build_ms;}
            upload(*instance, true, stream_hashes_);
        }
    }
    async_.batch.clear();
    async_.job = nullptr;
    async_.stats = nullptr;
    return outcome;
}

core::Result<void> EffectRegistry::present_all(const std::span<const EffectHandle> handles, const CameraFrame& camera) {
    if (auto resolved = resolve(handles); !resolved) return resolved;
    observe_environment(camera);
    if (clean_present_) {
        // A clean instance keeps the streams (and backend state) of its last build; only the
        // others are built and uploaded, in the order they were given.
        const auto kept = std::remove_if(batch_.begin(), batch_.end(), [this](const Instance* instance) {
            return is_clean(*instance);
        });
        work_.presents_clean += static_cast<std::uint64_t>(batch_.end() - kept);
        batch_.erase(kept, batch_.end());
    }
    measurement_caller_ = measure_ ? std::this_thread::get_id() : std::thread::id{};
    auto ran = run_batch([this, &camera](const std::size_t index) {
        Instance& instance = *batch_[index];
        instance.measured_on_main = measure_ && std::this_thread::get_id() == measurement_caller_;
        instance.cpu.measure_phases(measure_);
        instance.step_ms=instance.build_ms=0.0;
        Measure timer(measure_?&instance.build_ms:nullptr);
        instance.skipped = false;
        const std::size_t moved = instance.cpu.follow_emitter();
        // The frame moved but nothing lives in it: the streams of the last build still stand.
        if (clean_present_ && moved == 0 && follow_only(instance)) {
            instance.skipped = true;
            return;
        }
        build(instance, camera, nullptr);
    }, [this](const std::size_t index) {
        Instance* const instance = batch_[index];
        if(measure_){timings_.batch_step_cpu_ms+=instance->step_ms;timings_.batch_build_cpu_ms+=instance->build_ms;}
        if (instance->skipped) {
            instance->skipped = false;
            instance->frame_dirty = false;
            ++work_.presents_unmoved;
            return;
        }
        upload(*instance, false, false);
    });
    return ran;
}

core::Result<void> EffectRegistry::resolve(const std::span<const EffectHandle> handles) {
    batch_.clear();
    batch_.reserve(handles.size());
    for (const EffectHandle handle : handles) {
        Instance* const instance = find(handle);
        if (instance == nullptr) {
            batch_.clear();
            return core::Result<void>::failure(unknown(handle));
        }
        batch_.push_back(instance);
    }
    // Two tasks on one instance would race: a repeated handle fails the batch before any step.
    sorted_.assign(batch_.begin(), batch_.end());
    std::sort(sorted_.begin(), sorted_.end(), [](const Instance* left, const Instance* right) {
        return left->handle < right->handle;
    });
    const auto repeated = std::adjacent_find(sorted_.begin(), sorted_.end());
    if (repeated != sorted_.end()) {
        core::Diagnostic diagnostic{std::string(diagnostic_codes::batch), core::Severity::error,
            "effect handle " + std::to_string((*repeated)->handle) + " appears twice in one batch", {}, {}, {}, {}};
        diagnostics_.push_back(diagnostic);
        batch_.clear();
        return core::Result<void>::failure(std::move(diagnostic));
    }
    return core::Result<void>::success();
}

core::Result<void> EffectRegistry::run_batch(const std::function<void(std::size_t)>& task,
    const std::function<void(std::size_t)>& consume) {
    const std::size_t count = batch_.size();
    if (executor_ == nullptr || count < 2) {
        for (std::size_t index = 0; index < count; ++index) task(index);
        for (std::size_t index = 0; index < count; ++index) consume(index);
        return core::Result<void>::success();
    }
    // Contiguous slices of the batch in order, at most StepExecutor::max_ordered of them, which the pool's
    // workers claim; no two slices share an instance.
    const std::size_t max_tasks = default_slice_cap.load(std::memory_order_relaxed);
    const std::size_t tasks = std::min(count, max_tasks);
    batch_task_count_ = tasks;
    const auto start=measure_?TimingClock::now():TimingClock::time_point{};
    caller_work_ms_ = 0.0;
    const auto slice_range = [this](const std::size_t slice) {
        return std::pair<std::size_t, std::size_t>{batch_.size() * slice / batch_task_count_,
            batch_.size() * (slice + 1) / batch_task_count_};
    };
    // The commit overlaps the builds only when the executor has threads to build with and the
    // backend can take an update while it prepares other resources.
    const bool pipelined = executor_->participants() > 1 && backend_->overlapped_commit_safe();
    // The executor joins before returning, so it can refer to these stack callbacks.  Wrapping
    // them by reference avoids allocating a std::function for their three-pointer captures.
    const auto build_slice = [this, &task, &slice_range](const std::size_t slice) {
        Measure caller_timer(measure_&&std::this_thread::get_id()==measurement_caller_?&caller_work_ms_:nullptr);
        const auto [first, last] = slice_range(slice);
        for (std::size_t index = first; index < last; ++index) task(index);
    };
    bool ran = false;
    if (pipelined) {
        const auto consume_slice = [this, &consume, &slice_range](const std::size_t slice) {
            // The commits run on the caller: they are its work, not waiting for the pool.
            Measure caller_timer(measure_&&std::this_thread::get_id()==measurement_caller_?&caller_work_ms_:nullptr);
            const auto [first, last] = slice_range(slice);
            for (std::size_t index = first; index < last; ++index) consume(index);
        };
        ran = executor_->run_ordered(tasks, std::cref(build_slice), std::cref(consume_slice));
        if (ran) ++work_.pipelined_batches;
    } else {
        ran = executor_->run(tasks, std::cref(build_slice));
    }
    if(measure_){
        const double elapsed=std::chrono::duration<double,std::milli>(TimingClock::now()-start).count();
        timings_.executor_ms+=elapsed;
        // Includes dispatch/barrier overhead, not an exclusive OS wait counter.
        timings_.executor_wait_dispatch_ms+=std::max(0.0,elapsed-caller_work_ms_);
    }
    ++work_.batches;
    work_.tasks += tasks;
    if (!ran) {
        core::Diagnostic diagnostic{std::string(diagnostic_codes::batch), core::Severity::error,
            "the particle executor could not run a batch of " + std::to_string(count) + " effects", {}, {}, {}, {}};
        diagnostics_.push_back(diagnostic);
        return core::Result<void>::failure(std::move(diagnostic));
    }
    if (!pipelined) for (std::size_t index = 0; index < count; ++index) consume(index);
    return core::Result<void>::success();
}

void EffectRegistry::begin_measurement(const bool on) noexcept { measure_=on; timings_={}; }

void EffectRegistry::set_executor(const StepExecutor* const executor) noexcept { executor_ = executor; }
void EffectRegistry::set_stream_hashes(const bool on) noexcept { stream_hashes_ = on; }
const RegistryWorkCounts& EffectRegistry::work() const noexcept { return work_; }

void EffectRegistry::step(Instance& instance, const float delta_seconds, EffectFrameStats& stats) const {
    instance.stepped = false;
    instance.deferred = false;
    if (!culling_.enabled) {
        stats.advance = instance.cpu.advance(delta_seconds);
        instance.stepped = true;
        return;
    }
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0F ||
        delta_seconds > std::numeric_limits<float>::max() - instance.accumulated ||
        instance.accumulated + delta_seconds > std::numeric_limits<float>::max() - instance.cpu.presentation_time()) return;
    instance.accumulated += delta_seconds;
    // PS-38: the first update establishes the transform; thereafter successful
    // submission admits the full saved time, otherwise equality at 0.1 s admits.
    // PS-42: zero delta cannot initialize or consume a generation.
    if (instance.updated && !instance.was_rendered && instance.accumulated < 0.1F) {
        instance.deferred = delta_seconds > 0.0F;
        instance.cpu.follow_emitter();
        return;
    }
    instance.was_rendered = false;
    if (instance.updated && (instance.accumulated == 0.0F || instance.cpu.finished())) return;
    stats.advance = instance.cpu.advance(instance.accumulated);
    instance.accumulated = 0.0F;
    instance.updated = true;
    instance.stepped = true;
}

void EffectRegistry::build(Instance& instance, const CameraFrame& camera, EffectFrameStats* const stats) const {
    instance.bucket_visits = instance.stream_visits = 0;
    instance.stream_timing = {};
    instance.packing_ms = 0;
    const std::span<const Particle> live = instance.cpu.particles();
    instance.exhausted = instance.cpu.exhausted();
    const ParticleBounds bounds = particle_bounds(instance.plans, live);
    instance.visible = intersects(bounds, culling_);
    instance.prepared = !culling_.enabled || (instance.visible && !instance.cpu.finished());
    const bool hash = stats != nullptr && stream_hashes_;
    if (stats != nullptr) {
        stats->particles = live.size();
        stats->elapsed_seconds = instance.cpu.presentation_time();
        stats->deferred_seconds = instance.accumulated;
        stats->detached = instance.cpu.detached();
        stats->finished = instance.cpu.finished();
        stats->hash = hash && instance.prepared ? 0xcbf29ce484222325ULL : 0U;
        stats->emitters.resize(instance.plans.size());
        for (const Particle& particle : live) {
            if (particle.emitter_index < stats->emitters.size()) ++stats->emitters[particle.emitter_index].particles;
        }
    }
    if (!instance.prepared) {
        // Finished/empty groups still retire their last submission. Camera or
        // fog rejection alone never proves that retained particles are empty.
        if (live.empty()) {
            instance.render_active.clear();
            for (auto& stream : instance.streams) {
                if (instance.exhausted) stream = VertexStream{};
                else stream.clear();
            }
        }
        if (stats != nullptr) {
            stats->has_bounds = bounds.valid;
            stats->bounds_min = bounds.minimum;
            stats->bounds_max = bounds.maximum;
        }
        return;
    }
    if (instance.bucket_particles) {
        for (auto& bucket : instance.particle_buckets) bucket.clear();
        for (const Particle& particle : live) {
            ++instance.bucket_visits;
            if (particle.emitter_index >= instance.plans.size() || !particle.draw_eligible) continue;
            const auto& plan = instance.plans[particle.emitter_index];
            if (!plan.drawable || (!detail_.heat && plan.phase == DrawPhase::heat)) continue;
            instance.particle_buckets[particle.emitter_index].push_back(&particle);
        }
    }
    instance.render_active.clear();
    for (std::size_t index = 0; index < instance.plans.size(); ++index) {
        VertexStream& stream = instance.streams[index];
        stream.clear();
        // PS-34: heat phase admission is independent of the ordinary slot mask.
        if (instance.plans[index].drawable && (detail_.heat || instance.plans[index].phase != DrawPhase::heat)) {
            if (instance.bucket_particles) {
                const auto& selected = instance.particle_buckets[index];
                instance.stream_visits += selected.size();
                build_stream_subset(instance.plans[index], selected, camera, stream, measure_ ? &instance.stream_timing : nullptr);
            } else {
                instance.stream_visits += live.size();
                build_stream(instance.plans[index], live, camera, stream, measure_ ? &instance.stream_timing : nullptr);
            }
        }
        if (instance.brightness != 1.0F) {
            // PS-23/BP-45: byte channels receive brightness independently.
            // Out-of-range caller brightness saturates as a presentation safety policy.
            for (ParticleVertex& vertex : stream.vertices) {
                const auto channel=[&](const float value){return
                    std::clamp(std::trunc(value*255*instance.brightness),0.0F,255.0F)/255;};
                vertex.color = {channel(vertex.color.x),channel(vertex.color.y),channel(vertex.color.z),channel(vertex.color.w)};
            }
        }
        if (instance.resources[index] != 0U && !stream.indices.empty()) {
            instance.render_active.push_back(index);
            // Inside a deferred-upload scope only the last build of the frame is committed, so its
            // bytes are packed once at the scope's exit (commit_pending_uploads), not in every pass.
            if (upload_scopes_ == 0) {
                Measure timer(measure_ ? &instance.packing_ms : nullptr);
                backend_->prepare_emitter(instance.resources[index], stream);
            }
        }
        // Only terminal CPU lifecycles relinquish reusable stream capacity.
        // A temporary gap keeps it for the next burst, with identical RNG/age.
        if (instance.exhausted) stream = VertexStream{};
        if (stream.vertices.empty()) {
            stream.axis_samples = stream.axis_reversed = 0;
            stream.axis_max_sine_error = 0;
        }
        if (stats == nullptr) continue;
        const bool drawn = instance.resources[index] != 0U;
        EmitterFrameStats& emitter = stats->emitters[index];
        emitter.axis_samples = stream.axis_samples;
        emitter.axis_max_sine_error = stream.axis_max_sine_error;
        emitter.axis_reversed = stream.axis_reversed;
        emitter.quads = stream.quads;
        emitter.triangles = stream.triangles;
        for (const ParticleVertex& vertex : stream.vertices) {
            emitter.maximum_alpha = std::max(emitter.maximum_alpha, vertex.color.w);
        }
        if (hash) {
            emitter.hash = stream_hash(stream);
            stats->hash = stream_hash(stream, stats->hash);
        }
        emitter.drawn = drawn;
        if (drawn && !stream.indices.empty()) {
            if (!stats->has_bounds) {
                stats->bounds_min = stream.bounds_min;
                stats->bounds_max = stream.bounds_max;
                stats->has_bounds = true;
            } else {
                stats->bounds_min = {std::min(stats->bounds_min.x, stream.bounds_min.x),
                    std::min(stats->bounds_min.y, stream.bounds_min.y),
                    std::min(stats->bounds_min.z, stream.bounds_min.z)};
                stats->bounds_max = {std::max(stats->bounds_max.x, stream.bounds_max.x),
                    std::max(stats->bounds_max.y, stream.bounds_max.y),
                    std::max(stats->bounds_max.z, stream.bounds_max.z)};
            }
        }
    }
    if (stats == nullptr) return;
    stats->detached = instance.cpu.detached();
    stats->finished = instance.cpu.finished();
}

bool EffectRegistry::is_clean(const Instance& instance) const noexcept {
    // A frame set after the last follow (a zero-delta advance builds without following) still
    // owes linked particles their move, which only the rebuild in present applies.
    return !instance.dirty && !instance.frame_dirty && instance.built_environment == environment_
        && !instance.pending_upload && instance.cpu.emitter_followed();
}

bool EffectRegistry::follow_only(const Instance& instance) const noexcept {
    return !instance.dirty && instance.frame_dirty && instance.built_environment == environment_
        && !instance.pending_upload;
}

void EffectRegistry::upload(Instance& instance, const bool stepped, const bool hashed) {
    Measure commit_time(measure_?&timings_.commit_ms:nullptr);
    // Every upload follows a build of the state and environment as they stand now.
    instance.dirty = instance.frame_dirty = false;
    instance.built_environment = environment_;
    work_.particle_bucket_visits += instance.bucket_visits;
    work_.stream_particle_visits += instance.stream_visits;
    if (measure_) {
        auto& stages = instance.measured_on_main ? timings_.main_stages_ms : timings_.worker_stages_ms;
        stages[0] += instance.cpu.phase_ms()[0];
        stages[1] += instance.cpu.phase_ms()[1];
        stages[2] += instance.stream_timing.sort_ms;
        stages[3] += std::max(0.0, instance.build_ms - instance.stream_timing.sort_ms - instance.packing_ms
            - (stepped ? 0.0 : instance.cpu.phase_ms()[1]));
        stages[4] += instance.packing_ms;
    }
    backend_->record_group_work(instance.visible, instance.prepared, stepped && instance.stepped);
    work_.groups_visible += instance.visible ? 1U : 0U;
    work_.groups_prepared += instance.prepared ? 1U : 0U;
    if (stepped && instance.deferred) ++work_.updates_deferred;
    if (stepped) { if (instance.stepped) ++work_.steps; }
    else ++work_.presents;
    if (instance.prepared) {
        work_.streams_built += instance.plans.size();
        if (hashed) work_.streams_hashed += instance.plans.size();
    }
    // Preserve the logical render-admission feedback between catch-up samples.
    // Delaying the backend submission must not change PS-38 CPU update admission.
    if (instance.prepared) for (const auto index : instance.render_active) {
        if (!instance.streams[index].indices.empty()) instance.was_rendered = true;
    }
    if (upload_scopes_ != 0) {
        instance.pending_upload = true;
        return;
    }
    commit_upload(instance);
}

void EffectRegistry::commit_upload(Instance& instance) {
    // Workers establish empty streams; only this ordered main-thread commit
    // changes residency. Notify once per empty span and once on a later
    // terminal upgrade, including groups outside render preparation.
    if (instance.prepared || instance.cpu.particles().empty()) {
        for (std::size_t index = 0; index < instance.resources.size(); ++index) {
            if (instance.resources[index] == 0U
                || (instance.backend_empty[index] && (!instance.exhausted || instance.backend_terminal[index]))
                || !instance.streams[index].indices.empty()) continue;
            backend_->empty_emitter(instance.resources[index], instance.exhausted);
            instance.backend_empty[index] = true;
            instance.backend_terminal[index] = instance.exhausted;
        }
    }
    // PS-39: hide retained geometry when culled; re-entry rebuilds it before
    // restoring visibility. Backend calls remain in caller order.
    if (!instance.prepared) {
        if (instance.backend_visible) {
            for (const auto resource : instance.resources) if (resource != 0U) backend_->set_emitter_visible(resource, false);
            instance.backend_visible = false;
        }
        return;
    }
    for (const std::size_t index : instance.render_active) {
        const std::uint64_t deferred = backend_->deferred_updates();
        backend_->update_emitter(instance.resources[index], instance.streams[index]);
        // Held back by a retry backoff: the next present builds and submits this instance again.
        if (backend_->deferred_updates() != deferred) instance.dirty = true;
        instance.backend_empty[index] = false;
        instance.backend_terminal[index] = false;
        if (!instance.backend_visible) backend_->set_emitter_visible(instance.resources[index], true);
        if (!instance.streams[index].indices.empty()) instance.was_rendered = true;
        ++work_.uploads;
    }
    instance.backend_visible = true;
}

core::Result<void> EffectRegistry::release(const EffectHandle handle) {
    const auto found = std::lower_bound(instances_.begin(), instances_.end(), handle,
        [](const auto& instance, const EffectHandle wanted) { return instance->handle < wanted; });
    if (found == instances_.end() || (*found)->handle != handle) return core::Result<void>::failure(unknown(handle));
    for (const std::uint64_t resource : (*found)->resources) {
        if (resource == 0U) continue;
        backend_->destroy_emitter(resource);
        --live_resources_;
    }
    auto& instance = **found;
    if (instance.telemetry) {
        ++instance.telemetry->released;
        for (std::size_t index = 0; index < instance.telemetry->retired_dropped.size(); ++index) {
            auto& total = instance.telemetry->retired_dropped[index];
            total += std::min(instance.cpu.emitter_dropped(index), std::numeric_limits<std::size_t>::max() - total);
        }
    }
    std::fill(instance.resources.begin(), instance.resources.end(), 0U);
    if (instance.charged_bytes != 0 || instance.admitted_definition) {
        admission_.live_bytes -= instance.charged_bytes;
        if (instance.admitted_definition) {
            admission_.idle_bytes += instance.charged_bytes;
            idle_.push_back(std::move(*found));
        } else {
            budget_->release(ResourceKind::cpu_effect, instance.charged_bytes);
        }
    }
    instances_.erase(found);
    return core::Result<void>::success();
}

core::Result<EffectDetachState> EffectRegistry::detach(const EffectHandle handle) {
    Instance* const found = find(handle);
    if (found == nullptr) return core::Result<EffectDetachState>::failure(unknown(handle));
    if (!found->leave_particles) {
        // The source drops its last reference here, deleting every emitter.
        auto released = release(handle);
        if (!released) return core::Result<EffectDetachState>::failure(released.error());
        return core::Result<EffectDetachState>::success(EffectDetachState::released);
    }
    found->dirty = true;
    found->cpu.detach();
    return core::Result<EffectDetachState>::success(EffectDetachState::draining);
}

core::Result<void> EffectRegistry::stop_emission(const EffectHandle handle) {
    Instance* const found = find(handle);
    if (found == nullptr) return core::Result<void>::failure(unknown(handle));
    found->dirty = true;
    found->cpu.detach();
    return core::Result<void>::success();
}

const std::vector<EmitterRenderPlan>* EffectRegistry::plans(const EffectHandle handle) const {
    const Instance* const instance = find(handle);
    return instance != nullptr ? &instance->plans : nullptr;
}

std::size_t EffectRegistry::reserved_particles(const EffectHandle handle) const {
    const auto* instance = find(handle);
    if (!instance) return 0;
    std::size_t reserved{};
    for (const auto& capacity : instance->cpu.emitter_capacities()) reserved += capacity.reserved;
    return reserved;
}

std::span<const EmitterCapacity> EffectRegistry::emitter_capacities(const EffectHandle handle) const {
    const auto* instance = find(handle);
    return instance ? instance->cpu.emitter_capacities() : std::span<const EmitterCapacity>{};
}

std::size_t EffectRegistry::live_effects() const noexcept { return instances_.size(); }
std::size_t EffectRegistry::live_backend_resources() const noexcept { return live_resources_; }
bool EffectRegistry::exhausted(const EffectHandle handle) const noexcept {
    const auto* instance = find(handle);
    return instance != nullptr && instance->cpu.exhausted();
}
std::span<const core::Diagnostic> EffectRegistry::diagnostics() const noexcept { return diagnostics_; }

} // namespace eawr::presentation::particles
