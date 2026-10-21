#include "galactic_lobby_backend.hpp"
#include "eawr/platform/agreed_campaign.hpp"

#include "eawr/campaign/setup_plan.hpp"
#include "eawr/core/build_identity.hpp"
#include "eawr/skirmish/start.hpp"

namespace eawr::presentation::godot_backend {
namespace {
campaign::MpSessionTerms terms_for(std::uint64_t seed, std::uint32_t command_quota, std::uint32_t byte_quota) {
    campaign::MpSessionTerms terms;
    terms.seed = seed; terms.command_quota = command_quota; terms.byte_quota = byte_quota;
    return terms;
}
template<class T> core::Result<T> unavailable(const std::optional<core::Diagnostic>& error) {
    if (error) return core::Result<T>::failure(*error);
    return core::Result<T>::failure(net::lobby_diagnostic(net::LobbyReason::setup_failed, "Campaign content is unavailable."));
}
} // namespace

core::Result<GalacticLobbyContent> load_galactic_lobby_content(const vfs::Vfs& filesystem, const data::Catalog& catalog,
    std::shared_ptr<const void> keepalive) {
    using Result = core::Result<GalacticLobbyContent>;
    auto data = data::galactic::load_galactic_data(filesystem, catalog);
    if (!data) return Result::failure(data.error());
    auto identity = skirmish::read_data_identity(filesystem);
    if (!identity) return Result::failure(identity.error());
    GalacticLobbyContent content;
    content.keepalive = std::move(keepalive);
    content.data = std::make_shared<const data::galactic::GalacticData>(std::move(data).value());
    content.types = std::make_shared<const units::GalacticTypeTable>(units::galactic_type_table(catalog));
    content.local.script_manifest_digest = identity.value().script_manifest_digest;
    content.local.xml_digest = identity.value().ai_data_digest;
    content.local.command_quota = static_cast<std::uint32_t>(net::max_batch_commands);
    content.local.byte_quota = static_cast<std::uint32_t>(net::max_batch_bytes);
    // The filesystem lives as long as `keepalive`, which the content owns.
    content.local.planner = [&filesystem, data = content.data, types = content.types](
        const campaign::CampaignSetupOptions& options, std::string_view campaign_name, std::string_view difficulty) {
        return campaign::plan_live_campaign_setup(filesystem, *data, *types, campaign_name, options, difficulty);
    };
    // A fresh prepared owner for every derivation; the selected agreement retains its exact root.
    auto files = std::shared_ptr<const vfs::Vfs>(content.keepalive, &filesystem);
    auto installed = std::make_shared<const data::Catalog>(catalog);
    content.local.prepare_session = [files, installed, data = content.data, types = content.types](
        const campaign::CampaignSetupPlan& plan, std::string_view difficulty) -> core::Result<campaign::MpPreparedSession> {
        auto built = platform::PreparedLiveCampaign::from_plan(files, *installed, *data, *types,
            campaign::CampaignSetupPlan(plan), difficulty);
        if (!built) return core::Result<campaign::MpPreparedSession>::failure(built.error());
        auto prepared = std::shared_ptr<platform::PreparedLiveCampaign>(std::move(built).value());
        return core::Result<campaign::MpPreparedSession>::success({
            std::shared_ptr<campaign::GameSession>(prepared, &platform::AgreedCampaignAccess::session(*prepared)),
            prepared->services()});
    };
    return Result::success(std::move(content));
}

GalacticLobbyBackend::GalacticLobbyBackend(campaign::MpLocalContent local) : local_(std::move(local)), loaded_(true) {
    // This build's batch limits are part of what the offer agrees (the roster payload carries them).
    if (local_.command_quota == 0) local_.command_quota = static_cast<std::uint32_t>(net::max_batch_commands);
    if (local_.byte_quota == 0) local_.byte_quota = static_cast<std::uint32_t>(net::max_batch_bytes);
    if (local_.data) campaigns_ = campaign::list_mp_lobby_campaigns(*local_.data);
}

GalacticLobbyBackend::GalacticLobbyBackend(GalacticLobbyLoader loader) : loader_(std::move(loader)) {}

bool GalacticLobbyBackend::ensure() const {
    const std::lock_guard lock(mutex_);
    if (loaded_) return !load_error_;
    loaded_ = true;
    if (!loader_) {
        load_error_ = net::lobby_diagnostic(net::LobbyReason::setup_failed, "This build has no campaign content loader.");
        return false;
    }
    auto loaded = loader_();
    loader_ = nullptr;
    if (!loaded) { load_error_ = loaded.error(); return false; }
    content_ = std::move(loaded).value();
    local_ = content_.local;
    local_.data = content_.data.get();
    local_.types = content_.types.get();
    campaigns_ = campaign::list_mp_lobby_campaigns(*local_.data);
    return true;
}

const std::vector<sim::galactic::MpLobbyCampaign>& GalacticLobbyBackend::campaigns() const {
    (void)ensure();
    return campaigns_;
}

std::optional<core::Diagnostic> GalacticLobbyBackend::load_error() const {
    (void)ensure();
    const std::lock_guard lock(mutex_);
    return load_error_;
}

core::Result<void> GalacticLobbyBackend::validate(const net::LobbySnapshot& snapshot, bool starting) const {
    if (!ensure()) return unavailable<void>(load_error());
    return net::validate_galactic_lobby(snapshot, starting, campaigns_);
}

core::Result<net::SetupOffer> GalacticLobbyBackend::offer(const net::LobbySnapshot& snapshot,
    const std::vector<net::SetupSeat>& seats) const {
    using Result = core::Result<net::SetupOffer>;
    if (!ensure()) return unavailable<net::SetupOffer>(load_error());
    auto options = net::decode_galactic_lobby_game(snapshot.game);
    if (!options) return Result::failure(options.error());
    auto draft = net::galactic_roster_draft(snapshot, campaigns_);
    if (!draft) return Result::failure(draft.error());
    const auto terms = terms_for(options.value().seed, local_.command_quota, local_.byte_quota);
    auto sealed = campaign::seal_mp_roster(local_, std::move(draft).value(), terms);
    if (!sealed) return Result::failure(sealed.error());
    auto derived = campaign::derive_mp_agreement(local_, sealed.value(), terms);
    if (!derived) return Result::failure(derived.error());
    const sim::MatchAgreement identity{std::string(core::build_identity()), derived.value().content};
    auto offer = net::make_galactic_lobby_offer(options.value(), sealed.value(), identity, seats);
    if (offer && acceptance_final_tick_) offer.value().final_tick_count = acceptance_final_tick_;
    return offer;
}

core::Result<sim::MatchAgreement> GalacticLobbyBackend::identify(const net::SetupOffer& offer) {
    using Result = core::Result<sim::MatchAgreement>;
    if (!ensure()) return unavailable<sim::MatchAgreement>(load_error());
    auto payload = net::decode_galactic_setup(offer.setup);
    if (!payload) return Result::failure(payload.error());
    // The offer's claims are inputs to a local derivation, never replacements for it: a differing claim shows up as
    // a differing identity domain and the setup service refuses the offer before any shared step.
    const auto terms = terms_for(offer.seed, payload.value().command_quota, payload.value().byte_quota);
    auto derived = campaign::derive_mp_agreement(local_, payload.value().roster, terms);
    if (!derived) return Result::failure(derived.error());
    auto agreement = std::make_shared<campaign::MpAgreement>(std::move(derived).value());
    const sim::MatchAgreement identity{std::string(core::build_identity()), agreement->content};
    { const std::lock_guard lock(mutex_); agreement_ = std::move(agreement); }
    return Result::success(identity);
}

core::Result<std::unique_ptr<net::SessionAdapter>> GalacticLobbyBackend::build(const net::SetupOffer& offer) {
    using Result = core::Result<std::unique_ptr<net::SessionAdapter>>;
    const auto current = agreement();
    if (!current) {
        return Result::failure(net::lobby_diagnostic(net::LobbyReason::setup_failed, "The campaign offer has not been derived from local content."));
    }
    if (!adapter_factory_) {
        return Result::failure(net::lobby_diagnostic(net::LobbyReason::setup_failed,
            "Starting a multiplayer campaign needs the galactic lockstep adapter (GCMP-03), which this build does not have yet."));
    }
    return adapter_factory_(offer, *current);
}

std::shared_ptr<const campaign::MpAgreement> GalacticLobbyBackend::agreement() const {
    const std::lock_guard lock(mutex_);
    return agreement_;
}

std::shared_ptr<const platform::CampaignServiceConfiguration> GalacticLobbyBackend::runtime_services() const {
    const auto current = agreement();
    return current ? std::static_pointer_cast<const platform::CampaignServiceConfiguration>(current->runtime_services) : nullptr;
}

} // namespace eawr::presentation::godot_backend
