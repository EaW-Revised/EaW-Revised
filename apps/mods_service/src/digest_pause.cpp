#include "digest_pause.hpp"

#include <algorithm>

namespace eawr::mods_service {

bool DigestPauseGate::paused(const ProfileLocks& locks, Clock::time_point now) {
    std::lock_guard lock(mutex_);
    // Only instances that are unreadable right now keep a clock; one that recovered starts over if it breaks again.
    std::erase_if(unreadable_since_, [&](const auto& entry) {
        return std::find(locks.unreadable.begin(), locks.unreadable.end(), entry.first) == locks.unreadable.end();
    });
    bool waiting = false;
    for (const int instance : locks.unreadable) {
        const auto first = unreadable_since_.try_emplace(instance, now).first->second;
        if (now - first < grace_) waiting = true;
    }
    return !locks.locks.empty() || waiting;
}

} // namespace eawr::mods_service
