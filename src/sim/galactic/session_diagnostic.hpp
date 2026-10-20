#pragma once

#include "encoding.hpp"
#include "eawr/core/result.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace eawr::sim::galactic::session_detail {
template <typename T>
[[nodiscard]] core::Result<T> fail(const std::string_view code, std::string message) {
    return core::Result<T>::failure(detail::diagnostic(code, std::move(message)));
}

} // namespace eawr::sim::galactic::session_detail
