#pragma once

// Stored-value output on a RenderingDevice backend (docs/rendering.md, colour
// policy).
//
// The retail pipeline (Direct3D 9) samples texels without sRGB decode, does its
// arithmetic on the stored 8-bit values and blends them in the backbuffer. The
// shaders keep those semantics on every backend: no source_color hints, and
// ALBEDO is the stored value. Forward+ blends that value in its half-float
// colour buffer and encodes to sRGB after the tonemapper, so this compositor
// pass, after the transparent pass, clamps each pixel to [0, 1] (the 8-bit
// backbuffer's saturation) and decodes it once; the linear tonemapper at
// exposure 1 / white 1 then writes the stored value back. The Compatibility
// fallback writes ALBEDO to an sRGB output directly and gets no compositor;
// only its compensated ALBEDO writer differs (compatibility_source).
//
// With MSAA (the enhanced render profile, #184) each sample stands for one
// 8-bit backbuffer sample, which saturates before the resolve averages it.
// Godot's resolve averages the unclamped half-float samples, which brightens
// the edges of overbright additive draws, so the pass re-resolves from the
// multisample buffer: clamp each sample, average, then decode. Screen-space
// AA (SMAA) runs after the tonemapper on stored values and needs nothing.

#include "scene_bloom.hpp"
#include "shader_adapter.hpp"

#include <godot_cpp/classes/rd_sampler_state.hpp>
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
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace eawr::presentation::godot_backend::stored_output {

// Whether the running backend is a RenderingDevice one (Forward+).
[[nodiscard]] inline bool active() {
    godot::RenderingServer* rendering = godot::RenderingServer::get_singleton();
    return rendering && rendering->get_rendering_device() != nullptr;
}

// Shader text as the running backend compiles it: stored-value sources as
// written, or their Compatibility-fallback form.
[[nodiscard]] inline std::string backend_source(const std::string_view source) {
    std::string text(source);
    return active() ? text : compatibility_source(std::move(text));
}

namespace detail {

constexpr std::string_view decode_glsl = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(rgba16f, set = 0, binding = 0) uniform image2D color_image;
layout(push_constant, std430) uniform Params { ivec2 size; ivec2 pad; } params;
void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (pixel.x >= params.size.x || pixel.y >= params.size.y) return;
    vec4 color = imageLoad(color_image, pixel);
    vec3 stored = clamp(color.rgb, 0.0, 1.0);
    vec3 linear = mix(pow((stored + 0.055) / 1.055, vec3(2.4)), stored / 12.92,
                      lessThanEqual(stored, vec3(0.04045)));
    imageStore(color_image, pixel, vec4(linear, color.a));
}
)GLSL";

// The multisample form: each of `samples` samples saturates like its 8-bit
// backbuffer sample, then the pixel is their average.
constexpr std::string_view multisample_decode_glsl = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(rgba16f, set = 0, binding = 0) uniform image2D color_image;
layout(set = 0, binding = 1) uniform sampler2DMS color_samples;
layout(push_constant, std430) uniform Params { ivec2 size; int samples; int pad; } params;
void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (pixel.x >= params.size.x || pixel.y >= params.size.y) return;
    vec4 sum = vec4(0.0);
    for (int index = 0; index < params.samples; ++index) {
        vec4 sample_color = texelFetch(color_samples, pixel, index);
        sum += vec4(clamp(sample_color.rgb, 0.0, 1.0), sample_color.a);
    }
    vec4 color = sum / float(params.samples);
    vec3 stored = color.rgb;
    vec3 linear = mix(pow((stored + 0.055) / 1.055, vec3(2.4)), stored / 12.92,
                      lessThanEqual(stored, vec3(0.04045)));
    imageStore(color_image, pixel, vec4(linear, color.a));
}
)GLSL";

// Render-thread state: the decode pipelines, built on first use.
struct Pipeline final {
    godot::RID shader;
    godot::RID pipeline;
    bool failed{};
    // The multisample decode and the sampler its texel fetches need.
    godot::RID multisample_shader;
    godot::RID multisample_pipeline;
    godot::RID sampler;
    bool multisample_failed{};
};

