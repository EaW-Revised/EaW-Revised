#pragma once

#include "eawr/core/diagnostic.hpp"
#include "eawr/core/result.hpp"
#include "eawr/presentation/particles/help_board.hpp"
#include "eawr/presentation/particles/particles.hpp"
#include "eawr/presentation/presentation_budget.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Engine-free render planning for CPU particle output. Nothing here names a
// graphics engine, a file system, or simulation state: a backend adapter
// (src/presentation/godot/particle_adapter.cpp) consumes the vertex streams
// and plans produced here. Quad layout, corner order and texture-coordinate
// assignment follow the MIT-licensed alo-viewer revision
// 9bb0053919cc5df8377610d4f91b11d956d6c2f4 (DirectX9/ParticleRenderers.cpp).

namespace eawr::presentation::particles {

// Geometry families the adapters can draw. Everything else is `unsupported`
// with a recorded cause; no emitter silently falls through to a quad.
enum class RenderFamily : std::uint8_t { billboard, xy_aligned, heat_saturation, kites, unsupported };

// Blend policy taken from the render state of the selected public Engine/Prim*
// effect, never from a guess. `heat_distortion` blends a distorted scene sample
// (PrimHeat), which is why heat draws after transparents. `bump_alpha` is the
// alpha blend of PrimParticleBumpAlpha with its normal map lit by one
// directional light (an approximation of the effect's lighting).
enum class Blend : std::uint8_t { opaque, additive, alpha, modulate, heat_distortion, bump_alpha };

// Where the backend places the draw relative to the public pass sequence.
enum class DrawPhase : std::uint8_t { opaque, transparent, heat };

[[nodiscard]] std::string_view to_string(RenderFamily family) noexcept;
[[nodiscard]] std::string_view to_string(Blend blend) noexcept;
[[nodiscard]] std::string_view to_string(DrawPhase phase) noexcept;

// The legacy V1 blend selector indexes this table of public engine effects.
// Selector values outside the table are kept verbatim by the parser and fail
// closed here.
struct LegacyBlendSelector final {
    std::uint32_t value{};
    std::string_view program;
    std::string_view technique;
    bool supported{};
    std::string_view cause;
};
[[nodiscard]] std::span<const LegacyBlendSelector> legacy_blend_selectors() noexcept;

// MD-09: reference speed of a kite whose emitter inherits motion (debug build constant).
// Evidence: MD-09, debug build (detail kept in out/ only).
inline constexpr float kInheritedKiteReferenceSpeed = 1000.0F;

struct EmitterRenderPlan final {
    std::size_t emitter_index{};
    std::uint32_t renderer_id{};
    RenderFamily family{RenderFamily::unsupported};
    std::uint32_t blend_selector{};
    std::string_view program;
    std::string_view technique;
    Blend blend{Blend::alpha};
    DrawPhase phase{DrawPhase::transparent};
    bool depth_test{true};
    bool depth_write{};
    // PS-32: authored sort only for eligible blend modes with depth enabled.
    bool sort_particles{};
    bool triangles{};
    std::uint32_t order_in_phase{};
    std::string texture;
    // Bump-mapped selectors only: the emitter's normal-map texture (may be empty).
    std::string normal_texture;
    float tail_size{};
    bool legacy_kite_motion{};
    bool legacy_slot_order{};
    bool inherit_emitter_motion{};
    float kite_speed_limit{};
    bool drawable{};
    std::string cause;
};

// Resolves one emitter to a plan. A plan that is not drawable carries a cause
// and must not be drawn by any backend.
[[nodiscard]] EmitterRenderPlan plan_emitter(const EmitterDefinition& emitter, std::size_t index);
[[nodiscard]] std::vector<EmitterRenderPlan> plan_system(const SystemDefinition& system);

// A camera expressed in the particle (ALO, Z-up) basis.
struct CameraFrame final {
    Vec3 position{};
    Vec3 right{1.0F, 0.0F, 0.0F};
    Vec3 up{0.0F, 0.0F, 1.0F};
    bool collect_axis_diagnostics{};
};

// PS-39: scene culling precedes stream preparation. Planes use the particle
// basis, with the outside half-space dot(normal, position) > distance.
struct CullingFrame final {
    std::array<Vec4, 6> planes{};
    bool valid{};
    bool enabled{};
};
struct ParticleBounds final {
    Vec3 minimum{}, maximum{};
    bool valid{};
};
[[nodiscard]] ParticleBounds particle_bounds(std::span<const EmitterRenderPlan> plans,
    std::span<const Particle> particles);
[[nodiscard]] bool intersects(const ParticleBounds& bounds, const CullingFrame& frame) noexcept;

// Builds a camera frame from a render-basis (Y-up) look-at camera, applying the
// inverse of the documented asset-to-render conversion (x, y, z) -> (x, z, -y).
[[nodiscard]] CameraFrame camera_frame_from_render(
    const std::array<float, 3>& eye, const std::array<float, 3>& target,
    const std::array<float, 3>& up);

// An origin and basis in the particle basis for CpuSystem::set_origin/set_basis.
struct EmitterFrame final {
    Vec3 origin{};
    Basis3 basis{};
};
// Converts a render-basis column-major affine matrix (for example a P1-03
// attachment transform) back into the particle basis: C^-1 * M * C.
[[nodiscard]] EmitterFrame emitter_frame_from_render(const std::array<float, 16>& column_major);

struct ParticleVertex final {
    Vec3 position{};
    Color color{};
    float u{}, v{};
};

struct VertexStream final {
    std::vector<ParticleVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::size_t quads{};
    std::size_t triangles{};
    // Retained, emitter-local scratch for depth sorting or V1 alpha slot order.
    std::vector<const Particle*> sorted_particles;
    std::size_t sort_candidates{};
    Vec3 bounds_min{};
    Vec3 bounds_max{};
    std::uint64_t axis_samples{};
    double axis_max_sine_error{};
    std::uint64_t axis_reversed{};
    void clear();
};

struct StreamBuildTimings final {
    double sort_ms{};
};

// Appends the authored primitive per eligible particle, farthest first only
// when PS-32 requests sorting; otherwise V1 alpha follows primitive slot order.
// A non-drawable plan appends nothing. A particle whose consumed fields
// or computed positions, UVs, color, or bounds are nonfinite is skipped as a
// whole quad; earlier stream contents remain intact. Camera axes are consumed
// only by camera-facing families (or a kite using the no-direction fallback).
void build_stream(const EmitterRenderPlan& plan, std::span<const Particle> particles,
                  const CameraFrame& camera, VertexStream& stream, StreamBuildTimings* timing = nullptr);

// The same geometry from an emitter-local selection, in the input system's
// particle order. Selections remain valid until that system advances again.
void build_stream_subset(const EmitterRenderPlan& plan, std::span<const Particle* const> particles,
                         const CameraFrame& camera, VertexStream& stream, StreamBuildTimings* timing = nullptr);

// FNV-1a over the exact float bits of the stream. Equal hashes across runs are
// the determinism evidence; the hash is not a security primitive.
[[nodiscard]] std::uint64_t stream_hash(const VertexStream& stream,
                                        std::uint64_t seed = 0xcbf29ce484222325ULL);
[[nodiscard]] std::string hex64(std::uint64_t value);

// Largest summed |dR| + |dG| + |dB| (8-bit captured colour in [0, 1]) that one
// heat distortion draw can make to a pixel of a width x height viewport when
// its vertex alpha peaks at `peak_vertex_alpha`; `distortion_amount` is the
// fraction of the screen a full texel offset moves the scene sample. Premise:
// the covered pixel equals the heat pass's screen copy (the opaque scene).
[[nodiscard]] float heat_distortion_pixel_change_bound(float peak_vertex_alpha,
    float distortion_amount, std::int32_t width, std::int32_t height) noexcept;

// Backend-neutral resource sink. Each drawable emitter owns exactly one backend
// resource between create_emitter and destroy_emitter.
class RenderBackend {
public:
    virtual ~RenderBackend() = default;
    // Returns zero when the backend cannot create the resource; the emitter is
    // then reported as not drawn rather than drawn some other way.
    [[nodiscard]] virtual std::uint64_t create_emitter(const EmitterRenderPlan& plan) = 0;
    // Worker-safe buffer preparation: independent resources may prepare concurrently.
    // No engine/scene calls here; update_emitter commits the prepared data in caller order.
    virtual void prepare_emitter(std::uint64_t, const VertexStream&) {}
    virtual void update_emitter(std::uint64_t resource, const VertexStream& stream) = 0;
    // Empty render submission, independent of camera admission. A terminal CPU
    // lifecycle needs no dwell; resumable emitters may retain a short warm hold.
    virtual void empty_emitter(std::uint64_t resource, bool) { update_emitter(resource, VertexStream{}); }
    virtual void destroy_emitter(std::uint64_t resource) = 0;
    // Why the most recent create_emitter returned zero, for the per-emitter cause.
    [[nodiscard]] virtual std::string failure_cause() const { return {}; }
    // Snapshotted on the caller thread, then read without backend calls by workers.
    [[nodiscard]] virtual CullingFrame culling_frame() const { return {}; }
    virtual void set_emitter_visible(std::uint64_t, bool) {}
    virtual void record_group_work(bool, bool, bool) {}
    // Counts uploads the backend could not complete (a resource it failed to restore); the
    // registry rebuilds and resubmits every instance after the count moves, so a retry still
    // happens on an otherwise unchanged frame.
    [[nodiscard]] virtual std::uint64_t incomplete_updates() const noexcept { return 0; }
    // Counts uploads the backend held back while a failed resource waits out its retry backoff.
    // Not a failure: the registry resubmits only the instance whose update was held back, so
    // the rest of the frame stays clean.
    [[nodiscard]] virtual std::uint64_t deferred_updates() const noexcept { return 0; }
    // True when update_emitter of one resource may run on the caller while prepare_emitter of
    // other resources runs on workers (it touches only its own resource's state).
    [[nodiscard]] virtual bool overlapped_commit_safe() const noexcept { return false; }

protected:
    RenderBackend() = default;
    RenderBackend(const RenderBackend&) = default;
    RenderBackend& operator=(const RenderBackend&) = default;
};

namespace diagnostic_codes {
inline constexpr std::string_view unknown_effect = "EAWR-PARTICLE-0006";
inline constexpr std::string_view backend_resource = "EAWR-PARTICLE-0007";
inline constexpr std::string_view mesh_binding = "EAWR-PARTICLE-0008";
// A batched advance or present given a handle twice, or whose executor could not run its tasks (#638).
inline constexpr std::string_view batch = "EAWR-PARTICLE-0010";
} // namespace diagnostic_codes

struct EmitterFrameStats final {
    std::size_t particles{};
    std::size_t quads{};
    std::size_t triangles{};
    std::uint64_t hash{};
    bool drawn{};
    // Largest vertex colour alpha in the emitter's stream this frame (zero with
    // no quads). A heat emitter's screen offset scales with it.
    float maximum_alpha{};
    std::uint64_t axis_samples{};
    double axis_max_sine_error{};
    std::uint64_t axis_reversed{};
};

struct EffectFrameStats final {
    AdvanceStats advance;
    float elapsed_seconds{};
    float deferred_seconds{};
    std::size_t particles{};
    std::vector<EmitterFrameStats> emitters;
    std::uint64_t hash{};
    bool has_bounds{};
    Vec3 bounds_min{};
    Vec3 bounds_max{};
    // Lifecycle after this advance: `detached` once EffectRegistry::detach has
    // stopped root emission; `finished` once a detached instance has no live
    // particle and no child emission left (CpuSystem::finished).
    bool detached{};
    bool finished{};
};

using EffectHandle = std::uint32_t;

// Runs the independent per-instance work of EffectRegistry::advance_all and present_all (#638,
// the ADR-009 pattern): each task reads its own instance and the shared camera and writes only
// that instance and its own statistics slot, so the threads that run the tasks never change a
// result. The viewer gives its registries a small worker pool; without an executor the tasks run
// on the calling thread.
class StepExecutor {
public:
    virtual ~StepExecutor() = default;
    // Runs task(0) .. task(count - 1), each exactly once, possibly concurrently, and returns when
    // all have finished; false when they could not all run (a task that threw, a pool that failed).
    [[nodiscard]] virtual bool run(std::size_t count, const std::function<void(std::size_t)>& task) const = 0;
    // The threads run() uses at most at once, the calling thread included (1: it runs alone).
    [[nodiscard]] virtual std::size_t participants() const noexcept { return 1; }
    // PF lane-serial (a): where run_ordered() publishes its unclaimed tasks for a thread outside the executor (the
    // viewer's main thread waiting for the particle lane) to build. Null (the default): nobody else helps. A helper
    // only builds; the commits stay on the calling thread, in order, so the output is the same with or without help.
    [[nodiscard]] virtual HelpBoard* help_board() const noexcept { return nullptr; }
    // Like run(), plus consume(0) .. consume(count - 1) on the calling thread, in that order,
    // each after task(i) finished and while later tasks may still run: the caller commits what
    // is ready and builds a task itself only when the next one to commit is not. At most
    // max_ordered tasks. False when a task could not run. Failure is NOT all-or-nothing: the
    // commits of the tasks before the failed one stand (a prefix, since consume(i) runs as soon
    // as task(i) is done), where run() followed by a loop of commits would commit nothing; no
    // consume runs for the failed task or after it. A task that throws is caught and fails the run;
    // so is a consume that throws (run_ordered ends the commits there and never retries that one),
    // and the viewer's pool catches whatever else escapes a job.
    // Also the most slices a particle batch is cut into (EffectRegistry). 256, not the simulation's 64 partitions:
    // finer slices let the pool's helpers balance a batch better, and the commits stay in slice order, so the output
    // is the same with any slice count (PF lane-serial (c), 2026-11-03: wait_dispatch grew with the pool at 64).
    static constexpr std::size_t max_ordered = 256;
    [[nodiscard]] bool run_ordered(std::size_t count, const std::function<void(std::size_t)>& task,
        const std::function<void(std::size_t)>& consume) const;
    // N1: run() without the caller. Starts task(0) .. task(count - 1) on the executor's own threads (one
    // of them stands in for the caller as worker 0) and returns at once; `task` must stay valid until
    // join_async() returns. False when the executor cannot (the default: no thread of its own) or is
    // already running an async batch: nothing was started and the caller runs the batch as usual.
    // join_async() waits for the batch and returns whether every task ran; call it exactly once after a
    // true start_async. Calls to run()/run_ordered() from another thread meanwhile wait for the batch.
    [[nodiscard]] virtual bool start_async(std::size_t, const std::function<void(std::size_t)>&) const { return false; }
    [[nodiscard]] virtual bool join_async() const { return false; }

protected:
    StepExecutor() = default;
    StepExecutor(const StepExecutor&) = default;
    StepExecutor& operator=(const StepExecutor&) = default;
};

// What an EffectRegistry did, counted on the calling thread (#638). Deterministic for a given
// sequence of calls whatever the executor, so a test can hold the work to a budget.
struct RegistryWorkCounts final {
    std::uint64_t steps{};          // instances advanced (their CPU system stepped)
    std::uint64_t presents{};       // instances presented (followed their emitter, not stepped)
    std::uint64_t streams_built{};  // emitter vertex streams built
    std::uint64_t streams_hashed{}; // emitter streams hashed for the frame statistics
    std::uint64_t uploads{};        // streams handed to the backend
    std::uint64_t presents_clean{}; // presents answered by the streams already built (nothing changed)
    std::uint64_t presents_unmoved{}; // presents of a moved emitter frame that moved no particle (streams kept)
    std::uint64_t batches{};        // advance_all / present_all calls that ran their tasks on the executor
    std::uint64_t pipelined_batches{}; // of those, batches whose commit overlapped the builds
    std::uint64_t tasks{};          // executor tasks those batches ran
    std::uint64_t groups_visible{};
    std::uint64_t groups_prepared{};
    std::uint64_t updates_deferred{};
    std::uint64_t particle_bucket_visits{}; // one system scan when multiple drawable emitters share it
    std::uint64_t stream_particle_visits{}; // candidates consumed by stream builders, before sorting
};

// Opt-in timings: worker CPU sums and caller elapsed intervals are separate.
struct RegistryTimings final {
    double spawn_cpu_ms{}, spawn_backend_ms{};
    double scalar_step_ms{}, scalar_build_ms{};
    double batch_step_cpu_ms{}, batch_build_cpu_ms{};
    double executor_ms{}, executor_wait_dispatch_ms{}, commit_ms{};
    // Mutually exclusive CPU sums: emission, update, sorting, stream/statistics,
    // backend buffer packing. Caller work overlaps executor_ms; worker work can
    // overlap other worker work. Upload/backend commits remain caller-only.
    std::array<double, 5> main_stages_ms{}, worker_stages_ms{};
};

// Outcome of EffectRegistry::detach. `draining` keeps the instance and its
// backend resources until its owner releases it; `released` means the
// instance and its resources are already gone and the handle is dead.
enum class EffectDetachState : std::uint8_t { draining, released };
[[nodiscard]] std::string_view to_string(EffectDetachState state) noexcept;

// Owns CPU particle instances and their backend resources. It is presentation
// state only: it is advanced by a presentation clock, never serialized, and
// never read by simulation or replay.
class EffectRegistry final {
public:
    enum class Refusal : std::uint8_t { none, budget_pressure, allocation_failed, definition_failure, arithmetic_failure };
    enum class AdmissionStage : std::uint8_t { none, cpu, backend, publication };
    [[nodiscard]] static constexpr std::string_view refusal_name(Refusal reason) noexcept {
        switch (reason) {
        case Refusal::none: return "none";
        case Refusal::budget_pressure: return "budget_pressure";
        case Refusal::allocation_failed: return "allocation_failed";
        case Refusal::definition_failure: return "definition_failure";
        case Refusal::arithmetic_failure: return "arithmetic_failure";
        }
        return "unknown";
    }
    struct Admission final {
        EffectHandle handle{};
        Refusal refusal{Refusal::none};
        bool reused{};
        std::size_t requested_bytes{}, admitted_bytes{};
        AdmissionStage failed_stage{AdmissionStage::none};
        [[nodiscard]] explicit operator bool() const noexcept { return handle != 0; }
    };
    struct AdmissionStats final {
        std::size_t eligible{}, admitted{}, refused{}, reused{}, evicted{}, allocations{};
        std::size_t live_bytes{}, idle_bytes{}, high_water_bytes{};
        std::array<std::size_t, 5> refusals{};
        std::size_t requested_bytes{}, refused_bytes{};
        std::array<std::size_t, 4> allocation_failure_stages{};
    };
    // Deterministic fault injection for admission contracts; consumed once.
    void fail_next_admission(AdmissionStage stage) noexcept { fail_stage_ = stage; }
    // The immutable shared definition is also the compatibility key. Mesh-bound
    // effects receive full capacity but are not cached across binding changes.
    [[nodiscard]] Admission spawn_authored(std::shared_ptr<const SystemDefinition> system,
        std::uint32_t seed, std::optional<MeshBinding> mesh = std::nullopt);
    // Configure before first admission; provider must outlive this registry.
    void set_admission_budget(PresentationBudget& budget) noexcept { budget_ = &budget; }
    [[nodiscard]] const AdmissionStats& admission_stats() const noexcept { return admission_; }
    [[nodiscard]] std::span<const EmitterCapacity> emitter_capacities(EffectHandle handle) const;
    void evict_idle() noexcept;
    void write_admission_report(std::ostream& output) const;
    explicit EffectRegistry(RenderBackend& backend);
    // Developer comparison switch, configured before spawning systems. The
    // reference path retains the full-system stream input for paired profiling.
    // Live and idle layouts retain their construction-time setting until evicted.
    static void set_stream_bucketing_default(bool on) noexcept;
    // Diagnostic comparison of folded and per-sample presentation in one binary.
    static void set_catchup_folding_default(bool on) noexcept;
    // Developer comparison switch for the dirty-tracked present: when on (the default) a present
    // whose instance, camera and culling frame are unchanged since its last build reuses that
    // build (identical streams, identical backend state) instead of building and uploading again.
    // Off rebuilds every present, as before; read when a registry is constructed.
    static void set_clean_present_default(bool on) noexcept;
    // Test hook (PF lane-serial (c)): the most slices a batch is cut into, clamped to 1 .. StepExecutor::max_ordered
    // (the default). Output is the same for any value; the contracts check 7, 64 and 256.
    static void set_slice_cap_default(std::size_t slices) noexcept;
    [[nodiscard]] static bool catchup_folding_default() noexcept;
    // Whether a frame with `due` samples presented and `samples` already processed runs several passes whose
    // intermediate uploads nobody sees: two or more samples due. Its owners open a defer_uploads scope then,
    // so only the last pass packs, commits and uploads (folding itself starts above two samples).
    [[nodiscard]] static bool defers_catchup_uploads(std::uint64_t due, std::uint64_t samples) noexcept {
        return catchup_folding_default() && due > samples && due - samples > 1;
    }
    ~EffectRegistry();
    EffectRegistry(const EffectRegistry&) = delete;
    EffectRegistry& operator=(const EffectRegistry&) = delete;

