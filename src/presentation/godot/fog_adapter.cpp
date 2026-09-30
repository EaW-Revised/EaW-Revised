#include "fog_adapter.hpp"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <cstring>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

constexpr std::string_view nearest_program = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled;
uniform sampler2D eawr_fog_texture : filter_nearest, repeat_disable;
uniform vec2 eawr_fog_origin = vec2(0.0);
uniform vec2 eawr_fog_extent = vec2(1.0);
uniform vec2 eawr_fog_size = vec2(1.0);
uniform bool eawr_fog_bound = false;
uniform vec3 eawr_surface_color = vec3(1.0);
varying vec3 eawr_world_position;
void vertex() {
    eawr_world_position = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}
void fragment() {
    // Source XY is Godot world (X, -Z). The half-open [0, 1) bounds test runs
    // before the texel index is clamped, so outside samples dark.
    vec2 source = vec2(eawr_world_position.x, -eawr_world_position.z);
    vec2 uv = (source - eawr_fog_origin) / eawr_fog_extent;
    float attenuation = 0.0;
    if (eawr_fog_bound && uv.x >= 0.0 && uv.y >= 0.0 && uv.x < 1.0 && uv.y < 1.0) {
        vec2 texel = min(floor(uv * eawr_fog_size), eawr_fog_size - vec2(1.0));
        attenuation = texture(eawr_fog_texture, (texel + vec2(0.5)) / eawr_fog_size).r;
    }
    ALBEDO = eawr_surface_color * attenuation;
}
)GODOT";

[[nodiscard]] StringName name(const std::string_view value) {
    return StringName(String::utf8(value.data(), static_cast<int>(value.size())));
}

[[nodiscard]] Ref<Image> r8_image(const fog::TextureSpec& spec, const std::span<const std::uint8_t> pixels) {
    PackedByteArray data;
    data.resize(static_cast<std::int64_t>(pixels.size()));
    std::memcpy(data.ptrw(), pixels.data(), pixels.size());
    return Image::create_from_data(
        static_cast<std::int32_t>(spec.width), static_cast<std::int32_t>(spec.height), false,
        Image::FORMAT_R8, data);
}

} // namespace

GodotFogBackend::GodotFogBackend(const std::uint32_t max_dimension) noexcept
    : max_dimension_(max_dimension) {}

GodotFogBackend::~GodotFogBackend() {
    RenderingServer* server = RenderingServer::get_singleton();
    for (const auto& [handle, texture] : textures_) {
        if (server != nullptr) server->free_rid(texture.rid);
    }
}

fog::TextureHandle GodotFogBackend::create_texture(
    const fog::TextureSpec& spec,
    const std::span<const std::uint8_t> pixels) {
    if (spec.width == 0 || spec.height == 0 || spec.width > max_dimension_ || spec.height > max_dimension_) {
        failure_ = "fog texture " + std::to_string(spec.width) + "x" + std::to_string(spec.height)
            + " exceeds the device limit " + std::to_string(max_dimension_);
        return {};
    }
    if (pixels.size() != std::size_t{spec.width} * spec.height) {
        failure_ = "fog texture byte count does not match its dimensions";
        return {};
    }
    RenderingServer* server = RenderingServer::get_singleton();
    const Ref<Image> image = r8_image(spec, pixels);
    if (server == nullptr || image.is_null() || image->is_empty()) {
        failure_ = "Godot could not build the R8 fog image";
        return {};
    }
    const RID rid = server->texture_2d_create(image);
    if (!rid.is_valid()) {
        failure_ = "RenderingServer rejected the R8 fog texture";
        return {};
    }
    const fog::TextureHandle handle = next_texture_++;
    textures_.emplace(handle, Texture{rid, spec});
    return handle;
}

bool GodotFogBackend::update_texture(
    const fog::TextureHandle texture,
    const std::span<const std::uint8_t> pixels) {
    const auto found = textures_.find(texture);
    if (found == textures_.end()) {
        failure_ = "unknown fog texture handle " + std::to_string(texture);
        return false;
    }
    const fog::TextureSpec& spec = found->second.spec;
    if (pixels.size() != std::size_t{spec.width} * spec.height) {
        failure_ = "fog texture update changes the byte count";
        return false;
    }
    const Ref<Image> image = r8_image(spec, pixels);
    if (image.is_null() || image->is_empty()) {
        failure_ = "Godot could not build the R8 fog update image";
        return false;
    }
    RenderingServer* server = RenderingServer::get_singleton();
    if (server == nullptr) {
        failure_ = "Godot RenderingServer is unavailable";
        return false;
    }
    // Same size and format as created, so the update replaces every texel.
    server->texture_2d_update(found->second.rid, image, 0);
    return true;
}

