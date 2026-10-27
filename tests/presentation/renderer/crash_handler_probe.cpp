#include "crash_handler.hpp"
#include "crash_log_path.hpp"
#include <windows.h>

int wmain(int argc, wchar_t** argv) {
    if (argc != 1 && argc != 3) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    using namespace eawr::presentation::godot_backend;
    constexpr std::string_view fallback = "user://logs/godot.log";
    if (select_crash_log_path(fallback, "--eawr-test-crash") != fallback
        || select_crash_log_path(fallback, "--eawr-log-file=") != fallback
        || select_crash_log_path(fallback, "--eawr-log-file=C:/Player Logs/viewer.log") != "C:/Player Logs/viewer.log") return 5;
    if (argc == 1) {
        bool argument_read = false;
        const auto selected = resolve_crash_log_path(fallback, 0, [&](std::size_t) {
            argument_read = true;
            return std::string("--eawr-log-file=unexpected");
        });
        if (selected != fallback || argument_read) return 5;
        // The engine supplies its globalized default; no custom user argument
        // is present in this subprocess. The fixture supplies that native path.
        static wchar_t default_log[32768]{};
        const DWORD length = GetEnvironmentVariableW(L"EAWR_CRASH_PROBE_DEFAULT_LOG", default_log, 32768);
        if (!length || length >= 32768) return 2;
        if (!install_crash_handler(default_log, "0.1.0+crash-probe")) return 3;
        deliberate_test_crash();
        return 6;
    }
    if (argv[2][0] == L'l') {
        static wchar_t oversized[32768];
        for (auto& value : oversized) value = L'x';
        return install_crash_handler(oversized, "probe") ? 4 : 0;
    }
    static char maximum_build[256]{};
    for (unsigned index = 0; index < sizeof(maximum_build) - 1; ++index) maximum_build[index] = 'b';
    const bool maximum = argv[2][0] == L'm';
    if (!install_crash_handler(argv[1], maximum ? maximum_build : "0.1.0+crash-probe")) return 3;
    if (argv[2][0] == L'c' || maximum) deliberate_test_crash();
    uninstall_crash_handler();
    return 0;
}
