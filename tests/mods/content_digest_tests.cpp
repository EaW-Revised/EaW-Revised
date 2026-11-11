#include "../support/runtime_path.hpp"
// Per-mod content digest (include/eawr/mods/content_digest.hpp) against synthetic trees made by the test.
#include "eawr/mods/content_digest.hpp"
#include "eawr/mods/discovery.hpp"
#include "eawr/mods/fingerprint.hpp"

#define XXH_INLINE_ALL
#include "../../third_party/xxhash/xxhash.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace eawr::mods;
using namespace std::chrono_literals;
namespace fs = eawr::mods::steam::fs;

namespace {
int g_failures = 0;
#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            ++g_failures;                                                                   \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition "\n"; \
        }                                                                                   \
    } while (false)

class Scratch {
public:
    Scratch() {
        static std::atomic<int> counter{0};
        // Under the test's own build folder (EAWR_CONTENT_DIGEST_SCRATCH), not the system temp: pool hosts' C: is nearly full.
        root_ = eawr::testing::runtime_path("EAWR_TEST_BUILD_ROOT", "tests/mods", EAWR_CONTENT_DIGEST_SCRATCH) / ("scratch " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                                                          + "-" + std::to_string(counter++));
        fs::create_directories(root_);
    }
    ~Scratch() {
        std::error_code error;
        fs::remove_all(root_, error);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    [[nodiscard]] const fs::path& path() const { return root_; }
private:
    fs::path root_;
};

void write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

ModRecord record_for(const fs::path& root, const std::string& key = "mods:T", std::uint64_t cap = kDefaultTreeEntryCap) {
    ModRecord record;
    record.key = key;
    record.root = steam::path_to_utf8(root);
    const TreeFingerprint print = tree_fingerprint(root, cap);
    record.fingerprint.tree = print.tree;
    record.fingerprint.tree_files = print.files;
    record.fingerprint.tree_capped = print.capped;
    if (print.incomplete) record.warnings.push_back({std::string(codes::tree_walk_incomplete), {}});
    return record;
}

ContentDigestOptions fast(const fs::path& cache) {
    ContentDigestOptions options;
    options.cache_folder = cache;
    options.yield_for = 0ms;
    options.lower_priority = false;
    options.worker_count = 1;
    return options;
}

std::string tiny_blob() {   // 10 x bytes 0,1,2
    std::string blob;
    for (int i = 0; i < 10; ++i) {
        blob += '\0';
        blob += '\1';
        blob += '\2';
    }
    return blob;
}

void make_tiny(const fs::path& root) {
    write(root / "z.txt", "zed");
    write(root / "a.txt", "alpha");
    write(root / "Sub" / "b.bin", tiny_blob());
}

// Independently computed by content_digest_vector.py with python-xxhash 3.6.0.
constexpr const char* kTinyDigest = "2d15e9fbf9663f5b1f01205ae2c499c38673d845b82a158acdb732ceafa9a13d";

void test_canonical_and_cache() {
    Scratch scratch;
    const fs::path mod = scratch.path() / "mod";
    make_tiny(mod);
    const fs::path cache = scratch.path() / "cache";
    const ModRecord record = record_for(mod);
    CHECK(ContentDigests::compute_now(mod).sha256 == kTinyDigest);
    {
        ContentDigests digests(fast(cache));
        CHECK(digests.lookup(record, false).state == ContentDigestState::unavailable);
        CHECK(digests.lookup(record, false).note.empty());
        const ContentDigest first = digests.lookup(record, true);
        CHECK(first.state == ContentDigestState::pending);
        CHECK(digests.lookup(record, true).state != ContentDigestState::unavailable);   // idempotent
        const ContentDigest done = digests.wait(record, 30s);
        CHECK(done.state == ContentDigestState::ready);
        CHECK(done.sha256 == kTinyDigest);
        CHECK(done.files == 3 && done.bytes == 3 + 5 + 30);
        CHECK(digests.stats(record.key).files_hashed == 3);
        CHECK(digests.stats(record.key).files_total == 3);
        CHECK(digests.stats(record.key).bytes_total == 38);
    }
    // New process state: cache hit with zero reads, even when the files can no longer be opened.
    {
        int opens = 0;
        auto options = fast(cache);
        options.on_open = [&](std::string_view) { ++opens; };
        ContentDigests digests(options);
        const ContentDigest hit = digests.lookup(record, true);
        CHECK(hit.state == ContentDigestState::ready && hit.sha256 == kTinyDigest);
        CHECK(opens == 0);
        CHECK(!digests.stats(record.key).known);
    }
    // compute_now agrees.
    CHECK(ContentDigests::compute_now(mod).sha256 == kTinyDigest);
}

void test_portable_tree_ignores_timestamps() {
    Scratch scratch;
    const auto mod = scratch.path() / "mod";
    make_tiny(mod);
    const auto before = tree_fingerprint(mod);
    const auto file = mod / "a.txt";
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(5));
    const auto touched = tree_fingerprint(mod);
    CHECK(before.tree != touched.tree);
    CHECK(!before.portable_tree.empty() && before.portable_tree == touched.portable_tree);
    write(file, "different size");
    CHECK(tree_fingerprint(mod).portable_tree != before.portable_tree);
    CHECK(tree_fingerprint(mod, 1).portable_tree.empty());
}

