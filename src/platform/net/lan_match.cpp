#include "lan_match_internal.hpp"
#include "eawr/campaign/mp_agreement.hpp"
#include "eawr/net/galactic_setup.hpp"

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
namespace {
// The plain-words sentence for the part of the players' setups a refusal message names, or empty when it does not name one.
std::string_view setup_difference_sentence(std::string_view message) {
    constexpr std::string_view mismatch = "match identity mismatch: ";
    constexpr std::string_view ai = "The AI configuration differs", data = "The mods or campaign data differ", rules = "The game version differs";
    if (message.starts_with(mismatch)) {
        const auto domain = message.substr(mismatch.size());
        namespace domains = campaign::mp_domains;
        if (domain == domains::ai_data || domain == domains::ai_controllers) return ai;
        if (domain == domains::content || domain == domains::campaign || domain == domains::map
            || domain == domains::story || domain == domains::resolver) return data;
        if (domain == domains::rules) return rules;
        return {};
    }
    if (message.starts_with(net::setup_offer_differs_message)) {
        const auto rest = message.substr(net::setup_offer_differs_message.size());
        if (rest == std::string(": ").append(net::setup_difference_ai_configuration)) return ai;
        if (rest == std::string(": ").append(net::setup_difference_start_state)) return data;
    }
    return {};
}
} // namespace

std::string online_start_failure_text(const core::Diagnostic& diagnostic, const LanReason reason, const bool galactic) {
    // Online skirmish rooms fail Start with the same codes; their text stays the generic online one.
    if (!galactic) return {};
    const auto code = [](net::LobbyReason value) { return net::lobby_diagnostic(value, "").code; };
    if (diagnostic.code == code(net::LobbyReason::setup_failed)) {
        if (reason == LanReason::timeout) return "The campaign setup took too long. Try again.";
        if (reason == LanReason::peer_left) return "The other player left while the campaign was being set up.";
        std::string text = "The campaign could not be set up with the other player.";
        if (const auto sentence = setup_difference_sentence(diagnostic.message); !sentence.empty())
            text += " " + std::string(sentence) + " between your games.";
        return text + " Both players need the same game version, mods and campaign data.";
    }
    if (diagnostic.code == code(net::LobbyReason::not_ready)) return "The other player was no longer ready when the campaign started. Try again.";
    if (diagnostic.code == code(net::LobbyReason::link_closed)) return "The connection to the other player was lost while the campaign was being set up.";
    if (diagnostic.code == code(net::LobbyReason::left)) return "A player left the room while the campaign was being set up.";
    if (diagnostic.code == code(net::LobbyReason::kicked)) return "The host removed you from the room.";
    if (diagnostic.code == code(net::LobbyReason::host_closed)) return "The host closed the room.";
    return {};
}
core::Diagnostic galactic_start_aborted(const std::optional<net::LobbyNotice>& notice) {
    if (notice) return net::lobby_diagnostic(notice->reason, notice->message);
    return net::lobby_diagnostic(net::LobbyReason::setup_failed, "The campaign room ended before commitment.");
}
std::string failure_log_detail(const core::Diagnostic& diagnostic) {
    const bool known = diagnostic.code.starts_with("EAWR-NET-03")
        || (diagnostic.code.starts_with("EAWR-NET-16") && diagnostic.code != "EAWR-NET-1762" && diagnostic.code != "EAWR-NET-1764");
    if (!known || diagnostic.message.empty()) return {};
    // One line, no quotes, and no token that looks like a path: a derivation error may carry a file name.
    const auto text = net::sanitize_lobby_text(diagnostic.message, 160);
    std::string out;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto end = std::min(text.find(' ', at), text.size());
        auto token = text.substr(at, end - at);
        if (token.find_first_of("\\/@") != std::string::npos) token = "<path>";
        if (!token.empty()) out += (out.empty() ? "" : " ") + token;
        at = end + 1;
    }
    std::replace(out.begin(), out.end(), '"', '\'');
    return "message=\"" + out + "\"";
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
