#pragma once

#include "eawr/core/result.hpp"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>

namespace eawr::script::foc::ai::galactic_goals {

// WGA-57: these counts belong to a mode manager and are independent of
// target-local goal equivalence. Zero exclusion rows survive release/save.
struct ProposalCounts {
    std::map<std::string, std::uint64_t, std::less<>> ordinary;
    std::map<std::string, std::uint64_t, std::less<>> exclusions;
    std::uint64_t blockers{};
};
[[nodiscard]] bool proposable(const ProposalCounts&, std::string_view goal_type);
// Apply once on activation/release. Duplicate authored exclusions each count.
// Immutable return prevents an overflow from leaving partially changed state.
[[nodiscard]] core::Result<ProposalCounts> activate_exclusions(const ProposalCounts&, std::span<const std::string>);
[[nodiscard]] ProposalCounts release_exclusions(const ProposalCounts&, std::span<const std::string>);

} // namespace eawr::script::foc::ai::galactic_goals
