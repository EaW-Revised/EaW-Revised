#pragma once
#include "abi.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/presentation/shader_stage.hpp"
#include <bit>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace eawr::presentation::shader_runtime {
struct CompilerSession::Impl {
    Runtime runtime;
    StageLimits limits;
    abi::Api api;
    CompilerIdentity identity;
    std::optional<StageDiagnostic> unavailable;
    explicit Impl(Runtime value, StageLimits bounds)
        : runtime(std::move(value)), limits(bounds) {}
};
struct StageSessionAccess {
    using Impl = CompilerSession::Impl;
    static Impl &get(CompilerSession &session);
};
namespace detail {
using Session = StageSessionAccess::Impl;
// A failed compile can leave internally freed nonnull code fields. This owner
// is shared by production translation and the native late-failure regressions.
struct CompileOutput {
    abi::Api &api;
    abi::Code code;
    char *messages{};
    int result;
    CompileOutput(abi::Api &functions, const abi::Compile &info)
        : api(functions),
          result(api.vkd3d_shader_compile(&info, &code, &messages)) {}
    ~CompileOutput() {
        if (result >= 0)
            api.vkd3d_shader_free_shader_code(&code);
        api.vkd3d_shader_free_messages(messages);
    }
    CompileOutput(const CompileOutput &) = delete;
    CompileOutput &operator=(const CompileOutput &) = delete;
};
struct Failure {
    DiagnosticPhase phase;
    DiagnosticReason reason;
    const char *detail;
};
[[noreturn]] inline void refuse(DiagnosticPhase phase, DiagnosticReason reason,
                                const char *message) {
    throw Failure{phase, reason, message};
}
inline void require(bool condition, DiagnosticPhase phase,
                    DiagnosticReason reason, const char *message) {
    if (!condition)
        refuse(phase, reason, message);
}
inline StageDiagnostic diagnostic(const Failure &failure) {
    return {failure.phase, failure.reason, Severity::error, std::nullopt,
            failure.detail};
}
inline void checkpoint(const StageLimits &limits, DiagnosticPhase phase) {
    require(!limits.cancelled || !limits.cancelled(limits.cancellation_context),
            phase, DiagnosticReason::cancelled, "shader operation cancelled");
}
inline Digest digest(std::span<const std::byte> value) {
    return std::bit_cast<Digest>(core::sha256(
        {reinterpret_cast<const std::uint8_t *>(value.data()), value.size()}));
}
inline Digest digest(std::string_view value) {
    return digest(std::as_bytes(std::span(value.data(), value.size())));
}
std::string hex(const Digest &digest);
std::string bounded_text(const char *value, const StageLimits &limits,
                         DiagnosticPhase phase);
std::vector<std::byte> options_manifest(const StageOptions &options);
std::vector<std::byte> identity_manifest(const StageIdentity &identity);
void validate_artifact(const StageArtifact &artifact,
                       const StageLimits &limits);
void validate_spirv(std::span<const std::uint32_t> words,
                    assets::EffectProgramStage stage,
                    const StageLimits &limits);
struct ScannedSignature {
    std::string semantic;
    std::uint32_t semantic_index{}, index{}, mask{}, used_mask{}, sysval{};
};
struct Scan {
    std::vector<abi::Descriptor> descriptors;
    std::vector<abi::Pair> pairs;
    std::vector<ScannedSignature> input, output;
};
Scan scan(Session &session, const assets::EffectProgram &program);
PreparedStageRequest prepare(Session &session,
                             const assets::EffectProgram &program,
                             const StageOptions &options,
                             Scan *scanned = nullptr);
StageReflection reflect(Session &session, abi::H compiler,
                        const StageOptions &options, const Scan &scan);
} // namespace detail
inline StageSessionAccess::Impl &
StageSessionAccess::get(CompilerSession &session) {
    if (!session.impl_)
        detail::refuse(DiagnosticPhase::package_load,
                       DiagnosticReason::dependency_unavailable,
                       "compiler session moved from");
    return *session.impl_;
}
} // namespace eawr::presentation::shader_runtime
