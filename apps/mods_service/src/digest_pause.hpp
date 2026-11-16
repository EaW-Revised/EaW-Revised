#pragma once
// When the manager's background content digest should stand still (#6588, #6613).
// A live game holds the digest back (the game hashes the layers it mounts itself). A state file that cannot be
// read is not proof of a running game: it may be mid-replace, so it holds the digest back for a bounded grace, and
// then counts as stale, so a corrupt file never pauses the digest for good. Profile edits stay fail-closed on an
// unreadable state (profile_locks.hpp); only the digest pause is relaxed here.
#include "profile_locks.hpp"

#include <chrono>
#include <map>
#include <mutex>

namespace eawr::mods_service {

class DigestPauseGate {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::seconds kUnreadableGrace{10};

    explicit DigestPauseGate(std::chrono::milliseconds unreadable_grace = kUnreadableGrace) : grace_(unreadable_grace) {}

    // `locks` is the state the caller just read (read_profile_locks); the gate stores no reader, so this header needs
    // no <functional>. True while a game runs a profile, or while an unreadable state has been unreadable for less
    // than the grace. Safe to call from several digest jobs at once (each reads its own `locks`).
    [[nodiscard]] bool paused(const ProfileLocks& locks, Clock::time_point now = Clock::now());

private:
    std::chrono::milliseconds grace_;
    std::mutex mutex_;
    std::map<int, Clock::time_point> unreadable_since_;   // instance -> first poll that saw it unreadable
};

} // namespace eawr::mods_service
