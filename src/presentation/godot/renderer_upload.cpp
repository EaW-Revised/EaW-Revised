#include "renderer_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace {

[[nodiscard]] Vector3 axis_convert(const assets::Vec3f& value) {
    return {value.x, value.z, -value.y};
}

[[nodiscard]] Image::Format image_format(const assets::PixelFormat format) {
    switch (format) {
    case assets::PixelFormat::rgba8: return Image::FORMAT_RGBA8;
    case assets::PixelFormat::bgra8: return Image::FORMAT_RGBA8;
    case assets::PixelFormat::bgr8: return Image::FORMAT_RGB8;
    case assets::PixelFormat::l8: return Image::FORMAT_L8;
    case assets::PixelFormat::a8: return Image::FORMAT_LA8;
    case assets::PixelFormat::bc1: return Image::FORMAT_DXT1;
    case assets::PixelFormat::bc2: return Image::FORMAT_DXT3;
    case assets::PixelFormat::bc3: return Image::FORMAT_DXT5;
    case assets::PixelFormat::bc4: return Image::FORMAT_RGTC_R;
    case assets::PixelFormat::bc5: return Image::FORMAT_RGTC_RG;
    case assets::PixelFormat::bc7: return Image::FORMAT_BPTC_RGBA;
    }
    return Image::FORMAT_MAX;
}

[[nodiscard]] Variant legacy_uniform_value(const legacy::UniformValue& value) {
    return std::visit([](const auto& item) -> Variant {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, float>) {
            return item;
        } else if constexpr (std::is_same_v<T, std::array<float, 2>>) {
            return Vector2(item[0], item[1]);
        } else if constexpr (std::is_same_v<T, std::array<float, 3>>) {
            return Vector3(item[0], item[1], item[2]);
        } else {
            return Vector4(item[0], item[1], item[2], item[3]);
        }
    }, value);
}

// ---- material admission (#22) ----------------------------------------------
// Godot's shading-language compiler accepting a source is not the whole
// story: the driver builds each variant later, at first draw, and a
// material binding Godot cannot apply is dropped or zero-filled without an
// error. Both are checked here, against the compiled shader's reflected
// uniforms, before a material RID exists.
//
// The limits are portable on purpose: a material is admitted or refused
// identically on every GPU, on software rasterisers, on Forward+ and on the
// Compatibility fallback (rendering_device/fallback_to_opengl3, kept until
// FP-5 decides it).

// Godot's Compatibility renderer binds its own samplers at
// GL_MAX_TEXTURE_IMAGE_UNITS - 2 down to - 11 (drivers/gles3/shaders/
// scene.glsl) and a material's samplers from unit 0 upward. GL 3.3
// guarantees 16 units, so five material samplers are the most that cannot
// collide with an engine sampler or exceed the unit count on any conforming
// driver. Forward+ alone would admit more.
constexpr std::size_t portable_material_samplers = 5;
// Every non-sampler material uniform is packed into one std140 block
// (MaterialUniforms). Vulkan guarantees maxUniformBufferRange >= 16384 and
// GL 3.3 GL_MAX_UNIFORM_BLOCK_SIZE >= 16384.
constexpr std::size_t portable_material_uniform_bytes = 16384;

// Every sampler kind reflects as a *Texture* resource: one sampler as an
// OBJECT, a sampler array as an ARRAY of them.
[[nodiscard]] bool is_sampler(const ReflectedUniform& uniform) {
    const bool texture = uniform.hint_string.find("Texture") != std::string::npos;
    return texture && ((uniform.type == Variant::OBJECT && uniform.hint == PROPERTY_HINT_RESOURCE_TYPE)
        || (uniform.type == Variant::ARRAY && uniform.hint == PROPERTY_HINT_ARRAY_TYPE));
}

struct UniformDeclaration final {
    std::string type;
    std::optional<std::size_t> array;     // `[N]` after the type or the name
    std::vector<std::string> hints;       // identifiers between ':' and ';'
};

// The `uniform` declarations of `name` in the token stream. Reflection gives
// neither an array's length nor a sampler's engine hint, so both are read
// here; a caller treats anything but exactly one declaration as unreadable.
[[nodiscard]] std::vector<UniformDeclaration> uniform_declarations(
    const std::vector<std::string>& tokens, const std::string_view name) {
    const auto length = [&tokens](std::size_t& at) -> std::optional<std::size_t> {
        if (at + 2 >= tokens.size() || tokens[at] != "[" || tokens[at + 2] != "]") return std::nullopt;
        const std::string& digits = tokens[at + 1];
        if (digits.empty() || digits.size() > 6
            || !std::all_of(digits.begin(), digits.end(), [](const char c) { return c >= '0' && c <= '9'; })) {
            return std::nullopt;
        }
        at += 3;
        return static_cast<std::size_t>(std::stoul(digits));
    };
    std::vector<UniformDeclaration> result;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (tokens[index] != "uniform") continue;
        std::size_t at = index + 1;
        if (at < tokens.size() && (tokens[at] == "lowp" || tokens[at] == "mediump" || tokens[at] == "highp")) ++at;
        if (at + 1 >= tokens.size()) break;
        UniformDeclaration declaration{.type = tokens[at++], .array = std::nullopt, .hints = {}};
        declaration.array = length(at);
        if (at >= tokens.size() || tokens[at] != name) continue;
        ++at;
        if (!declaration.array) declaration.array = length(at);
        if (at < tokens.size() && tokens[at] == ":") {
            for (++at; at < tokens.size() && tokens[at] != ";"; ++at) {
                const char first = tokens[at].front();
                if (std::isalpha(static_cast<unsigned char>(first)) != 0 || first == '_') {
                    declaration.hints.push_back(tokens[at]);
                }
            }
        }
        result.push_back(std::move(declaration));
    }
    return result;
}

// Godot fills these samplers itself; they are not material texture units and
// no binding can supply them.
[[nodiscard]] bool engine_supplied_sampler(const std::vector<std::string>& tokens, const std::string_view name) {
    const auto declarations = uniform_declarations(tokens, name);
    if (declarations.size() != 1) return false;
    const auto& hints = declarations.front().hints;
    return std::any_of(hints.begin(), hints.end(), [](const std::string& hint) {
        return hint == "hint_screen_texture" || hint == "hint_depth_texture"
            || hint == "hint_normal_roughness_texture";
    });
}

