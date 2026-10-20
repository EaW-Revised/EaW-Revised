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
// The room-less menu channel (#3758): who is in the online room browser and what they say there. A line carries the
// sender's display name only; the member id is a per-connection number that is never reused for a capability.
struct OnlineMenuLine { std::uint32_t sequence{}, member{}; std::string name, text; };
struct OnlineMenuMember { std::uint32_t id{}; std::string name; };
struct OnlineMenuPage {
    std::vector<OnlineMenuLine> lines;
    std::uint32_t through{};
    // The roster is sent only when it differs from the version the caller already holds.
    bool roster_present{};
    std::uint32_t roster_version{};
    std::vector<OnlineMenuMember> members;
};

// Blocking, bounded service calls: invoke on an I/O worker, never a frame/sim thread.
class OnlineLobbyService {
    template<class T> static core::Result<T> menu_unavailable() {
        core::Diagnostic error; error.code = "EAWR-NET-1521"; error.message = "Lobby chat is not available on this server.";
        return core::Result<T>::failure(std::move(error));
    }
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
    // Menu channel (#3758), on a connection of its own so a relay without it never disturbs the directory connection.
    // A relay that predates the channel answers menu_enter with EAWR-NET-1521; callers treat that as "not available on
    // this server" and stop asking. The defaults below behave like such a relay.
    virtual core::Result<OnlineMenuPage> menu_enter(std::string /*display_name*/) { return menu_unavailable<OnlineMenuPage>(); }
    virtual core::Result<void> menu_say(std::string /*text*/) { return menu_unavailable<void>(); }
    virtual core::Result<OnlineMenuPage> menu_poll(std::uint32_t /*after*/, std::uint32_t /*roster_version*/) { return menu_unavailable<OnlineMenuPage>(); }
    // Drops the menu connection (joining or hosting a room, or closing the browser). Never fails.
    virtual void menu_leave() {}
    // The directory principal of this connection, the capability that proves the room's owner when its signalling scope is
    // opened (NetworkGnsRoom::open). A secret: never copy it into a row, chat, profile or report. Empty when there is none.
    virtual std::string principal() const { return {}; }
};
} // namespace eawr::net
