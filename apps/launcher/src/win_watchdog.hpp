#pragma once

#include "watchdog_policy.hpp"
#include "paths.hpp" // include-guard: keep (declares the fs namespace alias used below)
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace eawr::launcher {

// Watches the mod manager (the Godot launcher window) for a frozen window. It runs on its OWN thread, so nothing on
// the launch loop (a modal error dialog, a slow poll) can starve it, and it needs no GPU: the prompt is a plain
// Win32 window. The blocking window probe (a no-op message with a short timeout) runs on a helper thread of its own
// so a hung window never stalls the watchdog thread either.
//
// A hung launcher gets a prompt (topmost, with a taskbar button that flashes; it takes the focus only when no game
// runs). "Force close" ends ONLY the launcher's job object, never a running game's, and EAWR starts the launcher
// again. A launcher still hung kHungAutoEndAfterPromptMs after the prompt is ended the same way without asking.
class WindowsWatchdog {
public:
    ~WindowsWatchdog();
    // Starts the watchdog thread. `directory` is this session's channel (watchdog.json, force-close.json).
    void configure(const fs::path& directory, bool show_window);
    [[nodiscard]] const std::string& token() const { return token_; }
    // The launcher (manager) process and the job object holding it and its children. The watchdog ends that job.
    void watch_launcher(DWORD pid, HANDLE job);
    void unwatch_launcher();
    // A game runs: the prompt must not take the focus from it.
    void set_game_running(bool running) { game_running_.store(running); }
    // The session owner asked for the whole session to end (force-close.json with this session's token).
    [[nodiscard]] bool session_close_requested() const { return session_close_.load(); }
private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void run();
    void check_session_close_request();
    void sample(ULONGLONG now);
    void show(bool prompt);
    void end_launcher(DWORD exit_code, const char* how);
    // Harvests a finished probe, then starts the next one. Returns the pid of a process whose window did not
    // answer the latest finished probe, or 0.
    DWORD sample_probe(const std::vector<DWORD>& processes);
    fs::path directory_;
    std::string token_;
    HWND window_ = nullptr;       // Watchdog thread only.
    bool prompt_shown_ = false;   // Watchdog thread only: this prompt episode was already shown/flashed.
    bool show_window_ = false;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> session_close_{false};
    std::atomic<bool> end_requested_{false}; // The player chose Force close (set by the window procedure).
    std::atomic<bool> game_running_{false};
    std::mutex mutex_;                // Guards the watched launcher below.
    DWORD watched_pid_ = 0;
    HANDLE watched_job_ = nullptr;
    std::string last_end_;            // "", "force" or "auto": how the launcher was last ended (watchdog thread only).
    ULONGLONG last_poll_ = 0;
    ULONGLONG last_request_check_ = 0;
    HangTracker hang_;
    std::thread probe_thread_;
    std::atomic<bool> probe_running_{false};
    std::atomic<bool> stop_probe_{false};
    std::atomic<std::uint64_t> probe_latency_ms_{0}; // Written by the probe thread before probe_running_ clears.
    std::atomic<std::uint64_t> probe_finished_at_ms_{0}; // When the probe got its outcome (probe thread, before probe_running_ clears).
    std::atomic<bool> probe_answered_{false};            // The probe got a real answer (not zero windows, not a timeout).
    ProbeTiming timing_;
    std::atomic<DWORD> probe_hung_pid_{0}; // Written by the probe thread before probe_running_ clears.
    DWORD hung_pid_ = 0;                   // The latest harvested outcome: the unanswering process, or 0.
    std::uint64_t prompt_after_ms_ = kHungPromptAfterMs;
    std::uint64_t auto_end_after_prompt_ms_ = kHungAutoEndAfterPromptMs;
};

} // namespace eawr::launcher
