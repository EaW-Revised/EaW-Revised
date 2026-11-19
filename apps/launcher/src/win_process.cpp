#include "instance.hpp"
#include "launch_plan.hpp"
#include "win_platform.hpp"
#include "win_watchdog.hpp"

#include <windows.h>

#include <chrono>

namespace eawr::launcher {

namespace {

std::string last_error_text(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text = length != 0 && buffer != nullptr ? std::wstring(buffer, length) : std::wstring();
    if (buffer != nullptr) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'.')) text.pop_back();
    return utf8_from_wide(text) + " (error " + std::to_string(code) + ")";
}

std::vector<std::wstring> current_environment() {
    std::vector<std::wstring> entries;
    wchar_t* block = GetEnvironmentStringsW();
    if (block == nullptr) return entries;
    for (const wchar_t* entry = block; *entry != L'\0'; entry += wcslen(entry) + 1) entries.emplace_back(entry);
    FreeEnvironmentStringsW(block);
    return entries;
}

class WindowsProcessRunner final : public ProcessRunner {
public:
    void set_appdata(const std::optional<fs::path>& folder) override { appdata_ = folder; }
    void configure_watchdog(const fs::path& directory, bool show_window) override { watchdog_.configure(directory, show_window); }
    std::string watchdog_token() const override { return watchdog_.token(); }
    bool session_closed() const override { return session_closed_; }

    bool start_detached(const fs::path& exe, const std::vector<std::wstring>& args, const fs::path& cwd) override {
        // The updater intentionally survives this session. Only the native parent (outside its child jobs)
        // starts it, after the manager has exited; ordinary Godot children cannot break out of their job.
        auto command = quote_command_line(exe.wstring(), args);
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, cwd.c_str(), &startup, &process)) return false;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }

    ChildResult run(const fs::path& exe, const std::vector<std::wstring>& args, const fs::path& working_directory) override {
        return run_monitored(exe, args, working_directory, {}, {});
    }

    ChildResult run_monitored(const fs::path& exe, const std::vector<std::wstring>& args, const fs::path& working_directory,
                              const std::function<void()>& poll, const std::function<bool(unsigned)>& started) override {
        ChildResult result;
        // Non-inheritable job handle: a crash of EAWR closes the last handle and kills descendants.
        struct Job {
            HANDLE handle = CreateJobObjectW(nullptr, nullptr);
            ~Job() { if (handle) CloseHandle(handle); }
        } job;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job.handle || !SetInformationJobObject(job.handle, JobObjectExtendedLimitInformation, &limits, sizeof limits)) {
            result.error = "could not contain the child processes: " + last_error_text(GetLastError());
            return result;
        }
        std::wstring command_line = quote_command_line(exe.wstring(), args);
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        PROCESS_INFORMATION process{};
        const auto begin = std::chrono::steady_clock::now();
        // CREATE_NO_WINDOW: a console-subsystem child (the console Godot runtime, a script) gets no
        // visible console. The GUI runtime has none anyway. No handles are inherited.
        // Instance n >= 2: the child gets the inherited environment with only APPDATA replaced.
        std::wstring environment;
        if (appdata_) environment = build_environment_block(current_environment(), L"APPDATA", appdata_->wstring());
        const BOOL created = CreateProcessW(exe.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                                            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                                            appdata_ ? environment.data() : nullptr,
                                            working_directory.empty() ? nullptr : working_directory.c_str(), &startup, &process);
        if (!created) {
            result.error = last_error_text(GetLastError());
            return result;
        }
        result.started = true;
        if (!AssignProcessToJobObject(job.handle, process.hProcess)) {
            result.error = "could not contain the child processes: " + last_error_text(GetLastError());
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            result.started = false;
            return result;
        }
        // The foreground manager grants this launch loop permission at Play.
        // Transfer it to the child before resuming its first window creation.
        // This is permission only: the child activates once, never from polling.
        AllowSetForegroundWindow(process.dwProcessId);
        if (started && !started(process.dwProcessId)) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            result.started = false;
            result.error = "could not publish the running game state";
            return result;
        }
        if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
            result.error = "could not resume the game: " + last_error_text(GetLastError());
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            result.started = false;
            return result;
        }
        CloseHandle(process.hThread);
        // Only the manager (the one child run with a poll) is watched for a not-responding window; a game that
        // blocks its message loop while it loads must never raise the Force close prompt.
        active_.push_back({process.hProcess, job.handle, process.dwProcessId, static_cast<bool>(poll)});
        while (WaitForSingleObject(process.hProcess, 100) == WAIT_TIMEOUT) {
            // Closing the manager window leaves a running game alone; its trees end when the game does, when
            // EAWR itself dies (kill-on-close) or when the user picks Force close.
            std::vector<DWORD> pids;
            for (const auto& active : active_) {
                if (active.watched) pids.push_back(active.pid);
            }
            if (watchdog_.tick(pids)) {
                session_closed_ = true;
                for (const auto& active : active_) TerminateJobObject(active.job, kExitForceClosed);
            }
            if (session_closed_) continue;
            if (poll) poll();
        }
        active_.pop_back();
        DWORD code = 0;
        if (!GetExitCodeProcess(process.hProcess, &code)) code = 1;
        CloseHandle(process.hProcess);
        result.exit_code = code;
        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        return result;
    }

private:
    struct Active { HANDLE process; HANDLE job; DWORD pid; bool watched; };
    std::vector<Active> active_;
    WindowsWatchdog watchdog_;
    bool session_closed_ = false;
    std::optional<fs::path> appdata_;
};

} // namespace

std::unique_ptr<ProcessRunner> make_windows_process_runner() {
    return std::make_unique<WindowsProcessRunner>();
}

} // namespace eawr::launcher
