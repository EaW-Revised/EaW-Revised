#include "../support/check.hpp"
#include "stun_list.hpp"

#include <algorithm>
#include <iostream>
#include <string>

namespace {
using namespace eawr::net::gns_detail;
void expect(bool value, const char* message) { eawr::testing::require(value, message); }
const std::string cloudflare{cloudflare_stun_uri};
} // namespace

int main() {
    // No TURN list: still the public Cloudflare STUN, so a game without relay credentials can learn its public address.
    expect(stun_list_for("") == cloudflare, "An empty TURN list gives Cloudflare STUN only");

    // Cloudflare's own TURN addresses (after the relay client resolved them) come after the public STUN; the 443 and 53 listeners are skipped.
    expect(stun_list_for("turn:141.101.90.1:3478,turn:141.101.90.1:443,turn:141.101.90.1:53")
            == cloudflare + ",stun:141.101.90.1:3478", "Only port 3478 entries become STUN servers");

    // The operator's coturn answers STUN on its TURN port and is listed after the public server.
    expect(stun_list_for("turn:203.0.113.9:3478") == cloudflare + ",stun:203.0.113.9:3478", "An own coturn entry is added");
    expect(stun_list_for("turn:203.0.113.9:3478?transport=udp") == cloudflare + ",stun:203.0.113.9:3478", "A transport query is dropped");
    expect(stun_list_for("turn:[2001:db8::9]:3478") == cloudflare + ",stun:[2001:db8::9]:3478", "A bracketed IPv6 address passes through");

    // Duplicates and anything GNS must not resolve itself are dropped.
    expect(stun_list_for("turn:203.0.113.9:3478,turn:203.0.113.9:3478,turn:203.0.113.9:443") == cloudflare + ",stun:203.0.113.9:3478", "Duplicates collapse");
    expect(stun_list_for("turn:turn.example.org:3478,stun:203.0.113.9:3478,turn::3478,garbage") == cloudflare, "Hostnames, non-turn entries and empty hosts are dropped");
    expect(stun_list_for("turn:1.1.1.1:3478") != cloudflare && stun_list_for("turn:1.1.1.1:3478").find("stun:162.159.207.0:3478") == 0, "The public server stays first");

    // The list never grows past eight servers (the adapter's own validation limit).
    std::string many;
    for (int i = 1; i <= 12; ++i) many += (i > 1 ? "," : "") + std::string("turn:10.0.0.") + std::to_string(i) + ":3478";
    const auto capped = stun_list_for(many);
    expect(std::count(capped.begin(), capped.end(), ',') == 7 && capped.size() <= 512, "At most eight servers, within the length limit");

    std::cout << "GNS STUN list contracts passed\n";
    return 0;
}
