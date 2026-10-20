#pragma once

#include "eawr/core/diagnostic.hpp"
#include "eawr/core/result.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace eawr::testing {

template<class T> T take(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}

inline void take(core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}

template<class T> T must(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}

inline void must(core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}

inline void require(core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}

template<class T> T require(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}

namespace result_diagnostic {

template<class T> T take(core::Result<T> result) {
    if (!result) throw std::runtime_error(eawr::core::format_diagnostic(result.error()));
    return std::move(result).value();
}

inline void take(core::Result<void> result) {
    if (!result) throw std::runtime_error(eawr::core::format_diagnostic(result.error()));
}

template<class T> T must(core::Result<T> result) {
    if (!result) throw std::runtime_error(eawr::core::format_diagnostic(result.error()));
    return std::move(result).value();
}

inline void must(core::Result<void> result) {
    if (!result) throw std::runtime_error(eawr::core::format_diagnostic(result.error()));
}

} // namespace result_diagnostic

namespace result_code_colon {

template<class T> T take(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}

inline void take(core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
}

template<class T> T must(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}

inline void must(core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
}

} // namespace result_code_colon

namespace result_code_colon_value {

template<class T> T take(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}

inline void take(core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}

template<class T> T must(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}

inline void must(core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}

} // namespace result_code_colon_value

namespace result_code_space {

template<class T> T take(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + " " + result.error().message);
    return std::move(result).value();
}

inline void take(core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().code + " " + result.error().message);
}

template<class T> T must(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + " " + result.error().message);
    return std::move(result).value();
}

inline void must(core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().code + " " + result.error().message);
}

} // namespace result_code_space

namespace result_code_space_value {

template<class T> T take(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + " " + result.error().message);
    return std::move(result).value();
}

inline void take(core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}

template<class T> T must(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + " " + result.error().message);
    return std::move(result).value();
}

inline void must(core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}

} // namespace result_code_space_value

} // namespace eawr::testing
