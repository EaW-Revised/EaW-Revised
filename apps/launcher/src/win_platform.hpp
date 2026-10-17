#pragma once
// The Windows implementations behind launcher_flow's interfaces.
#include "launcher_flow.hpp"

#include <memory>

namespace eawr::launcher {

[[nodiscard]] std::string utf8_from_wide(const std::wstring& text);
[[nodiscard]] std::wstring wide_from_utf8(const std::string& text);

// HKCU / HKLM in the 32-bit or 64-bit view.
[[nodiscard]] std::unique_ptr<RegistryReader> make_windows_registry();
// CreateProcess with CREATE_NO_WINDOW: no console window for the child or anything it starts.
[[nodiscard]] std::unique_ptr<ProcessRunner> make_windows_process_runner();
// TaskDialog / MessageBox / the system folder picker. Call CoInitializeEx on this thread first.
[[nodiscard]] std::unique_ptr<Ui> make_windows_ui();
// No windows: errors go to stderr (when the launcher was given one), nothing is ever asked.
[[nodiscard]] std::unique_ptr<Ui> make_headless_ui();

} // namespace eawr::launcher
