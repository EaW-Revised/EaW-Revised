#pragma once

#include "watchdog_policy.hpp"
#include "paths.hpp" // include-guard: keep (declares the fs namespace alias used below)
#include <windows.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace eawr::launcher {

// Samples on the native launch loop, independent of either Godot render thread. The blocking window probe (a
// no-op message with a short timeout) runs on a helper thread so a hung window never stalls the launch loop.
class WindowsWatchdog {
public:
    ~WindowsWatchdog();
    void configure(const fs::path& directory, bool show_window);
    [[nodiscard]] const std::string& token() const { return token_; }
    [[nodiscard]] bool tick(const std::vector<DWORD>& processes);
private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void show(bool prompt);
    // Harvests a finished probe, then starts the next one. Returns the pid of a process whose window did not
    // answer the latest finished probe, or 0.
    DWORD sample_probe(const std::vector<DWORD>& processes);
    fs::path directory_;
    std::string token_;
    HWND window_ = nullptr;
    bool show_window_ = false;
    bool force_close_ = false;
    ULONGLONG last_poll_ = 0;
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
};

} // namespace eawr::launcher
