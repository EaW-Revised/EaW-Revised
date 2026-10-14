#pragma once

#include "eawr/campaign/conflict_entry.hpp"
#include "eawr/script/foc/tactical_ai.hpp"

namespace eawr::campaign {

// WGT-27, WGA-196..201: campaign space uses ordinary player accounts.
struct SpaceRulePlayer {
    sim::tactical::PlayerId player{};
    sim::math::Fixed credits{};
    std::uint32_t tech{}, max_tech{}, owned_planets{};
    sim::math::Fixed reinforcement_yaw{};
    bool ai{};
    struct Budget { sim::math::Fixed available{}, allowance{}; };
    std::vector<Budget> budgets; // qualification inputs; WGA-201 does not apply them to space
    bool budgets_known{}; // known-empty stock categories differ from an absent producer
    std::optional<sim::math::Fixed> strategic_credit_cap{}; // WGP-188: nonparticipant modifier owner
};
struct SpaceRuleInputs {
    std::vector<SpaceRulePlayer> players; // ascending player ID
    std::vector<sim::EntityId> restricted_reserves; // ascending identity; WR-09
    std::vector<sim::tactical::CampaignUpgrade> upgrades; // qualified holders, WGT-14..16
    std::vector<sim::EntityId> leaders; // only when GC installs kill-leader conditions, WGT-29
    sim::math::Fixed corruption_price_multiplier{sim::math::Fixed::from_raw(sim::math::Fixed::scale)};
    struct ReserveAttachment {
        sim::EntityId id{};
        std::vector<sim::tactical::CarriedObject> contained;
    };
    std::vector<ReserveAttachment> reserve_attachments; // qualified A identity trees, ascending root ID
    std::vector<sim::EntityId> reserve_order{}; // WGT-07: optional complete pool order, empty uses ID order
};
// Decorates A's prepared scene. No IO, no galaxy mutation, no extra tactical owner.
[[nodiscard]] core::Result<void> apply_space_rules(ConflictEntry& entry,
    const skirmish::StartInputs& content, const SpaceRuleInputs& inputs);
// Rebind exactly the recorded campaign policy for child replay and checkpoint load.
[[nodiscard]] core::Result<void> bind_space_rules(const sim::tactical::TacticalSetup& setup,
    const skirmish::StartInputs& inputs, sim::tactical::TacticalContent& content);

struct SpaceRuleReturn {
    std::vector<ReturnedCredits> credits; // final GC balances under the ordinary space account contract
    std::vector<sim::tactical::CampaignUpgrade> upgrades; // retained on the conflict planet
};
[[nodiscard]] core::Result<SpaceRuleReturn> space_rule_return(const sim::tactical::TacticalSetup& setup,
    const units::UnitTables& tables, std::span<const sim::tactical::EconomyView> accounts);
// WGT-33/WGA-52: scene/provider supplies the ordinary template/XML; this binds GC context.
void configure_space_ai(script::foc::AiSetup& ai, const ConflictRecord& conflict);

} // namespace eawr::campaign
