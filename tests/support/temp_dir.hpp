#pragma once

#include <filesystem>
#include <system_error>

namespace eawr::testing {

inline void remove_temp_directory(const std::filesystem::path& root) noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

} // namespace eawr::testing
