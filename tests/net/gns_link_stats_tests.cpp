#include "../../src/net/gns_link_stats.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;
using eawr::net::GnsLinkStatsSampler;
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
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
}
int main() {
    try { cadence(); smoothing(); std::cout << "GNS LinkStats contracts passed\n"; return 0; }
    catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
