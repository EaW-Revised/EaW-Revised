#pragma once

#include "eawr/presentation/renderer.hpp"
#include "eawr/presentation/skin_pose_profile.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/fog/fog.hpp"
#include "eawr/presentation/lighting/scene_bloom.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace godot {
class Node3D;
class RID;
}

namespace eawr::presentation::godot_backend {

// Mode/setup-owned immutable shader acceptance. Exact backend source is the key;
// materials, bindings, textures and all mutable renderer state remain local.
// A full cache falls back to ordinary resource ownership, without eviction.
class GodotShaderCache final {
public:
    static constexpr std::size_t entry_limit = 64;
    static constexpr std::size_t source_byte_limit = 1024 * 1024;
    struct Stats final { std::size_t entries{}, source_bytes{}; };
    GodotShaderCache();
    ~GodotShaderCache();
    [[nodiscard]] Stats stats() const noexcept;
private:
    friend class GodotRenderer;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

namespace fog_diagnostic_codes {
// A declared fog consumer is not a supported fog-stub-v1 material.
inline constexpr std::string_view unsupported_consumer = "EAWR-FOG-0006";
} // namespace fog_diagnostic_codes

// Godot owns the window, graphics device, swapchain, resize dispatch and final
// presentation. This adapter owns only RenderingServer resources attached to
// the host node's viewport/scenario; one node bootstraps any number of entities.
class GodotRenderer final : public Renderer {
public:
    // A presentation overlay shares the scene without replacing its camera/environment.
    explicit GodotRenderer(godot::Node3D& host, std::shared_ptr<GodotShaderCache> shaders = {},
        bool attach_scene = true);
    ~GodotRenderer() override;

    GodotRenderer(GodotRenderer&&) noexcept;
    GodotRenderer& operator=(GodotRenderer&&) noexcept;

    // Uploading identical render content under a live asset ID adds a lease
    // to the same RID bundle. Different model, texture or material content
    // fails with EAWR-RENDER-0002 and changes nothing; replacing an asset is
    // release (to the last lease) followed by upload. The identity covers
    // only the uploaded content, not renderer state chosen at upload time:
    // after set_lighting enables shadows, an identical re-upload of a live
    // asset shares the bundle compiled without the shadow-receiving variant.
    //
    // After Godot's compiler accepts the shader, and before a material RID
    // exists, the compiled shader is admitted against its reflected uniforms
    // (#22). It must build on every Vulkan driver and, for the Compatibility
    // fallback, on every GL 3.3 driver: at most five material samplers (the
    // Compatibility renderer binds its own samplers at the top ten of the 16
    // guaranteed texture units) and at most 16384 bytes of std140 material
    // uniforms (the Vulkan and GL 3.3 minimum); otherwise EAWR-RENDER-0007. Every binding must
    // name a declared uniform, once, of a kind Godot applies unchanged (an
    // integer to int/float/bool, a scalar to float, a float3 to vec3, vec2 or
    // a source_color vec3, a float4 to vec4 or a source_color vec4, a texture
    // to one sampler), and every material sampler must be bound; otherwise
    // EAWR-RENDER-0001. The legacy route's bindings are the ALO's authored
    // effect parameters: one its adapter does not declare is not consumed,
    // and the renderer binds BaseTexture itself. The fog texture and
    // engine-supplied samplers (hint_screen_texture, hint_depth_texture,
    // hint_normal_roughness_texture) need no binding. A refused upload frees
    // every RID it created and leaves the registry unchanged.
    [[nodiscard]] core::Result<void> upload(
        sim::AssetId asset_id,
        const assets::Model& model,
        const assets::Texture& texture,
        const MaterialDescription& material) override;
    // A texture for one named texture-valued binding of a material (P1 #27:
    // Planet.fx samples BaseTexture and CloudTexture). Every other
    // texture-valued binding keeps the upload's primary texture, except a
    // legacy family's own extra samplers (the bump colorize NormalTexture,
    // #199): without their entry the upload fails with EAWR-RENDER-0001.
    struct BindingTexture final {
        std::string binding;
        assets::Texture texture;
    };
    // upload() with per-binding textures. Each entry must name a distinct
    // texture-valued binding of `material`, or the upload fails with
    // EAWR-RENDER-0001 and creates nothing. The upload identity covers every
    // bound texture; with no entries this is exactly upload() above.
    [[nodiscard]] core::Result<void> upload(
        sim::AssetId asset_id,
        const assets::Model& model,
        const assets::Texture& texture,
        const MaterialDescription& material,
        std::span<const BindingTexture> binding_textures);
    // Shared material textures (P1-06 terrain layers, sky clouds). A binding
    // whose string value is "shared:<name>" binds the named shared texture
    // instead of the asset's own texture; an upload naming an unregistered
    // one fails with EAWR-RENDER-0005 and creates nothing. Shared textures
    // are owned by the renderer, live until it is destroyed and cannot be
    // replaced. An array's layers are decompressed to RGBA8, resized to
    // `edge` x `edge` and given a full mip chain, so layers of different
    // sizes and block formats fit one Texture2DArray.
    [[nodiscard]] core::Result<void> register_shared_texture(
        std::string_view name, const assets::Texture& texture);
    [[nodiscard]] core::Result<void> register_shared_texture_array(
        std::string_view name, std::span<const assets::Texture> layers, std::uint32_t edge);
    [[nodiscard]] std::size_t shared_texture_count() const noexcept;