// std140 size and alignment Godot's shader compiler gives one non-array
// uniform, from its reflected type (servers/rendering/shader_compiler.cpp).
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> uniform_layout(const ReflectedUniform& uniform) {
    switch (uniform.type) {
    case Variant::BOOL:
    case Variant::FLOAT: return std::pair<std::size_t, std::size_t>{4, 4};
    case Variant::INT:
        // bvec2/3/4 reflect as INT flags named by their components.
        if (uniform.hint == PROPERTY_HINT_FLAGS) {
            if (uniform.hint_string == "x,y") return std::pair<std::size_t, std::size_t>{8, 8};
            if (uniform.hint_string == "x,y,z") return std::pair<std::size_t, std::size_t>{12, 16};
            if (uniform.hint_string == "x,y,z,w") return std::pair<std::size_t, std::size_t>{16, 16};
        }
        return std::pair<std::size_t, std::size_t>{4, 4};
    case Variant::VECTOR2:
    case Variant::VECTOR2I: return std::pair<std::size_t, std::size_t>{8, 8};
    case Variant::VECTOR3:
    case Variant::VECTOR3I: return std::pair<std::size_t, std::size_t>{12, 16};
    case Variant::VECTOR4:
    case Variant::VECTOR4I: return std::pair<std::size_t, std::size_t>{16, 16};
    case Variant::COLOR:
        return uniform.hint == PROPERTY_HINT_COLOR_NO_ALPHA
            ? std::pair<std::size_t, std::size_t>{12, 16} : std::pair<std::size_t, std::size_t>{16, 16};
    case Variant::TRANSFORM2D: return std::pair<std::size_t, std::size_t>{32, 16};
    case Variant::BASIS: return std::pair<std::size_t, std::size_t>{48, 16};
    case Variant::PROJECTION: return std::pair<std::size_t, std::size_t>{64, 16};
    default: return std::nullopt;
    }
}

// Size of one array element's declared GLSL type, or nullopt.
[[nodiscard]] std::optional<std::size_t> element_size(const std::string_view type) {
    for (const std::string_view prefix : {"", "i", "u", "b"}) {
        if (type.substr(0, prefix.size()) != prefix) continue;
        const std::string_view rest = type.substr(prefix.size());
        if (rest == "vec2") return 8;
        if (rest == "vec3") return 12;
        if (rest == "vec4") return 16;
    }
    if (type == "float" || type == "int" || type == "uint" || type == "bool") return 4;
    if (type == "mat2") return 32;
    if (type == "mat3") return 48;
    if (type == "mat4") return 64;
    return std::nullopt;
}

// Whether Godot applies a binding of this kind to the reflected uniform
// unchanged. A float3 binds a vec2 because MaterialBinding has no float2
// kind; every other pairing Godot would truncate, zero-fill or ignore.
[[nodiscard]] bool binding_fits(const assets::ParameterValue& value, const ReflectedUniform& uniform) {
    switch (value.index()) {
    case 0: // integer
        return (uniform.type == Variant::INT && uniform.hint != PROPERTY_HINT_FLAGS)
            || uniform.type == Variant::FLOAT || uniform.type == Variant::BOOL;
    case 1: return uniform.type == Variant::FLOAT;
    case 2: // float3
        return uniform.type == Variant::VECTOR3 || uniform.type == Variant::VECTOR2
            || (uniform.type == Variant::COLOR && uniform.hint == PROPERTY_HINT_COLOR_NO_ALPHA);
    case 3: // float4
        return uniform.type == Variant::VECTOR4
            || (uniform.type == Variant::COLOR && uniform.hint != PROPERTY_HINT_COLOR_NO_ALPHA);
    default: // texture: one texture fills one sampler, never a sampler array
        return is_sampler(uniform) && uniform.type == Variant::OBJECT;
    }
}

[[nodiscard]] std::string reflected_kind(const ReflectedUniform& uniform) {
    if (is_sampler(uniform)) return uniform.type == Variant::ARRAY ? "a sampler array" : "a sampler";
    return utf8(Variant::get_type_name(uniform.type));
}

// Why the material's bindings do not reach the compiled shader exactly as
// written, or nullopt. Every binding must name one declared uniform of a
// fitting type, at most once, and every material sampler must receive a
// texture. The legacy route differs in two ways: its bindings are the ALO's
// authored effect parameters, and one the adapter does not declare is not
// consumed (the retail effect system also ignores a parameter an effect does
// not declare), and the renderer binds BaseTexture itself. The fog texture is
// bound by the fog backend, and engine-supplied samplers by Godot.
[[nodiscard]] std::optional<std::string> binding_problem(const MaterialDescription& source,
    const std::vector<ReflectedUniform>& uniforms, const std::vector<std::string>& tokens) {
    const bool legacy_route = source.route == MaterialRoute::legacy_effect;
    for (std::size_t index = 0; index < source.bindings.size(); ++index) {
        const MaterialBinding& binding = source.bindings[index];
        const std::string name = legacy::bindings::echoed(binding.name);
        for (std::size_t later = index + 1; later < source.bindings.size(); ++later) {
            if (source.bindings[later].name == binding.name) return "binds " + name + " more than once";
        }
        const auto uniform = std::find_if(uniforms.begin(), uniforms.end(),
            [&binding](const ReflectedUniform& candidate) { return candidate.name == binding.name; });
        if (uniform == uniforms.end()) {
            if (legacy_route) continue;
            return "binds " + name + ", which its shader does not declare";
        }
        if (!binding_fits(binding.value, *uniform)) {
            return "binds " + name + " as " + std::string(legacy::bindings::kind_name(binding.value))
                + ", but its shader declares " + reflected_kind(*uniform);
        }
    }
    for (const ReflectedUniform& uniform : uniforms) {
        if (!is_sampler(uniform) || engine_supplied_sampler(tokens, uniform.name)) continue;
        if (uniform.name == GodotFogBackend::texture_parameter) continue;
        if (legacy_route && uniform.name == "BaseTexture") continue;
        const bool bound = std::any_of(source.bindings.begin(), source.bindings.end(),
            [&uniform](const MaterialBinding& binding) {
                return binding.name == uniform.name && std::holds_alternative<std::string>(binding.value);
            });
        if (!bound) return "leaves sampler " + legacy::bindings::echoed(uniform.name) + " unbound";
    }
    return std::nullopt;
}

} // namespace

