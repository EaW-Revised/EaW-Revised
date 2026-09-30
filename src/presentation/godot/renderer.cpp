#include "eawr/presentation/godot/renderer.hpp"

#include "renderer_internal.hpp"

namespace eawr::presentation::godot_backend {

[[nodiscard]] std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), converted.length());
}

[[nodiscard]] Transform3D transform_from(const std::array<float, 16>& matrix) {
    return Transform3D(
        Basis(
            Vector3(matrix[0], matrix[1], matrix[2]),
            Vector3(matrix[4], matrix[5], matrix[6]),
            Vector3(matrix[8], matrix[9], matrix[10])),
        Vector3(matrix[12], matrix[13], matrix[14]));
}

[[nodiscard]] Projection godot_matrix(const SphChannelMatrix& matrix) {
    return Projection(
        Vector4(matrix.columns[0][0], matrix.columns[0][1], matrix.columns[0][2], matrix.columns[0][3]),
        Vector4(matrix.columns[1][0], matrix.columns[1][1], matrix.columns[1][2], matrix.columns[1][3]),
        Vector4(matrix.columns[2][0], matrix.columns[2][1], matrix.columns[2][2], matrix.columns[2][3]),
        Vector4(matrix.columns[3][0], matrix.columns[3][1], matrix.columns[3][2], matrix.columns[3][3]));
}

[[nodiscard]] Projection godot_matrix(const std::array<float, 16>& matrix) {
    return Projection(
        Vector4(matrix[0], matrix[1], matrix[2], matrix[3]),
        Vector4(matrix[4], matrix[5], matrix[6], matrix[7]),
        Vector4(matrix[8], matrix[9], matrix[10], matrix[11]),
        Vector4(matrix[12], matrix[13], matrix[14], matrix[15]));
}

GodotRenderer::Impl::Impl(Node3D& owner) : owner_(owner) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering || owner_.get_world_3d().is_null() || owner_.get_viewport() == nullptr) {
        fail(diagnostic_codes::backend_unavailable,
            "Godot RenderingServer, world, or viewport is unavailable");
        return;
    }
    scenario_ = owner_.get_world_3d()->get_scenario();
    viewport_ = owner_.get_viewport()->get_viewport_rid();
    camera_ = rendering->camera_create();
    rendering->camera_set_perspective(camera_, 45.0F, 1.0F, 20000.0F);
    Transform3D fixed_camera;
    fixed_camera.origin = Vector3(0.0F, 420.0F, 1050.0F);
    fixed_camera = fixed_camera.looking_at(Vector3(), Vector3(0.0F, 1.0F, 0.0F));
    rendering->camera_set_transform(camera_, fixed_camera);
    view_transform_ = fixed_camera;
    rendering->viewport_attach_camera(viewport_, camera_);
    environment_ = rendering->environment_create();
    rendering->environment_set_background(environment_, RenderingServer::ENV_BG_COLOR);
    // The stored-value pass decodes the colour buffer once more after the
    // RenderingDevice clear, so there the clear colour is encoded once more.
    const auto background = [](const float linear) {
        const float stored = linear_to_srgb_component(linear);
        return stored_output::active() ? linear_to_srgb_component(stored) : stored;
    };
    rendering->environment_set_bg_color(environment_, Color(
        background(0.006F), background(0.008F), background(0.015F), 1.0));
    // Colour policy (docs/rendering.md): the linear tonemapper at exposure 1 /
    // white 1 and the environment's defaults, without glow, adjustments or
    // auto-exposure, so the output pixel is the stored value.
    rendering->environment_set_tonemap(
        environment_, RenderingServer::ENV_TONE_MAPPER_LINEAR, 1.0, 1.0);
    rendering->scenario_set_environment(scenario_, environment_);
    stored_compositor_ = stored_output::create(*rendering);
    if (stored_compositor_.compositor.is_valid()) {
        rendering->scenario_set_compositor(scenario_, stored_compositor_.compositor);
    }
}

