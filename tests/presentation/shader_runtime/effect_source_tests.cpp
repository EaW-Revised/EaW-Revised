#include "eawr/presentation/shader_effect_source.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/presentation/shader_states.hpp"
#include "../../support/temp_dir.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace eawr;
namespace fx = presentation::shader_source;
namespace runtime = presentation::shader_runtime;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::span<const std::byte> bytes(std::string_view text) { return std::as_bytes(std::span(text.data(), text.size())); }
fx::Parsed parse(std::string_view text, fx::Limits limits = {}) {
    assets::Source root; root.logical_path = "data/art/shaders/invented.fx";
    return fx::parse(bytes(text), root, {}, limits);
}
constexpr std::string_view invented = R"(
float4 tint : COLOR < string Label = "invented"; > = float4(1, 0.5, 0, 1);
bool depth = true;
texture tex;
sampler2D surface = sampler_state { Texture = <tex>; MinFilter = LINEAR; AddressU = Clamp; };
float4 vertex(float4 p:POSITION):POSITION { return p; }
float4 pixel():COLOR { return tint; }
vertexshader named = compile vs_2_0 vertex();
technique first < string Label = "ordered"; > {
    pass draw { VertexShader = named; PixelShader = compile ps_2_0 pixel(); ZWriteEnable = <depth>; AlphaBlendEnable = true; }
    pass cleanup { AlphaBlendEnable = false; VertexShader = NULL; PixelShader = NULL; }
}
technique second { pass fixed { ZEnable = true; } }
)";
void graph_and_descriptors() {
    auto parsed = parse(invented); check(parsed.graph.has_value(), "invented effect parses");
    const auto& graph = *parsed.graph;
    check(graph.techniques.size() == 2 && graph.techniques[0].passes.size() == 2, "ordered alternatives and cleanup retained");
    check(graph.stages.size() == 2 && graph.declarations[0].annotations[0].type == "string", "typed annotations and named stages");
    const std::string stage_text(reinterpret_cast<const char*>(graph.stage_text.data()), graph.stage_text.size());
    check(stage_text.find("technique") == std::string::npos && stage_text.find("sampler_state") == std::string::npos && stage_text.find("sampler2D surface") != std::string::npos && stage_text.find("return tint;") != std::string::npos, "only effect wrappers removed");
    std::size_t calls{};
    auto result = fx::produce(std::move(*parsed.graph), [&](auto text, auto entry, auto profile) {
        check(text.size() == stage_text.size(), "one expanded stage input"); ++calls;
        runtime::SourceStage stage; stage.entry = entry; stage.profile = profile;
        stage.bytecode = {std::byte{0},std::byte{2},profile.starts_with("vs_") ? std::byte{0xfe} : std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0},std::byte{0}};
        return runtime::SourceCompilation{std::move(stage), {}};
    });
    check(result.effect.has_value() && calls == 2, "each exact stage compiled once into P1");
    auto owner = std::move(*result.effect); result.effect.reset();
    const auto& effect = owner.view();
    check(effect.techniques[0].name == "first" && effect.techniques[0].passes[1].name == "cleanup", "borrowed names survive handle moves");
    check(effect.values[effect.parameters[0].value].bytes.size() == 16, "logical vector default packed without CTAB padding");
    auto state_plan = presentation::shader_states::normalize_pass(effect, effect.techniques[0].passes[0]);
    check(state_plan && state_plan.value().requires_evaluation && state_plan.value().shader_states.size() == 2, "same P4 state route preserves parameter evaluation");
    const auto& sampler = effect.values[effect.parameters[3].value].sampler_elements[0];
    auto sampler_plan = presentation::shader_states::normalize_sampler(effect, sampler);
    check(sampler_plan && sampler_plan.value().requires_resource_resolution, "same P3/P4 sampler resource relationship");
    const auto named_slot = effect.values[effect.parameters[4].value].object_slots[0];
    check(effect.objects[named_slot].program == 0U, "named shader object links P1 program");
}
void refusals() {
    for (const auto text : {"technique x { pass p { MadeUp = 1; } }", "float4x4 m = {1}; technique x {pass p{}}", "float x = 1+2; technique x {pass p{}}", "stateblock x = { ZEnable = true; }; technique t {pass p{}}"}) {
        auto parsed = parse(text); check(parsed.graph.has_value(), "unknown state/expression remains in graph");
        const auto result = fx::produce(std::move(*parsed.graph), {});
        check(result.diagnostic && result.diagnostic->reason == fx::Failure::unsupported && result.diagnostic->location.line.has_value(), "located unsupported descriptor conversion");
    }
    for (const auto text : {"technique x { pass p { VertexShader = unresolved; } }", "technique x { unexpected; }", "float x; ] technique t {}", "/* unterminated"})
        check(parse(text).diagnostic.has_value(), "unknown syntax, references and unbalanced input refuse");
    fx::Limits limits; limits.tokens = 2;
    check(parse("technique x {}", limits).diagnostic->reason == fx::Failure::budget, "token bound");
    limits = {}; limits.nesting = 1;
    check(parse("technique x {pass p{}}", limits).diagnostic->reason == fx::Failure::budget, "depth bound");
    limits = {}; limits.nodes = 1;
    check(parse("technique x {pass p{}}", limits).diagnostic->reason == fx::Failure::budget, "node bound");
    auto source = parse("technique x {pass p {PixelShader=compile ps_1_1 legacy();}}");
    auto compiled = fx::produce(std::move(*source.graph), [](auto, auto, auto profile) {
        check(profile == "ps_1_1", "never promote a refused legacy profile");
        return runtime::SourceCompilation{{}, runtime::SourceDiagnostic{runtime::SourcePhase::stage_compile, runtime::SourceFailure::compiler_failure, {}}};
    });
    check(compiled.diagnostic && compiled.diagnostic->compiler->reason == runtime::SourceFailure::compiler_failure, "typed compiler refusal retained");
    auto known = parse("#line 41 \"data/art/shaders/invented.fx\"\ntechnique x {pass p{Unknown=1;}}");
    check(known.graph->techniques[0].passes[0].states[0].location.line == 41U, "known logical #line mapping");
    auto unknown = parse("#line 41 \"unverified.fx\"\ntechnique x {pass p{Unknown=1;}}");
    check(!unknown.graph->techniques[0].passes[0].states[0].location.line, "unknown coordinates remain absent");
}
void selection_and_identity() {
    const auto folder = std::filesystem::temp_directory_path() / ("eawr-effect-source-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { testing::remove_temp_directory(path); } } cleanup{folder};
    std::filesystem::create_directories(folder / "art/shaders");
    const auto stem = folder / "art/shaders/invented";
    std::ofstream(stem.string() + ".fx") << "invented";
    const std::array layers{vfs::MountSpec{"invented-layer", folder, "data", {}}};
    auto mounted = vfs::Vfs::mount(layers); check(bool(mounted), "mounted fixture");
    const fx::Allowed yes = [](const auto&) { return true; }, no = [](const auto&) { return false; };
    auto source = fx::select(mounted.value(), "DATA/ART/SHADERS/INVENTED", yes, yes);
    check(source && source.value().form == fx::Form::source && source.value().allowed, "effective source only when compiled absent");
    auto optout = fx::select(mounted.value(), "data/art/shaders/invented", no, yes);
    check(optout && !optout.value().allowed && fx::fallback(optout.value(), false) == fx::Fallback::omit, "opted-out present source never mislabeled missing");
    auto retail = fx::select(mounted.value(), "data/art/shaders/invented", yes, no);
    check(retail && retail.value().form == fx::Form::absent, "only mod source amendment");
    runtime::PreprocessedSource preprocessed;
    runtime::SourceDependency root; root.source = assets::source_from(*source.value().record);
    root.sha256.fill(std::byte{0x12}); preprocessed.dependencies.push_back(root);
    preprocessed.macros = {{"A","1"},{"A","2"}};
    auto parsed = parse(invented); presentation::shader_cache::Identity identity;
    check(fx::source_identity(identity, source.value(), preprocessed, *parsed.graph), "source closure binds selected provenance");
    check(identity.macros.size() == 2 && identity.stages.size() == 2 && identity.parser_schema == fx::schema_version && identity.root_layer == "invented-layer", "ordered macros, stages, parser and layer identity");
    check(!fx::source_identity(identity, optout.value(), preprocessed, *parsed.graph), "opt-out before cache identity");
    std::ofstream(stem.string() + ".fxo") << "corrupt compiled input";
    auto remounted = vfs::Vfs::mount(layers);
    auto compiled = fx::select(remounted.value(), "data/art/shaders/invented", yes, yes);
    check(compiled && compiled.value().form == fx::Form::compiled, "fresh resolution notices new compiled input even when corrupt");
    check(!fx::source_identity(identity, compiled.value(), preprocessed, *parsed.graph), "compiled failure cannot reuse source cache");
    check(fx::fallback(compiled.value(), true) == fx::Fallback::hand_adapter_approximation, "existing supplied effect uses explicit approximation");
}
} // namespace
int main() {
    try { graph_and_descriptors(); refusals(); selection_and_identity(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
