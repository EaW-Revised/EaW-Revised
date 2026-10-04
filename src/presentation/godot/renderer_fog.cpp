#include "renderer_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace {

// Why the source's declaration of `name` cannot be verified as
// `uniform [precision] sampler2D name`, or nullopt when it is. Reflection
// cannot tell sampler2D from isampler2D/usampler2D (all report Texture2D), and
// an integer sampler cannot read the R8 UNORM fog texture, so the declared
// type token is read from the comment-stripped source. Anything this lexical
// reading cannot settle fails closed: preprocessor directives (a macro or an
// include could supply the declaration), more or fewer than one uniform
// declaration, or a global/instance uniform.
[[nodiscard]] std::optional<std::string> sampler2d_declaration_problem(
    const std::string_view code, const std::string_view name) {
    const auto lexed = shader_tokens(code);
    if (!lexed) return std::string("has an unterminated block comment");
    const std::vector<std::string>& tokens = *lexed;
    if (std::find(tokens.begin(), tokens.end(), "#") != tokens.end()) {
        return "uses preprocessor directives, so its " + std::string(name) + " declaration cannot be verified";
    }
    std::vector<std::string> declared;
    for (std::size_t index = 2; index < tokens.size(); ++index) {
        if (tokens[index] != name) continue;
        std::size_t keyword = index - 2;
        const std::string& qualifier = tokens[keyword];
        if ((qualifier == "lowp" || qualifier == "mediump" || qualifier == "highp") && keyword > 0) --keyword;
        if (tokens[keyword] != "uniform") continue;
        if (keyword > 0 && (tokens[keyword - 1] == "global" || tokens[keyword - 1] == "instance")) {
            return std::string(name) + " is a " + tokens[keyword - 1] + " uniform";
        }
        declared.push_back(tokens[index - 1]);
    }
    if (declared.size() != 1) {
        return std::string(name) + " has " + std::to_string(declared.size())
            + " recognisable uniform declarations, not exactly one";
    }
    if (declared.front() != "sampler2D") {
        return std::string(name) + " is declared as " + declared.front() + ", not sampler2D";
    }
    return std::nullopt;
}

} // namespace

[[nodiscard]] core::Result<void> GodotRenderer::Impl::enable_fog(const GodotRenderer::FogOptions& options) {
    disable_fog();
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering || !scenario_.is_valid()) {
        return failure(diagnostic_codes::backend_unavailable,
            "fog-stub-v1 needs an initialized Godot renderer");
    }
    FogRuntime runtime;
    runtime.selection = options.selection;
    runtime.derive_legacy_stages = options.derive_legacy_stages;
    runtime.backend = std::make_unique<GodotFogBackend>(options.max_texture_dimension);
    runtime.cache = std::make_unique<fog::TextureCache>(*runtime.backend);
    fog_ = std::move(runtime);
    // All or nothing: every declared consumer attaches, or fog stays
    // disabled with the declarations unchanged. Each failure is in the
    // diagnostic history; the first is returned.
    std::optional<core::Diagnostic> failed;
    for (auto& [asset, consumer] : fog_consumers_) {
        auto attached = attach_fog_consumer(asset, consumer);
        if (attached && !failed) failed = std::move(attached);
    }
    for (auto& [handle, consumer] : external_fog_consumers_) {
        auto attached = attach_external_fog_consumer(consumer);
        if (attached && !failed) failed = std::move(attached);
    }
    if (failed) {
        disable_fog();
        return core::Result<void>::failure(std::move(*failed));
    }
    return core::Result<void>::success();
}

void GodotRenderer::Impl::disable_fog() {
    if (!fog_) return;
    // Consumers leave the cache (unbinding their materials) before the
    // fog variant materials are freed and default materials restored.
    for (auto& [asset, consumer] : fog_consumers_) detach_fog_consumer(asset, consumer);
    for (auto& [handle, consumer] : external_fog_consumers_) detach_external_fog_consumer(consumer);
    fog_->cache->reset();
    fog_->cache.reset();
    fog_->backend.reset();
    fog_.reset();
}

