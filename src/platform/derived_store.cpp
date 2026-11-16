#include "eawr/platform/derived_store.hpp"

#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace eawr::platform {
namespace {

constexpr std::array<char, 8> segment_magic{'E', 'A', 'W', 'F', 'A', 'C', 'T', '\0'};
constexpr std::uint32_t segment_format = 1;
constexpr std::size_t header_size = 48;
constexpr std::size_t frame_head_size = 4 + 16;
constexpr std::size_t payload_fixed_size = 1 + 16 + 32;
constexpr std::uint64_t max_frame_payload = 64ULL << 20U; // a frame longer than this is corrupt, whatever the file size says
constexpr auto stale_temporary_age = std::chrono::minutes(10);

void put_u32(std::vector<std::uint8_t>& out, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
}
void put_u64(std::vector<std::uint8_t>& out, const std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
}
// Writes `value` little-endian at `at`; returns the position after it. The caller guarantees 8 bytes of room.
std::uint8_t* store_u64(std::uint8_t* at, const std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) *at++ = static_cast<std::uint8_t>((value >> shift) & 0xffU);
    return at;
}
std::uint32_t get_u32(const std::uint8_t* at) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(at[i]) << (i * 8U);
    return value;
}
std::uint64_t get_u64(const std::uint8_t* at) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(at[i]) << (i * 8U);
    return value;
}

std::array<std::uint8_t, 16> check16(const std::uint8_t* data, const std::size_t size) {
    const auto digest = core::sha256(std::span<const std::uint8_t>(data, size));
    std::array<std::uint8_t, 16> out{};
    std::copy_n(digest.begin(), 16, out.begin());
    return out;
}

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string hex16(const std::uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(value));
    return std::string(text);
}

std::filesystem::path folder_of(const DerivedStoreOptions& options) {
    return options.folder_utf8.empty() ? std::filesystem::path{}
        : std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(options.folder_utf8.data()), options.folder_utf8.size()));
}

std::string index_key(const std::uint8_t kind, const DerivedKey& key) {
    std::string out;
    out.reserve(17);
    out.push_back(static_cast<char>(kind));
    out.append(reinterpret_cast<const char*>(key.data()), key.size());
    return out;
}

std::string new_segment_name(const std::uint64_t created_ms) {
    std::random_device random;
    return "seg-" + hex16(created_ms) + "-" + hex16((static_cast<std::uint64_t>(random()) << 32U) | random()) + ".efs";
}

// `size` bytes at `offset` of a file; false when the file is gone, short or unreadable.
bool read_range(const std::filesystem::path& path, const std::uint64_t offset, const std::uint64_t size, std::vector<std::uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (size != 0) in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    return static_cast<bool>(in);
}

// A whole frame (length, check, payload) is well formed: its length matches and its check covers the payload.
bool frame_intact(const std::vector<std::uint8_t>& frame) {
    if (frame.size() < frame_head_size + payload_fixed_size) return false;
    if (get_u32(frame.data()) != frame.size() - frame_head_size) return false;
    const std::uint8_t* payload = frame.data() + frame_head_size;
    return std::equal(payload - 16, payload, check16(payload, frame.size() - frame_head_size).begin());
}

// The frame is intact and is the record the caller asked for.
bool frame_is(const std::vector<std::uint8_t>& frame, const std::uint8_t kind, const DerivedKey& key, const DerivedStamp& stamp) {
    if (!frame_intact(frame)) return false;
    const std::uint8_t* payload = frame.data() + frame_head_size;
    return payload[0] == kind && std::equal(key.begin(), key.end(), payload + 1) && get_u64(payload + 17) == stamp.container_size
        && static_cast<std::int64_t>(get_u64(payload + 25)) == stamp.container_mtime && get_u64(payload + 33) == stamp.entry_offset
        && get_u64(payload + 41) == stamp.entry_size;
}