    // #2856: GPU memory accounting for the whole process (every renderer).
    // `live` resources hold `live_bytes`; `unique` distinct contents among
    // them would hold `unique_bytes` if each were uploaded once; `reuses`
    // counts requests served by a resident copy instead. Bytes are
    // this adapter's estimates; the device_* fields are Godot's own counters.
    struct GpuMemoryCategory final {
        std::uint64_t live{}, live_bytes{}, unique{}, unique_bytes{};
        std::uint64_t compressed_bytes{}, uncompressed_bytes{}, uploads{}, reuses{};
    };
    // #3027 GM-04: the adaptive texture budget. `device_bytes` is the device-local memory the
    // budget was taken from (0: unknown, unconstrained) and `source` where it came from;
    // `texture_budget` is what the pool may hold now (nine tenths of the device less
    // `other_bytes`, the device memory outside the pool), `resident_bytes` what it holds;
    // `drop` top mips are left out of every texture now, at least `minimum_drop` (the detail
    // setting); `downsized` textures were rebuilt smaller after their upload; `over_budget`
    // checks (uploads and frames) still found the pool over budget at the deepest drop;
    // `rebuild_failures` rebuilds the engine refused (those textures keep their older drop);
    // `constrained` is whether a device budget applies at all (texture_budget means nothing
    // without one).
    struct TextureBudgetFacts final {
        std::uint64_t device_bytes{};
        std::string source;
        std::uint64_t texture_budget{}, other_bytes{}, resident_bytes{};
        std::uint32_t drop{}, minimum_drop{};
        std::uint64_t downsized{}, over_budget{}, rebuild_failures{};
        bool constrained{};
    };
    struct GpuMemory final {
        GpuMemoryCategory textures, texture_arrays, meshes;
        std::uint64_t device_textures{}, device_buffers{}, device_total{};
        TextureBudgetFacts budget{};
        std::uint64_t deferred{}, admissions{}, evictions{}, dormant{}, admission_failures{};
    };
    [[nodiscard]] static GpuMemory gpu_memory();
    // Off: every texture upload creates its own GPU copy (the footprint
    // before content sharing), for measurement only. On by default.
    static void set_texture_sharing(bool enabled) noexcept;
    // GM-04: take the budget from `bytes` of device memory instead of the device's own figure
    // (a forced small budget for tests and small-card checks); 0 goes back to the device's.
    static void set_device_memory_override(std::uint64_t bytes);
    // GM-04 measurement switch: no budget, no device probe and no counter reads.
    static void disable_texture_budget();
    // GM-04: the texture detail setting: drop at least this many top mips (0..4) everywhere.
    static void set_texture_mip_drop(std::uint32_t levels);
    [[nodiscard]] core::Result<void> retain(sim::AssetId asset_id) override;
    [[nodiscard]] core::Result<void> release(sim::AssetId asset_id) override;
    [[nodiscard]] std::vector<ResourceReference> resources() const override;

