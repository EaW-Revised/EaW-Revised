#include "galactic_ai_goals.hpp"

#include <limits>

namespace eawr::script::foc::ai::galactic_goals {

bool proposable(const ProposalCounts& state, std::string_view goal_type) {
    if (state.blockers != 0) return false;
    const auto ordinary = state.ordinary.find(goal_type);
    if (ordinary == state.ordinary.end() || ordinary->second == 0) return false;
    const auto exclusion = state.exclusions.find(goal_type);
    return exclusion == state.exclusions.end() || exclusion->second == 0;
}

core::Result<ProposalCounts> activate_exclusions(const ProposalCounts& state, std::span<const std::string> types) {
    auto out = state;
    for (const auto& type : types) {
        auto& count = out.exclusions[type];
        if (count == std::numeric_limits<std::uint64_t>::max()) {
            return core::Result<ProposalCounts>::failure({"galactic-ai.exclusion-overflow",
                core::Severity::error, "Goal exclusion reference count exhausted", type, {}, {}, {}});
        }
        ++count;
    }
    return core::Result<ProposalCounts>::success(std::move(out));
}

ProposalCounts release_exclusions(const ProposalCounts& state, std::span<const std::string> types) {
    auto out = state;
    for (const auto& type : types) {
        const auto found = out.exclusions.find(type);
        if (found != out.exclusions.end() && found->second > 0) --found->second;
    }
    return out;
}

} // namespace eawr::script::foc::ai::galactic_goals
