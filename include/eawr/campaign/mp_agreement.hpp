#pragma once

#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/core/result.hpp"
#include "eawr/core/prepared_content.hpp"
#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/sim/galactic/mp_roster.hpp"
#include "eawr/sim/match_identity.hpp"
#include "eawr/units/galactic_types.hpp"

#include <atomic>
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
    std::vector<core::LoadNote> load_notes{};
    core::PreparedContentIdentity provider_identity{}; // Immutable future simulation inputs retained by runtime_services.
};

struct MpLocalContent {
    const data::galactic::GalacticData* data{};
    const units::GalacticTypeTable* types{};
    // Hex digests of the mounted content closure: every Lua script (story plots and AI scripts) and every XML
    // file (AI data included), as skirmish::read_data_identity computes them for the same mounts.
    std::string script_manifest_digest, xml_digest;
    // Complete loaded catalog input commitment, including authored includes outside the conventional XML prefix.
    std::string catalog_digest;
    std::string mounted_chain_digest; // Ordered checksums of the verified consumed simulation closure.
    std::vector<core::ContentLayerIdentity> mounted_layers; // Labels never enter agreement.
    std::function<core::Result<void>()> verify_preparation_content;
    // The difficulty entries each AI player type maps its levels to; the lobby's campaign list reads it. Absent: no campaign
    // states a difficulty list.
    std::function<std::array<std::string, 3>(std::string_view player_type)> ai_levels;
    // This build's batch limits (net::max_batch_commands / max_batch_bytes); the offer must carry the same.
    std::uint32_t command_quota{}, byte_quota{};
    // Test seam: the Lua sandbox policy identity this peer claims in the rules domain, to stand in for a peer on another
    // build. Empty (always, in production): this build's script::authoritative::sandbox_policy_identity.
    std::string sandbox_policy;
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
    // Every invocation owns fresh mounted services and directly imports the checkpoint.
    std::function<core::Result<MpPreparedSession>(const CampaignSetupPlan&, std::string_view,
        std::span<const std::uint8_t>)> restore_session;
    // Host-side conversion of a save whose finished-battle records came from another build
    // (checkpoint::recover_checkpoint_history), with the same fresh mounted services as restore_session.
    std::function<core::Result<checkpoint::RecoveredCheckpoint>(const CampaignSetupPlan&, std::string_view,
        std::span<const std::uint8_t>)> recover_history;
    // Production prepares the stock placement services so tick zero includes the starting forces; absent, tick
    // zero is the session built from the planned galaxy alone.
    std::function<core::Result<CampaignSetupServices>(const CampaignSetupPlan&)> services;
    // A custom services callback declares its complete immutable runtime inputs for concurrent Start.
    std::array<std::uint8_t, 32> service_identity{};
};

struct MpSessionTerms {
    std::uint64_t seed{};
    std::uint32_t command_quota{}, byte_quota{};
    // Absent: follow the campaign entry (multiplayer entries use multiplayer rules, single-player entries the
    // authored single-player rules). Every peer derives the same value from the agreed campaign.
    std::optional<CampaignRulesFlavour> rules_flavour{};
};

struct MpAgreement {
    std::optional<checkpoint::GameResumeMetadata> resume;
    sim::MatchIdentity content;                // derived locally; the net layer pairs it with the build identity
    std::array<std::uint8_t, 32> tick_zero{};  // this peer's own computation
    std::vector<std::vector<std::uint8_t>> ai_configuration; // per roster faction (empty for humans), local
    std::shared_ptr<const CampaignSetupPlan> plan;
    std::shared_ptr<GameSession> session;      // at tick zero, never stepped here; the adapter takes it over
    std::shared_ptr<const void> runtime_services{}; // travels with that owner across re-offers
    std::vector<core::LoadNote> load_notes; // local diagnostics only; never sent to peers
    core::PreparedContentIdentity provider_identity{};
    bool provider_identity_required{};
};

// New-campaign preparation only. Supplements existing tick-zero domains with complete owner archives and the
// immutable provider closure; checkpoint Start does not call this or change its saved identity domains.
[[nodiscard]] core::Result<std::vector<sim::IdentityDomain>> prepared_mp_domains(const MpAgreement&);

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

// #7869: a human joining a loaded save: the faction it plays and the seat id the room agreed for it. With `rejoin` (#7876) the
// seat is a departed human seat of the save that a joining player takes back.
struct MpExtraSeat {
    sim::galactic::PlayerId player{};
    sim::SeatId seat{};
    bool rejoin{};
    friend bool operator==(const MpExtraSeat&, const MpExtraSeat&) = default;
};
// Seats the extra humans into a just-restored session (GameSession::admit_human_seats, and reseat_human_seats for the departed
// seats taken back) and checks that each got exactly the agreed seat id on the agreed faction, else refuses
// "extra_seat_mismatch". Seats that are already in place (a recovery base saved after the humans joined) are left as they are.
[[nodiscard]] core::Result<void> admit_mp_extra_seats(GameSession&, std::span<const MpExtraSeat>);

// Derive local installed identities and directly restore, never run placement or player-load fixups.
// `cancel`, when given, is checked between the phases (roster check, local identity derivation, the other-flavour retry,
// the state comparison): once it is set the restore stops with the reason restore_cancelled (#7858).
// `extras` seat the humans that join the loaded save (admit_mp_extra_seats), after the saved state hash has been verified.
[[nodiscard]] core::Result<MpAgreement> restore_mp_agreement(const MpLocalContent&,
    const sim::galactic::MpRoster&, const MpSessionTerms&, std::span<const std::uint8_t>,
    const checkpoint::GameResumeMetadata&, const std::atomic<bool>* cancel = nullptr,
    std::span<const MpExtraSeat> extras = {});

// The host's "load without old battle history": the saved game's setup checks of restore_mp_agreement, then
// local.recover_history. The converted save is an ordinary checkpoint; select it like any other save.
[[nodiscard]] core::Result<checkpoint::RecoveredCheckpoint> recover_mp_history(const MpLocalContent&,
    const sim::galactic::MpRoster&, const MpSessionTerms&, std::span<const std::uint8_t>);
} // namespace eawr::campaign