    [[nodiscard]] core::Result<EffectHandle> spawn(
        SystemDefinition system, std::uint32_t seed, std::size_t max_particles);
    [[nodiscard]] core::Result<EffectHandle> spawn(
        SystemDefinition system, std::uint32_t seed, std::size_t max_particles,
        MeshBinding mesh_binding);
    [[nodiscard]] core::Result<void> set_frame(EffectHandle handle, const EmitterFrame& frame);
    [[nodiscard]] core::Result<void> set_mesh_frame(EffectHandle handle, const MeshFrame& frame);
    // The emitter's brightness vector (b, b, b, b; BP-45, the debug build): every particle vertex's
    // RGBA is multiplied by it when it is set up for rendering. 1 (the default) leaves the
    // streams untouched; a negative or non-finite value is a diagnostic.
    [[nodiscard]] core::Result<void> set_brightness(EffectHandle handle, float brightness);
    [[nodiscard]] core::Result<void> set_dynamic(EffectHandle handle, float size, float energy);
    [[nodiscard]] core::Result<void> set_pinched_cylinder(EffectHandle handle, float radius, float height, float fraction);
    // A rendered frame may advance several logical samples. Keep their CPU/statistics
    // work, but submit only the last stream of each surviving effect to the backend.
    class FrameUploads final {
    public:
        explicit FrameUploads(EffectRegistry& registry, bool enabled) noexcept;
        ~FrameUploads();
        FrameUploads(const FrameUploads&) = delete;
        FrameUploads& operator=(const FrameUploads&) = delete;
    private:
        EffectRegistry& registry_;
        bool enabled_;
    };
    [[nodiscard]] FrameUploads defer_uploads(bool enabled = true) noexcept { return FrameUploads(*this, enabled); }
    [[nodiscard]] core::Result<EffectFrameStats> advance(
        EffectHandle handle, float delta_seconds, const CameraFrame& camera);
    // A drawn frame between advances (#433): the particles that live in their emitter's frame
    // follow it to the frame last set (CpuSystem::follow_emitter), and the streams are built
    // again for `camera` at the current brightness and uploaded, without advancing time. It keeps
    // no stats, so a present allocates nothing once the instance's stream has grown (#439).
    [[nodiscard]] core::Result<void> present(EffectHandle handle, const CameraFrame& camera);
    // #638: advance() on every handle of `handles` with the same step and camera, with the same
    // results and the same backend uploads in `handles` order. The instances step and build their
    // streams as tasks on the executor (set_executor); the calling thread then uploads the
    // streams in order. `stats` receives one entry per handle. A handle that is not live, or that
    // appears twice, fails the batch before any instance changes.
    [[nodiscard]] core::Result<void> advance_all(std::span<const EffectHandle> handles, float delta_seconds,
        const CameraFrame& camera, std::vector<EffectFrameStats>& stats);
    // The same batch with one step per handle: new attachment generations use zero while their
    // older drains advance the clock. A size mismatch fails before any instance changes.
    [[nodiscard]] core::Result<void> advance_all(std::span<const EffectHandle> handles,
        std::span<const float> delta_seconds, const CameraFrame& camera, std::vector<EffectFrameStats>& stats);
    // N1: advance_all (per-handle steps) in two halves. begin_advance_all resolves the handles, observes the camera and
    // starts the instances' step, stream build and pack on the executor WITHOUT the calling thread, then returns:
    // true started, false nothing was started (no executor able to run async, fewer than two handles, a backend
    // that does not allow overlap, or a deferred-upload scope is open) and the caller runs advance_all instead;
    // a diagnostic fails before any instance changes, as advance_all does. `stats` is written until
    // finish_advance_all, which joins the executor and then commits the streams to the backend in handle order on
    // the calling thread; the results, uploads and counters equal advance_all's. Between the two calls the caller
    // must not use this registry (spawn, release, advance, present, set_*) nor the backend's engine side, and must
    // pass handles/deltas it may drop (they are copied); finish_advance_all is also run by the destructor.
    [[nodiscard]] core::Result<bool> begin_advance_all(std::span<const EffectHandle> handles,
        std::span<const float> delta_seconds, const CameraFrame& camera, std::vector<EffectFrameStats>& stats);
    [[nodiscard]] core::Result<void> finish_advance_all();
    [[nodiscard]] bool advance_pending() const noexcept { return async_.active; }
    // present() on every handle of `handles`, batched as advance_all.
    [[nodiscard]] core::Result<void> present_all(std::span<const EffectHandle> handles, const CameraFrame& camera);
    // The pool advance_all and present_all run their tasks on (null, the default: the calling
    // thread). It must outlive the registry's batched calls.
    void set_executor(const StepExecutor* executor) noexcept;
    // Whether an advance hashes the emitters' streams into EmitterFrameStats::hash and
    // EffectFrameStats::hash (on by default: the reports and tests compare them). Off, both
    // hashes are zero and every other statistic is unchanged (#638: the live battle's registries
    // leave them off, as nothing there reads them).
    void set_stream_hashes(bool on) noexcept;
    void begin_measurement(bool on) noexcept;
    [[nodiscard]] const RegistryTimings& timings() const noexcept { return timings_; }
    // PS-34..PS-37: applies to existing and future effects; caller-thread only.
    [[nodiscard]] core::Result<void> set_detail(ParticleDetail detail);
    // The detail the registry's instances run with now (the in-battle log reads it back).
    [[nodiscard]] ParticleDetail detail() const noexcept { return detail_; }
    [[nodiscard]] const RegistryWorkCounts& work() const noexcept;
    // Frees every backend resource of the instance. Releasing twice is a
    // diagnostic, not a crash.
    [[nodiscard]] core::Result<void> release(EffectHandle handle);
    // Source ParticleSystemInstance::Detach. With the system's leave-particles
    // flag clear the instance is released immediately (`released`). With it
    // set, root emitters stop and the instance keeps drawing its residual
    // particles and child chains (`draining`); the owner calls release once
    // EffectFrameStats::finished, or earlier to cut the drain short. Detaching
    // a draining instance again succeeds without resetting it. A released or
    // unknown handle is the same diagnostic as release. Handles are never reused.
    [[nodiscard]] core::Result<EffectDetachState> detach(EffectHandle handle);
    // Stops root emission and keeps drawing the residual particles whatever the system's
    // leave-particles flag says (a proxy an ability hides keeps its already emitted particles, #559,
    // battle-presentation BP-65). Like a draining detach the owner releases the instance once
    // EffectFrameStats::finished, or earlier. Repeating it succeeds; an unknown handle is the same
    // diagnostic as release.
    [[nodiscard]] core::Result<void> stop_emission(EffectHandle handle);

