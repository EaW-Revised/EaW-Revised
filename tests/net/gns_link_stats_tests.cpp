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
int main() {
    try { cadence(); smoothing(); link_liveness(); legacy_live_window_regression();
        std::cout << "GNS LinkStats contracts passed\n"; return 0; }
    catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