GodotRenderer::Impl::~Impl() {
    // Fog bindings go first, while every material RID is still valid.
    disable_fog();
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering) return;
    for (const auto& [entity, instance] : instances_) {
        static_cast<void>(entity);
        if (instance.rid.is_valid()) rendering->free_rid(instance.rid);
        if (instance.skeleton.is_valid()) rendering->free_rid(instance.skeleton);
    }
    for (const auto& [asset, resource] : resources_) {
        static_cast<void>(asset);
        free_resource(*rendering, resource);
    }
    for (const auto& [name, texture] : shared_textures_) {
        static_cast<void>(name);
        if (texture.is_valid()) rendering->free_rid(texture);
    }
    if (camera_.is_valid()) rendering->free_rid(camera_);
    if (environment_.is_valid()) rendering->free_rid(environment_);
    stored_output::release(*rendering, stored_compositor_);
    if (sun_instance_.is_valid()) rendering->free_rid(sun_instance_);
    if (sun_light_.is_valid()) rendering->free_rid(sun_light_);
}

void GodotRenderer::Impl::apply_lighting_params(RenderingServer& rendering, const RID& material) const {
    if (!lighting_) return;
    rendering.material_set_param(material, StringName("eawr_sph_r"), godot_matrix(lighting_->sph[0]));
    rendering.material_set_param(material, StringName("eawr_sph_g"), godot_matrix(lighting_->sph[1]));
    rendering.material_set_param(material, StringName("eawr_sph_b"), godot_matrix(lighting_->sph[2]));
    rendering.material_set_param(material, StringName("eawr_sph_fill_r"), godot_matrix(lighting_->sph_fill[0]));
    rendering.material_set_param(material, StringName("eawr_sph_fill_g"), godot_matrix(lighting_->sph_fill[1]));
    rendering.material_set_param(material, StringName("eawr_sph_fill_b"), godot_matrix(lighting_->sph_fill[2]));
    rendering.material_set_param(material, StringName("eawr_light_diffuse"),
        Vector3(lighting_->sun_diffuse[0], lighting_->sun_diffuse[1], lighting_->sun_diffuse[2]));
    rendering.material_set_param(material, StringName("eawr_light_direction"),
        Vector3(lighting_->toward_light[0], lighting_->toward_light[1], lighting_->toward_light[2]));
    rendering.material_set_param(material, StringName("eawr_light_specular"),
        Vector3(lighting_->specular[0], lighting_->specular[1], lighting_->specular[2]));
    rendering.material_set_param(material, StringName("eawr_shadow_floor"),
        Vector3(lighting_->shadow_floor[0], lighting_->shadow_floor[1], lighting_->shadow_floor[2]));
}

void GodotRenderer::Impl::apply_wind_params(
    RenderingServer& rendering, const RID& material, const MaterialDescription& source) const {
    if (!wind_ || !legacy::reads_wind(source)) return;
    rendering.material_set_param(material, StringName("eawr_wind"),
        Vector3(wind_->wind[0], wind_->wind[1], wind_->wind[2]));
    rendering.material_set_param(material, StringName("eawr_scene_time"), wind_->scene_time);
}

void GodotRenderer::Impl::set_wind(const GodotRenderer::WindState& wind) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering || (wind_ && *wind_ == wind)) return;
    wind_ = wind;
    for (const auto& [asset, resource] : resources_) {
        if (resource.material.is_valid()) apply_wind_params(*rendering, resource.material, resource.description);
        const auto consumer = fog_consumers_.find(asset);
        if (consumer != fog_consumers_.end() && consumer->second.shader.is_valid()) {
            apply_wind_params(*rendering, consumer->second.material, resource.description);
        }
    }
}

void GodotRenderer::Impl::set_scene_bloom(const std::optional<lighting::bloom::SceneBloom>& bloom) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering || !stored_compositor_.bloom.is_valid()) return;
    scene_bloom::configure(*rendering, stored_compositor_.bloom, bloom);
    scene_bloom_active_ = bloom.has_value();
}

