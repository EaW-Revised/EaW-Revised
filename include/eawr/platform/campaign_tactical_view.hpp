#pragma once

#include "eawr/sim/tactical/snapshot.hpp"
#include "eawr/campaign/conflict.hpp"
#include <map>
#include <optional>

namespace eawr::platform {

// WAR-10/65/75: retained policy facts are separate from solver availability.
struct CampaignRetreatPolicy {
    sim::EntityId conflict{};
    bool multiplayer{}, planet_excluded{};
    std::optional<bool> story_prevented{false};
    friend bool operator==(const CampaignRetreatPolicy&, const CampaignRetreatPolicy&) = default;
};

// WAR-10: a flattened force list proves a positive, but cannot prove that an
// omitted outer fleet/container lacks its own flag. Preserve that unknown.
[[nodiscard]] inline std::optional<bool> campaign_story_prevention(bool story,
    const campaign::ConflictRecord& conflict, const std::map<sim::galactic::TypeId, bool>& flags) {
    if (!story) return false;
    for (const auto& force : conflict.forces) {
        const auto found = flags.find(force.type);
        if (found != flags.end() && found->second) return true;
    }
    return std::nullopt;
}

// A borrowed observer view owns no lists and publishes no opponent reserve rows.
struct CampaignTacticalView {
    sim::tactical::PlayerId player{};
    const sim::tactical::EconomyView* economy{};
    const sim::tactical::RetreatView* retreat{};
    std::optional<CampaignRetreatPolicy> policy;
    [[nodiscard]] bool solver_dialog_allowed() const noexcept {
        return retreat && retreat->player == player && retreat->refusal == sim::tactical::RetreatRefusal::none
            && retreat->state.phase == sim::tactical::RetreatPhase::idle
            && retreat->auto_resolve_allowed && policy && !policy->multiplayer
            && !policy->planet_excluded && policy->story_prevented.has_value() && !*policy->story_prevented;
    }
};

} // namespace eawr::platform
