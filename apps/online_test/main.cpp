#include "options.hpp"
#include "run.hpp"
#include <filesystem>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
std::filesystem::path executable_directory(const char* argv0) {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH * 4]{};
    if (const auto length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer))); length > 0 && length < std::size(buffer))
        return std::filesystem::path(buffer).parent_path();
#endif
    std::error_code error;
    const auto absolute = std::filesystem::absolute(argv0, error);
    return error ? std::filesystem::current_path() : absolute.parent_path();
}
}

int main(int argc, char** argv) {
    std::vector<std::string> arguments(argv + 1, argv + argc);
    auto parsed = eawr::online_test::parse_arguments(arguments, executable_directory(argc > 0 ? argv[0] : ""));
    if (!parsed) {
        std::cerr << "RESULT: FAIL - " << parsed.error << '\n' << eawr::online_test::usage();
        return eawr::online_test::exit_usage;
    }
    return eawr::online_test::run(*parsed.value, std::cin, std::cout);
}
