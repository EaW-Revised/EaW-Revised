#pragma once

namespace eawr::presentation::godot_backend {
// Called during healthy startup. Paths are native UTF-16 on Windows; all state
// needed after an exception is reserved before installing the process filter.
bool install_crash_handler(const wchar_t* log_path, const char* build_id) noexcept;
void uninstall_crash_handler() noexcept;
// The command line must explicitly request --eawr-test-crash before calling.
void deliberate_test_crash() noexcept;
}
