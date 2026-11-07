#include "eawr/scene/texture_prefetch.hpp"
#include "eawr/assets/texture_chain.hpp"
#include "eawr/vfs/vfs.hpp"

#include "eawr/core/load_profile.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
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

// min(64 MiB, physical memory / 64): the most bytes decoded ahead and not yet asked for.
std::uint64_t default_window_bytes() {
    std::uint64_t window = 64ULL << 20U;
#ifdef _WIN32
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory) && memory.ullTotalPhys / 64 < window) window = memory.ullTotalPhys / 64;
#endif
    return window;
}

// One entry per path and processing: the same file wanted both plain and with a chain is two decodes.
std::string entry_key(const std::string_view text, const TextureProcessing processing) {
    std::string key = lower_key(text);
    // '|' is not a legal file name character: a processing suffix never collides with a path.
    if (processing == TextureProcessing::colour_mip_chain) key += "|chain";
    else if (processing == TextureProcessing::normal_mip_chain) key += "|normal-chain";
    return key;
}

assets::ChainKind chain_kind(const TextureProcessing processing) noexcept {
    return processing == TextureProcessing::normal_mip_chain ? assets::ChainKind::normal : assets::ChainKind::colour;
}

bool wants_chain(const TextureProcessing processing) noexcept { return processing != TextureProcessing::none; }

std::uint64_t payload_bytes(const assets::Texture& texture) noexcept {
    std::uint64_t bytes = 0;
    for (const assets::MipLevel& mip : texture.mips) bytes += mip.bytes.size();
    return bytes;
}

double since_ms(const std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

unsigned TexturePrefetch::policy_readers(const bool solid_state, const unsigned logical_processors) noexcept {
    if (!solid_state) return 1U; // spinning or unclassified: parallel reads lose
    const unsigned cores = std::max(1U, logical_processors);
    return std::clamp(cores > 2 ? cores - 2 : 1U, 1U, 8U);
}

TexturePrefetch::TexturePrefetch(const vfs::Vfs& filesystem, TexturePrefetchOptions options)
    : filesystem_(&filesystem), options_(std::move(options)) {
    stats_.window_bytes = options_.window_bytes != 0 ? options_.window_bytes : default_window_bytes();
    const unsigned count = options_.fixed_readers != 0
        ? options_.fixed_readers
        : policy_readers(options_.solid_state, std::thread::hardware_concurrency());
    stats_.readers = count;
    threads_.reserve(count);
    for (unsigned i = 0; i < count; ++i) threads_.emplace_back([this] { reader(); });
}

TexturePrefetch::~TexturePrefetch() { finish(); }

void TexturePrefetch::offer(const std::string_view logical_path, const std::uint64_t step, const TextureProcessing processing) {
    if (logical_path.empty()) return;
    const auto record = filesystem_->stat(logical_path); // a missing file stays the loader's to report
    if (!record) return;
    const auto key = entry_key(logical_path, processing);
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        const auto [found, inserted] = entries_.try_emplace(key);
        if (!inserted) return;
        found->second.path = std::string(logical_path);
        found->second.processing = processing;
        found->second.step = step;
        found->second.cost = std::max<std::uint64_t>(record.value().size, 1U) * 2U;
        queue_.push_back(key);
        ++stats_.planned;
    }
    changed_.notify_all();
}

core::Result<assets::Texture> TexturePrefetch::acquire(const std::string_view logical_path, const TextureProcessing processing) {
    const auto key = entry_key(logical_path, processing);
    std::optional<assets::Texture> ready;
    {
        std::unique_lock lock(mutex_);
        const auto found = entries_.find(key);
        if (found == entries_.end() || found->second.state == State::taken) {
            ++stats_.unplanned;
        } else {
            auto& entry = found->second;
            if (entry.state == State::decoding) {
                ++stats_.served_inflight;
                const auto waited = std::chrono::steady_clock::now();
                core::load_profile::Scope wait_scope(core::load_profile::Phase::texture_wait); // the loader's recorder only
                changed_.wait(lock, [&] { return entry.state != State::decoding || stopping_; });
                stats_.wait_ms += static_cast<std::uint64_t>(since_ms(waited));
            } else if (entry.state == State::queued) {
                ++stats_.served_unstarted; // a reader will skip it
            } else {
                ++stats_.served_ready;
            }
            if (entry.state == State::done && entry.held) {
                charged_ -= std::min(charged_, entry.cost);
                ready = std::move(entry.held);
                entry.held.reset();
            }
            // A stop leaves an in-flight decode `decoding`: marking it taken makes its reader free the result.
            entry.state = State::taken;
        }
    }
    changed_.notify_all();
    if (ready) return core::Result<assets::Texture>::success(std::move(*ready));
    // Nothing was ready: the loader decodes it itself, and builds the chain itself if one is wanted.
    auto result = assets::load_texture(*filesystem_, logical_path);
    if (result && wants_chain(processing) && assets::extend_mip_chain(result.value(), chain_kind(processing))) {
        std::lock_guard lock(mutex_);
        ++stats_.chained_by_loader;
    }
    return result;
}

