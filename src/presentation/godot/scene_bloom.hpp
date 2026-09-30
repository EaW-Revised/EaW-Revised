#pragma once

// Scene bloom (#201) on a RenderingDevice backend (docs/rendering.md#bloom).
//
// Retail SceneBloom.fx runs after the transparent phase on the gamma
// backbuffer: a bright pass samples a copy of it into a quarter-size 8-bit
// target (a pixel whose luminance exceeds the cutoff stays, any other becomes
// pixel^5), four 4-tap diagonal blurs ping-pong between two such targets with
// offsets growing by one texel, and the result, times the strength, is added
// back with an add-smooth blend (src + dst x (1 - src)). This compositor
// effect follows the stored-value decode (stored_output.hpp), so it reads the
// decoded colour buffer, encodes it back to the stored value the retail
// backbuffer holds (clamped and rounded to 8 bits), does the retail arithmetic
// there and decodes the combined pixel again. With MSAA it therefore sees the
// decode's per-sample-saturated resolve.

#include "eawr/presentation/lighting/scene_bloom.hpp"

#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/uniform_set_cache_rd.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace eawr::presentation::godot_backend::scene_bloom {

namespace detail {

// Shared by the three programs: the stored-value transfer the decode pass
// inverts, and a bilinear tap with clamped addressing at a texel-space
// position (texel centres on integers), as D3D9's LINEAR/CLAMP sampler reads.
constexpr std::string_view common_glsl = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
vec3 encode(vec3 linear) {
    linear = max(linear, vec3(0.0));
    return mix(1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055, linear * 12.92,
               lessThanEqual(linear, vec3(0.0031308)));
}
vec3 decode(vec3 stored) {
    return mix(pow((stored + 0.055) / 1.055, vec3(2.4)), stored / 12.92, lessThanEqual(stored, vec3(0.04045)));
}
#define BILINEAR(name, fetch) \
    vec3 name(vec2 position) { \
        vec2 base = floor(position); \
        vec2 weight = position - base; \
        ivec2 texel = ivec2(base); \
        vec3 top = mix(fetch(texel), fetch(texel + ivec2(1, 0)), weight.x); \
        vec3 bottom = mix(fetch(texel + ivec2(0, 1)), fetch(texel + ivec2(1, 1)), weight.x); \
        return mix(top, bottom, weight.y); \
    }
)GLSL";

// Backbuffer copy -> bloom target: each target texel's centre sampled from
// the full-size frame, then the cutoff or the fifth power.
constexpr std::string_view bright_glsl = R"GLSL(
layout(rgba16f, set = 0, binding = 0) uniform restrict readonly image2D scene_image;
layout(rgba8, set = 0, binding = 1) uniform restrict writeonly image2D bright_image;
layout(push_constant, std430) uniform Params { ivec2 scene_size; ivec2 bloom_size; float cutoff; float pad[3]; } params;
vec3 backbuffer(ivec2 texel) {
    vec3 linear = imageLoad(scene_image, clamp(texel, ivec2(0), params.scene_size - 1)).rgb;
    return round(clamp(encode(linear), 0.0, 1.0) * 255.0) / 255.0;
}
BILINEAR(sample_backbuffer, backbuffer)
void main() {
    ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    if (texel.x >= params.bloom_size.x || texel.y >= params.bloom_size.y) return;
    vec2 uv = (vec2(texel) + 0.5) / vec2(params.bloom_size);
    vec3 pixel = sample_backbuffer(uv * vec2(params.scene_size) - 0.5);
    float luminance = dot(pixel, vec3(0.299, 0.587, 0.114));
    vec3 bright = luminance > params.cutoff ? pixel : pixel * pixel * pixel * pixel * pixel;
    imageStore(bright_image, texel, vec4(bright, 1.0));
}
)GLSL";

// One blur iteration: four taps `offset` texels away on the diagonals.
constexpr std::string_view blur_glsl = R"GLSL(
layout(rgba8, set = 0, binding = 0) uniform restrict readonly image2D source_image;
layout(rgba8, set = 0, binding = 1) uniform restrict writeonly image2D target_image;
layout(push_constant, std430) uniform Params { ivec2 size; float offset; float pad; } params;
vec3 source(ivec2 texel) {
    return imageLoad(source_image, clamp(texel, ivec2(0), params.size - 1)).rgb;
}
BILINEAR(sample_source, source)
void main() {
    ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    if (texel.x >= params.size.x || texel.y >= params.size.y) return;
    vec2 centre = vec2(texel);
    float offset = params.offset;
    vec3 sum = sample_source(centre + vec2(offset, offset)) + sample_source(centre - vec2(offset, offset))
        + sample_source(centre + vec2(offset, -offset)) + sample_source(centre - vec2(offset, -offset));
    imageStore(target_image, texel, vec4(sum * 0.25, 1.0));
}
)GLSL";

