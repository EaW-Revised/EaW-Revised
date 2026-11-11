#pragma once

#include "eawr/sim/tactical/types.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

// The content half of fighter squadrons: what a MotionTable holds by value (docs/behaviour/space-fighters.md).
// The state and the flight rules are in eawr/sim/tactical/fighters.hpp.
namespace eawr::sim::tactical {

// Spin_Away_On_Death (#447, docs/behaviour/space-fighter-deaths.md SP-01): the chance that a
// killed craft spins away before it explodes, and for how long.
struct SpinAwayProfile {
    math::Fixed chance{}; // Spin_Away_On_Death_Chance, in [0, 1]
    math::Fixed time{};   // Spin_Away_On_Death_Time, seconds
    friend constexpr bool operator==(const SpinAwayProfile&, const SpinAwayProfile&) noexcept = default;
};

// One craft type's flight (FM-01): speeds per frame, turn, lift and roll in degrees per frame.
// Max speed, min speed, turn, lift and roll include Object_Max_Speed_Multiplier_Space; thrust
// (the speed change per frame) and the strafe distance do not.
struct CraftProfile {
    TypeId type_id{};
    math::Fixed max_speed{};
    math::Fixed min_speed{};
    math::Fixed rate_of_turn{};
    math::Fixed lift{};      // Max_Lift: pitch change per frame
    math::Fixed thrust{};    // Max_Thrust
    math::Fixed roll_rate{}; // Max_Rate_Of_Roll
    math::Fixed bank_angle{};
    math::Fixed strafe_distance{};
    math::Fixed layer_z{};   // Layer_Z_Adjust: the height a squadron settles at
    std::optional<SpinAwayProfile> spin_away; // a type with Spin_Away_On_Death (SP-01)
    // Out_Of_Combat_Defense_Adjustment: out of combat a hit does 1 - this times its damage (DG-26).
    math::Fixed out_of_combat_defense{};
    // #457 (FD-05, FD-06): Minimum_Follow_Distance and Targeting_Max_Attack_Distance, not scaled.
    math::Fixed follow_distance{};
    math::Fixed attack_distance{};
    friend constexpr bool operator==(const CraftProfile&, const CraftProfile&) noexcept = default;
};

// One squadron type (Squadron_Units, Squadron_Offsets): its craft in member order, each with its
// formation offset, and the ranges its formation diverts to attack within (FT-02).
struct SquadronProfile {
    TypeId type_id{};                    // also the type of its team container
    std::vector<TypeId> members;
    std::vector<math::Vec3> offsets;     // one per member; zero where the data has none
    math::Fixed guard_chase_range{};     // Guard_Chase_Range
    math::Fixed idle_chase_range{};      // Idle_Chase_Range
    math::Fixed attack_move_response_range{}; // Attack_Move_Response_Range (#452, FO-05)
    math::Fixed formation_tolerance{};   // Squadron_Formation_Error_Tolerance
    math::Fixed autonomous_move_extension_vs_attacker{}; // WMV-17: threat to team or escort
    math::Fixed formation_deceleration{}; // WMV-44: this member type, not its child craft
    math::Fixed formation_type_max_speed{}; // WMV-44: scaled own type maximum; zero bypasses the ratio
    std::uint32_t max_squad_size{10}; // FL-06: partition successful craft in input order
    friend bool operator==(const SquadronProfile&, const SquadronProfile&) = default;
};

// A reserve of this size never runs out (FL-02).
inline constexpr std::int32_t unlimited_reserve = -1;

struct SpawnEntryProfile {
    TypeId squadron{};
    std::int32_t starting{}; // Starting_Spawned_Units_Tech_0 count: the most alive at once
    std::int32_t reserve{};  // Reserve_Spawned_Units_Tech_0 count, or unlimited_reserve
    bool ship{};             // WSL-16: a type with no squadron members launches as one single ship (campaign stations)
    bool arrival{};          // WSL-16a: the ship's own Hyperspace tag: it arrives by hyperspace, not from the bay
    friend constexpr bool operator==(const SpawnEntryProfile&, const SpawnEntryProfile&) noexcept = default;
};

// A HARD_POINT_FIGHTER_BAY hardpoint (FL-05): its attachment bone in model space (scaled, with
// FoC's model turn) and the bone's X axis times Fighter_Bay_Flyout_Distance.
struct BayProfile {
    std::uint32_t hardpoint{}; // index in the spawner's HardPoints list
    math::Vec3 position{};
    math::Vec3 spawn_vector{};
    friend constexpr bool operator==(const BayProfile&, const BayProfile&) noexcept = default;
};

// A SPAWN_SQUADRON type (FL-01 to FL-09).
struct SpawnerProfile {
    TypeId type_id{};
    std::vector<SpawnEntryProfile> entries; // Starting_Spawned_Units_Tech_0 order
    std::uint32_t delay_frames{};           // trunc(Spawned_Squadron_Delay_Seconds x 30)
    std::vector<BayProfile> bays;           // HardPoints order
    bool mobile{};                          // it has a locomotor: its squadrons escort it (FL-07)
    bool starbase{};                        // WSL-26: DUMMY_STAR_BASE admits allied players' pending free forces
    friend bool operator==(const SpawnerProfile&, const SpawnerProfile&) = default;
};

// The #75 content of a MotionTable. Empty: nothing launches and craft never move, as before #75.
struct SquadronTable {
    std::vector<CraftProfile> craft;        // strictly increasing type_id
    std::vector<SquadronProfile> squadrons; // strictly increasing type_id
    std::vector<SpawnerProfile> spawners;   // strictly increasing type_id
    std::map<std::pair<TypeId, PlayerId>, SpawnerProfile> campaign_spawners; // WGT-58: content-bound owner-tech selections
    std::map<TypeId, std::array<SpawnerProfile, 7>> tech_spawners; // last entry is an invalid tech
    // FO-11 (#599): gameconstants.xml FormationMinimumSideError and FormationMaximumSideError, the
    // side error a squadron of a group move ignores and the one its lane steer divides by. A
    // maximum of zero (a table without the constants) turns the lane steer off.
    math::Fixed side_error_min{};
    math::Fixed side_error_max{};
    [[nodiscard]] const CraftProfile* find_craft(TypeId type_id) const noexcept;
    [[nodiscard]] const SquadronProfile* find_squadron(TypeId type_id) const noexcept;
    [[nodiscard]] const SpawnerProfile* find_spawner(TypeId type_id, PlayerId owner = 0) const noexcept;
    [[nodiscard]] const SpawnerProfile* find_spawner_at_tech(TypeId type_id, PlayerId owner, std::uint32_t tech) const noexcept;
    [[nodiscard]] bool empty() const noexcept { return craft.empty() && squadrons.empty() && spawners.empty() && campaign_spawners.empty() && tech_spawners.empty(); }
    friend bool operator==(const SquadronTable&, const SquadronTable&) = default;
};

} // namespace eawr::sim::tactical
