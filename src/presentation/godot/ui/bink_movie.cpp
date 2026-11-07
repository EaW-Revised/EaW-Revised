#include "bink_movie.hpp"

#include <utility>
#include <deque>

namespace eawr::presentation::godot_backend {
bool admit_bink(const std::span<const std::byte> bytes, const std::uint64_t memory_budget,
                BinkMetadata& metadata) {
    metadata = {};
    if (bytes.size() < 44 || bytes[0] != std::byte{'B'} || bytes[1] != std::byte{'I'}
        || bytes[2] != std::byte{'K'} || bytes[3] != std::byte{'i'}) return false;
    const auto word = [&](const std::size_t offset) {
        std::uint32_t value{};
        for (std::size_t i = 0; i < 4; ++i)
            value |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (8U * i);
        return value;
    };
    const auto count = word(8), max_packet = word(12), bound = word(16);
    const auto raw_width = word(20), height = word(24), numerator = word(28), denominator = word(32);
    const std::int64_t signed_width = raw_width <= 0x7fffffffU
        ? static_cast<std::int64_t>(raw_width) : static_cast<std::int64_t>(raw_width) - 0x100000000LL;
    const auto width = signed_width < 0 ? -signed_width : signed_width;
    if (static_cast<std::uint64_t>(word(4)) + 8 != bytes.size() || count == 0 || count > bound
        || bound > 1'000'000 || max_packet == 0 || max_packet > bytes.size()
        || width == 0 || width > 32767 || height == 0 || height > 32767
        || numerator == 0 || denominator == 0 || word(40) != 0) return false;
    const auto image_bytes = static_cast<std::uint64_t>(width) * 4 * height;
    if (image_bytes > memory_budget || bytes.size() > memory_budget - image_bytes) return false;
    const std::uint64_t table_end = 44ULL + 4ULL * (static_cast<std::uint64_t>(count) + 1ULL);
    if (table_end > bytes.size()) return false;
    auto previous = static_cast<std::uint64_t>(word(44) & ~1U);
    if (previous < table_end) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto next = static_cast<std::uint64_t>(word(44 + 4ULL * (static_cast<std::uint64_t>(i) + 1)) & ~1U);
        if (next <= previous || next > bytes.size() || next - previous > max_packet) return false;
        previous = next;
    }
    if (previous != bytes.size()) return false;
    metadata = {static_cast<std::uint32_t>(width), height, count, numerator, denominator};
    return true;
}
namespace {
// Joins retired workers away from gameplay; never join under the DLL loader lock.
class BinkReaper {
public:
    BinkReaper() : thread_([this] { run(); }) {}
    void retire(std::thread thread) {
        try {
            { std::lock_guard lock(mutex_); threads_.push_back(std::move(thread)); }
            wake_.notify_one();
        } catch (...) {
            // Allocation failure must not destroy a joinable thread.
            if (thread.joinable()) thread.detach();
        }
    }
private:
    void run() {
        for (;;) {
            std::thread retired;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return !threads_.empty(); });
                retired = std::move(threads_.front());
                threads_.pop_front();
            }
            if (retired.joinable()) retired.join();
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::thread> threads_;
    std::thread thread_;
};
BinkReaper& reaper() { static auto* instance = new BinkReaper; return *instance; }
} // namespace
struct BinkWorker::State {
    mutable std::mutex mutex;
    std::condition_variable wake;
    BinkFrame ready;
    bool pending{true}, decoding{}, has_frame{}, failed{}, stopping{};
    std::shared_ptr<std::atomic_bool> cancelled{std::make_shared<std::atomic_bool>(false)};
};
BinkWorker::BinkWorker(Factory factory, PrepareFrame prepare_frame) : state_(std::make_shared<State>()) {
    // Start the reaper at construction, never during gameplay teardown.
    (void)reaper();
    thread_ = std::thread([state = state_, factory = std::move(factory), prepare_frame = std::move(prepare_frame)]() mutable {
        run(std::move(state), std::move(factory), std::move(prepare_frame));
    });
}
BinkWorker::~BinkWorker() {
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        *state_->cancelled = true;
    }
    state_->wake.notify_one();
    // Cancellation wakes the owned worker; the reaper performs the join.
    if (thread_.joinable()) reaper().retire(std::move(thread_));
}
void BinkWorker::request() {
    {
        std::lock_guard lock(state_->mutex);
        if (state_->failed || state_->stopping || state_->pending || state_->decoding || state_->has_frame) return;
        state_->pending = true;
    }
    state_->wake.notify_one();
}
bool BinkWorker::take(BinkFrame& frame) {
    std::lock_guard lock(state_->mutex);
    if (!state_->has_frame) return false;
    frame = std::move(state_->ready);
    state_->has_frame = false;
    return true;
}
bool BinkWorker::failed() const {
    std::lock_guard lock(state_->mutex);
    return state_->failed;
}
void BinkWorker::run(std::shared_ptr<State> state, Factory factory, PrepareFrame prepare_frame) {
    try {
        auto decoder = factory(state->cancelled);
        if (!decoder) {
            std::lock_guard lock(state->mutex);
            state->failed = true;
            return;
        }
        for (;;) {
            {
                std::unique_lock lock(state->mutex);
                state->wake.wait(lock, [&] { return state->pending || state->stopping; });
                if (state->stopping) return;
                state->pending = false;
                state->decoding = true;
            }
            BinkFrame frame;
            if (!decoder->next(frame)) {
                std::lock_guard lock(state->mutex);
                state->failed = true;
                return;
            }
            if (prepare_frame) prepare_frame(frame);
            {
                std::lock_guard lock(state->mutex);
                if (state->stopping) return;
                state->decoding = false;
                state->ready = std::move(frame);
                state->has_frame = true;
            }
        }
    } catch (...) {
        std::lock_guard lock(state->mutex);
        state->failed = true;
    }
}
} // namespace eawr::presentation::godot_backend
