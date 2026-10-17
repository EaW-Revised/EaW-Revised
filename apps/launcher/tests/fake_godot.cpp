// A stand-in for the Godot runtime in the launcher smoke test. It is a console-subsystem program on
// purpose: if the launcher started it without CREATE_NO_WINDOW from a process that has a console
// window, it would share that window and report console_window=true.
//
// Environment: EAWR_FAKE_GODOT_RECORD  file that receives {"argv":[...],"cwd":...,"console_window":bool}
//              EAWR_FAKE_GODOT_EXIT    exit code (default 0)
//              EAWR_FAKE_GODOT_FAILURE text written as the viewer's {"status":"failed","failure":...} report
#include "text_formats.hpp"
#include "win_platform.hpp"

#include <windows.h>

#include <fstream>
#include <stdlib.h>

namespace {

std::wstring environment(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size <= 1) return {};
    std::wstring value(size, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), size);
    value.resize(length < size ? length : 0);
    return value;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    using namespace eawr::launcher;
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
    const std::wstring code = environment(L"EAWR_FAKE_GODOT_EXIT");
    return code.empty() ? 0 : _wtoi(code.c_str());
}
