#pragma once

#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/core/result.hpp"
#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/sim/galactic/mp_roster.hpp"
#include "eawr/sim/match_identity.hpp"
#include "eawr/units/galactic_types.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// #3768: what one peer derives, from its own installed content, for a multiplayer campaign offer. The net layer
// (net/galactic_setup.hpp) carries the roster and the host's claims; this layer turns the roster into the galaxy
// setup, the effective content identities and the tick-zero digest, and the offer is accepted only when those
// equal the host's. Nothing the host claims replaces a local computation: claims enter an identity domain only
// next to the local value, so a differing claim makes the domain differ and the setup service refuses the offer
// before any shared step.
namespace eawr::campaign {

// The identity domain names this layer produces. gc-tick-zero must equal net::galactic_tick_zero_domain.
namespace mp_domains {
inline constexpr std::string_view campaign = "gc-campaign", content = "gc-content", map = "gc-map",
    story = "gc-story", ai_data = "gc-ai-data", ai_controllers = "gc-ai-controllers", resolver = "gc-resolver",
    rules = "gc-rules", tick_zero = "gc-tick-zero";
} // namespace mp_domains

struct MpPreparedSession {
    std::shared_ptr<GameSession> session;
    // Opaque platform services belonging to this exact prepared owner. Not simulation state.
    std::shared_ptr<const void> runtime_services;
};

struct MpLocalContent {
    const data::galactic::GalacticData* data{};
    const units::GalacticTypeTable* types{};
    // Hex digests of the mounted content closure: every Lua script (story plots and AI scripts) and every XML
    // file (AI data included), as skirmish::read_data_identity computes them for the same mounts.
    std::string script_manifest_digest, xml_digest;
    // This build's batch limits (net::max_batch_commands / max_batch_bytes); the offer must carry the same.
    std::uint32_t command_quota{}, byte_quota{};
    // Production plans through plan_live_campaign_setup (fiscal policy, difficulty); the default is
    // plan_campaign_setup over the same data.
    std::function<core::Result<CampaignSetupPlan>(const CampaignSetupOptions&, std::string_view campaign,
        std::string_view difficulty)> planner;
    // Production supplies the selected AI player type per faction; the default derives the configuration from the
    // faction, difficulty and the two digests above, so differing AI content or difficulty yields differing bytes.
    std::function<core::Result<std::vector<std::uint8_t>>(std::string_view faction, sim::galactic::PlayerId player,
        std::string_view difficulty)> ai_configuration;
    // Optional production owner, fully configured before computing the tick-zero claim.
    // The aliasing shared pointer retains the initialized root and all runtime services.
    std::function<core::Result<MpPreparedSession>(const CampaignSetupPlan&, std::string_view difficulty)> prepare_session{};
    // Production prepares the stock placement services so tick zero includes the starting forces; absent, tick
    // zero is the session built from the planned galaxy alone.
    std::function<core::Result<CampaignSetupServices>(const CampaignSetupPlan&)> services;
};

struct MpSessionTerms {
    std::uint64_t seed{};
    std::uint32_t command_quota{}, byte_quota{};
};

struct MpAgreement {
    sim::MatchIdentity content;                // derived locally; the net layer pairs it with the build identity
    std::array<std::uint8_t, 32> tick_zero{};  // this peer's own computation
    std::vector<std::vector<std::uint8_t>> ai_configuration; // per roster faction (empty for humans), local
    std::shared_ptr<const CampaignSetupPlan> plan;
    std::shared_ptr<GameSession> session;      // at tick zero, never stepped here; the adapter takes it over
    std::shared_ptr<const void> runtime_services{}; // travels with that owner across re-offers
};

// Fills the host's claims from local content: the AI controller configuration of every non-human faction and
// the tick-zero digest. Everything else in the draft is the lobby's choice and is validated, not changed.
[[nodiscard]] core::Result<sim::galactic::MpRoster> seal_mp_roster(const MpLocalContent&,
    sim::galactic::MpRoster draft, const MpSessionTerms&);

// Seals a draft and retains the same local preparation used for its identity. The draft changes only on success.
[[nodiscard]] core::Result<MpAgreement> seal_mp_agreement(const MpLocalContent&,
    sim::galactic::MpRoster& draft, const MpSessionTerms&);

// Independent derivation for a received (or sealed) roster. Fails when the content cannot run the roster at all:
// campaign missing or not multiplayer, unknown factions, an unplayable faction on a human seat, a roster the galaxy
// refuses. Differences from the host's claims are not failures here; they surface as identity domains that differ.
[[nodiscard]] core::Result<MpAgreement> derive_mp_agreement(const MpLocalContent&,
    const sim::galactic::MpRoster&, const MpSessionTerms&);

} // namespace eawr::campaign
