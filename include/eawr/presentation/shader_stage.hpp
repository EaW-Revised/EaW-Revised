// Owned activation-worker results over the pinned public compiler C interfaces.
#pragma once
#include "eawr/assets/effect.hpp"
#include "eawr/presentation/shader_runtime.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::shader_runtime {
using Digest = std::array<std::byte, 32>;
using TypeIndex = std::uint32_t;
using ResourceIndex = std::uint32_t;

// B/P6 owns this opaque session, including DLL lifetime and bounded worker use.
// A receives a borrowed reference; no DLL load/free/export call is required.
struct StageLimits {
    std::size_t input_bytes{4U * 1024U * 1024U},
        spirv_words{4U * 1024U * 1024U};
    std::size_t glsl_bytes{16U * 1024U * 1024U},
        metadata_bytes{16U * 1024U * 1024U};
    std::size_t table_entries{65536}, nesting{64}, string_bytes{1024U * 1024U};
    std::size_t envelope_bytes{64U * 1024U * 1024U};
    bool (*cancelled)(void *){};
    void *cancellation_context{}; // borrowed for synchronous calls only
};
class CompilerSession {
public:
    // Consumes a Runtime opened with the complete reviewed pin. Closed or
    // replacement identities are refused; reported versions alone never
    // suffice.
    explicit CompilerSession(Runtime runtime, StageLimits limits = {});
    ~CompilerSession();
    CompilerSession(CompilerSession &&) noexcept;
    CompilerSession &operator=(CompilerSession &&) noexcept;
    CompilerSession(const CompilerSession &) = delete;
    CompilerSession &operator=(const CompilerSession &) = delete;

private:
    friend struct StageSessionAccess;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Versioned finite token IDs; serialize in numeric order, without duplicates.
// These identify measured wrapper operations, not universal profile support.
enum class CapabilityToken : std::uint16_t {
    d3d_bytecode_to_spirv = 1,
    spirv_to_glsl_330 = 2,
    typed_resource_reflection = 3,
    typed_interface_reflection = 4,
    combined_sampler_relationships = 5,
};
enum class VariantToken : std::uint16_t {
    desktop_glsl_330 = 1,
    preserve_clip_depth = 10,
    fixup_clip_depth = 11,
    preserve_vertex_y = 20,
    flip_vertex_y = 21,
    uniform_blocks = 30, // 31 reserved; plain uniforms not in the first scope
    combine_image_samplers = 41, // 40 reserved; no preserved-form variant yet
};
// Exactly one token per variant group. Unknown/duplicate/conflicting tokens
// are refused. P2b never selects a coordinate/binding/admission policy for A.
enum class RegisterKind : std::uint8_t {
    float_constant_bank,
    integer_constant_bank,
    boolean_constant_bank,
    texture,
    sampler,
};
struct SourceRegister {
    RegisterKind kind{};
    std::uint32_t space{}, index{}; // bank indices must be zero, not cN/iN/bN
};
struct BindingPoint {
    std::uint32_t descriptor_set{}, binding{};
};
struct BindingAssignment {
    SourceRegister source;
    BindingPoint target;
};
struct CombinedBindingAssignment {
    SourceRegister image, sampler;
    BindingPoint target;
};
struct StageOptions {
    std::uint32_t manifest_version{1};
    std::uint32_t binding_plan_version{1};
    std::vector<CapabilityToken> required_capabilities;
    std::vector<VariantToken> variants;
    // Empty plans use B's versioned deterministic scan/assignment rule, which
    // is recorded in the resolved manifest. Explicit plans remain A's policy.
    std::vector<BindingAssignment> bindings;
    std::vector<CombinedBindingAssignment> combined_bindings;
};

enum class ScalarKind : std::uint8_t {
    boolean,
    signed_integer,
    unsigned_integer,
    floating
};
enum class TypeKind : std::uint8_t {
    scalar_vector_matrix,
    structure,
    image,
    sampler,
    sampled_image
};
enum class ImageDimension : std::uint8_t {
    d1,
    d2,
    d3,
    cube,
    rectangle,
    buffer,
    subpass
};
struct ArrayDimension {
    std::optional<std::uint32_t> literal_count;
    std::optional<std::uint32_t> specialization_id;
};
struct TypeMember {
    std::string name;
    TypeIndex type{};
    std::optional<std::uint64_t> byte_offset, byte_size;
    std::optional<std::uint32_t> matrix_stride;
    bool row_major{};
};
struct ImageType {
    ImageDimension dimension{};
    TypeIndex sampled_type{};
    bool arrayed{}, multisampled{}, depth{}, storage{};
    std::optional<std::uint32_t>
        storage_format; // public SPIR-V numeric identity
};
struct ReflectedType {
    TypeKind kind{};
    std::optional<ScalarKind> scalar;
    std::uint32_t bit_width{}, vector_width{1}, columns{1};
    std::vector<ArrayDimension> array_dimensions;
    std::optional<std::uint32_t> array_stride;
    std::optional<std::uint64_t> declared_byte_size;
    std::vector<TypeMember> members;
    std::optional<ImageType> image;
};
enum class ResourceKind : std::uint8_t {
    uniform_buffer,
    storage_buffer,
    combined_sampler,
    separate_image,
    separate_sampler,
    storage_image,
    subpass_input,
    push_constant,
    atomic_counter,
};
struct ReflectedResource {
    ResourceKind kind{};
    TypeIndex type{};
    std::string source_name, emitted_name;
    std::optional<BindingPoint> binding;
    std::optional<SourceRegister> source_register;
    bool readonly{}, writeonly{};
};
enum class InterfaceDirection : std::uint8_t { input, output };
enum class BuiltinKind : std::uint8_t {
    position,
    point_size,
    vertex_index,
    instance_index,
    fragment_coordinate,
    front_facing,
    fragment_depth,
    clip_distance,
    cull_distance,
    sample_id,
    sample_position,
    sample_mask,
    unsupported,
};
struct Builtin {
    BuiltinKind kind{};
    std::uint32_t spirv_identity{};
};
enum class Interpolation : std::uint8_t { smooth, flat, no_perspective };
enum class AssociationAvailability : std::uint8_t {
    unavailable,
    public_backend_metadata
};
struct D3dRegisterIdentity {
    std::uint32_t register_class{}, index{};
};
struct D3dSemanticIdentity {
    std::uint32_t usage{}, index{};
    std::string name;
};
struct SourceInterfaceAssociation {
    AssociationAvailability availability{AssociationAvailability::unavailable};
    std::optional<D3dRegisterIdentity> source_register;
    std::optional<D3dSemanticIdentity> semantic;
    std::optional<std::uint32_t> declared_component_mask, used_component_mask;
};
struct ReflectedInterface {
    InterfaceDirection direction{};
    TypeIndex type{}; // leaf type; block members are flattened without loss
    std::optional<TypeIndex> aggregate_type;
    std::vector<std::uint32_t> member_path;
    std::string source_name, emitted_name;
    std::optional<std::uint32_t> location, component, index;
    std::optional<std::uint32_t> declared_component_mask;
    std::optional<Builtin> builtin;
    // Only associations actually supplied by public backend metadata. Never
    // infer a legacy register/semantic from emitted variable names. A owns
    // documented implicit D3D output derivation and validates it against this.
    SourceInterfaceAssociation source;
    Interpolation interpolation{Interpolation::smooth};
    bool centroid{}, sample{};
};
struct CombinedSampler {
    ResourceIndex combined{}, image{},
        sampler{}; // local resource-table indices
};
struct StageReflection {
    std::vector<ReflectedType> types;
    std::vector<ReflectedResource> resources;
    std::vector<ReflectedInterface> interfaces;
    std::vector<CombinedSampler> combined_samplers;
};

enum class DiagnosticPhase : std::uint8_t {
    package_load,
    stage_input,
    descriptor_scan,
    stage_compile,
    backend_translate,
    reflection,
    cache_read,
};
enum class DiagnosticReason : std::uint16_t {
    dependency_unavailable,
    dependency_integrity,
    dependency_abi,
    malformed_stage,
    unsupported_profile,
    unsupported_option,
    unsupported_resource,
    unsupported_interface,
    compiler_failure,
    backend_failure,
    budget_exceeded,
    cancelled,
    invalid_cached_result,
    reflection_failure,
    invalid_shader, // public compiler INVALID_SHADER; deterministic for this request
};
enum class Severity : std::uint8_t { warning, error };
struct DiagnosticLocation {
    std::string logical_source;
    std::optional<std::uint32_t> line, column;
    std::optional<std::uint64_t> byte_offset, byte_length;
};
struct StageDiagnostic {
    DiagnosticPhase phase{};
    DiagnosticReason reason{};
    Severity severity{Severity::error};
    std::optional<DiagnosticLocation> location;
    std::string detail; // bounded/sanitized; never a source/compiler-text dump
};
enum class CAbiCallingConvention : std::uint8_t { windows_x64_cdecl };
struct PublicCAbiIdentity {
    Digest manifest_sha256{}; // declared signatures/layouts, headers, ownership
    std::array<std::uint32_t, 2> vkd3d_public_api{};
    std::array<std::uint32_t, 3> spirv_cross_c_abi{};
    CAbiCallingConvention calling_convention{
        CAbiCallingConvention::windows_x64_cdecl};
    std::uint32_t pointer_bits{};
};
struct CompilerIdentity {
    Digest
        package_lock_sha256{}; // verified full closure, not reported versions
    PublicCAbiIdentity public_c_abi;
    Digest capability_manifest_sha256{};
    std::uint32_t result_schema_version{1}, options_manifest_version{1};
    std::vector<CapabilityToken> capabilities;
};
// Activation keys need the verified package/ABI identity before any stages
// are compiled. Closed or rejected sessions expose no usable identity.
[[nodiscard]] std::optional<CompilerIdentity> compiler_identity(CompilerSession& session);
struct StageIdentity {
    CompilerIdentity compiler;
    Digest input_stage_sha256{}, options_manifest_sha256{};
    std::vector<VariantToken> selected_variants;
};
struct PreparedStageRequest {
    StageOptions resolved_options;
    StageIdentity expected_identity;
    std::vector<std::byte> canonical_options_manifest;
};
struct StagePreparation {
    std::optional<PreparedStageRequest> request;
    std::vector<StageDiagnostic> diagnostics;
};
struct StageArtifact {
    assets::EffectProgramStage stage{};
    std::uint32_t source_major{}, source_minor{};
    std::vector<std::byte> compiled_stage; // owned original P1 stage envelope
    std::vector<std::uint32_t>
        spirv;        // owned words, wire encoding is little endian
    std::string glsl; // owned backend output, not a Godot/driver shader binary
    StageReflection reflection;
    StageIdentity identity;
    std::vector<std::byte> options_manifest;
};
struct StageResult {
    std::optional<StageArtifact>
        artifact; // absent on refusal, no partial success
    std::vector<StageDiagnostic> diagnostics;
};

// B/P6 calls this before C's lookup. Resolves/scans the binding plan without
// translating, canonicalizes options/tokens, hashes current stage bytes, and
// binds the request to the CURRENT verified session/package/ABI/capabilities.
// The activation owner retains this immutable snapshot and gives C its owned
// manifest/identities; C does not normalize compiler options independently.
[[nodiscard]] StagePreparation
prepare_stage(CompilerSession &session, const assets::EffectProgram &program,
              const StageOptions &options);

// Synchronous activation-worker operation; the descriptor only borrows for
// this call. The session owns DLL/ABI details; returned values are independent
// of DLL/context/session lifetime. A retains binding/state/admission policy.
[[nodiscard]] StageResult translate_stage(CompilerSession &session,
                                          const assets::EffectProgram &program,
                                          const StageOptions &options);

// Complete success envelope only. Decode requires the freshly prepared CURRENT
// request and validates everything before publishing an owned artifact.
struct EnvelopeResult {
    std::optional<std::vector<std::byte>> bytes;
    std::vector<StageDiagnostic> diagnostics;
};
[[nodiscard]] EnvelopeResult encode_stage_result(const StageArtifact &artifact,
                                                 StageLimits limits = {});
[[nodiscard]] StageResult
decode_stage_result(std::span<const std::byte> envelope,
                    const PreparedStageRequest &current,
                    StageLimits limits = {});
// Returned views borrow artifact.compiled_stage; retain it unchanged while
// used.
[[nodiscard]] core::Result<assets::EffectProgram>
view_stage_program(const StageArtifact &artifact);
struct DialectSummary {
    std::vector<std::string> directive_kinds;
    std::uint64_t uniform_blocks{}, structure_declarations{},
        member_declarations{};
    std::uint64_t function_definitions{}, conditionals{}, loops{};
    bool version_330_core_only{}, extension_directive{};
};
// Counts/shapes only; never includes source, generated text or compiler
// messages.
[[nodiscard]] DialectSummary summarize_dialect(std::string_view glsl);
} // namespace eawr::presentation::shader_runtime
