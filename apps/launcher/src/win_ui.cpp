#include "win_platform.hpp"

#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace eawr::launcher {

namespace {

constexpr int kBrowseButton = 900;
constexpr int kFirstInstallButton = 1000;

using TaskDialogIndirectFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);

// comctl32 6 (task dialogs) comes from the manifest. Looked up at run time so a process without that
// manifest, or a Windows without the function, falls back to a plain message box instead of failing to load.
HRESULT task_dialog(const TASKDIALOGCONFIG& config, int& pressed) {
    const HMODULE module = LoadLibraryExW(L"comctl32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) return E_FAIL;
    const auto function = reinterpret_cast<TaskDialogIndirectFn>(reinterpret_cast<void*>(GetProcAddress(module, "TaskDialogIndirect")));
    if (function == nullptr) return E_NOTIMPL;
    return function(&config, &pressed, nullptr, nullptr);
}

class WindowsUi final : public Ui {
public:
    void show_error(const std::string& title, const std::string& message) override {
        MessageBoxW(nullptr, wide_from_utf8(message).c_str(), wide_from_utf8(title).c_str(),
                    MB_OK | MB_ICONERROR | MB_TASKMODAL | MB_SETFOREGROUND);
    }

    Pick pick_install(const std::vector<Install>& installs, const std::string& heading) override {
        // Command links: one per installation (folder as the title, its source as the note), then Browse.
        std::vector<std::wstring> labels;
        std::vector<TASKDIALOG_BUTTON> buttons;
        for (std::size_t index = 0; index < installs.size() && index < 16; ++index) {
            labels.push_back(installs[index].root.wstring() + L"\n" + wide_from_utf8(installs[index].source));
        }
        for (std::size_t index = 0; index < labels.size(); ++index) {
            buttons.push_back({static_cast<int>(kFirstInstallButton + index), labels[index].c_str()});
        }
        const std::wstring browse = L"Browse for the folder\nChoose the folder that contains GameData and corruption";
        buttons.push_back({kBrowseButton, browse.c_str()});

        const std::wstring instruction = installs.empty() ? L"Forces of Corruption was not found" : L"Choose your game folder";
        const std::wstring content = wide_from_utf8(heading);
        TASKDIALOGCONFIG config{};
        config.cbSize = sizeof config;
        config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        config.pszWindowTitle = L"Empire at War Revised";
        config.pszMainIcon = installs.empty() ? TD_WARNING_ICON : TD_INFORMATION_ICON;
        config.pszMainInstruction = instruction.c_str();
        config.pszContent = content.c_str();
        config.cButtons = static_cast<UINT>(buttons.size());
        config.pButtons = buttons.data();
        int pressed = 0;
        if (FAILED(task_dialog(config, pressed))) return fallback_pick(installs, heading);
        Pick pick;
        if (pressed == kBrowseButton) {
            pick.kind = Pick::Kind::browse;
        } else if (pressed >= kFirstInstallButton && pressed < kFirstInstallButton + static_cast<int>(labels.size())) {
            pick.kind = Pick::Kind::install;
            pick.index = static_cast<std::size_t>(pressed - kFirstInstallButton);
        }
        return pick;
    }

    std::optional<fs::path> browse_for_folder(const std::string& prompt) override {
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return std::nullopt;
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        const std::wstring title = wide_from_utf8(prompt);
        dialog->SetTitle(title.c_str());
        if (FAILED(dialog->Show(nullptr))) return std::nullopt;
        Microsoft::WRL::ComPtr<IShellItem> item;
        if (FAILED(dialog->GetResult(&item))) return std::nullopt;
        PWSTR text = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &text)) || text == nullptr) return std::nullopt;
        fs::path path(text);
        CoTaskMemFree(text);
        return path;
    }

private:
    // Comctl32 v6 missing (a broken manifest): still let the player use the first install or Browse.
    Pick fallback_pick(const std::vector<Install>& installs, const std::string& heading) {
        Pick pick;
        std::wstring text = wide_from_utf8(heading) + L"\n\n";
        if (!installs.empty()) text += L"Use " + installs.front().root.wstring() + L"?\n\nYes: use it.  No: browse for another folder.";
        else text += L"Browse for the folder now?";
        const int answer = MessageBoxW(nullptr, text.c_str(), L"Empire at War Revised",
                                       (installs.empty() ? MB_OKCANCEL : MB_YESNOCANCEL) | MB_ICONQUESTION | MB_TASKMODAL);
        if (answer == IDYES) pick.kind = Pick::Kind::install;
        else if (answer == IDNO || answer == IDOK) pick.kind = Pick::Kind::browse;
        return pick;
    }
};

class HeadlessUi final : public Ui {
public:
    void show_error(const std::string& title, const std::string& message) override {
        const HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
        if (error == nullptr || error == INVALID_HANDLE_VALUE) return;
        const std::string text = title + ": " + message + "\n";
        DWORD written = 0;
        WriteFile(error, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    }
    Pick pick_install(const std::vector<Install>&, const std::string&) override { return {}; }
    std::optional<fs::path> browse_for_folder(const std::string&) override { return std::nullopt; }
};

} // namespace

std::unique_ptr<Ui> make_windows_ui() {
    return std::make_unique<WindowsUi>();
}

std::unique_ptr<Ui> make_headless_ui() {
    return std::make_unique<HeadlessUi>();
}

} // namespace eawr::launcher