void test_two_live_instances_share_cache() {
    Scratch scratch;
    const auto mod = scratch.path() / "mod", cache = scratch.path() / "shared";
    make_tiny(mod);
    const auto record = record_for(mod);
    ContentDigests first(fast(cache));
    (void)first.lookup(record, true);
    CHECK(first.wait(record, 30s).state == ContentDigestState::ready);
    int opens = 0;
    auto options = fast(cache);
    options.on_open = [&](std::string_view) { ++opens; };
    ContentDigests second(options);
    CHECK(second.lookup(record, true).state == ContentDigestState::ready);
    CHECK(opens == 0);
}

void test_invalidation() {
    Scratch scratch;
    const fs::path mod = scratch.path() / "mod";
    make_tiny(mod);
    const fs::path cache = scratch.path() / "cache";
    {
        ContentDigests digests(fast(cache));
        const ModRecord record = record_for(mod);
        (void)digests.lookup(record, true);
        CHECK(digests.wait(record, 30s).state == ContentDigestState::ready);
    }
    write(mod / "a.txt", "alphX");   // same size, new mtime may tie on coarse clocks; force it
    fs::last_write_time(mod / "a.txt", fs::last_write_time(mod / "a.txt") + 5s);
    {
        ContentDigests digests(fast(cache));
        const ModRecord record = record_for(mod);
        CHECK(digests.lookup(record, false).state == ContentDigestState::unavailable);
        (void)digests.lookup(record, true);
        const ContentDigest done = digests.wait(record, 30s);
        CHECK(done.state == ContentDigestState::ready && done.sha256 != kTinyDigest);
        CHECK(digests.stats(record.key).files_reused == 2);
        CHECK(digests.stats(record.key).files_hashed == 1);
    }
    write(mod / "a.txt", "alpha and more");   // size change
    {
        ContentDigests digests(fast(cache));
        const ModRecord record = record_for(mod);
        CHECK(digests.lookup(record, false).state == ContentDigestState::unavailable);
    }
}

void test_corrupt_cache() {
    Scratch scratch;
    const fs::path mod = scratch.path() / "mod";
    make_tiny(mod);
    const fs::path cache = scratch.path() / "cache";
    const ModRecord record = record_for(mod);
    {
        ContentDigests digests(fast(cache));
        (void)digests.lookup(record, true);
        CHECK(digests.wait(record, 30s).state == ContentDigestState::ready);
    }
    for (const auto& entry : fs::directory_iterator(cache)) write(entry.path(), "{ not json");
    ContentDigests digests(fast(cache));
    CHECK(digests.lookup(record, false).state == ContentDigestState::unavailable);
    (void)digests.lookup(record, true);
    CHECK(digests.wait(record, 30s).sha256 == kTinyDigest);
    CHECK(digests.stats(record.key).files_hashed == 3);
}