// Derives the shadow-receiving variant of a legacy adapter (P1-04). The
// adapters are unshaded because they evaluate their own irradiance; receiving
// a Godot directional shadow needs the lit pipeline, so `unshaded` is replaced
// by `ambient_light_disabled` and a light() function contributes exactly
// mix(shadow_floor, 1, ATTENUATION) per channel, making the lit result the
// unshaded result times the shadow term. An adapter without the expected
// render mode is reported rather than guessed at.
[[nodiscard]] std::optional<std::string> shadow_receiving_variant(const std::string_view source) {
    constexpr std::string_view unshaded = "render_mode unshaded,";
    const std::size_t position = source.find(unshaded);
    if (position == std::string_view::npos) return std::nullopt;
    std::string result(source);
    result.replace(position, unshaded.size(), "render_mode ambient_light_disabled,");
    result += "\nuniform vec3 eawr_shadow_floor = vec3(0.5);\n"
              "void light() {\n"
              "    DIFFUSE_LIGHT += mix(eawr_shadow_floor, vec3(1.0), ATTENUATION);\n"
              "}\n";
    return result;
}

// The comment-free tokens of shader text: each identifier or number is one
// token and every other non-space character is its own token. nullopt for an
// unterminated block comment.
[[nodiscard]] std::optional<std::vector<std::string>> shader_tokens(const std::string_view code) {
    std::string text;
    text.reserve(code.size());
    for (std::size_t at = 0; at < code.size();) {
        if (code.compare(at, 2, "//") == 0) {
            at = code.find('\n', at);
            if (at == std::string_view::npos) break;
        } else if (code.compare(at, 2, "/*") == 0) {
            const std::size_t end = code.find("*/", at + 2);
            if (end == std::string_view::npos) return std::nullopt;
            text.push_back(' ');
            at = end + 2;
        } else {
            text.push_back(code[at++]);
        }
    }
    const auto identifier_char = [](const char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    };
    std::vector<std::string> tokens;
    for (std::size_t at = 0; at < text.size();) {
        const char c = text[at];
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            ++at;
        } else if (identifier_char(c)) {
            const std::size_t begin = at;
            while (at < text.size() && identifier_char(text[at])) ++at;
            tokens.emplace_back(text, begin, at - begin);
        } else {
            tokens.emplace_back(1, c);
            ++at;
        }
    }
    return tokens;
}

// RenderingServer's reflected material uniforms of a compiled shader, in
// declaration order, without group/category entries. Global and instance
// uniforms are not listed.
[[nodiscard]] std::vector<ReflectedUniform> reflected_uniforms(RenderingServer& rendering, const RID& shader) {
    std::vector<ReflectedUniform> result;
    const TypedArray<Dictionary> parameters = rendering.get_shader_parameter_list(shader);
    for (int64_t index = 0; index < parameters.size(); ++index) {
        const Dictionary parameter = parameters[index];
        const int64_t usage = static_cast<int64_t>(parameter.get("usage", 0));
        if ((usage & (PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SUBGROUP | PROPERTY_USAGE_CATEGORY)) != 0) continue;
        result.push_back({
            utf8(String(static_cast<StringName>(parameter.get("name", StringName())))),
            static_cast<Variant::Type>(static_cast<int64_t>(parameter.get("type", Variant::NIL))),
            static_cast<int64_t>(parameter.get("hint", PROPERTY_HINT_NONE)),
            utf8(static_cast<String>(parameter.get("hint_string", String()))),
        });
    }
    return result;
}

// Why a compiled shader would not build on every Vulkan driver or, for
// the Compatibility fallback, on every GL 3.3 driver, or nullopt: more
// material samplers than
// portable_material_samplers, or a MaterialUniforms block larger than
// portable_material_uniform_bytes. An array whose declared length cannot be
// read fails closed. `tokens` is the source the shader was compiled from.
[[nodiscard]] std::optional<std::string> portable_limit_problem(
    const std::vector<ReflectedUniform>& uniforms, const std::vector<std::string>& tokens) {
    std::size_t samplers{};
    std::size_t offset{};
    for (const ReflectedUniform& uniform : uniforms) {
        const std::string name = legacy::bindings::echoed(uniform.name);
        const bool array = uniform.type == Variant::ARRAY
            || (uniform.type >= Variant::PACKED_BYTE_ARRAY && uniform.type <= Variant::PACKED_VECTOR4_ARRAY);
        std::optional<UniformDeclaration> declaration;
        if (array) {
            auto declarations = uniform_declarations(tokens, uniform.name);
            if (declarations.size() != 1 || !declarations.front().array) {
                return "declares uniform array " + name + " whose length cannot be read from its source";
            }
            declaration = std::move(declarations.front());
        }
        if (is_sampler(uniform)) {
            if (engine_supplied_sampler(tokens, uniform.name)) continue;
            samplers += declaration ? *declaration->array : 1;
            continue;
        }
        std::size_t size{};
        std::size_t alignment{16};
        if (declaration) {
            const auto element = element_size(declaration->type);
            if (!element) return "declares uniform array " + name + " of an unrecognised element type";
            const std::size_t count = *declaration->array;
            size = *element * count;
            if (size % (16 * count) != 0) size += 16 * count - size % (16 * count);
        } else {
            const auto layout = uniform_layout(uniform);
            if (!layout) {
                return "declares uniform " + name + " of reflected type "
                    + utf8(Variant::get_type_name(uniform.type)) + ", whose block layout is not known";
            }
            size = layout->first;
            alignment = layout->second;
        }
        if (offset % alignment != 0) offset += alignment - offset % alignment;
        offset += size;
    }
    if (offset % 16 != 0) offset += 16 - offset % 16;
    if (samplers > portable_material_samplers) {
        return "declares " + std::to_string(samplers) + " material samplers; at most "
            + std::to_string(portable_material_samplers)
            + " are portable: the Compatibility fallback binds its own samplers within GL 3.3's 16 texture units";
    }
    if (offset > portable_material_uniform_bytes) {
        return "needs a " + std::to_string(offset) + "-byte material uniform block; Vulkan and GL 3.3 guarantee "
            + std::to_string(portable_material_uniform_bytes) + " bytes";
    }
    return std::nullopt;
}

