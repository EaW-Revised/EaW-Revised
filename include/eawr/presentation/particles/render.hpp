#pragma once

#include "eawr/core/diagnostic.hpp"
#include "eawr/core/result.hpp"
#include "eawr/presentation/particles/particles.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
    // Converted V1 renderers all construct with sorting disabled, so quads
    // are drawn in CPU particle order; emitters are ordered by index through
    // `order_in_phase`, which a backend maps inside its pass priority range.
    bool sort_particles{};
    std::uint32_t order_in_phase{};
    std::string texture;
    // Bump-mapped selectors only: the emitter's normal-map texture (may be empty).
    std::string normal_texture;
    float tail_size{};
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
};

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
    Vec3 bounds_min{};
    Vec3 bounds_max{};
    void clear();
};

// Appends one quad per live particle of `plan.emitter_index`, in CPU particle
// order. A non-drawable plan appends nothing. A particle whose consumed fields
// or computed positions, UVs, color, or bounds are nonfinite is skipped as a
// whole quad; earlier stream contents remain intact. Camera axes are consumed
// only by camera-facing families (or a kite using the no-direction fallback).
void build_stream(const EmitterRenderPlan& plan, std::span<const Particle> particles,
                  const CameraFrame& camera, VertexStream& stream);

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
    virtual void update_emitter(std::uint64_t resource, const VertexStream& stream) = 0;
    virtual void destroy_emitter(std::uint64_t resource) = 0;
    // Why the most recent create_emitter returned zero, for the per-emitter cause.
    [[nodiscard]] virtual std::string failure_cause() const { return {}; }

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
    std::uint64_t hash{};
    bool drawn{};
    // Largest vertex colour alpha in the emitter's stream this frame (zero with
    // no quads). A heat emitter's screen offset scales with it.
    float maximum_alpha{};
};

struct EffectFrameStats final {
    AdvanceStats advance;
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
    std::uint64_t batches{};        // advance_all / present_all calls that ran their tasks on the executor
    std::uint64_t tasks{};          // executor tasks those batches ran
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
    explicit EffectRegistry(RenderBackend& backend);
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
    [[nodiscard]] std::size_t live_effects() const noexcept;
    [[nodiscard]] std::size_t live_backend_resources() const noexcept;
    [[nodiscard]] std::span<const core::Diagnostic> diagnostics() const noexcept;

private:
    [[nodiscard]] core::Result<EffectHandle> spawn_impl(
        SystemDefinition system, std::uint32_t seed, std::size_t max_particles,
        std::optional<MeshBinding> mesh_binding);
    struct Instance;
    // The live instance of `handle`, or null.
    [[nodiscard]] Instance* find(EffectHandle handle) const noexcept;
    // Records and returns the unknown-handle diagnostic.
    [[nodiscard]] core::Diagnostic unknown(EffectHandle handle);
    // Builds the instance's streams from its live particles and fills `stats` when given; touches
    // nothing but the instance and `stats`, so instances build concurrently.
    void build(Instance& instance, const CameraFrame& camera, EffectFrameStats* stats) const;
    // Hands the instance's built streams to the backend (the calling thread only) and counts the work.
    void upload(Instance& instance, bool stepped, bool hashed);
    // Resolves `handles` to live, distinct instances into batch_, or records the diagnostic.
    [[nodiscard]] core::Result<void> resolve(std::span<const EffectHandle> handles);
    // Runs task(i) for every instance of batch_, on the executor when there is more than one.
    [[nodiscard]] core::Result<void> run_batch(const std::function<void(std::size_t)>& task);
    RenderBackend* backend_;
    const StepExecutor* executor_{};
    bool stream_hashes_{true};
    RegistryWorkCounts work_;
    std::vector<Instance*> batch_;
    std::vector<Instance*> sorted_;
    std::vector<std::unique_ptr<Instance>> instances_;
    std::vector<core::Diagnostic> diagnostics_;
    EffectHandle next_handle_{1};
    std::size_t live_resources_{};
};

} // namespace eawr::presentation::particles
