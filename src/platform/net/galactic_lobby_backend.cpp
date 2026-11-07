#include "galactic_lobby_backend.hpp"
#include "eawr/vfs/vfs.hpp"

#include "eawr/platform/agreed_campaign.hpp"
#include "eawr/net/galactic_adapter.hpp"
#include "eawr/platform/sim_workers.hpp"

#include "eawr/campaign/setup_plan.hpp"
#include "eawr/core/build_identity.hpp"
#include "eawr/core/gc_load_trace.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/net/galactic_setup.hpp"

#include <thread>
#include "../galactic/live_save_transport.hpp"

#include <algorithm>
#include <set>
#include <sstream>

namespace eawr::presentation::godot_backend {
namespace {
campaign::MpSessionTerms terms_for(std::uint64_t seed, std::uint32_t command_quota, std::uint32_t byte_quota) {
    campaign::MpSessionTerms terms;
    terms.seed = seed; terms.command_quota = command_quota; terms.byte_quota = byte_quota;
    return terms;
}
bool same_preparation_inputs(const net::SetupOffer& a, const net::SetupOffer& b) {
    // Identity claims never replace the locally computed identity returned by identify.
    return a.seed == b.seed && a.domain == b.domain && a.setup == b.setup && a.seats == b.seats
        && a.input_delay == b.input_delay && a.recovery == b.recovery && a.final_tick_count == b.final_tick_count;
}

bool unchanged(const campaign::MpAgreement& agreement, const std::string& hash) {
    return agreement.session && agreement.session->completed_tick() == 0 && agreement.session->state_sha256() == hash;
}

template<class T> core::Result<T> unavailable(const std::optional<core::Diagnostic>& error) {
    if (error) return core::Result<T>::failure(*error);
    return core::Result<T>::failure(net::lobby_diagnostic(net::LobbyReason::setup_failed, "Campaign content is unavailable."));
}
} // namespace

core::Result<GalacticLobbyContent> load_galactic_lobby_content(const vfs::Vfs& filesystem, const data::Catalog& catalog,
    std::shared_ptr<const void> keepalive, std::array<std::uint8_t, 32> checkpoint_mod_list) {
    using Result = core::Result<GalacticLobbyContent>;
    auto data = data::galactic::load_galactic_data(filesystem, catalog);
    if (!data) return Result::failure(data.error());
    auto identity = skirmish::read_data_identity(filesystem);
    if (!identity) return Result::failure(identity.error());
    GalacticLobbyContent content;
    content.keepalive = std::move(keepalive);
    {
        std::string layers;
        for (const auto& id : filesystem.layer_ids()) layers += id + '\n';
        content.mod_layer_count = static_cast<std::uint32_t>(filesystem.layer_ids().size());
        content.mod_layers_hash = core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(layers.data()), layers.size()));
    }
    content.data = std::make_shared<const data::galactic::GalacticData>(std::move(data).value());
    content.types = std::make_shared<const units::GalacticTypeTable>(units::galactic_type_table(catalog));
    // The AI player types' own difficulty entries decide which difficulties the lobby offers for a campaign.
    if (const auto table = skirmish::load_difficulty_table(filesystem))
        content.local.ai_levels = [table](std::string_view type) { return table->level_entries(type); };
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
            prepared->services(), platform::AgreedCampaignAccess::load_notes(*prepared)});
    };
    content.local.restore_session = [files, installed, data = content.data, types = content.types, checkpoint_mod_list](
        const campaign::CampaignSetupPlan& plan, std::string_view difficulty, std::span<const std::uint8_t> bytes)
        -> core::Result<campaign::MpPreparedSession> {
        auto built = platform::PreparedLiveCampaign::from_plan(files, *installed, *data, *types,
            campaign::CampaignSetupPlan(plan), difficulty, true, {300, 1, 3, 3, 300, 1}, false, bytes, checkpoint_mod_list);
        if (!built) return core::Result<campaign::MpPreparedSession>::failure(built.error());
        auto prepared = std::shared_ptr<platform::PreparedLiveCampaign>(std::move(built).value());
        return core::Result<campaign::MpPreparedSession>::success({
            std::shared_ptr<campaign::GameSession>(prepared, &platform::AgreedCampaignAccess::session(*prepared)),
            prepared->services(), platform::AgreedCampaignAccess::load_notes(*prepared)});
    };
    return Result::success(std::move(content));
}

