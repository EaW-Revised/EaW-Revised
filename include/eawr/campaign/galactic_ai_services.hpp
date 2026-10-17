#pragma once

#include "eawr/campaign/conflict.hpp"
#include "eawr/script/foc/galactic_ai.hpp"
#include "eawr/sim/galactic/snapshot.hpp"

#include <array>
#include <functional>

namespace eawr::script::foc::ai { class GalacticFunding; }

namespace eawr::campaign {
class GameSession;
// WGA-50..56: retained plan listeners observe actual admission/return. They do
// not choose the battle result or service the frozen parent manager.
enum class GalacticAiBattleKind : std::uint8_t { entered, returned }; // entered selects a real tactical child
struct GalacticAiBattleNotice {
    GalacticAiBattleKind kind{};
    std::uint64_t synchronization_tick{};
    const ConflictRecord& conflict;
    const sim::galactic::GalacticSnapshot& world;
    // Current finalized result at committed return. latest_tactical_result()
    // is published after this notice; listeners must consume this pointer.
    const BattleResult* result{};
    bool auto_resolve{};
};
struct GalacticAiServices {
    std::shared_ptr<script::foc::galactic_ai::StateContributor> state;
    std::function<core::Result<void>(const GalacticAiBattleNotice&)> battle;
};
// Fresh native owners are constructed after world/child/Lua reconstruction.
// Restore publishes perception before goals, goals before funding, funding
// before execution reservations/routes, then producer billing identities.
struct GalacticAiContributors {
    std::shared_ptr<script::foc::galactic_ai::StateContributor> lifecycle, perception, goals;
    std::shared_ptr<script::foc::ai::GalacticFunding> funding;
    std::shared_ptr<script::foc::galactic_ai::StateContributor> execution, production;
    // Live goal identities are restored before this check. Dead obligations
    // may outlive their goals and must not be discarded for missing live IDs.
    std::function<core::Result<void>(const std::map<sim::galactic::PlayerId, sim::galactic::funding::Ledger>&)> validate_funding;
    // Qualified retained providers restore after parent/goals/funding, before
    // execution consumers. Identity keys also enter the canonical native hash.
    std::map<std::string, std::shared_ptr<script::foc::galactic_ai::StateContributor>, std::less<>> external;
    // Publish derived copies of the decoded funding owner before external/E
    // reconstruction. The same hook runs during transactional rollback.
    std::function<core::Result<void>()> after_funding_restore;
    // Pure derived publication after every native participant is restored.
    // Failure rolls native participants back before republishing their copies.
    std::function<core::Result<void>()> after_restore;
};
class ComposedGalacticAiState final : public script::foc::galactic_ai::StateContributor {
public:
    [[nodiscard]] static core::Result<std::shared_ptr<ComposedGalacticAiState>> create(GalacticAiContributors);
    [[nodiscard]] core::Result<script::foc::galactic_ai::NativeState> capture() const override;
    [[nodiscard]] core::Result<void> restore(const script::foc::galactic_ai::NativeState&) override;
    [[nodiscard]] core::Result<std::string> state_hash() const override;
    [[nodiscard]] core::Result<std::string> sync_hash() const override;
    [[nodiscard]] core::Result<std::string> record_sync_hash(bool cold = false) const override;
    // Deterministic work receipt, excluded from saves/hashes.
    [[nodiscard]] std::uint64_t digest_generations() const noexcept { return digest_generations_; }
    [[nodiscard]] std::uint64_t digest_requests() const noexcept { return digest_requests_; }
    [[nodiscard]] const std::array<std::uint64_t, 6>& owner_encodes() const noexcept { return owner_encodes_; }
    [[nodiscard]] std::map<std::string, std::uint64_t, std::less<>> external_encodes() const;
    [[nodiscard]] std::map<std::string, std::uint64_t, std::less<>> sync_encodes() const;
    [[nodiscard]] std::uint64_t sync_generations() const noexcept { return sync_generations_; }
    [[nodiscard]] bool full_verification_enabled() const noexcept;
private:
    using Contributor = std::shared_ptr<script::foc::galactic_ai::StateContributor>;
    explicit ComposedGalacticAiState(std::array<Contributor, 6>);
    std::array<Contributor, 6> owners_;
    std::optional<core::Diagnostic> failure_;
    struct OwnerCache {
        std::optional<std::uint64_t> revision;
        std::vector<std::uint8_t> bytes;
        std::string digest;
        std::uint64_t encodes{};
    };
    mutable std::array<OwnerCache, 6> owner_cache_;
    std::map<std::string, Contributor, std::less<>> external_;
    std::function<core::Result<void>()> after_restore_;
    mutable std::map<std::string, OwnerCache, std::less<>> external_cache_;
    mutable std::map<std::string, OwnerCache, std::less<>> sync_cache_;
    mutable std::string cached_sync_digest_;
    mutable std::uint64_t sync_generations_{};
    mutable bool sync_validation_pending_{true};
    mutable std::array<std::uint64_t, 6> owner_encodes_{};
    bool verify_hash_{};
    mutable std::string cached_digest_;
    mutable std::uint64_t digest_generations_{};
    mutable std::uint64_t digest_requests_{};
};
[[nodiscard]] core::Result<std::shared_ptr<ComposedGalacticAiState>> install_composed_galactic_ai(
    GameSession&, GalacticAiContributors,
    std::function<core::Result<void>(const GalacticAiBattleNotice&)> battle = {});
using GalacticAiContributorsFactory = std::function<core::Result<GalacticAiContributors>(sim::galactic::GalacticSession&)>;
// Optional runtime adapter for observation; canonical capture/restore/hash
// delegate to the same retained composed owner.
using GalacticAiStateAdapter = std::function<core::Result<std::shared_ptr<script::foc::galactic_ai::StateContributor>>(
    std::shared_ptr<ComposedGalacticAiState>)>;
[[nodiscard]] core::Result<std::shared_ptr<ComposedGalacticAiState>> install_composed_galactic_ai(
    GameSession&, GalacticAiContributorsFactory,
    std::function<core::Result<void>(const GalacticAiBattleNotice&)> battle = {}, GalacticAiStateAdapter = {});
struct GalacticAiSave {
    std::uint64_t completed_tick{}, galactic_frame{};
    std::optional<script::foc::galactic_ai::NativeState> state;
};
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_galactic_ai_save(const GalacticAiSave&);
[[nodiscard]] core::Result<GalacticAiSave> decode_galactic_ai_save(std::span<const std::uint8_t>);
} // namespace eawr::campaign
