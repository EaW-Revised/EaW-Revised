#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/galactic/synthetic.hpp"
#include "eawr/sim/galactic/types.hpp"
#include "eawr/sim/math/fixed.hpp"

#include <optional>
#include <string>

// Synthetic campaigns: written for the project, no game data. They drive the galactic tests, the
// replay corpus and sim_headless --galactic until the campaign session setup builds a real start
// from the installed game's campaign definitions.
namespace eawr::campaign {

struct SyntheticCampaign {
    std::string name;
    sim::galactic::SyntheticGalaxyOptions galaxy;
    // Fiscal_Cycle_Time_In_Secs as the content would author it; none for no fiscal window.
    std::optional<sim::math::Fixed> fiscal_cycle_seconds;
    friend bool operator==(const SyntheticCampaign&, const SyntheticCampaign&) = default;
};

// The campaign's tick-zero galaxy. Its campaign identity is the SHA-256 of the definition's
// canonical bytes, so two definitions that differ in anything differ in identity. Refuses an empty
// name, counts outside the synthetic fixture bounds or the galaxy object limit, or a fiscal
// duration fiscal_cycle_frames() refuses.
[[nodiscard]] core::Result<sim::galactic::GalacticSetup> build_synthetic_campaign(const SyntheticCampaign& campaign);

} // namespace eawr::campaign
