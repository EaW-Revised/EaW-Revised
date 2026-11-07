#include "eawr/presentation/shader_material_activation.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/core/sha256.hpp"
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
void offline() {
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
    constexpr std::string_view text =
        "float4 Tint=float4(1.0,1.0,1.0,1.0);\n"
        "float4 VertexEntry(float4 p:POSITION):POSITION{return p*Tint;}\n"
        "float4 PixelEntry():COLOR0{return float4(0.25,0.5,0.75,1.0);}\n"
        "technique main {pass draw {VertexShader=compile vs_3_0 VertexEntry();PixelShader=compile ps_3_0 PixelEntry();}}\n";
    { std::ofstream output(root / "mod/art/shaders/invented.fx", std::ios::binary); output << text; }
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
    material::Compiler compiler(std::move(session), cache, [&](const material::Program&) {
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