void GodotRenderer::Impl::set_lighting(const GodotRenderer::LightingState& lighting) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering || !scenario_.is_valid()) return;
    // RenderingServer cannot restore an unset light parameter. Replace a
    // previously tuned light when a later scene requests the defaults.
    if (lighting_ && ((lighting_->shadow_bias && !lighting.shadow_bias)
                      || (lighting_->shadow_normal_bias && !lighting.shadow_normal_bias)
                      || (lighting_->shadow_blur && !lighting.shadow_blur))) {
        if (sun_instance_.is_valid()) rendering->free_rid(sun_instance_);
        if (sun_light_.is_valid()) rendering->free_rid(sun_light_);
        sun_instance_ = RID();
        sun_light_ = RID();
    }
    lighting_ = lighting;
    for (const auto& [asset, resource] : resources_) {
        static_cast<void>(asset);
        if (resource.material.is_valid()) apply_lighting_params(*rendering, resource.material);
    }
    for (const auto& [asset, consumer] : fog_consumers_) {
        static_cast<void>(asset);
        if (consumer.shader.is_valid()) apply_lighting_params(*rendering, consumer.material);
    }
    if (!sun_light_.is_valid()) {
        sun_light_ = rendering->directional_light_create();
        sun_instance_ = rendering->instance_create2(sun_light_, scenario_);
    }
    rendering->light_set_color(sun_light_, Color(1.0F, 1.0F, 1.0F));
    rendering->light_set_param(sun_light_, RenderingServer::LIGHT_PARAM_ENERGY, 1.0);
    switch (lighting.shadow_layout) {
    case GodotRenderer::ShadowLayout::orthogonal:
        rendering->light_directional_set_shadow_mode(
            sun_light_, RenderingServer::LIGHT_DIRECTIONAL_SHADOW_ORTHOGONAL);
        break;
    case GodotRenderer::ShadowLayout::parallel_2_splits:
        rendering->light_directional_set_shadow_mode(
            sun_light_, RenderingServer::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_2_SPLITS);
        break;
    case GodotRenderer::ShadowLayout::parallel_4_splits:
        rendering->light_directional_set_shadow_mode(
            sun_light_, RenderingServer::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_4_SPLITS);
        break;
    }
    if (lighting.shadow_layout != GodotRenderer::ShadowLayout::orthogonal) {
        rendering->light_set_param(sun_light_, RenderingServer::LIGHT_PARAM_SHADOW_SPLIT_1_OFFSET,
            lighting.shadow_split_offsets[0]);
        rendering->light_set_param(sun_light_, RenderingServer::LIGHT_PARAM_SHADOW_SPLIT_2_OFFSET,
            lighting.shadow_split_offsets[1]);
        rendering->light_set_param(sun_light_, RenderingServer::LIGHT_PARAM_SHADOW_SPLIT_3_OFFSET,
            lighting.shadow_split_offsets[2]);
    }
    rendering->light_directional_set_blend_splits(sun_light_, lighting.shadow_blend_splits);
    rendering->light_set_param(sun_light_, RenderingServer::LIGHT_PARAM_SHADOW_MAX_DISTANCE,
        lighting.shadow_max_distance);
    rendering->directional_shadow_atlas_set_size(lighting.shadow_atlas_size, true);
    switch (lighting.shadow_filter) {
    case GodotRenderer::ShadowFilter::soft_low:
        rendering->directional_soft_shadow_filter_set_quality(RenderingServer::SHADOW_QUALITY_SOFT_LOW);
        break;
    case GodotRenderer::ShadowFilter::soft_medium:
        rendering->directional_soft_shadow_filter_set_quality(RenderingServer::SHADOW_QUALITY_SOFT_MEDIUM);
        break;
    case GodotRenderer::ShadowFilter::soft_high:
        rendering->directional_soft_shadow_filter_set_quality(RenderingServer::SHADOW_QUALITY_SOFT_HIGH);
        break;
    }
    if (lighting.shadow_bias) {
        rendering->light_set_param(sun_light_, RenderingServer::LIGHT_PARAM_SHADOW_BIAS, *lighting.shadow_bias);
    }
    if (lighting.shadow_normal_bias) {
        rendering->light_set_param(
            sun_light_, RenderingServer::LIGHT_PARAM_SHADOW_NORMAL_BIAS, *lighting.shadow_normal_bias);
    }
    if (lighting.shadow_blur) {
        rendering->light_set_param(sun_light_, RenderingServer::LIGHT_PARAM_SHADOW_BLUR, *lighting.shadow_blur);
    }
    rendering->light_set_shadow(sun_light_, lighting.shadows);
    const Vector3 toward(lighting.toward_light[0], lighting.toward_light[1], lighting.toward_light[2]);
    // A directional light shines along its local -Z.
    const Vector3 travel = -toward.normalized();
    const Vector3 up = std::abs(travel.y) > 0.99F ? Vector3(0.0F, 0.0F, 1.0F) : Vector3(0.0F, 1.0F, 0.0F);
    Transform3D transform;
    transform = transform.looking_at(travel, up);
    rendering->instance_set_transform(sun_instance_, transform);
}

