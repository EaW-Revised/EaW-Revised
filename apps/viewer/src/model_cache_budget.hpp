#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace eawr::viewer {

// Strong model-cache budget while a tactical battle loads (docs/performance/memory-attribution.md). The scene library
// only receives the byte count; the physical-RAM read stays in the viewer layer.
inline constexpr std::uint64_t model_cache_load_budget_min_bytes = 256ULL << 20U;
inline constexpr std::uint64_t model_cache_load_budget_max_bytes = 1ULL << 30U;
inline constexpr std::uint64_t model_cache_load_budget_ram_divisor = 32;

// clamp(total physical RAM / 32, 256 MiB, 1 GiB). An unknown size (0) takes the lower bound.
[[nodiscard]] constexpr std::uint64_t model_cache_load_budget(const std::uint64_t total_physical_bytes) noexcept {
    return std::clamp(total_physical_bytes / model_cache_load_budget_ram_divisor,
                      model_cache_load_budget_min_bytes, model_cache_load_budget_max_bytes);
}

// --eawr-model-cache-load-budget <MiB|off>: `off` restores the unbounded load of earlier builds (same-binary A/B
// control). Returns the byte count to use, or nullopt when the text is neither.
[[nodiscard]] constexpr std::optional<std::uint64_t> parse_model_cache_load_budget(const std::string_view text) noexcept {
    if (text == "off") return std::numeric_limits<std::uint64_t>::max();
    if (text.empty() || text.size() > 9) return std::nullopt; // 9 digits of MiB cannot overflow 64 bits
    std::uint64_t mib = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') return std::nullopt;
        mib = mib * 10 + static_cast<std::uint64_t>(digit - '0');
    }
    return mib << 20U;
}

} // namespace eawr::viewer
