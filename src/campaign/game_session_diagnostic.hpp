#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/galactic/types.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace eawr::campaign::game_session_detail {
namespace galactic = sim::galactic;
template <typename T>
core::Result<T> fail(std::string message, std::string_view code = galactic::diagnostic_codes::invalid_command) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.message = std::move(message);
    return core::Result<T>::failure(std::move(diagnostic));
}
} // namespace eawr::campaign::game_session_detail
