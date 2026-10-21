#pragma once

#include "eawr/campaign/mp_agreement.hpp"
#include "eawr/campaign/mp_lobby_catalog.hpp"
#include "eawr/net/galactic_lobby.hpp"

#include "eawr/platform/live_game_session.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

// What one machine's installed content gives a Galactic Conquest room: the galactic data and type table the roster is
// derived against, the content digests and the production planner. `local.data` and `local.types` are filled by the
// backend from the two shared pointers; `keepalive` holds whatever the planner's captured references live in.
struct GalacticLobbyContent {
    std::shared_ptr<const eawr::data::galactic::GalacticData> data;
    std::shared_ptr<const units::GalacticTypeTable> types;
    campaign::MpLocalContent local;
    std::shared_ptr<const void> keepalive;
};
using GalacticLobbyLoader = std::function<core::Result<GalacticLobbyContent>()>;

// The viewer's loader body: the mounted galactic data, the type table, the Lua and XML closure digests and the
// production campaign planner (plan_live_campaign_setup). Production prepares the retained root, stock placement,
// conflict services and native AI before the offer computes its tick-zero claim.
[[nodiscard]] core::Result<GalacticLobbyContent> load_galactic_lobby_content(const vfs::Vfs& filesystem,
    const eawr::data::Catalog& catalog, std::shared_ptr<const void> keepalive);

// What a Galactic Conquest room needs from this machine's installed content, as one object the lobby hooks call. It
// turns the agreed room choices into the #3768 setup offer (seal the roster with the local AI configurations and the
// tick-zero claim, derive the content identities) and, on every peer, derives the same agreement again from the
// received offer. The lobby layer (net/galactic_lobby.hpp) knows nothing of content; this layer knows nothing of the
// room beyond its snapshot.
//
// The seam for the match itself is `adapter_factory`: Start agrees the tick-zero state of the derived session on every
// peer (the setup barrier), and the galactic lockstep adapter that steps it is GCMP-03/#3769. Until a factory is
// installed `can_start()` is false (the lobby keeps Start off with a reason) and a barrier that would need an adapter
// refuses with a typed diagnostic instead of starting.
class GalacticLobbyBackend {
public:
    using AdapterFactory = std::function<core::Result<std::unique_ptr<net::SessionAdapter>>(
        const net::SetupOffer&, const campaign::MpAgreement&)>;

    // `local` must outlive the backend (it points at the installed galactic data and type table).
    explicit GalacticLobbyBackend(campaign::MpLocalContent local);
    // Loads on first use (the first validation, offer or derivation), so a skirmish-only session never pays for it. A
    // load that fails leaves the campaign list empty and `load_error()` set; every GC call then refuses with that error.
    explicit GalacticLobbyBackend(GalacticLobbyLoader loader);

    // The campaigns the lobby lists: Is_Listed + Is_Multiplayer from the local data, in registry order.
    [[nodiscard]] const std::vector<sim::galactic::MpLobbyCampaign>& campaigns() const;
    [[nodiscard]] std::optional<core::Diagnostic> load_error() const;
    void set_adapter_factory(AdapterFactory factory) { adapter_factory_ = std::move(factory); }
    // Whether a campaign match can be built here (the GCMP-03 driver installed its adapter factory).
    [[nodiscard]] bool can_start() const noexcept { return static_cast<bool>(adapter_factory_); }
    // Qualification only, set before opening a room. The ordinary offer retains its default cap.
    void acceptance_final_tick(std::uint64_t tick) { acceptance_final_tick_ = tick; }

    // Lobby hooks (net::LobbyHooks::validate / offer) for a room in Galactic Conquest mode.
    [[nodiscard]] core::Result<void> validate(const net::LobbySnapshot&, bool starting) const;
    [[nodiscard]] core::Result<net::SetupOffer> offer(const net::LobbySnapshot&, const std::vector<net::SetupSeat>&) const;
    // Setup providers (net::SetupProviders::identify / build) on every peer, including the host.
    [[nodiscard]] core::Result<sim::MatchAgreement> identify(const net::SetupOffer&);
    [[nodiscard]] core::Result<std::unique_ptr<net::SessionAdapter>> build(const net::SetupOffer&);
    // The agreement the last identify derived on this peer, or null: the session the adapter takes over.
    [[nodiscard]] std::shared_ptr<const campaign::MpAgreement> agreement() const;
    [[nodiscard]] std::shared_ptr<const platform::CampaignServiceConfiguration> runtime_services() const;

private:
    // Runs the loader once. False when the content is unavailable (the error is kept).
    bool ensure() const;
    mutable GalacticLobbyLoader loader_;
    mutable GalacticLobbyContent content_;
    mutable campaign::MpLocalContent local_;
    mutable std::vector<sim::galactic::MpLobbyCampaign> campaigns_;
    mutable std::optional<core::Diagnostic> load_error_;
    mutable bool loaded_{};
    AdapterFactory adapter_factory_;
    std::uint64_t acceptance_final_tick_{};
    mutable std::mutex mutex_;
    std::shared_ptr<campaign::MpAgreement> agreement_;
};

} // namespace eawr::presentation::godot_backend
