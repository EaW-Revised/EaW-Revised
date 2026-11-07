#include "eawr/scene/model_prefetch.hpp"
#include "eawr/vfs/vfs.hpp"

#include "eawr/scene/scene.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace eawr::scene {
namespace {

std::string lower_key(const std::string_view text) {
    std::string key(text);
    for (auto& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return key;
}

// min(64 MiB, physical memory / 64): the most estimated bytes decoded ahead and not yet asked for.
std::uint64_t default_window_bytes() {
    std::uint64_t window = 64ULL << 20U;
#ifdef _WIN32
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory) && memory.ullTotalPhys / 64 < window) window = memory.ullTotalPhys / 64;
#endif
    return window;
}

} // namespace

unsigned ModelPrefetch::policy_readers(const bool solid_state, const unsigned logical_processors) noexcept {
    if (!solid_state) return 1U; // spinning or unclassified: parallel reads lose
    const unsigned cores = std::max(1U, logical_processors);
    return std::clamp(cores > 2 ? cores - 2 : 1U, 1U, 8U);
}

ModelPrefetch::ModelPrefetch(const vfs::Vfs& filesystem, ModelPrefetchOptions options)
    : filesystem_(&filesystem), options_(std::move(options)) {
    stats_.window_bytes = options_.window_bytes != 0 ? options_.window_bytes : default_window_bytes();
    const unsigned count = options_.fixed_readers != 0
        ? options_.fixed_readers
        : policy_readers(options_.solid_state, std::thread::hardware_concurrency());
    stats_.readers = count;
    threads_.reserve(count);
    for (unsigned i = 0; i < count; ++i) threads_.emplace_back([this] { reader(); });
}

ModelPrefetch::~ModelPrefetch() { finish(); }

void ModelPrefetch::offer(const std::string_view logical_path) {
    if (logical_path.empty()) return;
    const auto record = filesystem_->stat(logical_path); // a missing file stays the loader's to report
    if (!record) return;
    const auto key = lower_key(logical_path);
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        const auto [found, inserted] = entries_.try_emplace(key);
        if (!inserted) return;
        found->second.path = std::string(logical_path);
        found->second.cost = record.value().size * 22 / 10;
        queue_.push_back(key);
        ++stats_.planned;
    }
    changed_.notify_one();
}

void ModelPrefetch::taken(const std::string_view logical_path) {
    const auto key = lower_key(logical_path);
    std::shared_ptr<const assets::Model> released; // freed after the lock is released
    {
        std::lock_guard lock(mutex_);
        const auto found = entries_.find(key);
        if (found == entries_.end()) {
            ++stats_.unplanned;
            return;
        }
        auto& entry = found->second;
        if (entry.state == State::taken) return;
        switch (entry.state) {
        case State::queued: ++stats_.served_unstarted; break; // a reader will skip it
        case State::decoding: ++stats_.served_inflight; break; // its reader releases the charge when it finishes
        case State::done:
            ++stats_.served_ready;
            // A failed decode already released its charge (nothing was held).
            if (entry.held) charged_ -= std::min(charged_, entry.cost);
            break;
        case State::taken: break;
        }
        released = std::move(entry.held);
        entry.state = State::taken;
    }
    changed_.notify_all();
}

void ModelPrefetch::skipped(const std::string_view logical_path) {
    const auto key = lower_key(logical_path);
    std::shared_ptr<const assets::Model> released; // freed after the lock is released
    {
        std::lock_guard lock(mutex_);
        const auto found = entries_.find(key);
        if (found == entries_.end()) return;
        auto& entry = found->second;
        if (entry.state == State::taken) return;
        ++stats_.skipped;
        if (entry.state == State::done && entry.held) {
            ++stats_.unrequested;
            charged_ -= std::min(charged_, entry.cost);
        }
        // queued: a reader will skip it; decoding: its reader releases the charge and the model when it finishes.
        released = std::move(entry.held);
        entry.state = State::taken;
    }
    changed_.notify_all();
}

void ModelPrefetch::reader() {
    for (;;) {
        std::string key;
        std::string path;
        std::uint64_t cost = 0;
        {
            std::unique_lock lock(mutex_);
            for (;;) {
                if (stopping_) return;
                // Skip what the loader already took.
                while (!queue_.empty() && entries_[queue_.front()].state != State::queued) queue_.pop_front();
                if (!queue_.empty()) {
                    const auto& head = entries_[queue_.front()];
                    // Window: held plus in-flight bytes stay under the cap, except one file alone.
                    if (charged_ == 0 || charged_ + head.cost <= stats_.window_bytes) break;
                }
                changed_.wait(lock);
            }
            key = queue_.front();
            queue_.pop_front();
            auto& entry = entries_[key];
            entry.state = State::decoding;
            cost = entry.cost;
            charged_ += cost;
            ++decoding_;
            stats_.readers_peak = std::max<std::uint64_t>(stats_.readers_peak, decoding_);
            stats_.held_bytes_peak = std::max(stats_.held_bytes_peak, charged_);
            path = entry.path;
        }
        std::shared_ptr<const assets::Model> model;
        try {
            if (options_.decode_hook) options_.decode_hook(path);
            VfsAssetCache cache(*filesystem_); // one wrapper per call: its memo maps are not thread-safe
            auto result = cache.shared_model_ahead(path);
            if (result) model = std::move(result).value();
        } catch (const std::exception&) {
            model.reset(); // the loader will meet the same failure on its own request and report it
        } catch (...) {
            model.reset();
        }
        std::shared_ptr<const assets::Model> released;
        {
            std::lock_guard lock(mutex_);
            auto& entry = entries_[key];
            --decoding_;
            ++stats_.decoded;
            if (entry.state == State::taken || stopping_ || !model) {
                // Already asked for (the loader pins it), shutting down, or nothing to hold.
                charged_ -= std::min(charged_, cost);
                released = std::move(model);
                if (entry.state != State::taken) entry.state = State::done;
            } else {
                entry.state = State::done;
                entry.held = std::move(model);
            }
        }
        changed_.notify_all();
    }
}

void ModelPrefetch::finish() {
    std::vector<std::shared_ptr<const assets::Model>> released;
    std::vector<std::thread> threads;
    {
        std::lock_guard lock(mutex_);
        if (stopping_ && threads_.empty()) return;
        stopping_ = true;
        queue_.clear();
        threads = std::move(threads_);
        threads_.clear();
    }
    changed_.notify_all();
    for (auto& thread : threads) thread.join();
    {
        std::lock_guard lock(mutex_);
        for (auto& [key, entry] : entries_) {
            if (entry.state == State::done && entry.held) ++stats_.unrequested;
            if (entry.held) released.push_back(std::move(entry.held));
            entry.held.reset();
        }
        charged_ = 0;
    }
}

ModelPrefetchStats ModelPrefetch::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

std::size_t ModelPrefetch::held_count() const {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto& [key, entry] : entries_) count += entry.held ? 1U : 0U;
    return count;
}

} // namespace eawr::scene