[[nodiscard]] core::Result<void> GodotRenderer::Impl::upload(
    const sim::AssetId asset_id,
    const assets::Model& model,
    const assets::Texture& texture,
    const MaterialDescription& description,
    const std::span<const GodotRenderer::BindingTexture> binding_textures) {
    // Each per-binding texture names a distinct texture-valued binding.
    std::vector<detail::NamedTexture> named;
    for (const GodotRenderer::BindingTexture& item : binding_textures) {
        const bool bound = std::any_of(description.bindings.begin(), description.bindings.end(),
            [&](const MaterialBinding& binding) {
                return binding.name == item.binding && std::holds_alternative<std::string>(binding.value);
            });
        const bool repeated = std::any_of(named.begin(), named.end(),
            [&](const detail::NamedTexture& seen) { return seen.binding == item.binding; });
        if (!bound || repeated) {
            return failure(diagnostic_codes::invalid_material,
                "per-binding texture '" + item.binding + "' of asset " + std::to_string(asset_id)
                    + (repeated ? " is given twice" : " names no texture-valued material binding"));
        }
        named.push_back({item.binding, &item.texture});
    }
    const detail::UploadIdentity identity = detail::upload_identity(model, texture, description, named);
    if (const auto existing = resources_.find(asset_id); existing != resources_.end()) {
        // Different content under a live asset ID would silently keep the
        // old RIDs; replacing an asset is an explicit release then upload.
        if (existing->second.identity != identity) {
            return failure(diagnostic_codes::duplicate_asset,
                "renderer asset " + std::to_string(asset_id)
                    + " is already uploaded with different model, texture or material content");
        }
        static_cast<void>(leases_.retain(asset_id));
        return core::Result<void>::success();
    }
    if (auto valid = validate_material(description); !valid) {
        // Validation includes the modern spatial preflight. Preserve its
        // exact bounded core Diagnostic in the renderer-visible history.
        diagnostics_.push(valid.error());
        return valid;
    }
    // A legacy/ family's extra samplers read only their own texture; the
    // base texture is never substituted for one (gate MULTITEX-01).
    for (const legacy::TextureBinding& sampled : legacy::binding_textures(description)) {
        const std::string_view required = sampled.name;
        const bool carried = std::any_of(named.begin(), named.end(),
            [&](const detail::NamedTexture& item) { return item.binding == required; });
        if (!carried) {
            return failure(diagnostic_codes::invalid_material, material_label(description, false)
                + " material for asset " + std::to_string(asset_id) + " binds " + legacy::bindings::echoed(required)
                + ", but the upload carries no per-binding texture for it (gate MULTITEX-01)");
        }
    }
    for (const MaterialBinding& binding : description.bindings) {
        const auto* name = std::get_if<std::string>(&binding.value);
        if (name != nullptr && shared_name(*name) && !shared_textures_.contains(shared_key(*name))) {
            return failure(diagnostic_codes::upload_failed,
                "renderer asset " + std::to_string(asset_id) + " binds unregistered shared texture '"
                    + shared_key(*name) + "'");
        }
    }
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering || !scenario_.is_valid()) {
        return failure(diagnostic_codes::backend_unavailable,
            "Godot renderer was not initialized with a valid scenario");
    }

    Resource resource;
    MaterialRefusal refusal;
    bool texture_uploaded = upload_texture(*rendering, texture, resource.texture);
    for (const GodotRenderer::BindingTexture& item : binding_textures) {
        if (!texture_uploaded) break;
        RID bound;
        texture_uploaded = upload_texture(*rendering, item.texture, bound);
        if (bound.is_valid()) resource.binding_textures.emplace_back(item.binding, bound);
    }
    const MaterialUpload material_uploaded = texture_uploaded
        ? upload_material(*rendering, description, resource, refusal)
        : MaterialUpload::failed;
    if (!texture_uploaded || material_uploaded != MaterialUpload::success
        || !upload_mesh(*rendering, model, resource, legacy::authored_binormals(description))) {
        free_resource(*rendering, resource);
        if (material_uploaded == MaterialUpload::shader_compile_failed) {
            return failure(diagnostic_codes::shader_compile_failed,
                compile_failure_message(description, resource.shadow_receiving, asset_id));
        }
        if (material_uploaded == MaterialUpload::refused) {
            return failure(refusal.code, material_label(description, resource.shadow_receiving) + " "
                + std::string(refusal.subject) + " for asset " + std::to_string(asset_id) + " " + refusal.reason);
        }
        return failure(diagnostic_codes::upload_failed,
            "Godot rejected mesh, texture, or material resources for asset "
                + std::to_string(asset_id));
    }
    // Counted only for a resource that exists; a failed mesh upload after
    // a compiled shadow variant frees that variant and leaves no count.
    if (resource.shadow_receiving) ++shadow_receiving_;
    if (resource.shadow_variant_failed) ++shadow_variant_failures_;
    resource.pass = description.pass;
    resource.description = description;
    resource.identity = identity;
    resources_.emplace(asset_id, std::move(resource));
    ++upload_generation_; // #888: the kept submission order resolves assets again
    static_cast<void>(leases_.upload(asset_id));
    return core::Result<void>::success();
}

[[nodiscard]] core::Result<void> GodotRenderer::Impl::retain(const sim::AssetId asset_id) {
    const auto found = resources_.find(asset_id);
    if (found == resources_.end()) {
        return failure(diagnostic_codes::missing_asset,
            "cannot retain missing renderer asset " + std::to_string(asset_id));
    }
    static_cast<void>(leases_.retain(asset_id));
    return core::Result<void>::success();
}

[[nodiscard]] core::Result<void> GodotRenderer::Impl::release(const sim::AssetId asset_id) {
    const auto found = resources_.find(asset_id);
    if (found == resources_.end()) {
        return failure(diagnostic_codes::missing_asset,
            "cannot release missing renderer asset " + std::to_string(asset_id));
    }
    const detail::ResourceLeaseLedger::Release disposition = leases_.release(asset_id);
    if (disposition == detail::ResourceLeaseLedger::Release::retained) {
        return core::Result<void>::success();
    }
    // Unregister the fog consumer before any of its material RIDs is freed.
    if (const auto consumer = fog_consumers_.find(asset_id); consumer != fog_consumers_.end()) {
        detach_fog_consumer(asset_id, consumer->second);
        fog_consumers_.erase(consumer);
    }
    fog_unsupported_.erase(asset_id);
    RenderingServer* rendering = RenderingServer::get_singleton();
    for (auto current = instances_.begin(); current != instances_.end();) {
        if (current->second.asset_id != asset_id) {
            ++current;
            continue;
        }
        current = remove_instance(rendering, current);
    }
    // A pose set for this asset on an entity without its instance (not
    // yet submitted, or waiting while the asset was missing) would
    // otherwise outlive the asset and bind to a later re-upload.
    std::erase_if(skin_poses_, [asset_id](const auto& pose) {
        return pose.second.asset_id == asset_id;
    });
    if (found->second.shadow_receiving) --shadow_receiving_;
    if (found->second.shadow_variant_failed) --shadow_variant_failures_;
    if (rendering) free_resource(*rendering, found->second);
    resources_.erase(found);
    ++upload_generation_;
    return core::Result<void>::success();
}

