#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/vfs/vfs_fwd.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace eawr::scene {

// What a reader does to a texture after decoding it. The result is the same whichever thread runs it; a reader only
// moves the work off the loader.
enum class TextureProcessing : std::uint8_t {
    none,
    // assets::extend_mip_chain(texture, ChainKind::colour): complete the mip chain of a texture that stops above 1x1
    // (#5989). A texture of a format it does not handle, or whose chain is complete, comes back as decoded.
    colour_mip_chain,
    // The same for a tangent-space normal map (ChainKind::normal: averaged normals are scaled back to unit length).
    normal_mip_chain,
    compensated_light_mip_chain, // single-level additive texture, with brightness-compensated lower levels
};

struct TexturePrefetchOptions final {
    // 0 = by policy: solid state gets clamp(cores - 2, 1, 8) readers, every other volume exactly one. Nonzero = that many
    // readers (tests, A/B).
    unsigned fixed_readers{};
    bool solid_state{}; // the textures' volume reported no seek penalty
    // Cap on the bytes of textures decoded ahead and not yet asked for, plus the estimate (file size x 2) of those being
    // decoded. 0 = min(64 MiB, physical memory / 64). One texture larger than the cap still runs, alone.
    std::uint64_t window_bytes{};
    // Called on a reader thread before it decodes a path; tests use it to block or count decodes.
    std::function<void(std::string_view)> decode_hook;
};

struct TexturePrefetchStats final {
    std::uint64_t planned{};          // distinct paths offered
    std::uint64_t decoded{};          // paths a reader finished (a failed decode counts)
    std::uint64_t failed{};           // reader decodes that failed (the loader decodes them itself and reports the failure)
    std::uint64_t served_ready{};     // loader asked after the reader had finished
    std::uint64_t served_inflight{};  // loader asked while a reader was decoding it (it waited)
    std::uint64_t served_unstarted{}; // loader asked before any reader took it (the loader decoded it itself)
    std::uint64_t unplanned{};        // loader asked for a path nobody offered, or one already served
    std::uint64_t unrequested{};      // decoded ahead and never asked for
    std::uint64_t skipped{};          // holds or queue slots released because the loader moved past the hinting step
    std::uint64_t readers{};          // reader threads
    std::uint64_t readers_peak{};     // most readers decoding at once
    std::uint64_t window_bytes{};
    std::uint64_t held_bytes_peak{};  // most bytes held or in flight (estimate) at once
    std::uint64_t reader_decode_ms{}; // summed wall time of the readers' decodes (they run beside the loader)
    std::uint64_t wait_ms{};          // wall time the loader waited for a texture still being decoded
    std::uint64_t chained{};          // textures a reader gave a mip chain (the two chain processings)
    std::uint64_t chain_added_bytes{};// bytes those chains added
    std::uint64_t chain_ms{};         // summed wall time of the readers' chain generation (inside reader_decode_ms)
    std::uint64_t chained_by_loader{};// chains the loader had to generate itself (hint dropped, never offered or not yet started)
};

// Decode-ahead for the textures a load asks for in a known order. The loader keeps its own order and its own cache; this
// class only makes the decode of a path happen earlier, on reader threads (assets::load_texture: file read and decode, no
// Godot object, no scene tree, no GPU). acquire() returns exactly what assets::load_texture(vfs, path) would return, with
// the offered TextureProcessing applied: a decode that failed on a reader is thrown away and the loader decodes the path itself, so every diagnostic is the
// loader's own. `advance` drops what the loader moved past.
class TexturePrefetch final {
public:
    TexturePrefetch(const vfs::Vfs& filesystem, TexturePrefetchOptions options);
    ~TexturePrefetch();
    TexturePrefetch(const TexturePrefetch&) = delete;
    TexturePrefetch& operator=(const TexturePrefetch&) = delete;

    // The loader will probably ask for this path soon; `step` is the ordinal of the loader step that will ask (used by
    // `advance`). Any thread. Duplicates (ASCII case-insensitive, per processing) are ignored; a missing file stays the
    // loader's. The loader must ask with the same `processing` it was offered with.
    void offer(std::string_view logical_path, std::uint64_t step, TextureProcessing processing = TextureProcessing::none);
    // The loader's texture for `logical_path`: the reader's result when it is ready, the reader's result after waiting
    // when one is decoding it, otherwise assets::load_texture on the calling thread, then `processing`. Call from the
    // loader's thread.
    [[nodiscard]] core::Result<assets::Texture> acquire(std::string_view logical_path, TextureProcessing processing = TextureProcessing::none);
    // The loader reached `step`: every hint offered for an earlier step that nobody asked for is dropped (a held decode
    // is freed, a queued path is never read), so readers are not throttled by a decode nobody will use.
    void advance(std::uint64_t step);
    // Stops the readers (each finishes at most its current file), joins them, drops every hold. Idempotent. Hints offered
    // afterwards are ignored and acquire() decodes on the calling thread.
    void finish();
    [[nodiscard]] TexturePrefetchStats stats() const;
    // Held entries and charged bytes right now (tests).
    [[nodiscard]] std::size_t held_count() const;
    [[nodiscard]] std::uint64_t charged_bytes() const;
    [[nodiscard]] static unsigned policy_readers(bool solid_state, unsigned logical_processors) noexcept;

private:
    enum class State : std::uint8_t { queued, decoding, done, taken };
    struct Entry final {
        State state{State::queued};
        std::string path; // as first offered
        TextureProcessing processing{TextureProcessing::none};
        std::uint64_t step{};
        std::uint64_t cost{}; // estimate while queued/decoding, the decoded payload once done
        std::optional<assets::Texture> held;
    };
    void reader();

    const vfs::Vfs* filesystem_;
    TexturePrefetchOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, Entry> entries_; // key: lower-case path
    std::deque<std::string> queue_;        // keys, FIFO
    std::uint64_t charged_{};              // bytes held or (estimated) decoding
    unsigned decoding_{};
    bool stopping_{};
    TexturePrefetchStats stats_;
    std::vector<std::thread> threads_; // last
};

} // namespace eawr::scene