std::vector<std::uint8_t> make_header_bytes(const DerivedStoreOptions& options, const std::uint64_t created) {
    std::vector<std::uint8_t> out;
    out.insert(out.end(), segment_magic.begin(), segment_magic.end());
    put_u32(out, segment_format);
    put_u32(out, options.facts_format);
    out.insert(out.end(), options.epoch.begin(), options.epoch.end());
    put_u64(out, created);
    const auto check = check16(out.data(), out.size());
    out.insert(out.end(), check.begin(), check.begin() + 8);
    return out;
}

} // namespace

std::array<std::uint8_t, 16> derived_epoch(const std::string_view domain, const std::string_view build) {
    std::string text(domain);
    text.push_back('\n');
    text.append(build);
    return check16(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

DerivedKey derived_key(const std::string_view domain, const std::span<const std::string_view> parts) {
    std::vector<std::uint8_t> bytes;
    const auto part = [&](const std::string_view value) {
        put_u32(bytes, static_cast<std::uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    };
    part(domain);
    for (const auto value : parts) part(value);
    return check16(bytes.data(), bytes.size());
}

struct DerivedStore::State {
    struct Segment final {
        std::string name;
        std::uint64_t created_ms{};
        std::vector<std::uint8_t> bytes; // header + frames while the segment is not yet written; empty afterwards
        std::uint64_t file_size{};       // size of the segment file once written (or loaded)
        bool persisted{};
        bool dead{};
        bool own{}; // written by this process (a segment loaded from the folder is not: another process may be reading it)
    };
    struct Entry final {
        std::uint32_t segment{};
        std::uint64_t body_offset{}, body_size{}, order{};
        DerivedStamp stamp;
    };
    struct StaleFile final {
        std::filesystem::path path;
        std::uint64_t created_ms{}, size{};
        bool bad{};
    };

    std::mutex mutex;
    std::condition_variable wake, idle;
    std::once_flag open_once;
    std::thread opener, writer;
    bool opener_started{}, stopping{}, flushing{}, compacted{};
    DerivedStoreStats stats;
    std::vector<std::unique_ptr<Segment>> segments;
    std::unordered_map<std::string, Entry> index;
    std::vector<StaleFile> stale;
    std::int64_t open_segment{-1};
    std::uint64_t pending_bytes{};
    std::uint64_t resident_bytes{};
    std::chrono::steady_clock::time_point pending_since{};
    std::uint64_t order{};
    std::uint64_t newest_created_ms{}; // creation time of the newest segment loaded or made; the next one is strictly later
};

namespace {
using State = DerivedStore::State;

// The creation time for a new segment. Segments load in name order (creation time first) and a later segment's record wins,
// so a new segment must sort after every segment this store has loaded or made, even inside the same millisecond.
std::uint64_t next_created_ms(State& state) {
    state.newest_created_ms = std::max(now_ms(), state.newest_created_ms + 1);
    return state.newest_created_ms;
}

inline std::uint64_t frame_start_of(const State::Entry& entry) { return entry.body_offset - payload_fixed_size - frame_head_size; }
inline std::uint64_t frame_size_of(const State::Entry& entry) { return frame_head_size + payload_fixed_size + entry.body_size; }

// Forgets a segment's records and bytes (a segment that could not be written, or one merged away).
void drop_segment(State& state, const std::uint32_t id) {
    auto& segment = *state.segments[id];
    state.resident_bytes -= std::min<std::uint64_t>(state.resident_bytes, segment.bytes.size());
    segment.bytes.clear();
    segment.bytes.shrink_to_fit();
    segment.dead = true;
    for (auto it = state.index.begin(); it != state.index.end();) {
        if (it->second.segment == id) it = state.index.erase(it);
        else ++it;
    }
}
} // namespace

DerivedStore::DerivedStore(DerivedStoreOptions options) : options_(std::move(options)), state_(std::make_unique<State>()) {
    state_->stats.disabled = options_.folder_utf8.empty();
}

DerivedStore::~DerivedStore() {
    finish();
    if (state_->opener.joinable()) state_->opener.join();
}

void DerivedStore::begin_open() {
    std::lock_guard lock(state_->mutex);
    if (state_->opener_started) return;
    state_->opener_started = true;
    state_->opener = std::thread([this] { ensure_open(); });
}

void DerivedStore::ensure_open() {
    std::call_once(state_->open_once, [this] { open_folder(); });
}

namespace {

// Reads one segment's frames in order into the index, one frame in memory at a time. Frames are checked as they are read;
// the first bad frame ends the scan of this segment (the records before it stay usable).
void scan_segment(State& state, const std::uint32_t id, std::ifstream& in, const std::uint64_t file_size) {
    std::uint64_t at = header_size;
    std::vector<std::uint8_t> head(frame_head_size), payload;
    while (at + frame_head_size <= file_size) {
        in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        if (!in) {
            ++state.stats.frames_truncated;
            return;
        }
        const std::uint32_t length = get_u32(head.data());
        if (length < payload_fixed_size || length > max_frame_payload || length > file_size - at - frame_head_size) {
            ++state.stats.frames_truncated;
            return;
        }
        payload.resize(length);
        in.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        if (!in) {
            ++state.stats.frames_truncated;
            return;
        }
        if (!std::equal(head.begin() + 4, head.end(), check16(payload.data(), length).begin())) {
            ++state.stats.frames_corrupt;
            return;
        }
        DerivedKey key{};
        std::copy_n(payload.data() + 1, 16, key.begin());
        State::Entry entry;
        entry.segment = id;
        entry.body_offset = at + frame_head_size + payload_fixed_size;
        entry.body_size = length - payload_fixed_size;
        entry.order = ++state.order;
        entry.stamp = {get_u64(payload.data() + 17), static_cast<std::int64_t>(get_u64(payload.data() + 25)), get_u64(payload.data() + 33),
                       get_u64(payload.data() + 41)};
        state.index.insert_or_assign(index_key(payload[0], key), entry);
        ++state.stats.records_indexed;
        at += frame_head_size + length;
    }
    if (at != file_size) ++state.stats.frames_truncated;
}

} // namespace

void DerivedStore::open_folder() {
    const auto open_started = std::chrono::steady_clock::now();
    State loaded;
    std::vector<std::filesystem::path> files;
    const auto folder_path = folder_of(options_);
    if (!folder_path.empty()) {
        std::error_code error;
        for (std::filesystem::directory_iterator it(folder_path, error), end; !error && it != end; it.increment(error)) {
            std::error_code entry_error;
            if (!it->is_regular_file(entry_error)) continue;
            const auto& path = it->path();
            if (path.extension() == ".efs") {
                files.push_back(path);
            } else if (path.extension() == ".tmp" && path.stem().extension() == ".efs") {
                // A write torn by a crash. A fresh one may be another process mid-write: only old ones are garbage.
                const auto written = std::filesystem::last_write_time(path, entry_error);
                if (!entry_error && std::filesystem::file_time_type::clock::now() - written > stale_temporary_age) {
                    std::filesystem::remove(path, entry_error);
                    if (!entry_error) ++loaded.stats.tmp_removed;
                }
            }
        }
        std::sort(files.begin(), files.end());
    }
    for (const auto& path : files) {
        std::error_code size_error;
        const std::uint64_t file_size = std::filesystem::file_size(path, size_error);
        std::ifstream in(path, std::ios::binary);
        if (!in || size_error) {
            ++loaded.stats.segments_unreadable;
            continue;
        }
        std::vector<std::uint8_t> header(header_size);
        if (file_size >= header_size) in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
        const bool header_ok = file_size >= header_size && static_cast<bool>(in)
            && std::equal(segment_magic.begin(), segment_magic.end(), header.begin())
            && std::equal(header.begin() + 40, header.begin() + 48, check16(header.data(), 40).begin());
        if (!header_ok) {
            ++loaded.stats.segments_bad;
            loaded.stale.push_back({path, 0, file_size, true});
            continue;
        }
        const std::uint64_t created = get_u64(header.data() + 32);
        if (get_u32(header.data() + 8) != segment_format || get_u32(header.data() + 12) != options_.facts_format
            || !std::equal(options_.epoch.begin(), options_.epoch.end(), header.begin() + 16)) {
            ++loaded.stats.segments_old;
            loaded.stale.push_back({path, created, file_size, false});
            continue;
        }
        auto segment = std::make_unique<State::Segment>();
        segment->name = path.filename().string();
        segment->created_ms = created;
        segment->file_size = file_size;
        segment->persisted = true;
        loaded.segments.push_back(std::move(segment));
        loaded.newest_created_ms = std::max(loaded.newest_created_ms, created);
        ++loaded.stats.segments_loaded;
        loaded.stats.bytes_loaded += file_size;
        scan_segment(loaded, static_cast<std::uint32_t>(loaded.segments.size() - 1), in, file_size);
    }
    std::lock_guard lock(state_->mutex);
    auto& state = *state_;
    // Records put before the open finished (none: put() opens first) would sit in state; the loaded index goes first.
    state.segments = std::move(loaded.segments);
    state.index = std::move(loaded.index);
    state.stale = std::move(loaded.stale);
    state.order = loaded.order;
    state.newest_created_ms = std::max(state.newest_created_ms, loaded.newest_created_ms);
    const auto keep = state.stats;
    state.stats = loaded.stats;
    state.stats.disabled = keep.disabled;
    state.stats.opened = true;
    state.stats.open_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - open_started).count());
}

std::optional<std::vector<std::uint8_t>> DerivedStore::lookup(const std::uint8_t kind, const DerivedKey& key,
                                                              const DerivedStamp& stamp) {
    ensure_open();
    std::filesystem::path path;
    std::uint64_t frame_start = 0, frame_size = 0;
    {
        std::lock_guard lock(state_->mutex);
        auto& state = *state_;
        ++state.stats.lookups;
        const auto found = state.index.find(index_key(kind, key));
        if (found == state.index.end()) {
            ++state.stats.misses_absent;
            return std::nullopt;
        }
        const auto& entry = found->second;
        if (!(entry.stamp == stamp)) {
            ++state.stats.misses_stale;
            return std::nullopt;
        }
        const auto& segment = *state.segments[entry.segment];
        if (!segment.bytes.empty()) { // a record not written yet is still in memory
            ++state.stats.hits;
            return std::vector<std::uint8_t>(segment.bytes.begin() + static_cast<std::ptrdiff_t>(entry.body_offset),
                segment.bytes.begin() + static_cast<std::ptrdiff_t>(entry.body_offset + entry.body_size));
        }
        path = folder_of(options_) / segment.name;
        frame_start = frame_start_of(entry);
        frame_size = frame_size_of(entry);
    }
    // Everything else is read from the segment file and its frame is checked again: the file may have changed since the
    // open scan, and a flipped bit must never come back as a record.
    std::vector<std::uint8_t> frame;
    const bool good = read_range(path, frame_start, frame_size, frame) && frame_is(frame, kind, key, stamp);
    std::lock_guard lock(state_->mutex);
    if (!good) {
        ++state_->stats.misses_unreadable;
        return std::nullopt;
    }
    ++state_->stats.hits;
    return std::vector<std::uint8_t>(frame.begin() + static_cast<std::ptrdiff_t>(frame_head_size + payload_fixed_size), frame.end());
}

bool DerivedStore::contains(const std::uint8_t kind, const DerivedKey& key, const DerivedStamp& stamp) {
    ensure_open();
    std::lock_guard lock(state_->mutex);
    const auto found = state_->index.find(index_key(kind, key));
    return found != state_->index.end() && found->second.stamp == stamp;
}

bool DerivedStore::put(const std::uint8_t kind, const DerivedKey& key, const DerivedStamp& stamp,
                       const std::span<const std::uint8_t> body) {
    ensure_open();
    std::unique_lock lock(state_->mutex);
    auto& state = *state_;
    const std::uint64_t frame_bytes = frame_head_size + payload_fixed_size + body.size();
    if (payload_fixed_size + body.size() > options_.max_record_bytes
        || (options_.folder_utf8.empty() && state.resident_bytes + frame_bytes > options_.max_memory_only_bytes)) {
        ++state.stats.puts_refused;
        return false;
    }
    if (state.open_segment < 0) {
        auto segment = std::make_unique<State::Segment>();
        segment->created_ms = next_created_ms(state);
        segment->name = new_segment_name(segment->created_ms);
        segment->own = true;
        segment->bytes = make_header_bytes(options_, segment->created_ms);
        state.resident_bytes += segment->bytes.size();
        state.segments.push_back(std::move(segment));
        state.open_segment = static_cast<std::int64_t>(state.segments.size() - 1);
    }
    auto& target = *state.segments[static_cast<std::size_t>(state.open_segment)];
    // The payload's size is known up front, so it is sized once and filled in place. (Growing a reserved vector with
    // push_back made GCC 14 report a false -Wfree-nonheap-object from the vector's never-taken reallocation path.)
    std::vector<std::uint8_t> payload(payload_fixed_size + body.size());
    {
        std::uint8_t* at = payload.data();
        *at++ = kind;
        at = std::copy(key.begin(), key.end(), at);
        at = store_u64(at, stamp.container_size);
        at = store_u64(at, static_cast<std::uint64_t>(stamp.container_mtime));
        at = store_u64(at, stamp.entry_offset);
        at = store_u64(at, stamp.entry_size);
        std::copy(body.begin(), body.end(), at);
    }
    const auto check = check16(payload.data(), payload.size());
    const std::size_t frame_at = target.bytes.size();
    put_u32(target.bytes, static_cast<std::uint32_t>(payload.size()));
    target.bytes.insert(target.bytes.end(), check.begin(), check.end());
    target.bytes.insert(target.bytes.end(), payload.begin(), payload.end());
    state.resident_bytes += frame_head_size + payload.size();
    State::Entry entry;
    entry.segment = static_cast<std::uint32_t>(state.open_segment);
    entry.body_offset = frame_at + frame_head_size + payload_fixed_size;
    entry.body_size = body.size();
    entry.order = ++state.order;
    entry.stamp = stamp;
    state.index.insert_or_assign(index_key(kind, key), entry);
    ++state.stats.puts;
    if (state.pending_bytes == 0) state.pending_since = std::chrono::steady_clock::now();
    state.pending_bytes += frame_head_size + payload.size();
    if (options_.writer_thread && !state.writer.joinable()) {
        state.stopping = false;
        state.writer = std::thread([this] { writer_loop(); });
    }
    if (state.pending_bytes >= options_.flush_bytes) state.wake.notify_all();
    else if (state.pending_bytes == frame_head_size + payload.size()) state.wake.notify_all();
    return true;
}

void DerivedStore::writer_loop() {
    std::unique_lock lock(state_->mutex);
    auto& state = *state_;
    while (!state.stopping) {
        if (state.pending_bytes == 0) {
            state.wake.wait(lock, [&] { return state.stopping || state.pending_bytes != 0; });
            continue;
        }
        const auto due = state.pending_since + options_.flush_interval;
        if (state.pending_bytes >= options_.flush_bytes || std::chrono::steady_clock::now() >= due) {
            write_pending(lock);
            continue;
        }
        state.wake.wait_until(lock, due);
    }
}

void DerivedStore::write_pending(std::unique_lock<std::mutex>& lock) {
    auto& state = *state_;
    state.idle.wait(lock, [&] { return !state.flushing; });
    if (state.open_segment < 0 || state.pending_bytes == 0) return;
    const auto id = static_cast<std::uint32_t>(state.open_segment);
    State::Segment* segment = state.segments[id].get();
    state.open_segment = -1;
    state.pending_bytes = 0;
    state.flushing = true;
    const auto folder = folder_of(options_);
    lock.unlock();
    bool written = false;
    if (!folder.empty()) {
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        const auto target = folder / segment->name;
        auto temporary = target;
        temporary += ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (out) {
                out.write(reinterpret_cast<const char*>(segment->bytes.data()), static_cast<std::streamsize>(segment->bytes.size()));
                out.flush();
                written = static_cast<bool>(out);
            }
        }
        if (written) {
            std::filesystem::rename(temporary, target, error);
            written = !error;
        }
        if (!written) std::filesystem::remove(temporary, error);
    }
    lock.lock();
    ++state.stats.flushes;
    if (!folder.empty()) {
        if (written) {
            segment->persisted = true;
            segment->file_size = segment->bytes.size();
            state.stats.bytes_written += segment->bytes.size();
            // From here a lookup reads this segment's records from the file; the bytes are not kept.
            state.resident_bytes -= std::min<std::uint64_t>(state.resident_bytes, segment->bytes.size());
            segment->bytes.clear();
            segment->bytes.shrink_to_fit();
        } else {
            // The folder cannot be written (read-only, full): drop the records instead of holding them for the session.
            ++state.stats.flush_failures;
            drop_segment(state, id);
        }
    }
    state.flushing = false;
    state.idle.notify_all();
    if (written) compact(lock);
}

