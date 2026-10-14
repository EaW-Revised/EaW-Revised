#pragma once

#include "eawr/core/result.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/campaign_end_view.hpp"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace eawr::platform {
struct CampaignServiceConfiguration;
struct CampaignScoringSource;

// WGP-37/38/105, GHUD-S48: the authored half of the campaign-end inputs. The score
// list text and the four win/lose pictures come from the LOCAL player's faction; the
// movie names come from the WINNING faction (WGP-105). Absent tags stay empty: no
// guessed text, image or movie substitutes for authored data, and a mod's faction
// changes every value. The scoring-script half (title key, efficiencies) belongs to
// the scoring owner and stays absent here, which the dialog shows as WGP-97's `0%`.
// `victory_movies_enabled` is the movie owner's gate (GHUD-S49) and stays false.
[[nodiscard]] core::Result<CampaignEndInputs> campaign_end_inputs_from_catalog(
    const data::Catalog& catalog, std::string_view local_faction, std::string_view winner_faction);

// Synthetic stand-in for the scoring owner's provider (GHUD-S61), for explicit
// qualification fixtures only: it never describes campaign facts.
struct SyntheticCampaignScoring {
    std::string title_key;
    std::array<std::optional<double>, 3> efficiencies;
};

// Engine-owned configuration whose end provider is the production one
// (campaign_end_provider.hpp) plus, when given, the synthetic scoring stand-in.
// Presentation holds it only as an opaque option for the live session.
[[nodiscard]] std::shared_ptr<const CampaignServiceConfiguration> campaign_end_services(
    std::shared_ptr<const data::Catalog> catalog, std::optional<SyntheticCampaignScoring> scoring);

// GHUD-S61: the production end provider (authored half + the mounted scoring module over the
// session's saved ledger) as an opaque services option, for fixtures that need real numbers
// without a stand-in. Presentation never sees the configuration's members.
[[nodiscard]] std::shared_ptr<const CampaignServiceConfiguration> campaign_end_services_scored(
    std::shared_ptr<const data::Catalog> catalog, std::shared_ptr<const CampaignScoringSource> scoring);

} // namespace eawr::platform
