// Contracts of the perf telemetry feed writer (#8058, P6): off costs nothing, the JSONL feed carries the documented
// records, a full ring drops with a counter instead of blocking, segments rotate, a capture is one balanced Chrome trace
// JSON, and the PC context parsers read the Linux text forms. Real files in a temporary directory; no game data.

#include "eawr/core/perf_diag/feed_writer.hpp"
#include "eawr/core/perf_diag/pc_context.hpp"
#include "eawr/core/perf_diag/sim_feed.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace eawr::core::perf_diag;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

[[nodiscard]] std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

[[nodiscard]] std::size_t count_of(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++count;
    return count;
}

[[nodiscard]] std::vector<fs::path> segments_in(const fs::path& dir) {
    std::vector<fs::path> found;
    for (const auto& entry : fs::directory_iterator(dir))
        if (entry.path().filename().string().rfind("eawr-perf-feed-", 0) == 0) found.push_back(entry.path());
    std::sort(found.begin(), found.end());
    return found;
}

FeedOptions options_for(const fs::path& dir, const std::chrono::milliseconds batch = 10ms) {
    FeedOptions options;
    options.directory = dir.string();
    options.batch = batch;
    return options;
}

fs::path fresh_directory(const std::string& name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("eawr-perf-feed-test-" + name + "-" + std::to_string(stamp));
    fs::remove_all(dir);
    return dir;
}

// One session worth of records: phases first, then the tick record (the order the sessions push).
void produce(SimFeed& feed, const std::uint64_t first, const std::uint64_t count, const std::uint64_t slow_tick = 0) {
    const PhaseId step = feed.intern_static("step");
    const PhaseId command = feed.intern_static("command-source");
    for (std::uint64_t n = first; n < first + count; ++n) {
        const bool slow = n == slow_tick;
        feed.push_phase(n, step, 8.0);
        feed.push_phase(n, command, slow ? 60.0 : 1.0);
        TickRecord tick;
        tick.tick = n;
        tick.wall_us = 1'000'000 + n * 33'300;
        tick.total_ms = slow ? 70.0F : 9.0F;
        tick.interval_ms = 33.3F;
        tick.cpu_ratio = 0.9F;
        tick.phase_count = 2;
        feed.push_tick(tick);
        if (n % 50 == 0) feed.mark(MarkerKind::save, n);
    }
}

void off_contracts() {
    const fs::path dir = fresh_directory("off");
    expect(!FeedWriter::active(), "no writer is active by default");
    SimFeed feed;
    expect(FeedWriter::attach_active(feed, "tactical") == -1, "attach without a writer returns -1");
    {
        FeedAttachment attachment(feed, "tactical");
        expect(!feed.enabled() && !feed.allocated(), "an attachment without a writer leaves the feed off and unallocated");
    }
    FeedWriter::request_capture_active(); // no writer: nothing happens
    FeedWriter::push_frame_active(FrameRecord{});
    FeedWriter writer(options_for(dir));
    expect(!writer.running() && !fs::exists(dir), "a writer that was never started creates no file and no directory");
}

