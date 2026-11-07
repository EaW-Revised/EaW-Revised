#pragma once

#include "eawr/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// S-18: finite, activation-time lowering core. The caller still owns stage
// reflection reconciliation, sampler policy, renderer wrappers and admission.
namespace eawr::presentation::shader_lowering {

inline constexpr std::uint32_t policy_version = 3U;
enum class Stage { vertex, fragment };
enum class Scalar { floating, signed_integer, unsigned_integer, boolean, sampler_2d };
struct Type final {
    Scalar scalar{Scalar::floating};
    std::uint32_t width{1U};
    friend bool operator==(const Type&, const Type&) = default;
};
enum class Direction { input, output };
enum class Builtin { none, position, point_size, vertex_index, instance_index,
                     fragment_coordinate, front_facing, fragment_depth };
enum class Interpolation { smooth, flat, no_perspective };
struct Layout final {
    std::optional<std::uint32_t> location, component, binding, descriptor_set;
    bool std140{};
};
struct Port final {
    std::string source_name, emitted_name;
    Type type;
    Direction direction{};
    Builtin builtin{Builtin::none};
    Layout layout;
    Interpolation interpolation{Interpolation::smooth};
    bool centroid{};
    std::uint32_t read_mask{}, written_mask{};
    // Conservative final-value proof from the typed straight-line graph.
    // Unknown arithmetic/resources never acquire a bit merely from defaults.
    std::uint32_t proven_one_mask{};
};
struct Uniform final {
    // block/member are empty for a plain uniform. An array becomes separately
    // declared uniforms in ascending element order; no dynamic access survives.
    std::string source_name, block_name, member_name, emitted_name;
    std::optional<std::uint32_t> element;
    Type type;
    Layout layout;
    // Actual statically reached reads of this element, independent of the
    // backend's possibly larger declared source-register bank.
    std::uint32_t read_mask{};
};
struct Statistics final {
    std::size_t tokens{}, symbols{}, nodes{}, work{}, emitted_bytes{}, maximum_depth{};
};
struct LoweredStage final {
    std::uint32_t version{policy_version};
    Stage stage{};
    std::string function_name, function_source;
    // Ports are function parameters in this order. Uniforms must be declared
    // by the caller with exactly these types/names and qualified sampler policy.
    // Neither source spelling nor preliminary layout is a semantic association.
    std::vector<Port> ports;
    std::vector<Uniform> uniforms;
    Statistics statistics;
};
struct Limits final {
    std::size_t source_bytes{256U * 1024U};
    std::size_t tokens{64U * 1024U}; // includes the terminal token
    std::size_t symbols{4096U};
    std::size_t uniforms{4096U}; // includes flattened array elements
    std::size_t nodes{64U * 1024U};
    std::size_t depth{64U};
    std::size_t work{2U * 1024U * 1024U};
    std::size_t emitted_bytes{512U * 1024U};
};

// Owns all returned strings; input is borrowed only for this call. Supports
// straight-line scalar/vector GLSL 330, static uniform arrays/blocks and a
// finite intrinsic set. Unknown syntax, directives, types, symbols, illegal
// writes, undefined reads and dynamic indexing fail with bounded locations.
// This is a function fragment, not a complete/admitted Godot material.
[[nodiscard]] core::Result<LoweredStage> lower_stage(std::string_view source,
    Stage stage, std::string_view logical_source = {}, Limits limits = {});

namespace diagnostic_codes {
inline constexpr std::string_view syntax = "EAWR-SHADER-LOWER-0001";
inline constexpr std::string_view unsupported = "EAWR-SHADER-LOWER-0002";
inline constexpr std::string_view symbol = "EAWR-SHADER-LOWER-0003";
inline constexpr std::string_view type = "EAWR-SHADER-LOWER-0004";
inline constexpr std::string_view limit = "EAWR-SHADER-LOWER-0005";
}
} // namespace eawr::presentation::shader_lowering
