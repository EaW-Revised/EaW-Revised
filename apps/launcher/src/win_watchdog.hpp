#pragma once

#include "watchdog_policy.hpp"
#include "paths.hpp" // include-guard: keep (declares the fs namespace alias used below)
#include <windows.h>
#include <string>
#include <vector>

namespace eawr::launcher {

// Runs on the native launch loop, independent of either Godot render thread.
class WindowsWatchdog {
public:
    ~WindowsWatchdog();
    void configure(const fs::path& directory, bool show_window);
    [[nodiscard]] const std::string& token() const { return token_; }
    [[nodiscard]] bool tick(const std::vector<DWORD>& processes);
private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void show(bool prompt);
    fs::path directory_;
    std::string token_;
    HWND window_ = nullptr;
    bool show_window_ = false;
    bool force_close_ = false;
    ULONGLONG last_poll_ = 0;
    HangTracker hang_;
    std::uint64_t prompt_after_ms_ = kHungPromptAfterMs;
};

} // namespace eawr::launcher