[[nodiscard]] core::Result<void> GodotRenderer::Impl::register_shared_texture(
    const std::string_view name, const assets::Texture& texture) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering) {
        return failure(diagnostic_codes::backend_unavailable, "Godot RenderingServer is unavailable");
    }
    const std::string key(name);
    if (key.empty() || shared_textures_.contains(key)) {
        return failure(diagnostic_codes::upload_failed,
            "shared texture '" + key + "' is empty or already registered");
    }
    RID target;
    if (!upload_texture(*rendering, texture, target)) {
        return failure(diagnostic_codes::upload_failed,
            "Godot rejected shared texture '" + key + "'");
    }
    shared_textures_.emplace(key, target);
    return core::Result<void>::success();
}

[[nodiscard]] core::Result<void> GodotRenderer::Impl::register_shared_texture_array(
    const std::string_view name, const std::span<const assets::Texture> layers, const std::uint32_t edge) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering) {
        return failure(diagnostic_codes::backend_unavailable, "Godot RenderingServer is unavailable");
    }
    const std::string key(name);
    if (key.empty() || shared_textures_.contains(key) || layers.empty() || edge == 0 || edge > 4096) {
        return failure(diagnostic_codes::upload_failed,
            "shared texture array '" + key + "' is empty, already registered or has an invalid edge");
    }
    TypedArray<Image> images;
    for (const assets::Texture& layer : layers) {
        const Image::Format format = image_format(layer.format);
        if (format == Image::FORMAT_MAX || layer.mips.empty()) {
            return failure(diagnostic_codes::upload_failed,
                "shared texture array '" + key + "' has a layer in an unsupported format");
        }
        // The top mip alone: the chain is regenerated after the resize.
        const assets::MipLevel& top = layer.mips.front();
        PackedByteArray bytes;
        bytes.resize(static_cast<int64_t>(top.bytes.size()));
        if (!top.bytes.empty()) std::memcpy(bytes.ptrw(), top.bytes.data(), top.bytes.size());
        Ref<Image> image = Image::create_from_data(static_cast<int32_t>(layer.width),
            static_cast<int32_t>(layer.height), false, format, bytes);
        if (image.is_null() || image->is_empty()) {
            return failure(diagnostic_codes::upload_failed,
                "Godot rejected a layer of shared texture array '" + key + "'");
        }
        if (image->is_compressed() && image->decompress() != OK) {
            return failure(diagnostic_codes::upload_failed,
                "a layer of shared texture array '" + key + "' could not be decompressed");
        }
        image->convert(Image::FORMAT_RGBA8);
        if (image->get_width() != static_cast<int32_t>(edge) || image->get_height() != static_cast<int32_t>(edge)) {
            image->resize(static_cast<int32_t>(edge), static_cast<int32_t>(edge), Image::INTERPOLATE_CUBIC);
        }
        image->generate_mipmaps();
        images.push_back(image);
    }
    const RID texture = rendering->texture_2d_layered_create(images, RenderingServer::TEXTURE_LAYERED_2D_ARRAY);
    if (!texture.is_valid()) {
        return failure(diagnostic_codes::upload_failed,
            "Godot rejected shared texture array '" + key + "'");
    }
    shared_textures_.emplace(key, texture);
    return core::Result<void>::success();
}

[[nodiscard]] bool GodotRenderer::Impl::upload_texture(
    RenderingServer& rendering, const assets::Texture& source, RID& target) {
    const Image::Format format = image_format(source.format);
    if (format == Image::FORMAT_MAX || source.mips.empty()) return false;
    std::size_t byte_count{};
    for (const assets::MipLevel& mip : source.mips) byte_count += mip.bytes.size();
    PackedByteArray bytes;
    bytes.resize(static_cast<int64_t>(byte_count));
    std::size_t offset{};
    for (const assets::MipLevel& mip : source.mips) {
        if (!mip.bytes.empty()) {
            std::memcpy(bytes.ptrw() + offset, mip.bytes.data(), mip.bytes.size());
        }
        offset += mip.bytes.size();
    }
    const Ref<Image> image = Image::create_from_data(
        static_cast<int32_t>(source.width), static_cast<int32_t>(source.height),
        source.mips.size() > 1, format, bytes);
    if (image.is_null() || image->is_empty()) return false;
    target = rendering.texture_2d_create(image);
    return target.is_valid();
}

// RenderingServer's reflected parameter list becomes empty when Godot's
// shading-language compiler rejects the source, so a uniform the source is
// known to declare reflects only after that compiler accepted it; neither
// engine stderr nor RID validity is consulted. The backend's per-variant
// GLSL compile by the driver happens later and is not observed here.
[[nodiscard]] bool GodotRenderer::Impl::shader_compiled(
    RenderingServer& rendering, const RID& shader, const StringName& declared) {
    const TypedArray<Dictionary> parameters = rendering.get_shader_parameter_list(shader);
    for (int64_t index = 0; index < parameters.size(); ++index) {
        const Dictionary parameter = parameters[index];
        if (static_cast<StringName>(parameter.get("name", StringName())) == declared) return true;
    }
    return false;
}

// Modern sources declare nothing the renderer knows, so it injects an
// otherwise-unused eawr_compile_probe directly after the leading
// `shader_type spatial;`. Anchoring it there (not at the first `;`, which
// may sit in a comment or another shader type's declaration) means an
// observed probe also shows that compiler accepted the source as spatial.
[[nodiscard]] std::optional<std::string> GodotRenderer::Impl::with_compile_probe(std::string source) {
    const auto declaration_end = spatial_declaration_end(source);
    if (!declaration_end) return std::nullopt;
    source.insert(*declaration_end, "\nuniform float eawr_compile_probe;\n");
    return source;
}