// Bloom target -> frame: strength x the upsampled bloom, add-smooth blended
// onto the stored value.
constexpr std::string_view combine_glsl = R"GLSL(
layout(rgba16f, set = 0, binding = 0) uniform restrict image2D color_image;
layout(rgba8, set = 0, binding = 1) uniform restrict readonly image2D bloom_image;
layout(push_constant, std430) uniform Params { ivec2 scene_size; ivec2 bloom_size; float strength; float pad[3]; } params;
vec3 bloom(ivec2 texel) {
    return imageLoad(bloom_image, clamp(texel, ivec2(0), params.bloom_size - 1)).rgb;
}
BILINEAR(sample_bloom, bloom)
void main() {
    ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    if (texel.x >= params.scene_size.x || texel.y >= params.scene_size.y) return;
    vec2 uv = (vec2(texel) + 0.5) / vec2(params.scene_size);
    vec3 source = clamp(params.strength * sample_bloom(uv * vec2(params.bloom_size) - 0.5), 0.0, 1.0);
    vec4 color = imageLoad(color_image, texel);
    // The retail destination is the A8R8G8B8 backbuffer: blend against byte codes.
    vec3 stored = round(clamp(encode(color.rgb), 0.0, 1.0) * 255.0) / 255.0;
    imageStore(color_image, texel, vec4(decode(source + stored * (1.0 - source)), color.a));
}
)GLSL";

enum Program : std::size_t { bright, blur, combine, program_count };

// Render-thread state: the three pipelines, built on first use.
struct Pipelines final {
    std::array<godot::RID, program_count> shaders;
    std::array<godot::RID, program_count> pipelines;
    bool failed{};
};

inline Pipelines& pipelines() {
    static Pipelines value;
    return value;
}

inline void disable(Pipelines& current, const char* reason) {
    current.failed = true;
    godot::UtilityFunctions::push_error("eawr scene bloom disabled: ", reason);
}

