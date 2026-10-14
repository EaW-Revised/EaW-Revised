#pragma once

#include <cstdint>

namespace eawr::sim::tactical {
enum class RetreatAction : std::uint32_t { start = 1, cancel = 2, auto_resolve = 3 };
} // namespace eawr::sim::tactical
