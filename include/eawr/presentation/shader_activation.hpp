#pragma once
#include "eawr/presentation/shader_translation_report.hpp"
#include "eawr/presentation/shader_translation_cache.hpp"
#include <atomic>
#include <compare>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

// SAC-01..06: CPU activation lifecycle. No draw-time compiler or renderer switch.
namespace eawr::presentation::shader_activation {
namespace report = shader_report;
enum class Policy { allowed, opted_out, malformed };
struct Artifact;
struct Diagnostic {
    report::FailureStage stage{};
    report::Reason reason{};
    report::Severity severity{report::Severity::error};
    std::string logical{};
    std::optional<report::Position> position{};
};
struct Input {
    std::string effect, sha256, layer, mount, logical, origin;
    report::SelectedForm form{report::SelectedForm::compiled};
    Policy policy{Policy::allowed};
    std::string adapter; // nonempty only for a qualified same-name adapter
    std::vector<std::byte> bytes; // owned activation snapshot, never reopened
    // Owned source/include closure or other producer metadata, never a VFS borrow.
    std::shared_ptr<const Artifact> context;
    std::optional<Diagnostic> snapshot_failure;
    std::optional<shader_cache::Key> cache_key;
};
struct Variant {
    std::uint64_t technique{}, state{};
    friend bool operator==(const Variant&, const Variant&) = default;
    friend auto operator<=>(const Variant&, const Variant&) = default;
};
// Owners derive immutable compiler/renderer payloads. A retained generation
// pins their lifetime independently of cache eviction or subsequent activation.
struct Artifact { virtual ~Artifact() = default; };
struct Prepared {
    std::shared_ptr<const Artifact> artifact;
    std::vector<Variant> variants; // complete supported finite reachable set
    std::vector<Diagnostic> diagnostics;
};
struct Admitted {
    std::shared_ptr<const Artifact> artifact;
    std::vector<Diagnostic> diagnostics;
};
struct VariantResult {
    Variant variant;
    report::Route route{report::Route::surface_omitted};
    std::shared_ptr<const Artifact> artifact;
    std::vector<Diagnostic> diagnostics;
};
struct EffectResult {
    Input input;
    std::vector<Diagnostic> diagnostics;
    std::vector<VariantResult> variants;
    report::Route fallback{report::Route::surface_omitted};
};
struct Generation {
    std::uint64_t id{};
    std::string chain_identity;
    std::vector<EffectResult> effects;
};
struct Limits {
    std::size_t effects{65536}, input_bytes{64U * 1024U * 1024U};
    std::size_t identifier_bytes{4096}, variants{262144}, diagnostics{262144};
    std::size_t variant_requests{1048576}; // pre-dedup callback output budget
};
struct Cancellation {
    std::atomic<bool> requested{false};
    [[nodiscard]] bool cancelled() const noexcept { return requested.load(); }
};
// prepare includes validated cache reuse OR translation and finite variant
// planning. admit must include real program admission AND prewarm; a payload
// without successful prewarm is not success. Callbacks run only from step().
struct Operations {
    std::function<Prepared(const Input&, const Cancellation&)> prepare;
    std::function<Admitted(const Input&, const Prepared&, Variant, const Cancellation&)> admit;
};
enum class State { running, complete, cancelled, failed };
struct Progress {
    State state{State::running};
    std::size_t effects{}, prepared{}, variants{}, admitted{}, failed_variants{}, work{};
};
class Job final {
public:
    Job(std::uint64_t id, std::string chain_identity, std::vector<Input> inputs,
        Operations operations, Limits limits = {});
    // Single worker owns step/progress/result. Other threads may cancel only.
    // One unit is one prepare or one admission/prewarm, including failed calls.
    void step(std::size_t work_budget);
    void cancel() noexcept;
    [[nodiscard]] Progress progress() const noexcept { return progress_; }
    [[nodiscard]] std::shared_ptr<const Generation> result() const;
private:
    friend class Publisher;
    std::shared_ptr<Generation> generation_;
    Operations operations_;
    Limits limits_;
    Cancellation cancellation_;
    Progress progress_;
    Prepared prepared_;
    std::size_t effect_{}, variant_{}, diagnostic_count_{};
    bool preparing_{true};
    void advance();
    bool retain(std::size_t count);
};
class Publisher final {
public:
    // Reserve before dispatching a job. IDs are monotonic and never reused.
    [[nodiscard]] std::uint64_t begin();
    // Serializes invalidation with publication; older active readers keep pins.
    void cancel(std::uint64_t id);
    [[nodiscard]] bool publish(const Job& job);
    [[nodiscard]] std::shared_ptr<const Generation> active() const;
private:
    mutable std::mutex mutex_;
    std::uint64_t latest_{};
    bool cancelled_{};
    std::shared_ptr<const Generation> active_;
};
// Pure lookup: unknown/late variants fall back without scheduling any work.
[[nodiscard]] report::Route route(const EffectResult& effect, Variant variant) noexcept;
} // namespace eawr::presentation::shader_activation