[[nodiscard]] inline bool build(godot::RenderingDevice& device, Pipelines& current) {
    using namespace godot;
    const std::array<std::string_view, program_count> bodies{bright_glsl, blur_glsl, combine_glsl};
    for (std::size_t program = 0; program < program_count; ++program) {
        const String text = String::utf8(common_glsl.data(), static_cast<int64_t>(common_glsl.size()))
            + String::utf8(bodies[program].data(), static_cast<int64_t>(bodies[program].size()));
        Ref<RDShaderSource> source;
        source.instantiate();
        source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
        source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, text);
        const Ref<RDShaderSPIRV> spirv = device.shader_compile_spirv_from_source(source);
        if (spirv.is_null() || !spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE).is_empty()) {
            disable(current, "a bloom shader did not compile");
            return false;
        }
        current.shaders[program] = device.shader_create_from_spirv(spirv);
        if (current.shaders[program].is_valid()) {
            current.pipelines[program] = device.compute_pipeline_create(current.shaders[program]);
        }
        if (!current.pipelines[program].is_valid()) {
            disable(current, "a bloom pipeline was not created");
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline godot::Ref<godot::RDUniform> image_uniform(const int32_t binding, const godot::RID& texture) {
    godot::Ref<godot::RDUniform> uniform;
    uniform.instantiate();
    uniform->set_uniform_type(godot::RenderingDevice::UNIFORM_TYPE_IMAGE);
    uniform->set_binding(binding);
    uniform->add_id(texture);
    return uniform;
}

template <typename Push>
inline void dispatch(godot::RenderingDevice& device, const Program program, const godot::RID& input,
                     const godot::RID& output, const Push& push, const godot::Vector2i& size) {
    using namespace godot;
    const Pipelines& current = pipelines();
    TypedArray<RDUniform> uniforms;
    uniforms.push_back(image_uniform(0, input));
    uniforms.push_back(image_uniform(1, output));
    const RID set = UniformSetCacheRD::get_cache(current.shaders[program], 0, uniforms);
    PackedByteArray constants;
    constants.resize(sizeof(push));
    std::memcpy(constants.ptrw(), &push, sizeof(push));
    const int64_t list = device.compute_list_begin();
    device.compute_list_bind_compute_pipeline(list, current.pipelines[program]);
    device.compute_list_bind_uniform_set(list, set, 0);
    device.compute_list_set_push_constant(list, constants, static_cast<uint32_t>(constants.size()));
    device.compute_list_dispatch(list, static_cast<uint32_t>((size.x + 7) / 8), static_cast<uint32_t>((size.y + 7) / 8), 1);
    device.compute_list_end();
}

struct ScenePush final {
    std::array<int32_t, 2> scene_size;
    std::array<int32_t, 2> bloom_size;
    float value;
    std::array<float, 3> pad;
};
struct BlurPush final {
    std::array<int32_t, 2> size;
    float offset;
    float pad;
};

// The two bloom targets of these render buffers, (re)created at `size`.
[[nodiscard]] inline std::array<godot::RID, 2> targets(godot::RenderSceneBuffersRD& buffers,
                                                       godot::RenderingDevice& device, const godot::Vector2i& size) {
    using namespace godot;
    const StringName context("eawr_scene_bloom");
    const std::array<StringName, 2> names{StringName("target_a"), StringName("target_b")};
    if (buffers.has_texture(context, names[0])) {
        const Ref<RDTextureFormat> format = device.texture_get_format(buffers.get_texture(context, names[0]));
        if (format.is_valid() && static_cast<int32_t>(format->get_width()) == size.x
            && static_cast<int32_t>(format->get_height()) == size.y) {
            return {buffers.get_texture(context, names[0]), buffers.get_texture(context, names[1])};
        }
        buffers.clear_context(context);
    }
    std::array<RID, 2> result;
    for (std::size_t index = 0; index < names.size(); ++index) {
        result[index] = buffers.create_texture(context, names[index], RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
            RenderingDevice::TEXTURE_USAGE_STORAGE_BIT, RenderingDevice::TEXTURE_SAMPLES_1, size, 1, 1, true, false);
    }
    return result;
}

// The compositor callback, on the render thread after the stored-value decode;
// the scene's parameters are bound to the callable.
inline void render(const int32_t, godot::RenderData* data, const float strength, const float cutoff,
                   const float size) {
    using namespace godot;
    namespace bloom = lighting::bloom;
    RenderingDevice* device = RenderingServer::get_singleton()->get_rendering_device();
    Pipelines& current = pipelines();
    if (!device || !data || current.failed) return;
    if (!current.pipelines[combine].is_valid() && !build(*device, current)) return;
    const Ref<RenderSceneBuffers> generic = data->get_render_scene_buffers();
    auto* buffers = Object::cast_to<RenderSceneBuffersRD>(generic.ptr());
    if (!buffers) return;
    const Vector2i scene = buffers->get_internal_size();
    const Vector2i extent(bloom::target_extent(scene.x), bloom::target_extent(scene.y));
    if (extent.x <= 0 || extent.y <= 0) return;
    const std::array<RID, 2> ping_pong = targets(*buffers, *device, extent);
    if (!ping_pong[0].is_valid() || !ping_pong[1].is_valid()) {
        disable(current, "the bloom targets were not created");
        return;
    }
    const std::array<int32_t, 2> scene_size{scene.x, scene.y};
    const std::array<int32_t, 2> bloom_size{extent.x, extent.y};
    for (uint32_t view = 0; view < buffers->get_view_count(); ++view) {
        const RID color = buffers->get_color_layer(view, false);
        const Ref<RDTextureFormat> format = device->texture_get_format(color);
        if (format.is_null() || format->get_format() != RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT
            || (format->get_usage_bits() & RenderingDevice::TEXTURE_USAGE_STORAGE_BIT) == 0) {
            disable(current, "the colour buffer is not a storable RGBA16F texture");
            return;
        }
        dispatch(*device, bright, color, ping_pong[0], ScenePush{scene_size, bloom_size, cutoff, {}}, extent);
        std::size_t source = 0;
        for (uint32_t iteration = 0; iteration < bloom::blur_iterations; ++iteration) {
            dispatch(*device, blur, ping_pong[source], ping_pong[1 - source],
                     BlurPush{bloom_size, bloom::blur_offset(size, iteration), 0.0F}, extent);
            source = 1 - source;
        }
        dispatch(*device, combine, color, ping_pong[source], ScenePush{scene_size, bloom_size, strength, {}}, scene);
    }
}

// Frees the pipelines on the render thread; a later frame rebuilds them.
inline void free_pipelines() {
    using namespace godot;
    Pipelines& current = pipelines();
    RenderingServer* rendering = RenderingServer::get_singleton();
    RenderingDevice* device = rendering ? rendering->get_rendering_device() : nullptr;
    // Freeing a shader also frees its pipeline and cached uniform sets.
    if (device) {
        for (const RID& shader : current.shaders) {
            if (shader.is_valid()) device->free_rid(shader);
        }
    }
    current = {};
}

} // namespace detail

// A disabled bloom effect for a stored-value compositor; configure() turns it
// on for a scene.
[[nodiscard]] inline godot::RID create_effect(godot::RenderingServer& rendering) {
    using namespace godot;
    const RID effect = rendering.compositor_effect_create();
    rendering.compositor_effect_set_flag(effect, RenderingServer::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_COLOR, true);
    rendering.compositor_effect_set_enabled(effect, false);
    return effect;
}

// The scene's bloom parameters, or none to turn the effect off.
inline void configure(godot::RenderingServer& rendering, const godot::RID& effect,
                      const std::optional<lighting::bloom::SceneBloom>& bloom) {
    using namespace godot;
    if (!effect.is_valid()) return;
    if (bloom) {
        rendering.compositor_effect_set_callback(effect, RenderingServer::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_TRANSPARENT,
            callable_mp_static(&detail::render).bind(bloom->strength, bloom->cutoff, bloom->size));
    }
    rendering.compositor_effect_set_enabled(effect, bloom.has_value());
}

} // namespace eawr::presentation::godot_backend::scene_bloom