GalacticLobbyBackend::GalacticLobbyBackend(campaign::MpLocalContent local) : local_(std::move(local)) {
    ready_.store(true, std::memory_order_release);
    // This build's batch limits are part of what the offer agrees (the roster payload carries them).
    if (local_.command_quota == 0) local_.command_quota = static_cast<std::uint32_t>(net::max_batch_commands);
    if (local_.byte_quota == 0) local_.byte_quota = static_cast<std::uint32_t>(net::max_batch_bytes);
    if (!local_.restore_session && !local_.prepare_session && !local_.services) {
        local_.restore_session = [](const campaign::CampaignSetupPlan& plan, std::string_view, std::span<const std::uint8_t> bytes)
            -> core::Result<campaign::MpPreparedSession> {
            checkpoint::GameLoadOptions options; options.script_requirement = campaign::ScriptRequirement::none;
            const sim::InlineExecutor executor;
            auto restored = checkpoint::load_game_checkpoint(bytes, checkpoint::game_identities(plan.galaxy()), options, executor);
            if (!restored) return core::Result<campaign::MpPreparedSession>::failure(restored.error());
            return core::Result<campaign::MpPreparedSession>::success({
                std::make_shared<campaign::GameSession>(std::move(restored).value().session), {}, {}});
        };
    }
    if (local_.data) campaigns_ = campaign::list_all_lobby_campaigns(*local_.data, local_.ai_levels);
}

GalacticLobbyBackend::GalacticLobbyBackend(GalacticLobbyLoader loader) : loader_(std::move(loader)) {}

bool GalacticLobbyBackend::ensure() const {
    if (!ready_.load(std::memory_order_acquire)) {
        // One caller loads; the others wait here, not on `mutex_`.
        const std::lock_guard load_lock(load_mutex_);
        if (!ready_.load(std::memory_order_relaxed) && !loading_started_) {
            loading_started_ = true;
            std::optional<core::Diagnostic> error;
            GalacticLobbyContent loaded_content;
            if (!loader_) {
                error = net::lobby_diagnostic(net::LobbyReason::setup_failed, "This build has no campaign content loader.");
            } else {
                core::gc_load::Scope scope("lobby.content_load");
                try {
                    auto loaded = loader_();
                    if (!loaded) error = loaded.error();
                    else loaded_content = std::move(loaded).value();
                } catch (const std::exception& failure) {
                    error = net::lobby_diagnostic(net::LobbyReason::setup_failed,
                        std::string("The campaign content load failed: ") + failure.what());
                } catch (...) {
                    error = net::lobby_diagnostic(net::LobbyReason::setup_failed, "The campaign content load failed.");
                }
                loader_ = nullptr;
            }
            {
                const std::lock_guard lock(mutex_);
                load_error_ = std::move(error);
                if (!load_error_) {
                    content_ = std::move(loaded_content);
                    local_ = content_.local;
                    local_.data = content_.data.get();
                    local_.types = content_.types.get();
                    campaigns_ = campaign::list_all_lobby_campaigns(*local_.data, local_.ai_levels);
                }
            }
            ready_.store(true, std::memory_order_release);
        }
    }
    const std::lock_guard lock(mutex_);
    return !load_error_;
}

void GalacticLobbyBackend::prefetch_content(const std::shared_ptr<GalacticLobbyBackend>& backend) {
    if (!backend || backend->content_ready() || backend->prefetching_.exchange(true)) return;
    try {
        std::thread([backend] {
            try { static_cast<void>(backend->ensure()); } catch (...) {}   // ensure() reports a failed load itself
        }).detach();
    } catch (const std::system_error&) {
        backend->prefetching_ = false;   // the next caller loads on its own thread, as before
    }
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
    // A checkpoint selection first publishes its saved rows to the staging
    // room. Peers must accept those rows before the checkpoint can transfer;
    // Start checks the same eligible catalog or a fully restored selection.
    if (!starting) {
        const auto staging_campaigns = campaign::list_all_lobby_campaigns(*local_.data, local_.ai_levels);
        return net::validate_galactic_lobby(snapshot, false, staging_campaigns);
    }
    if (starting && transfer_.manifest() && !resume_)
        return core::Result<void>::failure(net::lobby_diagnostic(net::LobbyReason::not_ready, "checkpoint_restore_pending"));
    if (resume_) {
        // Authoritative staging edits must remain receivable after a save was
        // restored. Missing saved humans refuse Ready/Start, never the room update.
        auto bound = bind_setup_seats(snapshot, {});
        if (!bound) return core::Result<void>::failure(bound.error());
        if (starting && !transfer_.view().restored)
            return core::Result<void>::failure(net::lobby_diagnostic(net::LobbyReason::not_ready, "checkpoint_restore_pending"));
        return core::Result<void>::success();
    }
    return net::validate_galactic_lobby(snapshot, starting, campaigns_);
}

