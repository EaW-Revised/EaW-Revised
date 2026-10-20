#include "lan_match_internal.hpp"

namespace eawr::presentation::godot_backend {
std::string online_pair_message(const net::GnsPairDiagnostic& pair) {
    switch (pair.phase) {
    case net::GnsPairPhase::absent: return "Waiting for connection";
    case net::GnsPairPhase::signalling: return "Connecting";
    case net::GnsPairPhase::handshaking: return "Checking connection";
    case net::GnsPairPhase::refreshing: return "Refreshing room credentials";
    case net::GnsPairPhase::repairing: return "Restoring connection";
    case net::GnsPairPhase::authenticating: return "Checking restored connection";
    case net::GnsPairPhase::open: return pair.route == net::GnsRoute::relayed ? "Connected through relay"
        : pair.route == net::GnsRoute::direct ? "Connected directly" : "Connected";
    case net::GnsPairPhase::failed:
    case net::GnsPairPhase::lost:
        switch (pair.code) {
        case 1790: return "Connection repair is unavailable";
        case 1791: return "Room credentials could not be refreshed";
        case 1792: return "Restored connection could not be authenticated";
        case 1793: return "Connection repair timed out";
        case 1794: return "Retained game data could not be restored";
        default: break;
        }
        return pair.relay_offered ? "Connection failed (relay was offered)" : "Connection failed; no relay was available";
    case net::GnsPairPhase::closed: return "Connection closed";
    default: return "Restoring connection";
    }
}
std::string online_mesh_failure_text(std::string_view code) {
    if (code == "EAWR-NET-1790") return "Connection repair is unavailable. You have left the match.";
    if (code == "EAWR-NET-1791") return "The online service could not restore your connection. You have left the match.";
    if (code == "EAWR-NET-1792") return "The returning connection could not be authenticated. You have left the match.";
    if (code == "EAWR-NET-1793") return "The connection could not be restored in time. You have left the match.";
    if (code == "EAWR-NET-1794") return "The connection could not catch up within the recovery limit. You have left the match.";
    if (code == net::mesh_errors::left) return "Your connection to the other players could not be restored. You have left the match.";
    if (code == net::mesh_errors::no_majority) return "The match ended: the players could not agree on which connections were lost.";
    if (code == net::mesh_errors::no_decision) return "The match ended: the players could not confirm who left.";
    if (code == net::mesh_errors::overlap) return "The match ended: another connection failed while a lost one was being settled.";
    if (code == net::mesh_errors::malformed) return "The match ended: a repair message could not be read.";
    return {};
}
void handle_online_start_failure(const core::Diagnostic& error,
    const std::function<void(const core::Diagnostic&)>& feedback,
    const std::function<void(const core::Diagnostic&)>& end_room) {
    feedback(error);
    if (error.code == "EAWR-NET-1021") end_room(error);
}
using namespace lan_detail;
net::LobbyReason lan_lobby_reason(const core::Diagnostic& error) {
    for (unsigned value = 0; value <= static_cast<unsigned>(net::LobbyReason::mesh_incomplete); ++value)
        if (error.code == net::lobby_diagnostic(static_cast<net::LobbyReason>(value), "").code) return static_cast<net::LobbyReason>(value);
    return net::LobbyReason::invalid_choice;
}
std::string lan_admission_name(const net::LobbySnapshot& room, net::PeerIndex peer) {
    const auto base = std::to_string(peer + 1);
    auto candidate = base;
    for (std::size_t suffix = 1; std::any_of(room.seats.begin(), room.seats.end(), [&](const auto& seat) { return seat.selection.name == candidate; }); ++suffix)
        candidate = base + "-" + std::to_string(suffix);
    return candidate;
}
std::shared_ptr<LanMatch> make_online_match(OnlineMatchOptions options, std::filesystem::path root, std::uint64_t delay) {
    auto result = std::make_shared<TcpLanMatch>(std::move(root), delay, false, std::filesystem::path{});
    result->configure_online(std::move(options));
    return result;
}
void shutdown_lan_drains() { drain_workers().shutdown(); }
std::shared_ptr<LanMatch> make_lan_match(std::filesystem::path root, std::uint64_t delay, bool smoke_progress, std::filesystem::path trace) {
    return std::make_shared<TcpLanMatch>(std::move(root), delay, smoke_progress, std::move(trace));
}
} // namespace eawr::presentation::godot_backend