void GodotRenderer::Impl::set_shadows_enabled(const bool enabled) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering || !sun_light_.is_valid()) return;
    rendering->light_set_shadow(sun_light_, enabled);
}

void GodotRenderer::Impl::set_casts_shadows(const sim::AssetId asset_id, const bool casts) {
    if (casts) non_casting_.erase(asset_id);
    else non_casting_.insert(asset_id);
}

void GodotRenderer::Impl::set_camera(const FixedCamera& camera) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering || !camera_.is_valid()) return;
    rendering->camera_set_perspective(
        camera_, camera.vertical_fov_degrees, camera.near_plane, camera.far_plane);
    Transform3D camera_transform;
    camera_transform.origin = Vector3(camera.eye[0], camera.eye[1], camera.eye[2]);
    camera_transform = camera_transform.looking_at(
        Vector3(camera.target[0], camera.target[1], camera.target[2]),
        Vector3(camera.up[0], camera.up[1], camera.up[2]));
    rendering->camera_set_transform(camera_, camera_transform);
    view_transform_ = camera_transform;
    for (auto& [entity, instance] : instances_) {
        const auto resource = resources_.find(instance.asset_id);
        if (resource != resources_.end() && !resource->second.billboard_modes.empty()) {
            refresh_billboards(*rendering, entity, instance, resource->second);
        }
    }
}

[[nodiscard]] core::Result<CaptureResult> GodotRenderer::Impl::capture(const FixedCamera& camera) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering || !camera_.is_valid()) {
        return core::Result<CaptureResult>::failure(make_diagnostic(
            diagnostic_codes::capture_failed, "capture camera is unavailable"));
    }
    set_camera(camera);
    const Ref<ViewportTexture> viewport_texture = owner_.get_viewport()->get_texture();
    const Ref<Image> image = viewport_texture.is_valid() ? viewport_texture->get_image() : Ref<Image>();
    if (image.is_null() || image->is_empty()) {
        return core::Result<CaptureResult>::failure(make_diagnostic(
            diagnostic_codes::capture_failed, "Godot returned an empty viewport image"));
    }
    const PackedByteArray packed = image->save_png_to_buffer();
    if (packed.is_empty()) {
        return core::Result<CaptureResult>::failure(make_diagnostic(
            diagnostic_codes::capture_failed, "Godot failed to encode fixed-camera PNG"));
    }
    std::vector<std::byte> png_bytes(static_cast<std::size_t>(packed.size()));
    std::memcpy(png_bytes.data(), packed.ptr(), png_bytes.size());
    return core::Result<CaptureResult>::success(CaptureResult{
        .png_bytes = std::move(png_bytes),
        .width = static_cast<std::uint32_t>(image->get_width()),
        .height = static_cast<std::uint32_t>(image->get_height()),
    });
}

[[nodiscard]] BackendInfo GodotRenderer::Impl::backend_info() const {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering) return {"Godot", "unavailable", {}, {}, {}};
    // The running method: --rendering-method overrides the project setting.
    return {
        "Godot 4.7.2-stable",
        utf8(rendering->get_current_rendering_method()),
        utf8(rendering->get_video_adapter_vendor()),
        utf8(rendering->get_video_adapter_name()),
        utf8(rendering->get_video_adapter_api_version()),
    };
}

