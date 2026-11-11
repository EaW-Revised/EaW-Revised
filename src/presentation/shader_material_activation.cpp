#include "eawr/presentation/shader_material_activation.hpp"
#include "eawr/core/sha256.hpp"

#include <algorithm>

namespace eawr::presentation::shader_material {
struct MemoryBudget final { std::atomic<std::size_t> bytes{}; };
namespace {
namespace activation = shader_activation;
namespace runtime = shader_runtime;
namespace report = shader_report;
struct Prepared final : activation::Artifact {
    std::vector<std::pair<activation::Variant, Program>> programs;
};
struct Charge final {
    std::shared_ptr<MemoryBudget> budget;
    std::size_t bytes{};
    ~Charge() { budget->bytes.fetch_sub(bytes); }
};
activation::Diagnostic issue(report::FailureStage stage, report::Reason reason) {
    return {stage, reason, report::Severity::error};
}
std::string hex(const runtime::Digest& value) {
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : value) {
        const auto n = std::to_integer<unsigned>(byte); result += alphabet[n >> 4U]; result += alphabet[n & 15U];
    }
    return result;
}
shader_cache::Key key(std::string_view domain, std::string_view base, std::string_view extra) {
    // Every variable component has an explicit length; names cannot alias
    // stage/profile boundaries. The outer key binds the freshly frozen input.
    const auto text = std::string(domain) + ":" + std::to_string(base.size()) + ":" + std::string(base)
        + ":" + std::to_string(extra.size()) + ":" + std::string(extra);
    return {core::sha256_hex({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()})};
}
std::size_t stage_bytes(const runtime::StageArtifact& artifact) {
    const auto& reflection = artifact.reflection;
    std::size_t bytes = sizeof(artifact) + artifact.glsl.capacity() + artifact.compiled_stage.capacity()
        + artifact.spirv.capacity() * sizeof(std::uint32_t) + artifact.options_manifest.capacity()
        + artifact.identity.selected_variants.capacity() * sizeof(runtime::VariantToken)
        + artifact.identity.compiler.capabilities.capacity() * sizeof(runtime::CapabilityToken)
        + reflection.types.capacity() * sizeof(runtime::ReflectedType)
        + reflection.resources.capacity() * sizeof(runtime::ReflectedResource)
        + reflection.interfaces.capacity() * sizeof(runtime::ReflectedInterface)
        + reflection.combined_samplers.capacity() * sizeof(runtime::CombinedSampler);
    for (const auto& type : reflection.types) {
        bytes += type.array_dimensions.capacity() * sizeof(runtime::ArrayDimension) + type.members.capacity() * sizeof(runtime::TypeMember);
        for (const auto& member : type.members) bytes += member.name.capacity();
    }
    for (const auto& resource : reflection.resources) bytes += resource.source_name.capacity() + resource.emitted_name.capacity();
    for (const auto& port : reflection.interfaces) {
        bytes += port.source_name.capacity() + port.emitted_name.capacity() + port.member_path.capacity() * sizeof(std::uint32_t);
        if (port.source.semantic) bytes += port.source.semantic->name.capacity();
    }
    return bytes;
}
runtime::StageOptions options() {
    runtime::StageOptions result;
    result.required_capabilities = {runtime::CapabilityToken::d3d_bytecode_to_spirv,
        runtime::CapabilityToken::spirv_to_glsl_330, runtime::CapabilityToken::typed_resource_reflection,
        runtime::CapabilityToken::typed_interface_reflection, runtime::CapabilityToken::combined_sampler_relationships};
    result.variants = {runtime::VariantToken::desktop_glsl_330, runtime::VariantToken::preserve_clip_depth,
        runtime::VariantToken::preserve_vertex_y, runtime::VariantToken::uniform_blocks,
        runtime::VariantToken::combine_image_samplers};
    return result;
}
activation::Diagnostic stage_issue(const runtime::StageResult& result) {
    if (result.diagnostics.empty()) return issue(report::FailureStage::backend_translate, report::Reason::backend_failure);
    const auto& diagnostic = result.diagnostics.front();
    const auto where = diagnostic.phase == runtime::DiagnosticPhase::package_load ? report::FailureStage::package_load
        : diagnostic.phase == runtime::DiagnosticPhase::stage_input ? report::FailureStage::stage_input
        : diagnostic.phase == runtime::DiagnosticPhase::descriptor_scan ? report::FailureStage::descriptor_scan
        : diagnostic.phase == runtime::DiagnosticPhase::stage_compile ? report::FailureStage::stage_compile
        : diagnostic.phase == runtime::DiagnosticPhase::reflection ? report::FailureStage::reflection
        : diagnostic.phase == runtime::DiagnosticPhase::cache_read ? report::FailureStage::cache_read : report::FailureStage::backend_translate;
    report::Reason reason = report::Reason::backend_failure;
    switch (diagnostic.reason) {
    case runtime::DiagnosticReason::dependency_unavailable: reason = report::Reason::dependency_unavailable; break;
    case runtime::DiagnosticReason::dependency_integrity: reason = report::Reason::dependency_integrity; break;
    case runtime::DiagnosticReason::dependency_abi: reason = report::Reason::dependency_abi; break;
    case runtime::DiagnosticReason::unsupported_profile: reason = report::Reason::unsupported_profile; break;
    case runtime::DiagnosticReason::unsupported_interface: reason = report::Reason::unsupported_interface; break;
    case runtime::DiagnosticReason::unsupported_resource: reason = report::Reason::unsupported_resource; break;
    case runtime::DiagnosticReason::unsupported_option: reason = report::Reason::unsupported_option; break;
    case runtime::DiagnosticReason::malformed_stage: reason = report::Reason::malformed_stage; break;
    case runtime::DiagnosticReason::invalid_shader:
    case runtime::DiagnosticReason::compiler_failure: reason = report::Reason::compile_failure; break;
    case runtime::DiagnosticReason::reflection_failure: reason = report::Reason::reflection_failure; break;
    case runtime::DiagnosticReason::invalid_cached_result: reason = report::Reason::cache_failure; break;
    case runtime::DiagnosticReason::budget_exceeded: reason = report::Reason::budget_exceeded; break;
    case runtime::DiagnosticReason::cancelled: reason = report::Reason::cancelled; break;
    case runtime::DiagnosticReason::backend_failure: break;
    }
    return issue(where, reason);
}
runtime::StageResult invalid_shader_refusal() {
    return {{}, {{runtime::DiagnosticPhase::stage_compile, runtime::DiagnosticReason::invalid_shader,
        runtime::Severity::error, {}, "public stage compiler failed"}}};
}
bool deterministic_refusal(const runtime::StageResult& result) {
    if (result.artifact || result.diagnostics.size() != 1U) return false;
    const auto& diagnostic = result.diagnostics.front();
    return diagnostic.phase == runtime::DiagnosticPhase::stage_compile
        && diagnostic.reason == runtime::DiagnosticReason::invalid_shader
        && diagnostic.severity == runtime::Severity::error && !diagnostic.location
        && diagnostic.detail == "public stage compiler failed";
}
// A single canonical typed refusal, with no compiler text or variable fields.
// Schema/domain changes are required before another reason can be persisted.
constexpr std::string_view refusal_payload = "eawr-stage-refusal/1:stage_compile:invalid_shader:error";
bool validate_refusal(std::span<const std::uint8_t> bytes) {
    return bytes.size() == refusal_payload.size()
        && std::equal(bytes.begin(), bytes.end(), refusal_payload.begin());
}
} // namespace
shader_cache::Key source_stage_cache_key(const shader_cache::Key& input, std::string_view entry,
    std::string_view profile, const runtime::CompilerIdentity& compiler) {
    return key("material-source-stage/3", input.sha256,
        std::to_string(entry.size()) + ":" + std::string(entry) + ":" + std::string(profile)
        + ":" + std::string(runtime::source_compiler_contract()) + ":"
        + hex(compiler.package_lock_sha256) + hex(compiler.capability_manifest_sha256)
        + hex(compiler.public_c_abi.manifest_sha256) + ":" + std::to_string(compiler.result_schema_version)
        + ":" + std::to_string(compiler.options_manifest_version));
}
Compiler::Compiler(runtime::CompilerSession session, std::shared_ptr<const shader_cache::Cache> cache,
    Prewarm prewarm, shader_techniques::Settings settings, ActivationLimits limits)
    : session_(std::move(session)), cache_(std::move(cache)), prewarm_(std::move(prewarm)), settings_(settings),
      limits_{std::min(limits.stages, ActivationLimits{}.stages), std::min(limits.programs, ActivationLimits{}.programs),
          std::min(limits.prepared, ActivationLimits{}.prepared), std::min(limits.resident, ActivationLimits{}.resident)},
      budget_(std::make_shared<MemoryBudget>()) {}

