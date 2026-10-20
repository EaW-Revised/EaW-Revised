#pragma once

#include <cstdint>

namespace eawr::script::foc::ai {
// Cumulative diagnostics for this thread only. Never saved, hashed or used by
// selection. Callers may subtract two reads to check preparation work bounds.
struct GalacticSelectionWork {
    std::uint64_t content_indices_built{}, content_index_rows{}, trait_index_rows{};
    std::uint64_t garrison_name_lookups{}, garrison_trait_lookups{}, proposal_trait_lookups{};
};
[[nodiscard]] GalacticSelectionWork galactic_selection_work() noexcept;
}