inline Pipeline& pipeline() {
    static Pipeline value;
    return value;
}

inline void disable(Pipeline& current, const char* reason) {
    current.failed = true;
    godot::UtilityFunctions::push_error("eawr stored-value output disabled: ", reason);
}

// Without the multisample decode, MSAA frames are decoded from Godot's resolve.
inline void disable_multisample(Pipeline& current, const char* reason) {
    current.multisample_failed = true;
    godot::UtilityFunctions::push_error(
        "eawr stored-value output decodes the resolved MSAA colour instead of its samples: ", reason);
}

// Compiles one decode program. The reason it failed, or nullptr.
[[nodiscard]] inline const char* compile(godot::RenderingDevice& device, const std::string_view glsl,
                                         godot::RID& shader, godot::RID& compute_pipeline) {
    using namespace godot;
    Ref<RDShaderSource> source;
    source.instantiate();
    source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
    source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE,
        String::utf8(glsl.data(), static_cast<int64_t>(glsl.size())));
    const Ref<RDShaderSPIRV> spirv = device.shader_compile_spirv_from_source(source);
    if (spirv.is_null() || !spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE).is_empty()) {
        return "the decode shader did not compile";
    }
    shader = device.shader_create_from_spirv(spirv);
    if (shader.is_valid()) compute_pipeline = device.compute_pipeline_create(shader);
    return compute_pipeline.is_valid() ? nullptr : "the decode pipeline was not created";
}

[[nodiscard]] inline bool build(godot::RenderingDevice& device, Pipeline& current) {
    if (const char* problem = compile(device, decode_glsl, current.shader, current.pipeline)) {
        disable(current, problem);
        return false;
    }
    return true;
}

[[nodiscard]] inline bool build_multisample(godot::RenderingDevice& device, Pipeline& current) {
    using namespace godot;
    if (const char* problem = compile(
            device, multisample_decode_glsl, current.multisample_shader, current.multisample_pipeline)) {
        disable_multisample(current, problem);
        return false;
    }
    // texelFetch ignores filtering; the default state is nearest.
    Ref<RDSamplerState> state;
    state.instantiate();
    current.sampler = device.sampler_create(state);
    if (!current.sampler.is_valid()) {
        disable_multisample(current, "the sampler was not created");
        return false;
    }
    return true;
}

// The compositor callback, on the render thread after the transparent pass.
// Forward+ has already resolved MSAA into the internal colour texture (the
// effect also asks for it), which is the one the tonemapper reads. Without
// MSAA the pass decodes that texture in place; with MSAA it overwrites it with
// the clamped samples' average, decoded.
inline void render(const int32_t, godot::RenderData* data) {
    using namespace godot;
    RenderingDevice* device = RenderingServer::get_singleton()->get_rendering_device();
    Pipeline& current = pipeline();
    if (!device || !data || current.failed) return;
    if (!current.pipeline.is_valid() && !build(*device, current)) return;
    const Ref<RenderSceneBuffers> generic = data->get_render_scene_buffers();
    auto* buffers = Object::cast_to<RenderSceneBuffersRD>(generic.ptr());
    if (!buffers) return;
    const Vector2i size = buffers->get_internal_size();
    if (size.x <= 0 || size.y <= 0) return;
    const bool multisampled = buffers->get_msaa_3d() != RenderingServer::VIEWPORT_MSAA_DISABLED
        && !current.multisample_failed
        && (current.multisample_pipeline.is_valid() || build_multisample(*device, current));
    const int32_t samples = multisampled ? 1 << static_cast<int32_t>(buffers->get_texture_samples()) : 0;
    const std::array<int32_t, 4> push{size.x, size.y, samples, 0};
    PackedByteArray constants;
    constants.resize(sizeof(push));
    std::memcpy(constants.ptrw(), push.data(), sizeof(push));
    for (uint32_t view = 0; view < buffers->get_view_count(); ++view) {
        const RID color = buffers->get_color_layer(view, false);
        const Ref<RDTextureFormat> format = device->texture_get_format(color);
        if (format.is_null() || format->get_format() != RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT
            || (format->get_usage_bits() & RenderingDevice::TEXTURE_USAGE_STORAGE_BIT) == 0) {
            disable(current, "the colour buffer is not a storable RGBA16F texture");
            return;
        }
        Ref<RDUniform> uniform;
        uniform.instantiate();
        uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
        uniform->set_binding(0);
        uniform->add_id(color);
        TypedArray<RDUniform> uniforms;
        uniforms.push_back(uniform);
        if (multisampled) {
            Ref<RDUniform> sampled;
            sampled.instantiate();
            sampled->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
            sampled->set_binding(1);
            sampled->add_id(current.sampler);
            sampled->add_id(buffers->get_color_layer(view, true));
            uniforms.push_back(sampled);
        }
        const RID shader = multisampled ? current.multisample_shader : current.shader;
        const RID set = UniformSetCacheRD::get_cache(shader, 0, uniforms);
        const int64_t list = device->compute_list_begin();
        device->compute_list_bind_compute_pipeline(list, multisampled ? current.multisample_pipeline : current.pipeline);
        device->compute_list_bind_uniform_set(list, set, 0);
        device->compute_list_set_push_constant(list, constants, static_cast<uint32_t>(constants.size()));
        device->compute_list_dispatch(list, static_cast<uint32_t>((size.x + 7) / 8),
                                      static_cast<uint32_t>((size.y + 7) / 8), 1);
        device->compute_list_end();
    }
}

