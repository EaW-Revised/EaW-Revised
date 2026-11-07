#pragma once

#include "eawr/assets/assets.hpp"
#include "particle_batch_census.hpp"
#include "particle_submission_cache.hpp"
#include "eawr/presentation/particles/render.hpp"
#include "eawr/presentation/renderer.hpp"
#include "eawr/presentation/render_churn.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace godot {
class Node3D;
class RID;
}

namespace eawr::presentation::godot_backend {

// RenderingServer backend for the engine-free particle registry. Each drawable
// emitter with live geometry owns one mesh, one material and one instance in the host's scenario;
// textures and shaders are shared and reference counted by name/policy. Vertex
// streams arrive in the ALO basis and receive the one documented conversion
// (x, y, z) -> (x, z, -y) here, exactly as model vertices do in renderer.cpp.
class GodotParticleBackend final : public particles::RenderBackend {
public:
    // Returns the decoded colour texture for an authored name, or nullptr when
    // it does not resolve. The backend never names a path or opens a file.
    using TextureResolver = std::function<const assets::Texture*(std::string_view name)>;
    struct FogCallbacks final {
        std::function<core::Result<std::uint64_t>(const godot::RID&, const godot::RID&)> register_material;
        std::function<void(std::uint64_t)> unregister_material;
        // Samples the final transformed ALO world XY of each submitted vertex.
        std::function<std::uint8_t(float, float)> attenuation_at_source_xy;
    };
    struct FogEmitterEvidence final {
        std::uint64_t resource{};
        std::uint64_t fog_handle{};
        std::size_t emitter_index{};
        std::size_t quads{};
        std::uint8_t minimum_attenuation{255};
        std::uint8_t maximum_attenuation{};
    };

    GodotParticleBackend(godot::Node3D& host, TextureResolver resolver, FogCallbacks fog = {},
                        std::size_t warm_policy_limit = 32,
                        std::uint64_t warm_texture_limit = 8U * 1024U * 1024U);
    ~GodotParticleBackend() override;
    GodotParticleBackend(const GodotParticleBackend&) = delete;
    GodotParticleBackend& operator=(const GodotParticleBackend&) = delete;

