#pragma once

#include "eawr/presentation/fog/fog.hpp"

#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::godot_backend {

// RenderingServer backend for the engine-free fog::TextureCache (fog-stub-v1,
// synthetic Phase 1 policy). Each texture is one Image::FORMAT_R8 2D texture:
// linear scalar bytes, no mipmaps; the sampler hints in the consuming shader
// disable wrapping. Consumers are Godot material RIDs owned by the caller;
// binding sets the fog uniforms below on that material only.
//
// Registration captures the material's own values of the five fog uniforms
// (its overrides, or nil where the shader default applies) and forces the
// explicit unbound state at once. unbind forces that state again: bound false,
// no fog texture and a neutral (0, 0) / (1, 1) / (1, 1) mapping, set as
// overrides, so neither a shader default nor a pre-existing override can keep
// a consumer logically bound. forget_material is the final detach: it puts
// the captured values back exactly, so a consumer leaves with the parameters
// it had before fog. The caller must remove a consumer from the cache and
// forget it here before freeing its RID.
class GodotFogBackend final : public fog::TextureBackend {
public:
    // Largest texture edge this backend accepts. The default is the
    // fog-stub-v1 grid limit; the caller supplies a lower device limit when it
    // knows one (no device query is made here), which also exercises the
    // fail-closed creation path.
    static constexpr std::uint32_t default_max_dimension = sim::fog::max_grid_dimension;

    explicit GodotFogBackend(std::uint32_t max_dimension = default_max_dimension) noexcept;
    ~GodotFogBackend() override;
    GodotFogBackend(const GodotFogBackend&) = delete;
    GodotFogBackend& operator=(const GodotFogBackend&) = delete;

    [[nodiscard]] fog::TextureHandle create_texture(
        const fog::TextureSpec& spec, std::span<const std::uint8_t> pixels) override;
    [[nodiscard]] bool update_texture(
        fog::TextureHandle texture, std::span<const std::uint8_t> pixels) override;
    void destroy_texture(fog::TextureHandle texture) override;
    void bind(fog::ConsumerId consumer, fog::TextureHandle texture, const fog::FogMapping& mapping) override;
    void unbind(fog::ConsumerId consumer) override;
    [[nodiscard]] std::string failure_cause() const override;

    // Registers a caller-owned material as a fog consumer (capture, then force
    // unbound); returns its ID for fog::TextureCache::add_consumer.
    // forget_material restores the captured parameters and drops the mapping.
    [[nodiscard]] fog::ConsumerId register_material(const godot::RID& material);
    void forget_material(fog::ConsumerId consumer);

    struct Readback {
        std::uint32_t width{};
        std::uint32_t height{};
        bool r8{};
        bool mipmaps{};
        std::vector<std::uint8_t> bytes;
    };
    // Reads the texture back from the RenderingServer (not from a CPU copy).
    [[nodiscard]] std::optional<Readback> read_back(fog::TextureHandle texture) const;
    [[nodiscard]] godot::RID texture_rid(fog::TextureHandle texture) const;
    [[nodiscard]] std::size_t live_textures() const noexcept { return textures_.size(); }

    // Uniform names every fog consumer shader declares.
    static constexpr std::string_view texture_parameter = "eawr_fog_texture";
    static constexpr std::string_view origin_parameter = "eawr_fog_origin";
    static constexpr std::string_view extent_parameter = "eawr_fog_extent";
    static constexpr std::string_view size_parameter = "eawr_fog_size";
    static constexpr std::string_view bound_parameter = "eawr_fog_bound";
    static constexpr std::array<std::string_view, 5> parameters{
        texture_parameter, origin_parameter, extent_parameter, size_parameter, bound_parameter};

    // Synthetic unshaded consumer: nearest texel-centre sampling, half-open
    // bounds tested before clamping, dark outside or while unbound, and the
    // attenuation applied once to linear surface RGB. Adapter evidence only;
    // no production material uses it.
    [[nodiscard]] static std::string_view synthetic_nearest_shader() noexcept;

private:
    struct Texture {
        godot::RID rid;
        fog::TextureSpec spec;
    };
    struct Material {
        godot::RID rid;
        // Pre-fog values in `parameters` order; nil means no override.
        std::array<godot::Variant, 5> captured;
    };

    static void force_unbound(const godot::RID& material);

    std::uint32_t max_dimension_;
    std::map<fog::TextureHandle, Texture> textures_;
    std::map<fog::ConsumerId, Material> materials_;
    fog::TextureHandle next_texture_{1};
    fog::ConsumerId next_consumer_{1};
    std::string failure_;
};

} // namespace eawr::presentation::godot_backend