[[nodiscard]] core::Diagnostic GodotRenderer::Impl::make_diagnostic(
    const std::string_view code, std::string message) const {
    BackendInfo info = backend_info();
    if (!info.adapter_name.empty()) {
        message += " [" + info.rendering_method + "; " + info.adapter_vendor + " "
            + info.adapter_name + "; " + info.driver_api + "]";
    }
    return core::Diagnostic{
        .code = std::string(code),
        .severity = core::Severity::error,
        .message = std::move(message),
        .logical_path = std::nullopt,
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = std::string("presentation.godot"),
    };
}

// fail(), returning the recorded diagnostic.
[[nodiscard]] core::Diagnostic GodotRenderer::Impl::pushed(const std::string_view code, std::string message) {
    core::Diagnostic diagnostic = make_diagnostic(code, std::move(message));
    diagnostics_.push(diagnostic);
    return diagnostic;
}

[[nodiscard]] core::Result<void> GodotRenderer::Impl::failure(const std::string_view code, std::string message) {
    core::Diagnostic diagnostic = make_diagnostic(code, std::move(message));
    diagnostics_.push(diagnostic);
    return core::Result<void>::failure(std::move(diagnostic));
}

GodotRenderer::GodotRenderer(Node3D& host) : impl_(std::make_unique<Impl>(host)) {}
GodotRenderer::~GodotRenderer() = default;
GodotRenderer::GodotRenderer(GodotRenderer&&) noexcept = default;
GodotRenderer& GodotRenderer::operator=(GodotRenderer&&) noexcept = default;

core::Result<void> GodotRenderer::upload(
    const sim::AssetId asset_id,
    const assets::Model& model,
    const assets::Texture& texture,
    const MaterialDescription& material) {
    return impl_->upload(asset_id, model, texture, material);
}

core::Result<void> GodotRenderer::upload(
    const sim::AssetId asset_id,
    const assets::Model& model,
    const assets::Texture& texture,
    const MaterialDescription& material,
    const std::span<const BindingTexture> binding_textures) {
    return impl_->upload(asset_id, model, texture, material, binding_textures);
}

core::Result<void> GodotRenderer::register_shared_texture(
    const std::string_view name, const assets::Texture& texture) {
    return impl_->register_shared_texture(name, texture);
}

core::Result<void> GodotRenderer::register_shared_texture_array(
    const std::string_view name, const std::span<const assets::Texture> layers, const std::uint32_t edge) {
    return impl_->register_shared_texture_array(name, layers, edge);
}

std::size_t GodotRenderer::shared_texture_count() const noexcept { return impl_->shared_texture_count(); }

core::Result<void> GodotRenderer::retain(const sim::AssetId asset_id) {
    return impl_->retain(asset_id);
}

core::Result<void> GodotRenderer::release(const sim::AssetId asset_id) {
    return impl_->release(asset_id);
}

std::vector<ResourceReference> GodotRenderer::resources() const { return impl_->resources(); }

core::Result<void> GodotRenderer::set_skin_pose(
    const sim::EntityId entity_id,
    const sim::AssetId asset_id,
    const std::span<const animation::BonePose> bones) {
    return impl_->set_skin_pose(entity_id, asset_id, bones);
}

std::vector<GodotRenderer::SkinBindingEvidence> GodotRenderer::skin_bindings() const {
    return impl_->skin_bindings();
}

std::vector<GodotRenderer::SubmissionEvidence> GodotRenderer::submission_evidence() const {
    return impl_->submission_evidence();
}

std::size_t GodotRenderer::instance_count() const noexcept { return impl_->instance_count(); }

void GodotRenderer::clear_skin_pose(const sim::EntityId entity_id) {
    impl_->clear_skin_pose(entity_id);
}
void GodotRenderer::set_billboard_light(const sim::AssetId asset_id,
                                       const std::array<float, 3>& toward_light) {
    impl_->set_billboard_light(asset_id, toward_light);
}