void test_valid_looking_cache_corruption() {
    Scratch scratch;
    const auto mod = scratch.path() / "mod";
    const auto cache = scratch.path() / "cache";
    make_tiny(mod);
    const auto record = record_for(mod);
    {
        ContentDigests digests(fast(cache));
        (void)digests.lookup(record, true);
        CHECK(digests.wait(record, 30s).state == ContentDigestState::ready);
    }
    for (const auto& entry : fs::directory_iterator(cache)) {
        if (entry.path().filename().string().find(".partial.") != std::string::npos) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto at = text.find(kTinyDigest);
        CHECK(at != std::string::npos);
        if (at != std::string::npos) text[at] = text[at] == '0' ? '1' : '0';
        in.close();
        write(entry.path(), text);
    }
    ContentDigests digests(fast(cache));
    (void)digests.lookup(record, true);
    CHECK(digests.wait(record, 30s).sha256 == kTinyDigest);
    CHECK(digests.stats(record.key).files_hashed == 3);
}

void test_forged_cache_and_metadata_change() {
    Scratch scratch;
    const auto mod = scratch.path() / "mod", cache = scratch.path() / "cache";
    make_tiny(mod);
    const auto record = record_for(mod);
    {
        ContentDigests digests(fast(cache)); (void)digests.lookup(record, true);
        CHECK(digests.wait(record, 30s).sha256 == kTinyDigest);
    }
    const std::string forged(64, '0');
    for (const auto& entry : fs::directory_iterator(cache)) {
        if (entry.path().filename().string().find(".partial.") != std::string::npos) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto pos = text.find(kTinyDigest); CHECK(pos != std::string::npos);
        if (pos == std::string::npos) continue;
        text.replace(pos, 64, forged);
        const std::string body = text.substr(33);
        XXH128_canonical_t canonical;
        XXH128_canonicalFromHash(&canonical, XXH3_128bits(body.data(), body.size()));
        constexpr char digits[] = "0123456789abcdef";
        for (std::size_t i = 0; i < 16; ++i) {
            text[i * 2] = digits[canonical.digest[i] >> 4];
            text[i * 2 + 1] = digits[canonical.digest[i] & 15];
        }
        in.close(); write(entry.path(), text);
    }
    { // A valid forged cache is indistinguishable from a real one for the same metadata.
        ContentDigests digests(fast(cache));
        CHECK(digests.lookup(record, false).sha256 == forged);
    }
    write(mod / "a.txt", "changed size");
    const auto changed = record_for(mod);
    ContentDigests digests(fast(cache));
    (void)digests.lookup(changed, true);
    const auto result = digests.wait(changed, 30s);
    CHECK(result.state == ContentDigestState::ready && result.sha256 != forged);
    CHECK(result.sha256 == ContentDigests::compute_now(mod).sha256);
}

void test_screen_wait_leases() {
    CHECK(ContentDigests::screen_waiter_count() == 0);
    {
        auto first = ContentDigests::screen_wait_lease();
        CHECK(ContentDigests::screen_waiter_count() == 1);
        auto copy = first;
        auto second = ContentDigests::screen_wait_lease();
        CHECK(ContentDigests::screen_waiter_count() == 2);
        first.reset(); CHECK(ContentDigests::screen_waiter_count() == 2);
        copy.reset(); CHECK(ContentDigests::screen_waiter_count() == 1);
    }
    CHECK(ContentDigests::screen_waiter_count() == 0);
    try { auto lease = ContentDigests::screen_wait_lease(); throw std::runtime_error("test"); }
    catch (const std::runtime_error&) {}
    CHECK(ContentDigests::screen_waiter_count() == 0);
}

