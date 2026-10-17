#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace eawr::presentation::space {

// One presentation frame's immutable snapshot reads. The admitted frame already
// owns its previous/latest snapshots; older event records retain successful
// history reads until this cache is destroyed. Misses remain retryable, and no
// result (including a same-tick replacement) survives into another frame.
// Storage is O(k) shared owners for k distinct successful historical tick
// requests this frame, with O(k) history lookup; tick values never size
// storage. The admitted pair uses constant-time reads and no cache allocation.
template<class Snapshot>
class EventSnapshotCache final {
public:
    EventSnapshotCache(const Snapshot& previous, const Snapshot& latest) noexcept
        : previous_(&previous), latest_(&latest) {}

    template<class Fetch>
    [[nodiscard]] const Snapshot* get(const std::uint64_t tick, const Fetch& fetch) {
        if (latest_->completed_tick() == tick) return latest_;
        if (previous_->completed_tick() == tick) return previous_;
        for (const auto& entry : history_) {
            if (entry.first == tick) return entry.second.get();
        }
        auto snapshot = fetch(tick);
        if (!snapshot) return nullptr;
        const auto* result = snapshot.get();
        history_.emplace_back(tick, std::move(snapshot));
        return result;
    }

private:
    const Snapshot* previous_;
    const Snapshot* latest_;
    std::vector<std::pair<std::uint64_t, std::shared_ptr<const Snapshot>>> history_;
};

} // namespace eawr::presentation::space