// Legacy selectors reaching upload passed validate_material, so they name
// an adapter row; modern shader text never enters the message.
[[nodiscard]] std::string GodotRenderer::Impl::compile_failure_message(
    const MaterialDescription& source, const bool shadow_receiving, const sim::AssetId asset_id) {
    if (source.route == MaterialRoute::modern_spatial) {
        return "Godot rejected modern spatial shader compilation for asset "
            + std::to_string(asset_id);
    }
    return "Godot rejected the " + source.program + " " + source.technique + "/"
        + source.pass_name + (shadow_receiving ? " shadow-receiving" : "")
        + " adapter shader compilation for asset " + std::to_string(asset_id);
}

// The selector a refusal names; legacy selectors reaching upload passed
// validate_material, so they are known, bounded family names.
[[nodiscard]] std::string GodotRenderer::Impl::material_label(const MaterialDescription& source, const bool shadow_receiving) {
    if (source.route == MaterialRoute::modern_spatial) return "modern spatial";
    return source.program + " " + source.technique + "/" + source.pass_name
        + (shadow_receiving ? " shadow-receiving" : "");
}

[[nodiscard]] std::optional<std::string_view> GodotRenderer::Impl::legacy_shader_source(
    const MaterialDescription& source) {
    if (legacy::find_family(source) != nullptr) return legacy::shader_source(source);
    if (source.program == "MeshGloss.fx"
        && source.technique == "sph_t0" && source.pass_name == "sph_t0_p0") {
        return source.pass == RenderPass::transparent
            ? meshgloss_shader_alpha : meshgloss_shader_opaque;
    }
    // Fixed-function MESH/BATCHMESH passes (P1-11). The transparent-phase
    // effects are accepted only in the transparent pass and the opaque one
    // only in the opaque pass, so a caller cannot move either across.
    if ((source.program == "MeshAlpha.fx" || source.program == "MeshAlphaGloss.fx"
            || source.program == "BatchMeshAlpha.fx")
        && source.technique == "sph_t1" && source.pass_name == "sph_t1_p0") {
        return source.pass == RenderPass::transparent
            ? std::optional<std::string_view>{fixed_mesh_shader_alpha} : std::nullopt;
    }
    if (source.pass != RenderPass::opaque) return std::nullopt;
    if (source.program == "BatchMeshGloss.fx"
        && source.technique == "sph_t1" && source.pass_name == "sph_t1_p0") {
        return fixed_mesh_shader_opaque;
    }
    const bool rskin =
        (source.program == "RSkinBumpColorize.fx"
            && source.technique == "sph_t0" && source.pass_name == "sph_t0_p0")
        || (source.program == "RSkinGloss.fx"
            && source.technique == "sph_t1" && source.pass_name == "sph_t1_p0")
        || (source.program == "RSkinGlossColorize.fx"
            && source.technique == "sph_t0" && source.pass_name == "sph_t0_p0")
        || (source.program == "MeshBumpColorize.fx"
            && source.technique == "t0" && source.pass_name == "t0_p0");
    return rskin ? std::optional<std::string_view>{rskin_shader_opaque} : std::nullopt;
}

[[nodiscard]] GodotRenderer::Impl::MaterialUpload GodotRenderer::Impl::upload_material(
    RenderingServer& rendering,
    const MaterialDescription& source,
    Resource& target,
    MaterialRefusal& refusal) {
    target.shader = rendering.shader_create();
    std::string_view shader_source;
    std::optional<std::string> modern_source;
    std::string receiving_source;
    // Legacy adapters all declare BaseTexture; modern sources carry the
    // injected probe. Either must reflect before the material is built.
    StringName declared("BaseTexture");
    if (source.route == MaterialRoute::legacy_effect) {
        const auto selected = legacy_shader_source(source);
        // validate_material() is the public fail-closed gate, but keep the
        // upload selection independently exhaustive: a future selector
        // cannot silently inherit MeshGloss or the RSKIN adapter.
        if (!selected) return MaterialUpload::failed;
        shader_source = *selected;
        if (lighting_ && lighting_->shadows && legacy::receives_shadows(source)) {
            if (auto variant = shadow_receiving_variant(*selected)) {
                receiving_source = std::move(*variant);
                shader_source = receiving_source;
                target.shadow_receiving = true;
            } else {
                target.shadow_variant_failed = true;
            }
        }
    } else {
        modern_source = with_compile_probe(source.program.empty()
            ? std::string(modern_shader_smoke) : source.program);
        if (!modern_source) return MaterialUpload::shader_compile_failed;
        shader_source = *modern_source;
        declared = StringName("eawr_compile_probe");
    }
    const std::string uploaded = stored_output::backend_source(shader_source);
    rendering.shader_set_code(target.shader, String::utf8(
        uploaded.data(), static_cast<int64_t>(uploaded.size())));
    if (!shader_compiled(rendering, target.shader, declared)) {
        return MaterialUpload::shader_compile_failed;
    }
    // Admission of the compiled shader, before any material RID exists:
    // it must build on every Vulkan and GL 3.3 driver, and the bindings must reach it
    // exactly as written.
    const std::vector<std::string> tokens = shader_tokens(uploaded).value_or(std::vector<std::string>{});
    const std::vector<ReflectedUniform> uniforms = reflected_uniforms(rendering, target.shader);
    if (auto problem = portable_limit_problem(uniforms, tokens)) {
        refusal = {diagnostic_codes::shader_compile_failed, "shader", std::move(*problem)};
        return MaterialUpload::refused;
    }
    if (auto problem = binding_problem(source, uniforms, tokens)) {
        refusal = {diagnostic_codes::invalid_material, "material", std::move(*problem)};
        return MaterialUpload::refused;
    }
    target.material = rendering.material_create();
    rendering.material_set_shader(target.material, target.shader);
    configure_material(rendering, source, target.material, target.texture, target.binding_textures);
    return target.shader.is_valid() && target.material.is_valid()
        ? MaterialUpload::success : MaterialUpload::failed;
}

