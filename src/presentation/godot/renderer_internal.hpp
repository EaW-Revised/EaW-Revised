#pragma once
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/presentation/space/environment_scene.hpp"
#include "eawr/presentation/skin_pose.hpp"
#include "eawr/presentation/render_churn.hpp"
#include "eawr/presentation/hull_batching.hpp"

#include "diagnostic_buffer.hpp"
#include "fog_adapter.hpp"
#include "gpu_memory_ledger.hpp"
#include "instance_reconciliation.hpp"
#include "legacy/registry.hpp"
#include "missing_asset_waits.hpp"
#include "modern_shader_smoke.hpp"
#include "pass_submission.hpp"
#include "resource_lease_ledger.hpp"
#include "shader_adapter.hpp"
#include "stored_output.hpp"
#include "submission_plan.hpp"
#include "upload_identity.hpp"
#include "upload_identity_pool.hpp"
#include "texture_budget.hpp"
#include "model_digest_memo.hpp"
#include "model_recipe.hpp"
#include "texture_recipe.hpp"
#include "texture_census.hpp"
#include "resource_residency.hpp"
#include "upload_winding.hpp"
#include "distance_band.hpp"

namespace eawr::presentation::shader_material::routing { struct Surface; }

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

using namespace godot;

// Private declarations shared by the Godot renderer implementation files.
namespace eawr::presentation::godot_backend {

[[nodiscard]] std::string utf8(const String& value);
[[nodiscard]] Transform3D transform_from(const std::array<float, 16>& matrix);
[[nodiscard]] Projection godot_matrix(const SphChannelMatrix& matrix);
[[nodiscard]] Projection godot_matrix(const std::array<float, 16>& matrix);
[[nodiscard]] std::optional<std::string> shadow_receiving_variant(const std::string_view source);
[[nodiscard]] std::optional<std::vector<std::string>> shader_tokens(const std::string_view code);

struct ReflectedUniform final {
    std::string name;
    Variant::Type type{Variant::NIL};
    std::int64_t hint{};
    std::string hint_string;
};

struct CachedShader final {
    RID shader;
    std::vector<ReflectedUniform> uniforms;
    std::vector<std::string> tokens;
    ~CachedShader();
};

class GodotShaderCache::Impl final {
public:
    std::map<std::string, std::shared_ptr<const CachedShader>, std::less<>> entries;
    std::size_t source_bytes{};
};

[[nodiscard]] std::vector<ReflectedUniform> reflected_uniforms(RenderingServer& rendering, const RID& shader);
[[nodiscard]] std::optional<std::string> portable_limit_problem(
    const std::vector<ReflectedUniform>& uniforms, const std::vector<std::string>& tokens);

class GodotRenderer::Impl final {
    friend class GodotRenderer;
    friend struct detail::ModelRecipeProbe;
    friend struct detail::RecipeMemoryProbe;
public:
    using FogSelection = GodotRenderer::FogSelection;
    void set_texture_loader(detail::TextureLoader loader) { texture_loader_ = std::move(loader); }
    void set_model_loader(detail::ModelLoader loader) { model_loader_ = std::move(loader); }

    enum class MaterialUpload { success, shader_compile_failed, refused, failed };
    // Why upload_material refused a compiled shader (MaterialUpload::refused).
    struct MaterialRefusal final {
        std::string_view code;
        std::string_view subject; // "shader" or "material"
        std::string reason;
    };

    struct Resource final {
        struct Recipe final {
            detail::ModelRecipe model;
            std::shared_ptr<detail::TextureRecipe> texture;
            std::vector<std::pair<std::string, std::shared_ptr<detail::TextureRecipe>>> bindings;
        };
        RID mesh;
        std::uint64_t category_triangles{};
        std::uint64_t diagnostic_model_meshes{}, diagnostic_model_submeshes{};
        std::vector<std::string> diagnostic_surface_names;
        std::optional<AABB> diagnostic_bounds;
        // Rest-pose model-space bounds (all visible surfaces); sizes a batch group's culling box.
        std::optional<AABB> batch_bounds;
        RID texture;
        // Per-binding textures (upload with BindingTexture entries), by name.
        std::vector<std::pair<std::string, RID>> binding_textures;
        RID shader;
        std::shared_ptr<const CachedShader> shared_shader;
        RID material;
        RenderPass pass{RenderPass::opaque};
        std::size_t bone_count{};
        std::vector<std::uint8_t> skin_used_bones;
        bool skinned{};
        bool hidden_geometry_collapses{};
        std::vector<std::uint32_t> billboard_modes;
        std::vector<std::int32_t> bone_parents;
        std::vector<Transform3D> bind_models;
        std::vector<float> billboard_distances;
        std::optional<Vector3> billboard_light;
        // Kept so an opt-in fog variant can be configured exactly like the
        // default material; neither is part of the asset's identity.
        MaterialDescription description;
        bool shadow_receiving{};
        // Shadows were on at upload, but the legacy source had no `unshaded`
        // render mode to replace, so the default shader is drawn instead.
        bool shadow_variant_failed{};
        // A repeated upload is a shared reference only for this identity.
        std::shared_future<detail::UploadIdentity> identity;
        // #2856: what this resource holds on the GPU, as ledger entries.
        std::vector<detail::GpuItem> gpu_items;
        // Admission-only state follows the existing frame fields, preserving their offsets.
        std::shared_ptr<const Recipe> recipe;
        std::optional<std::int32_t> priority{};
    };

