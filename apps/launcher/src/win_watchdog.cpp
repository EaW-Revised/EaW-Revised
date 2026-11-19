#include "win_watchdog.hpp"
#include "instance.hpp"
#include "text_formats.hpp" // include-guard: keep (re-exports JsonValue, parse_json and json_quote)

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace eawr::launcher {
namespace {
struct WindowProbe {
    const std::vector<DWORD>& processes;
    DWORD hung_pid = 0;
};

BOOL CALLBACK probe_window(HWND window, LPARAM parameter) {
    auto& probe = *reinterpret_cast<WindowProbe*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (std::find(probe.processes.begin(), probe.processes.end(), pid) != probe.processes.end()
        && IsWindowVisible(window) && IsHungAppWindow(window)) {
        probe.hung_pid = pid;
        return FALSE;
    }
    return TRUE;
}
} // namespace

WindowsWatchdog::~WindowsWatchdog() {
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
    WindowProbe probe{processes, 0};
    EnumWindows(probe_window, reinterpret_cast<LPARAM>(&probe));
    // IsHungAppWindow uses Windows' message-pump timeout (about five seconds); the prompt waits for a longer hang.
    const HangVerdict verdict = observe_hang(hang_, probe.hung_pid != 0, GetTickCount64(), prompt_after_ms_);
    const fs::path temporary = directory_ / "watchdog.json.tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << "{\"version\":1,\"token\":" << json_quote(token_) << ",\"hung\":" << (verdict.hung ? "true" : "false")
           << ",\"hung_ms\":" << verdict.hung_ms << ",\"prompt\":" << (verdict.prompt ? "true" : "false")
           << ",\"pid\":" << probe.hung_pid << "}\n";
    output.close();
    if (output) MoveFileExW(temporary.c_str(), (directory_ / "watchdog.json").c_str(), MOVEFILE_REPLACE_EXISTING);
    show(verdict.prompt);
    return force_close_;
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
