#include "win_watchdog.hpp"
#include "instance.hpp"
#include "launcher_flow.hpp"
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

// Test hook: the smoke tests shorten the waits; a player never sets these.
void read_override(const wchar_t* name, std::uint64_t& value) {
    wchar_t text[32]{};
    const DWORD length = GetEnvironmentVariableW(name, text, 32);
    if (length == 0 || length >= 32) return;
    wchar_t* end = nullptr;
    const auto parsed = wcstoull(text, &end, 10);
    if (end != text && *end == L'\0') value = parsed;
}
} // namespace

WindowsWatchdog::~WindowsWatchdog() {
    stop_ = true;
    if (thread_.joinable()) thread_.join(); // The thread destroys its own prompt window.
    stop_probe_ = true;
    if (probe_thread_.joinable()) probe_thread_.join();
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
    read_override(L"EAWR_WATCHDOG_PROMPT_AFTER_MS", prompt_after_ms_);
    read_override(L"EAWR_WATCHDOG_AUTO_END_AFTER_PROMPT_MS", auto_end_after_prompt_ms_);
    std::error_code error;
    fs::remove(directory_ / "force-close.json", error);
    if (directory_.empty() || thread_.joinable()) return;
    try {
        thread_ = std::thread([this] { run(); });
    } catch (const std::system_error&) {
        // No watchdog thread: the launcher still runs, only without the hung-window prompt.
    }
}

void WindowsWatchdog::watch_launcher(DWORD pid, HANDLE job) {
    const std::lock_guard lock(mutex_);
    watched_pid_ = pid;
    watched_job_ = job;
}

void WindowsWatchdog::unwatch_launcher() {
    const std::lock_guard lock(mutex_);
    watched_pid_ = 0;
    watched_job_ = nullptr;
}

void WindowsWatchdog::run() {
    while (!stop_.load()) {
        // Messages of the prompt window (its buttons) are handled here, on the thread that created it.
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        const ULONGLONG now = GetTickCount64();
        if (now - last_request_check_ >= 100) {
            last_request_check_ = now;
            check_session_close_request();
        }
        if (now - last_poll_ >= 1000) {
            last_poll_ = now;
            sample(now);
        }
        if (end_requested_.exchange(false)) end_launcher(kExitLauncherForceClosed, "force");
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
    }
    if (window_) DestroyWindow(window_);
    window_ = nullptr;
}

// Read a bounded request. A stale request from a previous session never kills this one.
void WindowsWatchdog::check_session_close_request() {
    if (session_close_.load() || directory_.empty()) return;
    std::ifstream request(directory_ / "force-close.json", std::ios::binary);
    std::string text(1025, '\0');
    request.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(request.gcount()));
    request.close();
    if (text.size() > 1024) return;
    const auto json = parse_json(text);
    if (!json || json->kind != JsonValue::Kind::object) return;
    const auto* token = json->string_member("token");
    const auto* action = json->string_member("action");
    if (token && *token == token_ && action && *action == "force-close") session_close_ = true;
}

void WindowsWatchdog::sample(ULONGLONG now) {
    std::vector<DWORD> processes;
    {
        const std::lock_guard lock(mutex_);
        if (watched_pid_ != 0) processes.push_back(watched_pid_);
    }
    const DWORD hung_pid = sample_probe(processes);
    // A probe that timed out is one failed sample; the prompt waits for consecutive ones (kHungPromptAfterMs).
    const HangVerdict verdict = observe_hang(hang_, hung_pid != 0, now, prompt_after_ms_, auto_end_after_prompt_ms_);
    const fs::path temporary = directory_ / "watchdog.json.tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << "{\"version\":1,\"token\":" << json_quote(token_) << ",\"hung\":" << (verdict.hung ? "true" : "false")
           << ",\"hung_ms\":" << verdict.hung_ms << ",\"prompt\":" << (verdict.prompt ? "true" : "false")
           << ",\"auto_end\":" << (verdict.auto_end ? "true" : "false") << ",\"ended\":" << json_quote(last_end_)
           << ",\"pid\":" << hung_pid << ",\"probe_ms\":" << timing_.last_probe_ms
           << ",\"since_answer_ms\":" << since_answer_ms(timing_, now) << "}\n";
    output.close();
    if (output) MoveFileExW(temporary.c_str(), (directory_ / "watchdog.json").c_str(), MOVEFILE_REPLACE_EXISTING);
    show(verdict.prompt);
    if (verdict.auto_end) end_launcher(kExitLauncherAutoEnded, "auto");
}

// Ends the launcher's job (the launcher and its children) and nothing else: a running game has a job of its own.
void WindowsWatchdog::end_launcher(DWORD exit_code, const char* how) {
    {
        const std::lock_guard lock(mutex_);
        if (watched_pid_ == 0 || watched_job_ == nullptr) return;
        TerminateJobObject(watched_job_, exit_code);
        watched_pid_ = 0; // Ended once; the launch loop unwatches it when the process is gone.
        watched_job_ = nullptr;
    }
    last_end_ = how;
    hang_ = HangTracker{};
    hung_pid_ = 0;
    show(false);
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
        probe_running_.store(false, std::memory_order_release); // No thread now: keep the last outcome, retry next sample.
    }
    return hung_pid_;
}

void WindowsWatchdog::show(bool prompt) {
    if (!show_window_) return;
    if (!prompt) {
        prompt_shown_ = false;
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
        // Topmost, so a frozen launcher or a game never hides it, and a normal application window (WS_EX_APPWINDOW),
        // so it has a taskbar button that can flash.
        window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW, type.lpszClassName, L"EAWR Mods is busy",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                                  470, 215, nullptr, nullptr, type.hInstance, this);
        if (window_) {
            const std::wstring text = L"The mod manager window has not answered for a while. It may just be busy, so you can wait.\n\n"
                L"Force close ends the mod manager and starts it again; a running game is not affected. "
                L"If it still does not answer, EAWR ends it automatically about "
                + std::to_wstring(auto_end_after_prompt_ms_ / 1000) + L" seconds from now.";
            CreateWindowW(L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE, 15, 15, 430, 105, window_, nullptr, type.hInstance, nullptr);
            CreateWindowW(L"BUTTON", L"Force close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                          165, 130, 130, 32, window_, reinterpret_cast<HMENU>(1), type.hInstance, nullptr);
        }
    }
    if (!window_ || prompt_shown_) return;
    prompt_shown_ = true;
    // With a game running the window must not steal the focus (a fullscreen game would lose its input): it only
    // shows and flashes. With no game it is activated so the player sees it at once.
    if (choose_prompt_show(game_running_.load()) == PromptShow::activate) {
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_); // Windows may refuse the focus; the flash below still shows the button.
    } else {
        ShowWindow(window_, SW_SHOWNOACTIVATE);
    }
    FLASHWINFO flash{};
    flash.cbSize = sizeof flash;
    flash.hwnd = window_;
    flash.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG; // Until the window comes to the front.
    FlashWindowEx(&flash);
    MessageBeep(MB_ICONEXCLAMATION);
}

LRESULT CALLBACK WindowsWatchdog::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<WindowsWatchdog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self && (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == 1))) {
        self->end_requested_ = true;
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace eawr::launcher
