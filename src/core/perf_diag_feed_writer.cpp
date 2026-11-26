#include "eawr/core/perf_diag/feed_writer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <limits>
#include <span>
#include <system_error>
#include <thread>
#include <vector>

#include "eawr/core/json_text.hpp"
#include "eawr/core/perf_diag/hitch.hpp"
#include "eawr/core/perf_diag/marker.hpp"
#include "eawr/core/perf_diag/pattern.hpp"
#include "eawr/core/perf_diag/pc_context.hpp"
#include "eawr/core/perf_diag/thread_cpu.hpp"
#include "eawr/core/perf_diag/verdict.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace eawr::core::perf_diag {

namespace {

std::filesystem::path to_path(const std::string& utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}
std::string from_path(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

using Clock = std::chrono::steady_clock;

std::uint64_t now_us() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

// --- JSON text helpers (numbers are always finite: a NaN or infinity would break every reader) ---
void put_u(std::string& out, const std::uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof text, "%llu", static_cast<unsigned long long>(value));
    out += text;
}
void put_i(std::string& out, const std::int64_t value) {
    char text[32];
    std::snprintf(text, sizeof text, "%lld", static_cast<long long>(value));
    out += text;
}
void put_f(std::string& out, const double value, const int digits = 3) {
    char text[48];
    std::snprintf(text, sizeof text, "%.*f", digits, std::isfinite(value) ? value : 0.0);
    out += text;
}
void put_s(std::string& out, const std::string_view value) { out += json_quote(value); }

constexpr std::size_t history_ticks = 32768;
constexpr std::size_t history_phases = 524288;
constexpr std::size_t history_markers = 8192;
constexpr std::size_t history_frames = 32768;
constexpr std::size_t history_hitches = 256;

[[nodiscard]] float percentile95(std::vector<float>& values) {
    if (values.empty()) return 0.0F;
    const std::size_t index = std::min(values.size() - 1, static_cast<std::size_t>(std::lround(0.95 * static_cast<double>(values.size() - 1))));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}

void lower_this_thread_priority() noexcept {
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__)
    setpriority(PRIO_PROCESS, 0, 5); // on Linux the target 0 is the calling thread
#endif
}

[[nodiscard]] std::string utc_stamp() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm parts{};
#if defined(_WIN32)
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y%m%d-%H%M%S", &parts);
    return text;
}

std::mutex& active_mutex() {
    static std::mutex mutex;
    return mutex;
}
std::atomic<FeedWriter*>& active_pointer() {
    static std::atomic<FeedWriter*> pointer{nullptr};
    return pointer;
}

HitchMode mode_of(const std::string& label, const std::uint16_t flags) noexcept {
    if (label == "galactic") return HitchMode::galactic;
    return (flags & TickFlags::land) != 0 ? HitchMode::tactical_land : HitchMode::tactical_space;
}
std::string_view mode_name(const HitchMode mode) noexcept {
    switch (mode) {
    case HitchMode::tactical_space: return "tactical-space";
    case HitchMode::tactical_land: return "tactical-land";
    case HitchMode::galactic: return "galactic";
    case HitchMode::viewer: return "viewer";
    }
    return "unknown";
}

} // namespace

PcCounters read_pc_counters() noexcept {
    PcCounters out;
#if defined(_WIN32)
    FILETIME idle{}, kernel{}, user{}, created{}, exited{}, own_kernel{}, own_user{};
    if (!GetSystemTimes(&idle, &kernel, &user) || !GetProcessTimes(GetCurrentProcess(), &created, &exited, &own_kernel, &own_user))
        return out;
    const auto value = [](const FILETIME& t) { return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    out.system_total = value(kernel) + value(user); // the kernel time includes the idle time
    out.system_busy = out.system_total - value(idle);
    out.own = value(own_kernel) + value(own_user);
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof memory;
    if (GlobalMemoryStatusEx(&memory)) {
        out.commit_limit = memory.ullTotalPageFile;
        out.commit_used = memory.ullTotalPageFile - memory.ullAvailPageFile;
        out.memory_available_kib = memory.ullAvailPhys / 1024;
    }
    out.valid = true;
#elif defined(__linux__)
    const auto slurp = [](const char* path) {
        std::string text;
        std::ifstream in(path, std::ios::binary);
        char buffer[4096];
        while (in.read(buffer, sizeof buffer) || in.gcount() > 0) {
            text.append(buffer, static_cast<std::size_t>(in.gcount()));
            if (text.size() > (1U << 20)) break;
        }
        return text;
    };
    const auto cpu = parse_proc_stat_cpu(slurp("/proc/stat"));
    const auto own = parse_proc_self_stat_cpu(slurp("/proc/self/stat"));
    if (!cpu || !own) return out;
    out.system_busy = cpu->first;
    out.system_total = cpu->second;
    out.own = *own;
    const std::string meminfo = slurp("/proc/meminfo");
    if (const auto limit = parse_meminfo_kib(meminfo, "CommitLimit")) out.commit_limit = *limit * 1024;
    if (const auto used = parse_meminfo_kib(meminfo, "Committed_AS")) out.commit_used = *used * 1024;
    if (const auto available = parse_meminfo_kib(meminfo, "MemAvailable")) out.memory_available_kib = *available;
    out.valid = true;
#endif
    return out;
}

namespace {

// This process's memory in MiB and its page-fault count (a cumulative counter); valid = false where unreadable.
struct ProcessMemory final {
    bool valid{};
    std::uint64_t commit_mib{};      // private bytes the process committed
    std::uint64_t working_set_mib{}; // resident in RAM
    std::uint64_t page_faults{};     // cumulative, soft and hard
};

ProcessMemory read_process_memory() noexcept {
    ProcessMemory out;
#if defined(_WIN32)
    using Function = BOOL(WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    static const Function query = reinterpret_cast<Function>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo")));
    if (query == nullptr) return out;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof counters;
    if (!query(GetCurrentProcess(), reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&counters), sizeof counters)) return out;
    out.commit_mib = static_cast<std::uint64_t>(counters.PrivateUsage) >> 20;
    out.working_set_mib = static_cast<std::uint64_t>(counters.WorkingSetSize) >> 20;
    out.page_faults = counters.PageFaultCount;
    out.valid = true;
#elif defined(__linux__)
    std::ifstream in("/proc/self/statm", std::ios::binary);
    std::uint64_t size = 0, resident = 0;
    if (!(in >> size >> resident)) return out;
    const long page = sysconf(_SC_PAGESIZE);
    const std::uint64_t bytes = page > 0 ? static_cast<std::uint64_t>(page) : 4096;
    out.commit_mib = (size * bytes) >> 20;
    out.working_set_mib = (resident * bytes) >> 20;
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) out.page_faults = static_cast<std::uint64_t>(usage.ru_minflt + usage.ru_majflt);
    out.valid = true;
#endif
    return out;
}

} // namespace

