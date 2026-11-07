#pragma once
#include "eawr/assets/shader_source.hpp"
#include "eawr/presentation/shader_stage.hpp"

namespace eawr::presentation::shader_runtime {
struct SourceLimits {
    std::size_t file_bytes{4U*1024U*1024U}, total_bytes{16U*1024U*1024U};
    std::size_t expanded_bytes{16U*1024U*1024U}, output_bytes{4U*1024U*1024U};
    std::size_t include_opens{1024}, depth{32}, name_bytes{4096}, macros{256};
};
struct SourceMacro { std::string name, value; };
enum class SourcePhase { preprocess, stage_compile, package_load };
enum class SourceFailure {
    unavailable, dependency_integrity, dependency_abi, invalid_path, missing_include, source_io, budget_exceeded,
    cancelled, include_cycle, callback_ownership, preprocess_failure,
    unsupported_profile, invalid_options, compiler_failure, invalid_output,
    unexpected_include
};
struct SourceDiagnostic {
    SourcePhase phase{};
    SourceFailure reason{};
    DiagnosticLocation location; // logical coordinates only; unknown lines stay absent
};
struct SourceDependency { assets::Source source; Digest sha256{}; };
struct PreprocessedSource {
    std::vector<std::byte> expanded;
    std::vector<SourceDependency> dependencies; // root first, then EACH successful open
    std::vector<SourceMacro> macros; // original order, including duplicate definitions
    CompilerIdentity compiler;
    Digest manifest_sha256{}, expanded_sha256{};
};
struct SourcePreparation {
    std::optional<PreprocessedSource> source;
    std::optional<SourceDiagnostic> diagnostic;
    std::size_t opened{}, closed{};
};
struct SourceStage {
    std::vector<std::byte> bytecode;
    assets::Source source;
    std::string entry, profile;
    Digest manifest_sha256{};
};
// Exact signatures/options and bounded measured scope, hashed into each result.
[[nodiscard]] std::string_view source_compiler_contract() noexcept;
struct SourceCompilation {
    std::optional<SourceStage> stage;
    std::optional<SourceDiagnostic> diagnostic;
};
// Synchronous activation-only work. Limits bound our acquisition/callback work
// and accepted foreign outputs, not compiler-internal CPU time or allocations.
[[nodiscard]] SourcePreparation preprocess_source(CompilerSession& session,
    const assets::ShaderSourceProvider& provider, std::string_view logical,
    std::span<const SourceMacro> macros = {}, SourceLimits limits = {});
// SOURCE-B supplies parser-identified stage text from the single expanded graph.
// No mounted include acquisition or macro application occurs at this boundary.
[[nodiscard]] SourceCompilation compile_source_stage(CompilerSession& session,
    const PreprocessedSource& source, std::span<const std::byte> stage_text,
    std::string_view entry, std::string_view profile, SourceLimits limits = {});
[[nodiscard]] core::Result<assets::EffectProgram> view_source_program(const SourceStage& stage);
} // namespace eawr::presentation::shader_runtime
