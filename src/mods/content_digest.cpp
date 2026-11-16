#include "eawr/mods/content_digest.hpp"

#include "eawr/core/sha256.hpp"
#include "eawr/core/text.hpp"
#include "eawr/platform/native_read.hpp"
#include "thread_priority.hpp"
#include "profile_io.hpp"

#define XXH_INLINE_ALL
#include "../../third_party/xxhash/xxhash.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>


namespace eawr::mods {

namespace fs = steam::fs;

namespace detail {
// Digest reads can block on slow disks, so they must not occupy simulation
// workers. All live ContentDigests instances share this eight-thread pool.
class DigestPool {
public:
    DigestPool() {
        try {
            for (std::size_t i = 0; i < 8; ++i) threads_.emplace_back([this] { worker(); });
        } catch (...) {
            shutdown();
            throw;
        }
    }
    ~DigestPool() { shutdown(); }
    DigestPool(const DigestPool&) = delete;
    DigestPool& operator=(const DigestPool&) = delete;

    void submit(std::function<void()> work) {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(work));
        }
        wake_.notify_one();
    }
private:
    void worker() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            try { task(); } catch (...) { /* a task must not terminate the shared pool */ }
        }
    }
    void shutdown() {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        wake_.notify_all();
        for (auto& thread : threads_) if (thread.joinable()) thread.join();
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> threads_;
    bool stopping_ = false;
};

std::shared_ptr<DigestPool> acquire_digest_pool() {
    struct Registry { std::mutex mutex; std::weak_ptr<DigestPool> weak; };
    // The registry outlives static viewer state, whose destructor joins digest jobs.
    static auto* registry = new Registry;
    std::lock_guard lock(registry->mutex);
    auto pool = registry->weak.lock();
    if (!pool) { pool = std::make_shared<DigestPool>(); registry->weak = pool; }
    return pool;
}
} // namespace detail


namespace {

constexpr std::string_view kDigestFormat = "eawr-mod-content-digest";
constexpr std::string_view kPartialFormat = "eawr-mod-content-partial";
// The digest format remains v2; discard earlier in-development cache entries.
constexpr int kCacheVersion = 3;
std::atomic<unsigned> screen_waiters{0};

bool is_hex(std::string_view text, std::size_t length) {
    return text.size() == length && std::all_of(text.begin(), text.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string hex128(XXH128_hash_t hash) {
    XXH128_canonical_t canonical;
    XXH128_canonicalFromHash(&canonical, hash);
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (const auto byte : canonical.digest) { out += digits[byte >> 4]; out += digits[byte & 15]; }
    return out;
}

bool write_atomically(const fs::path& target, const std::string& text) {
    std::error_code error;
    fs::create_directories(target.parent_path(), error);
    // Each writer gets its own temporary, including concurrent writers in this process.
    static std::atomic<std::uint64_t> serial{0};
    fs::path temporary = target;
    temporary += ".tmp-" + std::to_string(current_process_id()) + "-" + std::to_string(serial++);
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        const std::string envelope = hex128(XXH3_128bits(text.data(), text.size())) + "\n" + text;
        out.write(envelope.data(), static_cast<std::streamsize>(envelope.size()));
        out.flush();
        if (!out) {
            out.close();
            fs::remove(temporary, error);
            return false;
        }
    }
    // Another process may be reading the target right now (Windows refuses to replace an open file): try a few times.
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (detail::replace_profile_file(temporary, target)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    fs::remove(temporary, error);
    return false;
}

std::optional<std::string> read_whole(const fs::path& cache_path) {
    std::ifstream in(cache_path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() < 33 || text[32] != '\n') return std::nullopt;
    const std::string_view body(text.data() + 33, text.size() - 33);
    if (text.compare(0, 32, hex128(XXH3_128bits(body.data(), body.size()))) != 0) return std::nullopt;
    return std::string(body);
}

struct PartialFile {
    std::uint64_t size = 0;
    std::string mtime;   // decimal ticks; kept as text, a double does not hold a 100 ns tick count exactly
    std::string sha256;
    bool adopted = false;   // came from the partial file after this job started (another process hashed it)
};

} // namespace

struct ContentDigests::CacheEntry {
    std::string root;
    std::string tree;
    std::uint64_t tree_files = 0;
    std::string sha256;
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
};

struct ContentDigests::Job {
    std::string key;
    std::string root;
    std::string tree;
    std::uint64_t tree_files = 0;
    std::atomic<bool> cancel{false};
    std::atomic<std::uint64_t> files_done{0}, bytes_done{0}, files_total{0}, bytes_total{0};
    std::atomic<std::uint64_t> files_hashed{0}, files_reused{0}, files_adopted{0}, bytes_read{0};
    std::atomic<bool> paused{false}, adopted{false};
    std::atomic<std::size_t> workers{0};
    std::atomic<int> media{0};   // 0 unknown, 1 ssd, 2 hdd
    // Guarded by ContentDigests::mutex_.
    bool finished = false;
    ContentDigest result;
};

std::shared_ptr<void> ContentDigests::screen_wait_lease() {
    static int sentinel = 0;
    ++screen_waiters;
    return std::shared_ptr<void>(&sentinel, [](void*) { --screen_waiters; });
}

unsigned ContentDigests::screen_waiter_count() { return screen_waiters.load(); }

ContentDigests::ContentDigests(ContentDigestOptions options) : options_(std::move(options)) {}

ContentDigests::~ContentDigests() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        for (auto& [key, job] : jobs_) job->cancel = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

fs::path ContentDigests::cache_file(const std::string& key, bool partial) const {
    if (options_.cache_folder.empty()) return {};
    const std::string name = core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(key.data()), key.size())).substr(0, 32);
    return options_.cache_folder / (std::string("content-") + name + (partial ? ".partial.json" : ".json"));
}

