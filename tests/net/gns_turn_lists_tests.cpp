#include "../support/check.hpp"
#include "turn_lists.hpp"

#include <chrono>
#include <iostream>
#include <string>

namespace {
using namespace eawr::net;
using namespace eawr::net::gns_detail;
using namespace std::chrono_literals;
void expect(bool value, const char* message) { eawr::testing::require(value, message); }

const std::string cf1 = "turn:turn.cloudflare.com:3478", cf2 = "turn:turn.cloudflare.com:443", own = "turn:203.0.113.9:3478";

std::string joined(const std::vector<TurnEntry>& entries) {
    std::string text;
    for (const auto& entry : entries) text += entry.uri + "|" + entry.username + "|" + entry.password + ";";
    return text;
}
} // namespace

int main() {
    // One credential pair for every server (the single-provider frame) expands to one entry per server.
    expect(joined(turn_entries(cf1 + "," + cf2, "user", "pass")) == cf1 + "|user|pass;" + cf2 + "|user|pass;",
        "A single credential pair repeats per server");

    // Two providers keep the service's order and their own credentials.
    const auto both = turn_entries(cf1 + "," + cf2 + "," + own, "cfuser,cfuser,ownuser", "cfpass,cfpass,ownpass");
    expect(joined(both) == cf1 + "|cfuser|cfpass;" + cf2 + "|cfuser|cfpass;" + own + "|ownuser|ownpass;", "Per-server credentials stay with their server");
    expect(joined(for_provider(both, GnsTurnProvider::automatic)) == own + "|ownuser|ownpass;" + cf1 + "|cfuser|cfpass;" + cf2 + "|cfuser|cfpass;",
        "Automatic keeps both providers, the operator's own server first (Cloudflare is the fallback for a dead server)");
    const auto only_cloudflare_frame = turn_entries(cf1 + "," + cf2, "u", "p");
    expect(joined(for_provider(only_cloudflare_frame, GnsTurnProvider::automatic)) == joined(only_cloudflare_frame), "A Cloudflare-only frame keeps its order");

    // Forcing a provider keeps only its servers, with their credentials.
    expect(joined(for_provider(both, GnsTurnProvider::own)) == own + "|ownuser|ownpass;", "own keeps the operator's server and its credential");
    expect(joined(for_provider(both, GnsTurnProvider::cloudflare)) == cf1 + "|cfuser|cfpass;" + cf2 + "|cfuser|cfpass;",
        "cloudflare keeps both Cloudflare entries");
    expect(for_provider(turn_entries(cf1, "u", "p"), GnsTurnProvider::own).empty(), "own without an own server leaves nothing");
    expect(for_provider(turn_entries(own, "u", "p"), GnsTurnProvider::cloudflare).empty(), "cloudflare without a Cloudflare server leaves nothing");

    // Lists that do not fit the servers are refused, not guessed.
    expect(turn_entries(cf1 + "," + cf2 + "," + own, "a,b", "p").empty(), "Two users for three servers");
    expect(turn_entries(cf1 + "," + own, "u", "p,,q").empty(), "An empty password entry");
    expect(turn_entries(cf1, "u,v", "p").empty(), "Two users for one server");

    // The lists GNS gets: one user and password per server.
    std::string users, passwords;
    gns_turn_lists({cf1 + "," + cf2 + "," + own, "cfuser,cfuser,ownuser", "cfpass,cfpass,ownpass", 600s}, users, passwords);
    expect(users == "cfuser,cfuser,ownuser" && passwords == "cfpass,cfpass,ownpass", "Per-server lists pass through");
    gns_turn_lists({cf1 + "," + cf2, "user", "pass", 600s}, users, passwords);
    expect(users == "user,user" && passwords == "pass,pass", "A single pair is repeated per server for GNS");

    // Host classification: the domain and its subdomains only, any case, optional query.
    expect(cloudflare_server("turn:TURN.Cloudflare.com:3478?transport=udp"), "Case and query are ignored");
    expect(cloudflare_server("turn:cloudflare.com:3478"), "The bare domain counts");
    expect(!cloudflare_server("turn:notcloudflare.com:3478") && !cloudflare_server("turn:cloudflare.com.example.org:3478"), "Look-alike hosts do not count");
    expect(!cloudflare_server("turn:198.51.100.7:3478") && !cloudflare_server("turn:"), "Numeric hosts and a short entry do not count");

    std::cout << "GNS TURN list contracts passed\n";
    return 0;
}
