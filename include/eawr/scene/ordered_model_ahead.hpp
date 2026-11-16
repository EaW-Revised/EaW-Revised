#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/core/result.hpp"
#include "eawr/vfs/vfs_fwd.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace eawr::scene {

// One model read once: the SHA-256 is of the bytes that were decoded.
struct AheadModel final {
    std::string sha256;                // empty when the file could not be read
    core::Result<assets::Model> model; // the decode, or the diagnostic of the read or the decode that failed
};

// Reads the model's bytes once, hashes them and decodes them (stat for the source identity, open for the bytes). The same
// answer wherever it runs: this is what a loader does inline and what OrderedModelAhead does on its thread.
[[nodiscard]] AheadModel read_hash_decode_model(const vfs::Vfs& filesystem, std::string_view logical_path);

// Runs on the worker thread right after a model was decoded, before the loader can ask for it; the model stays valid for the
// call. The galaxy upload uses it to offer the textures that model will ask for to the texture read-ahead. Must not throw.
class OrderedModelAheadListener {
public:
    virtual ~OrderedModelAheadListener() = default;
    virtual void model_ready(std::size_t index, const std::string& logical_path, const AheadModel& decoded) noexcept = 0;
};

struct OrderedModelAheadStats final {
    std::uint64_t planned{};
    std::uint64_t decoded{};          // by the worker (a failed read or decode counts)
    std::uint64_t served_ready{};     // loader asked after the worker had finished
    std::uint64_t served_inflight{};  // loader asked while the worker was decoding it (it waited)
    std::uint64_t served_inline{};    // loader decoded it itself: not started, not planned, or the worker was held by the window
    std::uint64_t unrequested{};      // decoded ahead and never asked for
    std::uint64_t held_bytes_peak{};
    std::uint64_t decode_ms{};        // summed wall time of the worker's decodes
    std::uint64_t wait_ms{};          // wall time the loader waited for a model still being decoded
};

// Decode-ahead of a known list of models, in list order, on ONE worker thread (one reader: the right count for a spinning
// disk, and the models are few). take() returns exactly what read_hash_decode_model would return on the calling thread, so the
// consumer's order and diagnostics do not change; the worker only moves the read, the hash and the parse earlier. Finished models wait in a
// window (estimated payload bytes) until taken; a worker held by a full window never blocks the loader, which then decodes
// the model it wants itself.
class OrderedModelAhead final {
public:
    OrderedModelAhead(const vfs::Vfs& filesystem, std::vector<std::string> logical_paths, OrderedModelAheadListener* listener,
                      std::uint64_t window_bytes);
    ~OrderedModelAhead();
    OrderedModelAhead(const OrderedModelAhead&) = delete;
    OrderedModelAhead& operator=(const OrderedModelAhead&) = delete;

    // Starts the worker. Any thread, once; later calls and a failed thread start do nothing (take() then decodes inline).
    void start() noexcept;
    // The decode of `logical_path`, from the worker when it has it or is on it, otherwise on the calling thread.
    [[nodiscard]] AheadModel take(std::string_view logical_path);
    // Stops the worker (it finishes at most its current model), joins it and drops every held model. Idempotent.
    void finish();
    [[nodiscard]] OrderedModelAheadStats stats() const;

private:
    enum class State : std::uint8_t { queued, decoding, done, taken, abandoned };
    struct Entry final {
        std::string path;
        State state{State::queued};
        std::uint64_t cost{};
        std::optional<AheadModel> held;
    };
    void worker();

    const vfs::Vfs* filesystem_;
    OrderedModelAheadListener* listener_;
    std::uint64_t window_bytes_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Entry> entries_;
    std::map<std::string, std::size_t> index_; // lower-case path -> entry
    std::uint64_t charged_{};
    bool started_{};
    bool stopping_{};
    bool held_by_window_{};
    OrderedModelAheadStats stats_;
    std::thread thread_; // last
};

} // namespace eawr::scene
