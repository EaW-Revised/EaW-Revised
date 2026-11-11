#include "eawr/presentation/shader_material_activation.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/core/sha256.hpp"
#include "../../../apps/viewer/src/shader_prewarm_diagnostics.hpp"
#include "../../../src/presentation/shader_stage/internal.hpp"
#include "../../support/check.hpp"
#include "../../support/temp_dir.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <utility>

namespace {
using namespace eawr;
namespace material = presentation::shader_material;
namespace activation = presentation::shader_activation;
namespace runtime = presentation::shader_runtime;
testing::Checker checks;
void check(bool condition, std::string_view message) { checks.check(condition, message, std::cerr); }
struct Device final : activation::Artifact {};
// Synthetic recognized PCI vendor; this is not the build host's adapter.
constexpr presentation::shader_techniques::Settings desktop_settings{3U, 2U, 0x10deU, false};
std::vector<std::byte> refusal_effect() {
    // EFX-02/08: authored object slots and shader words, independent of game data.
    using Bytes = std::vector<std::byte>;
    const auto append = [](Bytes& bytes, const std::initializer_list<std::uint32_t> words) {
        for (const auto word : words) for (unsigned shift = 0U; shift < 32U; shift += 8U)
            bytes.push_back(static_cast<std::byte>((word >> shift) & 255U));
    };
    Bytes pool;
    append(pool, {0U});
    const auto add = [&](const std::initializer_list<std::uint32_t> words) {
        const auto offset = static_cast<std::uint32_t>(pool.size());
        append(pool, words); return offset;
    };
    const auto vertex = add({16U, 4U, 0U, 0U, 0U});
    const auto pixel = add({15U, 4U, 0U, 0U, 0U});
    const auto one = add({1U}), two = add({2U});
    Bytes bytes;
    append(bytes, {0xfeff0901U, static_cast<std::uint32_t>(pool.size())});
    bytes.insert(bytes.end(), pool.begin(), pool.end());
    append(bytes, {0U, 1U, 0U, 3U, 0U, 0U, 1U, 0U, 0U, 2U,
        146U, 0U, vertex, one, 147U, 0U, pixel, two, 2U, 0U});
    // Same late semantic refusal used by the public stage-wrapper contracts:
    // valid bytecode framing/scan, followed by an invalid constant destination.
    Bytes late;
    append(late, {0xfffe0200U, 0x0200001fU, 0x80000000U, 0x900f0000U,
        0x02000001U, 0xa00f0000U, 0x90e40000U, 0xffffU});
    append(bytes, {1U, static_cast<std::uint32_t>(late.size())});
    bytes.insert(bytes.end(), late.begin(), late.end());
    Bytes color;
    append(color, {0xffff0200U, 0x05000051U, 0xa00f0000U,
        0x3f800000U, 0x3f800000U, 0x3f800000U, 0x3f800000U,
        0x02000001U, 0x800f0800U, 0xa0e40000U, 0xffffU});
    append(bytes, {2U, static_cast<std::uint32_t>(color.size())});
    bytes.insert(bytes.end(), color.begin(), color.end());
    return bytes;
}
int forced_compile_status{};
int fail_compile(const runtime::abi::Compile*, runtime::abi::Code*, char**) { return forced_compile_status; }
void refusal_reuse(const std::filesystem::path& directory, const std::filesystem::path& root) {
    activation::Input input;
    input.effect = "invented-refusal"; input.logical = "data/art/shaders/invented-refusal.fxo";
    input.form = presentation::shader_report::SelectedForm::compiled;
    input.bytes = refusal_effect();
    input.sha256 = core::sha256_hex({reinterpret_cast<const std::uint8_t*>(input.bytes.data()), input.bytes.size()});
    input.cache_key = presentation::shader_cache::Key{input.sha256};
    const auto parsed = assets::read_compiled_effect(input.bytes);
    check(parsed && parsed.value().programs.size() == 2U, "refusal effect has two authored stage descriptors");
    if (!parsed) return;
    const auto run = [&](const char* label, std::optional<int> status = {}, unsigned identity_field = 5U) {
        runtime::Runtime library;
        check(library.open(directory) == runtime::Status::ready, "refusal regression opens the verified package");
        // Each launch uses a fresh session and cache object; only disk survives.
        auto cache = std::make_shared<presentation::shader_cache::Cache>(root / "refusal-cache",
            presentation::shader_cache::Limits{16U * 1024U * 1024U, 4U * 1024U * 1024U, 64U, 128U});
        runtime::CompilerSession session(std::move(library));
        // Deliberately perturb only this test session's private identity/status.
        // Production still verifies the complete package before every lookup.
        auto& internal = runtime::StageSessionAccess::get(session);
        if (status) { forced_compile_status = *status; internal.api.vkd3d_shader_compile = fail_compile; }
        if (identity_field == 0U) internal.identity.package_lock_sha256[0U] ^= std::byte{1U};
        if (identity_field == 1U) internal.identity.capability_manifest_sha256[0U] ^= std::byte{1U};
        if (identity_field == 2U) internal.identity.public_c_abi.manifest_sha256[0U] ^= std::byte{1U};
        if (identity_field == 3U) ++internal.identity.result_schema_version;
        if (identity_field == 4U) ++internal.identity.options_manifest_version;
        material::Compiler compiler(std::move(session), cache, {}, desktop_settings);
        activation::Job job(1U, label, {input}, compiler.operations());
        job.step(16U);
        const auto result = job.result();
        check(result && result->effects.size() == 1U && job.progress().admitted == 0U,
            "refusal never publishes a resident material");
        std::vector<activation::Diagnostic> diagnostics;
        if (result && result->effects.size() == 1U) {
            check(result->effects.front().fallback == presentation::shader_report::Route::surface_omitted,
                "cached refusal retains the established omission");
            diagnostics = result->effects.front().diagnostics;
        }
        check(std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.stage == presentation::shader_report::FailureStage::stage_compile
                && diagnostic.reason == presentation::shader_report::Reason::compile_failure;
        }), "late failure retains its typed compile refusal");
        return std::pair{compiler.work(), diagnostics};
    };
    const auto cold = run("refusal-cold"), warm = run("refusal-warm");
    check(cold.first.translations > 0U, "cold refusal reaches the actual stage compiler");
    check(warm.first.translations == 0U && warm.first.source_compilations == 0U && warm.first.cache_hits > 0U,
        "fresh-session warm refusal performs zero translation or source compilation");
    const auto same = [](const activation::Diagnostic& a, const activation::Diagnostic& b) {
        return a.stage == b.stage && a.reason == b.reason && a.severity == b.severity;
    };
    check(cold.second.size() == warm.second.size()
        && std::equal(cold.second.begin(), cold.second.end(), warm.second.begin(), same),
        "warm refusal preserves every typed diagnostic in order");
    std::cout << "refusal work cold_translate=" << cold.first.translations
        << " warm_translate=" << warm.first.translations << " warm_cache_hits=" << warm.first.cache_hits << '\n';
    auto warm_disk = std::make_shared<presentation::shader_cache::Cache>(root / "refusal-cache",
        presentation::shader_cache::Limits{16U * 1024U * 1024U, 4U * 1024U * 1024U, 64U, 128U});
    material::Compiler unavailable(runtime::CompilerSession(runtime::Runtime{}), warm_disk, {}, desktop_settings);
    activation::Cancellation cancellation;
    const auto missing = unavailable.prepare(input, cancellation);
    check(!missing.artifact && missing.diagnostics.size() == 1U
        && missing.diagnostics.front().reason == presentation::shader_report::Reason::dependency_unavailable
        && unavailable.work().cache_hits == 0U && unavailable.work().translations == 0U,
        "a negative entry never substitutes for the current verified compiler dependency");
    runtime::Runtime library;
    check(library.open(directory) == runtime::Status::ready, "cancellation control opens the verified package");
    material::Compiler cancelled(runtime::CompilerSession(std::move(library)), warm_disk, {}, desktop_settings);
    cancellation.requested.store(true);
    const auto stopped = cancelled.prepare(input, cancellation);
    check(!stopped.artifact && stopped.diagnostics.size() == 1U
        && stopped.diagnostics.front().reason == presentation::shader_report::Reason::cancelled
        && cancelled.work().cache_hits == 0U && cancelled.work().translations == 0U,
        "cancellation takes priority over a persisted refusal");
    const auto outer = input.cache_key;
    for (unsigned field = 0U; field < 5U; ++field) {
        const auto changed = run("changed-compiler", {}, field);
        check(changed.first.translations == cold.first.translations,
            "every current compiler identity component partitions negative reuse");
    }
    input.cache_key = presentation::shader_cache::Key{std::string(64U, '9')};
    check(run("changed-outer").first.translations == cold.first.translations,
        "the current outer input identity partitions negative reuse");
    input.cache_key = outer;
    const auto program_offset = static_cast<std::size_t>(parsed.value().programs.front().bytes.data() - input.bytes.data());
    input.bytes[program_offset + 26U] = std::byte{0xe0U}; // different source swizzle, still an invalid destination
    input.sha256 = core::sha256_hex({reinterpret_cast<const std::uint8_t*>(input.bytes.data()), input.bytes.size()});
    check(run("changed-stage").first.translations == 1U,
        "fresh stage bytes partition refusals even with the same outer cache key");
    input.bytes = refusal_effect();
    input.sha256 = core::sha256_hex({reinterpret_cast<const std::uint8_t*>(input.bytes.data()), input.bytes.size()});
    presentation::shader_cache::Cache disk(root / "refusal-cache",
        {16U * 1024U * 1024U, 4U * 1024U * 1024U, 64U, 128U});
    std::optional<presentation::shader_cache::Key> negative;
    const auto any = [](auto) { return true; };
    for (const auto& entry : std::filesystem::directory_iterator(root / "refusal-cache")) {
        if (entry.path().extension() != ".eawrshader") continue;
        presentation::shader_cache::Key candidate{entry.path().stem().string()};
        const auto cached = disk.lookup(candidate, true, any);
        constexpr std::string_view marker = "eawr-stage-refusal/1:stage_compile:invalid_shader:error";
        if (cached.payload.size() == marker.size()
            && std::equal(cached.payload.begin(), cached.payload.end(), marker.begin())) {
            // Invalidate every negative record; rebuilding the current one
            // below identifies its key without duplicating key derivation.
            check(disk.invalidate(candidate, true) == presentation::shader_cache::Status::invalidated,
                "test owns and invalidates a negative entry");
        }
    }
    check(run("identify-current-negative").first.translations == 1U, "only the invalid stage recompiles after invalidation");
    for (const auto& entry : std::filesystem::directory_iterator(root / "refusal-cache")) {
        if (entry.path().extension() != ".eawrshader") continue;
        presentation::shader_cache::Key candidate{entry.path().stem().string()};
        const auto cached = disk.lookup(candidate, true, any);
        if (cached.payload.size() < 100U) negative = candidate;
    }
    check(negative.has_value(), "canonical refusal has a bounded independent disk record");
    if (negative) {
        const auto original = disk.lookup(*negative, true, any).payload;
        for (unsigned mutation = 0U; mutation < 3U; ++mutation) {
            auto damaged = original;
            if (mutation == 0U) damaged.push_back(0U);
            else if (mutation == 1U) damaged.pop_back();
            else damaged.front() ^= 1U;
            check(disk.store(*negative, true, damaged, any) == presentation::shader_cache::Status::stored,
                "semantic corruption control has a valid outer checksum");
            check(run("corrupt-negative").first.translations == 1U,
                "malformed negative payload is a recoverable miss");
        }
    }
    for (const auto status : {-2, -1, -3, -5, -999, 1}) {
        input.cache_key = presentation::shader_cache::Key{core::sha256_hex(
            {reinterpret_cast<const std::uint8_t*>(&status), sizeof(status)})};
        const auto failed = run("retryable-cold", status);
        const auto retried = run("retryable-warm", status);
        check(failed.first.translations == 2U && retried.first.translations == 2U && retried.first.cache_hits == 0U,
            "resource, unknown and other non-qualified compiler failures retry in a fresh session");
    }
}
void offline() {
    std::string refusal_details;
    core::Diagnostic prewarm_error{.code = "EAWR-RENDER-0007", .severity = core::Severity::error,
        .message = "offscreen coverage incomplete; draw_calls=0\n\"bounded\"", .logical_path = {},
        .line = {}, .column = {}, .source_id = {}};
    presentation::godot_backend::append_prewarm_refusal(refusal_details, prewarm_error);
    check(refusal_details.find("EAWR-RENDER-0007") != std::string::npos
        && refusal_details.find("draw_calls=0\\n\\\"bounded\\\"") != std::string::npos,
        "the report retains the exact prewarm reason and quotes controls");
    prewarm_error.message.assign(65536U, 'x');
    for (unsigned i = 0U; i < 128U; ++i)
        presentation::godot_backend::append_prewarm_refusal(refusal_details, prewarm_error);
    check(refusal_details.size() <= presentation::godot_backend::prewarm_diagnostic_bytes,
        "prewarm refusals cannot exhaust the activation report budget");
    runtime::CompilerIdentity identity;
    const presentation::shader_cache::Key outer{std::string(64U, '7')};
    const auto original = material::source_stage_cache_key(outer, "VertexEntry", "vs_3_0", identity);
    for (unsigned field = 0U; field < 5U; ++field) {
        auto changed = identity;
        if (field == 0U) changed.package_lock_sha256[0U] = std::byte{1U};
        else if (field == 1U) changed.capability_manifest_sha256[0U] = std::byte{1U};
        else if (field == 2U) changed.public_c_abi.manifest_sha256[0U] = std::byte{1U};
        else if (field == 3U) ++changed.result_schema_version;
        else ++changed.options_manifest_version;
        check(material::source_stage_cache_key(outer, "VertexEntry", "vs_3_0", changed).sha256 != original.sha256,
            "every compiler identity component partitions source-stage reuse");
    }
    runtime::CompilerSession closed(runtime::Runtime{});
    check(!runtime::compiler_identity(closed), "closed session exposes no activation cache identity");
    material::Compiler compiler(std::move(closed), {}, {}, desktop_settings);
    activation::Input input; input.form = presentation::shader_report::SelectedForm::compiled;
    input.policy = activation::Policy::opted_out;
    activation::Cancellation cancellation;
    const auto result = compiler.prepare(input, cancellation);
    check(!result.artifact && compiler.work().translations == 0U, "opt-out never enters translation");
    input.policy = activation::Policy::allowed;
    cancellation.requested.store(true);
    check(!compiler.prepare(input, cancellation).artifact && compiler.work().translations == 0U,
        "cancelled mod activation never enters translation");
    activation::Generation generation;
    check(material::find(generation, "late") == nullptr && compiler.work().translations == 0U,
        "unknown battle-time effect is a pure failed lookup");
    input.form = presentation::shader_report::SelectedForm::absent;
    input.effect = "absent"; input.adapter = "qualified-same-name";
    activation::Publisher publisher;
    activation::Job missing(publisher.begin(), "missing", {input}, compiler.operations()); missing.step(8U);
    check(missing.result() && missing.result()->effects[0U].fallback == presentation::shader_report::Route::missing_effect_green,
        "absent effects retain the explicit missing-effect policy");
    input.form = presentation::shader_report::SelectedForm::compiled;
    input.sha256 = core::sha256_hex({});
    input.effect = "present";
    activation::Job adapter(publisher.begin(), "adapter", {input}, compiler.operations()); adapter.step(8U);
    check(adapter.result() && adapter.result()->effects[0U].fallback == presentation::shader_report::Route::hand_adapter_approximation,
        "present refused effects keep their qualified same-name approximation");
    input.adapter.clear();
    activation::Job omitted(publisher.begin(), "omitted", {input}, compiler.operations()); omitted.step(8U);
    check(omitted.result() && omitted.result()->effects[0U].fallback == presentation::shader_report::Route::surface_omitted,
        "present refused effects without an adapter remain explicit omissions");
}
void package(const std::filesystem::path& directory) {
    const auto root = std::filesystem::temp_directory_path() / ("eawr-material-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { testing::remove_temp_directory(path); } } cleanup{root};
    std::filesystem::create_directories(root / "mod/art/shaders");
    refusal_reuse(directory, root);
    constexpr std::string_view text =
        "float4 Tint=float4(1.0,1.0,1.0,1.0);\n"
        "float4 VertexEntry(float4 p:POSITION):POSITION{return p*Tint;}\n"
        "float4 PixelEntry():COLOR0{return float4(0.25,0.5,0.75,1.0);}\n"
        "technique main {pass draw {VertexShader=compile vs_3_0 VertexEntry();PixelShader=compile ps_3_0 PixelEntry();}}\n";
    // Same position-only source as the shipped five-launch control. This
    // exercises real compilation, lowering and renderer CPU validation without a GPU.
    constexpr std::string_view route_text =
        "float4 VertexEntry(float4 p:POSITION):POSITION {return float4(p.x * 0.002, p.z * 0.002, 0.5, 1.0);}\n"
        "float4 PixelEntry():COLOR0 {return float4(0.25,0.5,0.75,1.0);}\n"
        "technique main {pass draw {VertexShader=compile vs_3_0 VertexEntry();PixelShader=compile ps_3_0 PixelEntry();}}\n";
    { std::ofstream output(root / "mod/art/shaders/invented.fx", std::ios::binary); output << text; }
    { std::ofstream output(root / "mod/art/shaders/route.fx", std::ios::binary); output << route_text; }
    auto unsupported_profile = std::string(text);
    unsupported_profile.replace(unsupported_profile.find("vs_3_0"), 6U, "vs_4_0");
    auto unsupported_semantic = std::string(text);
    unsupported_semantic.replace(unsupported_semantic.find("p:POSITION"), 10U, "p:BLENDWEIGHT");
    { std::ofstream output(root / "mod/art/shaders/profile.fx", std::ios::binary); output << unsupported_profile; }
    { std::ofstream output(root / "mod/art/shaders/semantic.fx", std::ios::binary); output << unsupported_semantic; }
    const std::array layers{vfs::MountSpec{"mod", root / "mod", "data", {}}};
    auto mounted = vfs::Vfs::mount(layers);
    check(bool(mounted), "invented source mount"); if (!mounted) return;
    runtime::Runtime library;
    check(library.open(directory) == runtime::Status::ready, "exact pinned package opens");
    runtime::CompilerSession session(std::move(library));
    const auto verified = runtime::compiler_identity(session);
    check(verified && verified->public_c_abi.pointer_bits == 64U, "activation obtains verified identity without translation");
    assets::ShaderSourceProvider provider(mounted.value(), "data/art/shaders");
    auto frozen = runtime::preprocess_source(session, provider, "data/art/shaders/invented.fx");
    check(frozen.source.has_value(), "invented source is preprocessed through the mounted provider");
    if (!frozen.source) return;
    auto context = std::make_shared<activation::SourceSnapshot>(); context->source = std::move(*frozen.source);
    activation::Input input;
    input.effect = "invented"; input.form = presentation::shader_report::SelectedForm::source;
    input.logical = "data/art/shaders/invented.fx"; input.context = context;
    input.bytes.assign(reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data()) + text.size());
    input.sha256 = core::sha256_hex({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    input.cache_key = presentation::shader_cache::Key{std::string(64U, '7')};
    auto cache = std::make_shared<presentation::shader_cache::Cache>(root / "cache",
        presentation::shader_cache::Limits{16U * 1024U * 1024U, 4U * 1024U * 1024U, 64U, 128U});
    std::size_t prewarms{};
    material::Compiler compiler(std::move(session), cache, [&](const material::Program& program)
        -> std::shared_ptr<const activation::Artifact> {
        const auto valid = presentation::validate_material(program.material);
        check(bool(valid), "translated program passes production renderer CPU preflight");
        if (!valid) {
            std::cerr << valid.error().code << ": " << valid.error().message << '\n';
            return {};
        }
        ++prewarms; return std::make_shared<Device>();
    }, desktop_settings);
    activation::Publisher publisher;
    activation::Job cold(publisher.begin(), "cold", {input}, compiler.operations()); cold.step(16U);
    check(cold.progress().state == activation::State::complete && cold.progress().admitted == 1U, "cold activation admits a complete invented program");
    if (cold.progress().admitted != 1U) {
        if (const auto& diagnostic = compiler.wrapper_refusal())
            std::cerr << "material wrapper refusal " << diagnostic->code << ": " << diagnostic->message << '\n';
        if (const auto result = cold.result()) for (const auto& effect : result->effects)
            for (const auto& diagnostic : effect.diagnostics) std::cerr << "activation refusal " << static_cast<int>(diagnostic.stage)
                << '/' << static_cast<int>(diagnostic.reason) << '\n';
        return;
    }
    check(publisher.publish(cold), "complete cold generation publishes atomically");
    const auto old = publisher.active();
    const auto* resident = material::find(*old, "invented");
    check(resident && !resident->program.vertex.uniforms.empty(),
        "real compiler fixture reconciles a numeric uniform rather than only constant-only stages");
    if (resident) {
        auto instance = material::make_instance(resident->program);
        check(bool(material::update(resident->program, {}, instance)) && instance.valid,
            "real source parameter defaults execute through the retained P5 plan");
    }
    const auto counts = compiler.work();
    const auto cold_bytes = compiler.resident_bytes();
    check(cold_bytes > 0U && cold_bytes <= 64U * 1024U * 1024U, "cold generation reports its charged CPU payload");
    check(counts.source_compilations == 2U && counts.translations == 2U && prewarms == 1U,
        "cold work counters cover both stage compiles and translations before publication");
    activation::Job warm(publisher.begin(), "warm", {input}, compiler.operations()); warm.step(16U);
    const auto warm_counts = compiler.work();
    check(warm.progress().admitted == 1U && warm_counts.source_compilations == counts.source_compilations
        && warm_counts.translations == counts.translations && warm_counts.cache_hits >= counts.cache_hits + 4U,
        "warm activation has zero source compilations and zero backend translations");
    check(publisher.publish(warm) && material::find(*old, "invented") && material::find(*publisher.active(), "invented"),
        "mod-generation handover retains old admitted readers");
    check(compiler.resident_bytes() == 2U * cold_bytes,
        "old and current generation payloads both remain charged during handover");
    // Seed only source entries produced under another verified-identity partition.
    // If the production key becomes constant, these become hits and the measured
    // source compilation assertion fails, even though bytecode is otherwise valid.
    auto foreign_cache = std::make_shared<presentation::shader_cache::Cache>(root / "foreign-cache",
        presentation::shader_cache::Limits{16U * 1024U * 1024U, 4U * 1024U * 1024U, 64U, 128U});
    if (verified) {
        auto changed = *verified; changed.package_lock_sha256[0U] ^= std::byte{1U};
        const auto accept = [](std::span<const std::uint8_t> bytes) { return !bytes.empty(); };
        for (const auto& [entry, profile] : std::array{
            std::pair{"VertexEntry", "vs_3_0"}, std::pair{"PixelEntry", "ps_3_0"}}) {
            const auto current_key = material::source_stage_cache_key(*input.cache_key, entry, profile, *verified);
            const auto foreign_key = material::source_stage_cache_key(*input.cache_key, entry, profile, changed);
            const auto cached = cache->lookup(current_key, true, accept);
            check(cached.status == presentation::shader_cache::Status::hit,
                "identity replacement control obtains real compiled source bytes");
            check(foreign_cache->store(foreign_key, true, cached.payload, accept)
                == presentation::shader_cache::Status::stored, "foreign identity source partition is seeded");
        }
    }
    const auto open_session = [&]() {
        runtime::Runtime loaded;
        check(loaded.open(directory) == runtime::Status::ready, "budget/identity control opens verified package");
        return runtime::CompilerSession(std::move(loaded));
    };
    const auto device = [](const material::Program&) { return std::make_shared<Device>(); };
    material::Compiler replaced(open_session(), foreign_cache, device, desktop_settings);
    activation::Cancellation replacement_token;
    const auto replacement = replaced.prepare(input, replacement_token);
    const auto replacement_work = replaced.work();
    check(replacement.artifact && replacement_work.source_compilations == 2U && replacement_work.translations == 2U,
        "changed compiler identity misses foreign source bytes and recompiles/retranslates both stages");
    std::cout << "identity replacement source=" << replacement_work.source_compilations
        << " translate=" << replacement_work.translations << '\n';
    const auto budget_reason = [](const auto& result) {
        return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.reason == presentation::shader_report::Reason::budget_exceeded;
        });
    };
    for (unsigned tier = 0U; tier < 3U; ++tier) {
        material::ActivationLimits limits;
        if (tier == 0U) limits.stages = 1U;
        else if (tier == 1U) limits.programs = 1U;
        else limits.prepared = 1U;
        material::Compiler bounded(open_session(), cache, device, desktop_settings, limits);
        const auto over = bounded.prepare(input, replacement_token);
        check(!over.artifact && budget_reason(over) && bounded.resident_bytes() == 0U,
            "each tightened per-effect payload limit takes its real refusal branch");
    }
    material::ActivationLimits resident_limit; resident_limit.resident = cold_bytes;
    material::Compiler bounded(open_session(), cache, device, desktop_settings, resident_limit);
    const auto bounded_prepared = bounded.prepare(input, replacement_token);
    check(bounded_prepared.artifact && !bounded_prepared.variants.empty(), "resident cap control prepares a real program");
    if (bounded_prepared.artifact && !bounded_prepared.variants.empty()) {
        auto first = bounded.admit(input, bounded_prepared, bounded_prepared.variants.front(), replacement_token);
        const auto over = bounded.admit(input, bounded_prepared, bounded_prepared.variants.front(), replacement_token);
        check(first.artifact && !over.artifact && budget_reason(over) && bounded.resident_bytes() == cold_bytes,
            "resident cap refuses retained payload before a second device prewarm");
        first.artifact.reset();
        check(bounded.resident_bytes() == 0U
            && bounded.admit(input, bounded_prepared, bounded_prepared.variants.front(), replacement_token).artifact,
            "released resident charge permits admission again");
    }
    activation::Cancellation prewarm_token;
    material::Compiler cancelled_prewarm(open_session(), cache, [&](const material::Program&) {
        prewarm_token.requested.store(true); return std::make_shared<Device>();
    }, desktop_settings);
    const auto before_cancel = cancelled_prewarm.prepare(input, prewarm_token);
    check(before_cancel.artifact && !before_cancel.variants.empty(), "prewarm cancellation control prepares");
    if (before_cancel.artifact && !before_cancel.variants.empty())
        check(!cancelled_prewarm.admit(input, before_cancel, before_cancel.variants.front(), prewarm_token).artifact
            && cancelled_prewarm.resident_bytes() == 0U && cancelled_prewarm.work().admissions == 1U
            && cancelled_prewarm.work().prewarms == 0U,
            "cancellation during prewarm releases CPU charge and prevents resident publication");
    const auto before_late = compiler.work().translations;
    check(material::find(*publisher.active(), "late") == nullptr && compiler.work().translations == counts.translations,
        "battle-time late lookup does zero translation work");
    const auto late_translations = compiler.work().translations - before_late;
    activation::Job stale(publisher.begin(), "stale", {input}, compiler.operations()); stale.step(16U);
    const auto latest = publisher.begin(); publisher.cancel(latest);
    check(!publisher.publish(stale), "cancelled/superseded mod generation cannot publish");
    material::Compiler unavailable(runtime::CompilerSession(runtime::Runtime{}), cache, [&](const material::Program&) {
        ++prewarms; return std::make_shared<Device>();
    }, desktop_settings);
    activation::Job no_package(publisher.begin(), "no-package", {input}, unavailable.operations()); no_package.step(16U);
    check(no_package.progress().admitted == 0U && unavailable.work().translations == 0U
        && unavailable.work().cache_hits == 0U, "missing package cannot admit even a previously cached program");
    runtime::Runtime no_device_library;
    check(no_device_library.open(directory) == runtime::Status::ready, "device refusal uses a valid compiler session");
    material::Compiler refused_device(runtime::CompilerSession(std::move(no_device_library)), cache, {}, desktop_settings);
    activation::Cancellation token;
    const auto cpu_only = refused_device.prepare(input, token);
    check(cpu_only.artifact && !cpu_only.variants.empty()
        && !refused_device.admit(input, cpu_only, cpu_only.variants.front(), token).artifact
        && refused_device.resident_bytes() == 0U,
        "a CPU payload without real renderer prewarm cannot become resident");
    for (const auto& entry : std::filesystem::directory_iterator(root / "cache")) if (entry.is_regular_file()) {
        std::ofstream corrupt(entry.path(), std::ios::binary | std::ios::trunc); corrupt << "corrupt";
    }
    activation::Job recovery(publisher.begin(), "recovery", {input}, compiler.operations()); recovery.step(16U);
    check(recovery.progress().admitted == 1U && compiler.work().translations > counts.translations,
        "corrupt cache is regenerated during activation");
    runtime::Runtime acquisition_library;
    check(acquisition_library.open(directory) == runtime::Status::ready, "refusal fixtures use verified source acquisition");
    runtime::CompilerSession acquisition(std::move(acquisition_library));
    const auto refusal = [&](std::string_view name, presentation::shader_report::Reason reason) {
        const auto logical = std::string("data/art/shaders/") + std::string(name) + ".fx";
        auto source = runtime::preprocess_source(acquisition, provider, logical);
        check(source.source.has_value(), "refusal fixture is independently preprocessed");
        if (!source.source) return;
        auto snapshot = std::make_shared<activation::SourceSnapshot>(); snapshot->source = std::move(*source.source);
        auto candidate = input; candidate.logical = logical; candidate.effect = name;
        candidate.context = snapshot; candidate.cache_key.reset();
        candidate.bytes = snapshot->source.expanded;
        candidate.sha256 = core::sha256_hex({reinterpret_cast<const std::uint8_t*>(candidate.bytes.data()), candidate.bytes.size()});
        const auto prepared = compiler.prepare(candidate, token);
        check(!prepared.artifact && std::any_of(prepared.diagnostics.begin(), prepared.diagnostics.end(),
            [&](const auto& diagnostic) { return diagnostic.reason == reason; }), "profile/semantic refusal retains its named cause");
    };
    refusal("profile", presentation::shader_report::Reason::unsupported_profile);
    refusal("semantic", presentation::shader_report::Reason::unsupported_semantic);
    auto route_source = runtime::preprocess_source(acquisition, provider, "data/art/shaders/route.fx");
    check(route_source.source.has_value(), "five-launch source preprocesses without a GPU");
    if (route_source.source) {
        auto route_snapshot = std::make_shared<activation::SourceSnapshot>();
        route_snapshot->source = std::move(*route_source.source);
        auto route_input = input;
        route_input.effect = "route"; route_input.logical = "data/art/shaders/route.fx";
        route_input.context = route_snapshot; route_input.cache_key.reset();
        route_input.bytes = route_snapshot->source.expanded;
        route_input.sha256 = core::sha256_hex({reinterpret_cast<const std::uint8_t*>(route_input.bytes.data()), route_input.bytes.size()});
        activation::Job route_cpu(publisher.begin(), "route-cpu", {route_input}, compiler.operations());
        route_cpu.step(16U);
        check(route_cpu.progress().admitted == 1U,
            "five-launch source reaches successful renderer CPU preflight before device admission");
    }
    std::cout << "material work cold_source=" << counts.source_compilations << " cold_translate=" << counts.translations
        << " warm_source=" << warm_counts.source_compilations - counts.source_compilations
        << " warm_translate=" << warm_counts.translations - counts.translations
        << " warm_admitted=" << warm.progress().admitted << " battle_translate=" << late_translations
        << " cold_cpu_payload_bytes=" << cold_bytes << '\n';
}
} // namespace
int main(int argc, char** argv) {
    offline();
    if (argc == 2 && std::string_view(argv[1]) == "--package-env") {
#ifdef _WIN32
        char* path{};
        std::size_t length{};
        if (_dupenv_s(&path, &length, "EAWR_SHADER_RUNTIME_TEST_PACKAGE") != 0 || !path || length <= 1U) return 77;
        const std::unique_ptr<char, decltype(&std::free)> owner(path, &std::free);
#else
        const auto* path = std::getenv("EAWR_SHADER_RUNTIME_TEST_PACKAGE");
        if (!path || !*path) return 77;
#endif
        package(path);
    }
    return checks.finish();
}
