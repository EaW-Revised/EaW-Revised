#pragma once

#include "eawr/campaign/space_rule_inputs.hpp" // include-guard: keep
#include "eawr/campaign/space_replay_rules.hpp"
#include "eawr/script/foc/tactical_ai.hpp"

namespace eawr::campaign {

struct SpaceRuleReturn {
    std::vector<ReturnedCredits> credits; // final GC balances under the ordinary space account contract
    std::vector<sim::tactical::CampaignUpgrade> upgrades; // retained on the conflict planet
};
[[nodiscard]] core::Result<SpaceRuleReturn> space_rule_return(const sim::tactical::TacticalSetup& setup,
    const units::UnitTables& tables, std::span<const sim::tactical::EconomyView> accounts);
// WGT-33/WGA-52: scene/provider supplies the ordinary template/XML; this binds GC context.
void configure_space_ai(script::foc::AiSetup& ai, const ConflictRecord& conflict);

} // namespace eawr::campaign
