#pragma once

#include "eawr/battle/start.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/core/result.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/ai_level.hpp"
#include "eawr/sim/tactical/economy.hpp"
#include "eawr/sim/tactical/fog_cells.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/victory.hpp"
#include "eawr/sim/tactical/visibility.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <utility>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Tick zero of the pinned FoC space skirmish (P2-04, #67;
// plan/phase-2/m2-skirmish.md SK-01 to SK-24 and SK-30 to SK-36). The start is
// built in two steps: read_start_inputs() reads the map and the XML facts it
// needs from a mounted FoC view, and build_start() turns the fixture and those
// plain inputs into the tactical setup and its census. build_start() is a pure
// function of its inputs; nothing here reads a clock, a thread or host files.
// The rules are in docs/skirmish-start.md.
namespace eawr::skirmish {

using sim::math::Fixed;
using sim::math::Vec3;

// One lobby slot of the fixture (SK-10). The slot number is also the player ID.
struct LobbySlot final {
    std::uint32_t slot{};
    std::string faction;
    std::uint32_t team{}; // team t starts on the Team_tt markers (SK-03)
    bool human{};
    std::vector<std::string> fleet; // SK-22, after the free starting units
    std::optional<std::uint32_t> colour_index{}; // WSS-20: absent retains the pinned slot palette.
    sim::tactical::AiLevel difficulty{}; // WTA-47: a lobby AI's level (#735); ignored for a human
};

using battle::MatchOptions;

[[nodiscard]] inline sim::tactical::SkirmishMatchPolicy replay_policy(const MatchOptions& options) noexcept {
    return {options.allow_heroes, options.allow_superweapons, options.free_starting_units, options.pre_built_base};
}

// WSS-27..30: reconstruct the applied flags, retaining other authored options.
[[nodiscard]] inline MatchOptions replay_match_options(const sim::tactical::TacticalSetup& setup,
    MatchOptions defaults) {
    if (setup.skirmish) return setup.skirmish->match;
    const auto policy = setup.match_policy.value_or(sim::tactical::SkirmishMatchPolicy{});
    defaults.allow_heroes = policy.allow_heroes;
    defaults.allow_superweapons = policy.allow_superweapons;
    defaults.free_starting_units = policy.free_starting_units;
    defaults.pre_built_base = policy.pre_built_base;
    return defaults;
}

[[nodiscard]] core::Result<MatchOptions> read_match_defaults(const vfs::Vfs& filesystem);

// WSS-25: the staging's credits slider bounds, the GameConstants Min_Skirmish_Credits and
// Max_Skirmish_Credits.
struct CreditsBounds final {
    std::int64_t minimum{};
    std::int64_t maximum{};
};
[[nodiscard]] core::Result<CreditsBounds> read_credits_bounds(const vfs::Vfs& filesystem);

// MAT-08, MAT-09 (docs/behaviour/main-menu-attract.md): the start of a demo map. There is no
// lobby: the forces are the map's own placed objects, each kept for the editor player its owner
// index names (no ownership deletion), and no player buys anything. `ai` names the player type
// an editor player index runs; every other player runs none.
struct PlacedForces final {
    std::vector<std::pair<std::uint32_t, std::string>> ai;