runtime::StageResult Compiler::stage(const activation::Input& input, const assets::EffectProgram& program,
    const activation::Cancellation& cancellation) {
    if (cancellation.cancelled()) return {{}, {{runtime::DiagnosticPhase::stage_input,
        runtime::DiagnosticReason::cancelled, runtime::Severity::error, {}, {}}}};
    const auto prepared = runtime::prepare_stage(session_, program, options());
    if (!prepared.request) return {{}, prepared.diagnostics};
    const auto& current = *prepared.request;
    const auto& compiler = current.expected_identity.compiler;
    const auto identity = hex(current.expected_identity.input_stage_sha256) + hex(current.expected_identity.options_manifest_sha256)
        + hex(compiler.package_lock_sha256) + hex(compiler.capability_manifest_sha256)
        + hex(compiler.public_c_abi.manifest_sha256) + ":" + std::to_string(compiler.result_schema_version)
        + ":" + std::to_string(compiler.options_manifest_version);
    const auto cache_key = input.cache_key ? std::optional{key("material-stage/1", input.cache_key->sha256, identity)} : std::nullopt;
    const auto refusal_key = input.cache_key ? std::optional{key("material-stage-refusal/1", input.cache_key->sha256, identity)} : std::nullopt;
    const auto validate = [&](std::span<const std::uint8_t> bytes) {
        return runtime::decode_stage_result(std::as_bytes(bytes), current).artifact.has_value();
    };
    if (cache_ && cache_key) {
        const auto cached = cache_->lookup(*cache_key, true, validate);
        if (cached.status == shader_cache::Status::hit) {
            auto decoded = runtime::decode_stage_result(std::as_bytes(std::span(cached.payload)), current);
            if (decoded.artifact) { ++work_.cache_hits; return decoded; }
        }
        const auto refused = cache_->lookup(*refusal_key, true, validate_refusal);
        if (refused.status == shader_cache::Status::hit && !cancellation.cancelled()) {
            ++work_.cache_hits; return invalid_shader_refusal();
        }
    }
    if (cancellation.cancelled()) return {{}, {{runtime::DiagnosticPhase::stage_input,
        runtime::DiagnosticReason::cancelled, runtime::Severity::error, {}, {}}}};
    ++work_.translations;
    auto translated = runtime::translate_stage(session_, program, current.resolved_options);
    if (translated.artifact && cache_ && cache_key && !cancellation.cancelled()) {
        const auto encoded = runtime::encode_stage_result(*translated.artifact);
        if (encoded.bytes) static_cast<void>(cache_->store(*cache_key, true,
            {reinterpret_cast<const std::uint8_t*>(encoded.bytes->data()), encoded.bytes->size()}, validate));
    } else if (cache_ && refusal_key && deterministic_refusal(translated) && !cancellation.cancelled()) {
        static_cast<void>(cache_->store(*refusal_key, true,
            {reinterpret_cast<const std::uint8_t*>(refusal_payload.data()), refusal_payload.size()}, validate_refusal));
    }
    return translated;
}

