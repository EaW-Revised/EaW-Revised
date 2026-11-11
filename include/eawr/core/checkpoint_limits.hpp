#pragma once

#include <cstddef>

namespace eawr::core {

// Shared save and transfer admission ceiling. Sized above four times the
// measured large campaign checkpoint; readers still validate each field.
inline constexpr std::size_t checkpoint_byte_limit = 1024ULL * 1024U * 1024U;

} // namespace eawr::core
