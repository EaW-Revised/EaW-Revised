#pragma once

#include "eawr/assets/effect.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

// S-19: activation-time normalization only. A successful plan is not proof
// that its states, capabilities, multipass flow or expressions can be rendered.
namespace eawr::presentation::shader_states {
inline constexpr std::uint32_t policy_version = 1U;
enum class DepthMode : std::uint32_t { disabled, depth, w_buffer };
enum class Compare : std::uint32_t { never = 1U, less, equal, less_equal, greater, not_equal, greater_equal, always };
enum class Cull : std::uint32_t { none = 1U, clockwise, counter_clockwise };
enum class BlendFactor : std::uint32_t {
    zero = 1U, one, source_color, inverse_source_color, source_alpha, inverse_source_alpha,
    destination_alpha, inverse_destination_alpha, destination_color, inverse_destination_color,
    source_alpha_saturate, both_source_alpha, both_inverse_source_alpha, constant,
    inverse_constant, second_source_color, inverse_second_source_color,
};
enum class BlendOperation : std::uint32_t { add = 1U, subtract, reverse_subtract, minimum, maximum };
enum class Address : std::uint32_t { wrap = 1U, mirror, clamp, border, mirror_once };
enum class Filter : std::uint32_t { none, point, linear, anisotropic, pyramidal = 6U, gaussian, convolution };
enum class Field {
    depth_test, depth_write, depth_compare, cull, alpha_test, alpha_compare, alpha_reference,
    blend_enabled, blend_source, blend_destination, blend_operation, separate_alpha,
    alpha_source, alpha_destination, alpha_operation, color_mask, srgb_write, depth_bias, slope_bias,
    stencil_enabled, address_u, address_v, address_w, border_color, min_filter, mag_filter,
    mip_filter, lod_bias, max_mip, anisotropy, srgb_texture,
};
enum class Domain { boolean, depth, comparison, culling, blend_factor, blend_operation,
                    byte, mask, word, floating, address, filter, positive_integer };
using Literal = std::variant<bool, std::uint32_t, float, DepthMode, Compare, Cull,
                             BlendFactor, BlendOperation, Address, Filter>;
struct Assignment final {
    Field field{};
    Domain domain{};
    std::size_t state{}, value{}; // indices in the retained immutable EffectView
    assets::EffectValueKind source_kind{};
    std::optional<Literal> literal;
    // A parameter keeps its first override identity, even through aliases.
    // Expression/parameter results require later evaluation, conversion and
    // domain validation; their effect defaults are never a replacement.
    // This is the root override identity, not a flattened dependency list.
    // Evaluation must resolve the retained value/alias graph and all reached
    // parameter overrides under its own bounds, including terminal overrides.
    std::optional<std::size_t> parameter, expression;
};
struct Plan final {
    std::uint32_t version{policy_version};
    std::vector<Assignment> assignments; // explicit file order, duplicates retained
    std::vector<std::size_t> state_order; // includes numeric, shader and texture assignments
    std::vector<std::size_t> shader_states, texture_states;
    bool requires_evaluation{};
    bool requires_resource_resolution{};
    std::size_t work{};
};
struct Limits final { std::size_t states{4096U}, work{256U * 1024U}; };
struct DefaultState final { Field field; Literal literal; };

// Canonical initial device baselines, never an implicit reset between passes.
// The caller composes ordered passes/cleanup with the established baseline.
[[nodiscard]] std::vector<DefaultState> render_defaults(bool depth_attachment);
[[nodiscard]] std::vector<DefaultState> sampler_defaults();

// Descriptor indices remain borrowed identities; retain the EffectView and its
// backing bytes. Unknown IDs/selectors, malformed links/shapes, unsupported
// literal domains, cycles and budgets refuse with source/byte locations.
[[nodiscard]] core::Result<Plan> normalize_pass(const assets::EffectView& effect,
    const assets::EffectPass& pass, Limits limits = {});
[[nodiscard]] core::Result<Plan> normalize_sampler(const assets::EffectView& effect,
    const assets::EffectSamplerElement& sampler, Limits limits = {});
// S-19/S-21: apply the same scalar conversion/domain policy after dynamic
// resolution. The assignment must originate from normalize_pass/sampler.
[[nodiscard]] core::Result<Literal> convert_scalar(const assets::Source& source,
    const Assignment& assignment, assets::EffectValueKind kind, std::uint32_t bits,
    std::size_t byte_offset);
namespace diagnostic_codes {
inline constexpr std::string_view descriptor = "EAWR-SHADER-STATE-0001";
inline constexpr std::string_view unsupported = "EAWR-SHADER-STATE-0002";
inline constexpr std::string_view limit = "EAWR-SHADER-STATE-0003";
}
} // namespace eawr::presentation::shader_states
