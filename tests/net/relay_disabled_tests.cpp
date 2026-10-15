#include "eawr/net/network_introduction.hpp"

#include <iostream>

int main() {
#if EAWR_HAS_RELAY
    if (!eawr::net::NetworkGnsIntroduction::available() || !eawr::net::NetworkGnsRoom::available()) return 1;
#else
    using eawr::net::NetworkGnsIntroduction;
    if (NetworkGnsIntroduction::available()) return 1;
    NetworkGnsIntroduction service({});
    const auto publish = service.publish("host", std::chrono::seconds(1));
    const auto join = service.join("code", "peer");
    eawr::net::NetworkOnlineLobby lobby({});
    const auto rows = lobby.list(); const auto rooms = lobby.publish({}); const auto seat = lobby.join("room", "password", "name");
    if (rows || rooms || seat || lobby.leave() || lobby.send_chat("line") || lobby.chat(0) || lobby.heartbeat({}, {}) || lobby.remove({}) || lobby.kick({}, 2)) return 1;
    if (rows.error().code != "EAWR-NET-1519" || rooms.error().code != "EAWR-NET-1519" || seat.error().code != "EAWR-NET-1519") return 1;
    // The N-peer room client is disabled the same way: no factory connects, and every name stays linkable.
    if (eawr::net::NetworkGnsRoom::available()) return 1;
    const auto opened = eawr::net::NetworkGnsRoom::open({}, {}, "principal", "host", true);
    const auto attached = eawr::net::NetworkGnsRoom::attach_alias({}, "AB12", "peer");
    if (opened || attached || opened.error().code != "EAWR-NET-1519" || attached.error().code != "EAWR-NET-1519") return 1;
    const auto health = NetworkGnsIntroduction::health({});
    if (publish || join || health) return 1;
    if (publish.error().code != "EAWR-NET-1519" || join.error().code != "EAWR-NET-1519"
        || health.error().code != "EAWR-NET-1519") return 1;
#endif
    std::cout << "Relay availability and disabled definitions link correctly\n";
    return 0;
}
