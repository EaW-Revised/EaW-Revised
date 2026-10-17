#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace eawr::script::foc {
// Observer output only. This retention policy must never be used for native
// learning, replay effects, saved state or anything read by an AI decision.
inline constexpr std::size_t ai_diagnostic_capacity = 65536;

template<class T>
void append_ai_diagnostic(std::vector<T>& rows, std::uint64_t& dropped, T row) {
    if (rows.size() >= ai_diagnostic_capacity) {
        // Keep a chronological suffix and leave room for another half-buffer.
        // Each retained row moves at most once per half-buffer of appends;
        // erasing releases discarded strings without reallocating the vector.
        const auto count = rows.size() - ai_diagnostic_capacity / 2;
        rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(count));
        dropped += static_cast<std::uint64_t>(count);
    }
    rows.push_back(std::move(row));
}
} // namespace eawr::script::foc
