#pragma once

#include "galactic_lan_match.hpp"
#include "eawr/net/galactic_lobby.hpp"
#include "eawr/net/lobby_session.hpp"
#include "eawr/platform/live_game_session.hpp"

namespace eawr::campaign { class CampaignSetupPlan; }
namespace eawr::presentation::godot_backend {

// The exact local owner and display identities of a committed Galactic Conquest room.
// Simulation types remain opaque to the viewer's launch routing.
struct GalacticLaunch {
    net::GalacticLobbyOptions options;
    net::LobbySnapshot roster;
    std::uint32_t seat{};
    std::shared_ptr<GalacticLanMatch> match;
    std::shared_ptr<const campaign::CampaignSetupPlan> plan;
    std::shared_ptr<platform::PreparedLiveAgreement> owner;
    [[nodiscard]] const sim::galactic::GalacticSetup& layout() const;
    // Startup-owner reads before start(); active scenes consume LiveGameSession::frame(seat).
    [[nodiscard]] std::shared_ptr<const sim::galactic::GalacticSnapshot> snapshot() const;
    [[nodiscard]] std::shared_ptr<const sim::galactic::GalacticSnapshot> snapshot(sim::galactic::SeatId) const;
    [[nodiscard]] bool native_ai_installed() const;
    std::shared_ptr<const platform::CampaignServiceConfiguration> services;
    [[nodiscard]] core::Result<std::unique_ptr<platform::LiveGameSession>> start(platform::LiveGameSession::Options) const;
};

} // namespace eawr::presentation::godot_backend