void GodotRenderer::Impl::set_fog_team(const std::uint32_t team) {
    if (!fog_) return;
    fog_->selection.team = team;
    fog_->last_action.reset();
    fog_->last_rejection.reset();
    apply_fog();
}

void GodotRenderer::Impl::reset_fog_stream(const std::uint64_t stream) {
    if (!fog_) return;
    fog_->cache->reset_stream(fog_->selection.stream);
    fog_->selection.stream = stream;
    fog_->grids.reset();
    fog_->submitted_tick.reset();
    fog_->last_action.reset();
    fog_->last_rejection.reset();
}

[[nodiscard]] core::Result<void> GodotRenderer::Impl::declare_fog_consumer(const sim::AssetId asset_id) {
    const auto resource = resources_.find(asset_id);
    if (resource == resources_.end()) {
        return failure(diagnostic_codes::missing_asset,
            "cannot declare missing renderer asset " + std::to_string(asset_id) + " a fog consumer");
    }
    if (fog_consumers_.contains(asset_id)) return core::Result<void>::success();
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering) {
        return failure(diagnostic_codes::backend_unavailable, "Godot RenderingServer is unavailable");
    }
    std::string reason;
    const auto kind = fog_consumer_kind(*rendering, resource->second,
        fog_ && fog_->derive_legacy_stages, reason);
    if (!kind) {
        fog_unsupported_[asset_id] = reason;
        return failure(fog_diagnostic_codes::unsupported_consumer,
            "asset " + std::to_string(asset_id) + " is not a fog-stub-v1 consumer: " + reason);
    }
    FogConsumer& consumer = fog_consumers_.emplace(asset_id, FogConsumer{.kind = *kind}).first->second;
    // A consumer declared while fog is enabled receives the current grid.
    // One whose fog variant fails is not declared: its default material
    // stays and the declarations are unchanged.
    if (fog_) {
        if (auto failed = attach_fog_consumer(asset_id, consumer)) {
            fog_consumers_.erase(asset_id);
            return core::Result<void>::failure(std::move(*failed));
        }
    }
    fog_unsupported_.erase(asset_id);
    return core::Result<void>::success();
}

[[nodiscard]] core::Result<GodotRenderer::ExternalFogHandle> GodotRenderer::Impl::register_external_fog_material(
    const RID& material, const RID& shader) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering || !material.is_valid() || !shader.is_valid()) {
        return core::Result<GodotRenderer::ExternalFogHandle>::failure(make_diagnostic(
            fog_diagnostic_codes::unsupported_consumer, "external fog material or shader RID is invalid"));
    }
    if (const auto problem = fog_uniform_problem(*rendering, shader)) {
        return core::Result<GodotRenderer::ExternalFogHandle>::failure(make_diagnostic(
            fog_diagnostic_codes::unsupported_consumer, "external fog material " + *problem));
    }
    for (const auto& [handle, existing] : external_fog_consumers_) {
        if (existing.material == material) {
            return core::Result<GodotRenderer::ExternalFogHandle>::failure(make_diagnostic(
                fog_diagnostic_codes::unsupported_consumer, "external fog material RID is already registered"));
        }
    }
    const auto handle = next_external_fog_handle_++;
    auto& consumer = external_fog_consumers_.emplace(handle,
        ExternalFogConsumer{.material = material, .shader = shader}).first->second;
    if (fog_) {
        if (auto failed = attach_external_fog_consumer(consumer)) {
            external_fog_consumers_.erase(handle);
            return core::Result<GodotRenderer::ExternalFogHandle>::failure(std::move(*failed));
        }
    }
    return core::Result<GodotRenderer::ExternalFogHandle>::success(handle);
}

void GodotRenderer::Impl::unregister_external_fog_material(const GodotRenderer::ExternalFogHandle handle) {
    const auto found = external_fog_consumers_.find(handle);
    if (found == external_fog_consumers_.end()) return;
    detach_external_fog_consumer(found->second);
    external_fog_consumers_.erase(found);
}