    [[nodiscard]] std::uint64_t create_emitter(const particles::EmitterRenderPlan& plan) override;
    // Caller-owned name remains alive until the immediately following spawn ends.
    // Used only by the opt-in backend spawn diagnostic.
    void set_spawn_context(std::string_view effect, std::uint64_t tick, bool diagnostic_fighter = false) noexcept;
    // Construction-time provenance for diagnostics only; ordinary particles are the default.
    void set_churn_owner(render_churn::Owner owner) noexcept;
    // Population-time preparation pins shared textures/shaders, then releases
    // empty draw RIDs and active membership. Runtime instances retain ordinary
    // creation order. Oversized/refused plans fall back.
    [[nodiscard]] bool prewarm_emitter(const particles::EmitterRenderPlan& plan);
    // Load-only diagnostic: preserve the warmed shader/texture references and
    // allocation order, but remove every retained warm Emitter record.
    void retain_prewarm_cache_only();
    // Population-only source census. Visit the complete closure before warming;
    // it resolves texture charges without creating any GPU or logical resource.
    void inventory_prewarm_emitter(const particles::EmitterRenderPlan& plan);
    struct EmitterPrewarmWork final {
        std::size_t demand_references{}, demand_policies{}, demand_textures{}, unresolved_policies{};
        std::uint64_t demand_texture_bytes{};
        bool demand_complete{};
        std::size_t policy_limit{};
        std::uint64_t texture_limit{};
        std::size_t retained_holders{}, cache_only_policies{};
        std::size_t refused_policy_limit{}, refused_texture_limit{}, refused_missing_texture{}, refused_backend{};
    };
    [[nodiscard]] EmitterPrewarmWork emitter_prewarm_work() const noexcept;
    // Pin only the compiled program of a loaded render plan, bounded separately
    // from the texture-bearing warm emitters. No texture, material, mesh or
    // instance is created. A refused plan keeps the ordinary runtime fallback.
    [[nodiscard]] bool prewarm_program(const particles::EmitterRenderPlan& plan);
    // Population-only accounting; a full budget skips further definition decodes.
    [[nodiscard]] bool begin_prewarm_reference();
    void record_prewarm_lookup(double milliseconds, bool decoded);
    void finish_program_prewarm(double milliseconds);
    struct ProgramPrewarmWork final {
        std::size_t references{}, skipped_references{}, requested{}, duplicate_requests{}, refused{};
        std::size_t programs{}, prepared_shaders{}, live_shaders{}, decoded_systems{};
        double prepare_ms{}, lookup_ms{};
    };
    // Historical peak survives release for the final capture's budget evidence.
    [[nodiscard]] ProgramPrewarmWork program_prewarm_work() const noexcept;
    // Releases both kinds of warm hold; ordinary live emitters keep their refs.
    void release_prewarm();
    [[nodiscard]] static std::size_t prewarm_program_limit() noexcept;
    [[nodiscard]] std::size_t prewarmed_emitters() const noexcept;
    [[nodiscard]] std::uint64_t prewarmed_texture_bytes() const noexcept;
    void prepare_emitter(std::uint64_t resource, const particles::VertexStream& stream) override;
    // Empty submissions retire per-emitter RIDs and packing buffers. The logical
    // handle/plan survive; the next nonempty submission recreates synchronously,
    // once, in caller order, before that same stream is made visible. Shared
    // texture and program references survive the empty span until owner release,
    // including terminal owners (one shared RID per unique texture/program).
    // Resumable opaque/fog emitters keep empty identity RIDs; fog registration
    // survives with the material. Surfaces and packing buffers still retire.
    void update_emitter(std::uint64_t resource, const particles::VertexStream& stream) override;
    void empty_emitter(std::uint64_t resource, bool terminal) override;
    void destroy_emitter(std::uint64_t resource) override;
    // Battle backends may retain retired transparent non-fog emitters hidden, bounded by
    // entry and charged GPU-byte caps. Disable to drain before explicit teardown.
    // Every new owner still gets a fresh public resource ID and simulation state.
    void set_emitter_reuse(bool enabled);
    struct ReuseWork final {
        std::size_t retained_emitters{};
        std::uint64_t retained_bytes{};
        std::size_t peak_emitters{};
        std::uint64_t peak_bytes{}, hits{}, misses{}, refusals{};
        std::size_t live_rids{};
    };
    [[nodiscard]] ReuseWork reuse_work() const noexcept;
    [[nodiscard]] std::string failure_cause() const override;
    [[nodiscard]] particles::CullingFrame culling_frame() const override;
    // Lane latches (L1.2). Main snapshots what a lane thread cannot read itself and sets it before the
    // frame's particle work; std::nullopt (the default, and the inline path) reads the live value.
    // culling_frame() returns the latched camera planes; without a latch a second thread would read its
    // own empty thread_local camera map and fail open (every culled group would be built).
    void latch_culling(std::optional<particles::CullingFrame> frame) noexcept;
    // Replaces Engine::get_process_frames() in the residency logic (expire_reuse, sample_residency,
    // retry backoff, empty dwell).
    void latch_frame_number(std::optional<std::uint64_t> frame) noexcept;
    // The live values a latch snapshots; both must be called on the thread that owns the camera.
    [[nodiscard]] particles::CullingFrame current_culling() const;
    [[nodiscard]] static std::uint64_t current_process_frame();
    // True when any fog callback is set. A lane backend must have none (fog registration writes
    // renderer tables owned by the main thread).
    [[nodiscard]] bool uses_fog_callbacks() const noexcept;
    void set_emitter_visible(std::uint64_t resource, bool visible) override;
    void record_group_work(bool visible, bool prepared, bool stepped) override;
    [[nodiscard]] std::uint64_t incomplete_updates() const noexcept override;
    [[nodiscard]] std::uint64_t deferred_updates() const noexcept override;
    [[nodiscard]] bool overlapped_commit_safe() const noexcept override;
    // The scene light of bump-mapped emitters (PrimParticleBumpAlpha): light
    // 0's toward-light vector in the render (Y-up) world basis, its diffuse
    // and specular colours, and the SPH_LIGHT_FILL irradiance matrices in the
    // render basis (column-major, as GodotRenderer::LightingState::sph_fill).
    // Without a call the renderer's hemisphere fallback is used.
    struct BumpLighting final {
        std::array<float, 3> toward_light{};
        std::array<float, 3> diffuse{};
        std::array<float, 3> specular{};
        std::array<std::array<float, 16>, 3> fill{};
        friend bool operator==(const BumpLighting&, const BumpLighting&) = default;
    };
    [[nodiscard]] static BumpLighting default_bump_lighting() noexcept;
    // Applies to live and later emitters; repeating the current state is free.
    void set_lighting(const BumpLighting& lighting);

    // The material each emitter was created with. Particle materials pass the
    // same versioned MaterialDescription gate as every other renderer consumer.
    [[nodiscard]] static MaterialDescription material_for(const particles::EmitterRenderPlan& plan,
        bool fog = false);
    [[nodiscard]] static std::string_view adapter_name(particles::RenderFamily family) noexcept;
    [[nodiscard]] static std::string_view material_adapter_name(particles::Blend blend) noexcept;
    // PrimHeat's DistortionAmount: a heat texel offsets its scene sample by at
    // most this fraction of the screen, times texture alpha and vertex alpha.
    static constexpr float heat_distortion_amount = 0.01F;
    [[nodiscard]] static RenderPass pass_for(particles::DrawPhase phase) noexcept;