    struct FogConsumer final {
        GodotRenderer::FogConsumerKind kind{GodotRenderer::FogConsumerKind::batch_mesh_gloss};
        fog::ConsumerId id{}; // 0 while detached
        RID shader{};         // owned BatchMeshGloss fog variant while attached
        RID material{};       // the material registered with the fog backend
    };
    struct ExternalFogConsumer final {
        RID material;
        RID shader;
        fog::ConsumerId id{};
    };

    // Exists only while fog is enabled. Teardown order is explicit (see
    // disable_fog): consumers leave the cache before any material RID is
    // freed, and the cache releases its textures before the backend dies.
    struct FogRuntime final {
        FogSelection selection;
        std::unique_ptr<GodotFogBackend> backend;
        std::unique_ptr<fog::TextureCache> cache;
        std::optional<sim::fog::FogGridSet> grids; // immutable; last submitted set
        std::optional<std::uint64_t> submitted_tick;
        std::optional<fog::SubmitAction> last_action;
        std::optional<core::Diagnostic> last_rejection;
        bool derive_legacy_stages{};
    };

    struct SharedIdleSkeleton final {
        RID rid;
        std::uint64_t group{};
        std::vector<animation::Matrix> palette;
        std::vector<std::uint8_t> valid_bones;
        ~SharedIdleSkeleton();
    };
    struct SharedIdlePlan final {
        std::size_t surfaces{};
        std::vector<std::uint8_t> used;
    };
    struct Instance final {
        sim::AssetId asset_id{};
        RID rid;
        RID skeleton;
        std::shared_ptr<SharedIdleSkeleton> shared_idle_skeleton;
        Transform3D object_transform;
        detail::PlacedPiece placement; // #888: what it last sent, and the submit that carried it
        bool visible{true};
        // Shadow policy the RID got when it was created. set_casts_shadows only affects instances
        // created afterwards, so batching keys on this value, never on the asset's current flag.
        bool created_casts{true};
        bool reserved_proxy{};
        bool applied_visible{true};
        std::uint64_t category_submit_frame{~std::uint64_t{0}};
        std::uint64_t category_surfaces{};
        render_categories::Role category_role{render_categories::Role::unknown};
        mutable bool category_casts{true};
        bool band_hidden{}, group_hidden{}, part_hidden{};
        mutable RID diagnostic_cast_rid;
        mutable RenderingServer::ShadowCastingSetting diagnostic_cast_setting{RenderingServer::SHADOW_CASTING_SETTING_ON};
        // Instanced hulls (opt-in): while batched the retained instance stays, hidden, and its
        // transform lives in a group's multimesh slot. Leaving the group just unhides it.
        bool batch_hidden{}, batch_dirty{}, translated_parameters{};
        std::uint32_t batch_group{}, batch_slot{};
    };

    struct PendingPose final {
        sim::AssetId asset_id{};
        std::vector<animation::Matrix> palette{};
        std::vector<Transform3D> model_transforms{};
        std::shared_ptr<const SkinPosePalette<Transform3D>> prepared;
        [[nodiscard]] const std::vector<animation::Matrix>& active_palette() const noexcept {
            return prepared ? prepared->palette : palette;
        }
        [[nodiscard]] const std::vector<Transform3D>& active_models() const noexcept {
            return prepared ? prepared->model_transforms : model_transforms;
        }
    };