void GodotFogBackend::destroy_texture(const fog::TextureHandle texture) {
    const auto found = textures_.find(texture);
    if (found == textures_.end()) return;
    if (RenderingServer* server = RenderingServer::get_singleton()) server->free_rid(found->second.rid);
    textures_.erase(found);
}

void GodotFogBackend::bind(
    const fog::ConsumerId consumer,
    const fog::TextureHandle texture,
    const fog::FogMapping& mapping) {
    const auto material = materials_.find(consumer);
    const auto found = textures_.find(texture);
    RenderingServer* server = RenderingServer::get_singleton();
    if (material == materials_.end() || found == textures_.end() || server == nullptr) return;
    const RID& rid = material->second.rid;
    server->material_set_param(rid, name(texture_parameter), found->second.rid);
    server->material_set_param(rid, name(origin_parameter),
        Vector2(static_cast<real_t>(mapping.origin_x), static_cast<real_t>(mapping.origin_y)));
    server->material_set_param(rid, name(extent_parameter),
        Vector2(static_cast<real_t>(mapping.extent_x), static_cast<real_t>(mapping.extent_y)));
    server->material_set_param(rid, name(size_parameter),
        Vector2(static_cast<real_t>(mapping.width), static_cast<real_t>(mapping.height)));
    server->material_set_param(rid, name(bound_parameter), true);
}

void GodotFogBackend::unbind(const fog::ConsumerId consumer) {
    const auto material = materials_.find(consumer);
    if (material == materials_.end()) return;
    force_unbound(material->second.rid);
}

void GodotFogBackend::force_unbound(const RID& material) {
    RenderingServer* server = RenderingServer::get_singleton();
    if (server == nullptr) return;
    // Explicit overrides, not shader defaults: a consumer whose shader
    // defaults eawr_fog_bound to true, or whose material carried its own fog
    // overrides, is still dark and unbound. The texture override is erased
    // (nil) so no material refers to a fog texture the cache may destroy.
    server->material_set_param(material, name(bound_parameter), false);
    server->material_set_param(material, name(texture_parameter), Variant());
    server->material_set_param(material, name(origin_parameter), Vector2(0.0F, 0.0F));
    server->material_set_param(material, name(extent_parameter), Vector2(1.0F, 1.0F));
    server->material_set_param(material, name(size_parameter), Vector2(1.0F, 1.0F));
}

std::string GodotFogBackend::failure_cause() const {
    return failure_;
}

fog::ConsumerId GodotFogBackend::register_material(const RID& material) {
    const fog::ConsumerId consumer = next_consumer_++;
    Material entry{material, {}};
    RenderingServer* server = RenderingServer::get_singleton();
    if (server != nullptr) {
        // material_get_param returns the material's override, or nil when the
        // shader default applies; both are restored exactly on forget.
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            entry.captured[index] = server->material_get_param(material, name(parameters[index]));
        }
    }
    materials_.emplace(consumer, std::move(entry));
    // Unbound until the cache binds it, whatever the shader defaults say.
    force_unbound(material);
    return consumer;
}

void GodotFogBackend::forget_material(const fog::ConsumerId consumer) {
    const auto material = materials_.find(consumer);
    if (material == materials_.end()) return;
    if (RenderingServer* server = RenderingServer::get_singleton()) {
        // A nil capture erases the fog override, so the shader default
        // applies again exactly as before registration.
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            server->material_set_param(material->second.rid, name(parameters[index]),
                material->second.captured[index]);
        }
    }
    materials_.erase(material);
}

std::optional<GodotFogBackend::Readback> GodotFogBackend::read_back(const fog::TextureHandle texture) const {
    const auto found = textures_.find(texture);
    if (found == textures_.end()) return std::nullopt;
    const Ref<Image> image = RenderingServer::get_singleton()->texture_2d_get(found->second.rid);
    if (image.is_null()) return std::nullopt;
    Readback result;
    result.width = static_cast<std::uint32_t>(image->get_width());
    result.height = static_cast<std::uint32_t>(image->get_height());
    result.r8 = image->get_format() == Image::FORMAT_R8;
    result.mipmaps = image->has_mipmaps();
    const PackedByteArray data = image->get_data();
    result.bytes.assign(data.ptr(), data.ptr() + data.size());
    return result;
}

RID GodotFogBackend::texture_rid(const fog::TextureHandle texture) const {
    const auto found = textures_.find(texture);
    return found == textures_.end() ? RID() : found->second.rid;
}

std::string_view GodotFogBackend::synthetic_nearest_shader() noexcept {
    return nearest_program;
}

} // namespace eawr::presentation::godot_backend