namespace {

// Which of the other builds' segments (and unreadable files) this store deletes. Other builds are kept while they are
// recent and within a byte budget (newest first), so builds that share a folder do not push each other's records out.
std::vector<State::StaleFile> foreign_to_delete(const State& state, const DerivedStoreOptions& options, const std::uint64_t now) {
    auto sorted = state.stale;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.created_ms > b.created_ms; });
    const auto keep_ms = static_cast<std::uint64_t>(std::max<std::int64_t>(0, options.foreign_keep_age.count()));
    std::vector<State::StaleFile> doomed;
    std::uint64_t kept_bytes = 0;
    for (const auto& file : sorted) {
        const std::uint64_t age = now > file.created_ms ? now - file.created_ms : 0;
        if (file.bad || age > keep_ms || kept_bytes + file.size > options.max_foreign_bytes) doomed.push_back(file);
        else kept_bytes += file.size;
    }
    return doomed;
}

} // namespace

// Two jobs, both bounded to what this process may touch:
//  - merge the current-epoch segments this process wrote and the ones it loaded that have aged (adopt_age) into one, when
//    there are too many or the folder has passed its size cap (at most once per process). A younger loaded segment is left:
//    a concurrent process of the same build may be reading it. The copy streams frame by frame (one segment's records at
//    a time in memory) and runs without the lock; a failure leaves the old segments in place.
//  - delete other builds' segments only by the foreign rules (foreign_to_delete); never merge or rewrite them.
void DerivedStore::compact(std::unique_lock<std::mutex>& lock) {
    auto& state = *state_;
    if (options_.folder_utf8.empty()) return;
    const auto started = std::chrono::steady_clock::now();
    const std::uint64_t now = now_ms();
    const auto adopt_ms = static_cast<std::uint64_t>(std::max<std::int64_t>(0, options_.adopt_age.count()));
    const auto mergeable = [&](const State::Segment& segment) {
        return segment.persisted && !segment.dead && (segment.own || (now > segment.created_ms ? now - segment.created_ms : 0) >= adopt_ms);
    };
    std::uint64_t mergeable_bytes = 0, other_bytes = 0;
    unsigned mergeable_count = 0;
    for (const auto& segment : state.segments) {
        if (!segment->persisted || segment->dead) continue;
        if (mergeable(*segment)) { mergeable_bytes += segment->file_size; ++mergeable_count; }
        else other_bytes += segment->file_size;
    }
    const bool merge_needed = !state.compacted && mergeable_count > 0
        && (mergeable_count > options_.compact_after_segments || mergeable_bytes + other_bytes > options_.max_total_bytes);
    if (!merge_needed && foreign_to_delete(state, options_, now).empty()) return;

    struct Move { std::string key; std::uint32_t old_segment; std::uint64_t old_offset; std::uint64_t new_offset; };
    std::vector<Move> moves;
    std::vector<std::uint32_t> snapshot;
    std::vector<std::filesystem::path> snapshot_paths;
    std::error_code error;
    const auto folder = folder_of(options_);
    std::uint64_t merged_size = 0;
    std::string name;
    std::uint64_t created = 0;
    bool written = false;
    if (merge_needed) {
        state.compacted = true;
        struct Live { std::uint64_t order; std::string key; State::Entry entry; };
        std::vector<Live> live;
        live.reserve(state.index.size());
        for (const auto& [key, entry] : state.index) {
            if (mergeable(*state.segments[entry.segment])) live.push_back({entry.order, key, entry});
        }
        std::sort(live.begin(), live.end(), [](const Live& a, const Live& b) { return a.order > b.order; });
        // The segments left alone count against the cap, so the folder as a whole stays within it.
        const std::uint64_t budget = options_.max_total_bytes > other_bytes ? options_.max_total_bytes - other_bytes : 0;
        std::uint64_t kept_bytes = header_size;
        std::size_t kept = 0;
        for (; kept < live.size(); ++kept) {
            const auto frame = frame_size_of(live[kept].entry);
            if (kept_bytes + frame > budget) break;
            kept_bytes += frame;
        }
        live.resize(kept);
        // Read in file order (one open file at a time); the old segments are the snapshot taken here.
        std::sort(live.begin(), live.end(), [](const Live& a, const Live& b) {
            return a.entry.segment != b.entry.segment ? a.entry.segment < b.entry.segment : a.entry.body_offset < b.entry.body_offset;
        });
        for (std::uint32_t id = 0; id < state.segments.size(); ++id) {
            const auto& segment = *state.segments[id];
            if (!mergeable(segment)) continue;
            snapshot.push_back(id);
            snapshot_paths.push_back(folder / segment.name);
        }
        created = next_created_ms(state);
        name = new_segment_name(created);
        lock.unlock();

        const auto target = folder / name;
        auto temporary = target;
        temporary += ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (out) {
                const auto header = make_header_bytes(options_, created);
                out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
                merged_size = header.size();
                std::ifstream in;
                std::uint32_t open_id = std::numeric_limits<std::uint32_t>::max();
                std::vector<std::uint8_t> frame;
                for (const auto& item : live) {
                    if (item.entry.segment != open_id) {
                        in.close();
                        in.clear();
                        const auto at = std::find(snapshot.begin(), snapshot.end(), item.entry.segment);
                        in.open(snapshot_paths[static_cast<std::size_t>(at - snapshot.begin())], std::ios::binary);
                        open_id = item.entry.segment;
                    }
                    if (!in) continue;
                    frame.resize(static_cast<std::size_t>(frame_size_of(item.entry)));
                    in.clear();
                    in.seekg(static_cast<std::streamoff>(frame_start_of(item.entry)), std::ios::beg);
                    in.read(reinterpret_cast<char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
                    if (!in || !frame_intact(frame)) continue; // a damaged record is not carried over
                    out.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
                    moves.push_back({item.key, item.entry.segment, item.entry.body_offset, merged_size + frame_head_size + payload_fixed_size});
                    merged_size += frame.size();
                }
                out.flush();
                written = static_cast<bool>(out);
            }
        }
        if (written) {
            std::filesystem::rename(temporary, target, error);
            written = !error;
        }
        if (!written) std::filesystem::remove(temporary, error);
        lock.lock();
        if (!written) ++state.stats.flush_failures;
    }
    if (written) {
        auto merged = std::make_unique<State::Segment>();
        merged->name = name;
        merged->created_ms = created;
        merged->file_size = merged_size;
        merged->persisted = true;
        merged->own = true;
        const auto merged_id = static_cast<std::uint32_t>(state.segments.size());
        state.segments.push_back(std::move(merged));
        state.stats.bytes_written += merged_size;
        ++state.stats.compactions;
        // A record put again while the copy ran lives in a newer segment and keeps its entry; every other entry of a merged
        // segment moves to the merged file, or goes when it was not carried over.
        for (const auto& move : moves) {
            const auto found = state.index.find(move.key);
            if (found == state.index.end() || found->second.segment != move.old_segment || found->second.body_offset != move.old_offset) continue;
            found->second.segment = merged_id;
            found->second.body_offset = move.new_offset;
        }
        for (std::size_t i = 0; i < snapshot.size(); ++i) {
            std::filesystem::remove(snapshot_paths[i], error);
            if (!error) ++state.stats.segments_removed;
            drop_segment(state, snapshot[i]);
        }
    }
    // The other builds' files: bad ones are garbage; the rest go by age and byte budget only.
    const auto doomed = foreign_to_delete(state, options_, now);
    std::vector<State::StaleFile> remaining;
    for (const auto& file : state.stale) {
        const bool gone = std::any_of(doomed.begin(), doomed.end(), [&](const auto& item) { return item.path == file.path; });
        if (gone) {
            std::filesystem::remove(file.path, error);
            if (!error) ++state.stats.segments_removed;
        } else remaining.push_back(file);
    }
    state.stale = std::move(remaining);
    state.stats.compact_us += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
}