    [[nodiscard]] core::Result<void> bind_skin_pose(sim::EntityId entity_id, sim::AssetId asset_id,
        const Resource& resource,
        std::shared_ptr<const SkinPosePalette<Transform3D>> prepared, std::uint64_t shared_idle_group = 0);
    [[nodiscard]] core::Result<void> set_prepared_skin_pose(sim::EntityId entity_id, sim::AssetId asset_id,
        std::shared_ptr<const SkinPosePalette<Transform3D>> prepared, std::uint64_t shared_idle_group) {
        if (!prepared) return failure(diagnostic_codes::invalid_skin_pose,
            "skin pose contains a non-finite palette matrix");
        const auto resource = resources_.find(asset_id);
        if (resource == resources_.end()) return failure(diagnostic_codes::missing_asset,
            "cannot bind a skin pose to missing renderer asset " + std::to_string(asset_id));
        if (!resource->second.skinned || prepared->palette.size() != resource->second.bone_count)
            return failure(diagnostic_codes::invalid_skin_pose,
                "skin pose bone count does not match the uploaded skinned asset");
        return bind_skin_pose(entity_id, asset_id, resource->second, std::move(prepared), shared_idle_group);
    }
    explicit Impl(Node3D& owner, std::shared_ptr<GodotShaderCache> shaders, bool attach_scene);
    bool census_registered_{}; // texture census switch was on at construction
    std::unordered_map<sim::EntityId, render_categories::Role> category_roles_;
    struct DiagnosticOwner final {
        std::uint64_t unit{};
        std::string model;
        render_categories::HullPart part{render_categories::HullPart::unclassified};
        std::string object, layer, categories, xml_type, kind;
    };
    std::unordered_map<sim::EntityId, DiagnosticOwner> diagnostic_owners_;
    std::unordered_map<sim::EntityId, std::optional<AABB>> diagnostic_world_bounds_;
    render_categories::Sample diagnostic_groups_{};
    // Opt-in cost bound only; no atlas, no production LOD or pose skipping.
    struct DiagnosticQuad final { RID rid; Transform3D transform; bool visible{}; };
    RID diagnostic_quad_mesh_, diagnostic_quad_shader_, diagnostic_quad_material_;
    std::unordered_map<std::uint64_t, DiagnosticQuad> diagnostic_quads_;
    std::unordered_set<std::uint64_t> diagnostic_impostor_units_;
    void update_diagnostic_impostors(RenderingServer& rendering);
    void update_diagnostic_instances(RenderingServer& rendering);
    render_categories::Source category_source_{this, [](const void* owner, render_categories::Sample& sample) {
        const auto& self = *static_cast<const Impl*>(owner);
        if (render_categories::ablation != "off") {
            sample.fighter_units += self.diagnostic_groups_.fighter_units;
            sample.fighter_impostor_candidates += self.diagnostic_groups_.fighter_impostor_candidates;
            sample.fighter_impostor_quads += self.diagnostic_groups_.fighter_impostor_quads;
            sample.fighter_hull_surfaces += self.diagnostic_groups_.fighter_hull_surfaces;
            sample.fighter_attachment_surfaces += self.diagnostic_groups_.fighter_attachment_surfaces;
            sample.fighter_effect_surfaces += self.diagnostic_groups_.fighter_effect_surfaces;
            sample.fighter_requested_casters += self.diagnostic_groups_.fighter_requested_casters;
            sample.eligible_units += self.diagnostic_groups_.eligible_units;
            sample.compatible_groups += self.diagnostic_groups_.compatible_groups;
            sample.grouped_units += self.diagnostic_groups_.grouped_units;
            sample.potential_colour_saving += self.diagnostic_groups_.potential_colour_saving;
            sample.group_hidden_units += self.diagnostic_groups_.group_hidden_units;
            sample.t1_visible_pieces += self.diagnostic_groups_.t1_visible_pieces;
            sample.t1_visible_surfaces += self.diagnostic_groups_.t1_visible_surfaces;
            sample.t1_eligible_pieces += self.diagnostic_groups_.t1_eligible_pieces;
            sample.t1_groups += self.diagnostic_groups_.t1_groups;
            sample.t1_collapsed_surfaces += self.diagnostic_groups_.t1_collapsed_surfaces;
            sample.t1_collapsed_shadow_surfaces += self.diagnostic_groups_.t1_collapsed_shadow_surfaces;
            sample.t1_excluded_pass += self.diagnostic_groups_.t1_excluded_pass;
            sample.t1_excluded_billboard += self.diagnostic_groups_.t1_excluded_billboard;
            sample.t1_excluded_proxy += self.diagnostic_groups_.t1_excluded_proxy;
            sample.t1_excluded_pose += self.diagnostic_groups_.t1_excluded_pose;
        }
        self.batch_fill_sample(sample);
        for (const auto& [entity, instance] : self.instances_) {
            static_cast<void>(entity);
            sample.record(instance.category_role, instance.category_submit_frame == sample.frame,
                instance.category_casts, instance.category_surfaces,
                self.resources_.contains(instance.asset_id) ? self.resources_.at(instance.asset_id).category_triangles : 0,
                instance.category_surfaces);
            if (render_categories::ablation != "off") {
                sample.band_hidden_instances += instance.band_hidden ? 1U : 0U;
                if (instance.category_role == render_categories::Role::hull) {
                    const auto provenance = self.diagnostic_owners_.find(entity);
                    const auto part = provenance == self.diagnostic_owners_.end()
                        ? render_categories::HullPart::unclassified : provenance->second.part;
                    if (part == render_categories::HullPart::turret) sample.declared_turret_surfaces += instance.category_surfaces;
                    else if (part == render_categories::HullPart::body) sample.other_hull_surfaces += instance.category_surfaces;
                    else sample.unclassified_hull_surfaces += instance.category_surfaces;
                    const auto resource = self.resources_.find(instance.asset_id);
                    if (resource != self.resources_.end()) for (const auto& name : resource->second.diagnostic_surface_names)
                        sample.named_hull_surfaces += name.empty() ? 0U : 1U;
                }
            }
        }
    }, [](const void* owner) { return static_cast<std::uint64_t>(static_cast<const Impl*>(owner)->viewport_.get_id()); }};
    bool attach_scene_{true};