std::unique_ptr<ContentDigests::CacheEntry> ContentDigests::load_cache_file(const std::string& key) const {
    std::unique_ptr<CacheEntry> entry;
    const fs::path path = cache_file(key, false);
    if (!path.empty()) {
        // An unreadable, corrupt or old-format file is a miss; the next job overwrites it.
        if (const auto text = read_whole(path)) {
            if (const auto root = steam::parse_json(*text); root && root->kind == steam::JsonValue::Kind::object) {
                const std::string* format = root->string_member("format");
                const std::string* stored_key = root->string_member("key");
                const std::string* tree = root->string_member("tree");
                const std::string* stored_root = root->string_member("root");
                const std::string* sha = root->string_member("sha256");
                const steam::JsonValue* version = root->member("version");
                const steam::JsonValue* tree_files = root->member("tree_files");
                const steam::JsonValue* files = root->member("files");
                const steam::JsonValue* bytes = root->member("bytes");
                const auto count = [](const steam::JsonValue* v) {
                    return v != nullptr && v->kind == steam::JsonValue::Kind::number && v->number >= 0 && v->number <= 9007199254740992.0;
                };
                if (format && *format == kDigestFormat && version && version->kind == steam::JsonValue::Kind::number
                    && version->number == kCacheVersion && stored_key && *stored_key == key && stored_root && tree && sha && is_hex(*tree, 64)
                    && is_hex(*sha, 64) && count(tree_files) && count(files) && count(bytes)) {
                    entry = std::make_unique<CacheEntry>();
                    entry->root = *stored_root;
                    entry->tree = *tree;
                    entry->tree_files = static_cast<std::uint64_t>(tree_files->number);
                    entry->sha256 = *sha;
                    entry->files = static_cast<std::uint64_t>(files->number);
                    entry->bytes = static_cast<std::uint64_t>(bytes->number);
                }
            }
        }
    }
    return entry;
}

const ContentDigests::CacheEntry* ContentDigests::cached(const std::string& key, std::unique_lock<std::mutex>& lock) {
    if (auto found = cache_.find(key); found != cache_.end()) return found->second.get();
    // Read and parse the file without holding the lock; another thread may have filled the slot meanwhile.
    lock.unlock();
    std::unique_ptr<CacheEntry> entry = load_cache_file(key);
    lock.lock();
    auto [slot, inserted] = cache_.try_emplace(key);
    if (inserted) slot->second = std::move(entry);
    return slot->second.get();
}

