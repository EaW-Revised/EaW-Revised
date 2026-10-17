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

class WindowsProcessRunner final : public ProcessRunner {
public:
    ChildResult run(const fs::path& exe, const std::vector<std::wstring>& args, const fs::path& working_directory) override {
        ChildResult result;
        std::wstring command_line = quote_command_line(exe.wstring(), args);
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        PROCESS_INFORMATION process{};
        const auto begin = std::chrono::steady_clock::now();
        // CREATE_NO_WINDOW: a console-subsystem child (the console Godot runtime, a script) gets no
        // visible console. The GUI runtime has none anyway. No handles are inherited.
        const BOOL created = CreateProcessW(exe.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                                            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
                                            working_directory.empty() ? nullptr : working_directory.c_str(), &startup, &process);
        if (!created) {
            result.error = last_error_text(GetLastError());
            return result;
        }
        result.started = true;
        CloseHandle(process.hThread);
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD code = 0;
        if (!GetExitCodeProcess(process.hProcess, &code)) code = 1;
        CloseHandle(process.hProcess);
        result.exit_code = code;
        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        return result;
    }
};

} // namespace

std::unique_ptr<ProcessRunner> make_windows_process_runner() {
    return std::make_unique<WindowsProcessRunner>();
}

} // namespace eawr::launcher
