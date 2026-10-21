#pragma once

#include "eawr/platform/live_game_session.hpp"
#include "eawr/campaign/game_session.hpp"

// Engine-only access to the owner agreed by the lobby. Presentation retains the opaque handle.
namespace eawr::platform {
struct PreparedLiveAgreement {
    std::shared_ptr<campaign::GameSession> session;
};
struct AgreedCampaignAccess {
    [[nodiscard]] static campaign::GameSession& session(PreparedLiveCampaign&) noexcept;
};
} // namespace eawr::platform
