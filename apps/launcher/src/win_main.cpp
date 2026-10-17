// EAWR.exe: the quiet first-run launcher. A GUI-subsystem program (no console window, ever):
// finds the player's Forces of Corruption install, starts the bundled Godot
// runtime with no console window and reports problems in readable dialogs. See launcher_flow.cpp.
#include "launcher_flow.hpp"
#include "launcher_state.hpp"
#include "win_platform.hpp"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

namespace {

using namespace eawr::launcher;

std::optional<fs::path> environment_path(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size <= 1) return std::nullopt;
    std::wstring value(size, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), size);
    if (length == 0 || length >= size) return std::nullopt;
    value.resize(length);
    return fs::path(value);
}

fs::path module_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            return fs::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::vector<std::wstring> command_line_arguments() {
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::wstring> args;
    if (argv != nullptr) {
        for (int index = 1; index < count; ++index) args.emplace_back(argv[index]);
        LocalFree(argv);
    }
    return args;
}

const char* const kUsage =
    "EAWR.exe [options]\n\n"
    "  --game-root <folder>    use this Forces of Corruption folder (the one holding GameData and corruption)\n"
    "  --choose-game           ask which installation to use even if one is saved (also: hold Shift while starting)\n"
    "  --godot <exe>           use this Godot 4.7.2 runtime instead of the bundled one\n"
    "  --project <folder>      use this game project folder instead of the bundled one\n"
    "  --user-data <folder>    keep the launcher's files here instead of %APPDATA%\\Godot\\app_userdata\\EAWR Viewer\n"
    "  --allow-unknown-build   let the game start on a Forces of Corruption build EAWR has not verified\n"
    "  --no-dialogs            never open a window; errors go to stderr and the log\n"
    "  --godot-quit-after <n>  close the game after n frames (smoke tests)\n"
    "  -- <args>               passed on to the game\n";

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    Options options = parse_command_line(command_line_arguments());
    options.launcher_dir = module_directory();
    if (options.user_data.empty()) options.user_data = default_user_data_dir();
    if (!options.game_root) options.game_root = environment_path(L"EAWR_EAW_GAME_ROOT");
    if (!options.godot) options.godot = environment_path(L"EAWR_GODOT_EXECUTABLE");
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) options.choose_game = true;

    int code = kExitOk;
    if (options.help) {
        MessageBoxW(nullptr, wide_from_utf8(kUsage).c_str(), L"Empire at War Revised", MB_OK | MB_ICONINFORMATION | MB_TASKMODAL);
    } else {
        Log log(options.user_data.empty() ? fs::path() : options.user_data / "launcher" / "launcher.log");
        const std::unique_ptr<RegistryReader> registry = make_windows_registry();
        const std::unique_ptr<ProcessRunner> runner = make_windows_process_runner();
        const std::unique_ptr<Ui> ui = options.no_dialogs ? make_headless_ui() : make_windows_ui();
        code = run_launcher(options, *registry, *ui, *runner, log);
    }

    if (SUCCEEDED(com)) CoUninitialize();
    return code;
}