    friend bool operator==(const PlacedForces&, const PlacedForces&) = default;
};

struct Fixture final {
    std::string map;        // logical TED path (SK-01)
    std::string map_sha256; // lower-case hex
    std::vector<LobbySlot> slots;
    bool pre_built_base{true};      // MP_Default_Pre_Built_Base (SK-20)
    bool free_starting_units{true}; // MP_Default_Free_Starting_Units (SK-21)
    std::uint64_t seed{};
    std::optional<sim::tactical::VictoryCondition> victory_condition{};
    std::optional<MatchOptions> match{};
    // Set for a demo map's start (MAT-08): `slots` is then empty and build_start keeps the placed
    // forces instead of running the lobby start.
    std::optional<PlacedForces> placed{};
};

// The M2 fixture: Coruscant, slot 1 Rebel human on team 0, slot 2 Empire AI on
// team 1, the retail lobby defaults and the owner's SK-22 fleet.
[[nodiscard]] const Fixture& m2_fixture();

// #908: project lobby options. Omitted fields retain the pinned M2 values;
// an explicit slot list replaces the lobby, including each slot's fleet.
struct FixtureOptions final {
    std::optional<std::string> map;
    std::optional<std::vector<LobbySlot>> slots;
    std::optional<std::uint64_t> seed;
    std::optional<sim::tactical::VictoryCondition> victory_condition{};
    std::optional<MatchOptions> match{};
    // Editable network staging only. Vacancies never become simulation players.
    std::vector<std::uint32_t> open_slots{};
};

// Reads and validates the selected space map and binds its actual SHA-256.
// The pinned map retains its expected hash; no options returns M2 unchanged.
[[nodiscard]] core::Result<Fixture> fixture_from_options(
    const FixtureOptions& options, const vfs::Vfs& filesystem, const data::Catalog& catalog);

// MAT-08: the fixture of a demo map's start. Reads the space map at the logical path `map`, binds
// its SHA-256 and returns a fixture with no lobby slots, no victory condition (MAT-17), the free
// forces and base off, and `forces` as its placed-forces rules.
[[nodiscard]] core::Result<Fixture> placed_fixture(std::string_view map, PlacedForces forces, std::uint64_t seed,
    const vfs::Vfs& filesystem, const data::Catalog& catalog);

using battle::MarkerCandidate;
using battle::MapPlacement;

// A faction in editor player index order (scene::faction_order) with the flags the
// retail map-object ownership pass reads.
struct StartFaction final {
    std::string name{};
    bool playable{};           // Is_Playable
    bool multiplayer_player{}; // Create_Player_In_Multiplayer_Games
    bool neutral{};            // Is_Neutral
    // #530 PU-21: Space_Tactical_Unit_Cap, the faction's population cap in a space skirmish.
    std::optional<std::uint32_t> space_unit_cap{};
    std::string basic_ai{}; // WSS-35: authored Basic_AI player type; absent means no controller.
    std::array<std::string, 2> standalone_special_weapons{}; // WSW-01/02: faction A/B mappings.
    std::optional<sim::tactical::TacticalSetup::BeaconRule> beacon{};
    bool can_control_planets{}; // WGT-121: independent of Is_Playable.
};

struct FactionForces final {
    std::string faction{};
    std::vector<std::string> space_skirmish_default_forces{}; // Space_Skirmish_AI_Default_Forces
    std::optional<std::uint32_t> garrison_delay_frames{}; // Garrison_Reinforcement_Delay_Seconds x 30
};

using battle::LobbyColour;

inline constexpr std::uint32_t multiplayer_colour_count = 9; // WSS-20.
[[nodiscard]] core::Result<std::vector<LobbyColour>> read_lobby_colours(const vfs::Vfs& filesystem);

} // namespace eawr::skirmish
namespace eawr::script::foc { class AiDifficultyTable; }
namespace eawr::skirmish {

struct StartInputs final {
    std::string map{};
    std::string map_sha256{};
    std::vector<MapPlacement> placements{};      // TED record order
    std::vector<StartFaction> factions{};        // editor player index order (scene::faction_order)
    std::vector<FactionForces> faction_forces{}; // the fixture's lobby factions
    std::vector<LobbyColour> lobby_colours{};    // MP_Color_* in gameconstants.xml order
    const units::UnitTables* tables{};
    // The map's declared extents (root fields 0x10 and 0x11) as binary32 bit patterns, when its
    // header has them (#449).
    std::optional<std::pair<std::uint32_t, std::uint32_t>> map_extents{};
    // gameconstants.xml's DesiredSpaceFOWCellSize and SpaceFOWRegrowTime (FoC: 100 and 6 s), the
    // fog grid's cell and regrow time (#495).
    sim::math::Fixed fog_cell_size{sim::math::Fixed::from_raw(std::int64_t{100} * sim::math::Fixed::scale)};
    sim::math::Fixed fog_regrow_seconds{sim::math::Fixed::from_raw(std::int64_t{6} * sim::math::Fixed::scale)};
    sim::tactical::VictoryCondition space_victory_condition{sim::tactical::VictoryCondition::enemy_starbase_destroyed};
    MatchOptions match_defaults{};
    std::vector<sim::tactical::TypeId> named_heroes{}, superweapons{};
    // Effective mounted data closure; synthetic inputs use canonical empty digests.
    std::string script_manifest_digest{}, ai_data_digest{};
    // WTA-46 (#735): the mounted AI player types' level -> difficulty entry table; absent for synthetic inputs and
    // data without difficulty entries. Read it through skirmish::player_difficulty().
    std::shared_ptr<const script::foc::AiDifficultyTable> difficulty{};
};

// Rebuild lobby/faction bindings from recorded players; human ownership remains
// the caller's fixture contract, as for legacy replays.
[[nodiscard]] core::Result<Fixture> replay_fixture(const Fixture& base, const StartInputs& inputs,
    const sim::tactical::TacticalSetup& setup, bool check_identity = true);

// A TED map's declared extents (root minis 0x10 and 0x11) as binary32 bit patterns, read from
// each mini's payload in `ted`, the map's bytes. Nullopt without both or when a mini is not the
// 4-byte field the decoder reported.
[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> declared_extent_bits(
    std::span<const std::uint8_t> ted, const assets::Map& map);

// Reads the fixture's map (checking its SHA-256), its placements, the lobby
// factions' starting forces and the lobby colours from a mounted FoC view.
[[nodiscard]] core::Result<StartInputs> read_start_inputs(
    const Fixture& fixture,
    const vfs::Vfs& filesystem,
    const data::Catalog& catalog,
    const units::UnitTables& tables);

[[nodiscard]] sim::MatchIdentity match_identity(const StartInputs& inputs);

struct DataIdentity {
    std::string script_manifest_digest, ai_data_digest;
};
[[nodiscard]] core::Result<DataIdentity> read_data_identity(const vfs::Vfs& filesystem);
[[nodiscard]] core::Result<void> validate_replay_identity(const sim::tactical::TacticalSetup& setup,
    const StartInputs& inputs);

using battle::UnitRole;
using battle::MarkerUse;
using battle::to_string;
using battle::StartPlayer;
using battle::StartUnit;
using battle::StartMarker;
using battle::Launch;
using battle::Removal;
using battle::RemovedObject;
using SkirmishStart = battle::BattleStart;

// Attach recording metadata after building a live start; fixture builders retain their pins.
[[nodiscard]] sim::tactical::TacticalSetup recording_setup(const Fixture& fixture, const SkirmishStart& start);

using battle::type_id;
using battle::faction_id;
using battle::layer_position;
using battle::TacticalSuperWeaponStart;
using battle::tactical_super_weapon_start;
using battle::tactical_super_weapon_targets;

// True when a TED placement can become a map-object unit (add_map_objects): it names a catalog
// type, is not a designer marker, and is not a SpaceProp decoration. Map-object units are not in
// the unit tables, so this is also how a replay resolves their type_id back to a name (#501).
[[nodiscard]] bool is_map_object_placement(const MapPlacement& placement) noexcept;

// MAT-08: the types a demo map's start tries to make units of: every map-object placement that
// is not a space hazard and not a SpaceProp, minus the types the roster gate disables, sorted and
// without duplicates. The caller loads them into the unit tables before build_start.
[[nodiscard]] std::vector<std::string> placed_force_types(const std::vector<MapPlacement>& placements);

// The type name of every placement is_map_object_placement() admits, keyed by type_id(). The live
// path (add_map_objects) places map objects straight from these placements; a replay has no unit
// table entry for them, so it resolves the same way from the same placements (#501).
[[nodiscard]] std::map<sim::tactical::TypeId, std::string> map_object_type_names(
    const std::vector<MapPlacement>& placements);

// The sensor table of the unit tables (#68, #271): one profile per type with a
// units::sensor_range (REVEAL types with their own range, squadrons with their
// team container's), keyed by type_id() and sorted by it. It is content the
// tables' identity names, not replay data.
[[nodiscard]] std::vector<sim::tactical::SensorProfile> sensor_table(const units::UnitTables& tables);

// The sensor table of a revealed map (the fixed-force recordings stage fog revealed,
// docs/traces.md): every listed type sees revealed_sensor_range, so fog never hides a unit from
// targeting (space-targeting R-08). Sorted by type ID, duplicates removed.
inline constexpr std::int64_t revealed_sensor_range = std::int64_t{1} << 17; // whole source units
[[nodiscard]] std::vector<sim::tactical::SensorProfile> revealed_sensor_table(
    std::span<const sim::tactical::TypeId> types);

// The content tables a tactical session of the tables' types runs with: the sensor table
// (#68), the durability table (#72), the motion table (#70) and the combat table (#73, bound
// since #80 so live units fire). None of them is replay data; the setup's content identity
// names them. Fails when the durability, motion or combat table does.
// Since #76 it also holds the ability table, whose human players are `humans` (like the victory
// rules', not replay data).
struct SessionContent final {
    std::vector<sim::tactical::SensorProfile> sensors;
    sim::tactical::DurabilityTable durability;
    sim::tactical::MotionTable motion;
    sim::tactical::CombatTable combat;
    sim::tactical::AbilityTable abilities;
    // #495: the map's fog grid (space-visibility V-11 to V-19), set by the caller from the start
    // inputs (fog_rules); none for a revealed map, whose sensors see everything.
    std::optional<sim::tactical::FogRules> fog;
    // WDF-05: the space map's box, the fighter grids' low corner (map_box); the world origin when unset.
    sim::tactical::MapBox map_box;
};
[[nodiscard]] core::Result<SessionContent> session_content(
    const units::UnitTables& tables, std::span<const sim::tactical::PlayerId> humans = {},
    const units::SpawnPolicy& spawn_policy = {});

// WTA-46 (#735): bind each non-Normal AI slot's health, shield and damage scales into `content.durability.difficulty`
// (sorted by player). Humans, Normal slots and neutral adjustments add nothing, so Normal sessions are unchanged.
[[nodiscard]] core::Result<void> bind_difficulty(
    SessionContent& content, const SkirmishStart& start, const StartInputs& inputs);

// #495 (space-visibility V-18): FoC's fog grid of a skirmish map. It spans the map's declared
// extents centred on the origin (Coruscant: 130 x 130 cells of 100 from (-6500, 6500), as
// recorded), in cells of DesiredSpaceFOWCellSize, at most 512 a side, serviced every 16 frames
// with the SpaceFOWRegrowTime ramp. None when the map declares no extents.
[[nodiscard]] core::Result<std::optional<sim::tactical::FogRules>> fog_rules(const StartInputs& inputs);
// WDF-05: the space map's box for the fighter grids, its low corner at minus half the declared
// extents about the origin (the box the fog grid and the playable bounds span, PU-31). A map that
// declares no usable extents gives the world origin.
[[nodiscard]] sim::tactical::MapBox map_box(const StartInputs& inputs);
[[nodiscard]] sim::tactical::MapBox map_box_of_extents(
    const std::optional<std::pair<std::uint32_t, std::uint32_t>>& extents);
// The same for a map read from `filesystem` (a fixed-force scenario's map, which has no start).
[[nodiscard]] core::Result<sim::tactical::MapBox> map_box_of(
    const vfs::Vfs& filesystem, const data::Catalog& catalog, std::string_view map);

// The victory rules of a skirmish of these tables (#77, docs/behaviour/space-victory.md): FoC's
// default space win condition (SK-33), the tables' stations that are Victory_Relevant (every FoC
// StarBase type authors DUMMY_STAR_BASE), the setup's commandable players as the contenders (the
// lobby players; the non-playable factions' players have no command flag, OW-E3) and `humans`
// among them. Like the other content tables it is neither replay data nor state.
[[nodiscard]] sim::tactical::VictoryRules victory_rules(const sim::tactical::TacticalSetup& setup,
    const units::UnitTables& tables, std::span<const sim::tactical::PlayerId> humans,
    sim::tactical::VictoryCondition condition = sim::tactical::VictoryCondition::enemy_starbase_destroyed);
// The start's own human lobby players.
[[nodiscard]] sim::tactical::VictoryRules victory_rules(const SkirmishStart& start, const units::UnitTables& tables);

// The economy of a skirmish (#530, docs/behaviour/space-purchasing.md): every lobby player starts
// with MP_Default_Credits (PU-01) and its faction's Space_Tactical_Unit_Cap (PU-21), and its
// reinforcements arrive facing its SK-11 spawn marker (PU-34); the non-lobby players have none.
// Each table station's Tactical_Buildable_Objects_Multiplayer becomes one menu per faction group
// (PU-10): a table type is a unit with its multiplayer price, build time in frames (times
// Tactical_Build_Time_Multiplier; the Normal AI's multiplier is 1, SK-42) and population, and
// anything else (the upgrades, #540) is listed but not available (PU-20). A station's
// Income_Stream_Ability pays its base value per interval per frame, and each
// Income_Stream_Mod_Ability of the same station adds its additive value while the hardpoint that
// enables it stands (PU-02 to PU-04). Every table type with a Reinforcement_Prevention_Radius
// keeps reinforcements out (PU-31); the playable bounds are the map's declared extents about the
// origin; the queue holds 5 (PU-14); an arrival is vulnerable for
// Space_Elevated_Vulnerability_Duration by Space_Elevated_Vulnerability_Factor (PU-38). Fails when
// a constant it needs is missing or validate_economy rejects the rules.
[[nodiscard]] core::Result<sim::tactical::EconomyRules> economy_rules(
    const SkirmishStart& start, const StartInputs& inputs, const units::UnitTables& tables, bool campaign = false);
// WBP-01..05/50, WGT-03: capture services are shared by skirmish and campaign battles.
// Campaign purchasing/construction remains WGT-26's separate economy adapter.
[[nodiscard]] core::Result<sim::tactical::PadRules> capture_rules(
    const sim::tactical::TacticalSetup& setup, const StartInputs& inputs, const units::UnitTables& tables);
// The fixture's human slots (also the fallback for recordings without SKSU).
[[nodiscard]] std::vector<sim::tactical::PlayerId> human_slots(const Fixture& fixture);

// Fails on a missing marker or station candidate, a type the tables lack, a
// record without a yaw-only transform, a map object without an owner index, an
// owner index past the factions without a neutral faction, or a type-ID collision.
[[nodiscard]] core::Result<SkirmishStart> build_start(const Fixture& fixture, const StartInputs& inputs);

// An already selected force and its authored marker. Selection/ownership policy belongs to
// the caller; the shared builder supplies transforms, craft, map objects and free-space placement.
struct SelectedStartUnit final {
    sim::tactical::PlayerId owner{};
    std::string type;
    UnitRole role{UnitRole::fleet};
    std::uint32_t marker{};
    bool garrison_enabled{true};
};
// Optional admission after faction/player binding, before creation. Empty preserves skirmish.
using MapObjectFilter = std::function<bool(const MapPlacement&, const StartFaction&,
    sim::tactical::PlayerId)>;
[[nodiscard]] core::Result<SkirmishStart> build_selected_start(const Fixture& fixture,
    const StartInputs& inputs, std::span<const SelectedStartUnit> selected, bool map_first = false,
    const MapObjectFilter& map_filter = {});

// The tick-zero census as UTF-8 JSON (LF lines, no BOM). With `start`, it also
// lists names, markers, colours, powers and launches; without it, it lists only
// what the setup holds, so a census from a replay header alone can be compared
// with the census of the start that wrote it. `sensors` feeds the tick-zero
// snapshot's visibility; the state hash does not depend on it.
[[nodiscard]] core::Result<std::string> census_json(
    const sim::tactical::TacticalSetup& setup,
    const SkirmishStart* start,
    std::span<const sim::tactical::SensorProfile> sensors = {});

namespace diagnostic_codes {
inline constexpr std::string_view input = "EAWR-SKIRMISH-0001";
inline constexpr std::string_view fixture = "EAWR-SKIRMISH-0002";
} // namespace diagnostic_codes

} // namespace eawr::skirmish
