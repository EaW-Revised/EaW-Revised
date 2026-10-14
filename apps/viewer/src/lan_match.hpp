#pragma once
#include "eawr/net/gns_signalling.hpp"
#include "eawr/net/tactical_input.hpp"
#include "eawr/net/match_options.hpp"
#include "eawr/net/port_mapping.hpp"
#include "eawr/net/transport.hpp"
#include "eawr/net/setup.hpp"
#include "eawr/net/lobby_session.hpp"
#include "eawr/presentation/ui/lan_lobby.hpp"

#include "eawr/platform/live_session.hpp"
#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/skirmish/setup.hpp"
#include <memory>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <limits>
#include <algorithm>

namespace eawr::presentation::godot_backend {
enum class LanPhase { idle, connecting, handshake, setup, barrier, running, reconnecting, resyncing, ended };
enum class LanReason { none, version, build, identity, refused, timeout, desync, peer_left, cancelled };
enum class LanRecovery { none, waiting, held, catching_up, proving, rejoin_failed, desync, resyncing, recovered, failed };
struct LanView {
    LanPhase phase{LanPhase::idle};
    LanReason reason{LanReason::none};
    bool host{}, connected{}, read_only{}, start_enabled{}, accept_enabled{};
    std::uint16_t port{47624};
    std::uint64_t input_delay{3};
    std::string peer_name{"Opponent"}, message, bundle_path, setup_summary;
    std::optional<std::uint64_t> desync_tick;
    std::optional<bool> victory;
    std::optional<net::DepartureOutcome> departure{};
    std::vector<sim::tactical::PlayerId> human_owners{1, 2};
    std::uint32_t connected_seats{1};
    net::PeerIndex seat{};
    sim::tactical::PlayerId player{1};
    net::MappingView mapping;
    // Published by the current I/O owner under the view mutex, never queried by UI.
    std::string invitation_code;
    std::chrono::steady_clock::time_point invitation_expires{};
    bool invitation_consumed{};
    std::string remote_endpoint;
    std::optional<net::LinkStats> link_stats;
    net::MatchOptions options{};
    std::uint64_t options_tick{};
    bool reconnect_enabled{}, rejoin_enabled{};
    bool credential_provider_available{};
    std::string reconnect_unavailable{"Reconnect requires a confidential credential provider."};
    std::string diagnostic_code;
    net::PeerIndex reconnect_seat{};
    std::uint64_t recovery_epoch{};
    LanRecovery recovery{LanRecovery::none};
    std::optional<std::uint64_t> grace_remaining_ms, catch_up_tick;
    std::uint64_t grace_ms{}, progress_timeout_ms{}, hard_cap_ms{}, resync_timeout_ms{};
    bool diagnostic_available{}, diagnostic_saved{}, diagnostic_saved_automatically{};
    std::string reconnect_warning;
    // Curated, secret-free warnings about the online service session and the match length (EAWR-NET-1730..1738).
    std::string service_notice;
    std::string diagnostic_save_notice, input_feedback;
    std::uint64_t active_adapter_publications{};
};
[[nodiscard]] inline bool lan_gameplay_enabled(const LanView& view) {
    return view.phase == LanPhase::running && (view.recovery == LanRecovery::none
        || view.recovery == LanRecovery::waiting || view.recovery == LanRecovery::recovered);
}
[[nodiscard]] inline bool lan_recovery_audio_suppressed(const LanView& view) {
    if (view.phase == LanPhase::ended) return false;
    return view.recovery == LanRecovery::held || view.recovery == LanRecovery::catching_up
        || view.recovery == LanRecovery::proving || view.recovery == LanRecovery::resyncing || view.recovery == LanRecovery::desync;
}
// Presentation lifetime only: paused/held frames and recovery jumps do not count.
class LanRecoveredNotice final {
public:
    bool advance(const LanView& view, double presented_tick, bool paused) {
        if (view.recovery != LanRecovery::recovered) { last_.reset(); elapsed_ = 0; return false; }
        if (publication_ != view.active_adapter_publications || epoch_ != view.recovery_epoch || incident_ != view.desync_tick) {
            last_.reset(); elapsed_ = 0; publication_ = view.active_adapter_publications; epoch_ = view.recovery_epoch; incident_ = view.desync_tick;
        }
        if (last_ && !paused && lan_gameplay_enabled(view)) elapsed_ += std::max(0.0, presented_tick - *last_);
        last_ = presented_tick;
        return elapsed_ >= 10.0 * sim::tactical::logical_frames_per_second;
    }
private:
    std::optional<double> last_;
    double elapsed_{};
    std::uint64_t publication_{}, epoch_{};
    std::optional<std::uint64_t> incident_;
};
[[nodiscard]] std::string lan_incident_message(const LanView& view);
// Online notices use curated text only; service diagnostics may contain implementation details.
[[nodiscard]] inline std::string online_lease_notice(const std::optional<net::GnsSessionLease>& lease,
        std::chrono::steady_clock::time_point now, std::string_view failure = {}) {
    using namespace std::chrono;
    if (!failure.empty() || (lease && lease->ends <= now)) {
        const bool maximum = failure == "EAWR-NET-1730" || (failure.empty() && lease && !lease->renewable);
        return std::string("The online service session has ended. A direct connection keeps playing; a relayed connection reaches its separate credential time limit.\n")
            + (maximum ? "EAWR-NET-1730" : "EAWR-NET-1731");
    }
    if (!lease) return {};
    const auto minutes_left = static_cast<long long>(ceil<minutes>(lease->ends - now).count());
    if (!lease->renewable) {
        if (minutes_left > 15) return {};
        return "The online service session reaches its maximum length in " + std::to_string(minutes_left)
            + " min. The relayed connection has a separate credential time limit.\nEAWR-NET-1730";
    }
    if (lease->last_refusal == "EAWR-NET-1733")
        return "Renewing the online service session failed. Retries are limited; the service session ends in "
            + std::to_string(minutes_left) + " min unless renewed. Relay credentials have a separate time limit.\nEAWR-NET-1733";
    if (minutes_left > 2) return {};
    return "The online service session is about to end. Renewal does not extend the relayed connection's credential time limit.\nEAWR-NET-1735";
}
[[nodiscard]] inline bool online_credentials_expired(const std::optional<net::GnsTurnCredentials>& turn,
        std::chrono::steady_clock::time_point paired_at, std::chrono::steady_clock::time_point now) {
    return turn && !turn->uri.empty() && turn->lifetime.count() > 0 && now >= paired_at + turn->lifetime;
}
[[nodiscard]] inline bool online_relay_allocation_ended(LanReason reason, const std::optional<net::GnsTurnCredentials>& turn,
        std::chrono::steady_clock::time_point paired_at, std::chrono::steady_clock::time_point now) {
    return reason == LanReason::peer_left && online_credentials_expired(turn, paired_at, now);
}
[[nodiscard]] inline std::string online_credential_notice(const std::optional<net::GnsTurnCredentials>& turn,
        std::chrono::steady_clock::time_point paired_at, std::chrono::steady_clock::time_point now) {
    using namespace std::chrono;
    if (!turn || turn->uri.empty() || turn->lifetime.count() <= 0) return {};
    const auto end = paired_at + turn->lifetime;
    if (now >= end) return "The relayed connection has reached its credential time limit. A direct connection keeps playing.\nEAWR-NET-1736";
    const auto left = ceil<minutes>(end - now).count();
    if (left > 15) return {};
    return std::string(left <= 2 ? "The relayed connection is about to reach its time limit in " : "The relayed connection reaches its time limit in ")
        + std::to_string(left) + " min. Renewal cannot extend it. A direct connection keeps playing.\nEAWR-NET-1736";
}
[[nodiscard]] inline std::string match_length_notice(std::uint64_t completed_tick, std::uint64_t final_tick) {
    constexpr std::uint64_t warn_ticks = 15ULL * 60ULL * sim::tactical::logical_frames_per_second;
    if (final_tick == 0 || completed_tick >= final_tick || final_tick - completed_tick > warn_ticks) return {};
    constexpr std::uint64_t per_minute = 60ULL * sim::tactical::logical_frames_per_second;
    const auto minutes = (final_tick - completed_tick + per_minute - 1) / per_minute;
    return "This match reaches the maximum match length in " + std::to_string(minutes) + " min and then ends.\nEAWR-NET-1738";
}
[[nodiscard]] inline bool lan_reconnect_disabled_notice(const LanView& view) {
    return (view.phase == LanPhase::setup || view.phase == LanPhase::connecting)
        && view.credential_provider_available && !view.reconnect_enabled;
}
[[nodiscard]] inline std::uint32_t lan_lobby_ping(const std::optional<net::LinkStats>& stats) {
    if (!stats || stats->sample_count == 0) return 0; // Lobby model's unknown sentinel.
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(stats->smoothed_rtt).count();
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(milliseconds, 1, std::numeric_limits<std::uint32_t>::max()));
}
inline void apply_lan_lobby_ping(std::vector<presentation::ui::LobbyGame>& games, const LanView& view) {
    if (!view.connected || view.remote_endpoint.empty()) return;
    for (auto& game : games)
        if (game.address == view.remote_endpoint) game.ping_ms = lan_lobby_ping(view.link_stats);
}
// Before admission (or after a terminal failure), leaving must not await a sim tick.
[[nodiscard]] inline bool lan_can_abort(const LanView& view) {
    return view.phase != LanPhase::running || !lan_gameplay_enabled(view);
}
[[nodiscard]] inline bool lan_escape_aborts(const LanView& view) {
    return lan_can_abort(view) && !view.victory.has_value();
}
[[nodiscard]] inline bool live_begin_barrier(bool multiplayer) { return !multiplayer; }
// LAN staging application options are bounded, canonical, and validated against loaded maps.
[[nodiscard]] core::Result<net::LobbyGame> encode_lan_lobby_game(const skirmish::FixtureOptions&, std::uint64_t input_delay = 3);
[[nodiscard]] core::Result<skirmish::FixtureOptions> decode_lan_lobby_game(const net::LobbySnapshot&, std::uint64_t* input_delay = nullptr);
[[nodiscard]] net::LobbyReason lan_lobby_reason(const core::Diagnostic&);
[[nodiscard]] std::string lan_admission_name(const net::LobbySnapshot&, net::PeerIndex);
using LanLobbyBuilder = std::function<core::Result<sim::tactical::TacticalSetup>(const skirmish::FixtureOptions&)>;
struct LanLobbyState {
    net::LobbySnapshot snapshot;
    std::deque<net::LobbyChat> chat;
    std::optional<net::LobbyNotice> notice;
    bool preparing{}, local_ready{}, ready_enabled{}, start_enabled{}, identity_pending{};
};
// Setup calls belong to the Godot main thread. driver() transfers transport ownership
// between polls to the simulation thread; thereafter only view() is read by the UI.
// Replaceable delivery policy; production LAN trusts its admitted plaintext streams.
// Credentials stay outside setup, simulation, replay and presentation data.
using LanCredentialProvider = std::function<core::Result<net::ReconnectTokens>(const net::SetupOffer&)>;
class LanMatch {
public:
    virtual ~LanMatch() = default;
    // Names from the mounted catalog, used to resolve offered faction IDs.
    virtual void configure_factions(std::vector<std::string>) {}
    // Opt-in for the staging screen. The legacy setup screen keeps its existing path.
    virtual bool lobby_enabled() const { return false; }
    virtual void configure_lobby(std::string, std::uint32_t, std::vector<skirmish::SetupMap>, LanLobbyBuilder) {}
    virtual bool lobby_select(net::LobbySelection) { return false; }
    virtual bool lobby_assign(net::PeerIndex, net::LobbySelection) { return false; }
    virtual bool lobby_game(const skirmish::FixtureOptions&) { return false; }
    virtual bool lobby_ready(bool) { return false; }
    virtual bool lobby_chat(std::string) { return false; }
    virtual bool lobby_kick(net::PeerIndex) { return false; }
    virtual void lobby_leave() {}
    virtual std::optional<LanLobbyState> lobby_state() const { return {}; }
    // Stable transport seat order; shared entries control the same faction owner.
    // Call before Host/Join. A joining client only selects the advertised count;
    // its authoritative owner comes from the host's validated offer.
    virtual void configure_seats(std::vector<sim::tactical::PlayerId>) {}
    virtual void host(std::uint16_t port) = 0;
    virtual void set_recovery_policy(net::SetupRecoveryPolicy, LanCredentialProvider = {}) {}
    virtual bool rejoin() { return false; }
    // Local sink only. Available after a diagnostic bundle has been captured.
    virtual bool save_diagnostic_details() { return false; }
    virtual void acknowledge_recovery() {}
    virtual void acknowledge_input_feedback() {}
    virtual void set_discard_observer(std::function<void(std::uint64_t)>) {}
    // Applies only to the next host() call. Default off; joining never maps a port.
    virtual void set_port_mapping(bool) {}
    virtual void join(std::string endpoint) = 0;
    virtual void cancel() = 0;
    // Reserve an abort atomically against runtime admission. Release resources only
    // after the driver has been joined; terminal diagnostics survive that release.
    virtual bool request_abort() = 0;
    virtual void complete_abort() = 0;
    virtual void pump_setup(const skirmish::FixtureOptions& selected) = 0;
    virtual void accept() = 0;
    virtual void start(const skirmish::FixtureOptions& selected) = 0;
    virtual std::optional<skirmish::FixtureOptions> take_start() = 0;
    virtual std::optional<skirmish::FixtureOptions> offered_setup() const = 0;
    virtual LanView view() const = 0;
    virtual sim::tactical::PlayerId local_player() const = 0;
    virtual platform::LiveStepDriver driver(const sim::tactical::TacticalSetup& setup) = 0;
    // MP-06 requests wait for canonical future-tick admission. MP-33 pause is refused.
    virtual bool request_speed(std::uint32_t) { return false; }
    virtual void set_submission_observer(std::function<void(std::uint64_t, std::uint64_t)>) {}
    virtual void set_input_epoch_provider(std::function<std::optional<std::uint64_t>()>) {}
    // Acceptance-only event timing; never enters the lockstep wire or replay.
    virtual void note_input(std::uint64_t, std::uint64_t, std::uint64_t) {}
    virtual void note_native_input(std::uint64_t, std::uint64_t) {}
    virtual void note_issue(std::uint64_t, std::uint64_t) {}
};
// Every battle intent (world, ability and economy) passes the same live gate.
class LanCommandSink final : public presentation::ui::CommandSink {
public:
    LanCommandSink(presentation::ui::CommandSink& sink, std::shared_ptr<LanMatch> match,
        std::function<void(const core::Diagnostic&)> feedback = {})
        : sink_(sink), match_(std::move(match)), feedback_(std::move(feedback)) {}
    core::Result<void> issue(const presentation::ui::TacticalIntent& intent) override {
        if (!lan_gameplay_enabled(match_->view())) {
            core::Diagnostic error; error.code = "EAWR-NET-1800";
            error.message = "Gameplay input is unavailable until synchronization completes.";
            if (feedback_) feedback_(error);
            return core::Result<void>::failure(std::move(error));
        }
        // Clear the previous notice before issue: a concurrent drain may publish
        // a new cancellation notice as soon as the scheduler accepts this input.
        match_->acknowledge_input_feedback();
        return sink_.issue(intent);
    }
private:
    presentation::ui::CommandSink& sink_;
    std::shared_ptr<LanMatch> match_;
    std::function<void(const core::Diagnostic&)> feedback_;
};
// Main-thread exit seam, also exercised by the engine-free viewer contracts.
// `stop` ends whichever live source the caller owns: a standalone LiveSession or a borrowed tactical feed.
[[nodiscard]] inline bool try_abort_lan(LanMatch& match, const std::function<void()>& stop) {
    if (!match.request_abort()) return false;
    if (stop) stop();
    match.complete_abort();
    return true;
}
[[nodiscard]] inline bool try_abort_lan(LanMatch& match, platform::LiveSession* session) {
    return try_abort_lan(match, std::function<void()>([session] { if (session) session->stop(); }));
}
[[nodiscard]] std::shared_ptr<LanMatch> make_lan_match(std::filesystem::path diagnostic_root = {}, std::uint64_t input_delay = 3,
    bool smoke_progress = false, std::filesystem::path trace = {});
// After live drivers stop, settle remaining bounded drains before unloading the
// viewer extension. Ordinary match/UI cleanup never waits for these workers.
void shutdown_lan_drains();
// Explicit GPU presentation fixture; never selected by the production Host/Join controls.
[[nodiscard]] std::shared_ptr<LanMatch> make_fake_lan_match(std::string_view state);
[[nodiscard]] std::string lan_message(const LanView& view);
} // namespace eawr::presentation::godot_backend