    ~Impl();

    void apply_lighting_params(RenderingServer& rendering, const RID& material) const;
    void apply_ambient_params(RenderingServer& rendering, const RID& material) const;
    void set_ambient_light(const std::array<float, 3>& rgb);

    void set_lighting(const GodotRenderer::LightingState& lighting);
    [[nodiscard]] const std::optional<GodotRenderer::LightingState>& lighting() const noexcept { return lighting_; }
    void apply_wind_params(RenderingServer& rendering, const RID& material, const MaterialDescription& source) const;
    void set_wind(const GodotRenderer::WindState& wind);

    void set_shadows_enabled(const bool enabled);
    void set_scene_bloom(const std::optional<lighting::bloom::SceneBloom>& bloom);
    [[nodiscard]] bool scene_bloom_active() const noexcept { return scene_bloom_active_; }

    void set_casts_shadows(const sim::AssetId asset_id, const bool casts);

    // Live counts over registered resources: a failed upload is not counted
    // and a released resource is subtracted.
    [[nodiscard]] std::size_t shadow_receiving_materials() const noexcept { return shadow_receiving_; }
    [[nodiscard]] std::size_t shadow_variant_failures() const noexcept { return shadow_variant_failures_; }

    [[nodiscard]] core::Result<void> enable_fog(const GodotRenderer::FogOptions& options);

    void disable_fog();

    void set_fog_team(const std::uint32_t team);

    void reset_fog_stream(const std::uint64_t stream);

    [[nodiscard]] core::Result<void> declare_fog_consumer(const sim::AssetId asset_id);

    [[nodiscard]] core::Result<GodotRenderer::ExternalFogHandle> register_external_fog_material(
        const RID& material, const RID& shader);

    void unregister_external_fog_material(const GodotRenderer::ExternalFogHandle handle);

    [[nodiscard]] std::vector<GodotRenderer::ExternalFogEvidence> external_fog_evidence() const;

    [[nodiscard]] GodotRenderer::FogStatus fog_status() const;

    [[nodiscard]] std::vector<GodotRenderer::FogConsumerEvidence> fog_consumer_evidence() const;

    [[nodiscard]] core::Result<void> upload(
        const sim::AssetId asset_id,
        const ModelView& model,
        const assets::Texture& texture,
        const MaterialDescription& description,
        const std::span<const GodotRenderer::BindingTexture> binding_textures = {},
        ModelUpload retained_model = {});
    [[nodiscard]] core::Result<void> upload_resource(sim::AssetId asset_id, const ModelView& model,
        const assets::Texture& texture, const MaterialDescription& description,
        std::span<const GodotRenderer::BindingTexture> binding_textures, ModelUpload retained_model);
    [[nodiscard]] bool batch_translated_asset(sim::AssetId asset) const;
    struct TranslationState;
    [[nodiscard]] core::Result<bool> upload_translation(sim::AssetId asset, const ModelView& model,
        const assets::Texture& texture, ModelUpload retained_model, const MaterialDescription* overrides = nullptr);
    std::shared_ptr<TranslationState> translation_;
    void set_translation_camera(const FixedCamera& camera);
    void retire_translation(sim::AssetId asset);
    [[nodiscard]] core::Result<std::unique_ptr<shader_material::routing::Surface>> prepare_translation(
        const assets::Submesh& surface, std::vector<GodotRenderer::BindingTexture>& resolved,
        std::span<const MaterialBinding> overrides = {});
    void update_translation(RenderingServer& rendering, sim::EntityId entity, Instance& instance, Resource& resource);

    [[nodiscard]] core::Result<void> retain(const sim::AssetId asset_id);

    [[nodiscard]] core::Result<void> release(const sim::AssetId asset_id);

    [[nodiscard]] std::vector<ResourceReference> resources() const {
        return leases_.resources();
    }

