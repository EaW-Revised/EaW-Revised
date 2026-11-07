#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/vfs/vfs_fwd.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace eawr::scene {

struct ModelPrefetchOptions final {
    // 0 = by policy: solid state gets clamp(cores - 2, 1, 8) readers, every other volume exactly one. Nonzero = that many
    // readers (tests, A/B).
    unsigned fixed_readers{};
    bool solid_state{}; // the models' volume reported no seek penalty
    // Cap on the estimated bytes (file size x 2.2) of models decoded ahead and not yet asked for, plus those being decoded.
    // 0 = min(64 MiB, physical memory / 64). One file larger than the cap still runs, alone.
    std::uint64_t window_bytes{};
    // Called on a reader thread before it decodes a path; tests use it to block or count decodes.
    std::function<void(std::string_view)> decode_hook;
};

struct ModelPrefetchStats final {
    std::uint64_t planned{};          // distinct paths offered
    std::uint64_t decoded{};          // paths a reader decoded (or found already decoded)
    std::uint64_t served_ready{};     // loader asked after the reader had finished
    std::uint64_t served_inflight{};  // loader asked while a reader was decoding it
    std::uint64_t served_unstarted{}; // loader asked before any reader took it (the loader decoded it itself)
    std::uint64_t unplanned{};        // loader asked for a path nobody offered
    std::uint64_t unrequested{};      // decoded ahead and never asked for
    std::uint64_t skipped{};          // holds or queue slots released because the loader moved past the path
    std::uint64_t readers{};          // reader threads
    std::uint64_t readers_peak{};     // most readers decoding at once
    std::uint64_t window_bytes{};
    std::uint64_t held_bytes_peak{};  // most estimated bytes held or in flight at once
};

// Decode-ahead for the model store. The loader keeps its own order and asks the store as before; this class only makes
// the store's decode of a path happen earlier, on reader threads. A reader calls VfsAssetCache::shared_model_ahead, so the
// result is the store's own entry (one decode per file identity, std::call_once), not a copy, but it is not admitted to
// the store's LRU until the loader's own request. Readers hold the result (inside the window) until the loader asks
// (taken), moves past the path (skipped), or the prefetch ends; a decode nobody asks for is freed then. No Godot, no scene tree, no textures.
class ModelPrefetch final {
public:
    ModelPrefetch(const vfs::Vfs& filesystem, ModelPrefetchOptions options);
    ~ModelPrefetch();
    ModelPrefetch(const ModelPrefetch&) = delete;
    ModelPrefetch& operator=(const ModelPrefetch&) = delete;

    // The loader will probably ask for this path soon. Any thread. Duplicates (ASCII case-insensitive) are ignored.
    void offer(std::string_view logical_path);
    // The loader has asked for this path (call after its request returned). Drops the hold.
    void taken(std::string_view logical_path);
    // The loader moved past this path without asking for it (the unit that hinted it was loaded or failed first).
    // Drops the hold and the window charge so readers are not throttled by a decode nobody will use.
    void skipped(std::string_view logical_path);
    // Stops the readers (each finishes at most its current file), joins them, drops every hold. Idempotent.
    void finish();
    [[nodiscard]] ModelPrefetchStats stats() const;
    // Held entries right now (tests).
    [[nodiscard]] std::size_t held_count() const;
    [[nodiscard]] static unsigned policy_readers(bool solid_state, unsigned logical_processors) noexcept;

private:
    enum class State : std::uint8_t { queued, decoding, done, taken };
    struct Entry final {
        State state{State::queued};
        std::string path; // as first offered
        std::uint64_t cost{};
        std::shared_ptr<const assets::Model> held;
    };
    void reader();

    const vfs::Vfs* filesystem_;
    ModelPrefetchOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, Entry> entries_; // key: lower-case path
    std::deque<std::string> queue_;        // keys, FIFO
    std::uint64_t charged_{};              // estimated bytes held or decoding
    unsigned decoding_{};
    bool stopping_{};
    ModelPrefetchStats stats_;
    std::vector<std::thread> threads_; // last
};

} // namespace eawr::scene
