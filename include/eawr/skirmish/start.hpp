#pragma once

#include "eawr/assets/map.hpp"
#include "eawr/core/result.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/fog_cells.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/victory.hpp"
#include "eawr/sim/tactical/visibility.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <optional>
#include <utility>
#include <cstdint>
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
};

struct Fixture final {
    std::string map;        // logical TED path (SK-01)
    std::string map_sha256; // lower-case hex
    std::vector<LobbySlot> slots;
    bool pre_built_base{true};      // MP_Default_Pre_Built_Base (SK-20)
    bool free_starting_units{true}; // MP_Default_Free_Starting_Units (SK-21)
    std::uint64_t seed{};
};

// The M2 fixture: Coruscant, slot 1 Rebel human on team 0, slot 2 Empire AI on
// team 1, the retail lobby defaults and the owner's SK-22 fleet.
[[nodiscard]] const Fixture& m2_fixture();

// A station-marker candidate (Marker_For_Specific_Object_Type) and its
// Affiliation as authored.
struct MarkerCandidate final {
    std::string type;
    std::string affiliation;
};

// One TED placement record with the catalog facts the start needs.
struct MapPlacement final {
    std::uint32_t record{};                   // 1100 record ordinal
    std::string type;                         // catalog object id; empty when the CRC is not unique
    std::string element;                      // XML element of the winner, e.g. SpaceProp
    std::optional<std::int32_t> owner_index;  // TED mini 2, the editor player index
    std::string owner_faction;                // the faction that index names; empty when unmapped
    std::optional<Vec3> position;             // exact binary32 to Q24, TED units
    // Roll X, pitch Y, yaw Z in degrees, exact binary32 to Q24; absent when
    // TED mini 5 is absent or not finite.
    std::optional<Vec3> orientation_degrees;
    bool marker{};                            // Is_Marker = yes or Behavior MARKER
    std::vector<MarkerCandidate> marker_for;
    bool victory_relevant{};
    std::optional<Fixed> hull;                // Tactical_Health
    // Is_Decoration and Is_Discardable, read as retail reads booleans; retail's type
    // defaults are false and true (docs/skirmish-start.md, retail map-object ownership).
    bool decoration{};
    bool discardable{true};
    std::optional<Fixed> layer_z_adjust;      // Layer_Z_Adjust (SK-05)
};

// A faction in editor player index order (scene::faction_order) with the flags the
// retail map-object ownership pass reads.
struct StartFaction final {
    std::string name;
    bool playable{};           // Is_Playable
    bool multiplayer_player{}; // Create_Player_In_Multiplayer_Games
    bool neutral{};            // Is_Neutral
};

struct FactionForces final {
    std::string faction;
    std::vector<std::string> space_skirmish_default_forces; // Space_Skirmish_AI_Default_Forces
};

struct LobbyColour final {
    std::string constant; // MP_Color_<name>
    std::array<std::uint8_t, 3> rgb{};
};

struct StartInputs final {
    std::string map;
    std::string map_sha256;
    std::vector<MapPlacement> placements;      // TED record order
    std::vector<StartFaction> factions;        // editor player index order (scene::faction_order)
    std::vector<FactionForces> faction_forces; // the fixture's lobby factions
    std::vector<LobbyColour> lobby_colours;    // MP_Color_* in gameconstants.xml order
    const units::UnitTables* tables{};
    // The map's declared extents (root fields 0x10 and 0x11) as binary32 bit patterns, when its
    // header has them (#449).
    std::optional<std::pair<std::uint32_t, std::uint32_t>> map_extents;
    // gameconstants.xml's DesiredSpaceFOWCellSize and SpaceFOWRegrowTime (FoC: 100 and 6 s), the
    // fog grid's cell and regrow time (#495).
    sim::math::Fixed fog_cell_size{sim::math::Fixed::from_raw(std::int64_t{100} * sim::math::Fixed::scale)};
    sim::math::Fixed fog_regrow_seconds{sim::math::Fixed::from_raw(std::int64_t{6} * sim::math::Fixed::scale)};
};

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

enum class UnitRole : std::uint8_t {
    station,    // SK-20: the lobby's pre-built base on the station marker
    free_unit,  // SK-21: Space_Skirmish_AI_Default_Forces at the spawn marker
    fleet,      // SK-22: the fixture fleet, after the free units
    map_object, // SK-04: a non-marker, non-prop map placement (SK-32: inert)
    craft,      // #75: a squadron company's craft; the company is its team container
};

enum class MarkerUse : std::uint8_t { station, base_position, spawn, spawn_unused };

[[nodiscard]] std::string_view to_string(UnitRole role) noexcept;
[[nodiscard]] std::string_view to_string(MarkerUse use) noexcept;

struct StartPlayer final {
    sim::tactical::Player player;
    std::string faction;
    bool lobby{};                             // a lobby slot; otherwise a non-playable faction's player
    bool human{};
    std::string start_side;                   // Team_NN for lobby players
    std::optional<LobbyColour> colour;        // lobby players (SK-12)
    std::optional<std::int32_t> owner_index;  // non-lobby players: the faction's editor player index
    // SK-30, SK-31: no credits, income, production queue or population cap.
    std::int64_t credits{};
    bool income{};
    bool production_queue{};
    bool population_cap{};
    // SK-24 by AI_Combat_Power; a squadron counts as the sum of its craft.
    Fixed combat_power_tick_zero{};
    Fixed combat_power_launches{};
};

