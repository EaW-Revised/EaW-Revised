#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace eawr::core { template<class T> class Result; }
namespace eawr::sim::math { class Fixed; }
namespace eawr::sim::galactic { struct GalacticSetup; }
namespace eawr::sim::tactical { struct TacticalContent; }
namespace eawr::units { struct UnitTables; struct GalacticTypeTable; }
namespace eawr::battle { struct MapPlacement; }
namespace eawr::skirmish { struct StartInputs; using battle::MapPlacement; }
namespace eawr::campaign { struct SpaceEntryInputs; class CampaignSetupPlan; class StockSpaceEntry; struct CampaignSetupOptions; }
namespace eawr::data { class Catalog; }
namespace eawr::data::galactic { struct GalacticData; }
namespace eawr::vfs { class Vfs; }

namespace space_entry_test {
using namespace eawr;
namespace gc = sim::galactic;
namespace tt = sim::tactical;
using sim::math::Fixed;
Fixed whole(std::int64_t n);
void check(core::Result<void> result);

units::UnitTables synthetic_tables();
skirmish::MapPlacement marker(const std::uint32_t ordinal, std::string name, const std::int64_t x);
skirmish::StartInputs synthetic_inputs(const units::UnitTables& tables);
campaign::SpaceEntryInputs entry_inputs(const units::UnitTables& tables);
units::UnitTables tables_for_entry();
void rider_bonus(tt::TacticalContent& content);
gc::GalacticSetup galaxy_for_entry();
void salvage_contracts();
void script_manifest_contracts();
void nonparticipant_team_contracts();
void reserve_order_contract();
void ground_passenger_contracts();
void admitted_cycle();
void preparation_contracts();
void scene_contracts();
void orbital_owner_admission();
void formation_contracts();
void team_origin_contract();
void formation_stage_contracts();
void mixed_class_contracts();
void garrison_contracts();
void station_ship_spawn_contracts();
void commander_contracts();
void ai_retreat_completion();
void gateway_route(const vfs::Vfs& files, const data::Catalog& catalog,
    const data::galactic::GalacticData& authored, const units::GalacticTypeTable& types);
void installed_reserve_order(const std::shared_ptr<const campaign::StockSpaceEntry>& provider,
    const campaign::CampaignSetupPlan& plan, const data::galactic::GalacticData& authored);
void installed_garrison(const std::shared_ptr<const campaign::StockSpaceEntry>& provider,
    const campaign::CampaignSetupPlan& plan, const data::galactic::GalacticData& authored,
    const vfs::Vfs& files, const data::Catalog& catalog);
void stock_scene_relation_graphs(const data::galactic::GalacticData& data, const vfs::Vfs& files, const data::Catalog& catalog);
void installed_commanders(const campaign::CampaignSetupPlan& plan, const units::GalacticTypeTable& types,
    const data::galactic::GalacticData& authored, const std::shared_ptr<const campaign::StockSpaceEntry>& provider);
void installed_layer_services(const vfs::Vfs& files, const data::Catalog& catalog,
    data::galactic::GalacticData authored, const units::GalacticTypeTable& types,
    campaign::CampaignSetupOptions options, bool multiplayer_difficulty = false);
void installed(bool garrison_only = false, bool population_only = false, bool commander_only = false, bool superweapon_only = false,
    bool layers_only = false, bool multiplayer_difficulty = false, bool orphan_only = false);
} // namespace space_entry_test
