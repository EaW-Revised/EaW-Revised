#pragma once

#include "eawr/presentation/ui/lan_lobby.hpp"

// Header-only fixture: only contract tests compile this fake service.
namespace eawr::test::ui {

using presentation::ui::LobbyAttemptId;
using presentation::ui::OnlineInvitationTarget;
using presentation::ui::OnlineJoinResult;
using presentation::ui::OnlineJoinStatus;
using presentation::ui::normalize_invitation_code;

enum class FixtureInvitationState : std::uint8_t { available, consumed, expired };
// Test fixture only: no service access token or certificate material is stored here.
class FakeOnlineLobbySource final : public presentation::ui::OnlineLobbySource {
public:
    explicit FakeOnlineLobbySource(std::string service) : service_(std::move(service)) {}
    void add_invitation(std::string code, FixtureInvitationState state = FixtureInvitationState::available);
    void set_access_allowed(bool allowed) noexcept { access_allowed_ = allowed; }
    void set_trusted(bool trusted) noexcept { trusted_ = trusted; }
    [[nodiscard]] std::string service() const override { return service_; }
    void begin_join(presentation::ui::LobbyAttemptId attempt, const presentation::ui::OnlineInvitationTarget& target) override;
    void cancel(presentation::ui::LobbyAttemptId attempt) override;
    void pump() override;
    [[nodiscard]] std::vector<presentation::ui::OnlineJoinResult> take_results() override;
private:
    struct Invitation { std::string code; FixtureInvitationState state; };
    struct Pending { presentation::ui::LobbyAttemptId attempt; presentation::ui::OnlineInvitationTarget target; };
    std::string service_;
    std::vector<Invitation> invitations_;
    std::optional<Pending> pending_;
    std::vector<presentation::ui::OnlineJoinResult> results_;
    bool access_allowed_{true};
    bool trusted_{true};
};

inline void FakeOnlineLobbySource::add_invitation(std::string code, const FixtureInvitationState state) {
    const auto canonical = normalize_invitation_code(code);
    if (!canonical) return;
    for (auto& invitation : invitations_) {
        if (invitation.code == *canonical) { invitation.state = state; return; }
    }
    invitations_.push_back({*canonical, state});
}

inline void FakeOnlineLobbySource::begin_join(const LobbyAttemptId attempt, const OnlineInvitationTarget& target) {
    if (pending_) results_.push_back({attempt, OnlineJoinStatus::busy});
    else pending_ = Pending{attempt, target};
}

inline void FakeOnlineLobbySource::cancel(const LobbyAttemptId attempt) {
    if (pending_ && pending_->attempt == attempt) pending_.reset();
    // Already queued results may still arrive; the selection's generation rejects them.
}

inline void FakeOnlineLobbySource::pump() {
    if (!pending_) return;
    auto pending = std::move(*pending_);
    pending_.reset();
    OnlineJoinStatus status = OnlineJoinStatus::unavailable;
    if (pending.target.service != service_ || service_.empty()) status = OnlineJoinStatus::not_configured;
    else if (!trusted_) status = OnlineJoinStatus::trust_failed;
    else if (!access_allowed_) status = OnlineJoinStatus::access_denied;
    else {
        for (auto& invitation : invitations_) {
            if (invitation.code == pending.target.code && invitation.state == FixtureInvitationState::available) {
                invitation.state = FixtureInvitationState::consumed;
                status = OnlineJoinStatus::paired;
                break;
            }
        }
    }
    results_.push_back({pending.attempt, status});
}

inline std::vector<OnlineJoinResult> FakeOnlineLobbySource::take_results() {
    std::vector<OnlineJoinResult> results;
    results.swap(results_);
    return results;
}

} // namespace eawr::test::ui
