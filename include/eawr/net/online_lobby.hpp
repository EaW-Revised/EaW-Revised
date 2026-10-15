#pragma once

#include "eawr/core/result.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace eawr::net {
// Display metadata only. None of these fields authorize room mutations or peers.
struct OnlineRoomMetadata {
    std::string name, host_name, build, protocol, content, mod, mode, map;
    std::uint32_t occupancy{1}, capacity{9};
    bool starting{}, closed{};
    friend bool operator==(const OnlineRoomMetadata&, const OnlineRoomMetadata&) = default;
};
struct OnlineRoomRow {
    std::string room;
    OnlineRoomMetadata metadata;
    bool passworded{};
    friend bool operator==(const OnlineRoomRow&, const OnlineRoomRow&) = default;
};
struct OnlineRoomPage { std::vector<OnlineRoomRow> rooms; std::string next; };
struct OnlineRoomPublication { std::string room, ownership; std::uint32_t lease_ms{30000}; };
struct OnlineRoomAdmission {
    std::string room; std::uint32_t seat{};
    // A consumed directory binding, returned only over the admitted guest's TLS
    // connection. Never copy this capability into a row, chat, profile or report.
    std::string bridge_invitation{};
    // N-peer room scope (slice N3): the reserved seat (the transport peer index) and its member capability, present
    // only when the host has opened the room's signalling scope. The capability is a secret like the invitation.
    std::uint32_t slot{};
    std::string member_capability{};
};
struct OnlineChatLine {
    std::uint32_t sequence{}, seat{};
    std::string name, text;
};
struct OnlineRoomMember { std::uint32_t seat{}; std::string name; };
struct OnlineChatPage { std::vector<OnlineChatLine> lines; std::uint32_t through{}; std::vector<OnlineRoomMember> members; };

// Blocking, bounded service calls: invoke on an I/O worker, never a frame/sim thread.
class OnlineLobbyService {
public:
    virtual ~OnlineLobbyService() = default;
    virtual core::Result<OnlineRoomPage> list(std::string after = {}, std::uint32_t limit = 50) = 0;
    virtual core::Result<OnlineRoomPublication> publish(OnlineRoomMetadata metadata, std::string password = {}) = 0;
    virtual core::Result<OnlineRoomPublication> publish_bridge(OnlineRoomMetadata metadata, std::string password, std::string invitation, std::string /*ownership*/ = {}) {
        if (invitation.empty()) return publish(std::move(metadata), std::move(password));
        core::Diagnostic error; error.code = "EAWR-NET-1752"; error.message = "This room service cannot connect a game.";
        return core::Result<OnlineRoomPublication>::failure(std::move(error));
    }
    virtual core::Result<void> heartbeat(const OnlineRoomPublication&, const OnlineRoomMetadata&) = 0;
    virtual core::Result<void> remove(const OnlineRoomPublication&) = 0;
    virtual core::Result<OnlineRoomAdmission> join(std::string room, std::string password, std::string name) = 0;
    virtual core::Result<void> leave() = 0;
    virtual core::Result<void> send_chat(std::string text) = 0;
    virtual core::Result<OnlineChatPage> chat(std::uint32_t after) = 0;
    virtual core::Result<void> kick(const OnlineRoomPublication&, std::uint32_t seat) = 0;
};
} // namespace eawr::net
