#pragma once

#include "eawr/skirmish/setup.hpp"
#include "eawr/skirmish/land_terrain.hpp"
#include "eawr/sim/galactic/income.hpp"
#include "eawr/sim/tactical/economy.hpp"

namespace eawr::skirmish {

struct LandFactionStart {
    std::string faction;
    std::vector<std::string> forces;
    bool forces_authored{true}; // false means the tag was absent, distinct from an authored empty list.
    // Vehicles-only start: the stock vehicle companies a human of this faction starts with instead of
    // `forces` (see set_land_vehicles_only). Empty = this faction has no vehicle set; its defaults stay.
    std::vector<std::string> vehicle_forces;
    std::vector<sim::galactic::IncomeCurvePoint> company_cap;
};
struct LandStartInputs {
    StartInputs common;
    assets::Map map;
    std::vector<LandFactionStart> factions;
    bool human_vehicles_only{}; // one-switch vehicles-only land start for human slots; land only, no rules change
    units::UnitTables static_tables; // scene types and every authored pre-built marker candidate
    // LE-LOADER: the build pads of static_tables (a type with TACTICAL_BUILD_OBJECTS) decoded as land types with their
    // menus, prices, construction children and constructed types; empty when the map and its marker candidates have no pad.
    // A caller that builds the physical bindings from its own tables adds these rows to them (see merge_land_pad_types).
    units::UnitTables pad_tables;
    // LC-44..52 (#5894): the GameConstants rows the land reinforcement reads, filled by read_land_start_inputs. The
    // per-type rows are built from the loaded tables when the start is built.
    sim::tactical::LandReinforcementRules reinforcement_constants;
};
// The pad types of `pads` that `tables` does not already hold, appended in `pads` order. Needed so the durability, combat and
// sensor rows of pads, construction children and constructed structures reach the physical bindings.
void merge_land_pad_types(units::UnitTables& tables, const units::UnitTables& pads);
// LC-46: the land start's economy keeps its own EconomyPlayer rows (cap, start tech, credits, difficulty multipliers) and takes only
// the pad rules, the build menus and the pad sales from the shared economy builder run over the pad types. Everything else the
// builder derived (its player rows with the space unit cap, income, upgrades, prevention, footprints, disabled types) is dropped.
void merge_land_pad_rules(sim::tactical::EconomyRules& land, sim::tactical::EconomyRules&& builder);

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
    // WLAB-01 (#3238): the land ability rows of the selected companies, their members and the unlisted types. Bound to the
    // session's ability table; empty when no land row authors a kind of the vocabulary.
    sim::tactical::AbilityTable abilities;
};
struct LandStart {
    SkirmishStart start;
    sim::tactical::TacticalContent content;
    std::vector<std::string> notes;
};

// WLEN-55: discovery uses the map header and authored Team_NN_ spawn markers.
[[nodiscard]] core::Result<std::vector<SetupMap>> land_setup_maps(
    const vfs::Vfs&, const data::Catalog&, SetupMapQuery query = {});
// The same list with a stated number of decode workers (at most 8 are used; 1 reads serially). The list, its order and
// its entries do not depend on the number.
[[nodiscard]] core::Result<std::vector<SetupMap>> land_setup_maps(
    const vfs::Vfs&, const data::Catalog&, SetupMapQuery query, unsigned requested_workers);
// With a facts store (setup_facts.hpp), as setup_maps: a map whose header facts and finished entry are stored under this
// view's fingerprint is listed without opening or decoding it. The list is the same either way.
[[nodiscard]] core::Result<std::vector<SetupMap>> land_setup_maps(
    const vfs::Vfs&, const data::Catalog&, SetupMapQuery query, unsigned requested_workers, SetupMapFactsStore* facts);
[[nodiscard]] core::Result<LandStartInputs> read_land_start_inputs(
    const Fixture&, const vfs::Vfs&, const data::Catalog&, const units::UnitTables&);
// The same read for a caller that reads many land maps against one mounted view (the setup list): the map-independent
// facts and the catalog's object-type index are built once and reused through `cache`, and the data identity is
// the caller's. The result is the one the overload above returns.
[[nodiscard]] core::Result<LandStartInputs> read_land_start_inputs(
    const Fixture&, const vfs::Vfs&, const data::Catalog&, const units::UnitTables&, const DataIdentity&,
    CampaignStartInputsCache& cache);
// The stock vehicle companies a vehicles-only human start gives `faction` (empty for a faction without a set,
// e.g. a mod faction). Names only; build_land_start checks each is a generic-team vehicle company.
[[nodiscard]] std::vector<std::string> land_vehicle_start_forces(std::string_view faction);
// True for a company that moves on the generic team locomotor and whose members all carry a vehicle collision
// class (Vehicle, Large Vehicle, Giant Vehicle): no infantry container team, no air or empty member list.
[[nodiscard]] bool is_land_vehicle_company(const units::LandCompany& company) noexcept;
// Turns the switch on: sets human_vehicles_only and fills every faction's vehicle_forces.
void set_land_vehicles_only(LandStartInputs& inputs, bool enabled);
// LC-86 (#6183): the sensor rows of the placed static types that are in the session (`session_types`, any order). A row exists
// for a type that reveals (REVEAL with its authored range, 0 when absent) or samples the fog (Multisample_FOW_Check); a type
// outside `session_types` (census-only: never a session object) gets none. Sorted strictly ascending by type id.
[[nodiscard]] std::vector<sim::tactical::SensorProfile> land_static_sensor_table(
    const std::vector<units::LandStaticObject>& rows, std::span<const sim::tactical::TypeId> session_types);
// WLEN-53..56, LC-46: pure start construction. L5 validates/places company members
// through L2 plans and L4. Unbound scene objects remain census-only, with named notes.
[[nodiscard]] core::Result<LandStart> build_land_start(
    const Fixture&, const LandStartInputs&, const LandStartBindings&);

struct LandContentInputs;
// #6993: the companies the structures' menus sell (decoded with the pad types) that `selected` does not name, in the pad load's order.
// The authored build binds them after the selected ones (build_land_bindings, optional list). An explicit-bindings caller that wants the
// same content passes the same list and gates (add_offered_collision_classes).
[[nodiscard]] std::vector<std::string> land_offered_companies(const LandStartInputs&, std::span<const std::string> selected);
// The collision ordering gate lists the classes of the selected companies only; the classes the offered companies add follow it in sorted
// order (the assignment is unverified either way, see authored_land_content_inputs). A class of a selected company is not added.
void add_offered_collision_classes(LandContentInputs& gates, const LandStartInputs&, std::span<const std::string> offered);
// Authored path: derive physical bindings from the land tables; explicit test path stays above.
[[nodiscard]] core::Result<LandStart> build_land_start(
    const Fixture&, const LandStartInputs&, const LandContentInputs&);

} // namespace eawr::skirmish
