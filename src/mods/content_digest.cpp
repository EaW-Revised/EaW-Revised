#include "eawr/mods/content_digest.hpp"

#include "eawr/core/sha256.hpp"
#include "eawr/platform/native_read.hpp"
#include "thread_priority.hpp"

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

namespace {

constexpr std::string_view kDigestFormat = "eawr-mod-content-digest";
constexpr std::string_view kPartialFormat = "eawr-mod-content-partial";
constexpr int kCacheVersion = 1;

bool is_hex(std::string_view text, std::size_t length) {
    return text.size() == length && std::all_of(text.begin(), text.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// "<relative>\t<size>\t<hex>\n"
void add_line(core::Sha256Hasher& hasher, const std::string& relative, std::uint64_t size, const std::string& hex) {
    const std::string line = relative + '\t' + std::to_string(size) + '\t' + hex + '\n';
    hasher.update(std::span(reinterpret_cast<const std::uint8_t*>(line.data()), line.size()));
}

bool write_atomically(const fs::path& target, const std::string& text) {
    std::error_code error;
    fs::create_directories(target.parent_path(), error);
    const fs::path temporary = target.string().empty() ? target : fs::path(target.native() + fs::path::string_type(1, '~'));
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            out.close();
            fs::remove(temporary, error);
            return false;
        }
    }
    fs::rename(temporary, target, error);
    if (error) {
        fs::remove(temporary, error);
        return false;
    }
    return true;
}

std::optional<std::string> read_whole(const fs::path& cache_path) {
    std::ifstream in(cache_path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text;
}

struct PartialFile {
    std::uint64_t size = 0;
    std::string mtime;   // decimal ticks; kept as text, a double does not hold a 100 ns tick count exactly
    std::string sha256;
};

} // namespace

struct ContentDigests::CacheEntry {
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
    std::atomic<std::uint64_t> files_hashed{0}, files_reused{0}, bytes_read{0};
    // Guarded by ContentDigests::mutex_.
    bool finished = false;
    ContentDigest result;
};

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
                const std::string* sha = root->string_member("sha256");
                const steam::JsonValue* version = root->member("version");
                const steam::JsonValue* tree_files = root->member("tree_files");
                const steam::JsonValue* files = root->member("files");
                const steam::JsonValue* bytes = root->member("bytes");
                const auto count = [](const steam::JsonValue* v) {
                    return v != nullptr && v->kind == steam::JsonValue::Kind::number && v->number >= 0 && v->number <= 9007199254740992.0;
                };
                if (format && *format == kDigestFormat && version && version->kind == steam::JsonValue::Kind::number
                    && version->number == kCacheVersion && stored_key && *stored_key == key && tree && sha && is_hex(*tree, 64)
                    && is_hex(*sha, 64) && count(tree_files) && count(files) && count(bytes)) {
                    entry = std::make_unique<CacheEntry>();
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
    if (const CacheEntry* entry = cached(record.key, lock); entry != nullptr && entry->tree == print.tree && entry->tree_files == print.tree_files) {
        out.state = ContentDigestState::ready;
        out.sha256 = entry->sha256;
        out.files = entry->files;
        out.bytes = entry->bytes;
        return out;
    }
    if (const auto found = jobs_.find(record.key); found != jobs_.end()) {
        const std::shared_ptr<Job>& job = found->second;
        const bool same = job->tree == print.tree && job->tree_files == print.tree_files;
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
    out.bytes_read = job.bytes_read;
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
    if (options_.lower_priority) lower_current_thread_priority();
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

    // Per-file hashes from an earlier, interrupted run. Only a file with the same size and mtime counts.
    std::unordered_map<std::string, PartialFile> earlier;
    const fs::path partial_path = cache_file(job.key, true);
    if (!partial_path.empty()) {
        if (const auto text = read_whole(partial_path)) {
            const auto parsed = steam::parse_json(*text);
            const std::string* format = parsed ? parsed->string_member("format") : nullptr;
            const std::string* key = parsed ? parsed->string_member("key") : nullptr;
            const steam::JsonValue* version = parsed ? parsed->member("version") : nullptr;
            const steam::JsonValue* list = parsed ? parsed->member("files") : nullptr;
            if (format && *format == kPartialFormat && key && *key == job.key && version && version->number == kCacheVersion
                && list && list->kind == steam::JsonValue::Kind::array) {
                for (const steam::JsonValue& item : list->array) {
                    if (item.kind != steam::JsonValue::Kind::array || item.array.size() != 4) continue;
                    const auto& a = item.array;
                    if (a[0].kind != steam::JsonValue::Kind::string || a[1].kind != steam::JsonValue::Kind::number
                        || a[2].kind != steam::JsonValue::Kind::string || a[3].kind != steam::JsonValue::Kind::string
                        || !is_hex(a[3].string, 64) || !(a[1].number >= 0)) continue;
                    earlier[a[0].string] = {static_cast<std::uint64_t>(a[1].number), a[2].string, a[3].string};
                }
            }
        }
    }

    std::vector<std::pair<const TreeFile*, std::string>> done;   // finished files in walk order, with their hex
    done.reserve(listing.files.size());
    auto last_save = std::chrono::steady_clock::now();
    const auto save_partial = [&] {
        if (partial_path.empty() || done.empty()) return;
        std::string text = "{\"format\": " + steam::json_quote(kPartialFormat) + ", \"version\": " + std::to_string(kCacheVersion)
            + ", \"key\": " + steam::json_quote(job.key) + ", \"files\": [";
        bool first = true;
        for (const auto& [file, hex] : done) {
            text += first ? "\n" : ",\n";
            first = false;
            text += "[" + steam::json_quote(file->relative) + ", " + std::to_string(file->size) + ", \"" + std::to_string(file->mtime)
                + "\", \"" + hex + "\"]";
        }
        text += "\n]}\n";
        (void)write_atomically(partial_path, text);
        last_save = std::chrono::steady_clock::now();
    };
    const auto stop_with = [&](std::string note) {
        save_partial();
        fail(std::move(note));
    };

    std::vector<std::byte> buffer(kContentChunkBytes);   // the only large allocation of a run
    std::uint64_t since_yield = 0;
    for (const TreeFile& file : listing.files) {
        if (job.cancel) return stop_with("cancelled");
        std::string hex;
        const auto hit = earlier.find(file.relative);
        if (hit != earlier.end() && hit->second.size == file.size && hit->second.mtime == std::to_string(file.mtime)) {
            hex = hit->second.sha256;
            ++job.files_reused;
            job.bytes_done += file.size;
        } else {
            if (options_.on_open) options_.on_open(file.relative);
            const fs::path path = root / steam::path_from_utf8(file.relative);
            core::Sha256Hasher hasher;
            std::uint64_t read_total = 0;
            const auto read = platform::read_native_chunks(path, std::span(buffer),
                [&](const std::span<const std::byte> bytes) {
                    const auto got = static_cast<std::uint64_t>(bytes.size());
                    hasher.update(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
                    read_total += got;
                    job.bytes_read += got;
                    job.bytes_done += got;
                    since_yield += got;
                    if (options_.on_read) options_.on_read(bytes.size());
                    if (since_yield >= options_.yield_every_bytes && options_.yield_for.count() > 0) {
                        since_yield = 0;
                        std::this_thread::sleep_for(options_.yield_for);
                    }
                    return !job.cancel;   // cancel between chunks
                });
            if (job.cancel) return stop_with("cancelled");
            if (!read.ok) return stop_with("unreadable file " + file.relative);
            if (read_total != file.size) return stop_with("changed while hashing: " + file.relative);
            hex = hasher.hex_digest();
            ++job.files_hashed;
            // The size or mtime moved while we read it: this hash may describe a half-written file.
            std::error_code error;
            const fs::directory_entry entry(path, error);
            const auto size = error ? std::uintmax_t{} : entry.file_size(error);
            const auto time = error ? fs::file_time_type{} : entry.last_write_time(error);
            if (error || size != file.size || static_cast<std::int64_t>(time.time_since_epoch().count()) != file.mtime) {
                return stop_with("changed while hashing: " + file.relative);
            }
        }
        done.emplace_back(&file, std::move(hex));
        ++job.files_done;
        if (std::chrono::steady_clock::now() - last_save >= options_.partial_every) save_partial();
    }

    core::Sha256Hasher hasher;
    for (const auto& [file, hex] : done) add_line(hasher, file->relative, file->size, hex);
    ContentDigest result;
    result.state = ContentDigestState::ready;
    result.sha256 = hasher.hex_digest();
    result.files = listing.files.size();
    result.bytes = total;

    const fs::path digest_path = cache_file(job.key, false);
    if (!digest_path.empty()) {
        const std::string text = "{\"format\": " + steam::json_quote(kDigestFormat) + ", \"version\": " + std::to_string(kCacheVersion)
            + ", \"key\": " + steam::json_quote(job.key) + ", \"tree\": " + steam::json_quote(job.tree)
            + ", \"tree_files\": " + std::to_string(job.tree_files) + ", \"sha256\": " + steam::json_quote(result.sha256)
            + ", \"files\": " + std::to_string(result.files) + ", \"bytes\": " + std::to_string(result.bytes) + "}\n";
        if (write_atomically(digest_path, text) && !partial_path.empty()) {
            std::error_code error;
            fs::remove(partial_path, error);
        }
    }
    std::lock_guard lock(mutex_);
    auto entry = std::make_unique<CacheEntry>();
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