// Everything a material needs besides its shader. The opt-in fog variant
// is configured by this same function, so both carry identical pass
// priority, bindings, legacy constants and lighting.
void GodotRenderer::Impl::configure_material(
    RenderingServer& rendering,
    const MaterialDescription& source,
    const RID& material,
    const RID& texture,
    const std::vector<std::pair<std::string, RID>>& binding_textures) const {
    rendering.material_set_render_priority(material, render_pass_priority(source.pass));
    for (const MaterialBinding& binding : source.bindings) {
        Variant value;
        std::visit([&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::int32_t> || std::is_same_v<T, float>) {
                value = item;
            } else if constexpr (std::is_same_v<T, assets::Vec3f>) {
                value = Vector3(item.x, item.y, item.z);
            } else if constexpr (std::is_same_v<T, assets::Vec4f>) {
                value = Vector4(item.x, item.y, item.z, item.w);
            } else if constexpr (std::is_same_v<T, std::string>) {
                const auto shared = shared_name(item)
                    ? shared_textures_.find(shared_key(item)) : shared_textures_.end();
                value = shared != shared_textures_.end() ? shared->second : texture;
                for (const auto& [name, bound] : binding_textures) {
                    if (name == binding.name) value = bound;
                }
            }
        }, binding.value);
        rendering.material_set_param(material, StringName(binding.name.c_str()), value);
    }
    if (source.route == MaterialRoute::legacy_effect) {
        rendering.material_set_param(material, StringName("BaseTexture"), texture);
        const Vector3 light_direction = Vector3(0.35F, 0.75F, 0.56F).normalized();
        const std::array<float, 3> light{
            static_cast<float>(light_direction.x), static_cast<float>(light_direction.y),
            static_cast<float>(light_direction.z)};
        rendering.material_set_param(material, StringName("eawr_sph_r"),
            godot_matrix(meshgloss_hemisphere_matrix(0.08F, 2.0F, light)));
        rendering.material_set_param(material, StringName("eawr_sph_g"),
            godot_matrix(meshgloss_hemisphere_matrix(0.08F, 1.88F, light)));
        rendering.material_set_param(material, StringName("eawr_sph_b"),
            godot_matrix(meshgloss_hemisphere_matrix(0.10F, 1.72F, light)));
        // Adapters that light the sun per pixel read the fill lights alone
        // (SPH_LIGHT_FILL) and the sun's diffuse colour: under the P0
        // constants the fill is the ambient term and the sun the directional.
        rendering.material_set_param(material, StringName("eawr_sph_fill_r"),
            godot_matrix(meshgloss_hemisphere_matrix(0.08F, 0.0F, light)));
        rendering.material_set_param(material, StringName("eawr_sph_fill_g"),
            godot_matrix(meshgloss_hemisphere_matrix(0.08F, 0.0F, light)));
        rendering.material_set_param(material, StringName("eawr_sph_fill_b"),
            godot_matrix(meshgloss_hemisphere_matrix(0.10F, 0.0F, light)));
        rendering.material_set_param(material, StringName("eawr_light_diffuse"), Vector3(2.0F, 1.88F, 1.72F));
        rendering.material_set_param(material, StringName("eawr_eye_position"),
            Vector3(0.0F, 420.0F, 1050.0F));
        rendering.material_set_param(material, StringName("eawr_light_direction"), light_direction);
        rendering.material_set_param(material, StringName("eawr_light_specular"),
            Vector3(2.0F, 1.88F, 1.72F));
        rendering.material_set_param(material, StringName("eawr_light_scale"),
            Vector4(1.0F, 1.0F, 1.0F,
                source.pass == RenderPass::transparent && source.program == "MeshGloss.fx"
                    ? 0.5F : 1.0F));
        for (const legacy::Uniform& uniform : legacy::uniforms(source)) {
            rendering.material_set_param(material, StringName(String::utf8(uniform.name.data(),
                static_cast<int64_t>(uniform.name.size()))), legacy_uniform_value(uniform.value));
        }
    }
    // Without a lighting state the P0 constants above stand unchanged.
    apply_lighting_params(rendering, material);
    apply_wind_params(rendering, material, source);
}