void DerivedStore::flush() {
    {
        // Nothing queued: leave the folder alone (a view that never reads or writes records must not open it either).
        std::lock_guard lock(state_->mutex);
        if (state_->pending_bytes == 0 && state_->open_segment < 0 && !state_->flushing) return;
    }
    ensure_open();
    std::unique_lock lock(state_->mutex);
    write_pending(lock);
}

void DerivedStore::finish() {
    flush();
    std::thread writer;
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        writer = std::move(state_->writer);
    }
    state_->wake.notify_all();
    if (writer.joinable()) writer.join();
    std::lock_guard lock(state_->mutex);
    state_->stopping = false;
}

DerivedStoreStats DerivedStore::stats() const {
    std::lock_guard lock(state_->mutex);
    auto result = state_->stats;
    result.index_entries = state_->index.size();
    result.bytes_resident = state_->resident_bytes;
    for (const auto& segment : state_->segments)
        if (segment->persisted && !segment->dead) {
            result.bytes_on_disk += segment->file_size;
            ++result.segments_on_disk;
        }
    for (const auto& file : state_->stale) {
        ++result.segments_foreign;
        result.bytes_foreign += file.size;
    }
    std::map<std::uint8_t, DerivedStoreStats::Kind> kinds;
    for (const auto& [key, entry] : state_->index) {
        auto& item = kinds[static_cast<std::uint8_t>(key.front())];
        item.kind = static_cast<std::uint8_t>(key.front());
        ++item.records;
        item.body_bytes += entry.body_size;
    }
    for (const auto& entry : kinds) result.kinds.push_back(entry.second);
    return result;
}

