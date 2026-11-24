#include "eawr/core/perf_diag/sim_feed.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#if defined(_M_X64) || defined(_M_IX86)
#include <intrin.h>
#endif
#elif defined(__linux__)
#include <ctime>
#endif

namespace eawr::core::perf_diag {

ThreadCpuStamp thread_cpu_now() noexcept {
#if defined(_WIN32) && (defined(_M_X64) || defined(_M_IX86))
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(GetCurrentThread(), &cycles)) return {};
    return {static_cast<std::uint64_t>(cycles), static_cast<std::uint64_t>(__rdtsc()), true};
#elif defined(__linux__)
    timespec cpu{}, wall{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) != 0 || clock_gettime(CLOCK_MONOTONIC, &wall) != 0) return {};
    const auto nanos = [](const timespec& t) {
        return static_cast<std::uint64_t>(t.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(t.tv_nsec);
    };
    return {nanos(cpu), nanos(wall), true};
#else
    return {};
#endif
}

float thread_cpu_ratio(const ThreadCpuStamp& begin, const ThreadCpuStamp& end) noexcept {
    if (!begin.valid || !end.valid || end.wall <= begin.wall || end.cpu < begin.cpu) return cpu_ratio_unknown;
    const double ratio = static_cast<double>(end.cpu - begin.cpu) / static_cast<double>(end.wall - begin.wall);
    // A core boosting above the TSC rate can read above 1 on Windows; beyond 4x no CPU produces, so it is garbage.
    if (!(ratio >= 0.0) || ratio > 4.0) return cpu_ratio_unknown;
    return static_cast<float>(ratio);
}

} // namespace eawr::core::perf_diag
