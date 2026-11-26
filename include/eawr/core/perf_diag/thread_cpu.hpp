#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace eawr::core::perf_diag {

// Which thread is pinned (#8058, capture upgrade). Three pieces, all write-only observers:
//  - ThreadLabel: a thread names itself once when it starts. Nothing happens while no telemetry feed runs (one atomic load).
//  - ThreadCpuSampler: once per second, on the feed writer thread, reads the CPU time of every thread of this process
//    and turns the delta into a share of one core (1.0 = one thread-equivalent). Threads that carry a label use it; the
//    other threads use the name the engine gave them (Windows thread description, Linux comm) or "t<id>".
//  - CpuInfo: the machine's CPU as aggregates (model, cores, logical processors, performance/efficiency split).
// No host, user, path or process name enters any of it.

// A name for the calling thread, for the life of the object. The index (when given) makes "worker" into "worker-3".
class ThreadLabel final {
public:
    static constexpr std::size_t no_index = static_cast<std::size_t>(-1);
    explicit ThreadLabel(std::string_view name, std::size_t index = no_index) noexcept;
    ~ThreadLabel();
    ThreadLabel(const ThreadLabel&) = delete;
    ThreadLabel& operator=(const ThreadLabel&) = delete;

private:
    std::uint64_t id_{};
};

// Labels set right now, as (operating system thread id, label). Any thread.
[[nodiscard]] std::vector<std::pair<std::uint64_t, std::string>> labeled_threads();
// The operating system id of the calling thread (0 where unknown).
[[nodiscard]] std::uint64_t current_thread_os_id() noexcept;

// One thread's CPU use over the interval between two samples.
struct ThreadShare final {
    std::string name;
    float share{};   // CPU seconds per wall second: 1.0 is one core fully busy
    bool labeled{};  // false: the name came from the engine or is "t<id>"
};

// Raw per-thread CPU counters of one instant (unit: 100 ns on Windows, clock ticks on Linux).
struct ThreadCounters final {
    std::uint64_t id{};
    std::uint64_t cpu{};
    std::string engine_name; // may be empty
};

class ThreadCpuSampler final {
public:
    // Reads every thread of this process; the first call only primes the baseline and returns nothing. `now_us` is a
    // steady clock in microseconds. Threads that were idle in the interval and carry no label are left out; the rest are
    // sorted by share, highest first, labeled threads always included.
    [[nodiscard]] std::vector<ThreadShare> sample(std::uint64_t now_us);

    // The pure part (also the unit-test surface on every platform): turns two readings into shares.
    [[nodiscard]] std::vector<ThreadShare> shares_between(const std::vector<ThreadCounters>& counters, std::uint64_t now_us,
        const std::vector<std::pair<std::uint64_t, std::string>>& labels, double units_per_second);

private:
    std::unordered_map<std::uint64_t, std::uint64_t> previous_;
    std::uint64_t previous_us_{};
};

// Reads the counters of every thread of this process (empty where the platform offers nothing). Defined in
// src/core/perf_diag_threads.cpp.
[[nodiscard]] std::vector<ThreadCounters> read_thread_counters();
// Counter units per second of read_thread_counters().
[[nodiscard]] double thread_counter_units_per_second() noexcept;

struct CpuInfo final {
    std::string model;                      // marketing name as the CPU reports it ("" = unknown)
    std::size_t logical{};                  // logical processors
    std::size_t cores{};                    // physical cores
    std::size_t performance_cores{};        // cores of the highest efficiency class (all cores when not hybrid)
    std::size_t efficiency_cores{};         // cores of a lower class (0 when not hybrid)
    std::size_t performance_logical{};      // logical processors on the performance cores
    bool topology_known{};                  // false: only the logical count was readable
};

// The machine's CPU, read once (a few microseconds of CPUID plus one operating system call).
[[nodiscard]] const CpuInfo& cpu_info() noexcept;

// Text forms (unit-test surface on every platform).
// /proc/self/task/<tid>/stat is the same format as /proc/self/stat: see parse_proc_self_stat_cpu.
// The "model name" line of /proc/cpuinfo, trimmed; nullopt when absent.
[[nodiscard]] std::optional<std::string> parse_cpuinfo_model(std::string_view text);
// The CPUID brand string bytes (three leaves of 16 bytes) cleaned of padding and repeated blanks.
[[nodiscard]] std::string clean_cpu_brand(std::string_view raw);

} // namespace eawr::core::perf_diag
