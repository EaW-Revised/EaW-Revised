#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

// STUN servers for GNS server-reflexive candidates (P2P_STUN_ServerList).
//
// GNS reads only the relayed address from a TURN allocation, so without a STUN list it offers its peer private (host)
// addresses and the relay, nothing else: two players on different home networks cannot reach each other and the relay
// pair always wins. With a STUN server GNS learns each interface's public address and port and offers it too, which is
// what lets a hole punch through an ordinary (endpoint-independent) NAT work.
//
// The list is: Cloudflare's public STUN first (always reachable, no credentials), then every "turn:host:3478" entry of
// the TURN list the service handed out (coturn answers STUN on its TURN port; a Cloudflare TURN address ignores
// unauthenticated binding requests, which GNS treats as a timeout and moves on from). GNS asks the next server only when
// one does not answer, so the first working server decides. Numeric addresses only: GNS would resolve a hostname on its
// own thread, and the relay client already turned the TURN hostnames into addresses off the game thread.
namespace eawr::net::gns_detail {

// stun.cloudflare.com. The address is Cloudflare's published anycast STUN address; it is a literal so that nothing
// resolves a name on the game thread.
inline constexpr std::string_view cloudflare_stun_uri = "stun:162.159.207.0:3478";

inline std::string stun_list_for(std::string_view turn_uri_list) {
    std::string result(cloudflare_stun_uri);
    std::size_t servers = 1;
    while (!turn_uri_list.empty() && servers < 8) {
        const auto comma = turn_uri_list.find(',');
        auto item = turn_uri_list.substr(0, comma);
        turn_uri_list = comma == std::string_view::npos ? std::string_view{} : turn_uri_list.substr(comma + 1);
        if (!item.starts_with("turn:")) continue;
        item.remove_prefix(5);
        item = item.substr(0, item.find('?')); // "turn:host:3478?transport=udp"
        // Only the standard STUN/TURN port: coturn's other listeners and Cloudflare's 443/53 do not answer STUN.
        if (!item.ends_with(":3478")) continue;
        // "host:3478" with host a numeric IPv4 address or a bracketed IPv6 address; anything else is not for GNS.
        const auto host = item.substr(0, item.size() - 5);
        if (host.empty()) continue;
        const bool bracketed = host.front() == '[' && host.back() == ']';
        if (!bracketed && !std::all_of(host.begin(), host.end(), [](char ch) { return (ch >= '0' && ch <= '9') || ch == '.'; })) continue;
        const std::string entry = "stun:" + std::string(item);
        if (result.find(entry) != std::string::npos) continue;
        result += "," + entry;
        ++servers;
    }
    return result;
}

} // namespace eawr::net::gns_detail
