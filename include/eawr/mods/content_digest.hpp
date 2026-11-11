#pragma once
// Per-mod content identity: XXH3-128 per file, combined in sorted path order with a
// versioned domain and two seeded XXH3-128 hashes. The 64-hex result retains the
// existing serialized field width. Per-file size/mtime entries are persisted in user
// data and reused across tree changes; invalid caches are misses.
// This deliberately trusts path, size and mtime: same-size replacements with the
// same mtime, including edits within a coarse filesystem timestamp tick, can
// reuse a stale digest. This cache is not an integrity or anti-cheat check.
#include "eawr/mods/fingerprint.hpp"
#include "eawr/mods/record.hpp"

#include <chrono>
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

namespace eawr::mods {

namespace detail { class DigestPool; }

inline constexpr std::size_t kContentChunkBytes = 1024 * 1024;

struct ContentDigestOptions {
    // Where the digest and partial cache files live (user data: mods/cache/content). Empty: memory only.
    steam::fs::path cache_folder;
    std::uint64_t entry_cap = kDefaultTreeEntryCap;
    // Background reads pause after this many bytes.
    std::uint64_t yield_every_bytes = 64ull * 1024 * 1024;
    std::chrono::milliseconds yield_for{20};
    // Maximum interval between partial saves while files are completing.
    std::chrono::milliseconds partial_every{10'000};
    std::size_t worker_count = 0; // zero selects hardware count, capped at eight
    bool lower_priority = true;       // background priority for every hashing thread
    // background_io false (with lower_priority) lowers only the CPU priority and keeps I/O and memory priority normal. Windows
    // background I/O mode read a cold 7.6 GB mod in 248 s and 43.5 CPU-s, normal I/O priority in 40 s and 11.6 CPU-s
    // (same 2 threads, cache evicted between runs), so the mods service, which also pauses while a game runs, turns it off.
    bool background_io = true;
    // Polled between files by the hashing threads (about every pause_poll, never under a lock): while it answers true
    // nothing is read and the job stays pending. While paused the job also looks for a finished digest another
    // process wrote to the same cache folder, and takes it (same key, root, tree) without reading the mod. The mods
    // service pauses while a game runs: that game hashes the layers it mounts itself, and two readers of one
    // 25 GB mod only slow each other. A screen waiting for a digest (screen_wait_lease) ignores the pause.
    std::function<bool()> pause_while;
    std::chrono::milliseconds pause_poll{2000};
    // Test hooks, called on the worker thread. on_open: a mod file is about to be opened (relative path).
    // on_read: one chunk of that many bytes was read.
    std::function<void(std::string_view)> on_open;
    std::function<void(std::size_t)> on_read;
};

// What the job for one mod key has done, for tests and diagnostics.
struct ContentJobStats {
    bool known = false;
    bool finished = false;
    std::uint64_t files_total = 0;    // known after the listing pass
    std::uint64_t bytes_total = 0;
    std::uint64_t files_hashed = 0;   // files whose bytes this run read
    std::uint64_t files_reused = 0;   // files taken from the partial cache
    std::uint64_t bytes_read = 0;
    bool paused = false;              // waiting on ContentDigestOptions::pause_while right now
    bool adopted = false;             // finished by taking a digest another process cached; no file was read for it
};

class ContentDigests {
public:
    explicit ContentDigests(ContentDigestOptions options = {});
    ContentDigests(const ContentDigests&) = delete;
    ContentDigests& operator=(const ContentDigests&) = delete;
    ~ContentDigests();  // cancels the running job, keeps its partial cache, joins the thread

    // The digest state for this record's current tree fingerprint, without reading any mod file:
    // ready on a cache hit; pending when a job for this fingerprint runs or (request) was just queued;
    // otherwise unavailable (note empty when nothing was asked, else the reason). Idempotent per key + fingerprint.
    [[nodiscard]] ContentDigest lookup(const ModRecord& record, bool request);
    // Fills record.content from lookup(); adds EAWR-MODS-0020 once when the digest cannot be made.
    void apply(ModRecord& record, bool request);

    // Progress of the job for `key` (pending: files/bytes covered so far), or its result.
    [[nodiscard]] ContentDigest progress(const std::string& key) const;
    [[nodiscard]] ContentJobStats stats(const std::string& key) const;
    // Stops the job for `key` quickly (between chunks); it ends unavailable "cancelled" and keeps its partial cache.
    void cancel(const std::string& key);
    void cancel_all();
    // Waits up to `timeout` for the job of this record to end, then answers as lookup(record, false) does.
    // For tests and for the save path ("the digest if it is ready").
    [[nodiscard]] ContentDigest wait(const ModRecord& record, std::chrono::milliseconds timeout);

    // Keep this lease only while a screen is waiting for a digest. All digest
    // workers can then run at normal priority until the final lease is released.
    [[nodiscard]] static std::shared_ptr<void> screen_wait_lease();
    [[nodiscard]] static unsigned screen_waiter_count(); // diagnostic for lease tests

    // Synchronous reference path for tools and tests: list, hash and return in the caller's thread, no cache.
    [[nodiscard]] static ContentDigest compute_now(const steam::fs::path& root, std::uint64_t entry_cap = kDefaultTreeEntryCap);

private:
    struct Job;
    struct CacheEntry;
    void worker();
    void run(Job& job);
    [[nodiscard]] steam::fs::path cache_file(const std::string& key, bool partial) const;
    [[nodiscard]] std::unique_ptr<CacheEntry> load_cache_file(const std::string& key) const;   // no lock held
    [[nodiscard]] const CacheEntry* cached(const std::string& key, std::unique_lock<std::mutex>& lock);  // lock held on entry and exit

    ContentDigestOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    std::map<std::string, std::shared_ptr<Job>> jobs_;
    std::deque<std::shared_ptr<Job>> queue_;
    std::map<std::string, std::unique_ptr<CacheEntry>> cache_;
    std::thread thread_;
    std::shared_ptr<detail::DigestPool> pool_; // shared private blocking-I/O workers
    bool stop_ = false;
};

} // namespace eawr::mods
