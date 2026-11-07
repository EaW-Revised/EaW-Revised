#include "eawr/presentation/shader_source.hpp"
#include "eawr/vfs/vfs.hpp"
#include "../../support/temp_dir.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

using namespace eawr;
using namespace eawr::presentation::shader_runtime;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void write(const std::filesystem::path& path, std::string_view value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary); file << value;
}
struct Fixture {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("eawr-source-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { std::filesystem::create_directories(path); }
    ~Fixture() { testing::remove_temp_directory(path); }
    vfs::Vfs mount() const {
        const std::array layers{vfs::MountSpec{"leaf", path / "leaf", "data", {}},
            vfs::MountSpec{"parent", path / "parent", "data", {{path / "inputs.meg", "invented-archive", {}}}}};
        auto fs = vfs::Vfs::mount(layers);
        check(bool(fs), "invented mounted source set"); return std::move(fs).value();
    }
};
// Invented one-file MEG, independent public container fixture.
void archive(const std::filesystem::path& path) {
    const std::string name = "DATA/ART/SHADERS/ARCHIVE.FXH", data = "#define ARCHIVE_VALUE 1.0\n";
    std::vector<std::byte> bytes;
    const auto word = [&](std::uint32_t value) {
        for (unsigned n = 0; n < 32; n += 8) bytes.push_back(static_cast<std::byte>(value >> n));
    };
    word(1); word(1);
    bytes.push_back(static_cast<std::byte>(name.size())); bytes.push_back(std::byte{});
    for (char c : name) bytes.push_back(static_cast<std::byte>(c));
    word(0); word(0); word(static_cast<std::uint32_t>(data.size()));
    word(static_cast<std::uint32_t>(30 + name.size())); word(0);
    for (char c : data) bytes.push_back(static_cast<std::byte>(c));
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
void setup(Fixture& f) {
    std::filesystem::create_directories(f.path / "parent");
    write(f.path / "leaf/art/shaders/root.fx", "#include \"sub/branch.fxh\"\n#include <archive.fxh>\nfloat4 main(float4 p:POSITION):POSITION{return p*SCALE;}\n");
    write(f.path / "leaf/art/shaders/sub/branch.fxh", "#include \"../value.fxh\"\n");
    write(f.path / "leaf/art/shaders/value.fxh", "#define SCALE 2.0\n");
    write(f.path / "parent/art/shaders/value.fxh", "#define SCALE 9.0\n");
    write(f.path / "leaf/art/shaders/cycle.fx", "#include \"cycle.fx\"\n");
    write(f.path / "leaf/art/shaders/escape.fx", "#include \"../../../outside.fxh\"\n");
    write(f.path / "leaf/art/shaders/missing.fx", "#include \"missing.fxh\"\n");
    write(f.path / "leaf/art/shaders/repeat.fx", "#include \"empty.fxh\"\n#include \"empty.fxh\"\n");
    write(f.path / "leaf/art/shaders/empty.fxh", "");
    write(f.path / "leaf/art/shaders/sub/polarity.fx", "#include \"choice.fxh\"\n#include <choice.fxh>\n");
    write(f.path / "leaf/art/shaders/sub/choice.fxh", "#define QUOTED_CHOICE 1\n");
    write(f.path / "leaf/art/shaders/choice.fxh", "#define SYSTEM_CHOICE 1\n");
    write(f.path / "leaf/art/shaders/macro.fx", "float4 main(float4 p:POSITION):POSITION{return p*FACTOR;}\n");
    archive(f.path / "inputs.meg");
}
std::span<const std::byte> bytes(std::string_view text) { return std::as_bytes(std::span(text.data(), text.size())); }
void provider_tests() {
    Fixture f; setup(f); auto fs = f.mount();
    assets::ShaderSourceProvider provider(fs, "DATA/ART/SHADERS");
    const auto value = provider.include("data/art/shaders/sub/branch.fxh", "../VALUE.FXH", true, 1024);
    check(value && value.value().source.layer_id == "leaf", "relative path, case normalization, effective override");
    const auto meg = provider.include("data/art/shaders/sub/branch.fxh", "archive.fxh", true, 1024);
    check(meg && meg.value().source.origin == vfs::AssetOrigin::archive, "root fallback includes active MEG");
    for (const auto request : {"../../../../escape", "/absolute", "C:/outside", "\\\\server/share"}) {
        const auto bad = provider.include("data/art/shaders/sub/root.fx", request, true, 1024);
        check(!bad && bad.error().code == vfs::diagnostic_codes::invalid_path, "include path cannot escape logical root");
    }
    check(!provider.read("data/art/shaders/root.fx", 1), "stat size bounds read allocation");
    CompilerSession unavailable{Runtime{}};
    const auto result = preprocess_source(unavailable, provider, "data/art/shaders/root.fx");
    check(result.diagnostic && result.diagnostic->reason == SourceFailure::unavailable &&
        result.diagnostic->phase == SourcePhase::package_load, "activation remains off; unavailable package keeps its phase");
}
CompilerSession session(const std::filesystem::path& package, StageLimits limits = {}) {
    Runtime runtime;
    check(runtime.open(package) == Status::ready, "reviewed package opens");
    return CompilerSession(std::move(runtime), limits);
}
void package_tests(const std::filesystem::path& package) {
    Fixture f; setup(f); auto fs = f.mount();
    assets::ShaderSourceProvider provider(fs, "data/art/shaders");
    auto compiler = session(package);
    const auto nested = preprocess_source(compiler, provider, "data/art/shaders/root.fx");
    check(nested.source && nested.opened == 3 && nested.closed == 3, "nested/root parent callbacks and exact close ownership");
    check(nested.source->dependencies.size() == 4 && nested.source->dependencies[2].source.layer_id == "leaf" &&
        nested.source->dependencies[3].source.origin == vfs::AssetOrigin::archive, "ordered effective dependency ledger");
    const auto polarity = preprocess_source(compiler, provider, "data/art/shaders/sub/polarity.fx");
    check(polarity.source && polarity.opened == 2 && polarity.closed == 2 &&
        polarity.source->dependencies[1].source.logical_path == "data/art/shaders/sub/choice.fxh" &&
        polarity.source->dependencies[2].source.logical_path == "data/art/shaders/choice.fxh",
        "public local flag: quoted includes choose parent, angle includes choose root even when both exist");
    const auto repeated = preprocess_source(compiler, provider, "data/art/shaders/repeat.fx");
    check(repeated.source && repeated.opened == 2 && repeated.closed == 2 && repeated.source->dependencies.size() == 3,
        "empty includes are not silently include-once");
    for (const auto& [name, reason] : std::array{
        std::pair{"cycle.fx", SourceFailure::include_cycle}, std::pair{"escape.fx", SourceFailure::invalid_path},
        std::pair{"missing.fx", SourceFailure::missing_include}}) {
        const auto bad = preprocess_source(compiler, provider, std::string("data/art/shaders/") + name);
        check(!bad.source && bad.diagnostic && bad.diagnostic->reason == reason && bad.opened == bad.closed,
            "typed include failure and deterministic cleanup");
    }
    for (int mode = 0; mode < 5; ++mode) {
        SourceLimits limit;
        if (mode == 0) limit.depth = 1;
        if (mode == 1) limit.include_opens = 1;
        if (mode == 2) limit.total_bytes = 10;
        if (mode == 3) limit.expanded_bytes = 1;
        if (mode == 4) limit.name_bytes = 4;
        const auto bad = preprocess_source(compiler, provider, "data/art/shaders/root.fx", {}, limit);
        check(!bad.source && bad.diagnostic && bad.diagnostic->reason == SourceFailure::budget_exceeded && bad.opened == bad.closed,
            "source work, depth, byte and name budgets");
    }
    StageLimits cancellation; cancellation.cancelled = [](void*) { return true; };
    auto cancelled = session(package, cancellation);
    check(preprocess_source(cancelled, provider, "data/art/shaders/root.fx").diagnostic->reason == SourceFailure::cancelled,
        "cancellation before source acquisition");
    unsigned checkpoints{};
    cancellation.cancellation_context = &checkpoints;
    cancellation.cancelled = [](void* p) { return ++*static_cast<unsigned*>(p) == 3; };
    auto callback_cancelled = session(package, cancellation);
    const auto cancelled_include = preprocess_source(callback_cancelled, provider, "data/art/shaders/root.fx");
    check(cancelled_include.diagnostic && cancelled_include.diagnostic->reason == SourceFailure::cancelled &&
        cancelled_include.opened == 1 && cancelled_include.closed == 1, "callback cancellation closes already owned parent");
    const std::array macros1{SourceMacro{"FACTOR", "1.0"}}, macros2{SourceMacro{"FACTOR", "2.0"}};
    const auto m1 = preprocess_source(compiler, provider, "data/art/shaders/macro.fx", macros1);
    const auto m2 = preprocess_source(compiler, provider, "data/art/shaders/macro.fx", macros2);
    check(m1.source && m2.source && m1.source->manifest_sha256 != m2.source->manifest_sha256, "macros bind preprocessing identity");
    write(f.path / "leaf/art/shaders/value.fxh", "#define SCALE 3.0\n");
    auto changed_fs = f.mount();
    assets::ShaderSourceProvider changed_provider(changed_fs, "data/art/shaders");
    const auto changed = preprocess_source(compiler, changed_provider, "data/art/shaders/root.fx");
    check(changed.source && changed.source->manifest_sha256 != nested.source->manifest_sha256 &&
        changed.source->dependencies[2].sha256 != nested.source->dependencies[2].sha256, "effective include edit invalidates identity");
    for (const auto profile : {"vs_1_1", "ps_1_1", "vs_2_0", "ps_2_0", "vs_3_0", "ps_3_0"}) {
        const bool vertex = std::string_view(profile).starts_with("vs");
        const auto text = vertex ? "float4 main(float4 p:POSITION):POSITION{return p;}" : "float4 main(float4 c:COLOR0):COLOR0{return c;}";
        const auto result = compile_source_stage(compiler, *m1.source, bytes(text), "main", profile);
        check(result.stage.has_value(), "invented exact requested profile compiles");
        const auto program = view_source_program(*result.stage);
        check(program && program.value().major == static_cast<unsigned>(profile[3] - '0') &&
            program.value().minor == static_cast<unsigned>(profile[5] - '0'), "P1 validates exact profile without promotion");
        StageOptions options;
        options.variants = {VariantToken::desktop_glsl_330, VariantToken::preserve_clip_depth,
            VariantToken::preserve_vertex_y, VariantToken::uniform_blocks, VariantToken::combine_image_samplers};
        check(translate_stage(compiler, program.value(), options).artifact.has_value(), "source bytes use existing P2b backend");
    }
    const auto good = compile_source_stage(compiler, *m1.source, m1.source->expanded, "main", "vs_1_1");
    const auto other_macro = compile_source_stage(compiler, *m2.source, m2.source->expanded, "main", "vs_1_1");
    check(good.stage && other_macro.stage && good.stage->manifest_sha256 != other_macro.stage->manifest_sha256, "stage identity binds source manifest");
    for (int n = 0; n < 16; ++n) {
        const auto bad = compile_source_stage(compiler, *m1.source, bytes("invalid invented syntax"), "main", "ps_1_1");
        check(!bad.stage && bad.diagnostic->reason == SourceFailure::compiler_failure, "failure output never transfers ownership");
        check(compile_source_stage(compiler, *m1.source, m1.source->expanded, "main", "vs_2_0").stage.has_value(), "success after failure cleanup");
    }
    check(compile_source_stage(compiler, *m1.source, m1.source->expanded, "absent", "vs_2_0").diagnostic->reason == SourceFailure::compiler_failure,
        "entry mismatch remains compile failure");
    check(compile_source_stage(compiler, *m1.source, m1.source->expanded, "main", "vs_9_9").diagnostic->reason == SourceFailure::unsupported_profile,
        "unknown profile refused before foreign call");
    check(compile_source_stage(compiler, *m1.source, bytes("#include \"root.fx\"\n"), "main", "vs_2_0").diagnostic->reason == SourceFailure::unexpected_include,
        "per-stage include cannot fall through to host filesystem");
    SourceLimits output_limit; output_limit.output_bytes = 1;
    check(compile_source_stage(compiler, *m1.source, m1.source->expanded, "main", "vs_2_0", output_limit).diagnostic->reason == SourceFailure::budget_exceeded,
        "foreign output bounded before copying");
    auto owned = *good.stage;
    compiler = CompilerSession{Runtime{}};
    check(bool(view_source_program(owned)), "owned result survives compiler shutdown and copy");
}
// Private capability hook: source input stays in memory; stdout contains only
// status/profile/count metadata. Entry names and generated text never escape.
void probe(const std::filesystem::path& package) {
    std::string profile, entry, text;
    std::getline(std::cin, profile); std::getline(std::cin, entry);
    for (char c; std::cin.get(c);) {
        check(text.size() < 16U*1024U*1024U, "probe input bound"); text += c;
    }
    Fixture f; setup(f); auto fs = f.mount();
    assets::ShaderSourceProvider provider(fs, "data/art/shaders");
    auto compiler = session(package);
    auto prepared = preprocess_source(compiler, provider, "data/art/shaders/repeat.fx");
    check(prepared.source.has_value(), "probe compiler identity");
    const auto result = compile_source_stage(compiler, *prepared.source, bytes(text), entry, profile);
    std::cout << "profile=" << profile << " success=" << bool(result.stage)
              << " reason=" << (result.diagnostic ? static_cast<int>(result.diagnostic->reason) : -1)
              << " output_bytes=" << (result.stage ? result.stage->bytecode.size() : 0) << '\n';
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && (std::string_view(argv[1]) == "--package-env" || std::string_view(argv[1]) == "--probe-stdin")) {
            std::filesystem::path package;
#ifdef _WIN32
            std::wstring value(32768, L'\0');
            const auto length = GetEnvironmentVariableW(L"EAWR_SHADER_RUNTIME_TEST_PACKAGE", value.data(), static_cast<DWORD>(value.size()));
            if (!length) return 77;
            check(length < value.size(), "package environment path too long");
            value.resize(length);
            package = value;
#else
            const auto* value = std::getenv("EAWR_SHADER_RUNTIME_TEST_PACKAGE");
            if (!value || !*value) return 77;
            package = value;
#endif
            if (std::string_view(argv[1]) == "--probe-stdin") probe(package);
            else package_tests(package);
        } else provider_tests();
        std::cout << "source compiler contracts passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
