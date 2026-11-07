#include "eawr/core/load_profile.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/resource.h>
#endif

namespace eawr::core::load_profile {

// Presentation diagnostics only: the host's storage read counter, read at the edges of a profiled scope.
std::uint64_t process_read_bytes() noexcept {
#if defined(_WIN32)
    IO_COUNTERS counters{};
    if (!GetProcessIoCounters(GetCurrentProcess(), &counters)) return 0;
    return counters.ReadTransferCount;
#elif defined(__linux__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
    return static_cast<std::uint64_t>(usage.ru_inblock) * 512U; // block input operations, not byte-exact
#else
    return 0;
#endif
}

} // namespace eawr::core::load_profile
