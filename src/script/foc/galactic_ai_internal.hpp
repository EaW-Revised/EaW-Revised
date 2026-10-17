#pragma once

#include "galactic_ai.hpp"
#include "eawr/core/record_digest.hpp"

namespace eawr::script::foc::galactic_ai {
inline core::Diagnostic lifecycle_error(std::string message) {
    core::Diagnostic error;
    error.code = "EAWR-GCAI-0001";
    error.message = std::move(message);
    return error;
}
struct Manager::Impl {
    ai::AiData data;
    std::vector<PlanInput> plans;
    Capabilities capabilities;
    LifecycleState state;
    std::string configuration_digest;
    core::RecordDigest records;
    void update_record(gc::PlayerId player = 0);
    [[nodiscard]] core::RecordDigest rebuild_records() const;
    // Immutable policy copies shared by every due service input. A snapshot is
    // current only while its revision equals the player's policy revision; each
    // policy write and every restore bumps the revision and drops the copy.
    struct PolicyCache {
        std::uint64_t revision{1}, built_revision{};
        std::shared_ptr<const TemplatePolicy> snapshot;
    };
    std::map<gc::PlayerId, PolicyCache> policy_cache;
    PolicySnapshotStats policy_stats;
    void policy_changed(gc::PlayerId);
    void policy_restored();
    [[nodiscard]] std::shared_ptr<const TemplatePolicy> policy_snapshot(const PlayerLifecycle&);
    std::vector<ServiceTrace> traces;
    std::uint64_t traces_dropped{};
    std::optional<core::Diagnostic> failure;
    [[nodiscard]] core::Result<bool> positive(const PlayerSelection&, std::string_view) const;
    [[nodiscard]] core::Result<void> initialize(PlayerLifecycle&);
    [[nodiscard]] core::Result<void> templates(PlayerLifecycle&);
};
} // namespace eawr::script::foc::galactic_ai
