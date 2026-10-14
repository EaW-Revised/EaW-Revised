#pragma once

#include "eawr/campaign/setup_plan.hpp"

namespace eawr::script::foc::ai { struct GalacticSelectionContent; struct GalacticPerceptionSetup; }
namespace eawr::campaign {
// WGA-181..195: immutable original force authoring and planet normalization.
// Authored indigenous rows preserve original identities and affiliations;
// connectivity and contextual enemy disposition remain separate qualified owners.
[[nodiscard]] core::Result<std::shared_ptr<const script::foc::ai::GalacticPerceptionSetup>>
prepare_ai_perception_content(const vfs::Vfs&, const data::galactic::GalacticData&,
    const CampaignSetupPlan&, const script::foc::ai::GalacticSelectionContent&, std::string_view difficulty);
}