    [[nodiscard]] core::Result<void> set_skin_pose(
        const sim::EntityId entity_id,
        const sim::AssetId asset_id,
        const std::span<const animation::BonePose> bones);

    void begin_skin_batch(const bool enabled) {
        skin_work_ = {};
        skin_profile_ = enabled;
        skin_batch_active_ = true;
        skin_batch_.clear();
        shared_idle_batch_.clear();
        shared_idle_plans_.clear();
    }
    [[nodiscard]] SkinPoseWork end_skin_batch() {
        skin_profile_ = false;
        skin_batch_active_ = false;
        skin_batch_.clear();
        shared_idle_batch_.clear();
        shared_idle_plans_.clear();
        return skin_work_;
    }

    void clear_skin_pose(const sim::EntityId entity_id);
    void prepare_shared_idle_surface(std::uint64_t group, sim::EntityId entity_id, sim::AssetId asset_id);

    void set_billboard_light(const sim::AssetId asset_id, const std::array<float, 3>& toward_light);
    [[nodiscard]] core::Result<void> set_material_scalar(sim::AssetId asset_id, std::string_view binding, float value);
    [[nodiscard]] core::Result<void> set_material_vector3(sim::AssetId asset_id, std::string_view binding,
                                                         const std::array<float, 3>& value);
    [[nodiscard]] core::Result<void> set_material_priority(sim::AssetId asset_id, std::int32_t priority);
    void set_light_scale(sim::EntityId entity_id, const std::array<float, 3>& rgb);
    void apply_light_scale(RenderingServer& rendering, sim::EntityId entity_id, const Instance& instance) const;
    void set_unit_colorization(sim::EntityId entity_id, const std::array<float, 3>& rgb);
    void apply_unit_colorization(RenderingServer& rendering, sim::EntityId entity_id, const Instance& instance) const;
    void set_unit_opacity(sim::EntityId entity_id, float alpha);
    void copy_instance_pose(sim::EntityId source, sim::EntityId target, float light_factor);
    void forget_instance_pose(sim::EntityId entity_id);
    [[nodiscard]] std::vector<std::uint8_t> skin_geometry_bones(sim::AssetId asset_id) const;
    // Borrowed until the next renderer mutation on this thread; join workers before mutation.
    [[nodiscard]] std::span<const std::uint8_t> skin_geometry_bones_view(sim::AssetId asset_id) const;
    void apply_unit_opacity(RenderingServer& rendering, sim::EntityId entity_id, const Instance& instance) const;

    [[nodiscard]] std::vector<GodotRenderer::SkinBindingEvidence> skin_bindings() const;

    [[nodiscard]] std::vector<GodotRenderer::SubmissionEvidence> submission_evidence() const {
        return submission_evidence_;
    }

    [[nodiscard]] std::size_t instance_count() const noexcept { return instances_.size(); }

    [[nodiscard]] GodotRenderer::SubmitWork submit_work() const noexcept {
        return {work_.submits, work_.pieces, work_.orders_built, work_.order_sorts, work_.transforms_sent,
                work_.billboard_refreshes, work_.sweeps};
    }

    [[nodiscard]] GodotRenderer::LifecycleCounts lifecycle_counts() const noexcept;

    [[nodiscard]] std::vector<GodotRenderer::InstanceEvidence> instance_evidence() const;
    [[nodiscard]] std::optional<std::array<float, 12>> bone_evidence(sim::EntityId entity, std::int32_t bone) const;
    void set_instance_visible(sim::EntityId entity, bool visible);
    static void apply_diagnostic_casting(RenderingServer& rendering, const Instance& instance,
                                         const RenderingServer::ShadowCastingSetting setting) {
        // The fighter traversal visits every retained instance each frame. Avoid
        // resending unchanged policy in that opt-in path; default calls retain
        // their original behaviour. Key by RID as well as policy so a copied
        // instance cannot carry a cached setting to newly allocated geometry.
        if (instance.diagnostic_cast_rid == instance.rid && instance.diagnostic_cast_setting == setting) return;
        instance.diagnostic_cast_rid = instance.rid;
        instance.diagnostic_cast_setting = setting;
        rendering.instance_geometry_set_cast_shadows_setting(instance.rid, setting);
    }
    static void apply_visibility(RenderingServer& rendering, Instance& instance, bool requested) {
        const bool applied = render_categories::engine_visible(instance.category_role, requested)
            && !(instance.band_hidden && !render_categories::ablation.starts_with("shadows-band-"))
            && !instance.group_hidden && !instance.part_hidden && !instance.batch_hidden;
        if (instance.applied_visible != applied) {
            rendering.instance_set_visible(instance.rid, applied);
            instance.applied_visible = applied;
        }
        instance.visible = requested;
    }
    void set_render_category(sim::EntityId entity_id, render_categories::Role role);
    void write_churn_census(std::ostream& output) const;

