#include "internal.hpp"
#include <algorithm>
#include <limits>
#include <set>
#include <tuple>

namespace eawr::presentation::shader_runtime::detail {
namespace {
constexpr auto input = DiagnosticPhase::stage_input;
constexpr auto scanning = DiagnosticPhase::descriptor_scan;
auto key(const SourceRegister &r) {
    return std::tuple{r.kind, r.space, r.index};
}
SourceRegister source(const abi::Descriptor &d) {
    require(d.space == 0 && d.count == 1, scanning,
            DiagnosticReason::unsupported_resource,
            "unsupported descriptor space or array");
    if (d.type == 2 && d.index <= 2)
        return {static_cast<RegisterKind>(d.index), 0, 0};
    if (d.type == 0)
        return {RegisterKind::texture, d.space, d.index};
    if (d.type == 3)
        return {RegisterKind::sampler, d.space, d.index};
    refuse(scanning, DiagnosticReason::unsupported_resource,
           "unsupported descriptor register kind");
}
void check_program(const assets::EffectProgram &program,
                   const StageLimits &limits) {
    checkpoint(limits, input);
    require(program.bytes.size() <= limits.input_bytes, input,
            DiagnosticReason::budget_exceeded, "stage input budget exceeded");
    require(program.bytes.size() >= 8 && program.bytes.size() % 4 == 0, input,
            DiagnosticReason::malformed_stage,
            "stage envelope framing invalid");
    std::uint32_t version{};
    for (unsigned i = 0; i < 4; ++i)
        version |= std::to_integer<std::uint32_t>(program.bytes[i]) << (i * 8U);
    const auto kind = version >> 16U, major = (version >> 8U) & 255U,
               minor = version & 255U;
    require(kind == 0xfffeU || kind == 0xffffU, input,
            DiagnosticReason::malformed_stage, "stage signature invalid");
    require(major >= 1 && major <= 3 &&
                (major == 1
                     ? (kind == 0xfffeU ? minor == 1 : minor >= 1 && minor <= 4)
                     : minor == 0),
            input, DiagnosticReason::unsupported_profile,
            "unsupported bytecode profile");
    const auto parsed =
        assets::read_shader_program(program.bytes, {}, {limits.metadata_bytes});
    if (!parsed && parsed.error().code == assets::diagnostic_codes::limit)
        refuse(input, DiagnosticReason::budget_exceeded,
               "stage descriptor budget exceeded");
    require(bool(parsed), input, DiagnosticReason::malformed_stage,
            "malformed compiled stage");
    require(parsed.value().stage == program.stage &&
                parsed.value().major == program.major &&
                parsed.value().minor == program.minor,
            input, DiagnosticReason::malformed_stage,
            "stage descriptor disagrees with bytecode");
    require(program.major >= 1 && program.major <= 3, input,
            DiagnosticReason::unsupported_profile,
            "unsupported bytecode profile");
}
} // namespace
Scan scan(Session &s, const assets::EffectProgram &program) {
    checkpoint(s.limits, scanning);
    abi::Signatures signatures;
    abi::Target target;
    target.environment = 2;
    signatures.next = &target;
    abi::Pairs pairs;
    pairs.next = &signatures;
    abi::Descriptors descriptors;
    descriptors.next = &pairs;
    const abi::Option options[]{{4, 0}, {1, 0}};
    abi::Compile info;
    info.next = &descriptors;
    info.source = {program.bytes.data(), program.bytes.size()};
    info.options = options;
    info.option_count = 2;
    // Scan the actual SPIR-V-target interface, with the same public API options
    // as compilation. Messages are never parsed or copied into diagnostics.
    char *messages{};
    bool outputs_owned{};
    struct Cleanup {
        abi::Api &api;
        abi::Descriptors &descriptors;
        abi::Pairs &pairs;
        abi::Signatures &signatures;
        char *&messages;
        bool &outputs_owned;
        ~Cleanup() {
            if (outputs_owned) {
                api.vkd3d_shader_free_scan_descriptor_info(&descriptors);
                api.vkd3d_shader_free_scan_combined_resource_sampler_info(
                    &pairs);
                api.vkd3d_shader_free_scan_signature_info(&signatures);
            }
            api.vkd3d_shader_free_messages(messages);
        }
    } cleanup{s.api, descriptors, pairs, signatures, messages, outputs_owned};
    const auto result = s.api.vkd3d_shader_scan(&info, &messages);
    outputs_owned = result >= 0;
    require(result == 0, scanning, DiagnosticReason::compiler_failure,
            "descriptor scan failed");
    checkpoint(s.limits, scanning);
    const auto bounded = [&](std::size_t count, const void *pointer) {
        require(count <= s.limits.table_entries && (!count || pointer),
                scanning, DiagnosticReason::budget_exceeded,
                "scan table budget exceeded");
    };
    bounded(descriptors.count, descriptors.data);
    bounded(pairs.count, pairs.data);
    bounded(signatures.input.count, signatures.input.data);
    bounded(signatures.output.count, signatures.output.data);
    std::size_t metadata_left = s.limits.metadata_bytes;
    const auto reserve_metadata = [&](std::size_t count, std::size_t size) {
        require(count <= metadata_left / size, scanning,
                DiagnosticReason::budget_exceeded,
                "scan metadata budget exceeded");
        metadata_left -= count * size;
    };
    reserve_metadata(descriptors.count, sizeof(abi::Descriptor));
    reserve_metadata(pairs.count, sizeof(abi::Pair));
    reserve_metadata(signatures.input.count, sizeof(ScannedSignature));
    reserve_metadata(signatures.output.count, sizeof(ScannedSignature));
    require(!signatures.patch.count, DiagnosticPhase::reflection,
            DiagnosticReason::unsupported_interface,
            "unsupported patch signature");
    Scan out;
    if (descriptors.count)
        out.descriptors.assign(descriptors.data,
                               descriptors.data + descriptors.count);
    if (pairs.count)
        out.pairs.assign(pairs.data, pairs.data + pairs.count);
    const auto copy = [&](const abi::Signature &signature,
                          std::vector<ScannedSignature> &target) {
        for (std::size_t i = 0; i < signature.count; ++i) {
            const auto &v = signature.data[i];
            require(v.stream == 0 && v.mask <= 15 && !(v.used_mask & ~v.mask),
                    DiagnosticPhase::reflection,
                    DiagnosticReason::unsupported_interface,
                    "unsupported source signature");
            auto text_limits = s.limits;
            text_limits.string_bytes =
                std::min(text_limits.string_bytes, metadata_left);
            auto semantic = bounded_text(v.semantic, text_limits, scanning);
            metadata_left -= semantic.size();
            target.push_back({std::move(semantic), v.semantic_index, v.index,
                              v.mask, v.used_mask,
                              static_cast<std::uint32_t>(v.sysval)});
        }
    };
    copy(signatures.input, out.input);
    copy(signatures.output, out.output);
    return out;
}
PreparedStageRequest prepare(Session &s, const assets::EffectProgram &program,
                             const StageOptions &requested, Scan *scan_result) {
    check_program(program, s.limits);
    if (s.unavailable)
        refuse(s.unavailable->phase, s.unavailable->reason,
               "verified compiler session unavailable");
    require(requested.manifest_version == 1 &&
                requested.binding_plan_version == 1,
            input, DiagnosticReason::unsupported_option,
            "unsupported options schema");
    require(requested.required_capabilities.size() <= 5 &&
                requested.variants.size() == 5 &&
                requested.bindings.size() <= s.limits.table_entries &&
                requested.combined_bindings.size() <= s.limits.table_entries,
            input, DiagnosticReason::unsupported_option,
            "unsupported options count");
    PreparedStageRequest out;
    out.resolved_options = requested;
    auto &o = out.resolved_options;
    std::sort(o.required_capabilities.begin(), o.required_capabilities.end());
    std::sort(o.variants.begin(), o.variants.end());
    require(std::adjacent_find(o.required_capabilities.begin(),
                               o.required_capabilities.end()) ==
                o.required_capabilities.end(),
            input, DiagnosticReason::unsupported_option,
            "duplicate capability");
    for (const auto token : o.required_capabilities)
        require(std::find(s.identity.capabilities.begin(),
                          s.identity.capabilities.end(),
                          token) != s.identity.capabilities.end(),
                input, DiagnosticReason::unsupported_option,
                "unsupported capability");
    require(o.variants[0] == VariantToken::desktop_glsl_330 &&
                (o.variants[1] == VariantToken::preserve_clip_depth ||
                 o.variants[1] == VariantToken::fixup_clip_depth) &&
                (o.variants[2] == VariantToken::preserve_vertex_y ||
                 o.variants[2] == VariantToken::flip_vertex_y) &&
                o.variants[3] == VariantToken::uniform_blocks &&
                o.variants[4] == VariantToken::combine_image_samplers,
            input, DiagnosticReason::unsupported_option,
            "unsupported or conflicting variant");
    auto scanned = scan(s, program);
    std::vector<SourceRegister> registers;
    for (const auto &descriptor : scanned.descriptors)
        registers.push_back(source(descriptor));
    std::sort(registers.begin(), registers.end(),
              [](const auto &a, const auto &b) { return key(a) < key(b); });
    require(std::adjacent_find(registers.begin(), registers.end(),
                               [](const auto &a, const auto &b) {
                                   return key(a) == key(b);
                               }) == registers.end(),
            scanning, DiagnosticReason::unsupported_resource,
            "duplicate source descriptor");
    if (o.bindings.empty()) {
        for (const auto &reg : registers)
            o.bindings.push_back(
                {reg, {0, static_cast<std::uint32_t>(o.bindings.size())}});
    }
    std::sort(o.bindings.begin(), o.bindings.end(),
              [](const auto &a, const auto &b) {
                  return key(a.source) < key(b.source);
              });
    require(o.bindings.size() == registers.size(), scanning,
            DiagnosticReason::unsupported_option,
            "binding plan does not cover descriptors");
    std::vector<std::uint32_t> targets;
    for (std::size_t i = 0; i < registers.size(); ++i) {
        require(key(o.bindings[i].source) == key(registers[i]) &&
                    o.bindings[i].target.descriptor_set == 0,
                scanning, DiagnosticReason::unsupported_option,
                "binding plan register or descriptor set mismatch");
        targets.push_back(o.bindings[i].target.binding);
    }
    const auto pair_key = [](const CombinedBindingAssignment &p) {
        return std::tuple{key(p.image), key(p.sampler)};
    };
    std::vector<CombinedBindingAssignment> pairs;
    for (const auto &p : scanned.pairs) {
        require(p.image_space == 0 && p.sampler_space == 0, scanning,
                DiagnosticReason::unsupported_resource,
                "unsupported sampler pair space");
        pairs.push_back({{RegisterKind::texture, 0, p.image},
                         {RegisterKind::sampler, 0, p.sampler},
                         {}});
    }
    std::sort(pairs.begin(), pairs.end(), [&](const auto &a, const auto &b) {
        return pair_key(a) < pair_key(b);
    });
    pairs.erase(std::unique(pairs.begin(), pairs.end(),
                            [&](const auto &a, const auto &b) {
                                return pair_key(a) == pair_key(b);
                            }),
                pairs.end());
    if (o.combined_bindings.empty()) {
        std::set<std::uint32_t> used(targets.begin(), targets.end());
        std::uint32_t binding{};
        for (auto p : pairs) {
            while (used.contains(binding)) {
                require(binding != std::numeric_limits<std::uint32_t>::max(),
                        scanning, DiagnosticReason::unsupported_option,
                        "binding namespace exhausted");
                ++binding;
            }
            used.insert(binding);
            p.target = {0, binding};
            targets.push_back(binding);
            o.combined_bindings.push_back(p);
        }
    }
    std::sort(o.combined_bindings.begin(), o.combined_bindings.end(),
              [&](const auto &a, const auto &b) {
                  return pair_key(a) < pair_key(b);
              });
    require(o.combined_bindings.size() == pairs.size(), scanning,
            DiagnosticReason::unsupported_option,
            "combined plan does not cover pairs");
    for (std::size_t i = 0; i < pairs.size(); ++i)
        require(pair_key(o.combined_bindings[i]) == pair_key(pairs[i]) &&
                    o.combined_bindings[i].target.descriptor_set == 0,
                scanning, DiagnosticReason::unsupported_option,
                "combined plan source or descriptor set mismatch");
    targets.clear();
    for (const auto &b : o.bindings)
        targets.push_back(b.target.binding);
    for (const auto &b : o.combined_bindings)
        targets.push_back(b.target.binding);
    std::sort(targets.begin(), targets.end());
    require(std::adjacent_find(targets.begin(), targets.end()) == targets.end(),
            scanning, DiagnosticReason::unsupported_option,
            "duplicate target binding");
    out.canonical_options_manifest = options_manifest(o);
    out.expected_identity = {s.identity, digest(program.bytes),
                             digest(out.canonical_options_manifest),
                             o.variants};
    if (scan_result)
        *scan_result = std::move(scanned);
    return out;
}
} // namespace eawr::presentation::shader_runtime::detail

namespace eawr::presentation::shader_runtime {
StagePreparation prepare_stage(CompilerSession &session,
                               const assets::EffectProgram &program,
                               const StageOptions &options) {
    try {
        return {
            detail::prepare(StageSessionAccess::get(session), program, options),
            {}};
    } catch (const detail::Failure &failure) {
        return {std::nullopt, {detail::diagnostic(failure)}};
    } catch (const std::bad_alloc &) {
        return {std::nullopt,
                {{DiagnosticPhase::descriptor_scan,
                  DiagnosticReason::budget_exceeded, Severity::error,
                  std::nullopt, "shader allocation failed"}}};
    }
}
} // namespace eawr::presentation::shader_runtime
