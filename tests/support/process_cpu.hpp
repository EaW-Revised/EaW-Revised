#pragma once

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#endif
#include <cstdint>
#include <stdexcept>

namespace eawr::testing {
inline double process_cpu_ms() {
#if defined(_WIN32)
    FILETIME created{}, ended{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user))
        throw std::runtime_error("cannot read process CPU time");
    const auto count = [](FILETIME value) {
        return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    };
    return static_cast<double>(count(kernel) + count(user)) / 10'000.0;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) throw std::runtime_error("cannot read process CPU time");
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0
        + static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
#endif
}
} // namespace eawr::testing
