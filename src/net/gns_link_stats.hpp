#pragma once

#include "eawr/net/transport.hpp"

#include <limits>

namespace eawr::net {
// Local cadence observations of the SDK's integer-millisecond estimate. These
// are not native probe counts, and getters never advance the sampler.
class GnsLinkStatsSampler {
public:
    using Clock = std::chrono::steady_clock;
    bool due(Clock::time_point now) const {
        return !attempted_ || now - *attempted_ >= std::chrono::seconds(1);
    }
    void observe(Clock::time_point now, std::optional<int> ping_ms) {
        if (!due(now)) return;
        attempted_ = now;
        if (!ping_ms || *ping_ms < 0) return;
        const auto sample = std::chrono::microseconds(static_cast<std::int64_t>(*ping_ms) * 1000);
        if (stats_.sample_count == 0) stats_.smoothed_rtt = sample;
        else {
            const auto delta = sample > stats_.last_rtt ? sample - stats_.last_rtt : stats_.last_rtt - sample;
            stats_.jitter += (delta - stats_.jitter) / 4;
            stats_.smoothed_rtt += (sample - stats_.smoothed_rtt) / 8;
        }
        stats_.last_rtt = sample;
        if (stats_.sample_count != std::numeric_limits<std::uint64_t>::max()) ++stats_.sample_count;
    }
    std::optional<LinkStats> cached() const {
        if (stats_.sample_count == 0) return {};
        return stats_;
    }
    void reset() { attempted_.reset(); stats_ = {}; }
private:
    std::optional<Clock::time_point> attempted_;
    LinkStats stats_;
};
} // namespace eawr::net
