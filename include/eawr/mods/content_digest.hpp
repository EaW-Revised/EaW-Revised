#pragma once
// Per-mod CONTENT digest (ModRecord::content): a SHA-256 of the mod's file bytes, made on one low-priority
// background thread and cached, so a save can tell that a mod kept its name but changed its files.
// The material is documented at ContentDigest (record.hpp); in short, one line
// "<relative path>\t<size>\t<sha256 of the file bytes>\n" per regular file, sorted by the relative path's UTF-8
// bytes, hashed again. Symbolic links and junctions are not followed (list_tree in fingerprint.hpp).
//
// Cost model. The digest reads every byte of the mod once, in 1 MiB chunks (memory O(1 MiB), no whole-file reads),
// so a cold run is limited by the disk's read speed. It happens at most once per tree state:
//  - the cache is keyed by the mod key plus the tree fingerprint (`tree` + `tree_files`); an unchanged tree is a
//    cache lookup with zero reads of mod files, a changed fingerprint invalidates it;
//  - a cancelled, failed or interrupted run keeps the per-file hashes it finished (relative path + size + mtime) in
//    a partial cache file, so the next run skips those files; a file whose size or mtime changed is hashed again;
//  - nothing here runs on the caller's thread: lookup() answers from memory and queues a job on a miss.
// A missing, corrupt or old-format cache file means "compute again", never an error. Nothing here throws to the
// caller; every failure is state `unavailable` with a note (and EAWR-MODS-0020 on the record, see apply()).
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

inline constexpr std::size_t kContentChunkBytes = 1024 * 1024;

struct ContentDigestOptions {
    // Where the digest and partial cache files live (user data: mods/cache/content). Empty: memory only.
    steam::fs::path cache_folder;
    std::uint64_t entry_cap = kDefaultTreeEntryCap;
    // After this many bytes hashed the worker sleeps `yield_for`, so a game that is loading still gets the disk.
    std::uint64_t yield_every_bytes = 64ull * 1024 * 1024;
    std::chrono::milliseconds yield_for{20};
    // The partial cache is rewritten at most this often while a job runs (and always on cancel, failure, shutdown).
    std::chrono::milliseconds partial_every{10'000};
    bool lower_priority = true;      // below-normal thread priority and background I/O
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
    bool stop_ = false;
};

} // namespace eawr::mods
