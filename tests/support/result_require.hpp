#pragma once

#include "eawr/core/result.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace eawr::testing::result_code_colon {

// Keep the existing by-value require signatures and code/colon diagnostics here:
// result.hpp is frozen by another PR, and its top-level require is message-only.
// <string> supplies the string concatenation used by the diagnostic expressions.
template<class T> T require(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}

inline void require(core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
}

} // namespace eawr::testing::result_code_colon
