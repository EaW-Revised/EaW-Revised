#pragma once

#include "eawr/presentation/shader_bindings.hpp"
#include "eawr/presentation/shader_states.hpp"

// S-21: bounded numeric updates only. Activation and the backend still own
// fallback reporting, resource updates and admission of concrete state variants.
namespace eawr::presentation::shader_evaluation {
inline constexpr std::uint32_t policy_version = 1U;
struct Limits final {
    std::size_t words{65536U}, links{4096U}, work{262144U}, inputs{4096U}, constants{4096U};
};
struct NumericInput final {
    assets::EffectValueKind kind{};
    // Logical row order, unpadded, including successive array elements.
    std::span<const std::uint32_t> words;
};
struct ParameterInput final { std::size_t parameter{}; NumericInput value; };
struct EngineInput final { shader_bindings::EngineInput input{}; NumericInput value; };
struct Inputs final {
    std::span<const ParameterInput> parameters;
    std::span<const EngineInput> engine;
};
struct ParameterDependency final {
    std::size_t parameter{};
    std::optional<shader_bindings::EngineInput> engine_input;
};
struct Operand final {
    assets::EffectValueKind kind{};
    // Root first: a supplied material override wins; a reached engine identity
    // requires its authoritative provider, matching P3's stage-constant policy.
    std::vector<ParameterDependency> parameters;
    std::vector<std::uint32_t> defaults; // logical row order
};
struct Uniform final {
    shader_bindings::ConstantBinding binding;
    Operand operand;
    std::size_t offset{}, words{};
};
struct State final { shader_states::Assignment assignment; Operand operand; std::size_t byte_offset{}; };
struct Plan final {
    std::uint32_t version{policy_version};
    assets::Source source;
    std::vector<Uniform> uniforms;
    std::vector<State> states; // ordered sparse assignments; duplicates retained
    std::size_t words{};
    Limits limits;
};
// Prepare from P1/P3/P4 at activation, retaining no borrowed input bytes. Only
// reached effect expressions refuse; unrelated effect expressions are not visited.
// Caller supplies the used stage. Every nonempty PRES attached to that stage
// refuses unconditionally, even when the stage CTAB is empty.
[[nodiscard]] core::Result<Plan> prepare(const assets::EffectView& effect,
    const assets::EffectProgram& program, const shader_states::Plan& states = {}, Limits limits = {});
struct Output final {
    std::vector<std::uint32_t> words;
    std::vector<shader_states::Literal> states;
    std::size_t work{};
    bool valid{};
};
// Allocate once per retained plan/draw context, outside the draw path.
[[nodiscard]] Output make_output(const Plan& plan);
// No success-path allocations. Failure invalidates the complete output: the
// caller must use the explicit fallback, never partially upload its contents.
// State values are the exact ordered variant predicates, not a hashed key.
// Missing engine inputs refuse; parameter omission uses the prepared default.
[[nodiscard]] core::Result<void> update(const Plan& plan, Inputs inputs, Output& output);
namespace diagnostic_codes {
inline constexpr std::string_view descriptor = "EAWR-SHADER-EVAL-0001";
inline constexpr std::string_view unsupported = "EAWR-SHADER-EVAL-0002";
inline constexpr std::string_view limit = "EAWR-SHADER-EVAL-0003";
inline constexpr std::string_view missing_input = "EAWR-SHADER-EVAL-0004";
}
} // namespace eawr::presentation::shader_evaluation