    // Presentation-only pose state. The palette contains one asset-basis skin
    // matrix per model bone; the adapter performs the sole render-basis
    // conjugation and binds it to this entity's Godot skeleton RID.
    [[nodiscard]] core::Result<void> set_skin_pose(
        sim::EntityId entity_id,
        sim::AssetId asset_id,
        std::span<const animation::BonePose> bones);
    // Serial idle pose batch: each source span stays immutable until end.
    // End forgets source addresses; surface-owned immutable palettes survive.
    // Attribution is optional and never controls whether preparation is shared.
    void begin_skin_batch(bool enabled);
    [[nodiscard]] SkinPoseWork end_skin_batch();
    // Ends the entity's stored pose; a live skinned instance returns to its
    // rest palette. A pose survives the entity's absence from a snapshot (a
    // caller may pose once and resubmit later), so the renderer cannot tell
    // a retired entity from an absent one: a caller that retires entity IDs
    // must clear their poses, or the pose store grows with every posed ID.
    // Clearing an entity without a pose does nothing.
    void clear_skin_pose(sim::EntityId entity_id);
    // FW-26: freeze an independent instance's palette and RGB light state.
    // Geometry and authored team colour remain on the retained mesh asset.
    void copy_instance_pose(sim::EntityId source, sim::EntityId target, float light_factor);
    void forget_instance_pose(sim::EntityId entity_id);
    // Presentation scale for an individual billboard surface. Environment
    // glows may enlarge their authored quad to clear the opaque planet rim.
    // World-space direction toward light 0 for a mode-6 sunlight glow bone.
    void set_billboard_light(sim::AssetId asset_id, const std::array<float, 3>& toward_light);
    // Presentation-only: a new value for one scalar binding the asset's
    // material was uploaded with, such as an effect clock (#185). The upload
    // identity and leases are unchanged. A missing asset fails with
    // EAWR-RENDER-0003; a binding the material does not have, one that is not
    // a scalar, or a non-finite value fails with EAWR-RENDER-0001. A failure
    // changes nothing.
    [[nodiscard]] core::Result<void> set_material_scalar(sim::AssetId asset_id, std::string_view binding,
                                                       float value);
    // Same contract for an existing finite vector3 binding, including fog variants.
    [[nodiscard]] core::Result<void> set_material_vector3(sim::AssetId asset_id, std::string_view binding,
                                                         const std::array<float, 3>& value);
    // Presentation ordering for backdrop transparents, including any later fog variant.
    [[nodiscard]] core::Result<void> set_material_priority(sim::AssetId asset_id, std::int32_t priority);
    // Presentation-only: the entity's own light scale RGB (FoC's per-object
    // model light scale (the debug build), e.g. the shield colour flash), multiplied into the
    // lit diffuse of the legacy adapters through their per-instance
    // eawr_unit_light_scale. It holds while the entity is absent and applies
    // when its instance is (re)created; (1, 1, 1) ends it. Surfaces whose
    // shader has no such uniform are unchanged.
    void set_light_scale(sim::EntityId entity_id, const std::array<float, 3>& rgb);
    // WBP-50: stored RGB ownership colour, consumed only inside each shader's
    // colourise mask. Shared materials and noncolourising surfaces stay intact.
    // Holds across absence/recreation; unchanged values send no renderer update.
    void set_unit_colorization(sim::EntityId entity_id, const std::array<float, 3>& rgb);
    // Presentation-only: the entity's own opacity (#535, docs/behaviour/space-fog-presentation.md
    // FW-16 to FW-18: the local player's fog fading a unit in or out). It holds while the entity
    // is absent and applies when its instance is (re)created; 1 (fully opaque) ends it. Applied as
    // smooth geometry transparency, shared by hull and attachment materials (FW-19).
    void set_unit_opacity(sim::EntityId entity_id, float alpha);

    struct SkinBindingEvidence final {
        sim::EntityId entity_id{};
        sim::AssetId asset_id{};
        std::size_t bone_count{};
    };
    [[nodiscard]] std::vector<SkinBindingEvidence> skin_bindings() const;

