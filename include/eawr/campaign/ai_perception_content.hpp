#pragma once

#include "eawr/campaign/setup_plan.hpp"
#include "eawr/units/galactic_types.hpp"

#include <array>
#include <cstdint>
#include <functional>

namespace eawr::script::foc::ai { struct GalacticSelectionContent; struct GalacticPerceptionSetup; }
namespace eawr::campaign {
// One authored starting-spawn entry with its type resolved; `type` 0 is a null reference.
struct StartingSpawnEntry { std::uint64_t type{}; std::int32_t count{}; };
// RGC-21..23: the six authored tech rows with every name resolved. An undefined name stays a null reference (0) and is
// listed in `diagnostics` (the explicit name None is null without a line); no row is filtered, so a row holding only
// null references is still nonempty and keeps tier selection from falling back to a lower row.
[[nodiscard]] std::array<std::vector<StartingSpawnEntry>, 6> resolve_starting_spawns(const units::GalacticType& owner,
    const std::function<std::uint64_t(std::string_view)>& identity, std::vector<std::string>& diagnostics);
// WGA-181..195: immutable original force authoring and planet normalization.
// Authored indigenous rows preserve original identities and affiliations;
// connectivity and contextual enemy disposition remain separate qualified owners.
[[nodiscard]] core::Result<std::shared_ptr<const script::foc::ai::GalacticPerceptionSetup>>
prepare_ai_perception_content(const vfs::Vfs&, const data::galactic::GalacticData&,
    const CampaignSetupPlan&, const script::foc::ai::GalacticSelectionContent&, std::string_view difficulty);
}