struct FeedWriter::Impl {
    // One attached session feed and everything the writer derives from it.
    struct PendingHitch final {
        HitchRecord record;
        PhaseId phase{no_phase};
        std::uint16_t flags{};
    };
    struct WindowTick final {
        std::uint64_t wall_us{};
        float total_ms{};
        float interval_ms{};
        float session_lock_ms{};
        float cpu_ratio{cpu_ratio_unknown};
        PhaseId top_phase{no_phase};
        float top_ms{};
    };
    struct HitchEntry final {
        HitchRecord record;
        PhaseId phase{no_phase};
    };
    struct Source final {
        int id{};
        std::string label;
        std::size_t workers{};
        SimFeed* feed{};
        bool live{true};
        TickHitchDetector hitch;
        std::unique_ptr<PhaseExcessTracker<256>> excess{std::make_unique<PhaseExcessTracker<256>>()};
        std::vector<std::string> names;       // phase id -> name, announced once per segment
        std::vector<bool> announced;
        std::vector<PhaseRecord> pending_phases;
        std::deque<TickRecord> ticks;
        std::deque<PhaseRecord> phases;
        std::deque<Marker> markers;
        std::deque<HitchEntry> hitches;
        std::deque<PendingHitch> pending_hitches;
        std::deque<PatternSample> pattern_samples;
        std::deque<WindowTick> window;
        std::uint64_t last_tick{};
        std::uint64_t last_wall_us{};
        std::uint32_t galactic_days{};
        std::string last_pattern;
        std::string last_verdict;
        std::string last_verdict_kind;
        std::uint64_t reported_dropped{};
        std::uint64_t phases_unpaired{};
    };

    Impl(FeedWriter& writer, FeedOptions options) : owner(writer), opt(std::move(options)) {}

    FeedWriter& owner;
    FeedOptions opt;
    std::thread thread;
    std::mutex wake_mutex;
    std::condition_variable wake;
    bool stopping{};
    std::mutex sources_mutex;
    std::vector<std::unique_ptr<Source>> sources;
    SpscRing<FrameRecord> frames{16384};
    std::deque<FrameRecord> frame_history;
    std::deque<FrameRecord> frame_window;
    FrameHitchDetector frame_hitch;
    std::deque<HitchEntry> frame_hitches;
    std::atomic<bool> capture_requested{false};
    std::uint64_t reported_frame_drops{};
    // Main-thread frame phases (source id -1): the producer interns names, the writer announces them once per segment.
    SpscRing<FramePhases> frame_phases{4096};
    PhaseTable<256> frame_phase_table;
    std::deque<std::string> frame_phase_names; // stable storage behind frame_phase_table
    std::vector<bool> frame_phase_announced;
    std::uint64_t reported_phase_drops{};
    // The context record: the viewer's fields, rewritten at every segment start and when they change.
    std::mutex context_mutex;
    std::string context_fields;
    bool context_dirty{};
    ThreadCpuSampler thread_sampler;
    ProcessMemory memory_before;
    std::uint64_t memory_before_us{};

    std::ofstream file;
    std::filesystem::path segment_path;
    std::filesystem::path capture_path;
    mutable std::mutex path_mutex;
    std::uint64_t segment_bytes{};
    int next_segment{};
    std::vector<std::filesystem::path> segments;
    std::string out;                // one batch
    std::uint64_t start_us{};

    PcCounters pc_before;
    PcSample last_pc;
    std::uint64_t last_second_us{};
    PatternAnalyzer analyzer;

    // ---------------------------------------------------------------- file handling
    [[nodiscard]] static std::string segment_name(const int index) {
        char text[64];
        std::snprintf(text, sizeof text, "eawr-perf-feed-%04d.jsonl", index);
        return text;
    }