std::string format_derived_store_stats(const DerivedStoreStats& stats) {
    std::string out = "derived-store:";
    const auto field = [&out](const char* name, const std::uint64_t value) { out += std::string(" ") + name + "=" + std::to_string(value); };
    field("opened", stats.opened ? 1U : 0U);
    field("records", stats.index_entries);
    field("on_disk_bytes", stats.bytes_on_disk);
    field("segments", stats.segments_on_disk);
    field("foreign_segments", stats.segments_foreign);
    field("foreign_bytes", stats.bytes_foreign);
    field("resident_bytes", stats.bytes_resident);
    field("open_us", stats.open_us);
    field("lookups", stats.lookups);
    field("hits", stats.hits);
    field("misses_absent", stats.misses_absent);
    field("misses_stale", stats.misses_stale);
    field("misses_unreadable", stats.misses_unreadable);
    field("puts", stats.puts);
    field("puts_refused", stats.puts_refused);
    field("flushes", stats.flushes);
    field("flush_failures", stats.flush_failures);
    field("bytes_written", stats.bytes_written);
    field("compactions", stats.compactions);
    field("compact_us", stats.compact_us);
    field("segments_removed", stats.segments_removed);
    for (const auto& item : stats.kinds) {
        out += " kind" + std::to_string(item.kind) + "_records=" + std::to_string(item.records);
        out += " kind" + std::to_string(item.kind) + "_body_bytes=" + std::to_string(item.body_bytes);
    }
    return out;
}


} // namespace eawr::platform