core::Result<void> GodotRenderer::set_material_scalar(const sim::AssetId asset_id, const std::string_view binding,
                                                      const float value) {
    return impl_->set_material_scalar(asset_id, binding, value);
}

void GodotRenderer::set_light_scale(const sim::EntityId entity_id, const std::array<float, 3>& rgb) {
    impl_->set_light_scale(entity_id, rgb);
}

void GodotRenderer::set_unit_opacity(const sim::EntityId entity_id, const float alpha) {
    impl_->set_unit_opacity(entity_id, alpha);
}

GodotRenderer::LifecycleCounts GodotRenderer::lifecycle_counts() const noexcept {
    return impl_->lifecycle_counts();
}

std::vector<GodotRenderer::InstanceEvidence> GodotRenderer::instance_evidence() const {
    return impl_->instance_evidence();
}

void GodotRenderer::set_camera(const FixedCamera& camera) { impl_->set_camera(camera); }

void GodotRenderer::set_lighting(const LightingState& lighting) { impl_->set_lighting(lighting); }
const std::optional<GodotRenderer::LightingState>& GodotRenderer::lighting() const noexcept {
    return impl_->lighting();
}

void GodotRenderer::set_wind(const WindState& wind) { impl_->set_wind(wind); }

void GodotRenderer::set_shadows_enabled(const bool enabled) { impl_->set_shadows_enabled(enabled); }

void GodotRenderer::set_scene_bloom(const std::optional<lighting::bloom::SceneBloom>& bloom) {
    impl_->set_scene_bloom(bloom);
}

bool GodotRenderer::scene_bloom_active() const noexcept { return impl_->scene_bloom_active(); }

void GodotRenderer::set_casts_shadows(const sim::AssetId asset_id, const bool casts) {
    impl_->set_casts_shadows(asset_id, casts);
}

std::size_t GodotRenderer::shadow_receiving_materials() const noexcept {
    return impl_->shadow_receiving_materials();
}

std::size_t GodotRenderer::shadow_variant_failures() const noexcept {
    return impl_->shadow_variant_failures();
}

core::Result<void> GodotRenderer::enable_fog(const FogOptions& options) {
    return impl_->enable_fog(options);
}

void GodotRenderer::disable_fog() { impl_->disable_fog(); }

void GodotRenderer::set_fog_team(const std::uint32_t team) { impl_->set_fog_team(team); }

void GodotRenderer::reset_fog_stream(const std::uint64_t stream) { impl_->reset_fog_stream(stream); }

core::Result<void> GodotRenderer::declare_fog_consumer(const sim::AssetId asset_id) {
    return impl_->declare_fog_consumer(asset_id);
}

core::Result<GodotRenderer::ExternalFogHandle> GodotRenderer::register_external_fog_material(
    const RID& material, const RID& shader) {
    return impl_->register_external_fog_material(material, shader);
}

void GodotRenderer::unregister_external_fog_material(const ExternalFogHandle handle) {
    impl_->unregister_external_fog_material(handle);
}

std::vector<GodotRenderer::ExternalFogEvidence> GodotRenderer::external_fog_consumers() const {
    return impl_->external_fog_evidence();
}

GodotRenderer::FogStatus GodotRenderer::fog_status() const { return impl_->fog_status(); }

std::vector<GodotRenderer::FogConsumerEvidence> GodotRenderer::fog_consumers() const {
    return impl_->fog_consumer_evidence();
}

void GodotRenderer::submit(std::shared_ptr<const sim::RenderSnapshot> snapshot) {
    impl_->submit(std::move(snapshot));
}

core::Result<CaptureResult> GodotRenderer::capture(const FixedCamera& camera) {
    return impl_->capture(camera);
}

BackendInfo GodotRenderer::backend_info() const { return impl_->backend_info(); }
std::span<const core::Diagnostic> GodotRenderer::diagnostics() const {
    return impl_->diagnostics();
}

} // namespace eawr::presentation::godot_backend