core::Result<net::SetupOffer> GalacticLobbyBackend::offer(const net::LobbySnapshot& snapshot,
    const std::vector<net::SetupSeat>& seats) const {
    using Result = core::Result<net::SetupOffer>;
    const core::gc_load::Scope scope("lobby.host_offer");
    if (!ensure()) return unavailable<net::SetupOffer>(load_error());
    if (resume_) {
        auto bound = bind_setup_seats(snapshot, seats);
        if (!bound) return Result::failure(bound.error());
        const auto progress = transfer_.view();
        if (!progress.restored || (snapshot.host == resume_->manifest.peers.front() && resume_->bytes && !progress.agreed))
            return Result::failure(net::lobby_diagnostic(net::LobbyReason::not_ready, "checkpoint_restore_ack_pending"));
        if (resume_->agreement->session->state_sha256() != resume_->hash)
            return Result::failure(net::lobby_diagnostic(net::LobbyReason::setup_failed, "checkpoint_preparation_changed"));
        return Result::success(resume_->manifest.offer);
    }
    const std::lock_guard preparation_lock(preparation_mutex_);
    if (preparation_ && preparation_->snapshot == snapshot && preparation_->seats == seats
        && preparation_->offer.final_tick_count == final_tick_count()
        && unchanged(*preparation_->agreement, preparation_->state_hash))
        return Result::success(preparation_->offer);
    preparation_.reset();
    auto options = net::decode_galactic_lobby_game(snapshot.game);
    if (!options) return Result::failure(options.error());
    auto draft = net::galactic_roster_draft(snapshot, campaigns_);
    if (!draft) return Result::failure(draft.error());
    const auto terms = terms_for(options.value().seed, local_.command_quota, local_.byte_quota);
    auto derived = campaign::seal_mp_agreement(local_, draft.value(), terms);
    if (!derived) return Result::failure(derived.error());
    auto agreement = std::make_shared<campaign::MpAgreement>(std::move(derived).value());
    const sim::MatchAgreement identity{std::string(core::build_identity()), agreement->content};
    auto offer = net::make_galactic_lobby_offer(options.value(), draft.value(), identity, seats);
    if (!offer) return offer;
    offer.value().final_tick_count = final_tick_count();
    const auto hash = agreement->session->state_sha256();
    preparation_ = Preparation{snapshot, seats, offer.value(), std::move(agreement), hash};
    return offer;
}

core::Result<sim::MatchAgreement> GalacticLobbyBackend::identify(const net::SetupOffer& offer) {
    using Result = core::Result<sim::MatchAgreement>;
    const core::gc_load::Scope scope("lobby.identify");
    if (!ensure()) return unavailable<sim::MatchAgreement>(load_error());
    auto payload = net::decode_galactic_setup(offer.setup);
    if (!payload) return Result::failure(payload.error());
    if (payload.value().start == net::GalacticStart::checkpoint) {
        if (!resume_ || !transfer_.view().restored || !same_preparation_inputs(resume_->manifest.offer, offer)
            || resume_->agreement->session->completed_tick() != payload.value().initial_tick
            || resume_->agreement->session->state_sha256() != resume_->hash)
            return Result::failure(net::lobby_diagnostic(net::LobbyReason::setup_failed, "checkpoint_selection_or_hash_mismatch"));
        const sim::MatchAgreement identity{std::string(core::build_identity()), resume_->agreement->content};
        { const std::lock_guard lock(mutex_); agreement_ = resume_->agreement; }
        return Result::success(identity);
    }
    if (resume_) return Result::failure(net::lobby_diagnostic(net::LobbyReason::setup_failed, "checkpoint_selection_mismatch"));
    // The offer's claims are inputs to a local derivation, never replacements for it: a differing claim shows up as
    // a differing identity domain and the setup service refuses the offer before any shared step.
    const auto terms = terms_for(offer.seed, payload.value().command_quota, payload.value().byte_quota);
    std::shared_ptr<campaign::MpAgreement> agreement;
    {
        const std::lock_guard preparation_lock(preparation_mutex_);
        if (preparation_ && same_preparation_inputs(preparation_->offer, offer)) {
            if (unchanged(*preparation_->agreement, preparation_->state_hash)) agreement = preparation_->agreement;
            else preparation_.reset();
        }
    }
    if (!agreement) {
        auto derived = campaign::derive_mp_agreement(local_, payload.value().roster, terms);
        if (!derived) return Result::failure(derived.error());
        agreement = std::make_shared<campaign::MpAgreement>(std::move(derived).value());
    }
    const sim::MatchAgreement identity{std::string(core::build_identity()), agreement->content};
    { const std::lock_guard lock(mutex_); agreement_ = std::move(agreement); }
    return Result::success(identity);
}

