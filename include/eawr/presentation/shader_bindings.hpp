#pragma once

#include "eawr/assets/effect.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// S-13..16: activation-time binding metadata shared by compiled and source
// effects. No compiler, renderer object, expression evaluation or draw work.
namespace eawr::presentation::shader_bindings {

inline constexpr std::uint32_t policy_version = 1U;

enum class UpdateDomain { immutable, material, scene, instance };
enum class EngineInput {
    world, view, projection, world_view, view_projection, world_view_projection,
    world_inverse, view_inverse, world_view_inverse, eye_position, eye_object_position,
    resolution, ambient, sun_direction, sun_object_direction, sun_view_direction,
    sun_diffuse, sun_specular, sh_all, sh_fill, light_scale, fog, distance_fade, time,
    skin_palette, sky_texture, fog_texture, cloud_texture, fog_u, fog_v, cloud_u, cloud_v,
    wind_bend, wind_grass,
};

struct NumericShape final {
    assets::EffectValueKind kind{};
    assets::EffectValueCategory value_class{};
    std::uint32_t rows{}, columns{}, elements{1U};
};

enum class ConstantSource { effect_parameter, compiler_default };
struct ConstantBinding final {
    std::string name;
    assets::EffectRegisterSet register_set{};
    std::uint32_t first_register{}, register_count{};
    NumericShape shape;
    ConstantSource source{};
    std::optional<std::size_t> parameter;
    std::optional<EngineInput> engine_input;
    UpdateDomain update{UpdateDomain::immutable};
    // Raw register words, including register padding. Engine bindings have no
    // frozen initial value. Numeric material defaults are packed once here.
    std::vector<std::uint32_t> default_words;
};

struct TextureBinding final {
    assets::EffectValueKind kind{};
    std::optional<std::size_t> parameter; // immediate texture parameter; aliases resolve only its default
    std::optional<EngineInput> engine_input;
    UpdateDomain update{UpdateDomain::material};
    std::string default_name;
    bool unbound{true}; // admission must resolve a material/engine resource
};
struct SamplerElementBinding final {
    std::optional<TextureBinding> texture;
    std::vector<std::size_t> states; // immutable EffectView state indices for P4
};
struct SamplerBinding final {
    std::string name;
    std::uint32_t first_register{}, register_count{};
    std::size_t parameter{};
    // Resolved sampler value, retaining each element's texture/state links.
    // Indices refer to the same immutable EffectView used to make the plan.
    std::size_t value{};
    std::vector<SamplerElementBinding> elements;
};

struct ExpressionInputs final {
    std::size_t expression{}; // program.preshaders index
    std::vector<ConstantBinding> constants;
};

struct ProgramBindings final {
    std::uint32_t version{policy_version};
    assets::EffectProgramStage stage{};
    std::vector<ConstantBinding> constants;
    std::vector<SamplerBinding> samplers;
    std::vector<ExpressionInputs> expressions;
    // A consumer must provide P5 evaluation before admitting these programs;
    // a default-value-only upload is never an alternative to evaluation.
    bool requires_expression_evaluation{};
};

struct BindingLimits final {
    std::size_t constants{4096U};
    std::size_t register_words{64U * 1024U};
    std::size_t work{256U * 1024U};
};

// CTAB order determines output order. Parameter links must match name/shape;
// source-stage descriptors may omit the link and resolve by exact name.
// Unknown used semantics, overlapping registers, malformed descriptors and
// unsupported shapes fail with a bounded located diagnostic.
[[nodiscard]] core::Result<ProgramBindings> make_program_bindings(
    const assets::EffectView& effect, const assets::EffectProgram& program,
    BindingLimits limits = {});

enum class VertexInputPolicy { rigid, godot_pre_skinned, source_palette };
enum class Interpolation { vertex_only, perspective, flat };
enum class InterfaceValue { position, normal, color, texcoord, tangent, binormal, fog, point_size };
struct InterfaceBinding final {
    InterfaceValue value{};
    std::uint32_t semantic_index{}, register_type{}, register_index{}, mask{};
    Interpolation interpolation{Interpolation::vertex_only};
};

// Raw DCL metadata stays authoritative. Missing legacy output declarations
// require backend reflection before linking; they are not invented from a
// hard-coded register manifest. A source palette on pre-skinned input is a
// named refusal rather than a second skin transform.
// Legacy pixel DCL usage fields are not semantic identities and require the
// reflected/implicit-register route. Components retain stored numeric values;
// this metadata operation performs no colour transfer or saturation.
[[nodiscard]] core::Result<std::vector<InterfaceBinding>> make_declared_interface(
    const assets::EffectProgram& program, VertexInputPolicy policy,
    const ProgramBindings& bindings);

namespace diagnostic_codes {
inline constexpr std::string_view descriptor = "EAWR-SHADER-BIND-0001";
inline constexpr std::string_view unsupported = "EAWR-SHADER-BIND-0002";
inline constexpr std::string_view limit = "EAWR-SHADER-BIND-0003";
}

} // namespace eawr::presentation::shader_bindings
