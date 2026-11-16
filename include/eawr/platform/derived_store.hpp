#pragma once

// A small on-disk store of facts derived from files (docs/design: unit-table model facts, kind 1).
//
// The store is a folder of immutable segment files. A process never changes a segment in place: it writes the records it
// derived as a new, uniquely named segment (temporary name, then rename). Readers load every segment of the current
// format and epoch, check each record frame and use a record only when its stamp equals the caller's current stamp of
// the source file. Only an index (key -> stamp and place in a segment file) stays in memory; a record body is read from
// its segment file when it is asked for and its frame is checked again then, so a store near its size cap costs index
// memory only. Records not yet written (the open segment) are the only bodies held in memory, until the next flush. Anything else (a bad header, a torn or flipped frame, an old epoch, a changed source, a vanished
// segment) is a miss and the caller derives the fact again, so the store can only save work, never change a result.
//
// Trust model: the same as the mod content digest (docs/mod-content-digest.md): the stamp is path, size and modification
// time; a same-size replacement with an unchanged timestamp is not seen, and a forged store with matching keys is
// trusted. It is not an integrity or anti-cheat check.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::platform {

using DerivedKey = std::array<std::uint8_t, 16>;

// What identifies the exact bytes a record was derived from: the container file (the archive, or the loose file itself)
// by size and modification time, and the entry's place inside it.
struct DerivedStamp final {
    std::uint64_t container_size{};
    std::int64_t container_mtime{}; // the filesystem's own tick count, compared for equality only
    std::uint64_t entry_offset{};
    std::uint64_t entry_size{};
    friend bool operator==(const DerivedStamp&, const DerivedStamp&) = default;
};

struct DerivedStoreOptions final {
    std::string folder_utf8;               // empty: memory only (still correct, nothing persists)
    std::array<std::uint8_t, 16> epoch{};  // a segment of another epoch is ignored (engine build, extractor version)
    std::uint32_t facts_format{1};
    std::uint64_t max_total_bytes{512ULL << 20U};
    std::uint64_t max_memory_only_bytes{64ULL << 20U}; // with no folder the records only live in memory: a small cap
    std::uint64_t max_record_bytes{8ULL << 20U};
    // Segments of other builds (another epoch or facts format) share the folder. This store never merges or rewrites them.
    // It deletes one only when it is older than foreign_keep_age, or when the newer ones already hold max_foreign_bytes
    // (newest kept first), so two builds that alternate on one folder keep each other's records.
    std::chrono::milliseconds foreign_keep_age{std::chrono::hours(24 * 14)};
    std::uint64_t max_foreign_bytes{512ULL << 20U};
    // Compaction merges the segments this process wrote, plus current-epoch segments it loaded at open that are at least
    // this old. A younger loaded segment may be one a concurrent process of the same build is still reading, so it is left.
    std::chrono::milliseconds adopt_age{std::chrono::hours(1)};
    std::uint64_t flush_bytes{8ULL << 20U};
    std::chrono::milliseconds flush_interval{10'000};
    unsigned compact_after_segments{8};
    bool writer_thread{true};              // false: records are written only by flush()
};

struct DerivedStoreStats final {
    std::uint64_t segments_loaded{}, segments_bad{}, segments_old{}, segments_unreadable{};
    std::uint64_t frames_truncated{}, frames_corrupt{};
    std::uint64_t records_indexed{}, bytes_loaded{};
    std::uint64_t lookups{}, hits{}, misses_absent{}, misses_stale{}, misses_unreadable{};
    std::uint64_t puts{}, puts_refused{}, flushes{}, flush_failures{}, bytes_written{};
    std::uint64_t compactions{}, segments_removed{}, tmp_removed{};
    std::uint64_t index_entries{};    // records the index holds now
    std::uint64_t bytes_resident{};   // record bytes held in memory now (records not yet written; nothing else)
    std::uint64_t bytes_on_disk{};    // bytes of the segment files this process knows (current epoch)
    std::uint64_t segments_on_disk{}; // the same segments, counted
    std::uint64_t segments_foreign{}, bytes_foreign{}; // other builds' segments left in the folder (not read, not merged)
    std::uint64_t open_us{}, compact_us{};              // the folder scan at open, and the one compaction (microseconds)
    struct Kind final { std::uint8_t kind{}; std::uint64_t records{}, body_bytes{}; };
    std::vector<Kind> kinds;          // per record kind: records indexed and their body bytes (sorted by kind)
    bool opened{}, disabled{};
};

// One line of key=value fields for logs and measurement scripts ("derived-store: ...").
[[nodiscard]] std::string format_derived_store_stats(const DerivedStoreStats& stats);

// `epoch` for a build: the first 16 bytes of SHA-256(domain, newline, build identity).
[[nodiscard]] std::array<std::uint8_t, 16> derived_epoch(std::string_view domain, std::string_view build);
// A record key: the first 16 bytes of SHA-256 over the length-prefixed parts.
[[nodiscard]] DerivedKey derived_key(std::string_view domain, std::span<const std::string_view> parts);

class DerivedStore final {
public:
    explicit DerivedStore(DerivedStoreOptions options);
    ~DerivedStore();
    DerivedStore(const DerivedStore&) = delete;
    DerivedStore& operator=(const DerivedStore&) = delete;

    // Reads the segments on a background thread so a later lookup does not pay for it; lookups wait for it to finish.
    void begin_open();

    // The record body when this store holds a record for (kind, key) whose stamp equals `stamp`.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> lookup(std::uint8_t kind, const DerivedKey& key,
                                                                  const DerivedStamp& stamp);
    // lookup() without copying the body.
    [[nodiscard]] bool contains(std::uint8_t kind, const DerivedKey& key, const DerivedStamp& stamp);
    // Queues a record for the next segment. A record above max_record_bytes is refused. With no folder (memory only) a
    // record that would take the memory past max_memory_only_bytes is refused too.
    // True when the record was accepted; false when the store refused it (over the record size limit, or a memory-only store
    // at its cap): the caller decodes again next time.
    bool put(std::uint8_t kind, const DerivedKey& key, const DerivedStamp& stamp, std::span<const std::uint8_t> body);
    // Writes the queued records now (and compacts when the folder has grown past its bounds). Waits for a flush that is
    // already running. Does nothing, and does not open the folder, when no record is queued. Never throws; a segment
    // that cannot be written is dropped (counted in flush_failures), so a read-only folder does not accumulate memory.
    void flush();
    // flush() and stop the writer thread. The store stays usable (a later put starts the thread again).
    void finish();

    [[nodiscard]] DerivedStoreStats stats() const;
    [[nodiscard]] const DerivedStoreOptions& options() const noexcept { return options_; }

    struct State; // implementation detail (opaque)

private:
    void ensure_open();
    void open_folder();
    void write_pending(std::unique_lock<std::mutex>& lock);
    void writer_loop();
    void compact(std::unique_lock<std::mutex>& lock);
    DerivedStoreOptions options_;
    std::unique_ptr<State> state_;
};

} // namespace eawr::platform
