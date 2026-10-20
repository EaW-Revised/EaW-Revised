#pragma once

#include "eawr/net/gns_transport.hpp"
#include "gns_room_signal.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#if EAWR_HAS_GNS
#include <steam/steamnetworkingcustomsignaling.h>
#endif

namespace eawr::net::gns_detail {
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
core::Diagnostic error(unsigned number, std::string message);
// Diagnostics for godot.log (#4021). GNS threads and the I/O owner queue lines; the owner's poll hands them to the sink.
struct GnsLogState {
    std::mutex mutex;
    GnsLogSink sink;
    std::deque<std::string> pending;
    std::size_t dropped{};
    std::map<std::string, std::pair<Clock::time_point, std::size_t>> limited;
    std::atomic<bool> enabled{}, has_pending{};
    std::atomic<int> seat{-1};
    std::uint64_t generation{};
};
GnsLogState& gns_log();
// seat/peer < 0 are left out. The time is UTC of day, so lines from several machines can be lined up.
std::string gns_log_line(int seat, int peer, std::string_view body, std::string_view event = "gns");
#if EAWR_HAS_GNS
// `limited` lines (GNS warnings, which can repeat once per failed send during an outage) pass once per ten seconds per
// text, the next one carrying the count that was held back.
void gns_log_push(int seat, int peer, std::string_view text, bool limited = false, std::string_view event = "gns");
#endif
// I/O owner only: deliver what the GNS threads and the transports queued since the last poll.
void gns_log_flush();
#if EAWR_HAS_GNS
static_assert(gns_identity_max_bytes < SteamNetworkingIdentity::k_cchMaxGenericString);
// GNS spew: every warning or worse, and the ICE/TURN messages (the carried patch adds one per TURN reply).
void gns_spew(ESteamNetworkingSocketsDebugOutputType type, const char* text);
void u32(Bytes& bytes, std::uint32_t value);
// One numeric `turn:` URI (coturn) or up to eight comma-separated ones (a provider with several endpoints).
bool turn_uri_list(const std::string& list);
// Up to eight comma-separated printable "stun:" URIs, or none.
bool stun_uri_list(const std::string& list);
// The direct and paired adapters are exactly two peers; a room carries the agreed roster size (2..max_match_seats).
bool valid(const GnsOptions& options, bool room = false);
struct SignallingState {
    std::shared_ptr<GnsSignallingEndpoint> endpoint;
    // A room pair sends through the room to one destination slot instead of a paired endpoint.
    std::shared_ptr<GnsRoomEndpoint> room;
    PeerIndex destination{};
    std::optional<GnsFormationSecret> formation_grant; // immutable once GNS owns this signalling state
    std::atomic<bool> admitted{}, unavailable{}, refusing{};
    // Signals carried for this pair, for measuring ICE exchange cost (GNS thread writes the sent side).
    std::atomic<std::uint64_t> sent_blobs{}, sent_bytes{}, received_blobs{}, received_bytes{};
};
// One numeric turn: URI list plus single-credential checks shared by the paired and room factories.
std::string_view turn_invalid_check(const GnsTurnCredentials& turn);
bool turn_valid(const GnsTurnCredentials& turn);
void log_turn_refused(int seat, const std::optional<GnsTurnCredentials>& turn);
class Signal final : public ISteamNetworkingConnectionSignaling {
public:
    explicit Signal(std::shared_ptr<SignallingState> state);
    bool SendSignal(HSteamNetConnection, const SteamNetConnectionInfo_t&, const void* data, int size) override;
    void Release() override;
private:
    std::shared_ptr<SignallingState> state_;
};
#endif
} // namespace eawr::net::gns_detail
