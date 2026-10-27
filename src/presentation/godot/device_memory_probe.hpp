#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace eawr::presentation::godot_backend::detail {

struct DeviceMemory final {
    std::uint64_t bytes{};
    std::string source;
};

// One process-wide sample. Pending is distinct from a completed, unsupported probe:
// callers must not disable the adaptive budget while the render thread is sampling.
class DeviceMemoryProbe final {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto pending_warning_delay = std::chrono::seconds{1};

    [[nodiscard]] bool begin(const Clock::time_point now = Clock::now()) {
        std::lock_guard lock(mutex_);
        if (requested_) return false;
        requested_ = true;
        requested_at_ = now;
        return true;
    }
    void publish(DeviceMemory memory) {
        std::lock_guard lock(mutex_);
        memory_ = std::move(memory);
    }
    [[nodiscard]] bool warn_pending_once(const Clock::time_point now = Clock::now()) {
        std::lock_guard lock(mutex_);
        if (!requested_ || memory_ || pending_warned_ || now - requested_at_ < pending_warning_delay) return false;
        pending_warned_ = true;
        return true;
    }
    [[nodiscard]] std::optional<DeviceMemory> memory() const {
        std::lock_guard lock(mutex_);
        return memory_;
    }

private:
    mutable std::mutex mutex_;
    bool requested_{}, pending_warned_{};
    Clock::time_point requested_at_{};
    std::optional<DeviceMemory> memory_;
};

// The engine dispatcher executes inline in Safe mode and queues in Separate mode.
// Keep that dispatch outside the cache lock, including an immediate publication.
template<class Dispatch, class Warn>
[[nodiscard]] std::optional<DeviceMemory> request_device_memory(DeviceMemoryProbe& probe,
    Dispatch&& dispatch, Warn&& warn,
    const DeviceMemoryProbe::Clock::time_point now = DeviceMemoryProbe::Clock::now()) {
    if (probe.begin(now)) std::forward<Dispatch>(dispatch)();
    else if (probe.warn_pending_once(now)) std::forward<Warn>(warn)();
    return probe.memory();
}

} // namespace eawr::presentation::godot_backend::detail
