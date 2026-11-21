#include "win_watchdog.hpp"
#include "instance.hpp"
#include "text_formats.hpp" // include-guard: keep (re-exports JsonValue, parse_json and json_quote)

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace eawr::launcher {
namespace {
struct WatchedWindow {
    HWND handle = nullptr;
    DWORD pid = 0;
};

struct WindowCollector {
    const std::vector<DWORD>& processes;
    std::vector<WatchedWindow> windows;
};

BOOL CALLBACK collect_window(HWND window, LPARAM parameter) {
    auto& collector = *reinterpret_cast<WindowCollector*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (std::find(collector.processes.begin(), collector.processes.end(), pid) != collector.processes.end()
        && IsWindowVisible(window)) {
        collector.windows.push_back({window, pid});
    }
    return TRUE;
}

// A real answer: the window handles a no-op message within kProbeTimeoutMs. IsHungAppWindow is deliberately not
// used (an idle Godot window answers in milliseconds yet is flagged by it within seconds). SMTO_NORMAL, never
// SMTO_ABORTIFHUNG, which would give up at once on a window Windows already flags.
ProbeResult probe_window(const WatchedWindow& window) {
    DWORD_PTR ignored = 0;
    SetLastError(0);
    if (SendMessageTimeoutW(window.handle, WM_NULL, 0, 0, SMTO_NORMAL, static_cast<UINT>(kProbeTimeoutMs), &ignored)) {
        return ProbeResult::answered;
    }
    return GetLastError() == ERROR_TIMEOUT ? ProbeResult::timeout : ProbeResult::window_gone;
}
} // namespace

WindowsWatchdog::~WindowsWatchdog() {
    stop_probe_ = true;
    if (probe_thread_.joinable()) probe_thread_.join();
    if (window_) DestroyWindow(window_);
    if (!directory_.empty()) {
        std::error_code error;
        fs::remove(directory_ / "watchdog.json", error);
        fs::remove(directory_ / "force-close.json", error);
        fs::remove(directory_ / "watchdog.json.tmp", error);
        fs::remove(directory_, error); // Only this session's now-empty channel, never another manager's.
    }
}

void WindowsWatchdog::configure(const fs::path& directory, bool show_window) {
    directory_ = directory;
    show_window_ = show_window;
    token_ = std::to_string(GetCurrentProcessId()) + ":" + process_start_time(GetCurrentProcessId()).value_or("");
    // Test hook: the smoke tests shorten the sustained-hang wait; a player never sets it.
    wchar_t override_text[32]{};
    const DWORD length = GetEnvironmentVariableW(L"EAWR_WATCHDOG_PROMPT_AFTER_MS", override_text, 32);
    if (length > 0 && length < 32) {
        wchar_t* end = nullptr;
        const auto value = wcstoull(override_text, &end, 10);
        if (end != override_text && *end == L'\0') prompt_after_ms_ = value;
    }
    std::error_code error;
    fs::remove(directory_ / "force-close.json", error);
}

bool WindowsWatchdog::tick(const std::vector<DWORD>& processes) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (force_close_) return true;
    if (directory_.empty()) return false;
    // Read a bounded request. A stale request from a previous session never kills this one.
    std::ifstream request(directory_ / "force-close.json", std::ios::binary);
    std::string text(1025, '\0');
    request.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(request.gcount()));
    request.close();
    if (text.size() <= 1024) {
        const auto json = parse_json(text);
        if (json && json->kind == JsonValue::Kind::object) {
            const auto* token = json->string_member("token");
            const auto* action = json->string_member("action");
            if (token && *token == token_ && action && *action == "force-close") return true;
        }
    }
    if (GetTickCount64() - last_poll_ < 1000) return false;
    last_poll_ = GetTickCount64();
    const DWORD hung_pid = sample_probe(processes);
    // A probe that timed out is one failed sample; the prompt waits for consecutive ones (kHungPromptAfterMs).
    const HangVerdict verdict = observe_hang(hang_, hung_pid != 0, GetTickCount64(), prompt_after_ms_);
    const fs::path temporary = directory_ / "watchdog.json.tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << "{\"version\":1,\"token\":" << json_quote(token_) << ",\"hung\":" << (verdict.hung ? "true" : "false")
           << ",\"hung_ms\":" << verdict.hung_ms << ",\"prompt\":" << (verdict.prompt ? "true" : "false")
           << ",\"pid\":" << hung_pid << ",\"probe_ms\":" << timing_.last_probe_ms
           << ",\"since_answer_ms\":" << since_answer_ms(timing_, GetTickCount64()) << "}\n";
    output.close();
    if (output) MoveFileExW(temporary.c_str(), (directory_ / "watchdog.json").c_str(), MOVEFILE_REPLACE_EXISTING);
    show(verdict.prompt);
    return force_close_;
}

DWORD WindowsWatchdog::sample_probe(const std::vector<DWORD>& processes) {
    if (probe_running_.load(std::memory_order_acquire)) return hung_pid_; // The previous probe is still waiting.
    if (probe_thread_.joinable()) {
        probe_thread_.join();
        hung_pid_ = probe_hung_pid_.load();
        note_probe(timing_, probe_answered_.load(), probe_latency_ms_.load(), probe_finished_at_ms_.load());
    }
    WindowCollector collector{processes, {}};
    EnumWindows(collect_window, reinterpret_cast<LPARAM>(&collector));
    if (collector.windows.empty()) {
        hung_pid_ = 0;
        return 0;
    }
    probe_running_.store(true, std::memory_order_release);
    try {
        probe_thread_ = std::thread([this, windows = std::move(collector.windows)] {
            const ULONGLONG started = GetTickCount64();
            const auto summary = first_unanswered(windows, [this](const WatchedWindow& window) {
                return stop_probe_.load() ? ProbeResult::answered : probe_window(window);
            });
            const ULONGLONG finished = GetTickCount64();
            probe_latency_ms_.store(finished - started);
            probe_finished_at_ms_.store(finished);
            probe_answered_.store(summary.answered);
            probe_hung_pid_.store(summary.hung ? windows[summary.hung_index].pid : 0);
            probe_running_.store(false, std::memory_order_release);
        });
    } catch (const std::system_error&) {
        probe_running_.store(false, std::memory_order_release); // No thread now: keep the last outcome, retry next tick.
    }
    return hung_pid_;
}

void WindowsWatchdog::show(bool prompt) {
    if (!show_window_) return;
    if (!prompt) {
        if (window_) ShowWindow(window_, SW_HIDE);
        return;
    }
    if (!window_) {
        WNDCLASSW type{};
        type.lpfnWndProc = window_proc;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"EAWRProcessWatchdog";
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&type);
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, type.lpszClassName, L"EAWR Mods is busy",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                                  440, 185, nullptr, nullptr, type.hInstance, this);
        if (window_) {
            CreateWindowW(L"STATIC", L"The mod manager window has not answered for a while. It may just be busy, so you can wait.\nForce close ends this session and all its children.",
                          WS_CHILD | WS_VISIBLE, 15, 15, 400, 65, window_, nullptr, type.hInstance, nullptr);
            CreateWindowW(L"BUTTON", L"Force close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                          150, 95, 120, 30, window_, reinterpret_cast<HMENU>(1), type.hInstance, nullptr);
        }
    }
    if (window_ && !IsWindowVisible(window_)) ShowWindow(window_, SW_SHOWNOACTIVATE);
}

LRESULT CALLBACK WindowsWatchdog::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<WindowsWatchdog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self && (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == 1))) {
        self->force_close_ = true;
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace eawr::launcher
