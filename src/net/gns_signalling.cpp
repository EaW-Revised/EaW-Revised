#include "eawr/net/gns_signalling.hpp"

#include <array>
#include <deque>
#include <map>
#include <mutex>
#include <utility>

namespace eawr::net {
namespace {
core::Diagnostic error(std::string code, std::string message) {
    core::Diagnostic result;
    result.code = std::move(code); result.message = std::move(message);
    return result;
}
bool identity_valid(const std::string& identity) {
    if (identity.empty() || identity.size() > gns_identity_max_bytes) return false;
    for (const unsigned char byte : identity)
        if (byte < 33 || byte > 126) return false;
    return true;
}
struct Channel {
    std::mutex mutex;
    std::array<GnsRendezvous, 2> peers;
    std::array<std::deque<GnsSignal>, 2> inbox;
    std::array<std::size_t, 2> used{};
    InProcessGnsIntroduction::Now now;
    InProcessGnsIntroduction::Clock::time_point expires;
    std::size_t max_bytes{}, max_signals{};
    bool joined{};
};
class Endpoint final : public GnsSignallingEndpoint {
public:
    Endpoint(std::shared_ptr<Channel> channel, std::size_t index) : channel_(std::move(channel)), index_(index) {}
    GnsRendezvous rendezvous() const override {
        std::lock_guard lock(channel_->mutex);
        return channel_->peers[index_];
    }
    core::Result<void> send(std::span<const std::uint8_t> signal) override {
        std::lock_guard lock(channel_->mutex);
        if (!channel_->joined && channel_->now() >= channel_->expires)
            return core::Result<void>::failure(error("EAWR-NET-1510", "Introduction session expired"));
        if (!channel_->joined)
            return core::Result<void>::failure(error("EAWR-NET-1511", "Introduction has no joined peer"));
        const auto target = 1 - index_;
        if (signal.empty() || signal.size() > 64 * 1024)
            return core::Result<void>::failure(error("EAWR-NET-1512", "Invalid GNS signalling payload size"));
        if (channel_->inbox[target].size() >= channel_->max_signals
            || signal.size() > channel_->max_bytes - channel_->used[target])
            return core::Result<void>::failure(error("EAWR-NET-1513", "Introduction signal queue is full"));
        channel_->inbox[target].push_back({channel_->peers[index_].local_identity, {signal.begin(), signal.end()}});
        channel_->used[target] += signal.size();
        return core::Result<void>::success();
    }
    core::Result<std::optional<GnsSignal>> poll() override {
        std::lock_guard lock(channel_->mutex);
        if (!channel_->joined && channel_->now() >= channel_->expires)
            return core::Result<std::optional<GnsSignal>>::failure(error("EAWR-NET-1510", "Introduction session expired"));
        if (channel_->inbox[index_].empty()) return core::Result<std::optional<GnsSignal>>::success({});
        auto signal = std::move(channel_->inbox[index_].front()); channel_->inbox[index_].pop_front();
        channel_->used[index_] -= signal.bytes.size();
        return core::Result<std::optional<GnsSignal>>::success(std::move(signal));
    }
private:
    std::shared_ptr<Channel> channel_;
    std::size_t index_{};
};
}
struct InProcessGnsIntroduction::Impl {
    std::mutex mutex;
    Now now;
    std::size_t max_bytes{}, max_signals{};
    std::uint64_t sequence{};
    std::map<std::string, std::shared_ptr<Channel>> sessions;
    Impl(Now clock, std::size_t bytes, std::size_t signals) : now(clock), max_bytes(bytes), max_signals(signals) {}
    void prune() {
        const auto time = now();
        for (auto entry = sessions.begin(); entry != sessions.end();)
            if (time >= entry->second->expires) entry = sessions.erase(entry); else ++entry;
    }
};
InProcessGnsIntroduction::InProcessGnsIntroduction(Now now, std::size_t bytes, std::size_t signals)
    : impl_(std::make_unique<Impl>(now, bytes, signals)) {}
InProcessGnsIntroduction::~InProcessGnsIntroduction() = default;
core::Result<GnsIntroduction> InProcessGnsIntroduction::publish(std::string identity, std::chrono::milliseconds lifetime) {
    std::lock_guard service_lock(impl_->mutex);
    if (!impl_->now || !identity_valid(identity) || lifetime.count() <= 0 || lifetime > std::chrono::hours(24)
        || impl_->max_bytes == 0 || impl_->max_signals == 0)
        return core::Result<GnsIntroduction>::failure(error("EAWR-NET-1514", "Invalid introduction options"));
    impl_->prune();
    if (impl_->sessions.size() >= 64)
        return core::Result<GnsIntroduction>::failure(error("EAWR-NET-1513", "Introduction session capacity exceeded"));
    const auto code = "test-" + std::to_string(++impl_->sequence);
    auto channel = std::make_shared<Channel>();
    channel->now = impl_->now; channel->expires = impl_->now() + lifetime;
    channel->max_bytes = impl_->max_bytes; channel->max_signals = impl_->max_signals;
    channel->peers[0] = {code, std::move(identity), {}};
    impl_->sessions.emplace(code, channel);
    return core::Result<GnsIntroduction>::success({code, std::make_shared<Endpoint>(std::move(channel), 0)});
}
core::Result<std::shared_ptr<GnsSignallingEndpoint>> InProcessGnsIntroduction::join(std::string code, std::string identity) {
    std::lock_guard service_lock(impl_->mutex);
    if (!impl_->now || !identity_valid(identity))
        return core::Result<std::shared_ptr<GnsSignallingEndpoint>>::failure(error("EAWR-NET-1514", "Invalid introduction identity"));
    const auto found = impl_->sessions.find(code);
    if (found == impl_->sessions.end())
        return core::Result<std::shared_ptr<GnsSignallingEndpoint>>::failure(error("EAWR-NET-1515", "Unknown join code"));
    auto channel = found->second;
    std::lock_guard lock(channel->mutex);
    if (impl_->now() >= channel->expires)
        return core::Result<std::shared_ptr<GnsSignallingEndpoint>>::failure(error("EAWR-NET-1510", "Join code expired"));
    if (channel->joined || identity == channel->peers[0].local_identity)
        return core::Result<std::shared_ptr<GnsSignallingEndpoint>>::failure(error("EAWR-NET-1516", "Join code occupied or identity duplicated"));
    channel->peers[1] = {code, identity, channel->peers[0].local_identity};
    channel->peers[0].remote_identity = std::move(identity);
    channel->joined = true;
    return core::Result<std::shared_ptr<GnsSignallingEndpoint>>::success(std::make_shared<Endpoint>(std::move(channel), 1));
}
} // namespace eawr::net
