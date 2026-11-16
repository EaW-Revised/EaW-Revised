// The digest pause gate (#6613): a running game holds the digest back; an unreadable state file only for a bounded grace.
#include "digest_pause.hpp"

#include <chrono>
#include <iostream>

namespace {
using eawr::mods_service::DigestPauseGate;
using eawr::mods_service::ProfileLocks;
using namespace std::chrono_literals;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::cerr << "FAILED: " << what << '\n'; ++failures; }
}
}

int main() {
    const auto t0 = DigestPauseGate::Clock::time_point{} + 1000s;
    ProfileLocks state;
    DigestPauseGate gate(10s);

    check(!gate.paused(state, t0), "nothing running: not paused");

    // A live game pauses, however long it runs.
    state.locks.push_back({"p", {1}});
    check(gate.paused(state, t0 + 1s), "a running game pauses");
    check(gate.paused(state, t0 + 3600s), "a running game keeps pausing");
    state.locks.clear();
    check(!gate.paused(state, t0 + 3601s), "the game exited: not paused");

    // An unreadable state holds the digest back for the grace, then stops holding it.
    state.unreadable = {5};
    check(gate.paused(state, t0 + 4000s), "unreadable: paused at first sight (the file may be mid-replace)");
    check(gate.paused(state, t0 + 4009s), "unreadable: still within the grace");
    check(!gate.paused(state, t0 + 4010s), "unreadable for the whole grace: stale, not paused");
    check(!gate.paused(state, t0 + 9000s), "a corrupt file does not pause for good");

    // A running game still pauses next to a stale unreadable state.
    state.locks.push_back({"p", {1}});
    check(gate.paused(state, t0 + 9001s), "a live game pauses even while another state stays corrupt");
    state.locks.clear();
    check(!gate.paused(state, t0 + 9002s), "back to stale-only: not paused");

    // A recovered file starts a fresh grace if it breaks again.
    state.unreadable.clear();
    check(!gate.paused(state, t0 + 9100s), "recovered: not paused");
    state.unreadable = {5};
    check(gate.paused(state, t0 + 9101s), "broken again: a new grace begins");
    check(!gate.paused(state, t0 + 9111s), "and ends");

    // Each instance has its own grace.
    state.unreadable = {5, 6};
    check(gate.paused(state, t0 + 9200s), "second broken instance is waited for independently");
    check(!gate.paused(state, t0 + 9211s), "both stale after the grace");

    if (failures == 0) std::cout << "digest_pause_tests: ok\n";
    return failures == 0 ? 0 : 1;
}