    struct SubmissionEvidence final {
        sim::EntityId entity_id{};
        sim::AssetId asset_id{};
        RenderPass pass{RenderPass::opaque};
    };
    // Runtime qualification reads this after submit(). It is observed from the
    // same ordered work list that creates/updates RenderingServer instances;
    // it is not a restatement of render_pass_order.
    [[nodiscard]] std::vector<SubmissionEvidence> submission_evidence() const;
    [[nodiscard]] std::size_t instance_count() const noexcept;

    // #888: the submission work so far, as counts (never wall-clock). A
    // submit rebuilds the pass order only when the snapshot's pieces or the
    // uploads changed, and sends a piece's transform only when it moved.
    struct SubmitWork final {
        std::uint64_t submits{};
        std::uint64_t pieces{};
        std::uint64_t orders_built{};
        std::uint64_t order_sorts{};
        std::uint64_t transforms_sent{};
        std::uint64_t billboard_refreshes{};
        std::uint64_t sweeps{};
    };
    [[nodiscard]] SubmitWork submit_work() const noexcept;

    // Resource churn qualification (#22). The counts are this adapter's own
    // registry; instance_evidence() reads each live instance's mesh and
    // skeleton back from RenderingServer instead of restating bookkeeping.
    struct LifecycleCounts final {
        std::size_t assets{};
        std::size_t instances{};
        std::size_t skeletons{};
        // Stored poses, bound to an instance or pending one. A pose survives
        // its entity's absence from a snapshot and ends with its asset's
        // release, clear_skin_pose, or a replacing set_skin_pose.
        std::size_t skin_poses{};
        // Entities whose snapshot asset is not uploaded; each is reported
        // once as EAWR-RENDER-0003 when its wait starts, not every submit.
        std::size_t missing_asset_waits{};
    };
    [[nodiscard]] LifecycleCounts lifecycle_counts() const noexcept;
    struct InstanceEvidence final {
        sim::EntityId entity_id{};
        sim::AssetId asset_id{};
        std::int64_t mesh_surfaces{};
        std::int64_t skeleton_bones{};
        // Any skeleton bone transform differs from identity.
        bool skeleton_posed{};
        // The visibility the adapter last applied through set_instance_visible (RenderingServer has no getter).
        bool visible{true};
    };
    [[nodiscard]] std::vector<InstanceEvidence> instance_evidence() const;
    // GPU qualification: the live skeleton palette, read from RenderingServer.
    [[nodiscard]] std::optional<std::array<float, 12>> bone_evidence(sim::EntityId entity, std::int32_t bone) const;
    // Hide a submitted instance without retiring its live skeleton/pose.
    void set_instance_visible(sim::EntityId entity, bool visible);
    void set_camera(const FixedCamera& camera);