void test_failures() {
    Scratch scratch;
    {
        const fs::path empty = scratch.path() / "empty";
        fs::create_directories(empty / "dir");
        ContentDigests digests(fast({}));
        const ModRecord record = record_for(empty);
        const ContentDigest out = digests.lookup(record, true);
        CHECK(out.state == ContentDigestState::unavailable && out.note == "no files");
        ModRecord applied = record;
        digests.apply(applied, true);
        CHECK(applied.has_warning(codes::content_digest_unavailable));
        digests.apply(applied, true);
        CHECK(applied.warnings.size() == 1);
    }
    {
        const fs::path mod = scratch.path() / "capped";
        make_tiny(mod);
        ContentDigests digests(fast({}));
        const ModRecord record = record_for(mod, "mods:C", 2);
        CHECK(record.fingerprint.tree_capped);
        const ContentDigest out = digests.lookup(record, true);
        CHECK(out.state == ContentDigestState::unavailable && out.note == "capped walk");
    }
    {
        const fs::path mod = scratch.path() / "incomplete";
        make_tiny(mod);
        ModRecord record = record_for(mod, "mods:I");
        record.warnings.push_back({std::string(codes::tree_walk_incomplete), {}});
        ContentDigests digests(fast({}));
        CHECK(digests.lookup(record, true).note == "incomplete walk");
    }
    {
        const fs::path mod = scratch.path() / "missing";
        ModRecord record;
        record.key = "mods:M";
        record.root = steam::path_to_utf8(mod);
        record.state = ModState::missing;
        ContentDigests digests(fast({}));
        CHECK(digests.lookup(record, true).note == "folder missing");
    }
    {   // an unreadable file: removed between the fingerprint and the job's read pass is "changed", so make the open fail
        const fs::path mod = scratch.path() / "unreadable";
        make_tiny(mod);
        ModRecord record = record_for(mod, "mods:U");
        auto options = fast({});
        options.on_open = [&](std::string_view relative) {
            if (relative == "a.txt") {
                std::error_code error;
                fs::remove(mod / "a.txt", error);   // vanishes just before the open
            }
        };
        ContentDigests digests(options);
        (void)digests.lookup(record, true);
        const ContentDigest out = digests.wait(record, 30s);
        CHECK(out.state == ContentDigestState::unavailable);
        CHECK(out.note == "unreadable file a.txt");
    }
    {   // a tree that changed since the fingerprint
        const fs::path mod = scratch.path() / "changed";
        make_tiny(mod);
        const ModRecord record = record_for(mod, "mods:X");
        write(mod / "new.txt", "n");
        ContentDigests digests(fast({}));
        (void)digests.lookup(record, true);
        CHECK(digests.wait(record, 30s).note == "folder changed since it was scanned");
    }
}

void test_symlink_not_followed() {
    Scratch scratch;
    const fs::path mod = scratch.path() / "mod";
    make_tiny(mod);
    const fs::path outside = scratch.path() / "outside";
    write(outside / "big.txt", "should not be hashed");
    std::error_code error;
    fs::create_directory_symlink(outside, mod / "link", error);
    if (error) {
        std::cerr << "SKIPPED symlink test: cannot create a directory symlink here (" << error.message() << ")\n";
        return;
    }
    CHECK(ContentDigests::compute_now(mod).sha256 == kTinyDigest);
}