[[nodiscard]] std::vector<GodotRenderer::ExternalFogEvidence> GodotRenderer::Impl::external_fog_evidence() const {
    std::vector<GodotRenderer::ExternalFogEvidence> result;
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering) return result;
    for (const auto& [handle, consumer] : external_fog_consumers_) {
        GodotRenderer::ExternalFogEvidence evidence;
        evidence.handle = handle;
        evidence.attached = consumer.id != fog::ConsumerId{};
        const Variant bound = rendering->material_get_param(consumer.material,
            StringName("eawr_fog_bound"));
        evidence.bound = evidence.attached && bound.get_type() == Variant::BOOL && static_cast<bool>(bound);
        for (const std::string_view parameter : GodotFogBackend::parameters) {
            const Variant value = rendering->material_get_param(consumer.material,
                StringName(String::utf8(parameter.data(), static_cast<int64_t>(parameter.size()))));
            evidence.fog_parameters.emplace_back(std::string(parameter),
                utf8(Variant::get_type_name(value.get_type())) + ":" + utf8(value.stringify()));
        }
        result.push_back(std::move(evidence));
    }
    return result;
}

[[nodiscard]] GodotRenderer::FogStatus GodotRenderer::Impl::fog_status() const {
    GodotRenderer::FogStatus status;
    status.external_consumers = external_fog_consumers_.size();
    status.declared_consumers = fog_consumers_.size() + status.external_consumers;
    for (const auto& [asset, reason] : fog_unsupported_) status.unsupported.push_back({asset, reason});
    if (!fog_) return status;
    const fog::StreamTeam key = fog_key();
    const auto active = fog_->cache->active();
    status.selection = fog_->selection;
    status.submitted_tick = fog_->submitted_tick;
    status.last_action = fog_->last_action;
    status.last_rejection = fog_->last_rejection;
    status.cache = fog_->cache->stats();
    status.live_textures = fog_->backend->live_textures();
    status.attached_consumers = fog_->cache->consumers();
    const bool bound = active && *active == key;
    if (bound) status.bound_revision = fog_->cache->accepted(key)->revision();
    status.binding_retained = bound && fog_->last_rejection.has_value();
    status.readiness = fog_->last_rejection ? GodotRenderer::FogReadiness::rejected
        : bound && status.attached_consumers == status.declared_consumers
            ? GodotRenderer::FogReadiness::ready : GodotRenderer::FogReadiness::awaiting_grid;
    return status;
}

[[nodiscard]] std::vector<GodotRenderer::FogConsumerEvidence> GodotRenderer::Impl::fog_consumer_evidence() const {
    std::vector<GodotRenderer::FogConsumerEvidence> result;
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering) return result;
    for (const auto& [asset, consumer] : fog_consumers_) {
        const Resource& resource = resources_.at(asset);
        GodotRenderer::FogConsumerEvidence evidence{.asset_id = asset, .kind = consumer.kind};
        evidence.attached = consumer.id != fog::ConsumerId{};
        const std::int64_t surfaces = rendering->mesh_get_surface_count(resource.mesh);
        evidence.surfaces = static_cast<std::size_t>(surfaces);
        for (std::int64_t surface = 0; surface < surfaces; ++surface) {
            const RID material = rendering->mesh_surface_get_material(
                resource.mesh, static_cast<std::int32_t>(surface));
            if (material == resource.material) ++evidence.surfaces_with_default_material;
            if (evidence.attached && material == consumer.material) ++evidence.surfaces_with_fog_material;
        }
        evidence.default_shader_code = utf8(rendering->shader_get_code(resource.shader));
        if (consumer.shader.is_valid()) {
            evidence.fog_shader_code = utf8(rendering->shader_get_code(consumer.shader));
        }
        const RID& drawn = evidence.attached ? consumer.material : resource.material;
        for (const std::string_view parameter : GodotFogBackend::parameters) {
            const Variant value = rendering->material_get_param(drawn,
                StringName(String::utf8(parameter.data(), static_cast<int64_t>(parameter.size()))));
            evidence.fog_parameters.emplace_back(std::string(parameter),
                utf8(Variant::get_type_name(value.get_type())) + ":" + utf8(value.stringify()));
        }
        result.push_back(std::move(evidence));
    }
    return result;
}

