#pragma once

#include "eawr/skirmish/setup.hpp"
#include "eawr/skirmish/land_terrain.hpp"
#include "eawr/sim/galactic/income.hpp"

namespace eawr::skirmish {

struct LandFactionStart {
    std::string faction;
    std::vector<std::string> forces;
    bool forces_authored{true}; // false means the tag was absent, distinct from an authored empty list.
    std::vector<sim::galactic::IncomeCurvePoint> company_cap;
};
struct LandStartInputs {
    StartInputs common;
    assets::Map map;
    std::vector<LandFactionStart> factions;
    units::UnitTables static_tables; // scene types and every authored pre-built marker candidate
};

struct LandStaticBinding {
    sim::tactical::TypeId type{};
    sim::math::Fixed width, height, x_offset, y_offset;
    std::optional<sim::math::Fixed> z_extent;
    std::uint32_t primary_passability{}, secondary_passability{}, water_bit{};
    bool fine_tune{true}, model_present{};
    sim::math::Fixed scale_factor{sim::math::Fixed::from_raw(sim::math::Fixed::scale)};
    std::vector<units::CollisionMesh> collision_meshes;
};

// L2b supplies effective profiles, movement masks, static occupations and their digest.
// No stock locomotion, geometry, passability or capacity defaults are inferred here.
struct LandStartBindings {
    sim::tactical::LandSessionContent land;
    std::vector<sim::tactical::LandMovementMask> movement_masks;
    std::optional<sim::tactical::LandBounds> bounds;
    std::map<sim::tactical::PlayerId, std::uint32_t> last_known_capacity;
    std::array<std::uint8_t, 32> identity{};
    std::vector<LandStaticBinding> static_objects;
    std::vector<std::string> static_notes;
    // The land types' combat and durability rows (empty when the tables carry none),
    // applied by the land step and included in content identity.
    sim::tactical::DurabilityTable durability;
    sim::tactical::CombatTable combat;
};
struct LandStart {
    SkirmishStart start;
    sim::tactical::TacticalContent content;
    std::vector<std::string> notes;
};

// WLEN-55: discovery uses the map header and authored Team_NN_ spawn markers.
[[nodiscard]] core::Result<std::vector<SetupMap>> land_setup_maps(
    const vfs::Vfs&, const data::Catalog&, SetupMapQuery query = {});
[[nodiscard]] core::Result<LandStartInputs> read_land_start_inputs(
    const Fixture&, const vfs::Vfs&, const data::Catalog&, const units::UnitTables&);
// WLEN-53..56, LC-46: pure start construction. L5 validates/places company members
// through L2 plans and L4. Unbound scene objects remain census-only, with named notes.
[[nodiscard]] core::Result<LandStart> build_land_start(
    const Fixture&, const LandStartInputs&, const LandStartBindings&);

struct LandContentInputs;
// Authored path: derive physical bindings from the land tables; explicit test path stays above.
[[nodiscard]] core::Result<LandStart> build_land_start(
    const Fixture&, const LandStartInputs&, const LandContentInputs&);

} // namespace eawr::skirmish
