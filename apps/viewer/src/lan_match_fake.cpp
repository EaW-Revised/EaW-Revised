#include "lan_match.hpp"
#include <utility>
#include "eawr/net/lobby_session.hpp"

namespace eawr::presentation::godot_backend {
namespace {
std::string connection_message(const LanView& view) {
    switch (view.recovery) {
    case LanRecovery::waiting: {
        auto name = net::sanitize_lobby_text(view.peer_name, 64);
        // Also remove directional marks and the deprecated directional controls.
        for (const auto token : {"\xD8\x9C", "\xE2\x80\x8E", "\xE2\x80\x8F", "\xE2\x81\xAA", "\xE2\x81\xAB", "\xE2\x81\xAC", "\xE2\x81\xAD", "\xE2\x81\xAE", "\xE2\x81\xAF"})
            for (auto at = name.find(token); at != std::string::npos; at = name.find(token)) name.erase(at, std::char_traits<char>::length(token));
        return "Waiting for " + (name.empty() ? std::string("Opponent") : name) + "..."
            + (view.link_stats && view.link_stats->sample_count ? "\nConnection: " + std::to_string(lan_lobby_ping(view.link_stats)) + " ms" : "\nConnection status unavailable.");
    }
    case LanRecovery::held: return "Connection lost. Reconnecting...";
    case LanRecovery::catching_up: return "Rejoining game..."
        + (view.catch_up_tick ? "\nVerified completed tick: " + std::to_string(*view.catch_up_tick) : std::string{});
    case LanRecovery::proving: return "Checking synchronized state...";
    case LanRecovery::rejoin_failed: {
        const auto reason = view.diagnostic_code == net::reconnect_errors::auth
            ? "The returning connection could not be authenticated."
            : view.diagnostic_code == "EAWR-NET-1025" ? "Local reconnect credentials are unavailable."
            : view.diagnostic_code == "EAWR-NET-1026" ? "Local reconnect credential limit was exceeded."
            : "Your reserved seat is no longer available.";
        if (view.diagnostic_code != net::reconnect_errors::auth && view.diagnostic_code != "EAWR-NET-1025"
            && view.diagnostic_code != "EAWR-NET-1026" && view.diagnostic_code != net::reconnect_errors::timeout
            && view.diagnostic_code != net::reconnect_errors::unavailable && view.diagnostic_code != net::reconnect_errors::stale)
            return "Could not rejoin this game.";
        return "Could not rejoin this game.\n" + std::string(reason);
    }
    case LanRecovery::desync: return "Players' game states differ. Play is paused.";
    case LanRecovery::resyncing: return "Restoring synchronized game state...";
    case LanRecovery::recovered: return "Game synchronized.";
    case LanRecovery::failed: return "Synchronization could not be restored. This match cannot continue.";
    case LanRecovery::none: break;
    }
    if (view.desync_tick && view.reason == LanReason::desync) return "Desync at tick " + std::to_string(*view.desync_tick) + "\n" + view.bundle_path;
    if (!view.message.empty()) return view.message;
    switch (view.reason) {
    case LanReason::version: return "The other player uses a different network version.";
    case LanReason::build: return "The other player uses a different game build.";
    case LanReason::identity: return "The map, game files or match settings do not match.";
    case LanReason::refused: return "The other player refused the connection.";
    case LanReason::timeout: return "The other player did not respond in time.";
    case LanReason::peer_left: return "Opponent left.";
    case LanReason::cancelled: return "Connection cancelled.";
    case LanReason::desync: return "The match stopped because the game states differ.";
    case LanReason::none: break;
    }
    switch (view.phase) {
    case LanPhase::idle: return "Host a LAN battle or join by IP and port.";
    case LanPhase::connecting: return view.host ? (view.human_owners.size() > 2
        ? "Waiting for " + std::to_string(view.human_owners.size()) + " human seats to join..."
        : "Waiting for a player to join...") : "Connecting...";
    case LanPhase::handshake: return "Checking game version and match settings...";
    case LanPhase::setup: return view.read_only ? (view.accept_enabled ? "Review the host setup, then Accept or Leave." : "Connected - waiting for host setup") : "Player connected. Choose settings and start.";
    case LanPhase::barrier: return "Waiting for all human seats to finish loading...";
    case LanPhase::running: return {};
    case LanPhase::reconnecting: return "Reconnecting seat " + std::to_string(view.reconnect_seat + 1) + "...";
    case LanPhase::resyncing: return "Resyncing...";
    case LanPhase::ended: return view.victory ? (*view.victory ? "Victory" : "Defeat") : "Match ended.";
    }
    return {};
}
}
std::string lan_message(const LanView& source) {
    auto view = source;
    if (view.phase == LanPhase::ended && view.recovery != LanRecovery::failed && view.recovery != LanRecovery::rejoin_failed
        && view.recovery != LanRecovery::desync) {
        view.recovery = LanRecovery::none;
        if (view.reason == LanReason::timeout || (view.reason == LanReason::peer_left
            && (view.message.starts_with("Waiting for ") || view.message.starts_with("Reconnect is unavailable")))) view.message.clear();
    }
    auto message = connection_message(view);
    if (view.grace_remaining_ms && view.recovery == LanRecovery::held)
        message += "\nInitial grace remaining: " + std::to_string((*view.grace_remaining_ms + 999) / 1000) + " s";
    if (view.recovery == LanRecovery::held || view.recovery == LanRecovery::catching_up || view.recovery == LanRecovery::proving)
        message += "\nGrace " + std::to_string(view.grace_ms / 1000) + " s | Progress timeout "
            + std::to_string(view.progress_timeout_ms / 1000) + " s | Incident cap " + std::to_string(view.hard_cap_ms / 1000) + " s";
    if (view.recovery == LanRecovery::resyncing)
        message += "\nRecovery timeout: " + std::to_string(view.resync_timeout_ms / 1000) + " s";
    if (view.recovery != LanRecovery::none && view.desync_tick)
        message += "\nIncident at tick " + std::to_string(*view.desync_tick);
    if (view.recovery != LanRecovery::none && !view.bundle_path.empty()) message += "\nLocal diagnostic details: " + view.bundle_path;
    if (!view.input_feedback.empty() && (!lan_gameplay_enabled(view) || view.recovery == LanRecovery::recovered)) message += "\n" + view.input_feedback;
    if (!view.diagnostic_save_notice.empty() && (view.recovery != LanRecovery::none || view.phase != LanPhase::running)) message += "\n" + view.diagnostic_save_notice;
    if (!view.diagnostic_code.empty() && message.find(view.diagnostic_code) == std::string::npos) message += "\n" + view.diagnostic_code;
    if (view.host && view.phase != LanPhase::ended && view.mapping.phase != net::MappingPhase::disabled) {
        if (!message.empty()) message += "\n";
        if (!view.mapping.code.empty()) message += view.mapping.code + ": ";
        message += view.mapping.message;
    }
    return message;
}
std::string lan_incident_message(const LanView& view) {
    std::string text = view.reconnect_warning;
    if (!view.service_notice.empty()) { if (!text.empty()) text += "\n"; text += view.service_notice; }
    if (view.desync_tick) {
        if (!text.empty()) text += "\n";
        text += "Previous synchronization incident at tick " + std::to_string(*view.desync_tick) + ". ";
        text += !view.diagnostic_saved ? "Diagnostic details could not be saved. Retry Save."
            : view.diagnostic_saved_automatically ? "Diagnostic details were saved automatically." : "Diagnostic details were saved locally.";
    }
    if (!view.input_feedback.empty()) { if (!text.empty()) text += "\n"; text += view.input_feedback; }
    if (!view.diagnostic_save_notice.empty()) { if (!text.empty()) text += "\n"; text += view.diagnostic_save_notice; }
    return text;
}
namespace {
class FakeLanMatch final : public LanMatch {
public:
    explicit FakeLanMatch(std::string_view state) {
        view_.phase = LanPhase::setup;
        view_.connected = true;
        view_.start_enabled = true;
        view_.host = true;
        if (state == "host_waiting") { view_.phase = LanPhase::connecting; view_.connected = view_.start_enabled = false; }
        else if (state == "joined") { view_.host = false; view_.read_only = true; view_.start_enabled = false; }
        else if (state == "review") { view_.host = false; view_.read_only = true; view_.start_enabled = false; view_.accept_enabled = true;
            view_.setup_summary = "Host setup: Coruscant\nSeed 42 | Victory: eliminate opponents | Credits 10000 | Tech 1/5\nHeroes on | Superweapons on | Starting units on | Base on | Events off\nSlot 1 Rebel Human | Team 1 | Red | Fleet: X-Wing\nSlot 2 Empire Human | Team 2 | Blue | Fleet: TIE Fighter\nSlot 3 Rebel AI | Team 1 | Green | Fleet: Z-95\nBuild: presentation fixture\nContent: fixture identity"; }
        else if (state == "refused") { view_.phase = LanPhase::ended; view_.reason = LanReason::build; view_.start_enabled = false; }
        else if (state == "waiting") { view_.phase = LanPhase::running; view_.recovery = LanRecovery::waiting; view_.message = "Waiting for Opponent..."; }
        else if (state == "reconnecting" || state == "held" || state == "catching_up" || state == "proving" || state == "recovery_barrier") {
            view_.phase = LanPhase::reconnecting;
            view_.recovery = state == "catching_up" ? LanRecovery::catching_up : (state == "proving" || state == "recovery_barrier") ? LanRecovery::proving : LanRecovery::held;
            view_.grace_ms = 17000; view_.progress_timeout_ms = 23000; view_.hard_cap_ms = 91000;
            if (view_.recovery == LanRecovery::held) view_.grace_remaining_ms = 12000;
            if (view_.recovery == LanRecovery::catching_up) view_.catch_up_tick = 87;
        }
        else if (state == "resyncing") { view_.phase = LanPhase::resyncing; view_.recovery = LanRecovery::resyncing; view_.resync_timeout_ms = 19000; }
        else if (state == "desync_paused" || state == "recovered" || state == "recovery_failed" || state == "rejoin_failed" || state == "recovery_disabled") {
            view_.phase = state == "recovered" ? LanPhase::running : state == "desync_paused" ? LanPhase::running : LanPhase::ended;
            view_.recovery = state == "recovered" ? LanRecovery::recovered : state == "desync_paused" ? LanRecovery::desync
                : state == "rejoin_failed" ? LanRecovery::rejoin_failed : LanRecovery::failed;
            view_.desync_tick = 120; view_.diagnostic_available = true; view_.diagnostic_saved = view_.diagnostic_saved_automatically = true; view_.bundle_path = "out/desync/match-120";
        }
        else if (state == "reconnect_unavailable") {
            view_.phase = LanPhase::running; view_.reconnect_warning = "Reconnect is unavailable for this match. Play can continue.";
        }
        else if (state == "recovered_unsaved") {
            view_.phase = LanPhase::running; view_.recovery = LanRecovery::recovered; view_.desync_tick = 120; view_.diagnostic_available = true;
        }
        else if (state == "barrier") { view_.phase = LanPhase::barrier; }
        else if (state == "peer_left" || state == "failed_peer") {
            view_.phase = LanPhase::ended; view_.reason = LanReason::peer_left;
            failed_driver_ = state == "failed_peer";
        }
        else if (state == "desync") { view_.phase = LanPhase::ended; view_.reason = LanReason::desync; view_.desync_tick = 120; view_.bundle_path = "out/desync/match-120"; }
        else if (state == "result" || state == "result_join") {
            view_.phase = LanPhase::ended; view_.reason = LanReason::peer_left;
            view_.message = "Victory - opponent left."; view_.victory = true;
            view_.host = state != "result_join";
        }
        else if (state == "loss") { view_.phase = LanPhase::ended; view_.victory = false; }
    }
    void configure_seats(std::vector<sim::tactical::PlayerId> owners) override {
        view_.human_owners = std::move(owners);
        view_.connected_seats = view_.connected ? static_cast<std::uint32_t>(view_.human_owners.size()) : 1U;
    }
    void host(std::uint16_t) override {}
    void join(std::string) override {}
    void cancel() override { view_ = {}; }
    bool request_abort() override { return lan_can_abort(view_); }
    void complete_abort() override {
        if (view_.phase != LanPhase::ended) cancel();
        view_.connected = false;
    }
    void acknowledge_recovery() override { if (view_.recovery == LanRecovery::recovered) { view_.recovery = LanRecovery::none; view_.diagnostic_save_notice.clear(); view_.input_feedback.clear(); } }
    void acknowledge_input_feedback() override { view_.input_feedback.clear(); }
    bool save_diagnostic_details() override {
        if (!view_.diagnostic_available) return false;
        if (!view_.diagnostic_saved) { view_.bundle_path = "out/desync/match-120"; view_.diagnostic_saved = true;
            view_.diagnostic_save_notice = "Diagnostic details saved locally: " + view_.bundle_path; return true; }
        view_.diagnostic_save_notice = (view_.diagnostic_saved_automatically ? "Diagnostic details were already saved automatically: " : "Diagnostic details are already saved locally: ") + view_.bundle_path; return true;
    }
    void pump_setup(const skirmish::FixtureOptions&) override {}
    void accept() override { view_.accept_enabled = false; }
    void start(const skirmish::FixtureOptions&) override {}
    std::optional<skirmish::FixtureOptions> take_start() override { return {}; }
    std::optional<skirmish::FixtureOptions> offered_setup() const override { return {}; }
    LanView view() const override { return view_; }
    sim::tactical::PlayerId local_player() const override { return view_.host ? 1U : 2U; }
    platform::LiveStepDriver driver(const sim::tactical::TacticalSetup&) override {
        if (!failed_driver_) return {};
        return [](platform::LiveStepContext&, std::span<const sim::tactical::PlayerCommand>, std::uint64_t) {
            core::Diagnostic diagnostic;
            diagnostic.code = "EAWR-NET-0305";
            diagnostic.message = "Presentation fixture peer closed before tick zero.";
            return core::Result<bool>::failure(std::move(diagnostic));
        };
    }
private:
    LanView view_;
    bool failed_driver_{};
};
}
std::shared_ptr<LanMatch> make_fake_lan_match(std::string_view state) { return std::make_shared<FakeLanMatch>(state); }
} // namespace eawr::presentation::godot_backend