struct StartUnit final {
    sim::tactical::UnitState state;
    std::string type;
    UnitRole role{UnitRole::station};
    std::uint32_t record{}; // the marker it starts on, or the map object's own record
    Fixed yaw_degrees{};
    // A map object whose record also has roll or pitch: tick zero keeps the
    // yaw alone (the Euler order is unresolved, docs/asset-formats.md).
    std::optional<Vec3> dropped_orientation_degrees;
    std::optional<Fixed> combat_power;
    std::optional<Fixed> reveal_range; // units::sensor_range of the tables' type (#68, #271)
    std::uint32_t craft{};  // squadron members
    std::string craft_type;
    bool victory_relevant{};
    std::optional<Fixed> hull;
};

struct StartMarker final {
    std::uint32_t record{};
    std::string type;
    sim::tactical::PlayerId player{}; // the lobby player it serves; 0 when unused
    MarkerUse use{MarkerUse::spawn_unused};
    Vec3 position{};
    Fixed yaw_degrees{};
};

// SK-23 launch data for #75: each spawner's Starting_Spawned_Units_Tech_0,
// launched once after the delay. Tick zero does not simulate it.
struct Launch final {
    sim::EntityId spawner{};
    std::string spawner_type;
    sim::tactical::PlayerId owner{};
    std::string squadron;
    std::int32_t count{};
    std::optional<Fixed> delay_seconds;
    Fixed combat_power{}; // count x the squadron's craft sum
};

// Why the retail ownership pass deletes a map object at the start.
enum class Removal : std::uint8_t {
    playable_faction, // its owner index names a playable faction
    no_player,        // no player of its non-playable faction, and not a kept decoration
};

[[nodiscard]] std::string_view to_string(Removal reason) noexcept;

struct RemovedObject final {
    std::uint32_t record{};
    std::string type;
    std::string faction; // the faction its TED owner index names
    Removal reason{Removal::playable_faction};
};

struct SkirmishStart final {
    std::string map;
    std::string map_sha256;
    std::vector<StartPlayer> players; // player ID order
    std::vector<StartUnit> units;     // entity ID order
    std::vector<RemovedObject> removed; // map objects deleted at the start, record order
    std::vector<StartMarker> markers; // lobby team markers, record order
    std::vector<Launch> launches;
    sim::tactical::TacticalSetup setup;
};

// Type and faction IDs are the CRC-32 of the ASCII-upper-cased name, the hash
// TED placements use for object types (assets::object_type_crc).
[[nodiscard]] sim::tactical::TypeId type_id(std::string_view type) noexcept;
[[nodiscard]] sim::tactical::FactionId faction_id(std::string_view faction) noexcept;

// SK-05 (space-movement LZ-01): a created object stands at the point it is created at, raised
// by its type's Layer_Z_Adjust (none: 0).
[[nodiscard]] core::Result<Vec3> layer_position(Vec3 at, std::optional<Fixed> layer_z_adjust);

// True when a TED placement can become a map-object unit (add_map_objects): it names a catalog
// type, is not a designer marker, and is not a SpaceProp decoration. Map-object units are not in
// the unit tables, so this is also how a replay resolves their type_id back to a name (#501).
[[nodiscard]] bool is_map_object_placement(const MapPlacement& placement) noexcept;

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
};
[[nodiscard]] core::Result<SessionContent> session_content(
    const units::UnitTables& tables, std::span<const sim::tactical::PlayerId> humans = {});

// #495 (space-visibility V-18): FoC's fog grid of a skirmish map. It spans the map's declared
// extents centred on the origin (Coruscant: 130 x 130 cells of 100 from (-6500, 6500), as
// recorded), in cells of DesiredSpaceFOWCellSize, at most 512 a side, serviced every 16 frames
// with the SpaceFOWRegrowTime ramp. None when the map declares no extents.
[[nodiscard]] core::Result<std::optional<sim::tactical::FogRules>> fog_rules(const StartInputs& inputs);

// The victory rules of a skirmish of these tables (#77, docs/behaviour/space-victory.md): FoC's
// default space win condition (SK-33), the tables' stations that are Victory_Relevant (every FoC
// StarBase type authors DUMMY_STAR_BASE), the setup's commandable players as the contenders (the
// lobby players; the non-playable factions' players have no command flag, OW-E3) and `humans`
// among them. Like the other content tables it is neither replay data nor state.
[[nodiscard]] sim::tactical::VictoryRules victory_rules(const sim::tactical::TacticalSetup& setup,
    const units::UnitTables& tables, std::span<const sim::tactical::PlayerId> humans);
// The start's own human lobby players.
[[nodiscard]] sim::tactical::VictoryRules victory_rules(const SkirmishStart& start, const units::UnitTables& tables);
// The fixture's human slots: a replay does not record which players are human.
[[nodiscard]] std::vector<sim::tactical::PlayerId> human_slots(const Fixture& fixture);

// Fails on a missing marker or station candidate, a type the tables lack, a
// record without a yaw-only transform, a map object without an owner index, an
// owner index past the factions without a neutral faction, or a type-ID collision.
[[nodiscard]] core::Result<SkirmishStart> build_start(const Fixture& fixture, const StartInputs& inputs);

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