ContentDigest ContentDigests::lookup(const ModRecord& record, bool request) {
    ContentDigest out;
    const auto unavailable = [&](std::string note) {
        out.state = ContentDigestState::unavailable;
        out.note = std::move(note);
        return out;
    };
    if (record.state == ModState::missing || record.root.empty()) return unavailable("folder missing");
    const Fingerprint& print = record.fingerprint;
    if (print.tree.empty()) return unavailable("tree not walked");
    if (print.tree_capped) return unavailable("capped walk");
    if (record.has_warning(codes::tree_walk_incomplete)) return unavailable("incomplete walk");
    if (print.tree_files == 0) return unavailable("no files");

    std::unique_lock lock(mutex_);
    if (const CacheEntry* entry = cached(record.key, lock); entry != nullptr && entry->root == record.root && entry->tree == print.tree && entry->tree_files == print.tree_files) {
        out.state = ContentDigestState::ready;
        out.sha256 = entry->sha256;
        out.files = entry->files;
        out.bytes = entry->bytes;
        return out;
    }
    if (const auto found = jobs_.find(record.key); found != jobs_.end()) {
        const std::shared_ptr<Job>& job = found->second;
        const bool same = job->root == record.root && job->tree == print.tree && job->tree_files == print.tree_files;
        if (same && !(job->finished && job->result.note == "cancelled" && request)) {
            if (job->finished) return job->result;
            out.state = ContentDigestState::pending;
            out.files = job->files_done;
            out.bytes = job->bytes_done;
            return out;
        }
        if (!same) {
            job->cancel = true;   // another tree state of the same mod: the old job's result is worthless
            if (!job->finished) done_.notify_all();
        }
    }
    if (!request) return unavailable({});

    if (!thread_.joinable()) {
        try {
            thread_ = std::thread([this] { worker(); });
        } catch (...) {
            return unavailable("internal error: worker thread");   // never an exception out of lookup()
        }
    }
    auto job = std::make_shared<Job>();
    job->key = record.key;
    job->root = record.root;
    job->tree = print.tree;
    job->tree_files = print.tree_files;
    jobs_[record.key] = job;
    queue_.push_back(job);
    wake_.notify_all();
    out.state = ContentDigestState::pending;
    return out;
}

void ContentDigests::apply(ModRecord& record, bool request) {
    record.content = lookup(record, request);
    if (request && record.content.state == ContentDigestState::unavailable && !record.content.note.empty() && !record.has_warning(codes::content_digest_unavailable)) {
        record.warnings.push_back({std::string(codes::content_digest_unavailable), "no content digest for this folder: " + record.content.note});
    }
}

ContentDigest ContentDigests::progress(const std::string& key) const {
    std::lock_guard lock(mutex_);
    ContentDigest out;
    const auto found = jobs_.find(key);
    if (found == jobs_.end()) return out;
    const Job& job = *found->second;
    if (job.finished) return job.result;
    out.state = ContentDigestState::pending;
    out.files = job.files_done;
    out.bytes = job.bytes_done;
    return out;
}

ContentJobStats ContentDigests::stats(const std::string& key) const {
    std::lock_guard lock(mutex_);
    ContentJobStats out;
    const auto found = jobs_.find(key);
    if (found == jobs_.end()) return out;
    const Job& job = *found->second;
    out.known = true;
    out.finished = job.finished;
    out.files_total = job.files_total;
    out.bytes_total = job.bytes_total;
    out.files_hashed = job.files_hashed;
    out.files_reused = job.files_reused;
    out.files_adopted = job.files_adopted;
    out.bytes_read = job.bytes_read;
    out.workers = job.workers;
    out.media = job.media == 2 ? "hdd" : job.media == 1 ? "ssd" : "unknown";
    out.paused = job.paused;
    out.adopted = job.adopted;
    return out;
}

void ContentDigests::cancel(const std::string& key) {
    std::lock_guard lock(mutex_);
    if (const auto found = jobs_.find(key); found != jobs_.end()) found->second->cancel = true;
}

void ContentDigests::cancel_all() {
    std::lock_guard lock(mutex_);
    for (auto& [key, job] : jobs_) job->cancel = true;
}

ContentDigest ContentDigests::wait(const ModRecord& record, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock(mutex_);
    while (true) {
        const auto found = jobs_.find(record.key);
        if (found == jobs_.end() || found->second->finished) break;
        if (done_.wait_until(lock, deadline) == std::cv_status::timeout) break;
    }
    lock.unlock();
    return lookup(record, false);
}