void GodotRenderer::Impl::apply_fog() {
    if (!fog_ || !fog_->grids) return;
    auto result = fog_->cache->submit(*fog_->grids, fog_key());
    if (result) {
        fog_->last_action = result.value();
        fog_->last_rejection.reset();
        return;
    }
    fog_->last_action.reset();
    fog_->last_rejection = result.error();
    diagnostics_.push(result.error());
}

// Why the compiled shader is not a fog-stub-v1 consumer, or nullopt. Each
// fog uniform must be reflected by RenderingServer with its fog-stub-v1
// type; eawr_fog_texture must also be exactly one sampler2D (a Texture2D
// resource hint plus sampler2d_declaration_problem), because the backend
// binds a 2D R8 texture RID to it. The reflected list is empty when
// Godot's compiler rejected the source, so this doubles as the compile
// check of the fog variant.
[[nodiscard]] std::optional<std::string> GodotRenderer::Impl::fog_uniform_problem(
    RenderingServer& rendering, const RID& shader) {
    const std::array<std::pair<std::string_view, Variant::Type>, 5> required{{
        {GodotFogBackend::texture_parameter, Variant::OBJECT},
        {GodotFogBackend::origin_parameter, Variant::VECTOR2},
        {GodotFogBackend::extent_parameter, Variant::VECTOR2},
        {GodotFogBackend::size_parameter, Variant::VECTOR2},
        {GodotFogBackend::bound_parameter, Variant::BOOL},
    }};
    const TypedArray<Dictionary> parameters = rendering.get_shader_parameter_list(shader);
    for (const auto& [name, type] : required) {
        const StringName wanted(String::utf8(name.data(), static_cast<int64_t>(name.size())));
        std::optional<Dictionary> reflected;
        for (int64_t index = 0; index < parameters.size() && !reflected; ++index) {
            const Dictionary parameter = parameters[index];
            if (static_cast<StringName>(parameter.get("name", StringName())) == wanted) reflected = parameter;
        }
        if (!reflected) return "does not declare fog uniform " + std::string(name);
        const int64_t reflected_type = static_cast<int64_t>(reflected->get("type", Variant::NIL));
        if (reflected_type != static_cast<int64_t>(type)) {
            return "declares fog uniform " + std::string(name) + " as "
                + utf8(Variant::get_type_name(static_cast<Variant::Type>(reflected_type)))
                + ", not the fog-stub-v1 " + utf8(Variant::get_type_name(type));
        }
        if (type != Variant::OBJECT) continue;
        const int64_t hint = static_cast<int64_t>(reflected->get("hint", PROPERTY_HINT_NONE));
        const std::string hint_string = utf8(static_cast<String>(reflected->get("hint_string", String())));
        if (hint != static_cast<int64_t>(PROPERTY_HINT_RESOURCE_TYPE) || hint_string != "Texture2D") {
            return "declares fog uniform " + std::string(name) + " as a '" + hint_string
                + "' resource (hint " + std::to_string(hint) + "), not a sampler2D (Texture2D)";
        }
        if (const auto problem = sampler2d_declaration_problem(utf8(rendering.shader_get_code(shader)), name)) {
            return "declares fog uniform " + std::string(name) + " that is not verifiably a sampler2D: "
                + *problem;
        }
    }
    return std::nullopt;
}

// The derived fog variant of a legacy adapter (see derived_legacy_fog_variant),
// in the same shadow variant its default material was compiled in.
[[nodiscard]] std::optional<std::string> GodotRenderer::Impl::derived_fog_source(const Resource& resource) {
    const auto selected = legacy_shader_source(resource.description);
    if (!selected) return std::nullopt;
    auto derived = derived_legacy_fog_variant(*selected);
    if (!derived || !resource.shadow_receiving) return derived;
    return shadow_receiving_variant(*derived);
}

