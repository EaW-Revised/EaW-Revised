#pragma once

#include "eawr/campaign/mp_agreement.hpp"
#include "eawr/campaign/mp_lobby_catalog.hpp"
#include "eawr/net/galactic_lobby.hpp"
#include "eawr/net/galactic_checkpoint_worker.hpp"
#include "eawr/net/galactic_recovery.hpp"
#include "eawr/net/galactic_clock_policy.hpp"

#include "eawr/platform/live_game_session.hpp"

#include <atomic>
#include <chrono>
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
    // The mounted layers (base game, expansion, mods) as a count and a SHA-256 of their ids, for the Start log line only:
    // the layers are not part of the agreement, which compares derived content.
    std::uint32_t mod_layer_count{};
    std::string mod_layers_hash;
};
using GalacticLobbyLoader = std::function<core::Result<GalacticLobbyContent>()>;

// The viewer's loader body: the mounted galactic data, the type table, the Lua and XML closure digests and the
// production campaign planner (plan_live_campaign_setup). Production prepares the retained root, stock placement,
// conflict services and native AI before the offer computes its tick-zero claim.
[[nodiscard]] core::Result<GalacticLobbyContent> load_galactic_lobby_content(const vfs::Vfs& filesystem,
    const eawr::data::Catalog& catalog, std::shared_ptr<const void> keepalive,
    std::array<std::uint8_t, 32> checkpoint_mod_list = {});

