#pragma once

#include "eawr/platform/campaign_battle_view.hpp"

namespace eawr::campaign {

// Optional standalone-file metadata; does not alter native actor placement or state.
[[nodiscard]] sim::tactical::ReplayNativeScene native_scene_selection(
    const units::LoadInput& source, const units::UnitTables& battle);
[[nodiscard]] core::Result<units::UnitTables> native_scene_tables(
    const units::UnitTables& source, const sim::tactical::ReplayNativeScene&,
    const std::vector<skirmish::MapPlacement>&);
[[nodiscard]] core::Result<platform::PreparedCampaignBattle> load_standalone_space_replay(
    const sim::tactical::TacticalReplay&, const vfs::Vfs&, const data::Catalog&);

} // namespace eawr::campaign
