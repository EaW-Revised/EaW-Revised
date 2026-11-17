#pragma once

#include "eawr/core/result.hpp"
#include "eawr/net/tcp_transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace eawr::net {
// Delivery only: every native reliable envelope has a monotonically increasing
// ordinal. Small envelopes also travel in a bounded, independently sent UDP
// history. Merge both paths BEFORE the existing frame/control decoder, keeping
// its exact ordered-stream contract, including barriers and bulk fragments.
class GnsPacketDelivery {
public:
    using Bytes = std::vector<std::uint8_t>;
    using Clock = std::chrono::steady_clock;
    // Leave room for the provider's encryption/segment headers and its 100-byte
    // stats reserve, rather than fragmenting the redundant message itself.
    static constexpr std::size_t datagram_bytes = 1000, history_packets = 24;
    // At most 10,000 bytes/s of duplicate payload when history is full. This
    // leaves room for reliable originals at the 64 KiB/s WAN pacing floor.
    static constexpr std::chrono::milliseconds repeat_interval{100}, retention{750};
    explicit GnsPacketDelivery(std::size_t bytes, std::size_t packets) : byte_limit_(bytes), packet_limit_(packets) {}

    Bytes encode(std::span<const std::uint8_t> envelope) const {
        Bytes packet;
        for (unsigned i = 0; i < 8; ++i) packet.push_back(static_cast<std::uint8_t>(sent_ >> (8 * i)));
        packet.insert(packet.end(), envelope.begin(), envelope.end());
        return packet;
    }
    // Commit only after the native reliable send succeeds. Retrying a full
    // native queue must neither skip an ordinal nor publish an unsent copy.
    void sent(Bytes packet, Clock::time_point now, bool duplicate) {
        ++sent_;
        if (!duplicate || packet.size() + 2 > datagram_bytes) return;
        history_bytes_ += packet.size() + 2;
        history_.push_back({std::move(packet), now});
        while (history_bytes_ > datagram_bytes || history_.size() > history_packets) pop_history();
    }
    Bytes redundancy(Clock::time_point now) {
        while (!history_.empty() && now - history_.front().when >= retention) pop_history();
        if (history_.empty() || (last_repeat_ && now - *last_repeat_ < repeat_interval)) return {};
        last_repeat_ = now;
        Bytes packet;
        packet.reserve(history_bytes_);
        for (const auto& entry : history_) {
            const auto size = entry.packet.size();
            packet.push_back(static_cast<std::uint8_t>(size));
            packet.push_back(static_cast<std::uint8_t>(size >> 8));
            packet.insert(packet.end(), entry.packet.begin(), entry.packet.end());
        }
        return packet;
    }
    core::Result<std::vector<Bytes>> receive(std::span<const std::uint8_t> packet, bool reliable) {
        std::vector<Bytes> ready;
        if (reliable) {
            // A patch can meet a peer using the previous, unwrapped hello.
            // Only its first reliable message gets this escape: the hello
            // decoder then reports the typed protocol/build mismatch. A valid
            // ordinal zero always starts with zero, never the hello kind.
            if (!reliable_seen_ && next_ == 0 && packet.size() >= 5
                && packet.front() == static_cast<std::uint8_t>(TcpMessage::hello)
                && packet[1] == 'E' && packet[2] == 'A' && packet[3] == 'W' && packet[4] == 'R') {
                reliable_seen_ = true;
                ready.emplace_back(packet.begin(), packet.end());
                return core::Result<std::vector<Bytes>>::success(std::move(ready));
            }
            reliable_seen_ = true;
            if (!accept(packet, true, ready)) return malformed();
        } else {
            if (packet.empty() || packet.size() > datagram_bytes) return malformed();
            std::size_t count{};
            while (!packet.empty()) {
                if (packet.size() < 2 || ++count > history_packets) return malformed();
                const auto size = static_cast<std::size_t>(packet[0]) | (static_cast<std::size_t>(packet[1]) << 8);
                packet = packet.subspan(2);
                if (size > packet.size() || !accept(packet.first(size), false, ready)) return malformed();
                packet = packet.subspan(size);
            }
        }
        return core::Result<std::vector<Bytes>>::success(std::move(ready));
    }
    void reset() {
        sent_ = next_ = 0; reliable_seen_ = false; pending_.clear(); pending_bytes_ = 0;
        history_.clear(); history_bytes_ = 0; last_repeat_.reset();
    }
private:
    struct History { Bytes packet; Clock::time_point when; };
    std::size_t byte_limit_{}, packet_limit_{}, pending_bytes_{}, history_bytes_{};
    std::uint64_t sent_{}, next_{};
    bool reliable_seen_{};
    std::map<std::uint64_t, Bytes> pending_;
    std::deque<History> history_;
    std::optional<Clock::time_point> last_repeat_;
    void pop_history() { history_bytes_ -= history_.front().packet.size() + 2; history_.pop_front(); }
    static core::Result<std::vector<Bytes>> malformed() {
        core::Diagnostic error; error.code = "EAWR-NET-1518"; error.message = "Invalid GNS delivery envelope";
        return core::Result<std::vector<Bytes>>::failure(std::move(error));
    }
    bool accept(std::span<const std::uint8_t> packet, bool reliable, std::vector<Bytes>& ready) {
        if (packet.size() < 9) return false;
        std::uint64_t ordinal{};
        for (unsigned i = 0; i < 8; ++i) ordinal |= static_cast<std::uint64_t>(packet[i]) << (8 * i);
        const auto body = packet.subspan(8);
        if (ordinal < next_) return true; // late reliable original or redundant copy
        if (ordinal == next_) {
            ready.emplace_back(body.begin(), body.end());
            ++next_;
            for (;;) {
                const auto found = pending_.find(next_);
                if (found == pending_.end()) break;
                pending_bytes_ -= found->second.size();
                ready.push_back(std::move(found->second)); pending_.erase(found); ++next_;
            }
            return true;
        }
        // Reliable originals are natively ordered, so a future reliable ordinal
        // proves a sender gap. Unreliable lookahead is optional: discard it at
        // capacity and let the reliable stream recover without disconnecting.
        if (reliable) return false;
        const auto found = pending_.find(ordinal);
        if (found != pending_.end()) return std::equal(body.begin(), body.end(), found->second.begin(), found->second.end());
        if (ordinal - next_ > packet_limit_ || pending_.size() >= packet_limit_
            || body.size() > byte_limit_ - pending_bytes_) return true;
        pending_.emplace(ordinal, Bytes(body.begin(), body.end())); pending_bytes_ += body.size();
        return true;
    }
};
} // namespace eawr::net
