#pragma once

#include "eawr/net/gns_signalling.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Per-server TURN credentials. The introduction service may issue several providers in one frame: `uri` is a comma list, and
// `username` / `password` are either one value for every server or one value per server. GNS wants one user and password per
// listed server (P2P_TURN_ServerList, UserList, PassList). Header-only and free of GNS so the tests can reach it.
namespace eawr::net::gns_detail {
struct TurnEntry {
    std::string uri, username, password;
};

inline std::vector<std::string> split_commas(const std::string& text) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    for (;;) {
        const auto end = text.find(',', begin);
        parts.push_back(text.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) return parts;
        begin = end + 1;
    }
}

// One value for every listed server, or exactly one per server; never empty, never a count in between.
inline bool per_server_values(const std::string& text, std::size_t servers, std::vector<std::string>& out) {
    out = split_commas(text);
    if (out.size() == 1) out.resize(servers, out.front());
    return out.size() == servers && std::none_of(out.begin(), out.end(), [](const std::string& value) { return value.empty(); });
}

// The servers of credentials, each with its own user and password. Empty when the lists do not fit the servers.
inline std::vector<TurnEntry> turn_entries(const std::string& uri, const std::string& username, const std::string& password) {
    const auto uris = split_commas(uri);
    std::vector<std::string> users, passwords;
    if (!per_server_values(username, uris.size(), users) || !per_server_values(password, uris.size(), passwords)) return {};
    std::vector<TurnEntry> entries;
    for (std::size_t i = 0; i < uris.size(); ++i) entries.push_back({uris[i], users[i], passwords[i]});
    return entries;
}

// Cloudflare's relay hosts end in cloudflare.com; any other host is the operator's own coturn. Applies to the entry as the
// service sent it: a hostname is judged before the client turns it into a numeric address.
inline bool cloudflare_server(const std::string& uri) {
    if (uri.size() <= 5) return false;
    auto host = uri.substr(5);
    const auto query = host.find('?');
    if (query != std::string::npos) host.erase(query);
    const auto colon = host.rfind(':');
    if (colon != std::string::npos) host.erase(colon);
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    constexpr std::string_view domain = "cloudflare.com";
    return host == domain || (host.size() > domain.size() && std::string_view(host).ends_with(domain)
        && host[host.size() - domain.size() - 1] == '.');
}

// The entries `provider` allows. `automatic` keeps all of them with the operator's own servers first (the order inside each
// provider is the service's); `own` / `cloudflare` keep only that provider's entries.
inline std::vector<TurnEntry> for_provider(std::vector<TurnEntry> entries, GnsTurnProvider provider) {
    if (provider == GnsTurnProvider::automatic) {
        std::stable_partition(entries.begin(), entries.end(), [](const TurnEntry& entry) { return !cloudflare_server(entry.uri); });
        return entries;
    }
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const TurnEntry& entry) {
        return cloudflare_server(entry.uri) != (provider == GnsTurnProvider::cloudflare);
    }), entries.end());
    return entries;
}

// The GNS user and password lists for credentials whose lists fit their servers (turn_invalid_check says so).
inline void gns_turn_lists(const GnsTurnCredentials& turn, std::string& users, std::string& passwords) {
    users.clear(); passwords.clear();
    for (const auto& entry : turn_entries(turn.uri, turn.username, turn.password)) {
        if (!users.empty()) { users += ','; passwords += ','; }
        users += entry.username; passwords += entry.password;
    }
}
} // namespace eawr::net::gns_detail