ContentDigest ContentDigests::compute_now(const fs::path& root, std::uint64_t entry_cap) {
    ContentDigestOptions options;
    options.entry_cap = entry_cap;
    options.yield_for = std::chrono::milliseconds(0);
    options.lower_priority = false;
    ContentDigests digests(options);
    const TreeFingerprint print = tree_fingerprint(root, entry_cap);
    ModRecord record;
    record.key = "compute-now";
    record.root = steam::path_to_utf8(root);
    record.fingerprint.tree = print.tree;
    record.fingerprint.tree_files = print.files;
    record.fingerprint.tree_capped = print.capped;
    if (print.incomplete) record.warnings.push_back({std::string(codes::tree_walk_incomplete), {}});
    if (print.tree.empty()) record.state = ModState::missing;
    (void)digests.lookup(record, true);
    return digests.wait(record, std::chrono::hours(24 * 365));
}

void ContentDigests::worker() {
    while (true) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        // Mods never abort: whatever goes wrong inside a job ends that job as unavailable.
        std::string problem;
        try {
            run(*job);
        } catch (const std::exception& error) {
            problem = std::string("internal error: ") + error.what();
        } catch (...) {
            problem = "internal error";
        }
        {
            std::lock_guard lock(mutex_);
            if (!job->finished) {
                job->result = {};
                job->result.note = problem.empty() ? "internal error" : problem;
                job->finished = true;
            }
        }
        done_.notify_all();
    }
}

