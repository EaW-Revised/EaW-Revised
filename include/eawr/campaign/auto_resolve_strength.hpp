#pragma once

#include "eawr/campaign/conflict.hpp"
#include "eawr/units/galactic_types.hpp"
#include "eawr/units/unit_tables.hpp"

#include <array>
#include <span>

namespace eawr::campaign {

// Runtime classification and order are explicit: landed contents, built structures and
// layer capability are not yet in the GC snapshot. Fleet membership uses FT-09's actual
// ordered Fleet values. No tactical health or placement is needed (WAR-13..16/20/22).
enum class AutoResolveObjectKind { unit, transport, dummy_starship };
struct AutoResolveObject {
    sim::EntityId id{};
    sim::galactic::PlayerId owner{};
    std::string original_type, current_type;
    AutoResolveObjectKind kind{AutoResolveObjectKind::unit};
    bool space_capable{}, land_capable{}, tactical{}, has_special_abilities{};
    std::vector<sim::EntityId> contents; // transport order; units are looked up by ID
    std::vector<sim::EntityId> fleet_members; // contained own-fleet starships for WAR-16
    sim::galactic::TypeId current_type_id{}; // live explicit setup binding; zero only for pure type-name fixtures
    friend bool operator==(const AutoResolveObject&, const AutoResolveObject&) = default;
};
struct AutoResolvePlanet {
    sim::galactic::PlanetId id{};
    sim::galactic::PlayerId owner{};
    std::string base_type; // empty means no base
    std::vector<sim::EntityId> landed, ground_specials;
    std::vector<std::string> built_structures; // persistent tactical structures, in build order
    bool has_special_abilities{};
    friend bool operator==(const AutoResolvePlanet&, const AutoResolvePlanet&) = default;
};
struct AutoResolveFleet {
    sim::galactic::Fleet fleet;
    std::string squadron_type; // a squadron fleet is one combatant in space (WAR-15)
    bool space_capable{}; // a runtime fleet predicate, not a type default
    bool has_special_abilities{};
    friend bool operator==(const AutoResolveFleet&, const AutoResolveFleet&) = default;
};
struct AutoResolveTech {
    sim::galactic::PlayerId player{};
    std::uint32_t level{};
};
struct AutoResolveRoster {
    AutoResolvePlanet planet;
    std::vector<AutoResolveObject> objects;
    std::vector<AutoResolveFleet> fleets;
    std::vector<sim::EntityId> defender_entries, invader_entries; // WAR-13, fleets or objects
    std::vector<sim::EntityId> orbit_fleets; // planet orbit order, WAR-14/16
    std::vector<sim::EntityId> raid_entries; // WAR-14, invading objects
    std::vector<AutoResolveTech> tech;
};
struct AutoResolveCategory {
    std::string name;
    bool ground{};
    friend bool operator==(const AutoResolveCategory&, const AutoResolveCategory&) = default;
};
struct AutoResolveCombatant {
    sim::EntityId id{};
    std::string type; // WAR-20 resolved type; empty for a land planet without a base (WAR-22)
    bool planet{}, transport{}, named_hero{}, tactical{};
    std::uint32_t tech{};
    std::string hero_type, bonus_type;
    bool has_special_abilities{};
    bool contains_superweapon_killer{};
    friend bool operator==(const AutoResolveCombatant&, const AutoResolveCombatant&) = default;
};
struct AutoResolveSide {
    sim::galactic::PlayerId owner{};
    std::vector<AutoResolveCombatant> combatants;
    friend bool operator==(const AutoResolveSide&, const AutoResolveSide&) = default;
};
struct AutoResolveSides {
    std::array<AutoResolveSide, 2> sides;
    sim::galactic::PlayerId aggressor{}, planet_owner{}, bombing_side{};
    std::string bomber_type;
    std::uint32_t terrain_index{};
    std::vector<std::string> built_structures;
    BattleLayer layer{BattleLayer::space};
    friend bool operator==(const AutoResolveSides&, const AutoResolveSides&) = default;
};
struct AutoResolveSideStrength {
    // Ground total, space total, then contrast entries in supplied contrast-list order (WAR-21).
    std::vector<sim::math::Fixed> force;
    std::array<sim::math::Fixed, 32> category_bonuses{}; // factor minus one, WAR-26
    sim::EntityId weakest{}, superweapon{}, superweapon_killer{};
    friend bool operator==(const AutoResolveSideStrength&, const AutoResolveSideStrength&) = default;
};
struct AutoResolveStrength {
    AutoResolveSides gathered;
    std::vector<AutoResolveCategory> categories;
    std::array<AutoResolveSideStrength, 2> sides;
    friend bool operator==(const AutoResolveStrength&, const AutoResolveStrength&) = default;
};
// WAR-27: winning gathered-side index for the initiate-time space shortcut.
[[nodiscard]] std::optional<std::size_t> auto_resolve_space_shortcut(const AutoResolveStrength& strength) noexcept;

// Every supplied FT-08 force must be gathered. WAR-13/14 do not establish that forces
// is mandatory/exhaustive: reverse membership is the unresolved conflict-to-roster adapter seam.
[[nodiscard]] core::Result<AutoResolveSides> gather_auto_resolve_sides(
    const ConflictRecord& conflict, const AutoResolveRoster& roster, const units::GalacticTypeTable& types);
// Category bit values come from the existing validated dynamic enum loader; no hardcoded
// category order. Immutable per-combatant work is partitioned; sums/ties commit in gather order.
[[nodiscard]] core::Result<AutoResolveStrength> auto_resolve_strength(
    const AutoResolveSides& gathered, const units::GalacticTypeTable& types,
    std::span<const AutoResolveCategory> contrast, std::span<const units::EnumValue> category_bits,
    const sim::PartitionExecutor& executor);

struct AutoResolveBattleRecord {
    sim::galactic::PlanetId planet{};
    std::vector<sim::EntityId> killed;
    friend bool operator==(const AutoResolveBattleRecord&, const AutoResolveBattleRecord&) = default;
};
struct AutoResolvePreparation {
    std::optional<ConflictRecord> conflict;
    std::optional<AutoResolveStrength> strength;
    std::array<AutoResolveBattleRecord, 8> records{}; // WAR-11's ring
    std::size_t record_index{7}; // next prepare advances to entry zero
    std::uint64_t start_frame{};
    sim::galactic::PlayerId winner{}, retreating_player{};
    bool fought{};
    friend bool operator==(const AutoResolvePreparation&, const AutoResolvePreparation&) = default;
};
[[nodiscard]] core::Result<AutoResolvePreparation> prepare_auto_resolve(
    const AutoResolvePreparation& previous, const ConflictRecord& conflict, std::uint64_t frame);
[[nodiscard]] core::Result<AutoResolvePreparation> initiate_auto_resolve(
    const AutoResolvePreparation& prepared, const AutoResolveRoster& roster,
    const units::GalacticTypeTable& types, std::span<const AutoResolveCategory> contrast,
    std::span<const units::EnumValue> category_bits, const sim::PartitionExecutor& executor);
[[nodiscard]] AutoResolvePreparation cleanup_auto_resolve(const AutoResolvePreparation& previous);

} // namespace eawr::campaign
