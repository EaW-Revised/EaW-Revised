#pragma once

#include "eawr/skirmish/start.hpp"
#include "eawr/campaign/space_briefing.hpp"

#include <optional>

namespace eawr::platform {
// ADR-019: immutable prepared values for a borrowed campaign child. The
// provider retains the exact campaign policies and persistent-ID start;
// presentation must not reconstruct these from lobby defaults.
struct PreparedCampaignBattle {
    sim::tactical::TacticalSetup setup;
    skirmish::SessionContent content;
    sim::tactical::VictoryRules victory;
    sim::tactical::EconomyRules economy;
    units::UnitTables tables;
    std::optional<skirmish::SkirmishStart> start;
    struct Briefing {
        sim::tactical::PlayerId player{};
        campaign::SpaceBriefingContent content;
        std::vector<std::uint64_t> available_persistent_types;
        campaign::SpaceIntroGate intro;
        bool enabled{true};
    };
    std::vector<Briefing> briefings;
};
} // namespace eawr::platform
