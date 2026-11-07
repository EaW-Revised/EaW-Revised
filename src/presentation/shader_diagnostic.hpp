#pragma once

#include "eawr/assets/assets.hpp"

#include <limits>
#include <string_view>

namespace eawr::presentation::shader_detail {
// Numeric state/evaluation diagnostics share bounded, metadata-only provenance.
inline core::Diagnostic diagnostic(const assets::Source& source, std::string_view code,
    std::string_view message, std::size_t byte_offset) {
    core::Diagnostic result;
    result.code = code; result.message = message;
    if (!source.logical_path.empty()) result.logical_path = source.logical_path.substr(0U, 512U);
    if (!source.source_id.empty()) result.source_id = source.source_id.substr(0U, 512U);
    if (byte_offset < std::numeric_limits<std::uint64_t>::max()) result.column = static_cast<std::uint64_t>(byte_offset) + 1U;
    return result;
}
} // namespace eawr::presentation::shader_detail
