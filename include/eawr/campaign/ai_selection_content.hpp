#pragma once

#include "eawr/campaign/setup_plan.hpp"

namespace eawr::script::foc::ai { struct GalacticSelectionContent; }
namespace eawr::script::foc::galactic_ai { struct FreestoreFacts; struct FreestoreObject; }
namespace eawr::sim { class PartitionExecutor; }
namespace eawr::sim::galactic { class GalacticSnapshot; }
namespace eawr::campaign {
using FreestoreMemberQualifier = std::function<core::Result<bool>(sim::galactic::TypeId)>;
using FreestoreCurrentTypeQualifier = std::function<core::Result<sim::galactic::TypeId>(
    const script::foc::galactic_ai::FreestoreObject&)>;
// WGA-363/364: completed loaded flags, not inferred roster ancestry.
[[nodiscard]] FreestoreMemberQualifier freestore_member_qualifier(
    std::shared_ptr<const script::foc::ai::GalacticSelectionContent>);
// WGA-246..250: campaign-layer inversion. Original identities, inherited
// authoring and fixed numeric decoding precede immutable AI selection facts.
[[nodiscard]] core::Result<std::shared_ptr<const script::foc::ai::GalacticSelectionContent>>
prepare_ai_selection_content(const vfs::Vfs&, const data::Catalog&,
    const data::galactic::GalacticData&, const units::GalacticTypeTable&, const CampaignSetupPlan&);
// WGA-301/303/312/316: genuine fresh ordinary galaxy construction only.
// Loaded actors and inherited generated teams use their retained actor facts.
// WGA-363/364: member callback needs completed parse-pass flags (#2725).
[[nodiscard]] core::Result<std::vector<script::foc::galactic_ai::FreestoreFacts>>
prepare_fresh_freestore_facts(const sim::galactic::GalacticSnapshot&,
    const script::foc::ai::GalacticSelectionContent&, const units::GalacticTypeTable&,
    const sim::PartitionExecutor&, FreestoreMemberQualifier, FreestoreCurrentTypeQualifier = {});
}