    void set_camera(const FixedCamera& camera);

    void submit(std::shared_ptr<const sim::RenderSnapshot> snapshot);

    [[nodiscard]] core::Result<CaptureResult> capture(const FixedCamera& camera);

    [[nodiscard]] BackendInfo backend_info() const;

    [[nodiscard]] std::span<const core::Diagnostic> diagnostics() const {
        return diagnostics_.entries();
    }

    [[nodiscard]] core::Result<void> register_shared_texture(
        const std::string_view name, const assets::Texture& texture);

    [[nodiscard]] core::Result<void> register_shared_texture_array(
        const std::string_view name, const std::span<const assets::Texture> layers, const std::uint32_t edge);

    [[nodiscard]] std::size_t shared_texture_count() const noexcept { return shared_textures_.size(); }

private:
    [[nodiscard]] fog::StreamTeam fog_key() const noexcept {
        return {fog_->selection.stream, fog_->selection.team};
    }

    void apply_fog();

    [[nodiscard]] static std::optional<std::string> fog_uniform_problem(
        RenderingServer& rendering, const RID& shader);

    [[nodiscard]] static std::optional<std::string> derived_fog_source(const Resource& resource);

    [[nodiscard]] static std::optional<GodotRenderer::FogConsumerKind> fog_consumer_kind(
        RenderingServer& rendering, const Resource& resource, const bool derive_legacy,
        std::string& reason);

    void set_surface_material(RenderingServer& rendering, const RID& mesh, const RID& material) const;

    [[nodiscard]] std::optional<core::Diagnostic> attach_fog_consumer(
        const sim::AssetId asset_id, FogConsumer& consumer);

    void detach_fog_consumer(const sim::AssetId asset_id, FogConsumer& consumer);

    [[nodiscard]] std::optional<core::Diagnostic> attach_external_fog_consumer(ExternalFogConsumer& consumer);

    void detach_external_fog_consumer(ExternalFogConsumer& consumer);

    [[nodiscard]] core::Diagnostic make_diagnostic(
        const std::string_view code, std::string message) const;

    void fail(const std::string_view code, std::string message) {
        diagnostics_.push(make_diagnostic(code, std::move(message)));
    }

    [[nodiscard]] core::Diagnostic pushed(const std::string_view code, std::string message);

    [[nodiscard]] core::Result<void> failure(const std::string_view code, std::string message);

    // Instanced hulls (renderer_batching.cpp). All of this is inert unless hull_batching::enabled.
    struct BatchGroup final {
        RID multimesh, instance;
        sim::AssetId asset{};
        std::uint32_t capacity{}, members{};
        std::uint64_t surfaces{};
        bool casts{true}, dirty{};
        std::array<float, 4> local_sphere{};          // rest-pose centre xyz + radius
        std::vector<float> transforms;                // capacity * 12, row-major 3x4 per slot
        std::vector<std::array<float, 4>> spheres;    // per-slot world bounding sphere
    };
    struct BatchCounters final { std::uint64_t flushes{}, uploads{}, audits{}, repairs{}, key_builds{}; };
    [[nodiscard]] bool batch_active() const noexcept;
    void batch_mark(sim::EntityId entity);
    void batch_mark_asset(sim::AssetId asset);
    void batch_write_transform(sim::EntityId entity, const Instance& instance);
    void batch_update(RenderingServer& rendering, bool advance = true);
    void batch_forget(RenderingServer* rendering, sim::EntityId entity, Instance& instance);
    void batch_shutdown(RenderingServer& rendering);
    void batch_apply(RenderingServer& rendering, std::vector<hull_batching::Registry::Change> changes);
    [[nodiscard]] std::optional<hull_batching::Key> batch_key(sim::EntityId entity, const Instance& instance,
                                                              bool* pose_exit = nullptr) const;
    [[nodiscard]] bool rest_pose(sim::EntityId entity, const Instance& instance, const Resource& resource) const;
    void batch_fill_sample(render_categories::Sample& sample) const;
    hull_batching::Registry batch_registry_;
    std::unordered_map<std::uint32_t, BatchGroup> batch_groups_;
    std::vector<sim::EntityId> batch_dirty_;
    std::uint64_t batch_serial_{};
    mutable BatchCounters batch_counters_;

    using InstanceMap = std::unordered_map<sim::EntityId, Instance>;
    InstanceMap::iterator remove_instance(RenderingServer* rendering, const InstanceMap::iterator instance);