activation::Prepared Compiler::prepare(const activation::Input& input, const activation::Cancellation& cancellation) {
    wrapper_refusal_.reset();
    if (cancellation.cancelled()) return {{}, {}, {issue(report::FailureStage::selection, report::Reason::cancelled)}};
    if (input.policy != activation::Policy::allowed) return {{}, {}, {issue(report::FailureStage::selection, report::Reason::opted_out)}};
    // Validate this activation's dependency before either source-stage or
    // backend-stage cache access. Cached bytecode never substitutes for the
    // currently installed and verified compiler package.
    const auto compiler_identity = runtime::compiler_identity(session_);
    if (!compiler_identity)
        return {{}, {}, {issue(report::FailureStage::package_load, report::Reason::dependency_unavailable)}};
    std::optional<shader_source::Effect> source_effect;
    std::optional<assets::EffectView> compiled_effect;
    if (input.form == report::SelectedForm::compiled) {
        auto parsed = assets::read_compiled_effect(input.bytes, {.logical_path = input.logical, .source_id = {}, .layer_id = {}});
        if (!parsed) return {{}, {}, {issue(report::FailureStage::effect_parse, report::Reason::malformed_effect)}};
        compiled_effect = std::move(parsed).value();
    } else if (input.form == report::SelectedForm::source) {
        const auto snapshot = std::dynamic_pointer_cast<const activation::SourceSnapshot>(input.context);
        if (!snapshot || snapshot->source.dependencies.empty())
            return {{}, {}, {issue(report::FailureStage::preprocess, report::Reason::malformed_effect)}};
        std::vector<assets::Source> inputs;
        for (const auto& dependency : snapshot->source.dependencies) inputs.push_back(dependency.source);
        auto graph = shader_source::parse(snapshot->source.expanded, inputs.front(), inputs);
        if (!graph.graph) return {{}, {}, {issue(report::FailureStage::effect_parse, report::Reason::malformed_effect)}};
        auto produced = shader_source::produce(std::move(*graph.graph), [&](std::span<const std::byte> text,
            std::string_view entry, std::string_view profile) {
            if (cancellation.cancelled()) return runtime::SourceCompilation{{}, runtime::SourceDiagnostic{
                runtime::SourcePhase::stage_compile, runtime::SourceFailure::cancelled, {}}};
            const auto cache_key = input.cache_key ? std::optional{source_stage_cache_key(*input.cache_key, entry, profile, *compiler_identity)} : std::nullopt;
            const auto validate = [&](std::span<const std::uint8_t> bytes) {
                if (bytes.size() < 40U) return false;
                const auto program = assets::read_shader_program(std::as_bytes(bytes.subspan(32U)));
                return program && ((program.value().stage == assets::EffectProgramStage::vertex) == profile.starts_with("vs_"));
            };
            if (cache_ && cache_key) {
                auto cached = cache_->lookup(*cache_key, true, validate);
                if (cached.status == shader_cache::Status::hit) {
                    runtime::SourceStage result;
                    std::copy_n(reinterpret_cast<const std::byte*>(cached.payload.data()), 32U, result.manifest_sha256.begin());
                    result.bytecode.assign(reinterpret_cast<const std::byte*>(cached.payload.data()) + 32U,
                        reinterpret_cast<const std::byte*>(cached.payload.data()) + cached.payload.size());
                    result.source = snapshot->source.dependencies.front().source;
                    result.source.stored_size = result.bytecode.size();
                    result.entry = entry; result.profile = profile;
                    ++work_.cache_hits;
                    return runtime::SourceCompilation{std::move(result), {}};
                }
            }
            ++work_.source_compilations;
            auto compiled = runtime::compile_source_stage(session_, snapshot->source, text, entry, profile);
            if (compiled.stage && cache_ && cache_key && !cancellation.cancelled()) {
                std::vector<std::uint8_t> payload;
                for (const auto byte : compiled.stage->manifest_sha256) payload.push_back(std::to_integer<std::uint8_t>(byte));
                for (const auto byte : compiled.stage->bytecode) payload.push_back(std::to_integer<std::uint8_t>(byte));
                static_cast<void>(cache_->store(*cache_key, true, payload, validate));
            }
            return compiled;
        });
        if (!produced.effect) {
            const bool profile = produced.diagnostic && produced.diagnostic->compiler
                && produced.diagnostic->compiler->reason == runtime::SourceFailure::unsupported_profile;
            return {{}, {}, {issue(report::FailureStage::stage_compile, cancellation.cancelled() ? report::Reason::cancelled
                : profile ? report::Reason::unsupported_profile : report::Reason::compile_failure)}};
        }
        source_effect = std::move(produced.effect);
    } else return {{}, {}, {issue(report::FailureStage::selection, report::Reason::missing_effect)}};
    const auto& effect = source_effect ? source_effect->view() : *compiled_effect;
    if (effect.techniques.size() > 1024U || effect.programs.size() > 4096U)
        return {{}, {}, {issue(report::FailureStage::technique_admission, report::Reason::budget_exceeded)}};
    std::vector<std::optional<runtime::StageArtifact>> stages(effect.programs.size());
    std::vector<bool> attempted(effect.programs.size());
    std::vector<std::optional<Program>> programs(effect.techniques.size());
    std::vector<shader_techniques::CandidateValidation> validation(effect.techniques.size());
    std::vector<activation::Diagnostic> diagnostics;
    std::size_t retained_stages{}, retained_programs{};
    for (std::size_t t = 0U; t < effect.techniques.size(); ++t) {
        if (cancellation.cancelled()) return {{}, {}, {issue(report::FailureStage::technique_admission, report::Reason::cancelled)}};
        const auto flow = shader_techniques::make_pass_flow(effect, t, {.depth_attachment = true});
        if (!flow || flow.value().requires_cleanup_qualification || flow.value().passes.size() != 1U) continue;
        const auto& pass = flow.value().passes.front();
        if (!pass.vertex || !pass.pixel || !pass.vertex->descriptor || !pass.pixel->descriptor) continue;
        const auto v = *pass.vertex->descriptor, f = *pass.pixel->descriptor;
        if (v >= stages.size() || f >= stages.size()) continue;
        for (const auto index : {v, f}) if (!attempted[index]) {
            attempted[index] = true;
            auto result = stage(input, effect.programs[index], cancellation);
            if (result.artifact) {
                const auto bytes = stage_bytes(*result.artifact);
                if (bytes > limits_.stages - retained_stages) {
                    if (diagnostics.size() < 1024U) diagnostics.push_back(issue(report::FailureStage::backend_translate, report::Reason::budget_exceeded));
                } else { retained_stages += bytes; stages[index] = std::move(result.artifact); }
            }
            else if (diagnostics.size() < 1024U) diagnostics.push_back(stage_issue(result));
        }
        if (!stages[v] || !stages[f]) continue;
        auto result = assemble(effect, *stages[v], *stages[f], pass);
        if (!result) {
            wrapper_refusal_ = result.error();
            const auto& code = result.error().code;
            const auto reason = code == diagnostic_codes::semantic ? report::Reason::unsupported_semantic
                : code == diagnostic_codes::state ? report::Reason::unsupported_state
                : code == diagnostic_codes::resource ? report::Reason::unsupported_resource
                : code == diagnostic_codes::limit ? report::Reason::budget_exceeded : report::Reason::unsupported_interface;
            if (diagnostics.size() < 1024U) diagnostics.push_back(issue(report::FailureStage::technique_admission, reason));
            continue;
        }
        programs[t] = std::move(result).value();
        // Activation retains the selected technique/pass in its Variant.
        // The assembled modern material must keep legacy selectors empty.
        if (programs[t]->retained_bytes > limits_.programs - retained_programs) {
            programs[t].reset();
            diagnostics.push_back(issue(report::FailureStage::technique_admission, report::Reason::budget_exceeded)); break;
        }
        retained_programs += programs[t]->retained_bytes;
        validation[t].regular = shader_techniques::Validation::compatible;
    }
    auto prepared = std::make_shared<Prepared>();
    std::vector<activation::Variant> variants;
    std::size_t prepared_bytes{};
    for (std::size_t attempt = 0U; attempt < programs.size(); ++attempt) {
        const auto selected = shader_techniques::select_technique(effect, settings_, validation);
        if (!selected || !selected.value().technique) break;
        const auto t = *selected.value().technique;
        if (!programs[t]) break;
        const auto bytes = programs[t]->retained_bytes;
        if (bytes > limits_.prepared - prepared_bytes) {
            diagnostics.push_back(issue(report::FailureStage::technique_admission, report::Reason::budget_exceeded)); break;
        }
        prepared_bytes += bytes;
        const activation::Variant variant{t, 0U};
        variants.push_back(variant); prepared->programs.emplace_back(variant, std::move(*programs[t]));
        validation[t].regular = validation[t].software = shader_techniques::Validation::device;
    }
    if (variants.empty()) {
        diagnostics.push_back(issue(report::FailureStage::technique_admission, report::Reason::admission_failure));
        return {{}, {}, std::move(diagnostics)};
    }
    return {prepared, std::move(variants), {}};
}
activation::Admitted Compiler::admit(const activation::Input&, const activation::Prepared& prepared,
    activation::Variant variant, const activation::Cancellation& cancellation) {
    if (cancellation.cancelled()) return {{}, {issue(report::FailureStage::godot_admission, report::Reason::cancelled)}};
    const auto programs = std::dynamic_pointer_cast<const Prepared>(prepared.artifact);
    if (!programs || !prewarm_) return {};
    const auto program = std::find_if(programs->programs.begin(), programs->programs.end(),
        [&](const auto& item) { return item.first == variant; });
    if (program == programs->programs.end()) return {};
    const auto resident_limit = limits_.resident;
    const auto bytes = program->second.retained_bytes;
    auto charge = std::make_shared<Charge>(); charge->budget = budget_;
    auto used = budget_->bytes.load();
    do {
        if (used > resident_limit || bytes > resident_limit - used)
            return {{}, {issue(report::FailureStage::godot_admission, report::Reason::budget_exceeded)}};
    } while (!budget_->bytes.compare_exchange_weak(used, used + bytes));
    charge->bytes = bytes;
    ++work_.admissions;
    auto device = prewarm_(program->second);
    if (!device || cancellation.cancelled()) return {};
    ++work_.prewarms;
    auto resident = std::make_shared<Resident>();
    resident->program = program->second; resident->device = std::move(device); resident->charge = std::move(charge);
    return {std::move(resident), {}};
}
activation::Operations Compiler::operations() {
    return {[this](const auto& input, const auto& cancellation) { return prepare(input, cancellation); },
        [this](const auto& input, const auto& prepared, auto variant, const auto& cancellation) {
            return admit(input, prepared, variant, cancellation);
        }};
}
std::size_t Compiler::resident_bytes() const noexcept { return budget_->bytes.load(); }
const Resident* find(const activation::Generation& generation, std::string_view effect) noexcept {
    const auto found = std::lower_bound(generation.effects.begin(), generation.effects.end(), effect,
        [](const auto& item, std::string_view name) { return item.input.effect < name; });
    if (found == generation.effects.end() || found->input.effect != effect) return nullptr;
    for (const auto& variant : found->variants) if (variant.route == report::Route::translated)
        if (const auto* resident = dynamic_cast<const Resident*>(variant.artifact.get())) return resident;
    return nullptr;
}
} // namespace eawr::presentation::shader_material
