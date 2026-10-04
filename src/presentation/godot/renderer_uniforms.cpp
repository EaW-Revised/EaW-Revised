#include "eawr/core/load_profile.hpp"
#include "renderer_upload_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace {
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

} // namespace

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

} // namespace eawr::presentation::godot_backend
