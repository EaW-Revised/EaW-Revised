#pragma once

#include "env.hpp"

#include <filesystem>

namespace eawr::testing {

// Relocated test packages provide the source and build roots at execution time.
// Ordinary local builds retain their configured paths.
inline std::filesystem::path runtime_path(const char* variable, const char* relative,
                                          const char* configured) {
    if (const auto root = environment(variable)) return std::filesystem::path(*root) / relative;
    return configured;
}

} // namespace eawr::testing
