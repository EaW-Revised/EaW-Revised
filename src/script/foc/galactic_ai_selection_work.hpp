#pragma once

#include <cstdint>

namespace eawr::script::foc::ai {
// Cumulative diagnostics for this thread only. Never saved, hashed or used by
// selection. Callers may subtract two reads to check preparation work bounds.
struct GalacticSelectionWork {
    std::uint64_t content_indices_built{}, content_index_rows{}, trait_index_rows{};
    std::uint64_t garrison_name_lookups{}, garrison_trait_lookups{}, proposal_trait_lookups{};
    // Allowed-producer flags of a completed input: producer reads made, and reads answered from the kept flag.
    std::uint64_t allowed_site_reads{}, allowed_site_reuses{}, allowed_site_unowned{};
};
[[nodiscard]] GalacticSelectionWork galactic_selection_work() noexcept;
}