    [[nodiscard]] const std::vector<EmitterRenderPlan>* plans(EffectHandle handle) const;
    // Actual CPU slot reservation after normalization and emitter admission; zero for a dead handle.
    [[nodiscard]] std::size_t reserved_particles(EffectHandle handle) const;
    [[nodiscard]] std::size_t live_effects() const noexcept;
    [[nodiscard]] std::size_t live_backend_resources() const noexcept;
    // False for an unknown handle. Read-only: never detaches or consumes time/RNG.
    [[nodiscard]] bool exhausted(EffectHandle handle) const noexcept;
    [[nodiscard]] std::span<const core::Diagnostic> diagnostics() const noexcept;

private:
    [[nodiscard]] core::Result<EffectHandle> spawn_impl(
        SystemDefinition system, std::uint32_t seed, std::size_t max_particles,
        std::optional<MeshBinding> mesh_binding);
    struct Instance;
    struct LayoutTelemetry;
    // Records the camera and culling frame a batch is built for; builds made for another pair are
    // never reused. Caller thread, before any task.
    void observe_environment(const CameraFrame& camera);
    [[nodiscard]] bool is_clean(const Instance& instance) const noexcept;
    // Only the emitter frame changed: a follow that moves no particle leaves the streams as built.
    [[nodiscard]] bool follow_only(const Instance& instance) const noexcept;
    std::size_t upload_scopes_{};
    bool measure_{};
    RegistryTimings timings_;
    // Caller-owned dispatch scratch keeps worker callbacks within std::function's
    // inline storage. Workers read it only until the synchronous barrier returns.
    std::thread::id measurement_caller_{};
    double caller_work_ms_{};
    std::size_t batch_task_count_{};
    // The live instance of `handle`, or null.
    [[nodiscard]] Instance* find(EffectHandle handle) const noexcept;
    // Records and returns the unknown-handle diagnostic.
    [[nodiscard]] core::Diagnostic unknown(EffectHandle handle);
    // Builds the instance's streams from its live particles and fills `stats` when given; touches
    // nothing but the instance and `stats`, so instances build concurrently.
    void build(Instance& instance, const CameraFrame& camera, EffectFrameStats* stats) const;
    void step(Instance& instance, float delta_seconds, EffectFrameStats& stats) const;
    // Hands the instance's built streams to the backend (the calling thread only) and counts the work.
    void upload(Instance& instance, bool stepped, bool hashed);
    void commit_upload(Instance& instance);
    // The outermost FrameUploads' exit: packs each parked instance's final streams once (on the
    // executor), then commits them in the order the passes uploaded them. What a refused run or a
    // throwing pooled pack left parked is packed and committed here, on the caller. On the serial
    // path (no executor, or fewer than two parked instances) there is no such fallback: a pack or
    // commit that throws propagates, and the scope's destructor catches it and abandons what is
    // left (abandon_pending_uploads: marked dirty, rebuilt and uploaded by the next present).
    [[nodiscard]] core::Result<void> commit_pending_uploads();
    void pack_pending(Instance& instance) const;
    void finish_pending(Instance& instance) noexcept;
    void abandon_pending_uploads() noexcept;
    // Resolves `handles` to live, distinct instances into batch_, or records the diagnostic.
    [[nodiscard]] core::Result<void> resolve(std::span<const EffectHandle> handles);
    [[nodiscard]] core::Result<void> advance_batch(std::span<const EffectHandle> handles, float uniform_delta,
        std::span<const float> deltas, const CameraFrame& camera, std::vector<EffectFrameStats>& stats);
    // Runs task(i) for every instance of batch_, on the executor when there is more than one.
    // task(i) builds batch_[i] (on the executor); consume(i) then commits it, on the caller in
    // order: after the barrier, or while later tasks still build when the commit is pipelined.
    [[nodiscard]] core::Result<void> run_batch(const std::function<void(std::size_t)>& task,
        const std::function<void(std::size_t)>& consume);
    // The step, stream build and pack of batch element `index` (instance `instance`, step `delta`); on a worker or the caller.
    void advance_task(Instance& instance, float delta, const CameraFrame& camera, EffectFrameStats& slot, bool on_caller) const;
    // N1: the batch begin_advance_all started. Its own instance list: batch_ stays free for the caller's other calls.
    struct AsyncBatch final {
        bool active{};
        std::vector<Instance*> batch;
        std::vector<float> deltas;
        CameraFrame camera{};
        std::vector<EffectFrameStats>* stats{};
        std::size_t tasks{};
        std::function<void(std::size_t)> job;
    };
    AsyncBatch async_;
    RenderBackend* backend_;
    const StepExecutor* executor_{};
    bool clean_present_;
    bool environment_known_{};
    std::uint64_t environment_{};
    CameraFrame environment_camera_{};
    CullingFrame environment_culling_{};
    std::uint64_t incomplete_updates_{};
    bool stream_hashes_{true};
    RegistryWorkCounts work_;
    ParticleDetail detail_;
    CullingFrame culling_;
    std::vector<Instance*> batch_;
    std::vector<Instance*> sorted_;
    std::vector<std::unique_ptr<Instance>> instances_;
    std::vector<std::unique_ptr<Instance>> idle_;
    std::vector<std::unique_ptr<LayoutTelemetry>> layout_telemetry_;
    PayloadBudget default_budget_;
    PresentationBudget* budget_{&default_budget_};
    AdmissionStats admission_;
    AdmissionStage fail_stage_{AdmissionStage::none};
    std::vector<core::Diagnostic> diagnostics_;
    EffectHandle next_handle_{1};
    std::size_t live_resources_{};
};

} // namespace eawr::presentation::particles