[[nodiscard]] std::optional<GodotRenderer::FogConsumerKind> GodotRenderer::Impl::fog_consumer_kind(
    RenderingServer& rendering, const Resource& resource, const bool derive_legacy,
    std::string& reason) {
    const MaterialDescription& source = resource.description;
    if (source.route == MaterialRoute::legacy_effect) {
        if (source.program == "BatchMeshGloss.fx" && source.technique == "sph_t1"
            && source.pass_name == "sph_t1_p0" && source.pass == RenderPass::opaque) {
            return GodotRenderer::FogConsumerKind::batch_mesh_gloss;
        }
        if (source.program == "BatchMeshAlpha.fx" && source.technique == "sph_t1"
            && source.pass_name == "sph_t1_p0" && source.pass == RenderPass::transparent) {
            return GodotRenderer::FogConsumerKind::batch_mesh_alpha;
        }
        // The DX8 BatchMesh passes (#200) multiply by the fog-of-war texture
        // in their pixel stage, as the fixed-function ones do in stage 1, so
        // they take the derived stage in every fog mode.
        const bool batch_dx8 = source.technique == "sph_t0" && source.pass_name == "sph_t0_p0"
            && ((source.program == "BatchMeshGloss.fx" && source.pass == RenderPass::opaque)
                || (source.program == "BatchMeshAlpha.fx" && source.pass == RenderPass::transparent));
        if ((derive_legacy || batch_dx8) && derived_fog_source(resource)) {
            return GodotRenderer::FogConsumerKind::legacy_derived;
        }
        reason = "legacy " + source.program + " " + source.technique + "/" + source.pass_name + " ("
            + std::string(to_string(source.pass)) + ") has no fog-stub-v1 stage";
        return std::nullopt;
    }
    if (const auto problem = fog_uniform_problem(rendering, resource.shader)) {
        reason = "modern_spatial shader " + *problem;
        return std::nullopt;
    }
    return GodotRenderer::FogConsumerKind::modern_spatial;
}

void GodotRenderer::Impl::set_surface_material(RenderingServer& rendering, const RID& mesh, const RID& material) const {
    const std::int64_t surfaces = rendering.mesh_get_surface_count(mesh);
    for (std::int64_t surface = 0; surface < surfaces; ++surface) {
        rendering.mesh_surface_set_material(mesh, static_cast<std::int32_t>(surface), material);
    }
}

