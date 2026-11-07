#include "instance.hpp"
#include "launch_plan.hpp"
#include "win_platform.hpp"

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

    ChildResult run(const fs::path& exe, const std::vector<std::wstring>& args, const fs::path& working_directory) override {
        return run_monitored(exe, args, working_directory, {}, {});
    }

    ChildResult run_monitored(const fs::path& exe, const std::vector<std::wstring>& args, const fs::path& working_directory,
                              const std::function<void()>& poll, const std::function<bool(unsigned)>& started) override {
        ChildResult result;
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
                                            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | (started ? CREATE_SUSPENDED : 0),
                                            appdata_ ? environment.data() : nullptr,
                                            working_directory.empty() ? nullptr : working_directory.c_str(), &startup, &process);
        if (!created) {
            result.error = last_error_text(GetLastError());
            return result;
        }
        result.started = true;
        if (started && !started(process.dwProcessId)) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, INFINITE);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            result.started = false;
            result.error = "could not publish the running game state";
            return result;
        }
        if (started && ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
            result.error = "could not resume the game: " + last_error_text(GetLastError());
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, INFINITE);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            result.started = false;
            return result;
        }
        CloseHandle(process.hThread);
        while (WaitForSingleObject(process.hProcess, poll ? 100 : INFINITE) == WAIT_TIMEOUT) {
            if (poll) poll();
        }
        DWORD code = 0;
        if (!GetExitCodeProcess(process.hProcess, &code)) code = 1;
        CloseHandle(process.hProcess);
        result.exit_code = code;
        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        return result;
    }

private:
    std::optional<fs::path> appdata_;
};

} // namespace

std::unique_ptr<ProcessRunner> make_windows_process_runner() {
    return std::make_unique<WindowsProcessRunner>();
}

} // namespace eawr::launcher
