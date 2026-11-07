#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <span>
#include <cstddef>
#include <atomic>

namespace eawr::presentation::godot_backend {

struct BinkFrame {
    std::uint32_t width{}, height{}, rate_numerator{}, rate_denominator{};
    std::vector<std::uint8_t> rgba;
    std::shared_ptr<void> presentation_data;
    std::uint32_t selected_frame{}, total_frames{}, trace_first[3]{}, trace_last{};
};

struct BinkMetadata {
    std::uint32_t width{}, height{}, frames{}, rate_numerator{}, rate_denominator{};
};
// Structural admission only; compressed data must still be decoded in a child process.
[[nodiscard]] bool admit_bink(std::span<const std::byte> bytes, std::uint64_t memory_budget,
                              BinkMetadata& metadata);

// The backend and its source are owned and called only by the worker thread.
class BinkDecoder {
public:
    virtual ~BinkDecoder() = default;
    [[nodiscard]] virtual bool next(BinkFrame& frame) = 0;
};

using BinkSourceFactory = std::function<std::unique_ptr<BinkDecoder>(std::vector<std::byte>,
    std::shared_ptr<std::atomic_bool>)>;

class BinkWorker final {
public:
    using Factory = std::function<std::unique_ptr<BinkDecoder>(std::shared_ptr<std::atomic_bool>)>;
    using PrepareFrame = std::function<void(BinkFrame&)>;
    explicit BinkWorker(Factory factory, PrepareFrame prepare_frame = {});
    ~BinkWorker();
    BinkWorker(const BinkWorker&) = delete;
    BinkWorker& operator=(const BinkWorker&) = delete;
    // Coalesces requests; at most one completed frame is retained.
    void request();
    [[nodiscard]] bool take(BinkFrame& frame);
    [[nodiscard]] bool failed() const;
private:
    struct State;
    static void run(std::shared_ptr<State> state, Factory factory, PrepareFrame prepare_frame);
    std::shared_ptr<State> state_;
    std::thread thread_;
};

} // namespace eawr::presentation::godot_backend
