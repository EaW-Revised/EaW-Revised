#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/core/result.hpp"
#include "eawr/sim/snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation {

// The order is part of the production renderer contract. New producers append
// work to a pass; they do not reorder the frame graph.
enum class RenderPass : std::uint8_t {
    opaque,
    alpha_tested,
    transparent,
    post,
};

inline constexpr std::array<RenderPass, 4> render_pass_order{
    RenderPass::opaque,
    RenderPass::alpha_tested,
    RenderPass::transparent,
    RenderPass::post,
};

[[nodiscard]] constexpr std::string_view to_string(RenderPass pass) noexcept {
    switch (pass) {
    case RenderPass::opaque: return "opaque";
    case RenderPass::alpha_tested: return "alpha-tested";
    case RenderPass::transparent: return "transparent";
    case RenderPass::post: return "post";
    }
    return "invalid";
}

// These priorities are deliberately spaced so a consumer can add local work
// inside a pass without crossing the public pass boundary. Godot still owns
// opaque depth ordering and transparent depth sorting; the priority is the
// project-owned dependency submitted to that engine-managed ordering.
[[nodiscard]] constexpr bool is_valid_render_pass(const RenderPass pass) noexcept {
    return pass == RenderPass::opaque || pass == RenderPass::alpha_tested
        || pass == RenderPass::transparent || pass == RenderPass::post;
}

[[nodiscard]] constexpr std::int32_t render_pass_priority(const RenderPass pass) noexcept {
    switch (pass) {
    case RenderPass::opaque: return -16;
    case RenderPass::alpha_tested: return -8;
    case RenderPass::transparent: return 0;
    case RenderPass::post: return 8;
    }
    return 0;
}

enum class MaterialRoute : std::uint8_t {
    legacy_effect,
    modern_spatial,
};

[[nodiscard]] constexpr bool is_valid_material_route(const MaterialRoute route) noexcept {
    return route == MaterialRoute::legacy_effect || route == MaterialRoute::modern_spatial;
}

struct MaterialBinding final {
    std::string name;
    assets::ParameterValue value;
};

// Additive schema changes increment schema_version. Version 1 deliberately
// represents both translated legacy effects and newly authored Godot spatial
// shaders without making either route depend on the other.
struct MaterialDescription final {
    static constexpr std::uint32_t current_schema_version = 1;

    std::uint32_t schema_version{current_schema_version};
    MaterialRoute route{MaterialRoute::legacy_effect};
    RenderPass pass{RenderPass::opaque};
    std::string program;
    std::string technique;
    std::string pass_name;
    std::vector<MaterialBinding> bindings;
};

struct PresentationTransform final {
    sim::EntityId entity_id{};
    sim::AssetId asset_id{};
    // Column-major affine 4x4 matrix for presentation backends.
    std::array<float, 16> column_major{};
};

// This is the only fixed-to-float bridge used by the renderer. The immutable
// snapshot remains owned by simulation and is never retained mutably.
[[nodiscard]] std::vector<PresentationTransform> adapt_snapshot(
    const sim::RenderSnapshot& snapshot);

struct ResourceReference final {
    sim::AssetId asset_id{};
    std::size_t references{};
};

struct FixedCamera final {
    std::uint32_t width{1280};
    std::uint32_t height{720};
    float vertical_fov_degrees{45.0F};
    float near_plane{1.0F};
    float far_plane{20000.0F};
    std::array<float, 3> eye{0.0F, 420.0F, 1050.0F};
    std::array<float, 3> target{0.0F, 0.0F, 0.0F};
    std::array<float, 3> up{0.0F, 1.0F, 0.0F};
};

struct CaptureResult final {
    // The renderer owns capture/encoding only. Platform applications decide
    // where (or whether) these PNG bytes are persisted and hashed.
    std::vector<std::byte> png_bytes;
    std::uint32_t width{};
    std::uint32_t height{};
};

struct BackendInfo final {
    std::string engine;
    std::string rendering_method;
    std::string adapter_vendor;
    std::string adapter_name;
    std::string driver_api;
};

class Renderer {
public:
    virtual ~Renderer() = default;

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    [[nodiscard]] virtual core::Result<void> upload(
        sim::AssetId asset_id,
        const assets::Model& model,
        const assets::Texture& texture,
        const MaterialDescription& material) = 0;
    [[nodiscard]] virtual core::Result<void> retain(sim::AssetId asset_id) = 0;
    [[nodiscard]] virtual core::Result<void> release(sim::AssetId asset_id) = 0;
    [[nodiscard]] virtual std::vector<ResourceReference> resources() const = 0;

    virtual void submit(std::shared_ptr<const sim::RenderSnapshot> snapshot) = 0;
    [[nodiscard]] virtual core::Result<CaptureResult> capture(const FixedCamera& camera) = 0;
    [[nodiscard]] virtual BackendInfo backend_info() const = 0;
    [[nodiscard]] virtual std::span<const core::Diagnostic> diagnostics() const = 0;

protected:
    Renderer() = default;
    Renderer(Renderer&&) noexcept = default;
    Renderer& operator=(Renderer&&) noexcept = default;
};

namespace diagnostic_codes {
inline constexpr std::string_view invalid_material = "EAWR-RENDER-0001";
inline constexpr std::string_view duplicate_asset = "EAWR-RENDER-0002";
inline constexpr std::string_view missing_asset = "EAWR-RENDER-0003";
inline constexpr std::string_view backend_unavailable = "EAWR-RENDER-0004";
inline constexpr std::string_view upload_failed = "EAWR-RENDER-0005";
inline constexpr std::string_view capture_failed = "EAWR-RENDER-0006";
inline constexpr std::string_view shader_compile_failed = "EAWR-RENDER-0007";
inline constexpr std::string_view invalid_skin_pose = "EAWR-RENDER-0008";
} // namespace diagnostic_codes

// Fails closed with one bounded EAWR-RENDER-0001 error naming the first
// selector outside the contract: schema, route, render pass, then for the
// legacy route the effect family, technique, pass name and the family's
// permitted render passes, in that order. Modern spatial sources that cannot
// reach Godot's spatial compiler fail with EAWR-RENDER-0007 before any RID
// exists. A spatial source without fragment() would compile but is outside
// this renderer's contract, so it fails with EAWR-RENDER-0001 instead.
// Passing this check is not compilation success.
[[nodiscard]] core::Result<void> validate_material(const MaterialDescription& material);

// Offset one past the `;` of a modern source's leading `shader_type spatial;`
// statement. Only whitespace and comments may precede it, as Godot requires;
// anything else, another shader type, or an unterminated comment or string
// yields nullopt. The renderer inserts its compile probe at this offset, so a
// probe reflected after shader_set_code means Godot's shading-language
// compiler accepted the source as spatial.
[[nodiscard]] std::optional<std::size_t> spatial_declaration_end(std::string_view source);

} // namespace eawr::presentation