core::Result<std::unique_ptr<net::SessionAdapter>> GalacticLobbyBackend::build(const net::SetupOffer& offer) {
    using Result = core::Result<std::unique_ptr<net::SessionAdapter>>;
    const core::gc_load::Scope scope("lobby.build_adapter");
    const auto current = agreement();
    if (!current) {
        return Result::failure(net::lobby_diagnostic(net::LobbyReason::setup_failed, "The campaign offer has not been derived from local content."));
    }
    if (!adapter_factory_) {
        return Result::failure(net::lobby_diagnostic(net::LobbyReason::setup_failed,
            "Starting a multiplayer campaign needs the galactic lockstep adapter (GCMP-03), which this build does not have yet."));
    }
    // The adapter may step or mutate the owner even if construction later fails.
    { const std::lock_guard preparation_lock(preparation_mutex_); preparation_.reset(); }
    return adapter_factory_(offer, *current);
}

std::string GalacticLobbyBackend::start_summary(const net::SetupOffer& offer, const bool host) const {
    const auto hex = [](std::span<const std::uint8_t> bytes) {
        return core::sha256_hex(bytes).substr(0, 16);
    };
    std::ostringstream out;
    out << "role=" << (host ? "host" : "guest") << " mods=" << content_.mod_layer_count << ':'
        << (content_.mod_layers_hash.empty() ? std::string("none") : content_.mod_layers_hash.substr(0, 16))
        << " scripts=" << local_.script_manifest_digest.substr(0, 16) << " xml=" << local_.xml_digest.substr(0, 16)
        << " payload=" << hex(offer.setup);
    if (const auto payload = net::decode_galactic_setup(offer.setup)) {
        std::vector<std::uint8_t> claims;
        for (const auto& player : payload.value().roster.players)
            claims.insert(claims.end(), player.ai_configuration.begin(), player.ai_configuration.end());
        out << " ai=" << hex(claims) << " tick_zero=" << hex(payload.value().roster.tick_zero);
    }
    out << " identity=" << hex(offer.identity.content.sha256) << " domains=";
    for (std::size_t i = 0; i < offer.identity.content.domains.size(); ++i) {
        const auto& domain = offer.identity.content.domains[i];
        out << (i ? "," : "") << domain.name << ':' << hex(domain.digest).substr(0, 8);
    }
    return out.str();
}

std::shared_ptr<const campaign::MpAgreement> GalacticLobbyBackend::agreement() const {
    const std::lock_guard lock(mutex_);
    return agreement_;
}

std::optional<platform::LiveSaveCampaignSource> GalacticLobbyBackend::save_campaign_source(const net::GalacticLobbyOptions& options) const {
    if (!ensure() || !local_.data) return std::nullopt;
    const auto* authored = local_.data->campaign(options.campaign);
    if (authored == nullptr) return std::nullopt;
    platform::LiveSaveCampaignSource source;
    source.campaign = authored->name; source.campaign_set = authored->campaign_set;
    source.difficulty = options.difficulty;
    source.multiplayer = true; // a room's game resumes through a lobby, whichever kind of entry it started from
    if (options.custom) source.custom = campaign::CampaignCustomOptions{options.credits, options.starting_tech, options.maximum_tech, options.allow_heroes};
    for (const auto& row : options.rows) source.factions.push_back({row.player, row.faction});
    return source;
}

std::shared_ptr<const platform::CampaignServiceConfiguration> GalacticLobbyBackend::runtime_services() const {
    const auto current = agreement();
    return current ? std::static_pointer_cast<const platform::CampaignServiceConfiguration>(current->runtime_services) : nullptr;
}

} // namespace eawr::presentation::godot_backend