    // Scene lighting (P1-04). Without a call to set_lighting every legacy
    // material keeps the frozen P0 hemisphere constants. With one, the three
    // irradiance matrices (render basis, column-major) are bound as
    // eawr_sph_r/g/b on every current and future material, and one
    // directional light travelling opposite `toward_light` is created. When
    // `shadows` is set before an upload, legacy materials uploaded afterwards
    // are compiled in a shadow-receiving variant: `unshaded` becomes
    // `ambient_light_disabled` and a light() function multiplies the surface
    // by mix(shadow_floor, 1, ATTENUATION) per channel. Under the stored-value
    // colour mode that multiplies stored values, like the retail stencil
    // darkening; the Compatibility fallback multiplies linear ones. The directional light's
    // shadow covers `shadow_max_distance` in a `shadow_atlas_size` atlas, either as
    // one orthogonal map or cascaded by view depth into parallel splits at
    // `shadow_split_offsets` (fractions of the distance), optionally blended
    // across split borders. The light's shadow bias and normal bias are set
    // only when given; unset, RenderingServer's own light defaults apply
    // unchanged (the behaviour before these fields existed). RenderingServer
    // cannot restore a default on a live light, so set_lighting replaces a
    // previously tuned light when a later state leaves either bias unset.
    enum class ShadowLayout : std::uint8_t { orthogonal, parallel_2_splits, parallel_4_splits };
    enum class ShadowFilter : std::uint8_t { soft_low, soft_medium, soft_high };
    struct LightingState final {
        std::array<std::array<float, 16>, 3> sph{};
        // SPH_LIGHT_FILL (the two fill lights and ambient, without the sun)
        // and the sun's diffuse colour, for adapters that light the sun per
        // pixel (the bump colorize DX9 technique, #199).
        std::array<std::array<float, 16>, 3> sph_fill{};
        std::array<float, 3> sun_diffuse{2.0F, 1.88F, 1.72F};
        std::array<float, 3> toward_light{0.0F, 1.0F, 0.0F};
        std::array<float, 3> specular{2.0F, 1.88F, 1.72F};
        bool shadows{};
        std::array<float, 3> shadow_floor{0.5F, 0.5F, 0.5F};
        float shadow_max_distance{4096.0F};
        std::int32_t shadow_atlas_size{4096};
        ShadowFilter shadow_filter{ShadowFilter::soft_low};
        ShadowLayout shadow_layout{ShadowLayout::orthogonal};
        std::array<float, 3> shadow_split_offsets{0.1F, 0.2F, 0.5F};
        bool shadow_blend_splits{};
        std::optional<float> shadow_bias;
        std::optional<float> shadow_normal_bias;
        // LIGHT_PARAM_SHADOW_BLUR. RenderingServer's default of 0 turns off
        // the soft filter and the depth bias, which Godot scales by it.
        std::optional<float> shadow_blur;
        // Independent ambient RGB, before spherical fill contributions (CEM-12).
        std::array<float, 3> ambient{0.08F, 0.08F, 0.10F};
    };
    void set_lighting(const LightingState& lighting);
    // Environment-only input for families that read independent ambient.
    // Preserves the current SH, sun, shadow policy and global atlas settings.
    void set_ambient_light(const std::array<float, 3>& rgb);
    // The state of the last set_lighting call; empty before the first. Viewer
    // adapters outside the renderer (bump-mapped particles) follow it.
    [[nodiscard]] const std::optional<LightingState>& lighting() const noexcept;
    // Foliage wind (#147): the scene wind vector (render basis) and the scene
    // clock in seconds, bound as eawr_wind and eawr_scene_time on every
    // current and future material of a legacy family whose vertex stage reads
    // them (Tree.fx, Grass.fx), fog variants included. Without a call those
    // materials keep their shader defaults: no wind and time 0. Repeating the
    // current state changes nothing.
    struct WindState final {
        std::array<float, 3> wind{};
        float scene_time{};
        friend constexpr bool operator==(const WindState&, const WindState&) noexcept = default;
    };
    void set_wind(const WindState& wind);
    // Runtime toggle of the directional shadow; materials are unchanged.
    void set_shadows_enabled(bool enabled);
    // Scene bloom (#201): the retail SceneBloom post pass with one
    // environment's parameters (docs/rendering.md#bloom), or none to turn it
    // off, which is the default. It exists only on a RenderingDevice backend;
    // on the Compatibility fallback the call changes nothing and
    // scene_bloom_active() stays false.
    void set_scene_bloom(const std::optional<lighting::bloom::SceneBloom>& bloom);
    [[nodiscard]] bool scene_bloom_active() const noexcept;
    // Instances of `asset_id` created afterwards do not cast shadows (for
    // example a skydome that encloses the scene). The flag belongs to the
    // asset ID, not to one upload: it survives release, so a release-then-
    // upload replacement keeps it until set_casts_shadows(asset_id, true).
    void set_casts_shadows(sim::AssetId asset_id, bool casts);
    // Registered legacy materials compiled in the shadow-receiving variant,
    // and those whose adapter source had no `unshaded` render mode to
    // replace. Both are live counts: a failed upload is not counted and a
    // final release subtracts its resource.
    [[nodiscard]] std::size_t shadow_receiving_materials() const noexcept;
    [[nodiscard]] std::size_t shadow_variant_failures() const noexcept;