// What a Galactic Conquest room needs from this machine's installed content, as one object the lobby hooks call. It
// turns the agreed room choices into the #3768 setup offer (seal the roster with the local AI configurations and the
// tick-zero claim, derive the content identities) and, on every peer, reuses that local preparation for the matching
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

    // True once the content load has finished, successfully or not. The UI thread asks this before it touches the
    // campaign list or the load error: until then those calls would wait for the whole load (#5512).
    [[nodiscard]] bool content_ready() const noexcept { return ready_.load(std::memory_order_acquire); }
    // Starts the content load on its own thread (once) so the UI thread never runs it. A backend built from ready
    // content is a no-op. The thread keeps the backend alive until the load returns.
    static void prefetch_content(const std::shared_ptr<GalacticLobbyBackend>& backend);

    // The multiplayer and eligible co-op campaigns, in tab order from the local data.
    [[nodiscard]] const std::vector<sim::galactic::MpLobbyCampaign>& campaigns() const;
    [[nodiscard]] std::optional<core::Diagnostic> load_error() const;
    void set_adapter_factory(AdapterFactory factory) { adapter_factory_ = std::move(factory); }
    void set_mounted_mods(std::vector<std::uint8_t> value) { mounted_mods_ = std::move(value); }
    [[nodiscard]] const std::vector<std::uint8_t>& mounted_mods() const noexcept { return mounted_mods_; }
    // Whether a campaign match can be built here (the GCMP-03 driver installed its adapter factory).
    [[nodiscard]] bool can_start() const noexcept { return static_cast<bool>(adapter_factory_); }
    // Qualification only, set before opening a room. The ordinary offer retains its default cap.
    void acceptance_final_tick(std::uint64_t tick) { acceptance_final_tick_ = tick; }
    [[nodiscard]] std::uint64_t final_tick_count() const noexcept {
        return acceptance_final_tick_ ? acceptance_final_tick_ : sim::galactic::galactic_max_ticks;
    }

    // Lobby hooks (net::LobbyHooks::validate / offer) for a room in Galactic Conquest mode.
    [[nodiscard]] core::Result<void> validate(const net::LobbySnapshot&, bool starting) const;
    [[nodiscard]] core::Result<net::SetupOffer> offer(const net::LobbySnapshot&, const std::vector<net::SetupSeat>&) const;
    [[nodiscard]] core::Result<std::vector<sim::IdentityDomain>> preparation_inputs(
        const net::LobbySnapshot&, const std::vector<net::SetupSeat>&) const;
    [[nodiscard]] core::Result<std::vector<sim::IdentityDomain>> preparation_domains(const net::SetupOffer&) const;
    [[nodiscard]] core::Result<std::vector<core::ContentLayerIdentity>> preparation_mounts(
        const net::LobbySnapshot&, const net::SetupOffer*) const;
    // #6045: the same offer, derived on a worker thread so the window and the lobby keep running for the 10-20 minutes a
    // large mod chain needs. Answers net::start_pending_diagnostic until the derivation has finished, then the offer (or its
    // failure) exactly once. Call it again with the same arguments every pump. A call whose inputs differ from the running
    // derivation's waits for that derivation to end (one derivation runs per process at a time); `prepare_progress()` says so.
    // Nonce-bound Start requires the exact frozen snapshot; serial Start retains its readiness/revision memo rules. The worker reads the loaded
    // content (immutable after the load) and writes only its own job record; this backend is the only owner of the result.
    // The derivation has no stop point: it cannot be interrupted. Abandoned nonce-bound results never seed a retry.
    [[nodiscard]] static core::Result<net::SetupOffer> offer_async(const std::shared_ptr<GalacticLobbyBackend>&,
        const net::LobbySnapshot&, const std::vector<net::SetupSeat>&);
    struct PrepareProgress {
        enum class State : std::uint8_t { idle, running, waiting_for_earlier };
        State state{State::idle};
        std::chrono::seconds elapsed{};   // of the running derivation (the earlier one while waiting)
    };
    [[nodiscard]] PrepareProgress prepare_progress() const;
    // Nobody waits any more (Start cancelled): progress reads idle; an in-flight worker finishes only to release its result.
    void abandon_prepare() noexcept;
    void release_prepare() noexcept;
    // Setup providers (net::SetupProviders::identify / build) on every peer, including the host.
    [[nodiscard]] core::Result<sim::MatchAgreement> identify(const net::SetupOffer&);
    [[nodiscard]] core::Result<std::unique_ptr<net::SessionAdapter>> build(const net::SetupOffer&);
    // One log line of sanitised short digests: the mounted layers, the two content digests, the offer's payload, AI claims,
    // tick-zero claim and identity domains. No names, addresses or room codes. The host's offer and the guest's own derived
    // offer carry equal values when the peers agree, so the two logs compare line by line.
    [[nodiscard]] std::string start_summary(const net::SetupOffer& offer, bool host) const;
    // The agreement the last identify derived on this peer, or null: the session the adapter takes over.
    [[nodiscard]] std::shared_ptr<const campaign::MpAgreement> agreement() const;
    [[nodiscard]] std::shared_ptr<const platform::CampaignServiceConfiguration> runtime_services() const;
    // #6012: what a save of this room's game records about its campaign (the campaign_identity section the lobby's
    // Load Game lists saves by). The agreed-start path has no prepared campaign to take it from, so it comes from the
    // room's options and the installed data. Absent when the content is not loaded or the campaign is unknown.
    [[nodiscard]] std::optional<platform::LiveSaveCampaignSource> save_campaign_source(const net::GalacticLobbyOptions&) const;

    struct CheckpointCandidate {
        sim::galactic::MpRoster original_roster;
        checkpoint::GameResumeMetadata saved;
        std::uint64_t seed{};
    };
    struct PreparedCheckpoint {
        net::SaveManifest manifest;
        net::LobbySnapshot snapshot;
        std::shared_ptr<campaign::MpAgreement> agreement;
        std::shared_ptr<const std::vector<std::uint8_t>> bytes;
        std::string hash;
    };
    [[nodiscard]] core::Result<PreparedCheckpoint> prepare_checkpoint(const net::LobbySnapshot&,
        sim::galactic::MpRoster, std::vector<net::SetupSeat>,
        std::shared_ptr<const std::vector<std::uint8_t>>, std::uint64_t epoch) const;
    [[nodiscard]] core::Result<void> commit_checkpoint(PreparedCheckpoint, const net::LobbySnapshot&);
    // No file access or owner mutation; returns the saved faction/seat facts for assignment.
    [[nodiscard]] core::Result<CheckpointCandidate> inspect_checkpoint(std::span<const std::uint8_t>) const;
    // PR3 seam: host supplies original saved campaign roster and an explicit logical-seat
    // assignment in snapshot order. Selection never changes the saved owner/AI/controller roster.
    [[nodiscard]] core::Result<void> select_checkpoint(const net::LobbySnapshot&,
        sim::galactic::MpRoster, std::vector<net::SetupSeat>,
        std::shared_ptr<const std::vector<std::uint8_t>>, std::uint64_t epoch);
    struct CheckpointView {
        sim::galactic::MpRoster roster;
        std::uint64_t completed_tick{};
        std::vector<net::SetupSeat> bindings;
        std::vector<net::PeerIndex> peers;
    };
    // Small read-only facts from the staging manifest, available before private restore completes.
    [[nodiscard]] std::optional<CheckpointView> checkpoint_view() const;
    void clear_checkpoint();
    [[nodiscard]] net::SaveTransferView checkpoint_progress() const { return transfer_->view(); }
    // The lobby log observer (#6178 follow-up): transfer events from the net layer plus this backend's own select/receive failures.
    void set_checkpoint_trace(net::GalacticCheckpointTransfer::Trace trace);
    [[nodiscard]] std::shared_ptr<const net::SaveManifest> checkpoint_manifest() const { return transfer_->manifest(); }
    [[nodiscard]] std::uint64_t selection_epoch() const noexcept { return selection_epoch_; }
    [[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>> checkpoint_bytes() const noexcept {
        return resume_ ? (resume_->bytes ? resume_->bytes : transfer_->restored_bytes()) : nullptr;
    }
    [[nodiscard]] net::GalacticRecoveryWorker::Restore recovery_factory(
        const net::SetupOffer&, std::size_t workers, net::GalacticClockPolicy = net::GalacticClockPolicy::limited) const;
    [[nodiscard]] std::uint64_t initial_tick() const noexcept;
    [[nodiscard]] std::map<sim::SeatId, std::uint64_t> producer_sequences() const;
    [[nodiscard]] core::Result<std::vector<net::SetupSeat>> bind_setup_seats(const net::LobbySnapshot&,
        const std::vector<net::SetupSeat>&) const;
    [[nodiscard]] std::shared_ptr<net::GalacticCheckpointWorker> checkpoint_worker() const { return transfer_; }
    void pump_transfer(net::MessageTransport&, const net::LobbySnapshot&, std::uint64_t now);
    void receive_transfer(net::MessageTransport&, const net::LobbySnapshot&, const net::Received&, std::uint64_t now);

private:
    // Runs the loader once. False when the content is unavailable (the error is kept).
    bool ensure() const;
    mutable GalacticLobbyLoader loader_;
    mutable GalacticLobbyContent content_;
    mutable campaign::MpLocalContent local_;
    mutable std::vector<sim::galactic::MpLobbyCampaign> campaigns_;
    mutable std::optional<core::Diagnostic> load_error_;
    // The loader runs under `load_mutex_` only; `mutex_` is never held across it, so a UI-thread call that needs just
    // the agreement (or the load error after the load) is not stuck behind a minutes-long content load.
    mutable std::mutex load_mutex_;
    mutable std::atomic<bool> ready_{};
    mutable bool loading_started_{};
    std::atomic<bool> prefetching_{};
    AdapterFactory adapter_factory_;
    std::vector<std::uint8_t> mounted_mods_;
    std::uint64_t acceptance_final_tick_{};
    // One local preparation for an exact frozen attempt. Content and callbacks are immutable for this backend.
    struct Preparation {
        net::LobbySnapshot snapshot;
        std::vector<net::SetupSeat> seats;
        net::SetupOffer offer;
        std::shared_ptr<campaign::MpAgreement> agreement;
        std::string state_hash;
        std::vector<sim::IdentityDomain> domains{};
    };
    mutable std::mutex preparation_mutex_;
    mutable std::optional<Preparation> preparation_;
    // The derivation of one frozen attempt without touching `preparation_` (offer() and the worker both call it).
    [[nodiscard]] core::Result<Preparation> derive_preparation(const net::LobbySnapshot&, const std::vector<net::SetupSeat>&) const;
    // #6045 asynchronous derivation. `job_` is the one job this backend asked for; the process runs at most one job.
    struct PrepareJob;
    [[nodiscard]] static std::mutex& prepare_registry_mutex();
    [[nodiscard]] static PrepareJob*& prepare_registry_running();
    mutable std::mutex job_mutex_;
    std::shared_ptr<PrepareJob> job_;
    std::atomic<bool> wanted_{}, waiting_{}, release_{};
    mutable std::mutex mutex_;
    std::shared_ptr<campaign::MpAgreement> agreement_;
    bool nonce_bound_agreement_{};
    using ResumeSelection = PreparedCheckpoint;
    std::shared_ptr<net::GalacticCheckpointWorker> transfer_{std::make_shared<net::GalacticCheckpointWorker>()};
    net::GalacticCheckpointTransfer::Trace checkpoint_trace_;
    void trace_checkpoint(std::string_view event, const std::string& detail) const { if (checkpoint_trace_) checkpoint_trace_(event, detail); }
    // A failure that persists across frames (a select that keeps failing, a room that stays changed) is traced once per key.
    void trace_checkpoint_once(std::string_view event, const std::string& key, const std::string& detail) const {
        auto& last = traced_once_[std::string(event)];
        if (last == key) return;
        last = key; trace_checkpoint(event, detail);
    }
    mutable std::map<std::string, std::string> traced_once_;
    std::optional<ResumeSelection> resume_;
    std::uint64_t selection_epoch_{};
    bool cancel_transfer_{};
    [[nodiscard]] core::Result<void> restore_transfer(const net::SaveManifest&,
        const net::LobbySnapshot&, std::span<const std::uint8_t>);
    // #6130: the guest's restore of the received file runs on its own thread, so the lobby keeps drawing. The transfer owner
    // polls this from pump: false while the worker runs, true once the selection is installed (on the polling thread),
    // a diagnostic on refusal. `pumping_snapshot_` is the room as the current receive/pump call saw it.
    struct RestoreJob;
    std::shared_ptr<RestoreJob> restore_job_;
    const net::LobbySnapshot* pumping_snapshot_{};
    [[nodiscard]] core::Result<bool> restore_transfer_async(const net::SaveManifest&,
        std::shared_ptr<const std::vector<std::uint8_t>>);
};

} // namespace eawr::presentation::godot_backend
