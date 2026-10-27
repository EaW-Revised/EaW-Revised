#pragma once

#include "eawr/assets/assets.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace eawr::assets {

inline constexpr std::uint32_t effect_descriptor_version = 1U;

enum class EffectProgramStage { vertex, pixel };
enum class EffectRegisterSet : std::uint16_t { boolean, integer4, floating4, sampler };
struct EffectConstantMember final {
    std::string_view name;
    std::size_t type{};
};
struct EffectConstantType final {
    std::uint16_t value_class{}, kind{}, rows{}, columns{}, elements{};
    std::vector<EffectConstantMember> members; // indices in program.constant_types
};
struct EffectConstant final {
    std::string_view name;
    EffectRegisterSet register_set{};
    std::uint16_t register_index{}, register_count{};
    std::size_t type{};
    std::span<const std::byte> defaults; // register representation, not packed effect defaults
    std::optional<std::size_t> parameter; // matching top-level effect parameter, if present
};
struct EffectDeclaration final {
    std::uint32_t usage{}, usage_index{}, register_type{}, register_index{}, texture_type{}, write_mask{};
};
enum class EffectCommentKind { unknown, constant_table, preshader, literal_table, expression_code };
struct EffectProgramComment final {
    EffectCommentKind kind{};
    std::uint32_t tag{};
    std::size_t byte_offset{};
    std::span<const std::byte> bytes; // includes tag; excludes comment token
};
struct EffectExpression final {
    std::uint32_t version{};
    std::size_t byte_offset{};
    std::span<const std::byte> bytes;
    std::vector<EffectProgramComment> sections; // typed envelopes; arithmetic remains opaque
    std::vector<EffectConstantType> constant_types;
    std::vector<EffectConstant> inputs;
    std::string_view creator, target;
    std::uint32_t compiler_flags{};
};
struct EffectProgram final {
    Source source;
    EffectProgramStage stage{};
    std::uint32_t major{}, minor{};
    std::size_t byte_offset{};
    std::span<const std::byte> bytes; // version through END, including comments
    std::vector<EffectProgramComment> comments;
    std::vector<EffectConstantType> constant_types;
    std::vector<EffectConstant> constants;
    std::vector<EffectDeclaration> declarations; // raw semantic ids, including unknown usages
    std::vector<EffectExpression> preshaders; // same order as PRES comments
    std::string_view creator, target;
    std::uint32_t compiler_flags{};
};

// EFX-02: numeric values are public effect metadata, not renderer policy.
enum class EffectValueKind : std::uint32_t {
    none, boolean, integer, floating, text, texture, texture_1d, texture_2d,
    texture_3d, texture_cube, sampler, sampler_1d, sampler_2d, sampler_3d,
    sampler_cube, pixel_program, vertex_program,
};
enum class EffectValueCategory : std::uint32_t {
    scalar, vector, row_matrix, column_matrix, object, structure,
};

struct EffectType final {
    EffectValueKind kind{};
    EffectValueCategory value_class{};
    std::string_view name;
    std::string_view semantic;
    std::uint32_t array_count{}; // zero denotes a single value
    std::uint32_t columns{};
    std::uint32_t rows{};
    std::vector<std::size_t> members; // indices in EffectView::types
};
struct EffectSamplerElement final {
    std::vector<std::size_t> states; // indices in EffectView::states
};
struct EffectValue final {
    std::size_t type{};
    std::span<const std::byte> bytes;
    std::vector<std::size_t> members; // flattened array-element/member order
    std::vector<std::uint32_t> object_slots;
    std::vector<EffectSamplerElement> sampler_elements;
    std::optional<std::size_t> initializer;
};
enum class EffectShaderAssignmentKind { null_shader, program, expression };
struct EffectShaderAssignment final {
    EffectProgramStage stage{};
    EffectShaderAssignmentKind kind{};
    std::optional<std::size_t> descriptor; // program/expression index; absent for null
    std::optional<std::size_t> parameter; // immediate named reference, when present
};
struct EffectState final {
    std::uint32_t identifier{};
    std::uint32_t selector{};
    std::size_t value{};
    std::optional<EffectShaderAssignment> shader{};
    std::size_t byte_offset{};
};
struct EffectParameter final {
    std::size_t value{};
    std::uint32_t flags{};
    std::vector<std::size_t> annotations; // typed values, not executed
};
struct EffectPass final {
    std::string_view name;
    std::vector<std::size_t> annotations;
    std::vector<std::size_t> states;
};
struct EffectTechnique final {
    std::string_view name;
    std::vector<std::size_t> annotations;
    std::vector<EffectPass> passes;
};
enum class EffectInitializerKind : std::uint32_t { expression, parameter_reference };
struct EffectInitializer final {
    std::array<std::uint32_t, 4> target{}; // preserved serialized path
    EffectInitializerKind kind{};
    std::size_t value{};
    std::span<const std::byte> bytes;
    std::optional<std::size_t> parameter;
    std::optional<std::size_t> program;
    std::optional<std::size_t> expression; // index in EffectView::expressions
    std::size_t byte_offset{};
    std::uint32_t expression_tag{}; // first word of a non-program expression, if present
};
struct EffectObject final {
    std::optional<EffectValueKind> kind;
    bool has_payload{};
    std::span<const std::byte> bytes; // empty is a stored empty/null default
    std::optional<std::size_t> initializer;
    std::optional<std::size_t> program;
    std::optional<std::size_t> expression;
    std::optional<std::uint32_t> resolved_object; // terminal named-reference slot; zero is null
    std::size_t byte_offset{};
};
struct EffectView final {
    Source source;
    std::uint32_t auxiliary_field{}; // retained; not used as a policy/count
    std::vector<EffectType> types;
    std::vector<EffectValue> values;
    std::vector<EffectState> states;
    std::vector<EffectParameter> parameters;
    std::vector<EffectTechnique> techniques;
    std::vector<EffectObject> objects; // slot zero is the null sentinel
    std::vector<std::uint32_t> raw_object_order; // serialized payload order, independent of slot ids
    std::vector<EffectInitializer> initializers;
    std::vector<EffectProgram> programs;
    std::vector<EffectExpression> expressions;
};

struct EffectReadLimits final {
    // EFX-05: logical metadata budget limits allocation amplification from small
    // descriptors independently of large opaque program payloads. Caller tunable.
    std::size_t metadata_bytes{64U * 1024U * 1024U};
};

// EFX-01..11. All text/byte views borrow immutable caller-owned storage. The
// activation/cache owner must retain that storage until every descriptor and
// consumer of its views is destroyed; moving a descriptor does not own its bytes.
// No compiler DLL, technique choice, state execution or shader translation.
[[nodiscard]] core::Result<EffectView> read_compiled_effect(
    std::span<const std::byte> bytes, Source source = {}, EffectReadLimits limits = {});

// EFX-08..11: standalone stage-compiler output uses the same descriptor. Borrowed
// spans have the same lifetime contract. This validates framing/metadata only.
// Source identifies the diagnostic logical origin; offsets address the compiled
// stage bytes. Source compilers retain source-line/entry/profile mappings and
// compile/cache provenance in their outer activation record. stored_size, when
// nonzero, must describe these stage bytes rather than the original source text.
[[nodiscard]] core::Result<EffectProgram> read_shader_program(
    std::span<const std::byte> bytes, Source source = {}, EffectReadLimits limits = {});

} // namespace eawr::assets
