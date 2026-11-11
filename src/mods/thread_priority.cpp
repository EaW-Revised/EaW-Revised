#include "thread_priority.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace eawr::mods {

void lower_current_thread_priority() {
#ifdef _WIN32
    // Below normal CPU priority plus background I/O and memory priority: the game's own loading wins the disk.
    if (!SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN)) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__)
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
}

unsigned long current_process_id() {
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

void set_hash_thread_background(bool background, bool background_io) {
#ifdef _WIN32
    if (background) {
        if (!background_io || !SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN))
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    } else {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    }
#else
    // Linux cannot restore a raised nice value without privileges. Workers yield
    // cooperatively instead, so switching to a waiting screen remains reversible.
    static_cast<void>(background);
    static_cast<void>(background_io);
#endif
}

} // namespace eawr::mods
