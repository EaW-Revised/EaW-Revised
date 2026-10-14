#include "eawr/campaign/synthetic.hpp"

#include "eawr/core/sha256.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::campaign {

namespace galactic = sim::galactic;

namespace {

void put_u32(std::vector<std::uint8_t>& bytes, const std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) bytes.push_back(static_cast<std::uint8_t>((value >> (8U * index)) & 0xffU));
}

void put_u64(std::vector<std::uint8_t>& bytes, const std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) bytes.push_back(static_cast<std::uint8_t>((value >> (8U * index)) & 0xffU));
}

[[nodiscard]] core::Result<galactic::GalacticSetup> refuse(std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(galactic::diagnostic_codes::invalid_setup);
    diagnostic.message = std::move(message);
    return core::Result<galactic::GalacticSetup>::failure(std::move(diagnostic));
}

} // namespace

core::Result<galactic::GalacticSetup> build_synthetic_campaign(const SyntheticCampaign& campaign) {
    if (campaign.name.empty()) return refuse("a synthetic campaign needs a name");
    const auto& options = campaign.galaxy;
    if (options.planets < 2 || options.planets > 10'000 || options.players < 1
        || options.players > galactic::galactic_max_players || options.fleets_per_player > 1'000
        || options.members_per_fleet < 1 || options.members_per_fleet > 1'000) {
        return refuse("planets 2..10000, players 1..64, fleets per player 0..1000, members per fleet 1..1000");
    }
    const std::uint64_t routes = options.planets == 2 ? 1 : options.planets
        + (options.planets > 4 ? (options.planets + 3) / 4 : 0);
    const auto fleets = std::uint64_t{options.players} * options.fleets_per_player;
    if (options.planets + routes + fleets * (1 + options.members_per_fleet) > galactic::galactic_max_objects) {
        return refuse("the synthetic campaign exceeds the galaxy object limit");
    }
    auto setup = galactic::synthetic_galaxy(campaign.galaxy);
    if (campaign.fiscal_cycle_seconds) {
        const auto frames = galactic::fiscal_cycle_frames(*campaign.fiscal_cycle_seconds);
        if (!frames) return refuse("campaign " + campaign.name + ": " + frames.error().message);
        setup.rules.fiscal_cycle_frames = frames.value();
    }
    // The campaign identity: SHA-256 over a versioned canonical encoding of the definition.
    constexpr std::string_view tag = "eawr-synthetic-campaign-v1";
    std::vector<std::uint8_t> definition(tag.begin(), tag.end());
    put_u32(definition, static_cast<std::uint32_t>(campaign.name.size()));
    definition.insert(definition.end(), campaign.name.begin(), campaign.name.end());
    put_u64(definition, options.seed);
    put_u32(definition, options.planets);
    put_u32(definition, options.players);
    put_u32(definition, options.fleets_per_player);
    put_u32(definition, options.members_per_fleet);
    definition.push_back(options.debug_commands ? 1U : 0U);
    definition.push_back(options.coop_seat ? 1U : 0U);
    put_u64(definition, campaign.fiscal_cycle_seconds ? static_cast<std::uint64_t>(campaign.fiscal_cycle_seconds->raw()) : 0U);
    definition.push_back(campaign.fiscal_cycle_seconds ? 1U : 0U);
    setup.campaign_identity = core::sha256(definition);
    return core::Result<galactic::GalacticSetup>::success(std::move(setup));
}

} // namespace eawr::campaign
