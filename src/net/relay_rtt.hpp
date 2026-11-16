#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace eawr::net::relay {
// The decision rule on its own (the answered samples' RTTs): fewer than two answers -> no RTT, else the median.
std::optional<std::chrono::microseconds> rtt_from_answers(std::vector<std::chrono::microseconds> answers);
// Round-trip time to a UDP endpoint that answers STUN (RttProbe, relay_https.hpp, is the injectable form of this call).
// Three STUN binding requests (10 ms apart) from one ephemeral socket; the median of the answers that arrived within `cap`
// of its own request (a round trip above `cap` is no answer), and nothing unless at least two of the three were answered: a server that leaves two or more of its
// three probes unanswered counts as unreachable for the order (three probes cannot tell 0% from 25% loss, so this is a coarse
// demotion, not a loss estimate). Blocks for at most `cap` plus 20 ms (the last request leaves 20 ms after the first) and checks `stop`.
std::optional<std::chrono::microseconds> stun_rtt(const std::string& ipv4, std::uint16_t port, std::chrono::milliseconds cap,
    std::stop_token stop = {});
} // namespace eawr::net::relay