void ContentDigests::run(Job& job) {
    const auto fail = [&](std::string note) {
        ContentDigest result;
        result.note = std::move(note);
        result.files = job.files_done;
        result.bytes = job.bytes_done;
        std::lock_guard lock(mutex_);
        job.result = std::move(result);
        job.finished = true;
    };
    if (job.cancel) return fail("cancelled");

    const fs::path root = steam::path_from_utf8(job.root);
    const TreeListing listing = list_tree(root, options_.entry_cap);
    if (job.cancel) return fail("cancelled");
    if (!listing.root_exists) return fail("folder missing");
    if (listing.capped) return fail("capped walk");
    if (listing.incomplete) return fail("incomplete walk");
    if (listing.files.empty()) return fail("no files");
    {
        // The tree must still be the one the caller fingerprinted; otherwise a result stored under that
        // fingerprint would describe other bytes.
        std::string text;
        for (const TreeFile& file : listing.files) text += file.relative + '\t' + std::to_string(file.size) + '\t' + std::to_string(file.mtime) + '\n';
        const std::string tree = core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        if (tree != job.tree || listing.files.size() != job.tree_files) return fail("folder changed since it was scanned");
    }
    std::uint64_t total = 0;
    for (const TreeFile& file : listing.files) total += file.size;
    job.files_total = listing.files.size();
    job.bytes_total = total;

    // Per-file hashes another run or process left in the partial file. Only a file with the same size and mtime counts.
    const fs::path partial_path = cache_file(job.key, true);
    std::error_code cache_error;
    const fs::path complete_path = cache_file(job.key, false);
    const bool corrupt_complete = !complete_path.empty() && fs::exists(complete_path, cache_error) && !read_whole(complete_path);
    std::unordered_map<std::string, std::size_t> listed;   // relative path -> listing index
    listed.reserve(listing.files.size());
    for (std::size_t i = 0; i < listing.files.size(); ++i) listed.emplace(listing.files[i].relative, i);
    const auto read_partial = [&]() -> std::vector<std::pair<std::string, PartialFile>> {
        std::vector<std::pair<std::string, PartialFile>> out;
        if (partial_path.empty() || corrupt_complete) return out;
        const auto text = read_whole(partial_path);
        if (!text) return out;
        const auto parsed = steam::parse_json(*text);
        const std::string* format = parsed ? parsed->string_member("format") : nullptr;
        const std::string* key = parsed ? parsed->string_member("key") : nullptr;
        const std::string* stored_root = parsed ? parsed->string_member("root") : nullptr;
        const steam::JsonValue* version = parsed ? parsed->member("version") : nullptr;
        const steam::JsonValue* list = parsed ? parsed->member("files") : nullptr;
        if (!(format && *format == kPartialFormat && key && *key == job.key && stored_root && *stored_root == job.root && version
              && version->kind == steam::JsonValue::Kind::number && version->number == kCacheVersion
              && list && list->kind == steam::JsonValue::Kind::array)) return out;
        for (const steam::JsonValue& item : list->array) {
            if (item.kind != steam::JsonValue::Kind::array || item.array.size() != 4) return {};
            const auto& a = item.array;
            if (a[0].kind != steam::JsonValue::Kind::string || a[1].kind != steam::JsonValue::Kind::number
                || a[2].kind != steam::JsonValue::Kind::string || a[3].kind != steam::JsonValue::Kind::string
                || !is_hex(a[3].string, 32) || !(a[1].number >= 0 && a[1].number <= 9007199254740992.0)) return {};
            const auto at = listed.find(a[0].string);
            if (at == listed.end()) continue;
            const TreeFile& file = listing.files[at->second];
            const auto size = static_cast<std::uint64_t>(a[1].number);
            if (size != file.size || a[2].string != std::to_string(file.mtime)) continue;   // not the bytes this job lists
            out.emplace_back(a[0].string, PartialFile{size, a[2].string, a[3].string, false});
        }
        return out;
    };
    // Guarded by earlier_mutex: entries valid for this listing, from the partial file (at start and again while the job runs).
    std::mutex earlier_mutex;
    std::unordered_map<std::string, PartialFile> earlier;
    // Looks at the partial file again when its size or mtime changed (the other process saved), one thread at a time, at most every
    // partial_refresh; `force` skips the interval (partial saves merge before they write). Entries only ever get added.
    using Stamp = std::pair<std::uintmax_t, std::int64_t>;
    constexpr Stamp kNoStamp{~std::uintmax_t{}, 0};
    std::mutex refresh_mutex;
    auto next_refresh = std::chrono::steady_clock::now() + options_.partial_refresh;
    const auto stamp_of = [&]() -> Stamp {
        std::error_code ec;
        const fs::directory_entry entry(partial_path, ec);
        if (ec) return kNoStamp;
        const auto size = entry.file_size(ec);
        if (ec) return kNoStamp;
        const auto time = entry.last_write_time(ec);
        if (ec) return kNoStamp;
        return {size, static_cast<std::int64_t>(time.time_since_epoch().count())};
    };
    Stamp partial_stamp = partial_path.empty() ? kNoStamp : stamp_of();
    for (auto& [name, entry] : read_partial()) earlier.emplace(std::move(name), std::move(entry));   // after the stamp: a save in between is seen again
    const auto refresh_earlier = [&](bool force) {
        if (partial_path.empty()) return;
        std::unique_lock refresh_lock(refresh_mutex, std::try_to_lock);
        if (!refresh_lock) return;
        const auto now = std::chrono::steady_clock::now();
        if (!force && now < next_refresh) return;
        next_refresh = now + options_.partial_refresh;
        const Stamp stamp = stamp_of();
        if (stamp == partial_stamp || stamp == kNoStamp) return;
        partial_stamp = stamp;
        auto fresh = read_partial();   // no lock held while reading
        std::lock_guard lock(earlier_mutex);
        for (auto& [name, entry] : fresh) {
            entry.adopted = true;
            earlier.emplace(std::move(name), std::move(entry));
        }
    };

    // Each slot belongs to one worker; canonical aggregation below always follows listing order.
    std::vector<std::string> hashes(listing.files.size());
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex error_mutex, hook_mutex;
    std::string problem;
    const auto set_problem = [&](std::string message) {
        std::lock_guard lock(error_mutex);
        if (!failed.exchange(true)) problem = std::move(message);
    };
    std::mutex hashes_mutex;
    auto last_partial = std::chrono::steady_clock::now();
    const auto save_partial = [&] {
        if (partial_path.empty()) return;
        // Another process may have saved results since the last look: merge them in rather than replace them. The rename below is
        // atomic; a save that lands between this read and the rename is lost to the later writer, never torn.
        refresh_earlier(true);
        std::string text = "{\"format\": " + steam::json_quote(kPartialFormat) + ", \"version\": " + std::to_string(kCacheVersion)
            + ", \"key\": " + steam::json_quote(job.key) + ", \"root\": " + steam::json_quote(job.root) + ", \"files\": [";
        bool first = true;
        std::lock_guard earlier_lock(earlier_mutex);
        for (std::size_t i = 0; i < hashes.size(); ++i) {
            const auto& file = listing.files[i];
            const std::string* sha = hashes[i].empty() ? nullptr : &hashes[i];
            if (!sha) {
                // Not reached by this job yet, but another process finished it: keep that result in the file.
                const auto other = earlier.find(file.relative);
                if (other == earlier.end()) continue;
                sha = &other->second.sha256;
            }
            text += first ? "\n" : ",\n";
            first = false;
            text += "[" + steam::json_quote(file.relative) + ", " + std::to_string(file.size) + ", \"" + std::to_string(file.mtime)
                + "\", \"" + *sha + "\"]";
        }
        text += "\n]}\n";
        (void)write_atomically(partial_path, text);
    };
    // Pause (see ContentDigestOptions::pause_while) and "finished elsewhere": decided by one thread at a time, at most
    // once per pause_poll, so eight workers do not each read the same small state files.
    std::mutex pause_mutex;
    auto next_pause_check = std::chrono::steady_clock::now();
    bool pause_state = false;
    std::atomic<bool> adopt{false};
    std::mutex adopt_mutex;
    CacheEntry adopted_entry;   // valid once adopt is true
    const auto try_adopt = [&] {
        std::lock_guard lock(adopt_mutex);
        if (adopt) return;
        const auto entry = load_cache_file(job.key);
        if (!entry || entry->root != job.root || entry->tree != job.tree || entry->tree_files != job.tree_files) return;
        adopted_entry = *entry;
        adopt = true;
    };
    const auto paused = [&] {
        if (!options_.pause_while || screen_waiters.load() != 0) {
            job.paused = false;
            return false;
        }
        std::lock_guard lock(pause_mutex);
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_pause_check) {
            pause_state = options_.pause_while();
            next_pause_check = now + options_.pause_poll;
            if (pause_state) try_adopt();
        }
        job.paused = pause_state;
        return pause_state;
    };
    // One reader on a rotational disk (see ContentDigestOptions::rotational_worker_count); everything else keeps the usual count.
    platform::StorageMedia media = platform::StorageMedia::unknown;
    if (options_.is_rotational) media = options_.is_rotational(root) ? platform::StorageMedia::rotational : platform::StorageMedia::solid_state;
    else media = platform::storage_media_of(root);
    const bool rotational = media == platform::StorageMedia::rotational;
    job.media = rotational ? 2 : media == platform::StorageMedia::solid_state ? 1 : 0;
    std::size_t base_count = options_.worker_count ? options_.worker_count : std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
    if (rotational && options_.rotational_worker_count) base_count = std::min(base_count, options_.rotational_worker_count);
    const auto count = std::max<std::size_t>(1, std::min<std::size_t>({8, base_count, listing.files.size()}));
    job.workers = count;
    // On a rotational disk the files are read in on-disk order (ascending first extent, listing index breaks ties): one sweep of
    // the disk instead of a seek per file. Files with no known position (0) come first. Never changes the digest, which is
    // aggregated in sorted path order below.
    std::vector<std::size_t> read_order;   // empty = listing order
    if (rotational && options_.disk_order_on_rotational && listing.files.size() > 1) {
        std::vector<std::pair<std::uint64_t, std::size_t>> positions(listing.files.size());
        // The query opens each file (attributes only), so it obeys the same pause as the workers: no mod file is touched while
        // pause_while holds, and a digest another process finishes meanwhile (adopt) makes the order unnecessary.
        for (std::size_t i = 0; i < positions.size() && !adopt; ++i) {
            while (!job.cancel && !adopt && paused()) std::this_thread::sleep_for(std::min(options_.pause_poll, std::chrono::milliseconds(250)));
            if (job.cancel) return fail("cancelled");
            if (adopt) break;
            const fs::path path = root / steam::path_from_utf8(listing.files[i].relative);
            positions[i] = {options_.physical_offset ? options_.physical_offset(path) : platform::first_physical_offset(path), i};
        }
        if (!adopt) {
            std::sort(positions.begin(), positions.end());
            read_order.reserve(positions.size());
            for (const auto& entry : positions) read_order.push_back(entry.second);
        }
    }
    const auto work = [&] {
        bool background = false;
        const auto update_priority = [&] {
            const bool wanted = options_.lower_priority && screen_waiters.load() == 0;
            if (wanted != background) { set_hash_thread_background(wanted, options_.background_io); background = wanted; }
        };
        struct RestorePriority {
            bool& background;
            ~RestorePriority() { if (background) set_hash_thread_background(false); }
        } restore_priority{background};
        try {
            update_priority();
            std::uint64_t since_yield = 0;
            std::vector<std::byte> buffer(kContentChunkBytes);
            while (!job.cancel && !failed && !adopt) {
                // Between files only: a paused worker holds no mod file open (Steam may replace files meanwhile).
                while (!job.cancel && !failed && !adopt && paused())
                    std::this_thread::sleep_for(std::min(options_.pause_poll, std::chrono::milliseconds(250)));
                if (job.cancel || failed || adopt) break;
                update_priority();
                refresh_earlier(false);
                const auto slot = next.fetch_add(1);
                if (slot >= listing.files.size()) break;
                const auto index = read_order.empty() ? slot : read_order[slot];
                const TreeFile& file = listing.files[index];
                std::optional<PartialFile> hit;
                {
                    std::lock_guard lock(earlier_mutex);
                    if (const auto found = earlier.find(file.relative); found != earlier.end()) hit = found->second;
                }
                if (hit) {   // earlier entries are already checked against this listing's size and mtime
                    { std::lock_guard lock(hashes_mutex); hashes[index] = hit->sha256; }
                    ++job.files_reused;
                    if (hit->adopted) ++job.files_adopted;
                    job.bytes_done += file.size;
                } else {
                    if (options_.on_open) { std::lock_guard lock(hook_mutex); options_.on_open(file.relative); }
                    const fs::path path = root / steam::path_from_utf8(file.relative);
                    std::unique_ptr<XXH3_state_t, decltype(&XXH3_freeState)> state(XXH3_createState(), &XXH3_freeState);
                    if (!state) { set_problem("internal error: hash allocation"); break; }
                    XXH3_128bits_reset(state.get());
                    std::uint64_t read_total = 0;
                    const auto read = platform::read_native_chunks(path, std::span(buffer), [&](const std::span<const std::byte> bytes) {
                        XXH3_128bits_update(state.get(), bytes.data(), bytes.size());
                        const auto got = static_cast<std::uint64_t>(bytes.size());
                        read_total += got;
                        job.bytes_read += got;
                        job.bytes_done += got;
                        if (options_.on_read) { std::lock_guard lock(hook_mutex); options_.on_read(bytes.size()); }
                        update_priority();
                        since_yield += got;
                        if (background && options_.yield_every_bytes && since_yield >= options_.yield_every_bytes) {
                            since_yield = 0;
                            if (options_.yield_for.count() > 0) std::this_thread::sleep_for(options_.yield_for);
                            else std::this_thread::yield();
                        }
                        return !job.cancel && !failed;
                    });
                    const auto digest = XXH3_128bits_digest(state.get());
                    if (job.cancel || failed) break;
                    if (!read.ok) { set_problem("unreadable file " + file.relative); break; }
                    std::error_code error;
                    const fs::directory_entry entry(path, error);
                    const auto size = error ? std::uintmax_t{} : entry.file_size(error);
                    const auto time = error ? fs::file_time_type{} : entry.last_write_time(error);
                    if (read_total != file.size || error || size != file.size || static_cast<std::int64_t>(time.time_since_epoch().count()) != file.mtime) {
                        set_problem("changed while hashing: " + file.relative); break;
                    }
                    { std::lock_guard lock(hashes_mutex); hashes[index] = hex128(digest); }
                    ++job.files_hashed;
                }
                ++job.files_done;
                {
                    std::lock_guard lock(hashes_mutex);
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_partial >= options_.partial_every) {
                        save_partial();
                        last_partial = now;
                        try_adopt();   // another process may have finished this mod meanwhile
                    }
                }
            }
        } catch (const std::exception& error) { set_problem(std::string("internal error: ") + error.what()); }
          catch (...) { set_problem("internal error"); }
    };
    std::mutex batch_mutex;
    std::condition_variable batch_done;
    std::size_t remaining = 0;
    try {
        if (!pool_) pool_ = detail::acquire_digest_pool();
        for (std::size_t i = 0; i < count; ++i) {
            { std::lock_guard lock(batch_mutex); ++remaining; }
            try {
                pool_->submit([&] {
                    struct Completion {
                        std::mutex& mutex;
                        std::condition_variable& done;
                        std::size_t& remaining;
                        ~Completion() {
                            std::lock_guard lock(mutex);
                            --remaining;
                            done.notify_one();
                        }
                    } completion{batch_mutex, batch_done, remaining};
                    try { work(); }
                    catch (...) { set_problem("internal error: worker thread"); }
                });
            } catch (...) {
                { std::lock_guard lock(batch_mutex); --remaining; }
                throw;
            }
        }
    } catch (...) { set_problem("internal error: worker thread"); }
    {
        std::unique_lock lock(batch_mutex);
        batch_done.wait(lock, [&] { return remaining == 0; });
    }
    job.paused = false;

    if (adopt && !job.cancel && !failed) {
        // Another process cached the finished digest for this very tree; its bytes were hashed there.
        ContentDigest result;
        result.state = ContentDigestState::ready;
        result.sha256 = adopted_entry.sha256;
        result.files = adopted_entry.files;
        result.bytes = adopted_entry.bytes;
        std::lock_guard lock(mutex_);
        cache_[job.key] = std::make_unique<CacheEntry>(adopted_entry);
        job.adopted = true;
        job.paused = false;
        job.result = std::move(result);
        job.finished = true;
        return;
    }
    // Preserve completed entries on cancellation or failure as well as success.
    { std::lock_guard lock(hashes_mutex); save_partial(); }
    if (job.cancel) return fail("cancelled");
    if (failed) return fail(problem);
    std::string material = "eawr-mod-content-xxh3-v2\n";
    std::vector<std::size_t> order(hashes.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return core::to_lower(listing.files[a].relative) < core::to_lower(listing.files[b].relative);
    });
    const auto field = [&](std::string_view value) {
        material += std::to_string(value.size()) + ":";
        material.append(value);
    };
    std::string previous;
    for (const auto i : order) {
        const auto& file = listing.files[i];
        const auto path = core::to_lower(file.relative); // VFS folds ASCII and preserves other bytes.
        if (path == previous) return fail("ambiguous case-folded path: " + path);
        previous = path;
        field(path);
        field(std::to_string(file.size));
        field(hashes[i]);
    }
    ContentDigest result;
    result.state = ContentDigestState::ready;
    result.sha256 = hex128(XXH3_128bits_withSeed(material.data(), material.size(), 0))
                  + hex128(XXH3_128bits_withSeed(material.data(), material.size(), 1));
    result.files = listing.files.size();
    result.bytes = total;

    const fs::path digest_path = cache_file(job.key, false);
    if (!digest_path.empty()) {
        const std::string text = "{\"format\": " + steam::json_quote(kDigestFormat) + ", \"version\": " + std::to_string(kCacheVersion)
            + ", \"key\": " + steam::json_quote(job.key) + ", \"root\": " + steam::json_quote(job.root) + ", \"tree\": " + steam::json_quote(job.tree)
            + ", \"tree_files\": " + std::to_string(job.tree_files) + ", \"sha256\": " + steam::json_quote(result.sha256)
            + ", \"files\": " + std::to_string(result.files) + ", \"bytes\": " + std::to_string(result.bytes) + "}\n";
        (void)write_atomically(digest_path, text);
    }
    std::lock_guard lock(mutex_);
    auto entry = std::make_unique<CacheEntry>();
    entry->root = job.root;
    entry->tree = job.tree;
    entry->tree_files = job.tree_files;
    entry->sha256 = result.sha256;
    entry->files = result.files;
    entry->bytes = result.bytes;
    cache_[job.key] = std::move(entry);
    job.result = std::move(result);
    job.finished = true;
}

} // namespace eawr::mods