void test_resume_and_cancel() {
    Scratch scratch;
    const fs::path mod = scratch.path() / "mod";
    for (int i = 0; i < 6; ++i) write(mod / ("f" + std::to_string(i) + ".dat"), std::string(3 * 1024 * 1024 + i, static_cast<char>('a' + i)));
    const fs::path cache = scratch.path() / "cache";
    const ModRecord record = record_for(mod);
    std::string reference;
    {
        ContentDigests digests(fast({}));
        (void)digests.lookup(record, true);
        reference = digests.wait(record, 60s).sha256;
        CHECK(reference.size() == 64);
    }
    // Cancel after the third file has been opened.
    {
        ContentDigests* self = nullptr;
        int opens = 0;
        auto options = fast(cache);
        options.on_open = [&](std::string_view) {
            if (++opens == 4 && self != nullptr) self->cancel("mods:T");
        };
        ContentDigests digests(options);
        self = &digests;
        (void)digests.lookup(record, true);
        const auto begin = std::chrono::steady_clock::now();
        const ContentDigest out = digests.wait(record, 60s);
        CHECK(out.state == ContentDigestState::unavailable && out.note == "cancelled");
        CHECK(std::chrono::steady_clock::now() - begin < 10s);   // cancel latency: between chunks
        CHECK(digests.stats("mods:T").files_hashed >= 3);
        // A new request after a cancel restarts the job.
        CHECK(digests.lookup(record, true).state == ContentDigestState::pending);
        digests.cancel_all();
    }
    // Resume: files finished before the cancel are not read again.
    {
        ContentDigests digests(fast(cache));
        (void)digests.lookup(record, true);
        const ContentDigest out = digests.wait(record, 60s);
        CHECK(out.state == ContentDigestState::ready && out.sha256 == reference);
        const ContentJobStats stats = digests.stats(record.key);
        CHECK(stats.files_reused >= 3);
        CHECK(stats.files_reused + stats.files_hashed == 6);
    }
    // A partial entry whose file changed since is hashed again.
    {
        for (const auto& entry : fs::directory_iterator(cache)) fs::remove(entry.path());
        ContentDigests* self = nullptr;
        int opens = 0;
        auto options = fast(cache);
        options.on_open = [&](std::string_view) {
            if (++opens == 3 && self != nullptr) self->cancel("mods:T");
        };
        {
            ContentDigests digests(options);
            self = &digests;
            (void)digests.lookup(record, true);
            (void)digests.wait(record, 60s);
        }
        write(mod / "f0.dat", std::string(3 * 1024 * 1024 + 0, 'q'));   // same size, other bytes
        fs::last_write_time(mod / "f0.dat", fs::last_write_time(mod / "f0.dat") + 7s);
        const ModRecord changed = record_for(mod);
        ContentDigests digests(fast(cache));
        (void)digests.lookup(changed, true);
        const ContentDigest out = digests.wait(changed, 60s);
        CHECK(out.state == ContentDigestState::ready && out.sha256 != reference);
        CHECK(digests.stats(changed.key).files_hashed >= 1);
    }
}

void test_big_file_chunks() {
    Scratch scratch;
    const fs::path mod = scratch.path() / "mod";
    fs::create_directories(mod);
    constexpr int kMiB = 8;   // 8 chunks; pool hosts have little free disk, so no big file
    {
        std::ofstream out(mod / "big.meg", std::ios::binary);
        const std::string block(1024 * 1024, 'x');
        for (int i = 0; i < kMiB; ++i) out << block;
    }
    std::size_t biggest = 0, chunks = 0;
    auto options = fast({});
    options.on_read = [&](std::size_t bytes) {
        biggest = std::max(biggest, bytes);
        ++chunks;
    };
    ContentDigests digests(options);
    const ModRecord record = record_for(mod);
    (void)digests.lookup(record, true);
    const ContentDigest out = digests.wait(record, 120s);
    CHECK(out.state == ContentDigestState::ready);
    CHECK(out.bytes == static_cast<std::uint64_t>(kMiB) * 1024 * 1024);
    CHECK(biggest == kContentChunkBytes);   // never more than one 1 MiB chunk in flight (the only large buffer)
    CHECK(chunks == static_cast<std::size_t>(kMiB));
    CHECK(out.sha256 == ContentDigests::compute_now(mod).sha256);
    CHECK(digests.stats(record.key).bytes_read == static_cast<std::uint64_t>(kMiB) * 1024 * 1024);
}

// Same files created in another order (so another directory listing order and other mtimes): same digest.
void test_listing_order_independent() {
    Scratch scratch;
    const fs::path first = scratch.path() / "one";
    const fs::path second = scratch.path() / "two";
    make_tiny(first);
    write(second / "a.txt", "alpha");
    write(second / "Sub" / "b.bin", tiny_blob());
    write(second / "z.txt", "zed");
    CHECK(ContentDigests::compute_now(first).sha256 == kTinyDigest);
    CHECK(ContentDigests::compute_now(second).sha256 == kTinyDigest);
    for (const std::size_t workers : {1u, 2u, 4u, 8u}) {
        auto options = fast({});
        options.worker_count = workers;
        ContentDigests digests(options);
        const auto record = record_for(second);
        (void)digests.lookup(record, true);
        CHECK(digests.wait(record, 30s).sha256 == kTinyDigest);
    }
}