// Frees the pipelines on the render thread; a later frame rebuilds them.
inline void free_pipeline() {
    using namespace godot;
    Pipeline& current = pipeline();
    RenderingServer* rendering = RenderingServer::get_singleton();
    RenderingDevice* device = rendering ? rendering->get_rendering_device() : nullptr;
    // Freeing a shader also frees its pipeline and cached uniform sets.
    if (device) {
        for (const RID& owned : {current.shader, current.multisample_shader, current.sampler}) {
            if (owned.is_valid()) device->free_rid(owned);
        }
    }
    current = {};
}

// Live compositors, main thread only: the last release frees the pipeline.
inline int& users() {
    static int value{};
    return value;
}

} // namespace detail

struct Compositor final {
    godot::RID compositor;
    godot::RID effect;
    // Scene bloom (scene_bloom.hpp) after the decode, off until configured.
    godot::RID bloom;
};

// The compositor carrying the decode and the scene bloom, or empty RIDs when
// the backend is not a RenderingDevice one.
[[nodiscard]] inline Compositor create(godot::RenderingServer& rendering) {
    using namespace godot;
    if (!active()) return {};
    Compositor result;
    result.effect = rendering.compositor_effect_create();
    rendering.compositor_effect_set_callback(result.effect,
        RenderingServer::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_TRANSPARENT, callable_mp_static(&detail::render));
    rendering.compositor_effect_set_flag(result.effect, RenderingServer::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_COLOR, true);
    rendering.compositor_effect_set_enabled(result.effect, true);
    result.bloom = scene_bloom::create_effect(rendering);
    result.compositor = rendering.compositor_create();
    TypedArray<RID> effects;
    effects.push_back(result.effect);
    effects.push_back(result.bloom);
    rendering.compositor_set_compositor_effects(result.compositor, effects);
    ++detail::users();
    return result;
}

inline void release(godot::RenderingServer& rendering, Compositor& value) {
    if (!value.effect.is_valid()) return;
    if (value.compositor.is_valid()) rendering.free_rid(value.compositor);
    rendering.free_rid(value.effect);
    if (value.bloom.is_valid()) rendering.free_rid(value.bloom);
    value = {};
    if (--detail::users() == 0) {
        rendering.call_on_render_thread(callable_mp_static(&detail::free_pipeline));
        rendering.call_on_render_thread(callable_mp_static(&scene_bloom::detail::free_pipelines));
    }
}

} // namespace eawr::presentation::godot_backend::stored_output
