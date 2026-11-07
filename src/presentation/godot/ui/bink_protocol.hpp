#pragma once
#include <cstdint>

namespace eawr::presentation::godot_backend {
// Private, same-build Windows x64 parent/child protocol; synchronization uses
// inherited auto-reset events. One request is outstanding at a time.
struct BinkExchange {
    std::uint32_t version{1};
    std::uint32_t command{}; // 0 open, 1 next, 2 exit
    std::uint32_t status{};  // 0 pending, 1 success, 2 failure
    std::uint32_t source_size{};
    std::uint32_t output_size{};
    std::uint32_t width{}, height{}, frames{}, rate_numerator{}, rate_denominator{};
    std::uint32_t selected_frame{}, trace_first[3]{}, trace_last{};
};
inline constexpr std::uint64_t bink_memory_budget = 256ULL * 1024ULL * 1024ULL;
} // namespace eawr::presentation::godot_backend
