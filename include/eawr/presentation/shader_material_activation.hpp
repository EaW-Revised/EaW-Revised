#pragma once
#include "eawr/presentation/shader_activation_snapshot.hpp"
#include "eawr/presentation/shader_effect_source.hpp"
#include "eawr/presentation/shader_material.hpp"
#include <atomic>
#include <functional>
#include <optional>

namespace eawr::presentation::shader_material {
struct MemoryBudget;
struct Resident final : shader_activation::Artifact {
    Program program;
    // Real renderer admission/prewarm ownership, retained with its generation.
    std::shared_ptr<const shader_activation::Artifact> device;
    std::shared_ptr<const void> charge;
};
struct Work final {
    std::size_t source_compilations{}, translations{}, cache_hits{}, admissions{}, prewarms{};
};
// The renderer implementation returns a retained real prewarm receipt. A null
// receipt fails admission; a CPU-only descriptor can never count as resident.
using Prewarm = std::function<std::shared_ptr<const shader_activation::Artifact>(const Program&)>;
// The cache partition is derived from a verified session identity, never disk metadata.
[[nodiscard]] shader_cache::Key source_stage_cache_key(const shader_cache::Key& input,
    std::string_view entry, std::string_view profile, const shader_runtime::CompilerIdentity& compiler);
// Callers may tighten each cap; construction clamps larger values to these maxima.
struct ActivationLimits final {
    std::size_t stages{32U * 1024U * 1024U}, programs{32U * 1024U * 1024U},
        prepared{32U * 1024U * 1024U}, resident{64U * 1024U * 1024U};
};
// All methods and Job stepping use one activation thread. Operations borrow
// this non-movable Compiler, which must outlive every Job using them.
class Compiler final {
public:
    // Selection settings are explicit: zero-initialized P2 settings are the
    // fixed-function tier, not an inferred desktop/device default.
    Compiler(shader_runtime::CompilerSession session, std::shared_ptr<const shader_cache::Cache> cache,
        Prewarm prewarm, shader_techniques::Settings settings, ActivationLimits limits = {});
    Compiler(Compiler&&) = delete;
    Compiler& operator=(Compiler&&) = delete;
    [[nodiscard]] shader_activation::Prepared prepare(const shader_activation::Input& input,
        const shader_activation::Cancellation& cancellation);
    [[nodiscard]] shader_activation::Admitted admit(const shader_activation::Input& input,
        const shader_activation::Prepared& prepared, shader_activation::Variant variant,
        const shader_activation::Cancellation& cancellation);
    // Bind only at activation. Routes receive the published immutable generation.
    [[nodiscard]] shader_activation::Operations operations();
    [[nodiscard]] Work work() const noexcept { return work_; }
    [[nodiscard]] std::size_t resident_bytes() const noexcept;
    // Bounded wrapper refusal for activation diagnostics and native controls;
    // contains no shader source or compiler-owned message.
    [[nodiscard]] const std::optional<core::Diagnostic>& wrapper_refusal() const noexcept { return wrapper_refusal_; }
private:
    shader_runtime::CompilerSession session_;
    std::shared_ptr<const shader_cache::Cache> cache_;
    Prewarm prewarm_;
    shader_techniques::Settings settings_;
    Work work_;
    ActivationLimits limits_;
    std::shared_ptr<MemoryBudget> budget_;
    std::optional<core::Diagnostic> wrapper_refusal_;
    [[nodiscard]] shader_runtime::StageResult stage(const shader_activation::Input& input,
        const assets::EffectProgram& program, const shader_activation::Cancellation& cancellation);
};
// Exact activated effect lookup only. Late effects/variants never compile.
[[nodiscard]] const Resident* find(const shader_activation::Generation& generation, std::string_view effect) noexcept;
} // namespace eawr::presentation::shader_material