    // Every RenderingServer RID this backend currently owns (meshes, materials,
    // instances, shared textures and shaders). Zero after the last emitter is
    // destroyed and all reuse/prewarm holds are released is the no-leak evidence.
    [[nodiscard]] std::size_t live_rids() const noexcept;
    [[nodiscard]] std::size_t live_emitters() const noexcept; // resident render instances only
    [[nodiscard]] std::uint64_t suspensions() const noexcept;
    [[nodiscard]] std::uint64_t recreations() const noexcept;
    [[nodiscard]] std::uint64_t restore_asset_acquisitions() const noexcept;
    [[nodiscard]] std::uint64_t asset_acquisitions() const noexcept;
    [[nodiscard]] std::vector<FogEmitterEvidence> fog_emitter_evidence() const;

    // Opt-in frame profiling across particle backends. Conversion sums preparation time
    // on every worker, while submission is caller-thread time. Uploads include only
    // nonempty streams; replacements count newly allocated mesh surfaces.
    struct FrameWork final {
        std::uint64_t streams{};
        std::uint64_t uploads{};
        std::uint64_t replacements{};
        detail::ParticleCallCounts bounds_calls, visibility_calls;
        double conversion_ms{};
        double submission_ms{};
        double retirement_ms{};
        // PF lane-serial (b) step 1: what retirement_ms paid for. destroys = destroy() calls with a live emitter, of
        // them parked in the reuse cache; suspends = suspend() calls that retired draw state, of them terminal.
        std::uint64_t retire_destroys{}, retire_destroys_reused{}, retire_suspends{}, retire_suspends_terminal{};
        std::uint64_t groups{}, visible_groups{}, prepared_groups{}, stepped_groups{};
        // Opt-in (set_batch_census): merged-draw sizing, see detail::ParticleBatchCensus.
        detail::ParticleBatchCounts census;
        // Opt-in additive merge (set_merge_additive): groups drawn, streams and vertices in them,
        // surfaces replaced, and the caller time of GodotParticleBackend::flush_merged.
        std::uint64_t merged_groups{}, merged_streams{}, merged_vertices{}, merged_replacements{};
        double merge_flush_ms{};
        // Adds another thread's counters (the lane's thread_local FrameWork) to these. A frame that is not
        // measuring adds nothing, so a lane that ran with measurement off cannot double count.
        void merge(const FrameWork& other, const bool measuring) noexcept {
            if (!measuring) return;
            streams += other.streams;
            uploads += other.uploads;
            replacements += other.replacements;
            for (const auto& [target, source] : {std::pair{&bounds_calls, &other.bounds_calls},
                     std::pair{&visibility_calls, &other.visibility_calls}}) {
                target->attempted += source->attempted;
                target->sent += source->sent;
                target->skipped += source->skipped;
            }
            conversion_ms += other.conversion_ms;
            submission_ms += other.submission_ms;
            retirement_ms += other.retirement_ms;
            retire_destroys += other.retire_destroys;
            retire_destroys_reused += other.retire_destroys_reused;
            retire_suspends += other.retire_suspends;
            retire_suspends_terminal += other.retire_suspends_terminal;
            groups += other.groups;
            visible_groups += other.visible_groups;
            prepared_groups += other.prepared_groups;
            stepped_groups += other.stepped_groups;
            census.streams += other.census.streams;
            census.vertices += other.census.vertices;
            census.identical += other.census.identical;
            for (std::size_t kind = 0; kind < census.class_streams.size(); ++kind) {
                census.class_streams[kind] += other.census.class_streams[kind];
                census.class_keys[kind] += other.census.class_keys[kind];
            }
            merged_groups += other.merged_groups;
            merged_streams += other.merged_streams;
            merged_vertices += other.merged_vertices;
            merged_replacements += other.merged_replacements;
            merge_flush_ms += other.merge_flush_ms;
        }
    };
    // Opt-in per backend: additive streams sharing shader, colour texture and render priority are
    // drawn as one surface per group instead of one per emitter. The owner must call
    // flush_merged() once per frame after every particle update, or nothing is submitted.
    void set_merge_additive(bool enabled);
    static void flush_merged();
    static void set_merge_additive_default(bool enabled) noexcept;
    [[nodiscard]] static bool merge_additive_default() noexcept;
    // Process-wide switch for the census; off by default and free when off.
    static void set_batch_census(bool enabled) noexcept;
    static void begin_frame_measurement(bool enabled);
    [[nodiscard]] static FrameWork frame_work();
    // Adds counters measured on another thread (the lane's thread_local frame_work()) to the calling
    // thread's, so the trace reads the same totals as the inline path. No-op when not measuring.
    static void add_frame_work(const FrameWork& work);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace eawr::presentation::godot_backend
