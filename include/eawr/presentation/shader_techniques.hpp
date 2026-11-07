#pragma once

#include "eawr/presentation/shader_states.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// S-20: metadata selection and flow only. The activation owner supplies actual
// stage/resource/input/state compatibility; this component performs no device
// validation, expression evaluation, resource upload or renderer admission.
namespace eawr::presentation::shader_techniques {
inline constexpr std::uint32_t policy_version = 1U;
enum class Validation { compatible, stage, resources, inputs, expressions, states, device };
struct CandidateValidation final {
    Validation regular{Validation::device}, software{Validation::device};
};
struct Settings final {
    std::uint32_t shader_detail{}, water_detail{}, vendor{};
    bool software_processing{};
};
enum class Rejection { editor, shader_detail, water_detail, vendor, validation };
struct CandidateTrace final {
    std::size_t technique{};
    Rejection reason{};
    std::optional<Validation> validation;
    bool software_before{}, software_after{};
};
struct Selection final {
    std::uint32_t version{policy_version};
    std::optional<std::size_t> technique;
    std::optional<bool> validated_software; // selected mode before SWVP backoff
    std::vector<CandidateTrace> rejected;
    bool software_processing{}, cpu_skin{};
    std::size_t work{};
};
struct Limits final {
    std::size_t techniques{1024U}, passes{4096U}, annotations{1024U};
    // Bytes in each borrowed descriptor span: name views exclude a terminal
    // zero; object text payloads include their required terminal zero.
    std::size_t string_bytes{4096U}, states{4096U}, work{1024U * 1024U};
};

// R-08: first compatible candidate in declaration order. Validation has
// exactly one entry per technique and separate results for the retained SWVP
// mode; the complete table's outcomes are checked before selection. Consulted
// known annotations are static, scalar and unique in this subset;
// duplicate/dynamic/wrongly typed known annotations are named refusals. LOD
// values alone are ASCII-insensitive. Empty selection means no candidate;
// the activation owner must report R-10's load/validation failure.
[[nodiscard]] core::Result<Selection> select_technique(const assets::EffectView& effect,
    Settings settings, std::span<const CandidateValidation> validation, Limits limits = {});

struct EffectiveState final {
    shader_states::Field field{};
    // Exactly one alternative is present. Assigned values retain the P4b root
    // parameter/expression identity; their default is never substituted.
    std::optional<shader_states::Literal> initial;
    std::optional<shader_states::Assignment> assigned;
};
struct PassBoundary final {
    std::size_t pass{};
    bool cleanup{}; // trailing R-11 marker, pending cleanup qualification
    shader_states::Plan assignments; // sparse original order, including shaders
    std::vector<EffectiveState> render_state; // effective state after this pass
    // Effective stage state at this boundary, including proposed carry from
    // the earlier pass; explicit assignments remain in assignments.
    std::optional<assets::EffectShaderAssignment> vertex, pixel;
};
struct PassFlow final {
    std::uint32_t version{policy_version};
    std::size_t technique{};
    std::vector<PassBoundary> passes;
    bool requires_evaluation{}, requires_parameter_resolution{};
    bool requires_cleanup_qualification{};
    std::size_t work{};
};
struct InitialState final {
    // Explicit caller assertion of the canonical initial device baseline.
    // This is not an implicit reset per material/pass or a claim about prior
    // device state. Other inherited-state contexts need a later qualified API.
    bool depth_attachment{};
};
// Partial R-11: classify the trailing true AlamoCleanup annotation; its texture
// use record is out of scope. Project policy proposes one programmable draw
// plus that optional boundary. Retail no-geometry cleanup and interpass state
// carry are UNVERIFIED; a marked boundary sets requires_cleanup_qualification
// and must be qualified before admission. Unspecified shaders, fixed draws,
// misplaced cleanup, duplicate shader stages and real multipass refuse.
// Expressions/parameter roots remain admission prerequisites. Sampler
// state/resource realization still belongs to P3a/P4b and the consumer.
// Retain the immutable EffectView and backing bytes for every returned index.
[[nodiscard]] core::Result<PassFlow> make_pass_flow(const assets::EffectView& effect,
    std::size_t technique, InitialState initial, Limits limits = {});

namespace diagnostic_codes {
inline constexpr std::string_view descriptor = "EAWR-SHADER-TECH-0001";
inline constexpr std::string_view unsupported = "EAWR-SHADER-TECH-0002";
inline constexpr std::string_view limit = "EAWR-SHADER-TECH-0003";
}
} // namespace eawr::presentation::shader_techniques
