#include "../support/check.hpp"
#include "../../src/net/gns_link_stats.hpp"
#include "../../src/net/gns_link_watchdog.hpp"

#include <exception>
#include <iostream>
#include <optional>

namespace {
using namespace std::chrono_literals;
using eawr::net::GnsLinkStatsSampler;
void expect(bool value, const char* message) {
    if (!value) eawr::testing::require(value, message);
}
void cadence() {
    GnsLinkStatsSampler sampler;
    const GnsLinkStatsSampler::Clock::time_point start{};
    expect(!sampler.cached() && sampler.due(start), "Fresh native stats are unknown");
    sampler.observe(start, -1);
    expect(!sampler.cached() && !sampler.due(start + 999ms), "Invalid estimate consumes its cadence slot");
    sampler.observe(start + 1s, std::nullopt);
    expect(!sampler.cached(), "Failed native query cannot fabricate stats");
    sampler.observe(start + 2s, 0);
    const auto first = sampler.cached();
    expect(first && first->sample_count == 1 && first->last_rtt == 0us
        && first->smoothed_rtt == 0us && first->jitter == 0us, "Zero is a valid first native estimate");
    sampler.observe(start + 2999ms, 80);
    expect(sampler.cached()->sample_count == 1, "Poll frequency cannot increase the observation cadence");
    sampler.observe(start + 3s, 0);
    expect(sampler.cached()->sample_count == 2, "Equal cadence observations still count");
    for (unsigned i = 0; i < 100; ++i) static_cast<void>(sampler.cached());
    expect(sampler.cached()->sample_count == 2 && sampler.due(start + 4s), "Getters cannot move the cadence clock");
    sampler.observe(start + 30s, 0);
    expect(sampler.cached()->sample_count == 3, "A polling gap cannot fabricate missed observations");
    sampler.reset();
    expect(!sampler.cached() && sampler.due(start + 30s), "Retry discards both stats and cadence state");
    sampler.observe(start + 30s, 8);
    expect(sampler.cached()->sample_count == 1 && sampler.cached()->smoothed_rtt == 8ms,
        "First observation after reset initializes the new link");
}
void smoothing() {
    GnsLinkStatsSampler sampler;
    const GnsLinkStatsSampler::Clock::time_point start{};
    sampler.observe(start, 8);
    sampler.observe(start + 1s, 0);
    auto stats = sampler.cached();
    expect(stats && stats->last_rtt == 0us && stats->smoothed_rtt == 7ms
        && stats->jitter == 2ms && stats->sample_count == 2, "Native stats use the common RTT and jitter updates");
    sampler.observe(start + 2s, 0);
    stats = sampler.cached();
    expect(stats->smoothed_rtt == 6125us && stats->jitter == 1500us && stats->sample_count == 3,
        "Repeated estimate contributes zero RTT delta and decays jitter");
    sampler.observe(start + 3s, -1);
    expect(sampler.cached()->sample_count == 3 && sampler.cached()->smoothed_rtt == 6125us,
        "Unavailable native estimate preserves cached observations");
}
void long_pause_keeps_transport_live() {
    eawr::net::GnsLinkWatchdog watchdog;
    const eawr::net::GnsLinkWatchdog::Clock::time_point start{};
    for (unsigned ms = 0; ms <= 600'000; ms += 250) {
        const auto now = start + std::chrono::milliseconds(ms);
        watchdog.received_reliable(now); // keepalives/empty seals, no world tick required
        expect(watchdog.observe(now, 0, 100) < 15s, "ten-minute pause with live traffic cannot trigger NET-1507");
    }
    expect(watchdog.observe(start + 615s, 0, 100) >= 15s,
        "true ACK silence while the world remains paused still reaches the watchdog deadline");
}

void link_liveness() {
    eawr::net::GnsLinkWatchdog watchdog;
    const eawr::net::GnsLinkWatchdog::Clock::time_point start{};
    // Fixed 250 ms samples can see the same or growing window on a healthy
    // continuously sending link. Validated reliable deliveries disprove silence.
    for (int sample = 0; sample < 160; ++sample) {
        const auto now = start + sample * 250ms;
        watchdog.received_reliable(now);
        expect(watchdog.observe(now, 10, 100 + std::max(0, sample - 80)) < 15s,
            "Reliable inbound traffic keeps a steady or growing window alive past 15 seconds");
    }
    watchdog.reset();
    expect(watchdog.observe(start, 0, 100) == 0ms, "First in-flight bytes arm the detector");
    expect(watchdog.observe(start + 14999ms, 0, 100) < 15s, "Silent path keeps its complete grace");
    // NACKs and retransmits move the same bytes between these counters. They
    // must not restart the clock, even when unacked reaches zero temporarily.
    expect(watchdog.observe(start + 15s, 100, 0) == 15s, "Retransmit scheduling cannot manufacture an ACK");
    expect(watchdog.observe(start + 16s, 0, 100) == 16s, "Retransmitting cannot extend a dead link's grace");
    expect(watchdog.observe(start + 17s, 0, 80) == 0ms, "A shrinking total window proves send progress");
    expect(watchdog.observe(start + 18s, 0, 80) == 0ms, "Remaining bytes start a fresh grace");
    expect(watchdog.observe(start + 33s, 0, 80) == 15s, "A new stall is still bounded");
    expect(watchdog.observe(start + 34s, 0, 0) == 0ms, "Drained reliable stream clears silence");
    expect(watchdog.observe(start + 80s, 0, 0) == 0ms, "Idle link cannot time out without reliable bytes");
    watchdog.reset();
    expect(watchdog.observe(start + 100s, 0, 80) == 0ms, "A replacement link discards the old timer");
    watchdog.received_reliable(start + 114s);
    expect(watchdog.observe(start + 115s, 0, 80) == 1s, "Reliable receive restarts an armed silence clock");
    expect(watchdog.observe(start + 129s, 0, 80) == 15s, "Silence after the final receive is bounded");
}
// #4418 regression: the shipped predicate declares a replenished window dead
// despite reliable traffic arriving throughout its entire fifteen-second grace.
void legacy_live_window_regression() {
    using Clock = eawr::net::GnsLinkWatchdog::Clock;
    const Clock::time_point start{};
    eawr::net::GnsLinkWatchdog watchdog;
    std::optional<Clock::time_point> unacked_since;
    std::int32_t last_unacked{};
    std::optional<std::chrono::milliseconds> legacy_failure;
    for (int elapsed_ms = 0; elapsed_ms <= 20000; ++elapsed_ms) {
        const auto now = start + std::chrono::milliseconds(elapsed_ms);
        if (elapsed_ms % 33 == 0) watchdog.received_reliable(now);
        if (elapsed_ms % 250 != 0) continue;
        constexpr std::int32_t unacked = 100;
        // Original predicate: new sends can replace ACKed bytes between samples.
        if (unacked <= 0 || unacked < last_unacked) unacked_since.reset();
        else if (!unacked_since) unacked_since = now;
        last_unacked = unacked;
        if (unacked_since && now - *unacked_since >= 15s && !legacy_failure)
            legacy_failure = std::chrono::milliseconds(elapsed_ms);
        expect(watchdog.observe(now, 0, unacked) < 15s,
            "#4418: reliable delivery every 33 ms keeps a constant nonzero window alive");
    }
    expect(legacy_failure == 15s, "#4418: the original predicate falsely fires at 15 seconds on the same live samples");
    std::cout << "#4418 regression: legacy rule falsely times out at 15000 ms; corrected rule stays live for 20000 ms\n";
}
}
// ROUTE label: which relay provider carries a relayed pair, from GNS's TURN reply line and the issued credentials.
void route_label() {
    using namespace eawr::net;
    expect(turn_server_from_spew("ICE: TURN server 203.0.113.9:3478 accepted a request (message 0x108)\n") == "203.0.113.9:3478",
        "The accepting TURN server is read from the GNS line");
    expect(turn_server_from_spew("ICE: TURN server  accepted a request").empty() && turn_server_from_spew("ICE: connected").empty()
            && turn_server_from_spew("TURN server 1.2.3.4:3478 sent 401").empty(), "Other lines name no server");
    const std::string all = "turn:203.0.113.9:3478,turn:141.101.90.1:3478,turn:141.101.90.1:443";
    const std::optional<std::string> own{"turn:203.0.113.9:3478"}, none_own{std::string{}};
    expect(classify_relay_provider(GnsRoute::direct, "203.0.113.9:3478", own, all) == GnsRelayProvider::none, "A direct pair has no relay provider");
    expect(classify_relay_provider(GnsRoute::relayed, "203.0.113.9:3478", own, all) == GnsRelayProvider::own, "The operator's server is own");
    expect(classify_relay_provider(GnsRoute::relayed, "141.101.90.1:3478", own, all) == GnsRelayProvider::cloudflare, "Another listed server is Cloudflare");
    expect(classify_relay_provider(GnsRoute::relayed, "141.101.90.1:3478", none_own, all) == GnsRelayProvider::cloudflare, "No own entries: Cloudflare");
    expect(classify_relay_provider(GnsRoute::relayed, "", own, all) == GnsRelayProvider::unknown, "No allocation line yet: unknown");
    expect(classify_relay_provider(GnsRoute::relayed, "203.0.113.9:3478,141.101.90.1:3478", own, all) == GnsRelayProvider::unknown,
        "Different allocations on different providers: a mixed pair is unknown");
    expect(classify_relay_provider(GnsRoute::relayed, "141.101.90.1:3478,141.101.90.1:443", own, all) == GnsRelayProvider::cloudflare,
        "Several Cloudflare servers are still Cloudflare");
    expect(classify_relay_provider(GnsRoute::relayed, "198.51.100.5:3478", own, all) == GnsRelayProvider::unknown, "A server that is not listed: unknown");
    expect(classify_relay_provider(GnsRoute::relayed, "203.0.113.9:3478", std::nullopt, all) == GnsRelayProvider::unknown, "An endpoint that does not classify: unknown");
    expect(classify_relay_provider(GnsRoute::relayed, "203.0.113.9:3478", std::optional<std::string>{"turn:203.0.113.9:3478?transport=udp"}, all)
        == GnsRelayProvider::own, "A transport query on the entry is ignored");
    expect(route_text(GnsRoute::direct, GnsRelayProvider::none) == "direct" && route_text(GnsRoute::relayed, GnsRelayProvider::own) == "relay (own)"
            && route_text(GnsRoute::relayed, GnsRelayProvider::cloudflare) == "relay (cloudflare)"
            && route_text(GnsRoute::relayed, GnsRelayProvider::unknown) == "relay" && route_text(GnsRoute::unknown, GnsRelayProvider::none) == "unknown",
        "The route text for the log and the overlay");
}
// The loss hint: sustained loss on a relayed pair only, with hysteresis; it costs nothing on a healthy pair.
void loss_hint() {
    using eawr::net::GnsLossHint;
    using Clock = GnsLossHint::Clock;
    const auto start = Clock::now();
    GnsLossHint hint;
    for (int s = 0; s < 120; ++s) expect(!hint.observe(start + std::chrono::seconds(s), true, 2.0) && !hint.active(), "A healthy relayed pair never raises it");
    GnsLossHint high;
    for (int s = 0; s < 20; ++s) expect(!high.observe(start + std::chrono::seconds(s), true, 18.0), "Under 20 s of high loss nothing is raised");
    expect(high.observe(start + std::chrono::seconds(20), true, 18.0) && high.active() && high.peak_pct() == 18.0, "20 s of >= 15% loss raises it once");
    expect(!high.observe(start + std::chrono::seconds(21), true, 30.0) && high.peak_pct() == 30.0, "The peak follows, no second change");
    expect(!high.observe(start + std::chrono::seconds(22), true, 9.0) && high.active(), "Between 5% and 15% it stays raised (hysteresis)");
    for (int s = 23; s < 33; ++s) expect(!high.observe(start + std::chrono::seconds(s), true, 1.0) && high.active(), "Calm below 5% clears only after 10 s");
    expect(high.observe(start + std::chrono::seconds(33), true, 1.0) && !high.active(), "Cleared after 10 s below 5%");
    GnsLossHint flap;
    for (int s = 0; s < 60; ++s) flap.observe(start + std::chrono::seconds(s), true, s % 10 == 9 ? 2.0 : 40.0);
    expect(!flap.active(), "Loss that dips every 10 s never reaches 20 s in a row");
    GnsLossHint direct;
    for (int s = 0; s < 60; ++s) expect(!direct.observe(start + std::chrono::seconds(s), false, 50.0) && !direct.active(), "A direct pair is not observed here");
    GnsLossHint unknown;
    for (int s = 0; s < 60; ++s) expect(!unknown.observe(start + std::chrono::seconds(s), true, std::nullopt), "Unmeasured loss raises nothing");
    GnsLossHint left;
    for (int s = 0; s <= 20; ++s) left.observe(start + std::chrono::seconds(s), true, 20.0);
    expect(left.active() && left.observe(start + std::chrono::seconds(21), false, 20.0) && !left.active(), "Leaving the relay clears it");
}
int main() {
    try { loss_hint(); route_label(); cadence(); smoothing(); long_pause_keeps_transport_live(); link_liveness(); legacy_live_window_regression();
        std::cout << "GNS LinkStats contracts passed\n"; return 0; }
    catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
