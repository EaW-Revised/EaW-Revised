#include "internal.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace eawr::presentation::shader_runtime {
namespace {
using namespace detail;
void backend(bool ok) {
    require(ok, DiagnosticPhase::backend_translate,
            DiagnosticReason::backend_failure, "public GLSL backend failed");
}
void name_uniform_members(Session& session, abi::H compiler, const assets::EffectProgram& program) {
    // Public get_member_name returns an empty string for unnamed SPIR-V
    // members, while GLSL uses a fallback spelling that is not that API's
    // result. Assign an explicit identity before emission and reflection.
    abi::H resources{};
    backend(session.api.spvc_compiler_create_shader_resources(compiler, &resources) == 0 && resources);
    const abi::Resource* list{};
    std::size_t count{};
    backend(session.api.spvc_resources_get_resource_list_for_type(resources, 1, &list, &count) == 0);
    require(count <= session.limits.table_entries && (!count || list), DiagnosticPhase::backend_translate,
        DiagnosticReason::budget_exceeded, "uniform resource naming budget exceeded");
    std::size_t metadata{};
    const auto retain = [&](std::size_t bytes) {
        require(bytes <= session.limits.metadata_bytes - metadata, DiagnosticPhase::backend_translate,
            DiagnosticReason::budget_exceeded, "uniform member naming budget exceeded");
        metadata += bytes;
    };
    for (std::size_t resource = 0U; resource < count; ++resource) {
        const auto id = list[resource].base_type;
        const auto type = session.api.spvc_compiler_get_type_handle(compiler, id);
        backend(type != nullptr);
        const auto members = session.api.spvc_type_get_num_member_types(type);
        require(members <= session.limits.table_entries, DiagnosticPhase::backend_translate,
            DiagnosticReason::budget_exceeded, "uniform member count exceeded");
        std::set<std::string> names;
        std::vector<abi::U> unnamed;
        for (abi::U member = 0U; member < members; ++member) {
            auto name = bounded_text(session.api.spvc_compiler_get_member_name(compiler, id, member),
                session.limits, DiagnosticPhase::backend_translate);
            retain(name.size());
            if (name.empty()) unnamed.push_back(member);
            else names.insert(std::move(name));
        }
        for (const auto member : unnamed) {
            checkpoint(session.limits, DiagnosticPhase::backend_translate);
            auto name = "eawr_block_" + std::to_string(id) + "_member_" + std::to_string(member);
            if (names.contains(name)) throw StageDiagnostic{DiagnosticPhase::backend_translate,
                DiagnosticReason::unsupported_resource, Severity::error,
                DiagnosticLocation{program.source.logical_path, {}, {}, {}, {}}, "uniform member name collision"};
            retain(name.size()); names.insert(name);
            session.api.spvc_compiler_set_member_name(compiler, id, member, name.c_str());
        }
    }
}
using BindingMap = std::map<std::pair<abi::U, abi::U>, SourceRegister>;
SourceRegister register_for(const BindingMap &bindings, abi::Api &api,
                            abi::H compiler, abi::U id) {
    require(api.spvc_compiler_has_decoration(compiler, id, 33) &&
                api.spvc_compiler_has_decoration(compiler, id, 34),
            DiagnosticPhase::backend_translate,
            DiagnosticReason::unsupported_resource,
            "combined sampler lacks source binding");
    const auto set = api.spvc_compiler_get_decoration(compiler, id, 34);
    const auto binding = api.spvc_compiler_get_decoration(compiler, id, 33);
    if (const auto found = bindings.find({set, binding});
        found != bindings.end())
        return found->second;
    refuse(DiagnosticPhase::backend_translate,
           DiagnosticReason::unsupported_resource,
           "combined sampler source binding unknown");
}
StageArtifact translate(Session &s, const assets::EffectProgram &program,
                        const StageOptions &requested) {
    Scan scanned;
    const auto prepared = prepare(s, program, requested, &scanned);
    const auto &options = prepared.resolved_options;
    std::vector<abi::ResourceBinding> bindings;
    std::map<std::tuple<int, abi::U, abi::U>, const abi::Descriptor *>
        descriptors;
    for (const auto &d : scanned.descriptors)
        descriptors.emplace(std::tuple{d.type, d.space, d.index}, &d);
    BindingMap binding_sources;
    for (const auto &b : options.bindings)
        binding_sources.emplace(
            std::pair{b.target.descriptor_set, b.target.binding}, b.source);
    for (const auto &b : options.bindings) {
        const auto kind = b.source.kind;
        const bool bank = kind <= RegisterKind::boolean_constant_bank;
        const int type = bank ? 2 : (kind == RegisterKind::texture ? 0 : 3);
        const auto index =
            bank ? static_cast<std::uint32_t>(kind) : b.source.index;
        const auto descriptor = descriptors.find({type, b.source.space, index});
        require(descriptor != descriptors.end(),
                DiagnosticPhase::descriptor_scan,
                DiagnosticReason::unsupported_resource,
                "resolved descriptor disappeared");
        const auto flags = bank || descriptor->second->resource == 1 ? 1U : 2U;
        bindings.push_back(
            {type,
             b.source.space,
             index,
             program.stage == assets::EffectProgramStage::vertex ? 1 : 5,
             flags,
             {b.target.descriptor_set, b.target.binding, 1}});
    }
    abi::Interface interface_info;
    abi::Target target;
    target.environment = 2;
    interface_info.next = &target;
    interface_info.bindings = bindings.data();
    interface_info.count = static_cast<abi::U>(bindings.size());
    const abi::Option compile_options[]{{4, 0}, {1, 0}};
    abi::Compile info;
    info.source = {program.bytes.data(), program.bytes.size()};
    info.next = &interface_info;
    info.options = compile_options;
    info.option_count = 2;
    checkpoint(s.limits, DiagnosticPhase::stage_compile);
    CompileOutput owner{s.api, info};
    require(owner.result == 0, DiagnosticPhase::stage_compile,
            DiagnosticReason::compiler_failure, "public stage compiler failed");
    checkpoint(s.limits, DiagnosticPhase::stage_compile);
    const auto &compiled = owner.code;
    require(compiled.data && compiled.size >= 20 && compiled.size % 4 == 0,
            DiagnosticPhase::stage_compile, DiagnosticReason::backend_failure,
            "malformed SPIR-V compiler output");
    require(compiled.size / 4 <= s.limits.spirv_words,
            DiagnosticPhase::stage_compile, DiagnosticReason::budget_exceeded,
            "SPIR-V output size refused");
    StageArtifact out;
    out.stage = program.stage;
    out.source_major = program.major;
    out.source_minor = program.minor;
    out.compiled_stage.assign(program.bytes.begin(), program.bytes.end());
    out.spirv.resize(compiled.size / 4);
    std::memcpy(out.spirv.data(), compiled.data, compiled.size);
    validate_spirv(out.spirv, out.stage, s.limits);
    out.identity = prepared.expected_identity;
    out.options_manifest = prepared.canonical_options_manifest;
    abi::H context{};
    backend(s.api.spvc_context_create(&context) == 0 && context);
    struct ContextOwner {
        abi::Api &api;
        abi::H context;
        ~ContextOwner() { api.spvc_context_destroy(context); }
    } context_owner{s.api, context};
    abi::H ir{}, compiler{}, cross_options{};
    backend(s.api.spvc_context_parse_spirv(context, out.spirv.data(),
                                           out.spirv.size(), &ir) == 0 &&
            ir);
    backend(s.api.spvc_context_create_compiler(context, 1, ir, 1, &compiler) ==
                0 &&
            compiler);
    backend(s.api.spvc_compiler_create_compiler_options(compiler,
                                                        &cross_options) == 0 &&
            cross_options);
    const auto has = [&](VariantToken token) {
        return std::find(options.variants.begin(), options.variants.end(),
                         token) != options.variants.end();
    };
    backend(s.api.spvc_compiler_options_set_uint(cross_options, 0x2000008,
                                                 330) == 0);
    for (const auto &[name, value] :
         {std::pair{0x2000009, 0U}, std::pair{0x200000a, 0U},
          std::pair{0x2000007, 0U}, std::pair{0x2000023, 0U},
          std::pair{0x1000003, has(VariantToken::fixup_clip_depth) ? 1U : 0U},
          std::pair{0x1000004, has(VariantToken::flip_vertex_y) ? 1U : 0U}})
        backend(s.api.spvc_compiler_options_set_bool(
                    cross_options, name, static_cast<abi::B>(value)) == 0);
    backend(s.api.spvc_compiler_install_compiler_options(compiler,
                                                         cross_options) == 0);
    backend(s.api.spvc_compiler_build_combined_image_samplers(compiler) == 0);
    const abi::Combined *combined{};
    std::size_t combined_count{};
    backend(s.api.spvc_compiler_get_combined_image_samplers(
                compiler, &combined, &combined_count) == 0);
    require(combined_count <= s.limits.table_entries &&
                (!combined_count || combined),
            DiagnosticPhase::backend_translate,
            DiagnosticReason::budget_exceeded,
            "combined sampler budget exceeded");
    using PairKey =
        std::tuple<RegisterKind, abi::U, abi::U, RegisterKind, abi::U, abi::U>;
    std::map<PairKey, BindingPoint> combined_targets;
    for (const auto &p : options.combined_bindings)
        combined_targets.emplace(PairKey{p.image.kind, p.image.space,
                                         p.image.index, p.sampler.kind,
                                         p.sampler.space, p.sampler.index},
                                 p.target);
    for (std::size_t i = 0; i < combined_count; ++i) {
        const auto image =
            register_for(binding_sources, s.api, compiler, combined[i].image);
        const auto sampler =
            register_for(binding_sources, s.api, compiler, combined[i].sampler);
        const auto found = combined_targets.find(
            PairKey{image.kind, image.space, image.index, sampler.kind,
                    sampler.space, sampler.index});
        require(found != combined_targets.end(),
                DiagnosticPhase::backend_translate,
                DiagnosticReason::unsupported_resource,
                "combined sampler plan mismatch");
        s.api.spvc_compiler_set_decoration(compiler, combined[i].combined, 33,
                                           found->second.binding);
        s.api.spvc_compiler_set_decoration(compiler, combined[i].combined, 34,
                                           found->second.descriptor_set);
    }
    checkpoint(s.limits, DiagnosticPhase::backend_translate);
    name_uniform_members(s, compiler, program);
    const char *text{};
    backend(s.api.spvc_compiler_compile(compiler, &text) == 0 && text);
    checkpoint(s.limits, DiagnosticPhase::backend_translate);
    std::size_t length{};
    while (length < s.limits.glsl_bytes && text[length])
        ++length;
    require(length < s.limits.glsl_bytes, DiagnosticPhase::backend_translate,
            DiagnosticReason::budget_exceeded, "GLSL output budget exceeded");
    out.glsl.assign(text, length);
    out.reflection = reflect(s, compiler, options, scanned);
    validate_artifact(out, s.limits);
    return out;
}
} // namespace
StageResult translate_stage(CompilerSession &session,
                            const assets::EffectProgram &program,
                            const StageOptions &options) {
    try {
        return {translate(StageSessionAccess::get(session), program, options),
                {}};
    } catch (const StageDiagnostic& issue) {
        return {std::nullopt, {issue}};
    } catch (const detail::Failure &failure) {
        return {std::nullopt, {detail::diagnostic(failure)}};
    } catch (const std::bad_alloc &) {
        return {
            std::nullopt,
            {{DiagnosticPhase::stage_compile, DiagnosticReason::budget_exceeded,
              Severity::error, std::nullopt, "shader allocation failed"}}};
    }
}
} // namespace eawr::presentation::shader_runtime
