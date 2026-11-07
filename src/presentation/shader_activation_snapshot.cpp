#include "eawr/presentation/shader_activation_snapshot.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/presentation/shader_effect_source.hpp"
#include "eawr/core/sha256.hpp"
#include <algorithm>
#include <deque>
#include <map>
#include <set>

namespace eawr::presentation::shader_activation {
namespace {
core::Diagnostic problem(std::string message) {
    return {"EAWR-SHADER-ACTIVATION-0001", core::Severity::error, std::move(message), {}, {}, {}, {}};
}
std::string hash(std::span<const std::byte> bytes) {
    return core::sha256_hex({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}
std::string hex(const shader_runtime::Digest& digest) {
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : digest) {
        const auto value = std::to_integer<unsigned>(byte);
        result += alphabet[value >> 4U]; result += alphabet[value & 15U];
    }
    return result;
}
Diagnostic source_diagnostic(const shader_runtime::SourceDiagnostic& source) {
    using Failure = shader_runtime::SourceFailure;
    auto phase = source.phase == shader_runtime::SourcePhase::preprocess ? report::FailureStage::preprocess :
        source.phase == shader_runtime::SourcePhase::stage_compile ? report::FailureStage::stage_compile : report::FailureStage::package_load;
    report::Reason reason;
    switch (source.reason) {
    case Failure::unavailable: reason = report::Reason::dependency_unavailable; break;
    case Failure::dependency_integrity: reason = report::Reason::dependency_integrity; break;
    case Failure::dependency_abi: reason = report::Reason::dependency_abi; break;
    case Failure::unsupported_profile: reason = report::Reason::unsupported_profile; break;
    case Failure::invalid_options: reason = report::Reason::unsupported_option; break;
    case Failure::budget_exceeded: reason = report::Reason::budget_exceeded; break;
    case Failure::cancelled: reason = report::Reason::cancelled; break;
    case Failure::missing_include: reason = report::Reason::dependency_unavailable; break;
    case Failure::compiler_failure: case Failure::preprocess_failure: reason = report::Reason::compile_failure; break;
    default: reason = report::Reason::malformed_effect; break;
    }
    Diagnostic result{phase, reason, report::Severity::error};
    result.logical = source.location.logical_source;
    if (source.location.line) result.position = report::Position{*source.location.line, source.location.column};
    return result;
}
void failure(Input& input, Diagnostic diagnostic) {
    std::vector<std::byte>().swap(input.bytes); input.sha256.clear(); input.context.reset(); input.snapshot_failure = std::move(diagnostic);
}
std::optional<std::size_t> retained_source_bytes(const shader_runtime::PreprocessedSource& source, std::size_t cap) {
    std::size_t retained = 0;
    const auto add = [&](std::size_t count) { if (count > cap - retained) return false; retained += count; return true; };
    if (!add(sizeof(source)) || !add(source.expanded.size())) return {};
    for (const auto& dependency : source.dependencies) {
        if (!add(sizeof(dependency)) || !add(dependency.source.logical_path.size()) ||
            !add(dependency.source.layer_id.size()) || !add(dependency.source.source_id.size())) return {};
    }
    for (const auto& macro : source.macros)
        if (!add(sizeof(macro)) || !add(macro.name.size()) || !add(macro.value.size())) return {};
    for (const auto capability : source.compiler.capabilities) { (void)capability; if (!add(sizeof(capability))) return {}; }
    return retained;
}
}
core::Result<Snapshot> capture(const vfs::Vfs& mounted, std::string chain_identity,
    std::span<const LayerPolicy> layers, std::span<const std::string> reachable,
    const Adapter& adapter, const FreezeSource& freeze_source, const Cancellation& cancellation, SnapshotLimits limits, const Identity& identity) {
    const auto fail = [](std::string message) { return core::Result<Snapshot>::failure(problem(std::move(message))); };
    if (cancellation.cancelled()) return fail("activation snapshot cancelled");
    if (layers.size() > limits.inventory_entries || reachable.size() > limits.inventory_entries ||
        chain_identity.size() > limits.job.identifier_bytes) return fail("activation metadata budget exceeded");
    std::map<std::string, LayerPolicy> policy;
    for (const auto& layer : layers) {
        if (layer.layer.empty() || layer.layer.size() > limits.job.identifier_bytes || !policy.emplace(layer.layer, layer).second)
            return fail("activation layer identity invalid");
    }
    auto compiled = mounted.enumerate({}, ".fxo");
    if (!compiled) return core::Result<Snapshot>::failure(compiled.error());
    if (compiled.value().size() > limits.inventory_entries) return fail("activation inventory budget exceeded");
    auto sources = mounted.enumerate({}, ".fx");
    if (!sources) return core::Result<Snapshot>::failure(sources.error());
    if (sources.value().size() > limits.inventory_entries - compiled.value().size()) return fail("activation inventory budget exceeded");
    std::map<std::string, vfs::AssetRecord> selected;
    for (auto& record : compiled.value()) selected.emplace(record.canonical_path.substr(0, record.canonical_path.size() - 4), std::move(record));
    for (auto& record : sources.value()) {
        const auto layer = policy.find(record.layer_id);
        if (layer == policy.end()) return fail("activation winner has no captured contributor policy");
        if (layer->second.mod) selected.emplace(record.canonical_path.substr(0, record.canonical_path.size() - 3), std::move(record));
    }
    std::set<std::string> reachable_set;
    for (const auto& stem : reachable) {
        if (stem.size() > limits.job.identifier_bytes) return fail("activation reachable identity budget exceeded");
        auto canonical = vfs::canonicalize(stem);
        if (!canonical) return core::Result<Snapshot>::failure(canonical.error());
        reachable_set.insert(std::move(canonical).value());
    }
    if (selected.size() > limits.job.effects) return fail("activation effect budget exceeded");
    Snapshot snapshot; snapshot.chain_identity = std::move(chain_identity);
    std::size_t source_bytes = 0;
    for (const auto& [stem, record] : selected) {
        if (cancellation.cancelled()) return fail("activation snapshot cancelled");
        const auto layer = policy.find(record.layer_id);
        if (layer == policy.end()) return fail("activation winner has no captured contributor policy");
        for (const auto* identifier : {&record.canonical_path, &record.layer_id, &record.source_id})
            if (identifier->size() > limits.job.identifier_bytes) return fail("activation identifier budget exceeded");
        Input input; input.effect = stem; input.layer = record.layer_id; input.mount = record.source_id;
        input.origin = std::string(vfs::to_string(record.origin));
        input.logical = record.canonical_path; input.policy = layer->second.policy;
        input.form = record.canonical_path.ends_with(".fxo") ? report::SelectedForm::compiled : report::SelectedForm::source;
        if (input.form == report::SelectedForm::compiled) ++snapshot.compiled_inventory; else ++snapshot.source_inventory;
        if (reachable_set.contains(stem)) ++snapshot.reachable;
        if (adapter) input.adapter = adapter(stem);
        if (input.adapter.size() > limits.job.identifier_bytes) return fail("activation adapter identity budget exceeded");
        if (input.policy == Policy::allowed) {
            const auto available = limits.job.input_bytes - snapshot.input_bytes;
            if (record.size > available) failure(input, {report::FailureStage::stage_input, report::Reason::budget_exceeded, report::Severity::error});
            else {
                // Prefix acquisition caps the read even if a loose file grows
                // after mount. Compare sizes to refuse a changed mounted entry.
                auto bytes = mounted.open_prefix(record.canonical_path, record.size);
                if (!bytes || bytes.value().size() != record.size)
                    failure(input, {report::FailureStage::stage_input, report::Reason::malformed_effect, report::Severity::error});
                else {
                    input.bytes = std::move(bytes).value(); input.sha256 = hash(input.bytes);
                    snapshot.input_bytes += input.bytes.size();
                    if (input.form == report::SelectedForm::source) {
                        if (!freeze_source) failure(input, {report::FailureStage::package_load, report::Reason::dependency_unavailable, report::Severity::error});
                        else {
                            auto frozen = freeze_source(record, cancellation);
                            snapshot.source_opens += frozen.opened;
                            if (cancellation.cancelled() || (frozen.diagnostic && frozen.diagnostic->reason == shader_runtime::SourceFailure::cancelled))
                                return fail("activation snapshot cancelled");
                            if (!frozen.source) {
                                failure(input, frozen.diagnostic ? source_diagnostic(*frozen.diagnostic) :
                                    Diagnostic{report::FailureStage::preprocess, report::Reason::compile_failure, report::Severity::error});
                            } else {
                                auto& source = *frozen.source;
                                const auto& dependencies = source.dependencies;
                                const auto retained = retained_source_bytes(source, limits.source_bytes - source_bytes);
                                const bool root_matches = !dependencies.empty() && dependencies.front().source.logical_path == input.logical &&
                                    dependencies.front().source.layer_id == input.layer && dependencies.front().source.source_id == input.mount &&
                                    dependencies.front().source.origin == record.origin &&
                                    // Digest hex is an encoding, not another SHA operation.
                                    hex(dependencies.front().sha256) == input.sha256;
                                if (!root_matches || hex(source.expanded_sha256) != hash(source.expanded))
                                    failure(input, {report::FailureStage::preprocess, report::Reason::malformed_effect, report::Severity::error});
                                else if (!retained)
                                    failure(input, {report::FailureStage::preprocess, report::Reason::budget_exceeded, report::Severity::error});
                                else {
                                    source_bytes += *retained;
                                    auto owned = std::make_shared<SourceSnapshot>(); owned->source = std::move(source); input.context = std::move(owned);
                                }
                            }
                        }
                    }
                }
            }
        }
        if (identity && input.policy == Policy::allowed && !input.snapshot_failure) {
            auto requested = identity(input);
            if (!requested)
                failure(input, {report::FailureStage::selection, report::Reason::unsupported_option, report::Severity::error});
            else {
                Diagnostic refusal{report::FailureStage::selection, report::Reason::unsupported_option, report::Severity::error};
                input.cache_key = activation_key(input, std::move(*requested), &refusal);
                if (!input.cache_key) failure(input, std::move(refusal));
            }
        }
        snapshot.inputs.push_back(std::move(input));
    }
    for (const auto& stem : reachable_set) if (!selected.contains(stem)) {
        if (snapshot.inputs.size() == limits.job.effects) return fail("activation effect budget exceeded");
        Input input; input.effect = stem; input.logical = stem; input.form = report::SelectedForm::absent;
        snapshot.inputs.push_back(std::move(input)); ++snapshot.reachable;
    }
    if (cancellation.cancelled()) return fail("activation snapshot cancelled");
    return core::Result<Snapshot>::success(std::move(snapshot));
}
std::optional<shader_cache::Key> activation_key(const Input& input, shader_cache::Identity identity, Diagnostic* refusal) {
    if (input.policy != Policy::allowed || input.snapshot_failure || input.sha256.empty()) return {};
    identity.logical = input.logical; identity.root_sha256 = input.sha256;
    identity.root_layer = input.layer; identity.root_source = input.mount;
    identity.root_origin = input.origin;
    identity.selection_policy = "fxo-first/1";
    if (input.form == report::SelectedForm::source) {
        const auto source = std::dynamic_pointer_cast<const SourceSnapshot>(input.context);
        if (!source || source->source.dependencies.empty()) return {};
        std::vector<assets::Source> inputs;
        for (const auto& dependency : source->source.dependencies) inputs.push_back(dependency.source);
        auto parsed = shader_source::parse(source->source.expanded, inputs.front(), inputs);
        if (!parsed.graph) {
            if (refusal && parsed.diagnostic) {
                refusal->stage = report::FailureStage::effect_parse;
                refusal->reason = parsed.diagnostic->reason == shader_source::Failure::budget ? report::Reason::budget_exceeded :
                    parsed.diagnostic->reason == shader_source::Failure::unsupported ? report::Reason::unsupported_option : report::Reason::malformed_effect;
                refusal->logical = parsed.diagnostic->location.logical;
                if (parsed.diagnostic->location.line) refusal->position = report::Position{*parsed.diagnostic->location.line, parsed.diagnostic->location.column};
            }
            return {};
        }
        shader_source::Selection selected; selected.form = shader_source::Form::source; selected.allowed = true;
        vfs::AssetRecord record; record.canonical_path = input.logical; record.layer_id = input.layer; record.source_id = input.mount;
        record.origin = source->source.dependencies.front().source.origin;
        selected.record = std::move(record);
        if (!shader_source::source_identity(identity, selected, source->source, *parsed.graph)) return {};
    } else if (input.form == report::SelectedForm::compiled) identity.form = shader_cache::Form::compiled;
    else return {};
    return shader_cache::make_key(identity);
}
Operations cached_operations(Operations uncached, std::shared_ptr<const shader_cache::Cache> cache, CacheCodec codec) {
    Operations result; result.admit = std::move(uncached.admit);
    result.prepare = [fresh = std::move(uncached.prepare), cache = std::move(cache), codec = std::move(codec)]
        (const Input& input, const Cancellation& cancellation) {
        Prepared prepared;
        if (cancellation.cancelled() || input.policy != Policy::allowed) return prepared;
        bool cache_warning = false;
        const bool caching = cache && input.cache_key && codec.validate && codec.decode && codec.encode;
        if (caching) {
            auto cached = cache->lookup(*input.cache_key, true, codec.validate);
            if (cancellation.cancelled()) return prepared;
            if (cached.status == shader_cache::Status::hit) {
                prepared = codec.decode(cached.payload);
                if (prepared.artifact && !prepared.variants.empty()) return prepared;
                cache_warning = true;
            } else cache_warning = cached.status != shader_cache::Status::miss;
        }
        if (fresh) prepared = fresh(input, cancellation);
        if (cancellation.cancelled()) return Prepared{};
        if (cache_warning) prepared.diagnostics.push_back({report::FailureStage::cache_read, report::Reason::cache_failure, report::Severity::warning});
        if (caching && prepared.artifact && !prepared.variants.empty()) {
            const auto bytes = codec.encode(prepared);
            if (cancellation.cancelled()) return Prepared{};
            if (cache->store(*input.cache_key, true, bytes, codec.validate) != shader_cache::Status::stored)
                prepared.diagnostics.push_back({report::FailureStage::cache_write, report::Reason::cache_failure, report::Severity::warning});
        }
        return prepared;
    };
    return result;
}
FreezeSource source_freezer(shader_runtime::CompilerSession& session, const vfs::Vfs& mounted,
    std::string logical_root, std::vector<shader_runtime::SourceMacro> macros, shader_runtime::SourceLimits limits) {
    return [&session, &mounted, root = std::move(logical_root), macros = std::move(macros), limits]
        (const vfs::AssetRecord& record, const Cancellation& cancellation) {
        if (cancellation.cancelled()) {
            shader_runtime::SourcePreparation result;
            result.diagnostic = shader_runtime::SourceDiagnostic{shader_runtime::SourcePhase::preprocess, shader_runtime::SourceFailure::cancelled, {}};
            return result;
        }
        assets::ShaderSourceProvider provider(mounted, root);
        return shader_runtime::preprocess_source(session, provider, record.canonical_path, macros, limits);
    };
}
std::optional<Completion> complete(Publisher& publisher, const Job& job, report::Limits limits) {
    const auto generation = job.result();
    if (!generation) return std::nullopt;
    std::vector<report::EffectView> views;
    std::vector<std::vector<report::DiagnosticView>> rows;
    std::deque<std::string> names;
    const auto project = [&](const EffectResult& effect) {
        auto& projected = rows.emplace_back();
        const auto diagnostic = [&](const Diagnostic& item, std::string_view technique, std::string_view state) {
            report::DiagnosticView view{item.stage, item.reason, item.severity, {}};
            view.context.technique = technique; view.context.pass = state;
            if (item.position) view.context.location = {report::CoordinateSpace::source_text, item.logical, item.position, {}};
            projected.push_back(view);
        };
        for (const auto& item : effect.diagnostics) diagnostic(item, {}, {});
        auto selected_route = effect.variants.empty() ? effect.fallback : report::Route::translated;
        for (const auto& variant : effect.variants) {
            names.push_back("technique-id:" + std::to_string(variant.variant.technique));
            const auto& technique = names.back();
            names.push_back("state-variant-id:" + std::to_string(variant.variant.state));
            const auto& state = names.back();
            for (const auto& item : variant.diagnostics) diagnostic(item, technique, state);
            // One report count per effect. A partially admitted effect needs
            // the fallback note; exact routes remain on every VariantResult.
            if (variant.route != report::Route::translated) selected_route = effect.fallback;
        }
        const auto& input = effect.input;
        const auto sha = input.sha256.empty() ? std::optional<std::string_view>{} : std::optional<std::string_view>(input.sha256);
        views.push_back({input.effect, sha, {input.layer, input.mount, input.logical, input.form},
            selected_route, input.adapter, {}, projected});
    };
    for (const auto& effect : generation->effects) project(effect);
    auto result = report::build(views, limits);
    if (!publisher.publish(job)) return std::nullopt;
    return Completion{generation, std::move(result)};
}
} // namespace eawr::presentation::shader_activation
