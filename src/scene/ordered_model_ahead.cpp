#include "eawr/scene/ordered_model_ahead.hpp"

#include "eawr/assets/memory.hpp"
#include "eawr/core/ascii.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

namespace eawr::scene {
namespace {

using Clock = std::chrono::steady_clock;

std::string lower_key(const std::string_view text) {
    std::string key(text);
    for (auto& ch : key) ch = static_cast<char>(core::ascii_lower(static_cast<unsigned char>(ch)));
    return key;
}

std::uint64_t milliseconds_since(const Clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

} // namespace

AheadModel read_hash_decode_model(const vfs::Vfs& filesystem, const std::string_view logical_path) {
    auto bytes = filesystem.open(logical_path);
    if (!bytes) return {{}, core::Result<assets::Model>::failure(bytes.error())};
    std::string sha256 = core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(bytes.value().data()), bytes.value().size()));
    const auto record = filesystem.stat(logical_path);
    if (!record) return {std::move(sha256), core::Result<assets::Model>::failure(record.error())};
    return {std::move(sha256), assets::load_model(bytes.value(), assets::source_from(record.value()))};
}

OrderedModelAhead::OrderedModelAhead(const vfs::Vfs& filesystem, std::vector<std::string> logical_paths,
                                     OrderedModelAheadListener* listener, const std::uint64_t window_bytes)
    : filesystem_(&filesystem), listener_(listener), window_bytes_(window_bytes) {
    for (auto& path : logical_paths) {
        if (path.empty()) continue;
        const auto [found, inserted] = index_.try_emplace(lower_key(path), entries_.size());
        if (!inserted) continue;
        Entry entry;
        entry.path = std::move(path);
        entries_.push_back(std::move(entry));
    }
    stats_.planned = entries_.size();
}

OrderedModelAhead::~OrderedModelAhead() { finish(); }

void OrderedModelAhead::start() noexcept {
    std::lock_guard lock(mutex_);
    if (started_ || stopping_ || entries_.empty()) return;
    started_ = true;
    try {
        thread_ = std::thread([this] { worker(); });
    } catch (...) {
        started_ = false; // no thread to be had: take() decodes inline
        stopping_ = true;
    }
}

void OrderedModelAhead::worker() {
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        std::string path;
        {
            std::unique_lock lock(mutex_);
            for (;;) {
                if (stopping_) return;
                if (entries_[index].state != State::queued) break; // the loader decoded it itself
                // Window: finished models waiting to be taken stay under the cap, except one alone.
                if (charged_ == 0 || charged_ < window_bytes_) break;
                held_by_window_ = true;
                changed_.notify_all(); // a loader waiting for this entry stops waiting
                changed_.wait(lock);
                held_by_window_ = false;
            }
            if (entries_[index].state != State::queued) continue;
            entries_[index].state = State::decoding;
            path = entries_[index].path;
        }
        const auto began = Clock::now();
        std::optional<AheadModel> decoded;
        try {
            decoded = read_hash_decode_model(*filesystem_, path);
            if (listener_ != nullptr) listener_->model_ready(index, path, *decoded);
        } catch (...) {
            decoded.reset(); // the loader meets the same failure on its own request and reports it
        }
        const auto cost = decoded && decoded->model ? assets::payload_capacity(decoded->model.value()).total_bytes() : std::uint64_t{0};
        {
            std::lock_guard lock(mutex_);
            auto& entry = entries_[index];
            ++stats_.decoded;
            stats_.decode_ms += milliseconds_since(began);
            if (!decoded || stopping_) {
                entry.state = State::abandoned;
            } else {
                entry.state = State::done;
                entry.cost = cost;
                entry.held = std::move(decoded);
                charged_ += cost;
                stats_.held_bytes_peak = std::max(stats_.held_bytes_peak, charged_);
            }
        }
        changed_.notify_all();
    }
}

AheadModel OrderedModelAhead::take(const std::string_view logical_path) {
    const auto inline_decode = [&] {
        {
            std::lock_guard lock(mutex_);
            ++stats_.served_inline;
        }
        return read_hash_decode_model(*filesystem_, logical_path);
    };
    std::optional<AheadModel> ready;
    {
        std::unique_lock lock(mutex_);
        const auto found = index_.find(lower_key(logical_path));
        if (found == index_.end()) {
            lock.unlock();
            return inline_decode();
        }
        auto& entry = entries_[found->second];
        const auto began = Clock::now();
        bool waited = false;
        for (;;) {
            if (entry.state == State::done) break;
            if (entry.state == State::taken || entry.state == State::abandoned) break;
            // Not started, stopped, or the worker is held by a full window that this loader's order does not drain.
            if (entry.state == State::queued && (!started_ || stopping_ || held_by_window_)) {
                entry.state = State::taken; // the worker skips it
                break;
            }
            waited = true;
            changed_.wait(lock);
        }
        if (waited) stats_.wait_ms += milliseconds_since(began);
        if (entry.state == State::done) {
            ++(waited ? stats_.served_inflight : stats_.served_ready);
            charged_ -= std::min(charged_, entry.cost);
            ready = std::move(entry.held);
            entry.held.reset();
            entry.state = State::taken;
        }
    }
    changed_.notify_all();
    if (ready) return std::move(*ready);
    return inline_decode();
}

void OrderedModelAhead::finish() {
    std::thread thread;
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        thread = std::move(thread_);
    }
    changed_.notify_all();
    if (thread.joinable()) thread.join();
    std::lock_guard lock(mutex_);
    for (auto& entry : entries_) {
        if (entry.state == State::done && entry.held) ++stats_.unrequested;
        entry.held.reset();
    }
    charged_ = 0;
}

OrderedModelAheadStats OrderedModelAhead::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

} // namespace eawr::scene
