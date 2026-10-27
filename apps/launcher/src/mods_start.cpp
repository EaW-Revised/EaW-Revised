#include "mods_start.hpp"
#include "launch_plan.hpp"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <string>
#include <vector>

namespace {

int fail(const wchar_t* message) {
    MessageBoxW(nullptr, message, L"EAWR Mods", MB_OK | MB_ICONERROR);
    return 2;
}

std::filesystem::path own_path() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return buffer;
}

int create_shortcut(const std::filesystem::path& self) {
    const auto kit = self.parent_path();
    if (!std::filesystem::is_regular_file(kit / L"EAWR.exe") ||
        !std::filesystem::is_regular_file(kit / L"launcher/project.godot") ||
        !std::filesystem::is_regular_file(kit / L"eawr_mods_service.exe"))
        return fail(L"EAWR Mods is missing or incomplete. Extract the whole EAWR download and try again.");
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return fail(L"Windows could not initialize desktop shortcut creation.");
    PWSTR desktop = nullptr;
    HRESULT result = SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop);
    std::filesystem::path target;
    if (SUCCEEDED(result)) target = std::filesystem::path(desktop) / L"EAWR Mods.lnk";
    CoTaskMemFree(desktop);
    if (SUCCEEDED(result) && std::filesystem::exists(target) &&
        MessageBoxW(nullptr, L"An EAWR Mods desktop shortcut already exists. Replace it?", L"EAWR Mods",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        CoUninitialize();
        return 0;
    }
    IShellLinkW* link = nullptr;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(result)) result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (SUCCEEDED(result)) result = link->SetPath(self.c_str());
    if (SUCCEEDED(result)) result = link->SetWorkingDirectory(kit.c_str());
    if (SUCCEEDED(result)) result = link->SetDescription(L"Choose mods and play Empire at War Revised");
    if (SUCCEEDED(result)) result = link->SetIconLocation((kit / L"EAWR.exe").c_str(), 0);
    if (SUCCEEDED(result)) result = link->QueryInterface(IID_PPV_ARGS(&file));
    if (SUCCEEDED(result)) result = file->Save(target.c_str(), TRUE);
    if (file != nullptr) file->Release();
    if (link != nullptr) link->Release();
    CoUninitialize();
    return FAILED(result) ? fail(L"Windows could not save the EAWR Mods desktop shortcut.") : 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    using namespace eawr::launcher;
    try {
        int count = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
        if (argv == nullptr) return fail(L"Windows could not read the command line.");
        std::vector<std::wstring_view> arguments;
        for (int index = 1; index < count; ++index) arguments.emplace_back(argv[index]);
        const auto mode = mods_start_mode(arguments);
        LocalFree(argv);
        if (mode == ModsStartMode::invalid) return fail(L"Usage: EAWR Mods.exe [--create-shortcut]");
        const auto self = own_path();
        if (self.empty()) return fail(L"Windows could not locate EAWR Mods.exe.");
        if (mode == ModsStartMode::create_shortcut) return create_shortcut(self);
        const auto exe = mods_launcher_path(self);
        auto command = quote_command_line(exe.wstring(), {L"--mods"});
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
                            self.parent_path().c_str(), &startup, &process))
            return fail(L"EAWR.exe could not start. Extract the whole EAWR download and try again.");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 0;
    } catch (...) {
        return fail(L"EAWR Mods could not access the kit or desktop folder.");
    }
}