    // Synthetic fog-stub-v1 binding (P1-07, #28/#32): harness policy, not retail
    // fog semantics. It is off by default. Until enable_fog() no fog resource
    // exists, snapshot fog grids are ignored and every material keeps its
    // exact default shader; disable_fog() returns to that state.
    //
    // While enabled, each submit() hands the snapshot's immutable grid set to
    // a renderer-owned fog::TextureCache for the explicit (stream, team)
    // selection, with that cache's revision, team and failure rules. A
    // missing selected grid unbinds every consumer (dark) and reports
    // `rejected`; another team is never substituted. The selection and all
    // fog resources are presentation state: they never change asset IDs,
    // leases or snapshots.
    //
    // Asset consumers are declared per uploaded asset. Supported are exactly:
    //  - legacy BatchMeshGloss.fx sph_t1/sph_t1_p0 (opaque), which is drawn
    //    with fixed_mesh_shader_opaque_fog while fog is enabled and with its
    //    unchanged default material otherwise;
    //  - legacy BatchMeshAlpha.fx sph_t1/sph_t1_p0 (transparent), which is
    //    drawn with fixed_mesh_shader_alpha_fog while fog is enabled and with
    //    its unchanged default material otherwise;
    //  - legacy BatchMeshGloss.fx (opaque) and BatchMeshAlpha.fx
    //    (transparent) sph_t0/sph_t0_p0 (#200), drawn with their derived
    //    fog variant (derived_legacy_fog_variant) while fog is enabled and
    //    with their unchanged default material otherwise;
    //  - modern_spatial materials whose compiled shader itself declares the
    //    fog uniforms (sampler2D eawr_fog_texture, vec2 eawr_fog_origin,
    //    eawr_fog_extent, eawr_fog_size, bool eawr_fog_bound). The texture
    //    must reflect as a Texture2D resource and be declared lexically as
    //    exactly one plain sampler2D; integer, cube, array and 3D samplers,
    //    and declarations hidden behind the preprocessor, are refused. No
    //    shader text is added to them;
    //  - only with FogOptions::derive_legacy_stages, declared while fog is
    //    enabled: every other accepted legacy adapter whose source
    //    derived_legacy_fog_variant accepts, drawn with that derived variant
    //    while fog is enabled and with its unchanged default otherwise.
    // Anything else is refused with EAWR-FOG-0006 and recorded. External
    // caller-owned materials use the same compiled-uniform validation, but
    // have opaque handles and never enter the asset/surface evidence path.
    //
    // While attached, an unbound consumer (awaiting a grid, missing team,
    // stream reset) is forced dark with explicit overrides (bound false, no
    // texture, neutral mapping), whatever its shader defaults or earlier
    // overrides say. Detaching (disable, release, destruction) restores the
    // fog uniform values the material had before it was attached.
    struct FogSelection final {
        std::uint64_t stream{};
        std::uint32_t team{};
        friend constexpr bool operator==(const FogSelection&, const FogSelection&) noexcept = default;
    };
    struct FogOptions final {
        FogSelection selection;
        // Largest fog texture edge accepted (see GodotFogBackend); a lower
        // value models a device limit.
        std::uint32_t max_texture_dimension{sim::fog::max_grid_dimension};
        // Land map fog (#28): accept every legacy adapter with a derived fog
        // stage, not only the fixed BatchMesh passes. Off keeps the exact
        // consumer set above.
        bool derive_legacy_stages{false};
    };
    enum class FogReadiness : std::uint8_t {
        disabled,      // fog mode off; no fog resources exist
        awaiting_grid, // enabled, but no grid set submitted for the selection yet
        ready,         // the selected grid is bound to every attached consumer
        rejected,      // the last fog submit failed; see FogStatus::last_rejection
    };
    enum class FogConsumerKind : std::uint8_t {
        batch_mesh_gloss,
        modern_spatial,
        batch_mesh_alpha,
        legacy_derived,
    };
    struct UnsupportedFogConsumer final {
        sim::AssetId asset_id{};
        std::string reason;
    };
    struct FogStatus final {
        FogReadiness readiness{FogReadiness::disabled};
        std::optional<FogSelection> selection;
        // Tick of the snapshot whose grid set was last handed to the cache.
        std::optional<std::uint64_t> submitted_tick;
        // Revision of the grid bound to the consumers, if any.
        std::optional<std::uint64_t> bound_revision;
        std::optional<fog::SubmitAction> last_action;
        std::optional<core::Diagnostic> last_rejection;
        // After a rejection: the previous binding of the same selection stays.
        bool binding_retained{};
        fog::CacheStats cache;
        std::size_t live_textures{};
        std::size_t attached_consumers{};
        std::size_t declared_consumers{}; // asset and external declarations
        std::size_t external_consumers{};
        std::vector<UnsupportedFogConsumer> unsupported;
        [[nodiscard]] bool ready() const noexcept { return readiness == FogReadiness::ready; }
    };
    // Observed from RenderingServer state, not from bookkeeping.
    struct FogConsumerEvidence final {
        sim::AssetId asset_id{};
        FogConsumerKind kind{FogConsumerKind::batch_mesh_gloss};
        bool attached{};
        std::size_t surfaces{};
        std::size_t surfaces_with_default_material{};
        std::size_t surfaces_with_fog_material{};
        // RenderingServer's code of the asset's own shader, and of the fog
        // variant while one is attached (fixed BatchMesh passes only).
        std::string default_shader_code{};
        std::string fog_shader_code{};
        // RenderingServer's material_get_param of each fog uniform on the
        // material the asset draws (the fog material while attached, else its
        // own), as "<Variant type>:<value>"; "Nil:<null>" means no override.
        std::vector<std::pair<std::string, std::string>> fog_parameters{};
    };
    using ExternalFogHandle = std::uint64_t;
    struct ExternalFogEvidence final {
        ExternalFogHandle handle{};
        bool attached{};
        bool bound{};
        std::vector<std::pair<std::string, std::string>> fog_parameters;
    };

