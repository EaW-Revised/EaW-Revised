#include "eawr/platform/executable.hpp"

#include "eawr/sim/replay.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace eawr::platform {
namespace {

[[nodiscard]] std::optional<std::filesystem::path> current_executable_path() {
#if defined(_WIN32)
    std::wstring buffer(260, L'\0');
    while (buffer.size() <= 32768) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::nullopt;
#elif defined(__linux__)
    std::error_code error;
    auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        return std::nullopt;
    }
    return path;
#else
    return std::nullopt;
#endif
}

} // namespace

std::optional<std::string> current_executable_sha256() {
    const auto path = current_executable_path();
    if (!path) {
        return std::nullopt;
    }
    std::ifstream input(*path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    const std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad() || bytes.empty()) {
        return std::nullopt;
    }
    return sim::sha256_hex(bytes);
}

} // namespace eawr::platform
