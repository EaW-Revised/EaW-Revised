#include "../../../src/presentation/shader_stage/internal.hpp"
#include "../../../src/presentation/shader_stage/reflection_association.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/presentation/shader_stage.hpp"
#include "eawr/presentation/shader_lowering.hpp"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace eawr::presentation::shader_runtime;
namespace shader_lowering = eawr::presentation::shader_lowering;
namespace {
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void expect_error(const std::vector<StageDiagnostic> &diagnostics,
                  DiagnosticPhase phase, DiagnosticReason reason) {
    check(diagnostics.size() == 1U && diagnostics[0].phase == phase &&
              diagnostics[0].reason == reason,
          "failure phase and reason agree with the reporting contract");
}
std::vector<std::byte> words(std::initializer_list<std::uint32_t> values) {
    std::vector<std::byte> out;
    for (auto word : values)
        for (unsigned shift = 0; shift < 32U; shift += 8)
            out.push_back(static_cast<std::byte>(word >> shift));
    return out;
}
Digest sha(std::span<const std::byte> value) {
    return std::bit_cast<Digest>(eawr::core::sha256(
        {reinterpret_cast<const std::uint8_t *>(value.data()), value.size()}));
}
std::vector<std::byte> vertex() {
    return words({0xfffe0101, 0x1f, 0x80000000, 0x900f0000, 0x01, 0xc00f0000,
                  0x90e40000, 0xffff});
}
StageOptions options() {
    StageOptions out;
    out.required_capabilities = {
        CapabilityToken::d3d_bytecode_to_spirv,
        CapabilityToken::spirv_to_glsl_330,
        CapabilityToken::typed_resource_reflection,
        CapabilityToken::typed_interface_reflection,
        CapabilityToken::combined_sampler_relationships};
    out.variants = {
        VariantToken::desktop_glsl_330, VariantToken::preserve_clip_depth,
        VariantToken::preserve_vertex_y, VariantToken::uniform_blocks,
        VariantToken::combine_image_samplers};
    return out;
}
StageArtifact invented() {
    StageArtifact a;
    a.stage = eawr::assets::EffectProgramStage::vertex;
    a.source_major = 1;
    a.source_minor = 1;
    a.compiled_stage = vertex();
    // Authored wire fixture, never executed as a shader. Public SPIR-V framing:
    // Shader capability, Logical/GLSL450 memory model, empty vertex entry
    // point.
    a.spirv = {
        0x07230203, 0x10000, 0, 5,          0, 0x20011, 1, 0x3000e, 0,      1,
        0x5000f,    0,       1, 0x6e69616d, 0, 0x20013, 2, 0x30021, 3,      2,
        0x50036,    2,       1, 0,          3, 0x200f8, 4, 0x100fd, 0x10038};
    a.glsl = "#version 330 core\nvoid main() {}\n";
    a.options_manifest = {std::byte{'['}, std::byte{']'}};
    a.identity.compiler.public_c_abi.vkd3d_public_api = {2, 1};
    a.identity.compiler.public_c_abi.spirv_cross_c_abi = {0, 61, 0};
    a.identity.compiler.public_c_abi.pointer_bits = 64;
    a.identity.compiler.capabilities = options().required_capabilities;
    a.identity.input_stage_sha256 = sha(a.compiled_stage);
    a.identity.options_manifest_sha256 = sha(a.options_manifest);
    a.identity.selected_variants = options().variants;
    return a;
}
PreparedStageRequest current(const StageArtifact &a) {
    return {{}, a.identity, a.options_manifest};
}
void codec() {
    const auto a = invented();
    auto encoded = encode_stage_result(a);
    check(encoded.bytes.has_value(), "invented complete envelope");
    auto decoded = decode_stage_result(*encoded.bytes, current(a));
    check(decoded.artifact.has_value(), "owned round trip");
    check(decoded.artifact->compiled_stage == a.compiled_stage &&
              decoded.artifact->spirv == a.spirv &&
              decoded.artifact->glsl == a.glsl,
          "all owned sections survive");
    const auto view = view_stage_program(*decoded.artifact);
    check(bool(view) && view.value().bytes.data() ==
                            decoded.artifact->compiled_stage.data(),
          "P1 view borrows decoded owner");
    auto second = encode_stage_result(*decoded.artifact);
    check(second.bytes == encoded.bytes, "canonical re-encode");
    const auto hash = eawr::core::sha256_hex(
        {reinterpret_cast<const std::uint8_t *>(encoded.bytes->data()),
         encoded.bytes->size()});
    check(
        hash ==
            "2308d4c81bfe33f39963864484d3e66699feb687f31ea9f8975216b8a0c29e51",
        "independently pinned version-one wire digest");
    for (std::size_t i = 0; i < encoded.bytes->size(); ++i) {
        auto corrupted = *encoded.bytes;
        corrupted[i] ^= std::byte{1};
        const auto refused = decode_stage_result(corrupted, current(a));
        check(!refused.artifact && refused.diagnostics.size() == 1U &&
                  refused.diagnostics[0].phase == DiagnosticPhase::cache_read &&
                  (refused.diagnostics[0].reason ==
                       DiagnosticReason::invalid_cached_result ||
                   refused.diagnostics[0].reason ==
                       DiagnosticReason::budget_exceeded),
              "every single-byte corruption is a recoverable cache refusal");
    }
    for (const auto size : {0U, 8U, 39U, 40U}) {
        const auto refused = decode_stage_result(
            std::span(*encoded.bytes).first(size), current(a));
        check(!refused.artifact, "truncation refused");
        expect_error(refused.diagnostics, DiagnosticPhase::cache_read,
                     DiagnosticReason::invalid_cached_result);
    }
    auto trailing = *encoded.bytes;
    trailing.push_back(std::byte{});
    check(!decode_stage_result(trailing, current(a)).artifact,
          "trailing byte refused");
    auto expected = current(a);
    expected.expected_identity.compiler.package_lock_sha256[0] ^= std::byte{1};
    check(!decode_stage_result(*encoded.bytes, expected).artifact,
          "current package identity required");
    expected = current(a);
    expected.expected_identity.compiler.public_c_abi.manifest_sha256[0] ^=
        std::byte{1};
    check(!decode_stage_result(*encoded.bytes, expected).artifact,
          "current ABI identity required");
    expected = current(a);
    expected.expected_identity.compiler.capability_manifest_sha256[0] ^=
        std::byte{1};
    check(!decode_stage_result(*encoded.bytes, expected).artifact,
          "current capability identity required");
    expected = current(a);
    expected.expected_identity.input_stage_sha256[0] ^= std::byte{1};
    check(!decode_stage_result(*encoded.bytes, expected).artifact,
          "current input identity required");
    expected = current(a);
    expected.canonical_options_manifest.push_back(std::byte{' '});
    check(!decode_stage_result(*encoded.bytes, expected).artifact,
          "current options required");
    StageLimits small;
    small.envelope_bytes = 39;
    const auto cache_budget =
        decode_stage_result(*encoded.bytes, current(a), small);
    check(!cache_budget.artifact, "entry budget");
    expect_error(cache_budget.diagnostics, DiagnosticPhase::cache_read,
                 DiagnosticReason::budget_exceeded);
    small = {};
    small.input_bytes = 1;
    check(!encode_stage_result(a, small).bytes, "stage budget");
    small = {};
    small.cancelled = [](void *) { return true; };
    const auto cache_cancelled =
        decode_stage_result(*encoded.bytes, current(a), small);
    check(!cache_cancelled.artifact, "cancelled decode");
    expect_error(cache_cancelled.diagnostics, DiagnosticPhase::cache_read,
                 DiagnosticReason::cancelled);
    auto bad = a;
    bad.spirv[0] = 0;
    auto fresh_failure = encode_stage_result(bad);
    check(!fresh_failure.bytes, "fresh malformed SPIR-V refused");
    expect_error(fresh_failure.diagnostics, DiagnosticPhase::reflection,
                 DiagnosticReason::backend_failure);
    bad = a;
    ReflectedType cycle;
    cycle.kind = TypeKind::structure;
    cycle.members.push_back({"loop", 0, {}, {}, {}, false});
    bad.reflection.types.push_back(cycle);
    check(!encode_stage_result(bad).bytes, "type cycle refused");
    bad = a;
    bad.reflection.resources.push_back(
        {ResourceKind::uniform_buffer, 99, {}, {}, {}, {}, false, false});
    check(!encode_stage_result(bad).bytes, "dangling reflection refused");
    bad = a;
    bad.spirv[10] = 15;
    check(!encode_stage_result(bad).bytes, "zero instruction length refused");
    bad = a;
    bad.spirv[11] = 4;
    check(!encode_stage_result(bad).bytes, "SPIR-V stage mismatch refused");
    bad = a;
    bad.glsl += static_cast<char>(0xff);
    check(!encode_stage_result(bad).bytes, "invalid UTF-8 refused");
    bad = a;
    bad.glsl += '\0';
    check(!encode_stage_result(bad).bytes, "embedded text NUL refused");
    auto typed = a;
    ReflectedType vector_type;
    vector_type.scalar = ScalarKind::floating;
    vector_type.bit_width = 32;
    vector_type.vector_width = 4;
    ReflectedType block;
    block.kind = TypeKind::structure;
    block.declared_byte_size = 64;
    block.members.push_back({"invented_member", 0, 16, 16, 16, true});
    block.array_dimensions.push_back({2, {}});
    block.array_stride = 64;
    typed.reflection.types = {vector_type, block};
    typed.reflection.resources.push_back(
        {ResourceKind::uniform_buffer, 1, "invented_source", "invented_emitted",
         BindingPoint{0, 7},
         SourceRegister{RegisterKind::float_constant_bank, 0, 0}, true, false});
    ReflectedInterface leaf;
    leaf.type = 0;
    leaf.aggregate_type = 1;
    leaf.member_path = {0};
    leaf.location = 3;
    leaf.component = 0;
    leaf.declared_component_mask = 15;
    leaf.interpolation = Interpolation::flat;
    leaf.centroid = true;
    typed.reflection.interfaces.push_back(leaf);
    auto typed_wire = encode_stage_result(typed);
    check(typed_wire.bytes.has_value(), "typed metadata envelope");
    auto typed_result = decode_stage_result(*typed_wire.bytes, current(typed));
    check(typed_result.artifact.has_value(), "typed metadata decode");
    const auto &reflection = typed_result.artifact->reflection;
    check(reflection.types[1].members[0].row_major &&
              reflection.types[1].members[0].byte_offset == 16ULL &&
              reflection.types[1].members[0].matrix_stride == 16U &&
              reflection.types[1].array_dimensions[0].literal_count == 2U &&
              reflection.types[1].array_stride == 64U &&
              reflection.interfaces[0].member_path ==
                  std::vector<std::uint32_t>{0} &&
              reflection.interfaces[0].interpolation == Interpolation::flat &&
              reflection.interfaces[0].centroid &&
              reflection.resources[0].source_register->index == 0U,
          "typed layout, array, bank and interface fields survive");
    const auto dialect = summarize_dialect(
        "/* if(fake){} */\n#version 330 core\nstruct S { vec4 "
        "v; }; uniform B { vec4 x; };\nvoid helper() { if "
        "(true) { for (;;) {} } }\nvoid main() {}\n");
    check(dialect.version_330_core_only && !dialect.extension_directive &&
              dialect.structure_declarations == 1U &&
              dialect.uniform_blocks == 1U && dialect.member_declarations == 2U &&
              dialect.function_definitions == 2U && dialect.conditionals == 1U &&
              dialect.loops == 1U,
          "dialect summary skips comments and records shapes");
    check(!summarize_dialect("#version 330\nvoid main(){}\n")
               .version_330_core_only,
          "plain version spelling remains explicit");
    check(summarize_dialect("#version 330 core\n#extension X : require\n")
              .extension_directive,
          "extension cannot disappear from summary");
    Runtime runtime;
    check(runtime.verified_lock_sha256().empty(),
          "closed runtime has no attestation");
    CompilerSession session(std::move(runtime));
    const auto parsed = eawr::assets::read_shader_program(a.compiled_stage);
    check(bool(parsed), "invented P1 stage");
    const auto missing = prepare_stage(session, parsed.value(), options());
    check(!missing.request, "missing package refuses preparation");
    expect_error(missing.diagnostics, DiagnosticPhase::package_load,
                 DiagnosticReason::dependency_unavailable);
    auto malformed = parsed.value();
    malformed.bytes = std::span(a.compiled_stage).first(4);
    const auto bad_input = prepare_stage(session, malformed, options());
    check(!bad_input.request, "framing refuses before unavailable package");
    expect_error(bad_input.diagnostics, DiagnosticPhase::stage_input,
                 DiagnosticReason::malformed_stage);
    const auto profile_bytes = words({0xfffe0400, 0xffff});
    auto unsupported = parsed.value();
    unsupported.bytes = profile_bytes;
    unsupported.major = 4;
    unsupported.minor = 0;
    const auto bad_profile = prepare_stage(session, unsupported, options());
    check(!bad_profile.request,
          "unsupported profile refuses before unavailable package");
    expect_error(bad_profile.diagnostics, DiagnosticPhase::stage_input,
                 DiagnosticReason::unsupported_profile);
    const auto moved = std::move(session);
    static_cast<void>(moved);
    check(!prepare_stage(session, parsed.value(), options()).request,
          "moved-from session refuses safely");
    StageLimits limits;
    limits.input_bytes = 1;
    CompilerSession bounded(Runtime{}, limits);
    const auto limited = prepare_stage(bounded, parsed.value(), options());
    check(!limited.request && limited.diagnostics[0].reason ==
                                  DiagnosticReason::budget_exceeded,
          "input limit before backend");
    std::cout << "owned envelope: golden identity, all-byte corruption, "
                 "current-identity, framing, graph, UTF-8, limits and dialect "
                 "controls passed\n";
}
void association_refusals() {
    StageReflection reflection;
    ReflectedInterface leaf;
    leaf.direction = InterfaceDirection::input; leaf.location = 7U; leaf.emitted_name = "deliberately_unrelated";
    leaf.declared_component_mask = 15U;
    reflection.interfaces.push_back(leaf);
    detail::Scan scan;
    scan.input.push_back({"POSITION", 0U, 7U, 15U, 15U, 0U});
    const auto available = [&]() { return reflection.interfaces[0U].source.availability == AssociationAvailability::public_backend_metadata; };
    detail::associate_interfaces(reflection, scan);
    check(available(), "unique full-register signature associates");
    scan.input.push_back(scan.input.front());
    detail::associate_interfaces(reflection, scan);
    check(!available(), "duplicate signatures remain unavailable");
    scan.input.pop_back(); scan.input[0U].semantic = "INVENTED";
    detail::associate_interfaces(reflection, scan);
    check(!available(), "unknown signature semantics remain unavailable");
    scan.input[0U].semantic = "POSITION"; scan.input[0U].mask = 3U;
    detail::associate_interfaces(reflection, scan);
    check(!available(), "mismatched component mask remains unavailable");
    scan.input[0U].mask = 15U; reflection.interfaces[0U].component = 1U;
    detail::associate_interfaces(reflection, scan);
    check(!available(), "packed component leaf remains unavailable");
    reflection.interfaces[0U].component.reset(); scan.input[0U].sysval = 1U;
    detail::associate_interfaces(reflection, scan);
    check(!available(), "system semantic is not treated as a generic input");
    scan.input.clear();
    detail::associate_interfaces(reflection, scan);
    check(!available(), "missing signature remains unavailable");
    scan.output.push_back({"COLOR", 1U, 7U, 15U, 15U, 0U});
    detail::associate_interfaces(reflection, scan);
    check(!available(), "output signature cannot associate an input leaf");
    reflection.interfaces[0U].direction = InterfaceDirection::output;
    detail::associate_interfaces(reflection, scan);
    check(available() && reflection.interfaces[0U].source.semantic->index == 1U,
        "output direction uses its own signature table");
    scan.output[0U].used_mask = 31U;
    detail::associate_interfaces(reflection, scan);
    check(!available(), "used components outside the declaration remain unavailable");
    scan.output[0U].used_mask = 15U; reflection.interfaces[0U].member_path.push_back(0U);
    detail::associate_interfaces(reflection, scan);
    check(!available(), "nested interface member remains unavailable");
    reflection.interfaces[0U].member_path.clear(); reflection.interfaces[0U].index = 1U;
    detail::associate_interfaces(reflection, scan);
    check(!available(), "indexed interface leaf remains unavailable");
}
void measured_associations(CompilerSession& session) {
    // Invented SM3 controls permute register, semantic and declaration index
    // independently. All values are consumed so optimizer removal cannot
    // manufacture a successful association from an absent port.
    struct Control { std::uint32_t reg, usage, index; };
    constexpr Control controls[]{{7U, 0U, 0U}, {2U, 3U, 0U}, {11U, 5U, 2U}, {5U, 10U, 1U}};
    for (const auto& control : controls) {
        const auto input = 0x900f0000U | control.reg;
        const auto source = 0x90e40000U | control.reg;
        const auto code = words({0xfffe0300U,
            0x0200001fU, 0x80000000U | (control.index << 16U) | control.usage, input,
            0x0200001fU, 0x80000000U, 0xe00f0000U,
            0x02000001U, 0xe00f0000U, source, 0xffffU});
        const auto program = eawr::assets::read_shader_program(code);
        check(bool(program), "association control has valid P1 bytecode framing");
        detail::Scan scan;
        static_cast<void>(detail::prepare(StageSessionAccess::get(session), program.value(), options(), &scan));
        auto result = translate_stage(session, program.value(), options());
        if (!result.artifact) throw std::runtime_error(result.diagnostics[0U].detail);
        const auto found = std::find_if(result.artifact->reflection.interfaces.begin(), result.artifact->reflection.interfaces.end(),
            [](const auto& item) { return item.direction == InterfaceDirection::input && !item.builtin; });
        check(found != result.artifact->reflection.interfaces.end(), "measured input survives compilation");
        check(scan.input.size() == 1U && scan.input[0U].index == control.reg
            && found->location == control.reg && found->source.semantic
            && found->source.semantic->usage == control.usage && found->source.semantic->index == control.index
            && found->source.declared_component_mask == 15U,
            "public signature register/location and source semantic agree for permuted SM3 controls");
        std::cout << "association register=" << control.reg << " usage=" << control.usage
            << " semantic_index=" << control.index << " reflected_location=" << *found->location << " mask=15\n";
    }
}
void real(const std::filesystem::path &path) {
    StageArtifact retained;
    const auto late_failure =
        words({0xfffe0200, 0x0200001f, 0x80000000, 0x900f0000, 0x02000001,
               0xa00f0000, 0x90e40000, 0xffff});
    {
        Runtime runtime;
        check(runtime.open(path) == Status::ready,
              runtime.diagnostic().c_str());
        abi::Api api;
        api.vkd3d_shader_compile =
            std::bit_cast<decltype(api.vkd3d_shader_compile)>(
                runtime.public_export(Backend::vkd3d, "vkd3d_shader_compile"));
        api.vkd3d_shader_free_shader_code =
            std::bit_cast<decltype(api.vkd3d_shader_free_shader_code)>(
                runtime.public_export(Backend::vkd3d,
                                      "vkd3d_shader_free_shader_code"));
        api.vkd3d_shader_free_messages =
            std::bit_cast<decltype(api.vkd3d_shader_free_messages)>(
                runtime.public_export(Backend::vkd3d,
                                      "vkd3d_shader_free_messages"));
        check(api.vkd3d_shader_compile && api.vkd3d_shader_free_shader_code &&
                  api.vkd3d_shader_free_messages,
              "native failure regression C exports");
        const auto valid = vertex();
        const abi::Option api21{4, 21};
        for (unsigned cycle = 0; cycle < 8U; ++cycle) {
            for (const bool nondefault : {false, true}) {
                const auto &source = nondefault ? valid : late_failure;
                abi::Compile info;
                info.source = {source.data(), source.size()};
                info.options = nondefault ? &api21 : nullptr;
                info.option_count = nondefault ? 1U : 0U;
                // Exercise the exact RAII owner used by translate_stage. API21
                // is only a negative ABI control; production options remain
                // fixed at API1.0.
                detail::CompileOutput output{api, info};
                check(output.result < 0 && output.code.data && output.messages,
                      "late failure has non-owned stale code fields and owned "
                      "messages");
            }
        }
    }
    const std::vector<std::vector<std::byte>> controls{
        vertex(),
        words({0xfffe0200, 0x0200001f, 0x80000000, 0x900f0000, 0x03000002,
               0x800f0000, 0x90e40000, 0xa0e40000, 0x03000002,
               0xc00f0000, 0x80e40000, 0xa0e40001, 0xffff}),
        words({0xffff0200, 0x0200001f, 0x80000000, 0xb00f0000, 0x0200001f,
               0x90000000, 0xa00f0800, 0x03000042, 0x800f0000, 0xb0e40000,
               0xa0e40800, 0x02000001, 0x800f0800, 0x80e40000, 0xffff})};
    for (std::size_t i = 0; i < controls.size(); ++i) {
        Runtime runtime;
        check(runtime.open(path) == Status::ready,
              runtime.diagnostic().c_str());
        check(!runtime.verified_lock_sha256().empty(),
              "verified package attested");
        CompilerSession session(std::move(runtime));
        if (i == 0U) {
            measured_associations(session);
            const auto invalid_program =
                eawr::assets::read_shader_program(late_failure);
            check(bool(invalid_program),
                  "late semantic failure has valid P1 framing");
            for (unsigned cycle = 0; cycle < 8U; ++cycle) {
                const auto failed = translate_stage(
                    session, invalid_program.value(), options());
                check(!failed.artifact,
                      "late public translation failure refuses");
                expect_error(failed.diagnostics, DiagnosticPhase::stage_compile,
                             DiagnosticReason::compiler_failure);
            }
        }
        const auto program = eawr::assets::read_shader_program(controls[i]);
        check(bool(program), "real fixture P1 framing");
        auto prepared = prepare_stage(session, program.value(), options());
        if (!prepared.request)
            throw std::runtime_error(prepared.diagnostics[0].detail);
        auto translated = translate_stage(session, program.value(),
                                          prepared.request->resolved_options);
        if (!translated.artifact)
            throw std::runtime_error(translated.diagnostics[0].detail);
        if (i == 0U) {
            const auto& interfaces = translated.artifact->reflection.interfaces;
            const auto associated = std::find_if(interfaces.begin(), interfaces.end(), [](const auto& leaf) {
                return leaf.direction == InterfaceDirection::input && !leaf.builtin;
            });
            check(associated != interfaces.end() && associated->location == 0U && associated->source.semantic
                && associated->source.semantic->usage == 0U && associated->source.semantic->index == 0U,
                "pinned POSITION input association is measured through public scan and Cross reflection");
        }
        auto envelope = encode_stage_result(*translated.artifact);
        check(envelope.bytes.has_value(), "real complete envelope");
        auto decoded = decode_stage_result(*envelope.bytes, *prepared.request);
        check(decoded.artifact.has_value(),
              "real artifact validates against prelookup snapshot");
        check(!decoded.artifact->reflection.interfaces.empty(),
              "real typed interfaces");
          if (i == 1U) {
            check(
                std::any_of(decoded.artifact->reflection.resources.begin(),
                            decoded.artifact->reflection.resources.end(),
                            [](const auto &r) {
                                return r.source_register &&
                                       r.source_register->kind ==
                                           RegisterKind::float_constant_bank &&
                                       r.source_register->index == 0U;
                            }),
                  "float bank is a bank, not cN");
              const auto lowered = shader_lowering::lower_stage(decoded.artifact->glsl, shader_lowering::Stage::vertex);
              // Identify this fixture's unique public SPIR-V Block type and
              // its explicit Offset decorations, independently of any names.
              std::set<std::uint32_t> blocks;
              std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> offsets;
              bool framing = true;
              const auto& spirv_words = decoded.artifact->spirv;
              for (std::size_t at = 5U; at < spirv_words.size();) {
                  const auto size = spirv_words[at] >> 16U, opcode = spirv_words[at] & 0xffffU;
                  if (!size || size > spirv_words.size() - at) { framing = false; break; }
                  if (opcode == 71U && size >= 3U && spirv_words[at + 2U] == 2U) blocks.insert(spirv_words[at + 1U]);
                  if (opcode == 72U && size >= 5U && spirv_words[at + 3U] == 35U)
                      offsets.emplace(std::pair{spirv_words[at + 1U], spirv_words[at + 2U]}, spirv_words[at + 4U]);
                  at += size;
              }
              bool all_associated = framing && blocks.size() == 1U;
              std::size_t used{};
              if (lowered) for (const auto& uniform : lowered.value().uniforms) {
                  if (uniform.read_mask == 0U) continue;
                  ++used;
                  std::size_t matches{};
                  for (const auto& resource : decoded.artifact->reflection.resources) {
                      if (resource.emitted_name != uniform.source_name
                          || resource.kind != ResourceKind::uniform_buffer
                          || resource.type >= decoded.artifact->reflection.types.size() || blocks.size() != 1U) continue;
                      const auto& block = decoded.artifact->reflection.types[resource.type];
                      for (std::size_t member = 0U; member < block.members.size(); ++member) {
                          const auto& value = block.members[member];
                          const auto index = static_cast<std::uint32_t>(member);
                          const auto expected = "eawr_block_" + std::to_string(*blocks.begin()) + "_member_" + std::to_string(index);
                          const auto offset = offsets.find({*blocks.begin(), index});
                          if (value.name == expected && uniform.member_name == expected && value.byte_offset
                              && offset != offsets.end() && *value.byte_offset == offset->second) ++matches;
                      }
                  }
                  if (matches != 1U) all_associated = false;
              }
              check(lowered && used >= 2U && all_associated,
                  "every used bank uniform has its exact assigned public name and SPIR-V Offset in typed reflection");
          }
        if (i == 2U)
            check(!decoded.artifact->reflection.combined_samplers.empty(),
                  "texture/sampler relationships retained");
        const auto d = summarize_dialect(decoded.artifact->glsl);
        std::cout << "dialect control=" << i
                  << " package="
                     "816fedd1459ea0f0a2f9cad0a05ba38673c230f29f26fea8c543e74a2"
                     "982dfbc"
                  << " options="
                  << eawr::core::sha256_hex(
                         {reinterpret_cast<const std::uint8_t *>(
                              decoded.artifact->options_manifest.data()),
                          decoded.artifact->options_manifest.size()})
                  << " version330core=" << d.version_330_core_only
                  << " extension=" << d.extension_directive
                  << " blocks=" << d.uniform_blocks
                  << " structures=" << d.structure_declarations
                  << " members=" << d.member_declarations
                  << " functions=" << d.function_definitions
                  << " conditionals=" << d.conditionals << " loops=" << d.loops
                  << " directives=";
        for (const auto &kind : d.directive_kinds)
            std::cout << kind << ',';
        std::cout << '\n';
        auto invalid = options();
        invalid.variants.push_back(VariantToken::fixup_clip_depth);
        const auto refused = prepare_stage(session, program.value(), invalid);
        check(!refused.request && refused.diagnostics[0].reason ==
                                      DiagnosticReason::unsupported_option,
              "conflicting variants refused");
        retained = std::move(*decoded.artifact);
    }
    check(!retained.glsl.empty() && bool(view_stage_program(retained)),
          "owned output survives all sessions and DLL contexts");
    std::cout
        << "real pinned stage translation: vertex, constant-bank, textured "
           "pixel, typed reflection and independent owned lifetimes passed\n";
}
} // namespace
int main(int argc, char **argv) {
    try {
        association_refusals();
        if (argc == 2 && std::string_view(argv[1]) == "--package-env") {
#ifdef _WIN32
            std::wstring path(32768, L'\0');
            const auto length = GetEnvironmentVariableW(
                L"EAWR_SHADER_RUNTIME_TEST_PACKAGE", path.data(),
                static_cast<DWORD>(path.size()));
            if (!length) {
                std::cout << "optional shader stage package absent\n";
                return 77;
            }
            check(length < path.size(), "package environment path too long");
            path.resize(length);
            real(path);
#else
            const auto *path = std::getenv("EAWR_SHADER_RUNTIME_TEST_PACKAGE");
            if (!path || !*path) {
                std::cout << "optional shader stage package absent\n";
                return 77;
            }
            real(path);
#endif
        } else
            codec();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