    [[nodiscard]] static bool apply_skin_pose(
        RenderingServer& rendering, const Instance& instance, const PendingPose& pose,
        std::span<const std::uint8_t> changes = {}, SkinPoseWork* work = nullptr);

    void refresh_billboards(RenderingServer& rendering, const sim::EntityId entity,
                            const Instance& instance, const Resource& resource);

    static void bind_instance_skin(
        RenderingServer& rendering,
        Instance& instance,
        const Resource& resource,
        const PendingPose* pose);

    // #2856: the pooled texture for this content, created on first use; its
    // item (one pool reference) is appended to `items`.
    [[nodiscard]] bool acquire_texture(RenderingServer& rendering, const assets::Texture& source, RID& target,
        std::vector<detail::GpuItem>& items);
    [[nodiscard]] bool acquire_texture(RenderingServer& rendering, const detail::TextureRecipe& recipe, RID& target,
        std::vector<detail::GpuItem>& items);
    void release_texture(RenderingServer& rendering, const detail::GpuItem& item);
    void note_shared_census(detail::TextureOwnerKind owner_kind, const std::string& name,
        const detail::GpuItem& item, const assets::Source* source);
    // `drop` top mips are left out (GM-04); 0 uploads the whole stored chain.
    [[nodiscard]] static RID create_texture(RenderingServer& rendering, const assets::Texture& source,
        std::uint32_t drop = 0);
    // GM-04: rebuild pooled textures at the current budget drop, in place, at most `limit` of
    // them; true when none is left above it.
    bool apply_texture_drop(RenderingServer& rendering, std::size_t limit = ~std::size_t{});
    // GM-04: once a frame: finish a pending drop a few textures at a time, else measure the
    // device when due (process-wide cadence) and raise the drop when over budget.
    void enforce_texture_budget(RenderingServer& rendering);
    // The engine's frames drawn, which stamps texture releases for the budget (0 without an engine).
    [[nodiscard]] static std::uint64_t drawn_frame();
    detail::TextureDropProgress texture_drop_;

    [[nodiscard]] static bool shader_compiled(
        RenderingServer& rendering, const RID& shader, const StringName& declared);

    [[nodiscard]] static std::optional<std::string> with_compile_probe(std::string source);

    [[nodiscard]] static std::string compile_failure_message(
        const MaterialDescription& source, const bool shadow_receiving, const sim::AssetId asset_id);

    [[nodiscard]] static std::string material_label(const MaterialDescription& source, const bool shadow_receiving);

    [[nodiscard]] static std::optional<std::string_view> legacy_shader_source(
        const MaterialDescription& source);

    [[nodiscard]] MaterialUpload upload_material(
        RenderingServer& rendering,
        const MaterialDescription& source,
        Resource& target,
        MaterialRefusal& refusal);

    void configure_material(
        RenderingServer& rendering,
        const MaterialDescription& source,
        const RID& material,
        const RID& texture,
        const std::vector<std::pair<std::string, RID>>& binding_textures) const;

    // With authored_binormals every vertex also carries its binormal as
    // CUSTOM0 coefficients (legacy::binormal_coefficients).
    [[nodiscard]] bool upload_mesh(RenderingServer& rendering, const ModelView& source, Resource& target,
        bool authored_binormals = false, bool resident = true);
    [[nodiscard]] bool admit_resource(RenderingServer& rendering, sim::AssetId asset, Resource& resource);
    void evict_resource(RenderingServer& rendering, sim::AssetId asset, Resource& resource);
    void reconcile_residency(RenderingServer& rendering, std::span<const sim::RenderInstance> instances);
    std::shared_ptr<detail::TextureRecipe> keep_texture(const assets::Texture& texture, const detail::GpuItem& item);

    [[nodiscard]] static bool shared_name(const std::string& value) {
        return value.starts_with("shared:");
    }
    [[nodiscard]] static std::string shared_key(const std::string& value) {
        return value.substr(std::string_view("shared:").size());
    }

    void free_resource(RenderingServer& rendering, const Resource& resource);