namespace eawr::presentation::godot_backend {
namespace {
using ResumeDigest = std::array<std::uint8_t, 32>;
template<class T> core::Result<T> resume_refusal(std::string reason) {
    return core::Result<T>::failure(net::lobby_diagnostic(net::LobbyReason::setup_failed, "checkpoint: " + std::move(reason)));
}
bool same_resume_room(const net::LobbySnapshot& left, const net::LobbySnapshot& right) {
    if (left.room != right.room || left.game != right.game || left.host != right.host || left.seats.size() != right.seats.size()) return false;
    for (std::size_t i = 0; i < left.seats.size(); ++i)
        if (left.seats[i].peer != right.seats[i].peer || left.seats[i].seat != right.seats[i].seat
            || left.seats[i].player != right.seats[i].player || left.seats[i].selection.slot != right.seats[i].selection.slot)
            return false;
    return true;
}
ResumeDigest resume_terms(const net::SaveManifest& manifest, const net::LobbySnapshot& snapshot) {
    if (snapshot.seats.size() != manifest.offer.seats.size() || manifest.peers.size() != manifest.offer.seats.size()) return {};
    std::vector<std::uint8_t> bytes;
    const auto word = [&](std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    const auto text = [&](std::string_view value) { word(value.size()); bytes.insert(bytes.end(), value.begin(), value.end()); };
    text("eawr-gc-resume-terms-v1"); text(snapshot.room); text(snapshot.game.map);
    word(snapshot.game.options.size()); bytes.insert(bytes.end(), snapshot.game.options.begin(), snapshot.game.options.end());
    word(manifest.epoch); word(manifest.byte_length); word(manifest.offer.seed);
    word(manifest.offer.input_delay); word(manifest.offer.final_tick_count);
    for (std::size_t i = 0; i < manifest.offer.seats.size(); ++i) {
        const auto& seat = manifest.offer.seats[i];
        word(manifest.peers[i]); word(snapshot.seats[i].seat.value); word(seat.peer); word(seat.seat.value); word(seat.player);
    }
    return core::sha256(bytes);
}
core::Result<void> saved_bindings(const checkpoint::GameResumeMetadata& metadata, const net::SaveManifest& manifest,
    const net::LobbySnapshot& snapshot) {
    const auto humans = static_cast<std::size_t>(std::count_if(metadata.seats.begin(), metadata.seats.end(),
        [](const auto& row) { return row.kind == sim::SeatKind::human && row.active; }));
    if (manifest.offer.seats.size() < humans || snapshot.seats.size() < humans) return resume_refusal<void>("missing_human_seat");
    if (manifest.offer.seats.size() > humans || snapshot.seats.size() > humans) return resume_refusal<void>("extra_human_seat");
    if (manifest.peers.size() != humans) return resume_refusal<void>("peer_mapping_mismatch");
    std::set<sim::SeatId> mapped;
    for (std::size_t i = 0; i < manifest.offer.seats.size(); ++i) {
        const auto& binding = manifest.offer.seats[i];
        if (binding.peer != i || manifest.peers[i] != snapshot.seats[i].peer)
            return resume_refusal<void>("authenticated_peer_mapping_mismatch");
        if (!mapped.insert(binding.seat).second) return resume_refusal<void>("duplicate_saved_seat");
        const auto found = std::find_if(metadata.seats.begin(), metadata.seats.end(), [&](const auto& row) { return row.seat_id == binding.seat; });
        if (found == metadata.seats.end() || found->kind != sim::SeatKind::human) return resume_refusal<void>("unknown_saved_human_seat");
        if (!found->active) return resume_refusal<void>("inactive_saved_seat");
        if (found->player != binding.player || snapshot.seats[i].player != binding.player) return resume_refusal<void>("saved_faction_mismatch");
    }
    return core::Result<void>::success();
}
void bind_resume_identity(campaign::MpAgreement& agreement, const net::GalacticSetupPayload& payload) {
    for (const auto& [name, digest] : std::vector<std::pair<std::string, ResumeDigest>>{
        {"gc-checkpoint-file", payload.checkpoint_digest}, {"gc-checkpoint-frontier", payload.frontier_digest},
        {"gc-checkpoint-terms", payload.terms_digest}})
        agreement.content.domains.push_back({name, 1, digest});
    agreement.content = sim::compute_match_identity(std::move(agreement.content));
}
}
core::Result<GalacticLobbyBackend::CheckpointCandidate> GalacticLobbyBackend::inspect_checkpoint(std::span<const std::uint8_t> bytes) const {
    using Result = core::Result<CheckpointCandidate>;
    if (!ensure()) return unavailable<CheckpointCandidate>(load_error());
    auto parsed = checkpoint::parse_checkpoint(bytes);
    if (!parsed) return Result::failure(parsed.error());
    auto source = platform::save_detail::decode_campaign(parsed.value());
    if (!source) return Result::failure(source.error());
    if (!source.value() || !source.value()->multiplayer) return resume_refusal<CheckpointCandidate>("multiplayer_campaign_metadata_required");
    auto setup = checkpoint::inspect_game_setup(parsed.value());
    if (!setup) return Result::failure(setup.error());
    auto metadata = checkpoint::inspect_game_resume(parsed.value());
    if (!metadata) return Result::failure(metadata.error());
    // Saved co-op games may use an SP entry; the ordinary new-game list remains MP-only.
    const auto saved_campaigns = campaign::list_all_lobby_campaigns(*local_.data, local_.ai_levels);
    const auto* catalog = campaign::find_mp_lobby_campaign(saved_campaigns, source.value()->campaign);
    if (!catalog || !setup.value().seat_control) return resume_refusal<CheckpointCandidate>("saved_campaign_or_control_unavailable");
    CheckpointCandidate candidate; candidate.saved = std::move(metadata).value(); candidate.seed = setup.value().seed;
    auto& roster = candidate.original_roster;
    roster.campaign = source.value()->campaign; roster.difficulty = source.value()->difficulty;
    if (source.value()->custom) {
        const auto& custom = *source.value()->custom; roster.custom = true;
        roster.credits = custom.credits; roster.starting_tech = custom.starting_tech;
        roster.maximum_tech = custom.maximum_tech; roster.allow_heroes = custom.allow_heroes;
    }
    roster.seats = setup.value().seats; roster.teams = setup.value().seat_control->teams;
    roster.tick_zero = candidate.saved.state_digest;
    for (const auto& player : setup.value().players) {
        const auto faction = std::find_if(catalog->factions.begin(), catalog->factions.end(), [&](const auto& row) {
            return row.player == player.player_id && row.faction_id == player.faction_id && row.flags == player.flags;
        });
        const auto controller = std::find_if(setup.value().seat_control->controllers.begin(), setup.value().seat_control->controllers.end(),
            [&](const auto& row) { return row.player == player.player_id; });
        if (faction == catalog->factions.end() || controller == setup.value().seat_control->controllers.end())
            return resume_refusal<CheckpointCandidate>("saved_faction_or_controller_identity_mismatch");
        sim::galactic::MpRosterPlayer row;
        row.player = player.player_id; row.faction_id = player.faction_id; row.flags = player.flags; row.faction = faction->name;
        row.dropout = controller->dropout; row.ai_configuration = controller->configuration;
        roster.players.push_back(std::move(row));
    }
    if (auto valid = sim::galactic::validate_mp_roster(roster); !valid) return Result::failure(valid.error());
    return Result::success(std::move(candidate));
}
net::GalacticRecoveryWorker::Restore GalacticLobbyBackend::recovery_factory(const net::SetupOffer& offer,
    std::size_t workers, net::GalacticClockPolicy clock_policy) const {
    if (!ensure() || !local_.restore_session || !workers || workers > platform::ThreadWorkerAdapter::max_worker_count) return {};
    auto payload = net::decode_galactic_setup(offer.setup);
    const auto prepared = agreement();
    if (!payload || !prepared || !prepared->plan) return {};
    // Capture immutable plan/content closures, never the live agreement/session.
    return [restore = local_.restore_session, plan = prepared->plan, difficulty = payload.value().roster.difficulty,
        workers, clock_policy](const net::GalacticRecoveryPackage& package) -> core::Result<std::unique_ptr<net::SessionAdapter>> {
        if (!package.bytes) return resume_refusal<std::unique_ptr<net::SessionAdapter>>("checkpoint_unavailable");
        auto restored = restore(*plan, difficulty, *package.bytes);
        if (!restored) return core::Result<std::unique_ptr<net::SessionAdapter>>::failure(restored.error());
        auto owner = std::move(restored).value();
        auto executor = std::make_shared<platform::ThreadWorkerAdapter>(workers);
        net::GalacticAdapterOptions options; options.clock_policy = clock_policy;
        auto adapter = net::GalacticSessionAdapter::owned(std::move(owner.session), std::move(executor), options,
            std::move(owner.runtime_services));
        if (!adapter) return core::Result<std::unique_ptr<net::SessionAdapter>>::failure(adapter.error());
        return core::Result<std::unique_ptr<net::SessionAdapter>>::success(std::move(adapter).value());
    };
}
std::uint64_t GalacticLobbyBackend::initial_tick() const noexcept {
    return resume_ && resume_->agreement->resume ? resume_->agreement->resume->completed_tick : 0;
}
std::map<sim::SeatId, std::uint64_t> GalacticLobbyBackend::producer_sequences() const {
    return resume_ && resume_->agreement->resume ? resume_->agreement->resume->producer_sequences : std::map<sim::SeatId, std::uint64_t>{};
}
core::Result<GalacticLobbyBackend::PreparedCheckpoint> GalacticLobbyBackend::prepare_checkpoint(const net::LobbySnapshot& snapshot,
    sim::galactic::MpRoster roster, std::vector<net::SetupSeat> bindings,
    std::shared_ptr<const std::vector<std::uint8_t>> bytes, std::uint64_t epoch) const {
    if (!ensure()) return unavailable<PreparedCheckpoint>(load_error());
    if (snapshot.phase != net::LobbyPhase::staging || snapshot.host != 0 || !epoch
        || !bytes || bytes->empty() || bytes->size() > net::save_transfer_limit)
        return resume_refusal<PreparedCheckpoint>("invalid_selection_or_stale_epoch");
    auto options = net::decode_galactic_lobby_game(snapshot.game);
    if (!options) return core::Result<PreparedCheckpoint>::failure(options.error());
    const auto terms = terms_for(options.value().seed, local_.command_quota, local_.byte_quota);
    auto parsed = checkpoint::parse_checkpoint(*bytes);
    if (!parsed) return core::Result<PreparedCheckpoint>::failure(parsed.error());
    auto metadata_result = checkpoint::inspect_game_resume(parsed.value());
    if (!metadata_result) return core::Result<PreparedCheckpoint>::failure(metadata_result.error());
    auto restored = campaign::restore_mp_agreement(local_, roster, terms, *bytes, metadata_result.value());
    if (!restored) return core::Result<PreparedCheckpoint>::failure(restored.error());
    auto agreement = std::make_shared<campaign::MpAgreement>(std::move(restored).value());
    const auto& metadata = *agreement->resume;
    net::SaveManifest manifest; manifest.epoch = epoch; manifest.byte_length = bytes->size();
    for (const auto& seat : snapshot.seats) manifest.peers.push_back(seat.peer);
    manifest.offer.domain = net::galactic_setup_domain; manifest.offer.seed = options.value().seed;
    manifest.offer.input_delay = options.value().input_delay; manifest.offer.final_tick_count = final_tick_count();
    manifest.offer.seats = std::move(bindings);
    if (auto bound = saved_bindings(metadata, manifest, snapshot); !bound)
        return core::Result<PreparedCheckpoint>::failure(bound.error());
    net::GalacticSetupPayload payload; payload.roster = std::move(roster); payload.roster.tick_zero = metadata.state_digest;
    payload.start = net::GalacticStart::checkpoint; payload.initial_tick = metadata.completed_tick;
    payload.checkpoint_digest = core::sha256(*bytes); payload.state_digest = metadata.state_digest;
    payload.frontier_digest = metadata.frontier_digest; payload.terms_digest = resume_terms(manifest, snapshot);
    auto encoded = net::encode_galactic_setup(payload);
    if (!encoded) return core::Result<PreparedCheckpoint>::failure(encoded.error());
    manifest.offer.setup = std::move(encoded).value();
    bind_resume_identity(*agreement, payload);
    manifest.offer.identity = {std::string(core::build_identity()), agreement->content};
    if (auto valid = net::validate_galactic_offer(manifest.offer); !valid)
        return core::Result<PreparedCheckpoint>::failure(valid.error());
    const auto hash = agreement->session->state_sha256();
    return core::Result<PreparedCheckpoint>::success(PreparedCheckpoint{
        std::move(manifest), snapshot, std::move(agreement), std::move(bytes), hash});
}
core::Result<void> GalacticLobbyBackend::commit_checkpoint(PreparedCheckpoint prepared, const net::LobbySnapshot& current) {
    if (current != prepared.snapshot || prepared.manifest.epoch <= selection_epoch_)
        return resume_refusal<void>("invalid_selection_or_stale_epoch");
    selection_epoch_ = prepared.manifest.epoch;
    resume_ = std::move(prepared);
    { const std::lock_guard lock(preparation_mutex_); preparation_.reset(); }
    return core::Result<void>::success();
}
core::Result<void> GalacticLobbyBackend::select_checkpoint(const net::LobbySnapshot& snapshot,
    sim::galactic::MpRoster roster, std::vector<net::SetupSeat> bindings,
    std::shared_ptr<const std::vector<std::uint8_t>> bytes, std::uint64_t epoch) {
    if (epoch <= selection_epoch_) return resume_refusal<void>("invalid_selection_or_stale_epoch");
    auto prepared = prepare_checkpoint(snapshot, std::move(roster), std::move(bindings), std::move(bytes), epoch);
    if (!prepared) return core::Result<void>::failure(prepared.error());
    return commit_checkpoint(std::move(prepared).value(), snapshot);
}
std::optional<GalacticLobbyBackend::CheckpointView> GalacticLobbyBackend::checkpoint_view() const {
    const auto* manifest = resume_ ? &resume_->manifest : transfer_.manifest();
    if (!manifest) return std::nullopt;
    auto payload = net::decode_galactic_setup(manifest->offer.setup);
    if (!payload) return std::nullopt;
    const auto tick = payload.value().initial_tick;
    auto roster = std::move(payload).value().roster;
    if (resume_ && resume_->agreement->resume) roster.seats = resume_->agreement->resume->seats;
    return CheckpointView{std::move(roster), tick,
        manifest->offer.seats, manifest->peers};
}
void GalacticLobbyBackend::clear_checkpoint() {
    resume_.reset(); cancel_transfer_ = true; ++selection_epoch_;
    { const std::lock_guard lock(mutex_); agreement_.reset(); }
    { const std::lock_guard lock(preparation_mutex_); preparation_.reset(); }
}
core::Result<std::vector<net::SetupSeat>> GalacticLobbyBackend::bind_setup_seats(const net::LobbySnapshot& snapshot,
    const std::vector<net::SetupSeat>& ordinary) const {
    using Result = core::Result<std::vector<net::SetupSeat>>;
    if (!resume_) return Result::success(ordinary);
    if (auto valid = saved_bindings(*resume_->agreement->resume, resume_->manifest, snapshot); !valid) return Result::failure(valid.error());
    if (!same_resume_room(resume_->snapshot, snapshot)) return resume_refusal<std::vector<net::SetupSeat>>("selection_or_membership_changed");
    return Result::success(resume_->manifest.offer.seats);
}
core::Result<void> GalacticLobbyBackend::restore_transfer(const net::SaveManifest& manifest,
    const net::LobbySnapshot& snapshot, std::span<const std::uint8_t> bytes) {
    if (!ensure()) return unavailable<void>(load_error());
    auto payload = net::decode_galactic_setup(manifest.offer.setup);
    if (!payload) return core::Result<void>::failure(payload.error());
    if (manifest.offer.identity.build != core::build_identity()) return resume_refusal<void>("build_identity_mismatch");
    if (resume_terms(manifest, snapshot) != payload.value().terms_digest || core::sha256(bytes) != payload.value().checkpoint_digest)
        return resume_refusal<void>("selection_mapping_or_file_digest_mismatch");
    if (resume_ && resume_->manifest.epoch == manifest.epoch && same_preparation_inputs(resume_->manifest.offer, manifest.offer)
        && resume_->hash == resume_->agreement->session->state_sha256()) return core::Result<void>::success();
    auto parsed = checkpoint::parse_checkpoint(bytes);
    if (!parsed) return core::Result<void>::failure(parsed.error());
    auto metadata_result = checkpoint::inspect_game_resume(parsed.value());
    if (!metadata_result) return core::Result<void>::failure(metadata_result.error());
    auto agreement = campaign::restore_mp_agreement(local_, payload.value().roster,
        terms_for(manifest.offer.seed, payload.value().command_quota, payload.value().byte_quota), bytes, metadata_result.value());
    if (!agreement) return core::Result<void>::failure(agreement.error());
    const auto& metadata = *agreement.value().resume;
    if (metadata.completed_tick != payload.value().initial_tick || metadata.state_digest != payload.value().state_digest
        || metadata.frontier_digest != payload.value().frontier_digest) return resume_refusal<void>("restored_hash_or_frontier_mismatch");
    if (auto bound = saved_bindings(metadata, manifest, snapshot); !bound) return bound;
    bind_resume_identity(agreement.value(), payload.value());
    const sim::MatchAgreement identity{std::string(core::build_identity()), agreement.value().content};
    if (auto mismatch = sim::describe_mismatch(manifest.offer.identity, identity)) return core::Result<void>::failure(*mismatch);
    auto owner = std::make_shared<campaign::MpAgreement>(std::move(agreement).value());
    const auto hash = owner->session->state_sha256();
    resume_ = ResumeSelection{manifest, snapshot, std::move(owner), {}, hash}; selection_epoch_ = manifest.epoch;
    return core::Result<void>::success();
}
void GalacticLobbyBackend::pump_transfer(net::MessageTransport& transport, const net::LobbySnapshot& snapshot, std::uint64_t now) {
    if (cancel_transfer_) { transfer_.cancel(transport, "selection_cleared"); transfer_.reset(); cancel_transfer_ = false; }
    if (resume_ && resume_->bytes && transfer_.view().epoch != resume_->manifest.epoch) {
        auto selected = transfer_.select(transport, resume_->manifest, resume_->bytes,
            [this, &snapshot](const auto& manifest, auto bytes) { return restore_transfer(manifest, snapshot, bytes); }, now);
        if (!selected) { transfer_.cancel(transport, selected.error().message); return; }
    }
    if (resume_ && !same_resume_room(resume_->snapshot, snapshot)) {
        auto binding = bind_setup_seats(snapshot, {});
        if (!binding) transfer_.cancel(transport, binding.error().message);
    }
    static_cast<void>(transfer_.pump(transport, now));
}
void GalacticLobbyBackend::receive_transfer(net::MessageTransport& transport, const net::LobbySnapshot& snapshot,
    const net::Received& received, std::uint64_t now) {
    auto accepted = transfer_.receive(transport, received,
        [this, &snapshot](const auto& manifest, auto bytes) { return restore_transfer(manifest, snapshot, bytes); }, now);
    // Stale epochs must not cancel the newer selection. Every current-epoch
    // refusal is broadcast by the transfer owner and retained for the UI.
    if (!accepted && transfer_.view().refusal) {
        const auto error = *transfer_.view().refusal;
        if (received.from == snapshot.host && received.frame.front() == net::save_cancel_kind
            && error.message == "checkpoint transfer: selection_cleared") {
            resume_.reset(); transfer_.reset(); ++selection_epoch_;
        }
        const std::lock_guard lock(mutex_); agreement_.reset();
    }
}
} // namespace eawr::presentation::godot_backend
