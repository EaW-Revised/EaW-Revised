// Per-mod content digest (include/eawr/mods/content_digest.hpp) against synthetic trees made by the test.
#include "eawr/mods/content_digest.hpp"
#include "eawr/mods/discovery.hpp"
#include "eawr/mods/fingerprint.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
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
        root_ = fs::path(EAWR_CONTENT_DIGEST_SCRATCH) / ("scratch " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
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

// Computed independently with Python hashlib: the lines "Sub/b.bin", "a.txt", "z.txt" (byte order, so 'S' < 'a').
constexpr const char* kTinyDigest = "e5a89a22a239eb26e87474b6c68980099c78ce9d4940edc8a9a60baf4c803db7";

void test_canonical_and_cache() {
    Scratch scratch;
    const fs::path mod = scratch.path() / "mod";
    make_tiny(mod);
    const fs::path cache = scratch.path() / "cache";
    const ModRecord record = record_for(mod);
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
    // Material is "big.meg\t<size>\t<sha of the file>\n"; both hashes computed with Python hashlib.
    CHECK(out.sha256 == "652db3cc5437bd22477dd24bcc97f2f58587dac3b8d47c30abacceaa450adf43");
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
    test_canonical_and_cache();
    test_invalidation();
    test_corrupt_cache();
    test_failures();
    test_symlink_not_followed();
    test_resume_and_cancel();
    test_big_file_chunks();
    test_listing_order_independent();
    test_json_roundtrip();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "content digest tests passed\n";
    return 0;
}