void TexturePrefetch::advance(const std::uint64_t step) {
    std::vector<std::optional<assets::Texture>> released; // freed after the lock is released
    {
        std::lock_guard lock(mutex_);
        for (auto& [key, entry] : entries_) {
            if (entry.state == State::taken || entry.step >= step) continue;
            ++stats_.skipped;
            if (entry.state == State::done && entry.held) {
                ++stats_.unrequested;
                charged_ -= std::min(charged_, entry.cost);
                released.push_back(std::move(entry.held));
                entry.held.reset();
            }
            // queued: a reader will skip it; decoding: its reader releases the charge and the texture when it finishes.
            entry.state = State::taken;
        }
    }
    changed_.notify_all();
}

void TexturePrefetch::reader() {
    for (;;) {
        std::string key;
        std::string path;
        TextureProcessing processing = TextureProcessing::none;
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
            processing = entry.processing;
        }
        std::optional<assets::Texture> texture;
        const auto started = std::chrono::steady_clock::now();
        std::uint64_t chain_ms = 0;
        std::uint64_t chain_bytes = 0;
        try {
            if (options_.decode_hook) options_.decode_hook(path);
            auto result = assets::load_texture(*filesystem_, path); // reader thread: no recorder, nothing attributed
            if (result) texture = std::move(result).value();
            if (texture && wants_chain(processing)) {
                const auto chain_started = std::chrono::steady_clock::now();
                chain_bytes = assets::mip_chain_added_bytes(*texture);
                if (!assets::extend_mip_chain(*texture, chain_kind(processing))) chain_bytes = 0;
                chain_ms = static_cast<std::uint64_t>(since_ms(chain_started));
            }
        } catch (const std::exception&) {
            texture.reset(); // the loader will meet the same failure on its own request and report it
        } catch (...) {
            texture.reset();
        }
        const auto decode_ms = static_cast<std::uint64_t>(since_ms(started));
        std::optional<assets::Texture> released;
        {
            std::lock_guard lock(mutex_);
            auto& entry = entries_[key];
            --decoding_;
            ++stats_.decoded;
            stats_.reader_decode_ms += decode_ms;
            if (chain_bytes != 0 && texture) {
                ++stats_.chained;
                stats_.chain_added_bytes += chain_bytes;
                stats_.chain_ms += chain_ms;
            }
            if (!texture) ++stats_.failed;
            if (entry.state == State::taken || stopping_ || !texture) {
                // Already asked for or skipped, shutting down, or nothing to hold.
                charged_ -= std::min(charged_, cost);
                released = std::move(texture);
                if (entry.state != State::taken) entry.state = State::done;
            } else {
                // Charge what is really held instead of the estimate.
                const std::uint64_t actual = payload_bytes(*texture);
                charged_ = charged_ - std::min(charged_, cost) + actual;
                entry.cost = actual;
                entry.state = State::done;
                entry.held = std::move(texture);
                stats_.held_bytes_peak = std::max(stats_.held_bytes_peak, charged_);
            }
        }
        changed_.notify_all();
    }
}

void TexturePrefetch::finish() {
    std::vector<std::optional<assets::Texture>> released;
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

TexturePrefetchStats TexturePrefetch::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

std::size_t TexturePrefetch::held_count() const {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto& [key, entry] : entries_) count += entry.held ? 1U : 0U;
    return count;
}

std::uint64_t TexturePrefetch::charged_bytes() const {
    std::lock_guard lock(mutex_);
    return charged_;
}

} // namespace eawr::scene