[[nodiscard]] bool GodotRenderer::Impl::upload_mesh(
    RenderingServer& rendering, const assets::Model& source, Resource& target, const bool authored_binormals) {
    target.mesh = rendering.mesh_create();
    target.bone_count = source.bones.size();
    bool has_billboard = false;
    for (const assets::Bone& bone : source.bones) {
        const std::uint32_t mode = bone.billboard & 15U;
        has_billboard = has_billboard || mode == 1 || mode == 2 || mode == 3 || mode == 6 || mode == 7;
    }
    if (has_billboard) {
        auto player = animation::Player::create(source);
        if (!player) return false;
        auto rest_pose = player.value().sample({});
        if (!rest_pose) return false;
        for (std::size_t index = 0; index < source.bones.size(); ++index) {
            target.billboard_modes.push_back(source.bones[index].billboard);
            target.bone_parents.push_back(source.bones[index].parent);
            target.bind_models.push_back(transform_from(animation::Player::asset_to_render_transform(
                rest_pose.value().bones[index].model_asset)));
            const auto& bind = source.bones[index].relative_transform;
            target.billboard_distances.push_back(std::hypot(bind[3], bind[7], bind[11]));
        }
    }
    // Rigid ALO vertices are stored in their bone's space and move to bind
    // model space here, so one animated * inverse(bind) palette places
    // both routes (Player::rigid_vertex_to_bind).
    std::optional<animation::Pose> rest;
    for (const assets::Mesh& mesh : source.meshes) {
        if (!mesh.visible) continue;
        for (const assets::Submesh& submesh : mesh.submeshes) {
            const bool palette_skinned = !submesh.skin_bones.empty();
            const bool rigid_skinned = !palette_skinned && mesh.bone >= 0;
            if ((palette_skinned || rigid_skinned) && source.bones.empty()) return false;
            if (rigid_skinned
                && static_cast<std::size_t>(mesh.bone) >= source.bones.size()) return false;
            if (rigid_skinned && !rest) {
                auto player = animation::Player::create(source);
                if (!player) return false;
                auto sampled = player.value().sample({});
                if (!sampled) return false;
                rest = std::move(sampled.value());
            }
            const animation::Matrix* rigid_bind = rigid_skinned
                ? &rest->bones[static_cast<std::size_t>(mesh.bone)].model_asset : nullptr;
            PackedVector3Array vertices;
            PackedVector3Array normals;
            PackedVector2Array uv;
            PackedColorArray colors;
            PackedFloat32Array tangents;
            PackedFloat32Array binormals;
            PackedInt32Array bones;
            PackedFloat32Array weights;
            PackedInt32Array indices;
            vertices.resize(static_cast<int64_t>(submesh.vertices.size()));
            normals.resize(static_cast<int64_t>(submesh.vertices.size()));
            uv.resize(static_cast<int64_t>(submesh.vertices.size()));
            colors.resize(static_cast<int64_t>(submesh.vertices.size()));
            tangents.resize(static_cast<int64_t>(submesh.vertices.size() * 4));
            if (authored_binormals) binormals.resize(static_cast<int64_t>(submesh.vertices.size() * 3));
            if (palette_skinned || rigid_skinned) {
                bones.resize(static_cast<int64_t>(submesh.vertices.size() * 4));
                weights.resize(static_cast<int64_t>(submesh.vertices.size() * 4));
                target.skinned = true;
            }
            for (std::size_t index = 0; index < submesh.vertices.size(); ++index) {
                const assets::Vertex vertex = rigid_bind
                    ? animation::Player::rigid_vertex_to_bind(*rigid_bind, submesh.vertices[index])
                    : submesh.vertices[index];
                const Vector3 normal = axis_convert(vertex.normal).normalized();
                const Vector3 tangent = axis_convert(vertex.tangent).normalized();
                const Vector3 binormal = axis_convert(vertex.binormal).normalized();
                vertices.set(static_cast<int64_t>(index), axis_convert(vertex.position));
                normals.set(static_cast<int64_t>(index), normal);
                uv.set(static_cast<int64_t>(index),
                    Vector2(vertex.texcoord[0].x, vertex.texcoord[0].y));
                // Authored vertex colour; only shaders that read COLOR see it.
                colors.set(static_cast<int64_t>(index),
                    Color(vertex.color.x, vertex.color.y, vertex.color.z, vertex.color.w));
                tangents.set(static_cast<int64_t>(index * 4), tangent.x);
                tangents.set(static_cast<int64_t>(index * 4 + 1), tangent.y);
                tangents.set(static_cast<int64_t>(index * 4 + 2), tangent.z);
                tangents.set(static_cast<int64_t>(index * 4 + 3),
                    normal.cross(tangent).dot(binormal) < 0.0 ? -1.0F : 1.0F);
                if (authored_binormals) {
                    const Vector3 authored = axis_convert(vertex.binormal);
                    const auto coefficients = legacy::binormal_coefficients(
                        {static_cast<float>(normal.x), static_cast<float>(normal.y), static_cast<float>(normal.z)},
                        {static_cast<float>(tangent.x), static_cast<float>(tangent.y), static_cast<float>(tangent.z)},
                        {static_cast<float>(authored.x), static_cast<float>(authored.y),
                            static_cast<float>(authored.z)});
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        binormals.set(static_cast<int64_t>(index * 3 + axis), coefficients[axis]);
                    }
                }
                if (rigid_skinned) {
                    bones.set(static_cast<int64_t>(index * 4), mesh.bone);
                    weights.set(static_cast<int64_t>(index * 4), 1.0F);
                    for (std::size_t influence = 1; influence < 4; ++influence) {
                        bones.set(static_cast<int64_t>(index * 4 + influence), 0);
                        weights.set(static_cast<int64_t>(index * 4 + influence), 0.0F);
                    }
                } else if (palette_skinned) {
                    for (std::size_t influence = 0; influence < 4; ++influence) {
                        const std::uint32_t local = vertex.bone_indices[influence];
                        const float weight = vertex.bone_weights[influence];
                        if (!std::isfinite(weight) || weight < 0.0F
                            || (weight != 0.0F && local >= submesh.skin_bones.size())) return false;
                        const std::uint32_t global = weight == 0.0F
                            ? 0U : submesh.skin_bones[local];
                        if (global >= source.bones.size()) return false;
                        bones.set(static_cast<int64_t>(index * 4 + influence),
                            static_cast<std::int32_t>(global));
                        weights.set(static_cast<int64_t>(index * 4 + influence), weight);
                    }
                }
            }
            indices.resize(static_cast<int64_t>(submesh.indices.size()));
            for (std::size_t index = 0; index < submesh.indices.size(); ++index) {
                indices.set(static_cast<int64_t>(index),
                    detail::uploaded_triangle_index(submesh.indices, index));
            }
            Array arrays;
            arrays.resize(RenderingServer::ARRAY_MAX);
            arrays[RenderingServer::ARRAY_VERTEX] = vertices;
            arrays[RenderingServer::ARRAY_NORMAL] = normals;
            arrays[RenderingServer::ARRAY_TANGENT] = tangents;
            arrays[RenderingServer::ARRAY_TEX_UV] = uv;
            arrays[RenderingServer::ARRAY_COLOR] = colors;
            if (authored_binormals) arrays[RenderingServer::ARRAY_CUSTOM0] = binormals;
            if (palette_skinned || rigid_skinned) {
                arrays[RenderingServer::ARRAY_BONES] = bones;
                arrays[RenderingServer::ARRAY_WEIGHTS] = weights;
            }
            arrays[RenderingServer::ARRAY_INDEX] = indices;
            rendering.mesh_add_surface_from_arrays(
                target.mesh, RenderingServer::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(),
                authored_binormals ? static_cast<BitField<RenderingServer::ArrayFormat>>(
                    static_cast<int64_t>(RenderingServer::ARRAY_CUSTOM_RGB_FLOAT)
                    << RenderingServer::ARRAY_FORMAT_CUSTOM0_SHIFT) : BitField<RenderingServer::ArrayFormat>(0));
            rendering.mesh_surface_set_material(
                target.mesh, rendering.mesh_get_surface_count(target.mesh) - 1,
                target.material);
        }
    }
    return target.mesh.is_valid() && rendering.mesh_get_surface_count(target.mesh) > 0;
}

void GodotRenderer::Impl::free_resource(RenderingServer& rendering, const Resource& resource) {
    if (resource.mesh.is_valid()) rendering.free_rid(resource.mesh);
    if (resource.material.is_valid()) rendering.free_rid(resource.material);
    if (resource.shader.is_valid()) rendering.free_rid(resource.shader);
    if (resource.texture.is_valid()) rendering.free_rid(resource.texture);
    for (const auto& item : resource.binding_textures) {
        if (item.second.is_valid()) rendering.free_rid(item.second);
    }
}

} // namespace eawr::presentation::godot_backend