    Node3D& owner_;
    std::shared_ptr<GodotShaderCache> shader_cache_;
    RID scenario_;
    RID viewport_;
    RID camera_;
    Transform3D view_transform_;
    RID environment_;
    stored_output::Compositor stored_compositor_;
    // Declared before resources so it also outlives their digest futures.
    std::unique_ptr<detail::UploadIdentityPool> upload_identity_pool_;
    std::map<sim::AssetId, Resource> resources_;
    std::map<std::string, RID> shared_textures_;
    std::vector<detail::GpuItem> shared_gpu_items_;
    // Texture census only (switch on): owner class and name of each shared registration.
    std::vector<std::tuple<detail::TextureOwnerKind, std::string, detail::GpuItem>> shared_census_;
    struct PooledTexture final {
        RID rid;
        std::uint64_t references{};
        // GM-04: the ledger item as resident now, the top mips left out, and the source's mip
        // edges and byte counts, so a later drop knows what each level costs.
        detail::GpuItem item;
        std::uint32_t dropped{};
        std::vector<std::array<std::uint32_t, 2>> mip_edges;
        std::vector<std::uint64_t> mip_bytes;
        bool rebuild_failed{};  // the engine refused a rebuild; not retried
    };
    std::map<std::pair<std::uint64_t, std::uint64_t>, PooledTexture> texture_pool_;
    std::uint64_t unshared_textures_{};
    std::optional<GodotRenderer::LightingState> lighting_;
    std::optional<std::array<float, 3>> ambient_;
    std::optional<GodotRenderer::WindState> wind_;
    bool scene_bloom_active_{};
    RID sun_light_;
    RID sun_instance_;
    std::set<sim::AssetId> non_casting_;
    std::size_t shadow_receiving_{};
    std::size_t shadow_variant_failures_{};
    detail::ResourceLeaseLedger leases_;
    std::unordered_map<sim::EntityId, Instance> instances_;
    struct ProxyReservations {
        PayloadBudget fallback;
        PresentationBudget* budget{&fallback};
        std::unordered_map<sim::EntityId, std::size_t> charges;
        ~ProxyReservations() {
            for (const auto& [entity, bytes] : charges) {
                static_cast<void>(entity);
                budget->release(ResourceKind::gpu_proxy, bytes);
            }
        }
    } proxy_;
    std::unordered_map<sim::EntityId, PendingPose> skin_poses_;
    std::vector<std::uint8_t> skin_changes_;
    SkinPoseWork skin_work_{};
    bool skin_profile_{};
    SkinPoseBatch<Transform3D> skin_batch_;
    bool skin_batch_active_{};
    // Each idle index has exactly one prepared palette object in a batch;
    // pointer identity therefore identifies its placement for this batch only.
    std::unordered_map<const SkinPosePalette<Transform3D>*, std::shared_ptr<SharedIdleSkeleton>> shared_idle_batch_;
    std::unordered_map<std::uint64_t, SharedIdlePlan> shared_idle_plans_;
    static void detach_shared_idle(RenderingServer& rendering, Instance& instance);
    static void release_instance_skeleton(RenderingServer* rendering, Instance& instance);
    BillboardPoseScratch<Transform3D> billboard_pose_;
    // Per-entity light scale RGB (set_light_scale); absent means (1, 1, 1).
    std::unordered_map<sim::EntityId, std::array<float, 3>> light_scales_;
    // Allocated once per coloured piece, retained while capture progress changes.
    std::unordered_map<sim::EntityId, std::array<float, 3>> colorizations_;
    // Per-entity opacity (set_unit_opacity); absent means 1. Geometry transparency shares the
    // smooth blend across every material (space-fog-presentation.md FW-18, FW-19).
    std::unordered_map<sim::EntityId, float> opacities_;
    detail::MissingAssetWaits missing_waits_;
    std::vector<GodotRenderer::SubmissionEvidence> submission_evidence_;
    // #888: the submission state kept between frames. The order's resource
    // pointers stay valid while upload_generation_ is unchanged (every upload
    // and release of resources_ advances it).
    detail::SubmissionOrder order_;
    std::vector<Resource*> order_resources_;
    detail::SubmitWork work_;
    std::uint64_t upload_generation_{};
    std::uint64_t submit_serial_{};
    std::uint64_t billboard_generation_{};
    std::uint64_t billboard_applied_generation_{};
    detail::DiagnosticBuffer diagnostics_;
    std::map<sim::AssetId, FogConsumer> fog_consumers_;
    std::map<GodotRenderer::ExternalFogHandle, ExternalFogConsumer> external_fog_consumers_;
    GodotRenderer::ExternalFogHandle next_external_fog_handle_{1};
    std::map<sim::AssetId, std::string> fog_unsupported_;
    std::optional<FogRuntime> fog_;
    // Keep the eager frame state at its original offsets. Fitting submits only read the flag.
    detail::ResourceResidency residency_;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::weak_ptr<detail::TextureRecipe>> texture_recipes_;
    detail::TextureLoader texture_loader_;
    detail::ModelLoader model_loader_;
    detail::ModelRecipe::Batch admission_model_;
    detail::ModelRecipeStats model_stats_;
    // The digest of each shared model object, so the surfaces of one model hash it once.
    detail::ModelDigestMemo model_digests_;
    [[nodiscard]] detail::UploadIdentity model_digest(const std::shared_ptr<const assets::Model>& model);
    std::size_t model_reload_failures_{};
};

} // namespace eawr::presentation::godot_backend
