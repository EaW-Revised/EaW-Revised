#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend {

// A bounded presentation I/O worker. Request/drain never wait for an in-flight
// read: a contended queue is retried next frame. The reader owns its inputs
// (including the shared VFS), never a borrowed BattleAudio or scene-tree node.
// Destruction cancels queued work and joins the one current read before inputs
// or prepared resources are released. There is no synchronous fallback.
template<class Key, class Value>
class BattleAudioLoadQueue final {
public:
    struct Ready final {
        Key key;
        std::optional<Value> value;
        std::string error;
    };
    using Reader = std::function<Value(const Key&)>;

    explicit BattleAudioLoadQueue(Reader reader, const std::size_t capacity = 128)
        : reader_(std::move(reader)), capacity_(capacity), worker_([this] { run(); }) {}

    ~BattleAudioLoadQueue() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            pending_.clear();
        }
        condition_.notify_one();
        if (worker_.joinable()) worker_.join();
    }
    BattleAudioLoadQueue(const BattleAudioLoadQueue&) = delete;
    BattleAudioLoadQueue& operator=(const BattleAudioLoadQueue&) = delete;

    [[nodiscard]] bool request(const Key& key) {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || stopping_) return false;
        if (active_.contains(key)) return true;
        if (active_.size() >= capacity_) return false;
        active_.insert(key);
        pending_.push_back(key);
        condition_.notify_one();
        return true;
    }

    [[nodiscard]] std::vector<Ready> take_ready(const std::size_t limit = std::numeric_limits<std::size_t>::max()) {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return {};
        std::vector<Ready> result;
        result.reserve(std::min(limit, ready_.size()));
        // Stable key order, independent of the caller's request order.
        while (!ready_.empty() && result.size() < limit) {
            auto first = ready_.begin();
            active_.erase(first->first);
            result.push_back(std::move(first->second));
            ready_.erase(first);
        }
        return result;
    }

private:
    void run() {
        for (;;) {
            std::optional<Key> key;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
                if (stopping_) return;
                key.emplace(std::move(pending_.front()));
                pending_.pop_front();
            }
            Ready result{*key, std::nullopt, {}};
            try { result.value.emplace(reader_(*key)); }
            catch (const std::exception& error) { result.error = error.what(); }
            catch (...) { result.error = "audio worker failed"; }
            {
                std::lock_guard lock(mutex_);
                if (stopping_) return;
                ready_.emplace(*key, std::move(result));
            }
        }
    }

    Reader reader_;
    std::size_t capacity_{};
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stopping_{};
    std::deque<Key> pending_;
    std::set<Key> active_;
    std::map<Key, Ready> ready_;
    std::thread worker_; // last: the synchronized state exists before thread start
};

} // namespace eawr::presentation::godot_backend