void test_parallel_worker_counts() {
    Scratch scratch;
    const auto mod = scratch.path() / "parallel";
    for (int i = 0; i < 24; ++i) write(mod / ("file" + std::to_string(i)), std::string(4096 + i, static_cast<char>('a' + i)));
    const auto record = record_for(mod);
    std::string reference;
    for (const std::size_t workers : {1u, 2u, 4u, 8u}) {
        auto options = fast({});
        options.worker_count = workers;
        ContentDigests digests(options);
        (void)digests.lookup(record, true);
        const auto result = digests.wait(record, 30s);
        CHECK(result.state == ContentDigestState::ready);
        if (reference.empty()) reference = result.sha256;
        CHECK(result.sha256 == reference);
    }
}

void test_old_cache_versions() {
    Scratch scratch;
    const auto mod = scratch.path() / "mod", cache = scratch.path() / "cache";
    make_tiny(mod); const auto record = record_for(mod);
    {
        ContentDigests digests(fast(cache)); (void)digests.lookup(record, true);
        CHECK(digests.wait(record, 30s).state == ContentDigestState::ready);
    }
    // Change both cache bodies to the old version, retaining a valid envelope.
    for (const auto& entry : fs::directory_iterator(cache)) {
        std::ifstream in(entry.path(), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto pos = text.find("\"version\": 3");
        CHECK(pos != std::string::npos);
        if (pos == std::string::npos) continue;
        text[pos + std::string("\"version\": ").size()] = '1';
        const std::string body = text.substr(33);
        XXH128_canonical_t canonical;
        XXH128_canonicalFromHash(&canonical, XXH3_128bits(body.data(), body.size()));
        constexpr char digits[] = "0123456789abcdef";
        for (std::size_t i = 0; i < 16; ++i) {
            text[i * 2] = digits[canonical.digest[i] >> 4];
            text[i * 2 + 1] = digits[canonical.digest[i] & 15];
        }
        in.close(); write(entry.path(), text);
    }
    ContentDigests digests(fast(cache));
    CHECK(digests.lookup(record, false).state == ContentDigestState::unavailable);
    (void)digests.lookup(record, true);
    CHECK(digests.wait(record, 30s).sha256 == kTinyDigest);
    CHECK(digests.stats(record.key).files_hashed == 3);
}

void test_parallel_failure_and_cancel() {
    Scratch scratch;
    const auto mod = scratch.path() / "mod";
    for (int i = 0; i < 12; ++i) write(mod / ("f" + std::to_string(i)), std::string(2 * 1024 * 1024, 'x'));
    const auto record = record_for(mod);
    {
        auto lease = ContentDigests::screen_wait_lease();
        auto options = fast({}); options.worker_count = 4;
        options.on_read = [](std::size_t) { throw std::runtime_error("injected worker exception"); };
        ContentDigests digests(options);
        (void)digests.lookup(record, true);
        const auto result = digests.wait(record, 30s);
        CHECK(result.state == ContentDigestState::unavailable);
        CHECK(result.note.find("injected worker exception") != std::string::npos);
    }
    {
        auto lease = ContentDigests::screen_wait_lease();
        auto options = fast({}); options.worker_count = 4;
        ContentDigests* self = nullptr;
        options.on_read = [&](std::size_t) { self->cancel(record.key); };
        ContentDigests digests(options); self = &digests;
        (void)digests.lookup(record, true);
        const auto result = digests.wait(record, 30s);
        CHECK(result.state == ContentDigestState::unavailable && result.note == "cancelled");
    }
    CHECK(ContentDigests::screen_waiter_count() == 0);
}

void test_shared_pool_cap() {
    Scratch scratch;
    std::mutex ids_mutex;
    std::set<std::thread::id> ids;
    std::vector<std::unique_ptr<ContentDigests>> jobs;
    std::vector<ModRecord> records;
    for (int job = 0; job < 3; ++job) {
        const auto root = scratch.path() / ("mod" + std::to_string(job));
        for (int i = 0; i < 48; ++i) write(root / ("f" + std::to_string(i)), "content");
        records.push_back(record_for(root, "mods:" + std::to_string(job)));
        auto options = fast({}); options.worker_count = 8;
        options.on_open = [&](std::string_view) {
            { std::lock_guard lock(ids_mutex); ids.insert(std::this_thread::get_id()); }
            std::this_thread::sleep_for(2ms);
        };
        jobs.push_back(std::make_unique<ContentDigests>(options));
    }
    for (std::size_t i = 0; i < jobs.size(); ++i) (void)jobs[i]->lookup(records[i], true);
    for (std::size_t i = 0; i < jobs.size(); ++i) CHECK(jobs[i]->wait(records[i], 30s).state == ContentDigestState::ready);
    CHECK(!ids.empty() && ids.size() <= 8); // shared pool, including concurrent ContentDigests instances
}

void test_case_and_same_mtime_risk() {
    Scratch scratch;
    const auto one = scratch.path() / "one", two = scratch.path() / "two", cache = scratch.path() / "cache";
    write(one / "Data" / "A.txt", "abc"); write(two / "data" / "a.TXT", "abc");
    CHECK(ContentDigests::compute_now(one).sha256 == ContentDigests::compute_now(two).sha256);
    const auto before = record_for(one);
    std::string original;
    {
        ContentDigests digests(fast(cache)); (void)digests.lookup(before, true);
        original = digests.wait(before, 30s).sha256;
    }
    const auto path = one / "Data" / "A.txt";
    const auto time = fs::last_write_time(path);
    write(path, "xyz"); fs::last_write_time(path, time);
    const auto replaced = record_for(one);
    CHECK(replaced.fingerprint.tree == before.fingerprint.tree);
    ContentDigests digests(fast(cache));
    CHECK(digests.lookup(replaced, true).sha256 == original); // accepted metadata-cache risk
    CHECK(ContentDigests::compute_now(one).sha256 != original);
}

void test_periodic_partial() {
    Scratch scratch;
    const auto mod = scratch.path() / "mod", cache = scratch.path() / "cache";
    for (int i = 0; i < 4; ++i) write(mod / ("f" + std::to_string(i)), "content");
    auto options = fast(cache); options.partial_every = 0ms;
    bool seen = false;
    options.on_open = [&](std::string_view) {
        if (fs::exists(cache)) for (const auto& entry : fs::directory_iterator(cache))
            if (entry.path().filename().string().find(".partial.") != std::string::npos) seen = true;
    };
    ContentDigests digests(options); const auto record = record_for(mod);
    (void)digests.lookup(record, true);
    CHECK(digests.wait(record, 30s).state == ContentDigestState::ready);
    CHECK(seen);
}

// #6588: the mods service digests only in the background: a paused job reads no mod file, resumes when the pause
// ends, takes a digest another process cached for the same tree, and is overridden by a waiting screen.
void test_pause_and_adopt() {
    {
        Scratch scratch;
        const auto mod = scratch.path() / "mod", cache = scratch.path() / "cache";
        make_tiny(mod);
        const auto record = record_for(mod);
        std::atomic<bool> pause{true};
        std::atomic<int> opens{0};
        auto options = fast(cache);
        options.pause_while = [&] { return pause.load(); };
        options.pause_poll = 20ms;
        options.on_open = [&](std::string_view) { ++opens; };
        ContentDigests digests(options);
        CHECK(digests.lookup(record, true).state == ContentDigestState::pending);
        std::this_thread::sleep_for(400ms);
        CHECK(opens == 0);
        CHECK(digests.stats(record.key).paused);
        CHECK(digests.lookup(record, false).state == ContentDigestState::pending);
        pause = false;
        const ContentDigest done = digests.wait(record, 30s);
        CHECK(done.state == ContentDigestState::ready && done.sha256 == kTinyDigest);
        CHECK(opens == 3);
        CHECK(!digests.stats(record.key).paused && !digests.stats(record.key).adopted);
    }
    {
        Scratch scratch;
        const auto mod = scratch.path() / "mod", cache = scratch.path() / "cache";
        make_tiny(mod);
        const auto record = record_for(mod);
        std::atomic<int> opens{0};
        auto options = fast(cache);
        options.pause_while = [] { return true; };
        options.pause_poll = 20ms;
        options.on_open = [&](std::string_view) { ++opens; };
        ContentDigests paused(options);
        CHECK(paused.lookup(record, true).state == ContentDigestState::pending);
        std::this_thread::sleep_for(200ms);
        ContentDigests other(fast(cache));   // the game's own digest in the same cache folder
        (void)other.lookup(record, true);
        CHECK(other.wait(record, 30s).sha256 == kTinyDigest);
        const ContentDigest done = paused.wait(record, 30s);
        CHECK(done.state == ContentDigestState::ready && done.sha256 == kTinyDigest);
        CHECK(done.files == 3 && done.bytes == 38);
        CHECK(opens == 0);
        CHECK(paused.stats(record.key).adopted && paused.stats(record.key).files_hashed == 0);
        CHECK(paused.lookup(record, false).state == ContentDigestState::ready);
    }
    {
        Scratch scratch;
        const auto mod = scratch.path() / "mod", cache = scratch.path() / "cache";
        make_tiny(mod);
        const auto record = record_for(mod);
        auto options = fast(cache);
        options.pause_while = [] { return true; };
        options.pause_poll = 20ms;
        ContentDigests digests(options);
        const auto lease = ContentDigests::screen_wait_lease();   // a screen is waiting: the pause does not apply
        (void)digests.lookup(record, true);
        const ContentDigest done = digests.wait(record, 30s);
        CHECK(done.state == ContentDigestState::ready && done.sha256 == kTinyDigest);
        CHECK(digests.stats(record.key).files_hashed == 3);
    }
    {
        // Cancel and teardown reach a paused job quickly.
        Scratch scratch;
        const auto mod = scratch.path() / "mod";
        make_tiny(mod);
        const auto record = record_for(mod);
        auto options = fast(scratch.path() / "cache");
        options.pause_while = [] { return true; };
        options.pause_poll = 20ms;
        const auto begin = std::chrono::steady_clock::now();
        {
            ContentDigests digests(options);
            (void)digests.lookup(record, true);
            std::this_thread::sleep_for(100ms);
            digests.cancel(record.key);
            const ContentDigest out = digests.wait(record, 30s);
            CHECK(out.note == "cancelled");
        }
        CHECK(std::chrono::steady_clock::now() - begin < 10s);
    }
}

void test_background_io_off_same_digest() {
    Scratch scratch;
    const auto mod = scratch.path() / "mod";
    make_tiny(mod);
    const auto record = record_for(mod);
    auto options = fast(scratch.path() / "cache");
    options.lower_priority = true;
    options.background_io = false;   // the mods service: below-normal CPU, normal I/O priority
    ContentDigests digests(options);
    (void)digests.lookup(record, true);
    const ContentDigest done = digests.wait(record, 30s);
    CHECK(done.state == ContentDigestState::ready && done.sha256 == kTinyDigest);
}

void test_json_roundtrip() {
    ModRecord record;
    record.key = "mods:J";
    record.root = "x";
    record.content = {"ab", ContentDigestState::pending, 3, 40, "n"};
    const std::string text = record_to_json(record);
    const auto parsed = steam::parse_json(text);
    CHECK(parsed.has_value());
    const auto back = record_from_json(*parsed);
    CHECK(back && back->content == record.content);
}
} // namespace

int main() {
    test_pause_and_adopt();
    test_background_io_off_same_digest();
    test_old_cache_versions();
    test_screen_wait_leases();
    test_forged_cache_and_metadata_change();
    test_parallel_failure_and_cancel();
    test_shared_pool_cap();
    test_case_and_same_mtime_risk();
    test_periodic_partial();
    test_canonical_and_cache();
    test_two_live_instances_share_cache();
    test_portable_tree_ignores_timestamps();
    test_invalidation();
    test_corrupt_cache();
    test_valid_looking_cache_corruption();
    test_failures();
    test_symlink_not_followed();
    test_resume_and_cancel();
    test_big_file_chunks();
    test_listing_order_independent();
    test_parallel_worker_counts();
    test_json_roundtrip();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "content digest tests passed\n";
    return 0;
}
