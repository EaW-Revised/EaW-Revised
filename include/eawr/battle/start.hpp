#pragma once

#include "eawr/battle/arrival_plan.hpp"

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/victory.hpp"
#include "eawr/units/unit_tables.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::battle {

using sim::math::Fixed;
using sim::math::Vec3;

// WSS-22/24: a copied match record; only mode-supported controls are exposed.
using MatchOptions = sim::tactical::ReplayMatchOptions;

// A station-marker candidate (Marker_For_Specific_Object_Type) and its
// Affiliation as authored.
struct MarkerCandidate final {
    std::string type{};
    std::string affiliation{};
};

// One TED placement record with the catalog facts the start needs.
struct MapPlacement final {
    std::uint32_t record{};                   // 1100 record ordinal
    std::string type{};                         // catalog object id; empty when the CRC is not unique
    std::string element{};                      // XML element of the winner, e.g. SpaceProp
    std::optional<std::int32_t> owner_index{};  // TED mini 2, the editor player index
    std::string owner_faction{};                // the faction that index names; empty when unmapped
    std::optional<Vec3> position{};             // exact binary32 to Q24, TED units
    // Roll X, pitch Y, yaw Z in degrees, exact binary32 to Q24; absent when
    // TED mini 5 is absent or not finite.
    std::optional<Vec3> orientation_degrees{};
    bool marker{};                            // Is_Marker = yes or the Marker object class
    bool space_hazard{};                      // WHZ-01: a tracked, independently flagged space hazard
    std::vector<MarkerCandidate> marker_for{};
    bool victory_relevant{};
    std::optional<Fixed> hull{};                // Tactical_Health
    // Is_Decoration and Is_Discardable, read as retail reads booleans; retail's type
    // defaults are false and true (docs/skirmish-start.md, retail map-object ownership).
    bool decoration{};
    bool discardable{true};
    std::optional<Fixed> layer_z_adjust{};      // Layer_Z_Adjust (SK-05)
    bool special_weapon_source{}; // WSW-01: DUMMY_SPECIAL_WEAPON_SOURCE
    bool tactical_super_weapon_target{}; // TSW-01: DUMMY_TACTICAL_SUPER_WEAPON_TARGET
    // WGT-121: copied campaign branch facts; ordinary skirmish does not consume them.
    bool branched_discardable{true};
    bool loads_when_faction_present{};
    std::int8_t enable_at_level{}; // TED mini 11; absent defaults to zero.
    bool marker_behavior{}; // WGT-121: MARKER in generic Behavior; mode-specific lists do not qualify.
};

struct LobbyColour final {
    std::string constant{}; // MP_Color_<name>
    std::array<std::uint8_t, 3> rgb{};
};

enum class UnitRole : std::uint8_t {
    station,    // SK-20: the lobby's pre-built base on the station marker
    free_unit,  // SK-21: Space_Skirmish_AI_Default_Forces at the spawn marker
    fleet,      // SK-22: the fixture fleet, after the free units
    map_object, // SK-04: a non-marker, non-prop map placement (SK-32: inert)
    craft,      // #75: a squadron company's craft; the company is its team container
    special_weapon, // WSW-01: hidden planetary source, retained for object/AI identity.
    placed,     // MAT-08: a force of a demo map, a table type kept for the player that owns it
};

enum class MarkerUse : std::uint8_t { station, base_position, spawn, spawn_unused };

[[nodiscard]] std::string_view to_string(UnitRole role) noexcept;
[[nodiscard]] std::string_view to_string(MarkerUse use) noexcept;

struct StartPlayer final {
    sim::tactical::Player player;
    std::string faction;
    bool lobby{};                             // a lobby slot; otherwise a non-playable faction's player
    bool human{};
    std::string ai_player_type;               // MAT-09: the AI player type a demo map's player runs
    sim::tactical::AiLevel difficulty{};      // WTA-47: the lobby slot's AI level (#735)
    std::string start_side;                   // Team_NN for lobby players
    std::optional<LobbyColour> colour;        // lobby players (SK-12)
    std::optional<std::int32_t> owner_index;  // non-lobby players: the faction's editor player index
    // SK-30, SK-31 (#530): the starting credits (MP_Default_Credits), and whether the player
    // earns income, builds at its station and has a population cap.
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
    roster_gated,     // MAT-08: a demo map's force the roster gate disables (RG-04)
    campaign_branch, // WGT-121: rejected by the supplied campaign map admission rule
};

[[nodiscard]] std::string_view to_string(Removal reason) noexcept;

struct RemovedObject final {
    std::uint32_t record{};
    std::string type;
    std::string faction; // the faction its TED owner index names
    Removal reason{Removal::playable_faction};
};

struct BattleStart final {
    bool campaign_intro_admitted{}; // WGT-23: one-shot presentation input, outside replay bytes
    sim::MatchIdentity identity;
    std::string map;
    std::string map_sha256;
    std::vector<StartPlayer> players; // player ID order
    std::vector<StartUnit> units;     // entity ID order
    std::vector<RemovedObject> removed; // map objects deleted at the start, record order
    std::vector<StartMarker> markers; // lobby team markers, record order
    std::vector<Launch> launches;
    sim::tactical::TacticalSetup setup;
    sim::tactical::VictoryCondition victory_condition{sim::tactical::VictoryCondition::enemy_starbase_destroyed};
    MatchOptions match{}; // applied flags bind replays; other copied options remain content.
    ArrivalPlan arrivals{}; // presentation capacity only; deliberately outside setup/replay state
};

// Type and faction IDs are the CRC-32 of the ASCII-upper-cased name, the hash
// TED placements use for object types (assets::object_type_crc).
[[nodiscard]] sim::tactical::TypeId type_id(std::string_view type) noexcept;
[[nodiscard]] sim::tactical::FactionId faction_id(std::string_view faction) noexcept;

// SK-05 (space-movement LZ-01): a created object stands at the point it is created at, raised
// by its type's Layer_Z_Adjust (none: 0).
[[nodiscard]] core::Result<Vec3> layer_position(Vec3 at, std::optional<Fixed> layer_z_adjust);

// TSW-01: where the debug build puts a TACTICAL_SUPER_WEAPON object at its first service. `targets` are the
// positions of the enemy-owned super weapon targets (tactical_super_weapon_targets); the nearest one to
// `current` anchors the start: the object stands start_distance from it (attack_distance when its owner is the
// defending player), on the line from the target through `current`, at the target's height when
// start_pos_match_target_z is set, then z_adjust higher. With no target it stays where it is. Either way it then
// faces the battlefield centre (the origin), keeping only the heading.
struct TacticalSuperWeaponStart final {
    Vec3 position{};
    Fixed yaw_degrees{};
};
[[nodiscard]] core::Result<TacticalSuperWeaponStart> tactical_super_weapon_start(
    const units::UnitType::TacticalSuperWeapon& data, Vec3 current, std::span<const Vec3> targets, bool owner_defends);
// Position of every DUMMY_TACTICAL_SUPER_WEAPON_TARGET placement that is not owned by `own_faction`, in record order: its
// map position as authored; the debug build does not add the prop's Layer_Z_Adjust to the target's height. A target whose
// editor owner names no faction (an unmapped index) is not owned by `own_faction` and counts; an empty `own_faction`
// skips nothing.
[[nodiscard]] core::Result<std::vector<Vec3>> tactical_super_weapon_targets(
    const std::vector<MapPlacement>& placements, std::string_view own_faction);

} // namespace eawr::battle