    bool open_directory(std::string& error) {
        std::error_code ec;
        std::filesystem::create_directories(to_path(opt.directory) / "captures", ec);
        if (ec) {
            error = "cannot create the telemetry directory: " + ec.message();
            return false;
        }
        // Retain segments from earlier runs too, in numeric order (the padding is only a minimum width).
        std::vector<std::pair<int, std::filesystem::path>> existing;
        constexpr std::string_view prefix = "eawr-perf-feed-";
        constexpr std::string_view suffix = ".jsonl";
        for (const auto& entry : std::filesystem::directory_iterator(to_path(opt.directory), ec)) {
            const std::string name = entry.path().filename().string();
            if (!entry.is_regular_file() || !name.starts_with(prefix) || !name.ends_with(suffix)) continue;
            const std::string_view digits(name.data() + prefix.size(), name.size() - prefix.size() - suffix.size());
            int index = 0;
            const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), index);
            if (digits.empty() || result.ec != std::errc{} || result.ptr != digits.data() + digits.size() || index < 0
                || index == std::numeric_limits<int>::max()) continue;
            existing.emplace_back(index, entry.path());
            next_segment = std::max(next_segment, index + 1);
        }
        if (ec) {
            error = "cannot scan the telemetry directory: " + ec.message();
            return false;
        }
        std::sort(existing.begin(), existing.end());
        for (const auto& [index, path] : existing) {
            (void)index;
            segments.push_back(path);
        }
        return open_segment(error);
    }

    bool open_segment(std::string& error) {
        const std::filesystem::path path = to_path(opt.directory) / segment_name(next_segment++);
        file.close();
        file.clear();
        file.open(path, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "cannot open the telemetry feed file";
            return false;
        }
        {
            const std::lock_guard lock(path_mutex);
            segment_path = path;
        }
        segment_bytes = 0;
        segments.push_back(path);
        while (segments.size() > std::max<std::size_t>(1, opt.keep_segments)) {
            std::error_code ec;
            std::filesystem::remove(segments.front(), ec);
            segments.erase(segments.begin());
        }
        return true;
    }

    // The records every segment starts with, so a segment read alone is complete: header, sources, phase names.
    void write_preamble() {
        out += "{\"r\":\"header\",\"schema\":";
        put_i(out, feed_schema_version);
        out += ",\"clock\":\"steady-us\",\"build\":";
        put_s(out, opt.build);
        out += ",\"engine\":";
        put_s(out, opt.engine);
        out += ",\"t\":";
        put_u(out, now_us());
        out += "}\n";
        for (const auto& source : sources) {
            if (!source->live && source->ticks.empty()) continue;
            announce_source(*source);
            std::fill(source->announced.begin(), source->announced.end(), false);
            for (std::size_t id = 0; id < source->names.size(); ++id) announce_phase(*source, static_cast<PhaseId>(id));
        }
        std::fill(frame_phase_announced.begin(), frame_phase_announced.end(), false);
        announce_frame_phases();
        write_context(false);
    }
    // The main-thread frame phases (source -1): names announced once per segment, when first used.
    void announce_frame_phases() {
        const std::size_t count = frame_phase_table.size();
        if (frame_phase_announced.size() < count) frame_phase_announced.resize(count, false);
        for (std::size_t id = 0; id < count; ++id) {
            if (frame_phase_announced[id]) continue;
            frame_phase_announced[id] = true;
            out += "{\"r\":\"phase-name\",\"src\":-1,\"id\":";
            put_u(out, id);
            out += ",\"name\":";
            put_s(out, frame_phase_table.name(static_cast<PhaseId>(id)));
            out += "}\n";
        }
    }
    // The context record: the machine's CPU as aggregates plus whatever the viewer gave. `only_if_dirty`: the
    // once-per-batch call writes nothing unless the viewer changed the text.
    void write_context(const bool only_if_dirty) {
        std::string fields;
        {
            const std::lock_guard lock(context_mutex);
            if (only_if_dirty && !context_dirty) return;
            context_dirty = false;
            fields = context_fields;
        }
        const CpuInfo& cpu = cpu_info();
        out += "{\"r\":\"context\",\"t\":";
        put_u(out, now_us());
        out += ",\"cpu\":";
        put_s(out, cpu.model);
        out += ",\"cores\":";
        put_u(out, cpu.cores);
        out += ",\"logical\":";
        put_u(out, cpu.logical);
        out += ",\"p_cores\":";
        put_u(out, cpu.performance_cores);
        out += ",\"e_cores\":";
        put_u(out, cpu.efficiency_cores);
        out += ",\"p_logical\":";
        put_u(out, cpu.performance_logical);
        out += ",\"topology\":";
        out += cpu.topology_known ? "true" : "false";
        if (!fields.empty()) {
            out += ',';
            out += fields;
        }
        out += "}\n";
        count_record();
    }
    void announce_source(const Source& source) {
        out += "{\"r\":\"source\",\"src\":";
        put_i(out, source.id);
        out += ",\"label\":";
        put_s(out, source.label);
        if (source.workers != 0) {
            out += ",\"workers\":";
            put_u(out, source.workers);
        }
        out += ",\"t\":";
        put_u(out, now_us());
        out += "}\n";
    }
    void announce_phase(Source& source, const PhaseId id) {
        if (id >= source.names.size()) return;
        if (id >= source.announced.size()) source.announced.resize(source.names.size(), false);
        if (source.announced[id] || source.names[id].empty()) return;
        source.announced[id] = true;
        out += "{\"r\":\"phase-name\",\"src\":";
        put_i(out, source.id);
        out += ",\"id\":";
        put_u(out, id);
        out += ",\"name\":";
        put_s(out, source.names[id]);
        out += "}\n";
    }
    // The name of a phase of this source, read from the feed's table on first sight.
    void learn_phase(Source& source, const PhaseId id) {
        if (id == no_phase || id >= 4096) return;
        if (id >= source.names.size()) source.names.resize(static_cast<std::size_t>(id) + 1);
        if (source.names[id].empty()) source.names[id] = std::string(source.feed->phase_name(id));
        announce_phase(source, id);
    }
    [[nodiscard]] std::string_view name_of(const Source& source, const PhaseId id) const noexcept {
        return id < source.names.size() ? std::string_view(source.names[id]) : std::string_view{};
    }

    // The preamble text of a new segment. sources_mutex held; `out` is left as it was.
    std::string make_preamble() {
        std::string saved;
        saved.swap(out);
        write_preamble();
        std::string preamble;
        preamble.swap(out);
        out.swap(saved);
        return preamble;
    }

    void write_text(const std::string& text) {
        if (text.empty()) return;
        if (!file) {
            owner.write_failures_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.flush();
        if (!file) {
            owner.write_failures_.fetch_add(1, std::memory_order_relaxed);
            file.clear();
        } else {
            segment_bytes += text.size();
            owner.bytes_.fetch_add(text.size(), std::memory_order_relaxed);
        }
    }

    // Writes one batch taken from `out` under the sources mutex. Runs on the writer thread (or start/stop) without that
    // mutex, so a session attaching meanwhile never waits for the disk.
    void write_batch(const std::string& batch) {
        write_text(batch);
        if (segment_bytes < opt.rotate_bytes) return;
        std::string error;
        if (!open_segment(error)) return;
        std::string preamble;
        {
            const std::lock_guard lock(sources_mutex);
            preamble = make_preamble();
        }
        write_text(preamble);
    }
    void count_record() { owner.records_.fetch_add(1, std::memory_order_relaxed); }

    // ---------------------------------------------------------------- one source
    void finalize_hitch(Source& source, const PendingHitch& pending) {
        HitchRecord record = pending.record;
        for (const Marker& marker : source.markers) {
            if (marker.frame_side != 0) continue;
            const std::uint64_t distance = marker.tick > record.number ? marker.tick - record.number : record.number - marker.tick;
            if (distance <= 2) record.marker_mask |= marker_bit(marker.kind);
        }
        record.galactic_day = source.galactic_days;
        source.hitches.push_back({record, pending.phase});
        if (source.hitches.size() > history_hitches) source.hitches.pop_front();
        source.pattern_samples.push_back({record.number, record.wall_us, pending.phase, record.top_excess_ms, record.marker_mask});
        if (source.pattern_samples.size() > PatternAnalyzer::max_hitches) source.pattern_samples.pop_front();

        out += "{\"r\":\"hitch\",\"src\":";
        put_i(out, source.id);
        out += ",\"kind\":\"tick\",\"n\":";
        put_u(out, record.number);
        out += ",\"t\":";
        put_u(out, record.wall_us);
        out += ",\"ms\":";
        put_f(out, record.total_ms);
        out += ",\"median\":";
        put_f(out, record.median_ms);
        out += ",\"phase\":";
        put_s(out, name_of(source, pending.phase));
        out += ",\"excess\":";
        put_f(out, record.top_excess_ms);
        out += ",\"markers\":[";
        bool first = true;
        for (unsigned kind = 0; kind < static_cast<unsigned>(MarkerKind::count_); ++kind) {
            if ((record.marker_mask & (std::uint64_t{1} << kind)) == 0) continue;
            if (!first) out += ',';
            first = false;
            put_s(out, marker_name(static_cast<MarkerKind>(kind)));
        }
        out += "],\"day\":";
        put_u(out, record.galactic_day);
        out += ",\"speed\":";
        put_f(out, record.speed, 2);
        out += ",\"mode\":";
        put_s(out, mode_name(record.mode));
        out += "}\n";
        count_record();
    }

    void finalize_ready_hitches(Source& source, const bool force) {
        while (!source.pending_hitches.empty()
               && (force || source.last_tick >= source.pending_hitches.front().record.number + 3)) {
            finalize_hitch(source, source.pending_hitches.front());
            source.pending_hitches.pop_front();
        }
    }

    void process_tick(Source& source, const TickRecord& tick, std::size_t& cursor) {
        std::array<PhaseSample, 512> samples{};
        std::size_t count = 0;
        // Phases of this tick were pushed before its record; older orphans (a dropped tick record) are skipped.
        while (cursor < source.pending_phases.size() && source.pending_phases[cursor].tick < tick.tick) {
            ++source.phases_unpaired;
            ++cursor;
        }
        const std::size_t first_phase = cursor;
        while (cursor < source.pending_phases.size() && source.pending_phases[cursor].tick == tick.tick) {
            const PhaseRecord& phase = source.pending_phases[cursor++];
            if (count < samples.size()) samples[count++] = {phase.id, phase.ms};
            source.phases.push_back(phase);
            learn_phase(source, phase.id);
        }
        const std::span<const PhaseSample> phase_span(samples.data(), count);
        const PhaseExcess top = source.excess->top_excess(phase_span);
        PhaseId top_phase = top.id;
        float top_ms = 0.0F;
        for (const PhaseSample& sample : phase_span)
            if (sample.id == top.id) top_ms = sample.ms;

        const float median = source.hitch.median_ms();
        const bool hitch = source.hitch.observe({tick.total_ms, tick.pacing_lag_ms, tick.interval_ms, (tick.flags & TickFlags::clock_reset) != 0});
        source.excess->commit(phase_span);

        out += "{\"r\":\"tick\",\"src\":";
        put_i(out, source.id);
        out += ",\"n\":";
        put_u(out, tick.tick);
        out += ",\"t\":";
        put_u(out, tick.wall_us);
        out += ",\"ms\":";
        put_f(out, tick.total_ms);
        out += ",\"lag\":";
        put_f(out, tick.pacing_lag_ms);
        out += ",\"lock\":";
        put_f(out, tick.session_lock_ms);
        out += ",\"cpu\":";
        put_f(out, tick.cpu_ratio);
        out += ",\"iv\":";
        put_f(out, tick.interval_ms);
        out += ",\"fl\":";
        put_u(out, tick.flags);
        out += ",\"ph\":[";
        for (std::size_t i = first_phase; i < cursor; ++i) {
            if (i != first_phase) out += ',';
            out += '[';
            put_u(out, source.pending_phases[i].id);
            out += ',';
            put_f(out, source.pending_phases[i].ms);
            out += ']';
        }
        out += "]}\n";
        count_record();

        source.ticks.push_back(tick);
        if (source.ticks.size() > history_ticks) source.ticks.pop_front();
        source.window.push_back({tick.wall_us, tick.total_ms, tick.interval_ms, tick.session_lock_ms, tick.cpu_ratio, top_phase, top_ms});
        while (!source.window.empty() && source.window.front().wall_us + 5'000'000 < tick.wall_us) source.window.pop_front();
        source.last_tick = tick.tick;
        source.last_wall_us = tick.wall_us;

        if (hitch) {
            HitchRecord record;
            record.kind = HitchKind::tick;
            record.mode = mode_of(source.label, tick.flags);
            record.top_phase = top_phase;
            record.number = tick.tick;
            record.wall_us = tick.wall_us;
            record.speed = tick.interval_ms > 0.0F ? 1000.0F / tick.interval_ms / 30.0F : 1.0F;
            record.total_ms = tick.total_ms;
            record.median_ms = median;
            record.top_excess_ms = top.excess_ms;
            source.pending_hitches.push_back({record, top_phase, tick.flags});
        }
    }

    // One pass over one source: ticks first, then their phases, then markers (see the pairing note in process_tick).
    void drain_source(Source& source) {
        SimFeed& feed = *source.feed;
        std::vector<TickRecord> ticks;
        TickRecord tick;
        while (ticks.size() < 8192 && feed.pop_tick(tick)) ticks.push_back(tick);
        PhaseRecord phase;
        std::size_t popped = 0;
        while (popped < 262144 && feed.pop_phase(phase)) {
            source.pending_phases.push_back(phase);
            ++popped;
        }
        Marker marker;
        std::vector<Marker> markers;
        while (markers.size() < 4096 && feed.pop_marker(marker)) markers.push_back(marker);

        for (const Marker& m : markers) {
            source.markers.push_back(m);
            if (m.kind == MarkerKind::galactic_day) ++source.galactic_days;
            out += "{\"r\":\"marker\",\"src\":";
            put_i(out, source.id);
            out += ",\"k\":";
            put_s(out, marker_name(m.kind));
            out += ",\"n\":";
            put_u(out, m.tick);
            out += ",\"v\":";
            put_i(out, m.value);
            out += ",\"fs\":";
            put_u(out, m.frame_side);
            out += "}\n";
            count_record();
        }
        while (source.markers.size() > history_markers) source.markers.pop_front();

        std::size_t cursor = 0;
        for (const TickRecord& t : ticks) process_tick(source, t, cursor);
        // Phases of ticks whose record has not arrived stay pending; keep the unconsumed tail only.
        source.pending_phases.erase(source.pending_phases.begin(), source.pending_phases.begin() + static_cast<std::ptrdiff_t>(cursor));
        if (source.pending_phases.size() > 65536)
            source.pending_phases.erase(source.pending_phases.begin(),
                                        source.pending_phases.begin() + static_cast<std::ptrdiff_t>(source.pending_phases.size() - 65536));
        while (source.phases.size() > history_phases) source.phases.pop_front();
        finalize_ready_hitches(source, false);
        trim_history(source);
    }

    void trim_history(Source& source) {
        const std::uint64_t window_us = static_cast<std::uint64_t>(opt.capture_window.count()) * 1'000'000;
        const std::uint64_t newest = source.last_wall_us;
        while (!source.ticks.empty() && source.ticks.front().wall_us + window_us < newest) source.ticks.pop_front();
        const std::uint64_t oldest_tick = source.ticks.empty() ? 0 : source.ticks.front().tick;
        while (!source.phases.empty() && source.phases.front().tick < oldest_tick) source.phases.pop_front();
        while (!source.markers.empty() && source.markers.front().frame_side == 0 && source.markers.front().tick + 4 < oldest_tick)
            source.markers.pop_front();
    }

    // ---------------------------------------------------------------- frames
    void drain_frames() {
        FrameRecord frame;
        while (frames.pop(frame)) {
            const float median = frame_hitch.median_ms();
            const bool hitch = frame_hitch.observe(frame.frame_ms);
            out += "{\"r\":\"frame\",\"n\":";
            put_u(out, frame.frame);
            out += ",\"t\":";
            put_u(out, frame.wall_us);
            out += ",\"ms\":";
            put_f(out, frame.frame_ms);
            out += ",\"main\":";
            put_f(out, frame.main_ms);
            out += ",\"render\":";
            put_f(out, frame.render_cpu_ms);
            out += ",\"gpu\":";
            put_f(out, frame.gpu_ms);
            out += ",\"lock\":";
            put_f(out, frame.ui_lock_ms);
            out += ",\"stale\":";
            put_u(out, (frame.flags & FrameFlags::stale) != 0 ? 1 : 0);
            if ((frame.flags & FrameFlags::fight) != 0) {
                out += ",\"u\":";
                put_u(out, frame.units);
                out += ",\"eu\":";
                put_u(out, frame.enemy_units);
                out += ",\"atk\":";
                put_u(out, frame.attacking);
                out += ",\"proj\":";
                put_u(out, frame.projectiles);
                out += ",\"fx\":";
                put_u(out, frame.effects);
                out += ",\"part\":";
                put_u(out, frame.particles);
                out += ",\"dc\":";
                put_u(out, frame.draw_calls);
                out += ",\"obj\":";
                put_u(out, frame.objects);
            }
            out += "}\n";
            count_record();
            frame_history.push_back(frame);
            frame_window.push_back(frame);
            while (frame_history.size() > history_frames) frame_history.pop_front();
            while (!frame_window.empty() && frame_window.front().wall_us + 5'000'000 < frame.wall_us) frame_window.pop_front();
            const std::uint64_t window_us = static_cast<std::uint64_t>(opt.capture_window.count()) * 1'000'000;
            while (!frame_history.empty() && frame_history.front().wall_us + window_us < frame.wall_us) frame_history.pop_front();
            if (hitch) {
                HitchRecord record;
                record.kind = HitchKind::frame;
                record.mode = HitchMode::viewer;
                record.number = frame.frame;
                record.wall_us = frame.wall_us;
                record.total_ms = frame.frame_ms;
                record.median_ms = median;
                frame_hitches.push_back({record, no_phase});
                if (frame_hitches.size() > history_hitches) frame_hitches.pop_front();
                out += "{\"r\":\"hitch\",\"kind\":\"frame\",\"n\":";
                put_u(out, frame.frame);
                out += ",\"t\":";
                put_u(out, frame.wall_us);
                out += ",\"ms\":";
                put_f(out, frame.frame_ms);
                out += ",\"median\":";
                put_f(out, median);
                out += "}\n";
                count_record();
            }
        }
    }

    void drain_frame_phases() {
        FramePhases phases;
        while (frame_phases.pop(phases)) {
            announce_frame_phases();
            out += "{\"r\":\"fph\",\"n\":";
            put_u(out, phases.frame);
            out += ",\"ph\":[";
            const std::uint32_t count = std::min<std::uint32_t>(phases.count, FramePhases::max_entries);
            for (std::uint32_t i = 0; i < count; ++i) {
                if (i != 0) out += ',';
                out += '[';
                put_u(out, phases.entries[i].id);
                out += ',';
                put_f(out, phases.entries[i].ms);
                out += ']';
            }
            out += "]}\n";
            count_record();
        }
    }

    // ---------------------------------------------------------------- once per second
    [[nodiscard]] WindowStats window_stats(const Source& source, const std::uint64_t now) const {
        WindowStats stats;
        const auto& window = source.window;
        stats.idle = window.size() < 8 || now > source.last_wall_us + 2'000'000;
        if (stats.idle) return stats;
        const double span_s = static_cast<double>(window.back().wall_us - window.front().wall_us) / 1e6;
        if (span_s < 1.0) { stats.idle = true; return stats; }
        stats.interval_ms = window.back().interval_ms;
        const double requested = stats.interval_ms > 0.0F ? 1000.0 / static_cast<double>(stats.interval_ms) : 0.0;
        if (requested <= 0.0) { stats.idle = true; return stats; }
        stats.delivered_ratio = static_cast<float>(static_cast<double>(window.size() - 1) / span_s / requested);
        std::vector<float> totals;
        totals.reserve(window.size());
        double sum = 0.0, lock = 0.0, cpu = 0.0;
        std::size_t cpu_n = 0;
        for (const WindowTick& t : window) {
            totals.push_back(t.total_ms);
            sum += t.total_ms;
            lock += t.session_lock_ms;
            if (t.cpu_ratio >= 0.0F) { cpu += std::min(1.0F, t.cpu_ratio); ++cpu_n; }
        }
        stats.mean_tick_ms = static_cast<float>(sum / static_cast<double>(window.size()));
        stats.session_lock_ms = static_cast<float>(lock / static_cast<double>(window.size()));
        stats.sim_cpu = cpu_n >= window.size() / 2 ? static_cast<float>(cpu / static_cast<double>(cpu_n)) : -1.0F;
        stats.tick_p95_ms = percentile95(totals);
        // The phase most ticks at or above the p95 lost their time to (its mean cost over those ticks).
        std::array<std::pair<std::size_t, double>, 256> per_phase{};
        for (const WindowTick& t : window) {
            if (t.total_ms < stats.tick_p95_ms || t.top_phase >= per_phase.size()) continue;
            per_phase[t.top_phase].first += 1;
            per_phase[t.top_phase].second += t.top_ms;
        }
        std::size_t best = 0;
        for (std::size_t i = 1; i < per_phase.size(); ++i)
            if (per_phase[i].first > per_phase[best].first) best = i;
        if (per_phase[best].first > 0) {
            stats.top_phase = static_cast<PhaseId>(best);
            stats.top_phase_ms = static_cast<float>(per_phase[best].second / static_cast<double>(per_phase[best].first));
        }
        // Frame side, when frames arrive: a latest-5-second window of the frame records.
        if (frame_window.size() >= 8) {
            std::vector<float> frame_ms;
            double mean = 0, main = 0, render = 0, gpu = 0, ui = 0, stale = 0;
            std::size_t main_n = 0, render_n = 0, gpu_n = 0;
            for (const FrameRecord& f : frame_window) {
                frame_ms.push_back(f.frame_ms);
                mean += f.frame_ms;
                ui += f.ui_lock_ms;
                if ((f.flags & FrameFlags::stale) != 0) stale += 1;
                if (f.main_ms >= 0.0F) { main += f.main_ms; ++main_n; }
                if (f.render_cpu_ms >= 0.0F) { render += f.render_cpu_ms; ++render_n; }
                if (f.gpu_ms >= 0.0F) { gpu += f.gpu_ms; ++gpu_n; }
            }
            const double n = static_cast<double>(frame_window.size());
            stats.frame_p95_ms = percentile95(frame_ms);
            stats.mean_frame_ms = static_cast<float>(mean / n);
            stats.ui_lock_ms = static_cast<float>(ui / n);
            stats.stale_share = static_cast<float>(stale / n);
            if (main_n != 0) { stats.main_ms = static_cast<float>(main / static_cast<double>(main_n)); stats.main_viewer_ms = stats.main_ms; }
            if (render_n != 0) stats.render_cpu_ms = static_cast<float>(render / static_cast<double>(render_n));
            if (gpu_n != 0) stats.gpu_ms = static_cast<float>(gpu / static_cast<double>(gpu_n));
        }
        stats.others_cpu = last_pc.others_busy;
        return stats;
    }

    void once_per_second(const std::uint64_t now) {
        const PcCounters after = read_pc_counters();
        last_pc = pc_sample_between(pc_before, after);
        pc_before = after;
        if (after.valid) {
            out += "{\"r\":\"pc\",\"t\":";
            put_u(out, now);
            out += ",\"busy\":";
            put_f(out, last_pc.system_busy);
            out += ",\"own\":";
            put_f(out, last_pc.own_busy);
            out += ",\"others\":";
            put_f(out, last_pc.others_busy);
            out += ",\"commit\":";
            put_f(out, last_pc.commit_ratio);
            out += ",\"avail_mb\":";
            put_u(out, last_pc.memory_available_mib);
            out += "}\n";
            count_record();
        }
        {
            // Which thread is busy: shares of one core (1.0 = one thread-equivalent), named threads first.
            const std::vector<ThreadShare> shares = thread_sampler.sample(now);
            if (!shares.empty()) {
                out += "{\"r\":\"thr\",\"t\":";
                put_u(out, now);
                out += ",\"th\":[";
                bool first = true;
                for (const ThreadShare& share : shares) {
                    if (!first) out += ',';
                    first = false;
                    out += '[';
                    put_s(out, share.name);
                    out += ',';
                    put_f(out, share.share, 3);
                    out += ',';
                    out += share.labeled ? '1' : '0';
                    out += ']';
                }
                out += "]}\n";
                count_record();
            }
            // This process's memory and paging: private commit and working set in MiB, page faults per second.
            const ProcessMemory memory = read_process_memory();
            if (memory.valid) {
                out += "{\"r\":\"mem\",\"t\":";
                put_u(out, now);
                out += ",\"commit_mb\":";
                put_u(out, memory.commit_mib);
                out += ",\"ws_mb\":";
                put_u(out, memory.working_set_mib);
                out += ",\"pf_s\":";
                if (memory_before.valid && now > memory_before_us && memory.page_faults >= memory_before.page_faults)
                    put_f(out, static_cast<double>(memory.page_faults - memory_before.page_faults) * 1e6 / static_cast<double>(now - memory_before_us), 1);
                else out += "-1";
                out += "}\n";
                count_record();
                memory_before = memory;
                memory_before_us = now;
            }
        }
        for (const auto& holder : sources) {
            Source& source = *holder;
            if (!source.live) continue;
            const WindowStats stats = window_stats(source, now);
            const Verdict verdict = evaluate_verdict(stats);
            const std::string text = format_verdict(verdict, [&](const PhaseId id) { return name_of(source, id); });
            out += "{\"r\":\"verdict\",\"src\":";
            put_i(out, source.id);
            out += ",\"t\":";
            put_u(out, now);
            out += ",\"text\":";
            put_s(out, text);
            out += ",\"d\":";
            put_f(out, stats.delivered_ratio, 3);
            out += ",\"p95\":";
            put_f(out, stats.tick_p95_ms);
            out += "}\n";
            count_record();

            if (source.pattern_samples.size() >= 2) {
                const std::vector<PatternSample> samples(source.pattern_samples.begin(), source.pattern_samples.end());
                const PatternReadout readout = analyzer.analyze(samples);
                const std::string line = format_pattern(readout, [&](const PhaseId id) { return name_of(source, id); });
                if (line != source.last_pattern) {
                    source.last_pattern = line;
                    out += "{\"r\":\"pattern\",\"src\":";
                    put_i(out, source.id);
                    out += ",\"t\":";
                    put_u(out, now);
                    out += ",\"text\":";
                    put_s(out, line);
                    out += "}\n";
                    count_record();
                }
            }
            const std::uint64_t dropped = source.feed->dropped();
            if (dropped != source.reported_dropped) {
                source.reported_dropped = dropped;
                out += "{\"r\":\"dropped\",\"src\":";
                put_i(out, source.id);
                out += ",\"t\":";
                put_u(out, now);
                out += ",\"total\":";
                put_u(out, dropped);
                out += "}\n";
                count_record();
            }
        }
        const std::uint64_t phase_drops = frame_phases.dropped();
        if (phase_drops != reported_phase_drops) {
            reported_phase_drops = phase_drops;
            out += "{\"r\":\"dropped\",\"src\":-2,\"t\":";
            put_u(out, now);
            out += ",\"total\":";
            put_u(out, phase_drops);
            out += "}\n";
            count_record();
        }
        const std::uint64_t frame_drops = frames.dropped();
        if (frame_drops != reported_frame_drops) {
            reported_frame_drops = frame_drops;
            out += "{\"r\":\"dropped\",\"src\":-1,\"t\":";
            put_u(out, now);
            out += ",\"total\":";
            put_u(out, frame_drops);
            out += "}\n";
            count_record();
        }
    }

    void drain_all(const bool force_hitches) {
        std::string batch;
        {
            const std::lock_guard lock(sources_mutex);
            for (const auto& source : sources)
                if (source->live) {
                    drain_source(*source);
                    if (force_hitches) finalize_ready_hitches(*source, true);
                }
            write_context(true);
            drain_frames();
            drain_frame_phases();
            const std::uint64_t now = now_us();
            if (now >= last_second_us + 1'000'000) {
                last_second_us = now;
                once_per_second(now);
            }
            batch.swap(out);
        }
        write_batch(batch);
    }

    // ---------------------------------------------------------------- Chrome trace capture
    struct TraceWriter final {
        std::ofstream& stream;
        bool first{true};
        void begin() { stream << "{\"traceEvents\":[\n"; }
        void event(const std::string& body) {
            if (!first) stream << ",\n";
            first = false;
            stream << body;
        }
    };

    static void trace_args_ms(std::string& s, const char* key, const double value) {
        s += "\"";
        s += key;
        s += "\":";
        put_f(s, value);
    }

    std::filesystem::path write_capture() {
        // Only snapshot the fields used by the trace while holding the source lock. Encoding and disk I/O happen
        // afterward, so session attach/detach never waits for a capture file to be written.
        std::vector<std::unique_ptr<Source>> snapshot;
        {
            const std::lock_guard lock(sources_mutex);
            for (const auto& original : sources) {
                auto copy = std::make_unique<Source>();
                copy->id = original->id;
                copy->label = original->label;
                copy->names = original->names;
                copy->ticks = original->ticks;
                copy->phases = original->phases;
                copy->markers = original->markers;
                copy->hitches = original->hitches;
                copy->last_pattern = original->last_pattern;
                snapshot.push_back(std::move(copy));
            }
        }
        if (opt.capture_before_write) opt.capture_before_write(opt.capture_before_write_context);
        const std::string name = "capture-" + utc_stamp() + "-" + std::to_string(owner.captures_.load()) + ".json";
        const std::filesystem::path path = to_path(opt.directory) / "captures" / name;
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream) {
            owner.write_failures_.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        std::string line;
        TraceWriter trace{stream};
        trace.begin();
        const auto meta = [&](const char* kind, const std::uint64_t tid, const std::string_view label) {
            line.clear();
            line += "{\"ph\":\"M\",\"pid\":1,\"tid\":";
            put_u(line, tid);
            line += ",\"name\":\"";
            line += kind;
            line += "\",\"args\":{\"name\":";
            put_s(line, label);
            line += "}}";
            trace.event(line);
        };
        meta("process_name", 0, "eawr");
        std::string patterns;
        for (const auto& holder : snapshot) {
            const Source& source = *holder;
            const std::uint64_t base = 10000ULL * (static_cast<std::uint64_t>(source.id) + 1);
            meta("thread_name", base, "ticks: " + source.label);
            meta("thread_name", base + 90, "hitches and markers: " + source.label);
            std::vector<bool> phase_track(source.names.size(), false);
            for (const TickRecord& t : source.ticks) {
                const double dur_us = static_cast<double>(t.total_ms) * 1000.0;
                const std::uint64_t start = t.wall_us > static_cast<std::uint64_t>(dur_us) ? t.wall_us - static_cast<std::uint64_t>(dur_us) : 0;
                line.clear();
                line += "{\"ph\":\"X\",\"pid\":1,\"tid\":";
                put_u(line, base);
                line += ",\"name\":\"tick\",\"ts\":";
                put_u(line, start);
                line += ",\"dur\":";
                put_f(line, dur_us, 1);
                line += ",\"args\":{\"tick\":";
                put_u(line, t.tick);
                line += ',';
                trace_args_ms(line, "lag_ms", t.pacing_lag_ms);
                line += ',';
                trace_args_ms(line, "cpu", t.cpu_ratio);
                line += "}}";
                trace.event(line);
                line.clear();
                line += "{\"ph\":\"C\",\"pid\":1,\"name\":\"tick ms (";
                line += source.label;
                line += ")\",\"ts\":";
                put_u(line, t.wall_us);
                line += ",\"args\":{\"ms\":";
                put_f(line, t.total_ms);
                line += "}}";
                trace.event(line);
            }
            // Phases: one track per phase id, each placed at the start of its tick. Same-phase slices of consecutive
            // ticks never overlap; sub-0.05 ms rows are left out to keep the file small.
            std::size_t tick_cursor = 0;
            for (const PhaseRecord& p : source.phases) {
                if (p.ms < 0.05F || p.id >= source.names.size()) continue;
                while (tick_cursor < source.ticks.size() && source.ticks[tick_cursor].tick < p.tick) ++tick_cursor;
                if (tick_cursor >= source.ticks.size() || source.ticks[tick_cursor].tick != p.tick) continue;
                const TickRecord& t = source.ticks[tick_cursor];
                const double total_us = static_cast<double>(t.total_ms) * 1000.0;
                const std::uint64_t start = t.wall_us > static_cast<std::uint64_t>(total_us) ? t.wall_us - static_cast<std::uint64_t>(total_us) : 0;
                if (!phase_track[p.id]) {
                    phase_track[p.id] = true;
                    meta("thread_name", base + 100 + p.id, source.label + ": " + source.names[p.id]);
                }
                line.clear();
                line += "{\"ph\":\"X\",\"pid\":1,\"tid\":";
                put_u(line, base + 100 + p.id);
                line += ",\"name\":";
                put_s(line, source.names[p.id]);
                line += ",\"ts\":";
                put_u(line, start);
                line += ",\"dur\":";
                put_f(line, static_cast<double>(p.ms) * 1000.0, 1);
                line += ",\"args\":{\"tick\":";
                put_u(line, p.tick);
                line += "}}";
                trace.event(line);
            }
            for (const Marker& m : source.markers) {
                if (m.frame_side != 0) continue;
                // A marker has a tick number only: place it at that tick's end when the tick is in the window.
                const auto at = std::lower_bound(source.ticks.begin(), source.ticks.end(), m.tick,
                                                 [](const TickRecord& t, const std::uint64_t tick) { return t.tick < tick; });
                if (at == source.ticks.end() || at->tick != m.tick) continue;
                line.clear();
                line += "{\"ph\":\"i\",\"s\":\"t\",\"pid\":1,\"tid\":";
                put_u(line, base + 90);
                line += ",\"name\":";
                put_s(line, marker_name(m.kind));
                line += ",\"ts\":";
                put_u(line, at->wall_us);
                line += ",\"args\":{\"tick\":";
                put_u(line, m.tick);
                line += ",\"value\":";
                put_i(line, m.value);
                line += "}}";
                trace.event(line);
            }
            for (const HitchEntry& h : source.hitches) {
                if (source.ticks.empty() || h.record.wall_us < source.ticks.front().wall_us) continue;
                line.clear();
                line += "{\"ph\":\"i\",\"s\":\"t\",\"pid\":1,\"tid\":";
                put_u(line, base + 90);
                line += ",\"name\":\"hitch\",\"ts\":";
                put_u(line, h.record.wall_us);
                line += ",\"args\":{\"tick\":";
                put_u(line, h.record.number);
                line += ',';
                trace_args_ms(line, "ms", h.record.total_ms);
                line += ',';
                trace_args_ms(line, "median_ms", h.record.median_ms);
                line += ",\"phase\":";
                put_s(line, name_of(source, h.phase));
                line += ',';
                trace_args_ms(line, "excess_ms", h.record.top_excess_ms);
                line += "}}";
                trace.event(line);
            }
            if (!source.last_pattern.empty()) patterns += (patterns.empty() ? "" : "; ") + source.label + ": " + source.last_pattern;
        }
        meta("thread_name", 5, "frames");
        for (const FrameRecord& f : frame_history) {
            const double dur_us = static_cast<double>(f.frame_ms) * 1000.0;
            line.clear();
            line += "{\"ph\":\"X\",\"pid\":1,\"tid\":5,\"name\":\"frame\",\"ts\":";
            put_u(line, f.wall_us > static_cast<std::uint64_t>(dur_us) ? f.wall_us - static_cast<std::uint64_t>(dur_us) : 0);
            line += ",\"dur\":";
            put_f(line, dur_us, 1);
            line += ",\"args\":{\"frame\":";
            put_u(line, f.frame);
            line += ',';
            trace_args_ms(line, "main_ms", f.main_ms);
            line += ',';
            trace_args_ms(line, "render_cpu_ms", f.render_cpu_ms);
            line += ',';
            trace_args_ms(line, "gpu_ms", f.gpu_ms);
            line += ',';
            trace_args_ms(line, "ui_lock_ms", f.ui_lock_ms);
            line += "}}";
            trace.event(line);
        }
        for (const HitchEntry& h : frame_hitches) {
            line.clear();
            line += "{\"ph\":\"i\",\"s\":\"t\",\"pid\":1,\"tid\":5,\"name\":\"frame hitch\",\"ts\":";
            put_u(line, h.record.wall_us);
            line += ",\"args\":{\"frame\":";
            put_u(line, h.record.number);
            line += ',';
            trace_args_ms(line, "ms", h.record.total_ms);
            line += "}}";
            trace.event(line);
        }
        line.clear();
        line += "{\"ph\":\"C\",\"pid\":1,\"name\":\"other processes cpu\",\"ts\":";
        put_u(line, now_us());
        line += ",\"args\":{\"share\":";
        put_f(line, last_pc.others_busy < 0.0F ? 0.0F : last_pc.others_busy);
        line += "}}";
        trace.event(line);
        stream << "\n],\"displayTimeUnit\":\"ms\",\"otherData\":{\"schema\":" << feed_schema_version << ",\"build\":" << json_quote(opt.build)
               << ",\"engine\":" << json_quote(opt.engine) << ",\"pattern\":" << json_quote(patterns) << ",\"window_s\":"
               << opt.capture_window.count() << "}}\n";
        stream.flush();
        if (!stream) {
            owner.write_failures_.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        owner.captures_.fetch_add(1, std::memory_order_relaxed);
        {
            const std::lock_guard path_lock(path_mutex);
            capture_path = path;
        }
        return path;
    }

    // ---------------------------------------------------------------- thread
    void run() {
        lower_this_thread_priority();
        for (;;) {
            {
                std::unique_lock lock(wake_mutex);
                wake.wait_for(lock, opt.batch, [&] { return stopping; });
                if (stopping) return;
            }
            drain_all(false);
            if (capture_requested.exchange(false, std::memory_order_acq_rel)) {
                drain_all(true);
                (void)write_capture();
            }
        }
    }

    // ---------------------------------------------------------------- sources
    int attach(SimFeed& feed, const std::string_view label, const std::size_t workers) {
        const std::lock_guard lock(sources_mutex);
        auto source = std::make_unique<Source>();
        source->id = static_cast<int>(sources.size());
        source->label = std::string(label);
        source->workers = workers;
        source->feed = &feed;
        const int id = source->id;
        announce_source(*source);
        sources.push_back(std::move(source));
        feed.set_fine(opt.fine);
        feed.set_enabled(true);
        return id;
    }
    void detach(const int id) {
        const std::lock_guard lock(sources_mutex);
        if (id < 0 || static_cast<std::size_t>(id) >= sources.size()) return;
        Source& source = *sources[static_cast<std::size_t>(id)];
        if (!source.live) return;
        source.feed->set_enabled(false);
        drain_source(source);
        finalize_ready_hitches(source, true);
        source.live = false;
        source.feed = nullptr;
        out += "{\"r\":\"source-end\",\"src\":";
        put_i(out, id);
        out += ",\"t\":";
        put_u(out, now_us());
        out += "}\n";
    }
};

FeedWriter::FeedWriter(FeedOptions options) : impl_(std::make_unique<Impl>(*this, std::move(options))) {}

FeedWriter::~FeedWriter() { stop(); }

bool FeedWriter::start(std::string& error) {
    const std::lock_guard global(active_mutex());
    if (running_.load(std::memory_order_acquire)) return true;
    if (active_pointer().load(std::memory_order_acquire) != nullptr) {
        error = "a telemetry feed is already running";
        return false;
    }
    if (impl_->opt.directory.empty()) {
        error = "no telemetry directory";
        return false;
    }
    impl_->stopping = false;
    if (!impl_->open_directory(error)) return false;
    impl_->start_us = now_us();
    impl_->last_second_us = impl_->start_us;
    impl_->pc_before = read_pc_counters();
    {
        const std::lock_guard lock(impl_->sources_mutex);
        impl_->write_text(impl_->make_preamble());
    }
    impl_->thread = std::thread([this] { impl_->run(); });
    running_.store(true, std::memory_order_release);
    active_pointer().store(this, std::memory_order_release);
    return true;
}

void FeedWriter::stop() {
    const std::lock_guard global(active_mutex());
    if (!running_.load(std::memory_order_acquire)) return;
    active_pointer().store(nullptr, std::memory_order_release); // no new attach from here on
    running_.store(false, std::memory_order_release);
    {
        const std::lock_guard lock(impl_->wake_mutex);
        impl_->stopping = true;
    }
    impl_->wake.notify_all();
    if (impl_->thread.joinable()) impl_->thread.join();
    {
        // Sessions still attached keep running: switch their feeds off (their owners are alive: a destroying session
        // detaches through detach_active(), which waits for the global mutex held here).
        const std::lock_guard lock(impl_->sources_mutex);
        for (const auto& source : impl_->sources)
            if (source->live && source->feed != nullptr) source->feed->set_enabled(false);
    }
    impl_->drain_all(true);
    {
        const std::lock_guard lock(impl_->sources_mutex);
        for (const auto& source : impl_->sources) {
            source->live = false;
            source->feed = nullptr;
        }
    }
    {
        std::string rest;
        {
            const std::lock_guard lock(impl_->sources_mutex);
            rest.swap(impl_->out);
        }
        impl_->write_batch(rest);
    }
    impl_->file.close();
}

namespace {
std::unique_ptr<FeedWriter>& process_writer() {
    static std::unique_ptr<FeedWriter> writer;
    return writer;
}
std::mutex& process_mutex() {
    static std::mutex mutex;
    return mutex;
}
} // namespace

bool FeedWriter::start_process(FeedOptions options, std::string& error) {
    const std::lock_guard lock(process_mutex());
    if (process_writer()) return true;
    auto writer = std::make_unique<FeedWriter>(std::move(options));
    if (!writer->start(error)) return false;
    process_writer() = std::move(writer);
    return true;
}

void FeedWriter::stop_process() noexcept {
    std::unique_ptr<FeedWriter> writer;
    {
        const std::lock_guard lock(process_mutex());
        writer = std::move(process_writer());
    }
    if (writer) writer->stop();
}

bool FeedWriter::active() noexcept { return active_pointer().load(std::memory_order_acquire) != nullptr; }

int FeedWriter::attach_active(SimFeed& feed, const std::string_view label, const std::size_t workers) {
    if (!active()) return -1;
    const std::lock_guard global(active_mutex());
    FeedWriter* const writer = active_pointer().load(std::memory_order_acquire);
    return writer == nullptr ? -1 : writer->impl_->attach(feed, label, workers);
}

void FeedWriter::detach_active(const int source) noexcept {
    if (source < 0) return;
    const std::lock_guard global(active_mutex());
    if (FeedWriter* const writer = active_pointer().load(std::memory_order_acquire)) writer->impl_->detach(source);
}

void FeedWriter::request_capture_active() noexcept {
    if (FeedWriter* const writer = active_pointer().load(std::memory_order_acquire))
        writer->impl_->capture_requested.store(true, std::memory_order_release);
}

void FeedWriter::push_frame_active(const FrameRecord& record) noexcept {
    if (FeedWriter* const writer = active_pointer().load(std::memory_order_acquire)) writer->impl_->frames.push(record);
}

PhaseId FeedWriter::intern_frame_phase_active(const std::string_view name) noexcept {
    FeedWriter* const writer = active_pointer().load(std::memory_order_acquire);
    if (writer == nullptr) return no_phase;
    Impl& impl = *writer->impl_;
    const PhaseId found = impl.frame_phase_table.find(name);
    if (found != no_phase) return found;
    if (impl.frame_phase_table.size() == impl.frame_phase_table.capacity()) return no_phase;
    try {
        impl.frame_phase_names.emplace_back(name);
    } catch (...) {
        return no_phase;
    }
    return impl.frame_phase_table.intern(impl.frame_phase_names.back());
}

void FeedWriter::push_frame_phases_active(const FramePhases& phases) noexcept {
    if (FeedWriter* const writer = active_pointer().load(std::memory_order_acquire)) writer->impl_->frame_phases.push(phases);
}

void FeedWriter::set_context_active(const std::string_view fields) {
    FeedWriter* const writer = active_pointer().load(std::memory_order_acquire);
    if (writer == nullptr) return;
    Impl& impl = *writer->impl_;
    const std::lock_guard lock(impl.context_mutex);
    if (impl.context_fields == fields) return;
    impl.context_fields = std::string(fields);
    impl.context_dirty = true;
}

std::string FeedWriter::current_segment() const {
    const std::lock_guard lock(impl_->path_mutex);
    return from_path(impl_->segment_path);
}
std::string FeedWriter::last_capture() const {
    const std::lock_guard lock(impl_->path_mutex);
    return from_path(impl_->capture_path);
}

} // namespace eawr::core::perf_diag