    // Starts a fresh fog lifetime (a previous one is disabled first) and
    // attaches every declared consumer, unbound until a grid arrives. It is
    // all or nothing: if any consumer's fog variant fails (EAWR-RENDER-0007)
    // or an external consumer cannot attach, fog is left disabled, the
    // declarations are unchanged, every failure is in diagnostics() and the
    // first is returned.
    [[nodiscard]] core::Result<void> enable_fog(const FogOptions& options);
    void disable_fog();
    // Reapplies the last submitted grid set to the new team at once.
    void set_fog_team(std::uint32_t team);
    // Seek or new stream: releases the current stream's textures (revision 1
    // is accepted again) and selects `stream`; consumers are dark until the
    // next submit.
    void reset_fog_stream(std::uint64_t stream);
    // While fog is enabled the consumer attaches at once; if its fog variant
    // fails, the declaration fails with that EAWR-RENDER-0007 diagnostic and
    // the asset stays undeclared on its default material.
    [[nodiscard]] core::Result<void> declare_fog_consumer(sim::AssetId asset_id);
    // The caller owns both RIDs and must unregister before freeing either.
    // Declarations survive disable/enable for as long as those RIDs remain live.
    // A registration that cannot attach to enabled fog fails and is not kept.
    [[nodiscard]] core::Result<ExternalFogHandle> register_external_fog_material(
        const godot::RID& material, const godot::RID& shader);
    void unregister_external_fog_material(ExternalFogHandle handle);
    [[nodiscard]] std::vector<ExternalFogEvidence> external_fog_consumers() const;
    [[nodiscard]] FogStatus fog_status() const;
    [[nodiscard]] std::vector<FogConsumerEvidence> fog_consumers() const;

    void submit(std::shared_ptr<const sim::RenderSnapshot> snapshot) override;
    [[nodiscard]] core::Result<CaptureResult> capture(const FixedCamera& camera) override;
    [[nodiscard]] BackendInfo backend_info() const override;
    [[nodiscard]] std::span<const core::Diagnostic> diagnostics() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] constexpr std::string_view to_string(const GodotRenderer::ShadowFilter filter) noexcept {
    switch (filter) {
    case GodotRenderer::ShadowFilter::soft_low: return "soft_low";
    case GodotRenderer::ShadowFilter::soft_medium: return "soft_medium";
    case GodotRenderer::ShadowFilter::soft_high: return "soft_high";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(const GodotRenderer::ShadowLayout layout) noexcept {
    switch (layout) {
    case GodotRenderer::ShadowLayout::orthogonal: return "orthogonal";
    case GodotRenderer::ShadowLayout::parallel_2_splits: return "parallel_2_splits";
    case GodotRenderer::ShadowLayout::parallel_4_splits: return "parallel_4_splits";
    }
    return "unknown";
}

} // namespace eawr::presentation::godot_backend