// Attaches one declared consumer to the live fog runtime. A failure is
// pushed to the diagnostic history and returned, and leaves the consumer
// detached on its default material with no fog RID.
[[nodiscard]] std::optional<core::Diagnostic> GodotRenderer::Impl::attach_fog_consumer(
    const sim::AssetId asset_id, FogConsumer& consumer) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!fog_ || !rendering || consumer.id != fog::ConsumerId{}) return std::nullopt;
    const Resource& resource = resources_.at(asset_id);
    if (consumer.kind == GodotRenderer::FogConsumerKind::batch_mesh_gloss
        || consumer.kind == GodotRenderer::FogConsumerKind::batch_mesh_alpha
        || consumer.kind == GodotRenderer::FogConsumerKind::legacy_derived) {
        // The fixed (BatchMesh) or derived fog variant of the same adapter,
        // in the same shadow variant the default material was compiled in.
        const bool derived = consumer.kind == GodotRenderer::FogConsumerKind::legacy_derived;
        const bool alpha = consumer.kind == GodotRenderer::FogConsumerKind::batch_mesh_alpha;
        const std::string family = derived ? resource.description.program
            : (alpha ? "BatchMeshAlpha" : "BatchMeshGloss");
        std::string source(alpha ? fixed_mesh_shader_alpha_fog : fixed_mesh_shader_opaque_fog);
        if (derived) {
            auto variant = derived_fog_source(resource);
            if (!variant) {
                return pushed(diagnostic_codes::shader_compile_failed,
                    family + " has no derived fog-stub-v1 variant for asset " + std::to_string(asset_id));
            }
            source = std::move(*variant);
        } else if (resource.shadow_receiving) {
            auto receiving = shadow_receiving_variant(source);
            if (!receiving) {
                return pushed(diagnostic_codes::shader_compile_failed,
                    family + " fog-stub-v1 variant has no shadow-receiving form for asset "
                        + std::to_string(asset_id));
            }
            source = std::move(*receiving);
        }
        source = stored_output::backend_source(source);
        consumer.shader = rendering->shader_create();
        rendering->shader_set_code(consumer.shader,
            String::utf8(source.data(), static_cast<int64_t>(source.size())));
        if (const auto problem = fog_uniform_problem(*rendering, consumer.shader)) {
            rendering->free_rid(consumer.shader);
            consumer.shader = {};
            return pushed(diagnostic_codes::shader_compile_failed,
                "Godot rejected the " + family
                    + " fog-stub-v1 variant for asset "
                    + std::to_string(asset_id) + ": it " + *problem);
        }
        const auto tokens = shader_tokens(source).value_or(std::vector<std::string>{});
        if (auto problem = portable_limit_problem(reflected_uniforms(*rendering, consumer.shader), tokens)) {
            rendering->free_rid(consumer.shader);
            consumer.shader = {};
            return pushed(diagnostic_codes::shader_compile_failed,
                family
                    + " fog-stub-v1 variant for asset " + std::to_string(asset_id) + " " + *problem);
        }
        consumer.material = rendering->material_create();
        rendering->material_set_shader(consumer.material, consumer.shader);
        configure_material(*rendering, resource.description, consumer.material, resource.texture,
            resource.binding_textures);
        if (resource.priority) rendering->material_set_render_priority(consumer.material, *resource.priority);
        set_surface_material(*rendering, resource.mesh, consumer.material);
    } else {
        consumer.material = resource.material;
    }
    consumer.id = fog_->backend->register_material(consumer.material);
    // add_consumer binds at once when a grid is already bound. It fails
    // only for a zero or repeated backend ID, which register_material
    // never returns.
    static_cast<void>(fog_->cache->add_consumer(consumer.id));
    return std::nullopt;
}

void GodotRenderer::Impl::detach_fog_consumer(const sim::AssetId asset_id, FogConsumer& consumer) {
    if (!fog_ || consumer.id == fog::ConsumerId{}) return;
    static_cast<void>(fog_->cache->remove_consumer(consumer.id));
    fog_->backend->forget_material(consumer.id);
    consumer.id = {};
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (consumer.shader.is_valid() && rendering) {
        const Resource& resource = resources_.at(asset_id);
        set_surface_material(*rendering, resource.mesh, resource.material);
        rendering->free_rid(consumer.material);
        rendering->free_rid(consumer.shader);
    }
    consumer.shader = {};
    consumer.material = {};
}

[[nodiscard]] std::optional<core::Diagnostic> GodotRenderer::Impl::attach_external_fog_consumer(ExternalFogConsumer& consumer) {
    if (!fog_ || consumer.id != fog::ConsumerId{}) return std::nullopt;
    consumer.id = fog_->backend->register_material(consumer.material);
    const auto attached = fog_->cache->add_consumer(consumer.id);
    if (!attached) {
        fog_->backend->forget_material(consumer.id);
        consumer.id = {};
        diagnostics_.push(attached.error());
        return attached.error();
    }
    return std::nullopt;
}

void GodotRenderer::Impl::detach_external_fog_consumer(ExternalFogConsumer& consumer) {
    if (!fog_ || consumer.id == fog::ConsumerId{}) return;
    static_cast<void>(fog_->cache->remove_consumer(consumer.id));
    fog_->backend->forget_material(consumer.id);
    consumer.id = {};
}

} // namespace eawr::presentation::godot_backend
