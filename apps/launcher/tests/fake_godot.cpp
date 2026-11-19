// A stand-in for the Godot runtime in the launcher smoke test. It is a console-subsystem program on
// purpose: if the launcher started it without CREATE_NO_WINDOW from a process that has a console
// window, it would share that window and report console_window=true.
//
// Environment: EAWR_FAKE_GODOT_RECORD  file that receives {"argv":[...],"cwd":...,"console_window":bool}
//              EAWR_FAKE_GODOT_EXIT    exit code (default 0)
//              EAWR_FAKE_GODOT_FAILURE text written as the viewer's {"status":"failed","failure":...} report
//              EAWR_FAKE_GODOT_SLEEP_MS keep running this long before exiting (instance tests)
// The record also holds the APPDATA and PATH it was started with (the per-instance environment block).
#include "text_formats.hpp"
#include "win_platform.hpp"
#include "launch_plan.hpp"

#include <windows.h>

#include <fstream>
#include <stdlib.h>
#include <algorithm>

namespace {

std::wstring environment(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size <= 1) return {};
    std::wstring value(size, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), size);
    value.resize(length < size ? length : 0);
    return value;
}

int tree_fixture(int argc, wchar_t** argv) {
    using namespace eawr::launcher;
    const fs::path directory(environment(L"EAWR_FAKE_TREE_DIR"));
    std::vector<std::wstring> arguments(argv + 1, argv + argc);
    const bool leaf = std::find(arguments.begin(), arguments.end(), L"--tree-leaf") != arguments.end();
    const bool end = std::find(arguments.begin(), arguments.end(), L"--tree-end") != arguments.end();
    const bool manager = std::find(arguments.begin(), arguments.end(), L"--eawr-mods-service") != arguments.end();
    DWORD child = 0;
    if (!end) {
        auto command = quote_command_line(argv[0], {leaf ? L"--tree-end" : L"--tree-leaf"});
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(argv[0], command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return 1;
        child = process.dwProcessId;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    WNDCLASSW type{};
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"EAWRWatchdogFixture";
    RegisterClassW(&type);
    HWND window = CreateWindowW(type.lpszClassName, L"Synthetic watchdog child", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                0, 0, 200, 100, nullptr, nullptr, type.hInstance, nullptr);
    if (!window) return 2;
    const auto healthy_state = environment(L"EAWR_FAKE_HEALTHY_STATE");
    if (healthy_state == L"minimized") ShowWindow(window, SW_MINIMIZE);
    const std::string role = manager ? "manager" : "game";
    const std::string name = leaf || end ? "leaf-" + std::to_string(GetCurrentProcessId()) : role;
    std::ofstream(directory / (name + ".json")) << "{\"pid\":" << GetCurrentProcessId() << ",\"child\":" << child << "}";
    const auto deadline = GetTickCount64() + 120000;
    ULONGLONG next_health_sample = 0;
    unsigned message_pumps = 0;
    while (GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        ++message_pumps;
        // A healthy loading/cache-build or paused window presents no frames, but still pumps messages.
        // Exercise that distinction with the same real Windows window the native watchdog observes.
        if (!healthy_state.empty() && GetTickCount64() >= next_health_sample) {
            next_health_sample = GetTickCount64() + 250;
            std::ofstream(directory / ("health-" + std::to_string(GetCurrentProcessId()) + ".json"))
                << "{\"state\":" << json_quote(utf8_from_wide(healthy_state)) << ",\"message_pumps\":" << message_pumps
                << ",\"presented_frames\":0,\"minimized\":" << (IsIconic(window) ? "true" : "false") << "}";
        }
        Sleep(10);
    }
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    using namespace eawr::launcher;
    if (argc == 3 && std::wstring(argv[1]) == L"--handoff-probe") {
        Sleep(1000); // The detached updater stand-in must survive the old native launcher's exit.
        std::ofstream(fs::path(argv[2])) << "alive";
        return 0;
    }
    if (!environment(L"EAWR_FAKE_TREE_DIR").empty()) return tree_fixture(argc, argv);
    std::wstring cwd(MAX_PATH, L'\0');
    cwd.resize(GetCurrentDirectoryW(static_cast<DWORD>(cwd.size()), cwd.data()));

    const std::wstring record = environment(L"EAWR_FAKE_GODOT_RECORD");
    if (!record.empty()) {
        std::string json = "{\"argv\":[";
        for (int index = 0; index < argc; ++index) {
            if (index != 0) json += ",";
            json += json_quote(utf8_from_wide(argv[index]));
        }
        json += "],\"cwd\":" + json_quote(utf8_from_wide(cwd));
        json += ",\"appdata\":" + json_quote(utf8_from_wide(environment(L"APPDATA")));
        json += ",\"path\":" + json_quote(utf8_from_wide(environment(L"PATH")));
        json += std::string(",\"console_window\":") + (GetConsoleWindow() != nullptr ? "true" : "false") + "}\n";
        std::ofstream(fs::path(record), std::ios::binary | std::ios::trunc) << json;
    }

    const std::wstring failure = environment(L"EAWR_FAKE_GODOT_FAILURE");
    if (!failure.empty()) {
        for (int index = 0; index + 1 < argc; ++index) {
            if (std::wstring(argv[index]) == L"--eawr-report") {
                std::ofstream(fs::path(argv[index + 1]), std::ios::binary | std::ios::trunc)
                    << "{\"status\":\"failed\",\"failure\":" << json_quote(utf8_from_wide(failure)) << "}\n";
            }
        }
    }
    const std::wstring sleep = environment(L"EAWR_FAKE_GODOT_SLEEP_MS");
    if (!sleep.empty()) Sleep(static_cast<DWORD>(_wtoi(sleep.c_str())));
    const std::wstring code = environment(L"EAWR_FAKE_GODOT_EXIT");
    return code.empty() ? 0 : _wtoi(code.c_str());
}
