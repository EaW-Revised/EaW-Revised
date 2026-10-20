#pragma once

#include "eawr/net/lockstep_session.hpp"

#include <utility>

// Shared wire helpers for the session implementation only.
namespace eawr::net::lockstep_detail {
using Bytes = std::vector<std::uint8_t>;
inline constexpr std::array<std::string_view, 8> resync_codes{resync_errors::options, resync_errors::malformed,
    resync_errors::agreement, resync_errors::replay, resync_errors::hash, resync_errors::timeout,
    resync_errors::overlap, resync_errors::unavailable};
inline core::Diagnostic diagnostic(std::string_view code, std::string message) {
    core::Diagnostic result;
    result.code = code;
    result.message = std::move(message);
    return result;
}
template<class T> core::Result<T> refused(std::string_view code, std::string message) {
    return core::Result<T>::failure(diagnostic(code, std::move(message)));
}
inline void append(Bytes& bytes, std::uint64_t value, unsigned width) {
    for (unsigned index = 0; index < width; ++index)
        bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
}
inline bool read(std::span<const std::uint8_t>& bytes, std::uint64_t& value, unsigned width) {
    if (bytes.size() < width) return false;
    value = 0;
    for (unsigned index = 0; index < width; ++index)
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    bytes = bytes.subspan(width);
    return true;
}

} // namespace eawr::net::lockstep_detail
