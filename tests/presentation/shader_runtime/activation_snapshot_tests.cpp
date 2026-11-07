#include "eawr/presentation/shader_activation_mods.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/mods/chain.hpp"
#include "eawr/core/sha256.hpp"
#include "../../support/temp_dir.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace eawr;
namespace shader_runtime = eawr::presentation::shader_runtime;
using namespace presentation::shader_activation;
#define REQUIRE(x) do { if (!(x)) throw std::runtime_error("snapshot line " + std::to_string(__LINE__) + ": " #x); } while(false)
struct Folder {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("eawr-activation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Folder() { testing::remove_temp_directory(root); }
    void write(std::string_view layer, std::string_view file, std::string_view text) {
        const auto path = root / layer / "art" / file;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary); out << text; REQUIRE(out.good());
    }
    vfs::Vfs mount() {
        for (const auto* layer : {"child", "parent", "retail"}) std::filesystem::create_directories(root / layer);
        const std::array mounts{vfs::MountSpec{"child", root / "child", "data", {}},
            vfs::MountSpec{"parent", root / "parent", "data", {}}, vfs::MountSpec{"retail", root / "retail", "data", {}}};
        auto mounted = vfs::Vfs::mount(mounts); REQUIRE(mounted); return std::move(mounted).value();
    }
};
mods::ModRecord record(std::string_view json) {
    const auto info = mods::parse_modinfo(json); REQUIRE(info);
    mods::ModRecord result; result.key = "parent";
    result.metadata.kind = mods::steam::JsonValue::Kind::object;
    result.metadata.keys = {"modinfo"}; result.metadata.values = {info->metadata}; return result;
}
struct Payload final : Artifact {};
presentation::shader_cache::Identity base_identity() {
    presentation::shader_cache::Identity result;
    result.package_sha256 = result.capability_manifest = result.options_manifest_sha256 = std::string(64, 'a');
    for (auto* field : {&result.compiler_abi, &result.resolver_policy, &result.translator, &result.emitter,
         &result.binding_policy, &result.state_policy, &result.technique_policy, &result.admission_policy,
         &result.godot_engine, &result.shader_abi, &result.language, &result.renderer, &result.backend, &result.platform, &result.architecture})
        *field = "invented-1";
    return result;
}
shader_runtime::Digest digest(std::span<const std::byte> bytes) {
    const auto computed = core::sha256({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    shader_runtime::Digest result{};
    for (std::size_t i = 0; i < computed.size(); ++i) result[i] = static_cast<std::byte>(computed[i]);
    return result;
}
void effective_inventory_and_policies() {
    Folder folder;
    folder.write("parent", "parent-only.fxo", "parent");
    folder.write("parent", "overridden.fxo", "parent"); folder.write("child", "overridden.fxo", "child");
    folder.write("parent", "both.fxo", "compiled"); folder.write("child", "both.fx", "source");
    folder.write("child", "source-only.fx", "source"); folder.write("retail", "retail.fxo", "retail");
    folder.write("retail", "ignored.fx", "unused retail source");
    auto mounted = folder.mount();
    auto parent = record(R"({"name":"Invented","shader_translation":false})");
    const std::array layers{LayerPolicy{"child", true, Policy::allowed}, contributor_policy("parent", parent), LayerPolicy{"retail", false, Policy::allowed}};
    const std::array reachable{std::string("DATA/ART/BOTH"), std::string("data/art/missing")};
    Cancellation cancellation;
    auto captured = capture(mounted, "chain", layers, reachable,
        [](auto) { return "same-name"; }, {}, cancellation);
    REQUIRE(captured);
    auto& snapshot = captured.value();
    REQUIRE(snapshot.compiled_inventory == 4 && snapshot.source_inventory == 1 && snapshot.reachable == 2);
    REQUIRE(snapshot.inputs.size() == 6);
    folder.write("child", "overridden.fxo", "edited");
    std::size_t prepared = 0;
    Operations operations{[&](const Input& input, const Cancellation&) {
        ++prepared; REQUIRE(input.layer == "child" || input.layer == "retail");
        if (input.layer == "child") REQUIRE(input.bytes.size() == 5 && input.bytes[0] == std::byte{'c'});
        return Prepared{std::make_shared<Payload>(), {{0, 0}}, {}};
    }, [](const Input&, const Prepared&, Variant, const Cancellation&) { return Admitted{std::make_shared<Payload>(), {}}; }};
    Publisher publisher;
    Job job(publisher.begin(), snapshot.chain_identity, std::move(snapshot.inputs), operations);
    job.step(100); REQUIRE(job.progress().state == State::complete && prepared == 2);
    const auto completed = complete(publisher, job, {64, 128, 64, 64, 4096, 65536});
    REQUIRE(completed && completed->report.note && completed->report.note->details_available);
    REQUIRE(completed->report.counts.routes[0] == 2 && completed->report.counts.routes[1] == 3 && completed->report.counts.routes[3] == 1);
    REQUIRE(completed->report.details.find("opted_out") != std::string::npos);
    REQUIRE(completed->report.details.find("dependency_unavailable") != std::string::npos);
    REQUIRE(!complete(publisher, job, {64, 128, 64, 64, 4096, 65536}));
    // Content changed after capture cannot change the owned job snapshot.
    folder.write("child", "overridden.fxo", "edited");
    const auto active = publisher.active();
    const auto& effect = active->effects[2];
    REQUIRE(effect.input.effect == "data/art/overridden");
    REQUIRE(effect.input.bytes.empty() && !effect.input.context);
    parent = record(R"({"name":"Invented","shader_translation":"false"})");
    REQUIRE(contributor_policy("parent", parent).policy == Policy::malformed);
    auto invalid_layers = layers; invalid_layers[0].layer = "parent";
    REQUIRE(!capture(mounted, "chain", invalid_layers, {}, {}, {}, cancellation));
    REQUIRE(!capture(mounted, "chain", std::span(layers).first(1), {}, {}, {}, cancellation));
}
void source_closure_and_failures() {
    Folder folder; folder.write("child", "source.fx", "source");
    auto mounted = folder.mount(); Cancellation cancellation;
    const std::array layers{LayerPolicy{"child", true, Policy::allowed}};
    std::size_t frozen = 0;
    auto freezer = [&](const vfs::AssetRecord& selected, const Cancellation&) {
        ++frozen;
        auto bytes = mounted.open(selected); REQUIRE(bytes);
        shader_runtime::PreprocessedSource source;
        source.expanded = bytes.value(); source.expanded_sha256 = digest(source.expanded);
        source.dependencies.push_back({assets::source_from(selected), digest(bytes.value())});
        return shader_runtime::SourcePreparation{std::move(source), {}, 1, 1};
    };
    auto captured = capture(mounted, "chain", layers, {}, {}, freezer, cancellation); REQUIRE(captured && frozen == 1);
    auto snapshot = std::move(captured).value();
    const auto source = std::dynamic_pointer_cast<const SourceSnapshot>(snapshot.inputs[0].context);
    REQUIRE(source && source->source.expanded.size() == 6);
    folder.write("child", "source.fx", "edited");
    REQUIRE(source->source.expanded[0] == std::byte{'s'});
    auto broken = [&](const vfs::AssetRecord& selected, const Cancellation& token) {
        auto result = freezer(selected, token); result.source->dependencies[0].sha256 = {}; return result;
    };
    auto failed = capture(mounted, "chain", layers, {}, {}, broken, cancellation); REQUIRE(failed);
    REQUIRE(failed.value().inputs[0].snapshot_failure && failed.value().inputs[0].bytes.empty());
    REQUIRE(failed.value().inputs[0].snapshot_failure->stage == report::FailureStage::preprocess);
    std::size_t called = 0;
    Operations operations{[&](const Input&, const Cancellation&) { ++called; return Prepared{}; }, {}};
    Job job(1, "chain", std::move(failed.value().inputs), operations); job.step(100);
    REQUIRE(job.progress().state == State::complete && called == 0);
    REQUIRE(job.result()->effects[0].fallback == report::Route::surface_omitted);
    auto caps = SnapshotLimits{}; caps.job.input_bytes = 1;
    auto budget = capture(mounted, "chain", layers, {}, {}, freezer, cancellation, caps); REQUIRE(budget);
    REQUIRE(budget.value().inputs[0].snapshot_failure->reason == report::Reason::budget_exceeded);
    caps = {}; caps.inventory_entries = 0;
    REQUIRE(!capture(mounted, "chain", layers, {}, {}, freezer, cancellation, caps));
    const auto blocked = std::array{LayerPolicy{"child", true, Policy::opted_out}};
    const auto before = frozen;
    auto opted_out = capture(mounted, "chain", blocked, {}, {}, freezer, cancellation); REQUIRE(opted_out);
    REQUIRE(frozen == before && !opted_out.value().inputs[0].context);
    cancellation.requested.store(true);
    REQUIRE(!capture(mounted, "chain", layers, {}, {}, freezer, cancellation));
}
void cache_identity_controls() {
    Folder folder; folder.write("child", "invented.fxo", "synthetic compiled content");
    auto mounted = folder.mount(); Cancellation cancellation;
    const std::array layers{LayerPolicy{"child", true, Policy::allowed}};
    auto identity = [](const Input&) { return std::optional(base_identity()); };
    auto captured = capture(mounted, "chain", layers, {}, {}, {}, cancellation, {}, identity); REQUIRE(captured);
    const auto& selected = captured.value().inputs[0]; REQUIRE(selected.cache_key);
    auto changed = base_identity(); changed.macros = {{"invented", "changed"}};
    const auto changed_key = activation_key(selected, changed); REQUIRE(changed_key);
    REQUIRE(changed_key->sha256 != selected.cache_key->sha256);
    auto different_layer = selected; different_layer.layer = "another-layer";
    const auto layer_key = activation_key(different_layer, base_identity()); REQUIRE(layer_key);
    REQUIRE(layer_key->sha256 != selected.cache_key->sha256);
    const auto cache = std::make_shared<presentation::shader_cache::Cache>(folder.root / "cache",
        presentation::shader_cache::Limits{65536, 8192, 16, 64});
    CacheCodec codec;
    codec.validate = [](std::span<const std::uint8_t> bytes) { return bytes.size() == 1 && bytes[0] == 42U; };
    codec.decode = [](std::span<const std::uint8_t>) { return Prepared{std::make_shared<Payload>(), {{0, 0}}, {}}; };
    codec.encode = [](const Prepared&) { return std::vector<std::uint8_t>{42}; };
    std::size_t translations = 0, prewarms = 0;
    Operations fresh{[&](const Input&, const Cancellation&) { ++translations; return Prepared{std::make_shared<Payload>(), {{0, 0}}, {}}; },
        [&](const Input&, const Prepared&, Variant, const Cancellation&) { ++prewarms; return Admitted{std::make_shared<Payload>(), {}}; }};
    auto operations = cached_operations(fresh, cache, codec);
    Job cold(1, "chain", captured.value().inputs, operations); cold.step(100);
    REQUIRE(cold.progress().state == State::complete && translations == 1 && prewarms == 1);
    REQUIRE(cold.result()->effects[0].input.bytes.empty());
    Job warm(2, "chain", captured.value().inputs, operations); warm.step(100);
    REQUIRE(warm.progress().state == State::complete && translations == 1 && prewarms == 2);
    // A different full key must not dedupe under the same material name.
    auto another = captured.value().inputs[0]; another.cache_key = changed_key;
    Job distinct(3, "chain", {captured.value().inputs[0], another}, operations); distinct.step(100);
    REQUIRE(distinct.progress().effects == 2 && translations == 2 && prewarms == 4);
    const auto blocked = std::array{LayerPolicy{"child", true, Policy::opted_out}};
    auto opted_out = capture(mounted, "chain", blocked, {}, {}, {}, cancellation, {}, identity); REQUIRE(opted_out);
    Job skipped(4, "chain", std::move(opted_out.value().inputs), operations); skipped.step(100);
    REQUIRE(translations == 2 && prewarms == 4 && skipped.progress().state == State::complete);
}
}
int main() {
    try { effective_inventory_and_policies(); source_closure_and_failures(); cache_identity_controls(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