void feed_contracts() {
    const fs::path dir = fresh_directory("on");
    SimFeed feed;
    {
        FeedOptions options = options_for(dir);
        options.build = "test-build";
        options.engine = "test-engine";
        FeedWriter writer(options);
        std::string error;
        expect(writer.start(error), "the writer starts: " + error);
        expect(FeedWriter::active(), "a started writer is the active one");
        FeedWriter second(options_for(dir));
        expect(!second.start(error), "a second writer is refused");
        {
            FeedAttachment attachment(feed, "tactical");
            expect(feed.enabled(), "an attachment enables the feed");
            produce(feed, 1, 120, 100);
            for (std::uint64_t frame = 1; frame <= 40; ++frame)
                FeedWriter::push_frame_active(FrameRecord{frame, 1'000'000 + frame * 16'000, 16.0F, 5.0F, 6.0F, 14.0F, 0.0F, 0, 0, 0});
            FeedWriter::request_capture_active();
            for (int wait = 0; wait < 300 && writer.captures_written() == 0; ++wait) std::this_thread::sleep_for(10ms);
            expect(writer.captures_written() == 1, "a requested capture is written by the writer thread");
        }
        expect(!feed.enabled(), "detaching switches the feed off");
        writer.stop();
        expect(!FeedWriter::active() && !writer.running(), "stop ends the active writer");
        writer.stop(); // twice is fine
        expect(writer.write_failures() == 0, "no write failed");
        expect(writer.records_written() > 120, "records were counted");
    }
    const auto segments = segments_in(dir);
    expect(segments.size() == 1, "one segment for a short run");
    const std::string feed_text = slurp(segments.front());
    expect(feed_text.rfind("{\"r\":\"header\",\"schema\":1", 0) == 0, "the feed starts with the versioned header");
    expect(feed_text.find("\"build\":\"test-build\"") != std::string::npos && feed_text.find("\"engine\":\"test-engine\"") != std::string::npos,
           "the header names the build and the engine");
    expect(feed_text.find("{\"r\":\"source\",\"src\":0,\"label\":\"tactical\"") != std::string::npos, "the source is announced");
    expect(feed_text.find("\"r\":\"phase-name\",\"src\":0,\"id\":0,\"name\":\"step\"") != std::string::npos, "phase names are announced once");
    expect(count_of(feed_text, "\"name\":\"step\"") == 1, "a phase name is announced once per segment");
    expect(count_of(feed_text, "{\"r\":\"tick\"") == 120, "every tick is a record");
    expect(feed_text.find("\"n\":7,") != std::string::npos && feed_text.find("\"ph\":[[0,8.000],[1,1.000]]") != std::string::npos,
           "ticks carry their phases by id");
    expect(count_of(feed_text, "{\"r\":\"marker\"") == 2 && feed_text.find("\"k\":\"save\",\"n\":50") != std::string::npos, "markers are records");
    expect(count_of(feed_text, "{\"r\":\"frame\"") == 40, "frames pushed by the frame side are records");
    expect(count_of(feed_text, "\"r\":\"hitch\"") == 1 && feed_text.find("\"kind\":\"tick\",\"n\":100,") != std::string::npos
               && feed_text.find("\"phase\":\"command-source\"") != std::string::npos,
           "the slow tick is one hitch, attributed to the phase with the excess");
    expect(feed_text.find("\"markers\":[") != std::string::npos, "the hitch lists its markers");
    expect(feed_text.find("{\"r\":\"source-end\"") != std::string::npos, "the end of a source is a record");
    expect(feed_text.find('\r') == std::string::npos, "records end in a bare newline");

    const fs::path captures = dir / "captures";
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(captures)) files.push_back(entry.path());
    expect(files.size() == 1, "one capture file");
    const std::string trace = slurp(files.front());
    expect(trace.rfind("{\"traceEvents\":[", 0) == 0, "a capture is a Chrome trace-event object");
    expect(count_of(trace, "{") == count_of(trace, "}") && count_of(trace, "[") == count_of(trace, "]"), "the capture JSON is balanced");
    expect(count_of(trace, "\"name\":\"tick\"") == 120 && count_of(trace, "\"name\":\"hitch\"") == 1 && count_of(trace, "\"name\":\"save\"") == 2,
           "a capture holds the ticks, the hitch and the markers");
    expect(count_of(trace, "\"name\":\"frame\"") == 40, "a capture holds the frames");
    expect(trace.find("\"otherData\":{\"schema\":1") != std::string::npos, "a capture names its schema");
}

void drop_and_rotation_contracts() {
    const fs::path dir = fresh_directory("drop");
    SimFeed feed;
    {
        // A slow writer (5 s wake-ups): a burst larger than the ring overwrites the oldest records and counts them.
        FeedWriter writer(options_for(dir, 5000ms));
        std::string error;
        expect(writer.start(error), "drop writer starts: " + error);
        {
            FeedAttachment attachment(feed, "galactic");
            const auto begin = std::chrono::steady_clock::now();
            for (std::uint64_t n = 1; n <= 3 * SimFeed::tick_capacity; ++n) {
                TickRecord tick;
                tick.tick = n;
                tick.wall_us = n * 33'300;
                tick.total_ms = 9.0F;
                feed.push_tick(tick);
            }
            expect(std::chrono::steady_clock::now() - begin < 5s, "the producer never waits for the writer");
        }
        // The consumer counts what it skipped when it reads; detaching drained the ring.
        expect(feed.dropped() >= 2 * SimFeed::tick_capacity, "a full ring drops with a counter");
    }
    const std::string text = slurp(segments_in(dir).front());
    expect(count_of(text, "{\"r\":\"tick\"") <= SimFeed::tick_capacity, "no more ticks than the ring held reach the feed");

    const fs::path rot = fresh_directory("rotate");
    SimFeed other;
    {
        FeedOptions options = options_for(rot, 5ms);
        options.rotate_bytes = 4096;
        options.keep_segments = 2;
        FeedWriter writer(options);
        std::string error;
        expect(writer.start(error), "rotation writer starts: " + error);
        FeedAttachment attachment(other, "tactical");
        for (std::uint64_t round = 0; round < 40; ++round) {
            produce(other, 1 + round * 50, 50);
            std::this_thread::sleep_for(8ms);
        }
    }
    const auto segments = segments_in(rot);
    expect(segments.size() == 2, "only the newest keep_segments segments stay: " + std::to_string(segments.size()));
    for (const auto& segment : segments) {
        const std::string text2 = slurp(segment);
        expect(text2.rfind("{\"r\":\"header\"", 0) == 0, "every segment starts with a header");
        expect(text2.find("\"r\":\"source\"") != std::string::npos && text2.find("\"r\":\"phase-name\"") != std::string::npos,
               "every segment re-announces its sources and phase names");
    }
}

void prior_run_retention_contracts() {
    const fs::path dir = fresh_directory("prior-runs");
    fs::create_directories(dir);
    for (int n = 1; n <= 5; ++n) {
        const std::string name = "eawr-perf-feed-000" + std::to_string(n) + ".jsonl";
        std::ofstream(dir / name) << "old";
    }
    std::ofstream(dir / "eawr-perf-feed-not-a-segment.jsonl") << "unrelated";
    FeedOptions options = options_for(dir);
    options.keep_segments = 2;
    FeedWriter writer(options);
    std::string error;
    expect(writer.start(error), "writer starts with old segments: " + error);
    writer.stop();
    expect(!fs::exists(dir / "eawr-perf-feed-0004.jsonl") && fs::exists(dir / "eawr-perf-feed-0005.jsonl")
               && fs::exists(dir / "eawr-perf-feed-0006.jsonl"), "oldest prior segments are pruned at startup");
    expect(fs::exists(dir / "eawr-perf-feed-not-a-segment.jsonl"), "unrelated files are preserved");
}

void capture_does_not_block_attachment() {
    const fs::path dir = fresh_directory("capture-lock");
    std::atomic<bool> writing{false}, release{false}, attached_and_detached{false};
    FeedOptions options = options_for(dir);
    struct WriteGate { std::atomic<bool>& writing; std::atomic<bool>& release; } gate{writing, release};
    options.capture_before_write_context = &gate;
    options.capture_before_write = [](void* context) {
        auto& gate = *static_cast<WriteGate*>(context);
        gate.writing.store(true, std::memory_order_release);
        while (!gate.release.load(std::memory_order_acquire)) std::this_thread::yield();
    };
    FeedWriter writer(options);
    std::string error;
    expect(writer.start(error), "capture lock writer starts: " + error);
    FeedWriter::request_capture_active();
    for (int n = 0; n < 300 && !writing.load(std::memory_order_acquire); ++n) std::this_thread::sleep_for(10ms);
    expect(writing.load(std::memory_order_acquire), "capture reaches the write boundary");
    std::thread session([&] {
        SimFeed feed;
        { FeedAttachment attachment(feed, "tactical"); }
        attached_and_detached.store(true, std::memory_order_release);
    });
    for (int n = 0; n < 300 && !attached_and_detached.load(std::memory_order_acquire); ++n) std::this_thread::sleep_for(10ms);
    expect(attached_and_detached.load(std::memory_order_acquire), "attach and detach complete while capture writing is paused");
    release.store(true, std::memory_order_release);
    session.join();
    writer.stop();
}

void pc_contracts() {
    const auto cpu = parse_proc_stat_cpu("cpu  100 0 50 800 50 0 0 0 0 0\ncpu0 1 2 3\n");
    expect(cpu && cpu->first == 150 && cpu->second == 1000, "proc stat: busy excludes idle and iowait");
    expect(!parse_proc_stat_cpu("intr 1 2 3"), "not a cpu line");
    const auto own = parse_proc_self_stat_cpu("12 (a b) c) S 1 2 3 4 5 6 7 8 9 10 40 25 0 0 20 0 1 0 100 1 2\n");
    expect(own && *own == 65, "self stat: utime + stime after a comm with spaces and a bracket");
    expect(!parse_proc_self_stat_cpu("garbage"), "self stat without a comm");
    const std::string meminfo = "MemTotal:       16384 kB\nMemAvailable:    8192 kB\nCommitLimit:    20000 kB\n";
    expect(parse_meminfo_kib(meminfo, "MemAvailable") == 8192u && parse_meminfo_kib(meminfo, "CommitLimit") == 20000u, "meminfo values");
    expect(!parse_meminfo_kib(meminfo, "Committed_AS"), "a missing key");

    PcCounters before{true, 1000, 2000, 100, 0, 0, 0};
    PcCounters after{true, 1600, 3000, 300, 5000, 10000, 4096};
    const PcSample sample = pc_sample_between(before, after);
    expect(std::abs(sample.system_busy - 0.6F) < 1e-5F && std::abs(sample.own_busy - 0.2F) < 1e-5F && std::abs(sample.others_busy - 0.4F) < 1e-5F,
           "others = system - own");
    expect(std::abs(sample.commit_ratio - 0.5F) < 1e-5F && sample.memory_available_mib == 4, "commit ratio and available memory");
    const PcSample backwards = pc_sample_between(after, before);
    expect(backwards.system_busy < 0.0F && backwards.others_busy < 0.0F, "counters that went backwards are 'not measured'");
    const PcCounters now = read_pc_counters();
    if (now.valid) expect(now.system_total > 0, "the live counters read something where the platform offers them");
}

} // namespace

int main() {
    off_contracts();
    feed_contracts();
    drop_and_rotation_contracts();
    prior_run_retention_contracts();
    capture_does_not_block_attachment();
    pc_contracts();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "perf_diag feed contracts ok\n";
    return 0;
}
