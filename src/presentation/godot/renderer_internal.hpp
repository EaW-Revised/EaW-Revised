#pragma once
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/presentation/space/environment_scene.hpp"

#include "diagnostic_buffer.hpp"
#include "fog_adapter.hpp"
#include "instance_reconciliation.hpp"
#include "legacy/registry.hpp"
#include "missing_asset_waits.hpp"
#include "modern_shader_smoke.hpp"
#include "pass_submission.hpp"
#include "resource_lease_ledger.hpp"
#include "shader_adapter.hpp"
#include "stored_output.hpp"
#include "upload_identity.hpp"
#include "upload_winding.hpp"

#include <godot_cpp/classes/global_constants.hpp>
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

[[nodiscard]] std::vector<ReflectedUniform> reflected_uniforms(RenderingServer& rendering, const RID& shader);
[[nodiscard]] std::optional<std::string> portable_limit_problem(
    const std::vector<ReflectedUniform>& uniforms, const std::vector<std::string>& tokens);

class GodotRenderer::Impl final {
public:
    using FogSelection = GodotRenderer::FogSelection;

    enum class MaterialUpload { success, shader_compile_failed, refused, failed };
    // Why upload_material refused a compiled shader (MaterialUpload::refused).
    struct MaterialRefusal final {
        std::string_view code;
        std::string_view subject; // "shader" or "material"
        std::string reason;
    };

    struct Resource final {
        RID mesh;
        RID texture;
        // Per-binding textures (upload with BindingTexture entries), by name.
        std::vector<std::pair<std::string, RID>> binding_textures;
        RID shader;
        RID material;
        RenderPass pass{RenderPass::opaque};
        std::size_t bone_count{};
        bool skinned{};
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
        detail::UploadIdentity identity{};
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

    struct Instance final {
        sim::AssetId asset_id{};
        RID rid;
        RID skeleton;
        Transform3D object_transform;
    };

    struct PendingPose final {
        sim::AssetId asset_id{};
        std::vector<animation::Matrix> palette{};
        std::vector<Transform3D> model_transforms{};
    };

    explicit Impl(Node3D& owner);

    ~Impl();

    void apply_lighting_params(RenderingServer& rendering, const RID& material) const;

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
        const assets::Model& model,
        const assets::Texture& texture,
        const MaterialDescription& description,
        const std::span<const GodotRenderer::BindingTexture> binding_textures = {});

    [[nodiscard]] core::Result<void> retain(const sim::AssetId asset_id);

    [[nodiscard]] core::Result<void> release(const sim::AssetId asset_id);

    [[nodiscard]] std::vector<ResourceReference> resources() const {
        return leases_.resources();
    }

    [[nodiscard]] core::Result<void> set_skin_pose(
        const sim::EntityId entity_id,
        const sim::AssetId asset_id,
        const std::span<const animation::BonePose> bones);

    void clear_skin_pose(const sim::EntityId entity_id);

    void set_billboard_light(const sim::AssetId asset_id, const std::array<float, 3>& toward_light);
    [[nodiscard]] core::Result<void> set_material_scalar(sim::AssetId asset_id, std::string_view binding, float value);
    void set_light_scale(sim::EntityId entity_id, const std::array<float, 3>& rgb);
    void apply_light_scale(RenderingServer& rendering, sim::EntityId entity_id, const Instance& instance) const;
    void set_unit_opacity(sim::EntityId entity_id, float alpha);
    void apply_unit_opacity(RenderingServer& rendering, sim::EntityId entity_id, const Instance& instance) const;

    [[nodiscard]] std::vector<GodotRenderer::SkinBindingEvidence> skin_bindings() const;

    [[nodiscard]] std::vector<GodotRenderer::SubmissionEvidence> submission_evidence() const {
        return submission_evidence_;
    }

    [[nodiscard]] std::size_t instance_count() const noexcept { return instances_.size(); }

    [[nodiscard]] GodotRenderer::LifecycleCounts lifecycle_counts() const noexcept;

    [[nodiscard]] std::vector<GodotRenderer::InstanceEvidence> instance_evidence() const;

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

    using InstanceMap = std::unordered_map<sim::EntityId, Instance>;
    InstanceMap::iterator remove_instance(RenderingServer* rendering, const InstanceMap::iterator instance);

    [[nodiscard]] static bool apply_skin_pose(
        RenderingServer& rendering, const Instance& instance, const PendingPose& pose);

    void refresh_billboards(RenderingServer& rendering, const sim::EntityId entity,
                            const Instance& instance, const Resource& resource);

    static void bind_instance_skin(
        RenderingServer& rendering,
        Instance& instance,
        const Resource& resource,
        const PendingPose* pose);

    [[nodiscard]] bool upload_texture(
        RenderingServer& rendering, const assets::Texture& source, RID& target);

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
    [[nodiscard]] bool upload_mesh(RenderingServer& rendering, const assets::Model& source, Resource& target,
        bool authored_binormals = false);

    [[nodiscard]] static bool shared_name(const std::string& value) {
        return value.starts_with("shared:");
    }
    [[nodiscard]] static std::string shared_key(const std::string& value) {
        return value.substr(std::string_view("shared:").size());
    }

    static void free_resource(RenderingServer& rendering, const Resource& resource);

    Node3D& owner_;
    RID scenario_;
    RID viewport_;
    RID camera_;
    Transform3D view_transform_;
    RID environment_;
    stored_output::Compositor stored_compositor_;
    std::map<sim::AssetId, Resource> resources_;
    std::map<std::string, RID> shared_textures_;
    std::optional<GodotRenderer::LightingState> lighting_;
    std::optional<GodotRenderer::WindState> wind_;
    bool scene_bloom_active_{};
    RID sun_light_;
    RID sun_instance_;
    std::set<sim::AssetId> non_casting_;
    std::size_t shadow_receiving_{};
    std::size_t shadow_variant_failures_{};
    detail::ResourceLeaseLedger leases_;
    std::unordered_map<sim::EntityId, Instance> instances_;
    std::unordered_map<sim::EntityId, PendingPose> skin_poses_;
    // Per-entity light scale RGB (set_light_scale); absent means (1, 1, 1).
    std::unordered_map<sim::EntityId, std::array<float, 3>> light_scales_;
    // Per-entity opacity (set_unit_opacity, #535); absent means 1 (fully opaque). Applied as an
    // instance shader parameter that the hull adapters dither with, not FoC's shader-level alpha
    // blend (space-fog-presentation.md FW-18).
    std::unordered_map<sim::EntityId, float> opacities_;
    detail::MissingAssetWaits missing_waits_;
    std::vector<GodotRenderer::SubmissionEvidence> submission_evidence_;
    detail::DiagnosticBuffer diagnostics_;
    std::map<sim::AssetId, FogConsumer> fog_consumers_;
    std::map<GodotRenderer::ExternalFogHandle, ExternalFogConsumer> external_fog_consumers_;
    GodotRenderer::ExternalFogHandle next_external_fog_handle_{1};
    std::map<sim::AssetId, std::string> fog_unsupported_;
    std::optional<FogRuntime> fog_;
};

} // namespace eawr::presentation::godot_backend
